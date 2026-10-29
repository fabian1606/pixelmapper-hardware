//! One WebSocket connection as a bridge transport: upgrade with `Origin` check, then a reader loop that hands
//! frames to the [`ClientSession`] and a writer task that sends what the session's [`OutgoingRx`] yields, plus
//! pings. All protocol behaviour lives in [`crate::bridge`].

use super::Shared;
use crate::backend::NetBackend;
use crate::bridge::{ClientInfo, ClientSession, Outgoing, OutgoingRx};
use futures_util::stream::{SplitSink, SplitStream};
use futures_util::{SinkExt, StreamExt};
use std::sync::Arc;
use std::time::Duration;
use tokio::net::TcpStream;
use tokio_tungstenite::tungstenite::handshake::server::{ErrorResponse, Request, Response};
use tokio_tungstenite::tungstenite::http::{header, StatusCode};
use tokio_tungstenite::tungstenite::protocol::frame::coding::CloseCode;
use tokio_tungstenite::tungstenite::protocol::{CloseFrame, WebSocketConfig};
use tokio_tungstenite::tungstenite::{Bytes, Message};
use tokio_tungstenite::WebSocketStream;

/// How long the writer tries to deliver a close frame before dropping the socket.
const CLOSE_SEND_TIMEOUT: Duration = Duration::from_secs(1);

/// Type of the WebSocket sink.
type WsSink = SplitSink<WebSocketStream<TcpStream>, Message>;

// ── Upgrade ───────────────────────────────────────────────────────────────────────────────────────────────────────

/// Performs the WebSocket upgrade; answers 403 when the `Origin` is not allowed (contract §1). Returns the stream
/// and the client's `Origin`.
// The callback's large `Err` type is dictated by tungstenite's handshake API.
#[allow(clippy::result_large_err)]
async fn accept<B: NetBackend>(
    shared: &Shared<B>,
    stream: TcpStream,
) -> Option<(WebSocketStream<TcpStream>, Option<String>)> {
    let policy = shared.config.origins.clone();
    let seen_origin = Arc::new(std::sync::Mutex::new(None::<String>));
    let seen = seen_origin.clone();
    let callback = move |req: &Request, resp: Response| -> Result<Response, ErrorResponse> {
        // A non-UTF-8 Origin never matches.
        let origin = req
            .headers()
            .get(header::ORIGIN)
            .map(|v| v.to_str().unwrap_or(""));
        if policy.allows(origin) {
            if let Ok(mut s) = seen.lock() {
                *s = origin.map(str::to_owned);
            }
            Ok(resp)
        } else {
            tracing::info!(origin = ?origin, "rejected WebSocket upgrade from a foreign origin");
            let mut reject = ErrorResponse::new(Some("origin not allowed".into()));
            *reject.status_mut() = StatusCode::FORBIDDEN;
            Err(reject)
        }
    };
    let ws_config = WebSocketConfig::default()
        .max_message_size(Some(shared.config.max_message_size))
        .max_frame_size(Some(shared.config.max_message_size));
    let upgrade =
        tokio_tungstenite::accept_hdr_async_with_config(stream, callback, Some(ws_config));
    match tokio::time::timeout(shared.config.handshake_timeout, upgrade).await {
        Ok(Ok(ws)) => {
            let origin = seen_origin.lock().ok().and_then(|mut s| s.take());
            Some((ws, origin))
        }
        Ok(Err(e)) => {
            tracing::debug!("WebSocket upgrade failed: {e}");
            None
        }
        Err(_) => {
            tracing::debug!("WebSocket upgrade timed out");
            None
        }
    }
}

// ── Connection ────────────────────────────────────────────────────────────────────────────────────────────────────

/// Serves one TCP connection until it closes.
pub(super) async fn run_connection<B: NetBackend>(shared: Arc<Shared<B>>, stream: TcpStream) {
    let Some((ws, origin)) = accept(&shared, stream).await else {
        return;
    };
    let (sink, mut source) = ws.split();
    let (session, outgoing) = shared
        .bridge
        .connect(ClientInfo::new("websocket").with_origin(origin));
    let id = session.id();
    let writer = tokio::spawn(writer_loop(sink, outgoing, shared.config.ping_interval));
    read_loop(&session, &mut source).await;
    // Dropping the session ends it unless it already ended on its own (then the writer still delivers its close).
    drop(session);
    let _ = writer.await;
    tracing::debug!(conn = id, "connection closed");
}

/// Hands every text and binary frame to the session in order, until either side closes.
async fn read_loop(session: &ClientSession, source: &mut SplitStream<WebSocketStream<TcpStream>>) {
    loop {
        tokio::select! {
            biased;
            _ = session.closed() => break,
            msg = source.next() => match msg {
                Some(Ok(Message::Text(text))) => {
                    if session.handle_text(text.as_str()).await.is_err() {
                        break;
                    }
                }
                Some(Ok(Message::Binary(data))) => {
                    if session.handle_binary(data).await.is_err() {
                        break;
                    }
                }
                Some(Ok(Message::Close(_))) | Some(Err(_)) | None => {
                    session.close();
                    break;
                }
                // Ping/pong are answered by tungstenite.
                Some(Ok(_)) => {}
            },
        }
    }
}

/// Sends the session's messages in order and pings every `ping_every`; ends after the session's close (sent as a
/// close frame) or when the socket fails. Dropping `outgoing` at the end marks the session closed.
async fn writer_loop(mut sink: WsSink, mut outgoing: OutgoingRx, ping_every: Duration) {
    let mut ping = tokio::time::interval_at(tokio::time::Instant::now() + ping_every, ping_every);
    ping.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
    loop {
        let msg = tokio::select! {
            biased;
            msg = outgoing.recv() => match msg {
                Some(Outgoing::Text(text)) => Message::text(text),
                Some(Outgoing::Close { code, reason }) => {
                    let _ = tokio::time::timeout(
                        CLOSE_SEND_TIMEOUT,
                        sink.send(close_message(code, &reason)),
                    )
                    .await;
                    break;
                }
                None => break,
            },
            _ = ping.tick() => Message::Ping(Bytes::new()),
        };
        // A stuck socket must not keep the writer alive after the session closed: the next `recv` then yields the
        // close (or `None`).
        tokio::select! {
            r = sink.send(msg) => if r.is_err() { break },
            _ = outgoing.closed() => {}
        }
    }
}

/// A close frame message.
fn close_message(code: u16, reason: &str) -> Message {
    Message::Close(Some(CloseFrame {
        code: CloseCode::from(code),
        reason: reason.into(),
    }))
}
