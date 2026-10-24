//! The sending side of one WebSocket connection: a bounded, ordered outbound queue drained by a writer task, and
//! the close signal shared by the reader, the writer, the hub and command tasks.
//!
//! Slow-client policy: everything for a connection goes through its bounded queue with `try_send`. When the
//! queue is full the connection is closed with code 4003 instead of blocking the hub or dropping single
//! messages (a dropped `node`/`nodeGone` would silently corrupt the app's state; after a reconnect the app
//! re-reads the snapshot). Other connections are never affected.

use futures_util::stream::SplitSink;
use futures_util::SinkExt;
use std::sync::Arc;
use std::time::Duration;
use tokio::net::TcpStream;
use tokio::sync::{mpsc, watch};
use tokio_tungstenite::tungstenite::protocol::frame::coding::CloseCode;
use tokio_tungstenite::tungstenite::protocol::CloseFrame;
use tokio_tungstenite::tungstenite::{Bytes, Message};
use tokio_tungstenite::WebSocketStream;

// ── Close codes ───────────────────────────────────────────────────────────────────────────────────────────────────

/// Client protocol major differs (contract §1).
pub const CLOSE_INCOMPATIBLE: u16 = 4001;
/// No client `hello` within the deadline (contract §1).
pub const CLOSE_NO_HELLO: u16 = 4002;
/// The client did not read its messages fast enough (outbound queue full).
pub const CLOSE_TOO_SLOW: u16 = 4003;
/// Bridge shutting down.
pub const CLOSE_GOING_AWAY: u16 = 1001;

/// How long the writer tries to deliver a close frame before dropping the socket.
const CLOSE_SEND_TIMEOUT: Duration = Duration::from_secs(1);

/// Why a connection ends.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CloseReason {
    /// Send a close frame with this code and reason, then drop the socket.
    Code(u16, String),
    /// The peer is gone or the socket failed: nothing more to send.
    Gone,
}

/// A close frame message.
pub fn close_message(code: u16, reason: &str) -> Message {
    Message::Close(Some(CloseFrame {
        code: CloseCode::from(code),
        reason: reason.into(),
    }))
}

// ── ConnTx ────────────────────────────────────────────────────────────────────────────────────────────────────────

/// Cloneable handle to send to one connection.
#[derive(Debug, Clone)]
pub struct ConnTx {
    id: u64,
    tx: mpsc::Sender<Message>,
    close: Arc<watch::Sender<Option<CloseReason>>>,
}

impl ConnTx {
    /// Connection number (also the source of its ArtDmx `Physical`).
    pub fn id(&self) -> u64 {
        self.id
    }

    /// Queues a message in order. Returns false when the connection is closed or closing; a full queue closes
    /// the connection with 4003 (see module docs).
    pub fn send(&self, msg: Message) -> bool {
        if self.close.borrow().is_some() {
            return false;
        }
        match self.tx.try_send(msg) {
            Ok(()) => true,
            Err(mpsc::error::TrySendError::Full(_)) => {
                tracing::warn!(conn = self.id, "outbound queue full, closing slow client");
                self.close_now(CloseReason::Code(CLOSE_TOO_SLOW, "client too slow".into()));
                false
            }
            Err(mpsc::error::TrySendError::Closed(_)) => false,
        }
    }

    /// Queues a JSON text message.
    pub fn send_text(&self, text: String) -> bool {
        self.send(Message::text(text))
    }

    /// Queues a close frame behind everything already queued (orderly close: 4001, 4002, 1001).
    pub fn close_after_queue(&self, code: u16, reason: &str) {
        self.send(close_message(code, reason));
    }

    /// Ends the connection now, skipping the queue. Only the first reason counts.
    pub fn close_now(&self, reason: CloseReason) {
        self.close.send_if_modified(|current| {
            if current.is_none() {
                *current = Some(reason);
                true
            } else {
                false
            }
        });
    }

    /// A receiver that changes when the connection is closed.
    pub fn closed(&self) -> watch::Receiver<Option<CloseReason>> {
        self.close.subscribe()
    }
}

// ── Writer ────────────────────────────────────────────────────────────────────────────────────────────────────────

/// Type of the WebSocket sink.
pub type WsSink = SplitSink<WebSocketStream<TcpStream>, Message>;

/// Creates the handle and spawns the writer task for `sink`. The writer drains the queue in order, pings every
/// `ping_every`, ends on a queued close frame, on `close_now`, or when the socket fails — and then marks the
/// connection closed so the reader stops too.
pub fn spawn_writer(
    id: u64,
    sink: WsSink,
    capacity: usize,
    ping_every: Duration,
) -> (ConnTx, tokio::task::JoinHandle<()>) {
    let (tx, rx) = mpsc::channel(capacity.max(1));
    let (close, close_rx) = watch::channel(None);
    let conn = ConnTx {
        id,
        tx,
        close: Arc::new(close),
    };
    let handle = tokio::spawn(writer_loop(conn.clone(), sink, rx, close_rx, ping_every));
    (conn, handle)
}

/// Body of the writer task.
async fn writer_loop(
    conn: ConnTx,
    mut sink: WsSink,
    mut rx: mpsc::Receiver<Message>,
    mut close: watch::Receiver<Option<CloseReason>>,
    ping_every: Duration,
) {
    let mut ping = tokio::time::interval_at(tokio::time::Instant::now() + ping_every, ping_every);
    loop {
        tokio::select! {
            biased;
            _ = close.changed() => {
                let reason = close.borrow_and_update().clone();
                if let Some(CloseReason::Code(code, text)) = reason {
                    let _ = tokio::time::timeout(CLOSE_SEND_TIMEOUT, sink.send(close_message(code, &text))).await;
                }
                break;
            }
            msg = rx.recv() => {
                let Some(msg) = msg else { break };
                let is_close = matches!(msg, Message::Close(_));
                // A stuck socket must not keep the writer alive after `close_now`.
                let sent = tokio::select! {
                    r = sink.send(msg) => r.is_ok(),
                    _ = close.changed() => false,
                };
                if !sent || is_close {
                    break;
                }
            }
            _ = ping.tick() => {
                if sink.send(Message::Ping(Bytes::new())).await.is_err() {
                    break;
                }
            }
        }
    }
    conn.close_now(CloseReason::Gone);
    let _ = tokio::time::timeout(CLOSE_SEND_TIMEOUT, sink.close()).await;
}
