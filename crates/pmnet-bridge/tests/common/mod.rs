//! Shared helpers of the integration tests: a server on an ephemeral port backed by a `MockBackend`, and a
//! small WebSocket test client that correlates command ids and buffers events.

#![allow(dead_code)]

use futures_util::{SinkExt, StreamExt};
use pmnet_bridge::backend::mock::{sample_node, MockBackend};
use pmnet_bridge::protocol::{NodeState, PortRef, RdmUid, Tod};
use pmnet_bridge::server::{serve, ServerConfig, ServerHandle};
use serde_json::{json, Value};
use std::collections::VecDeque;
use std::net::{Ipv4Addr, SocketAddr};
use std::sync::Arc;
use std::time::Duration;
use tokio::net::TcpStream;
use tokio_tungstenite::tungstenite::client::IntoClientRequest;
use tokio_tungstenite::tungstenite::http::HeaderValue;
use tokio_tungstenite::tungstenite::{self, Message};
use tokio_tungstenite::{MaybeTlsStream, WebSocketStream};

/// How long a test waits for any expected message.
pub const WAIT: Duration = Duration::from_secs(3);

/// Node id of the sample node.
pub const NODE: &str = "7FF0:8A1B2C3D";
/// A tube on port 1.
pub const TUBE: RdmUid = RdmUid::new(0x7FF0, 0x74B6_510C);

/// Test configuration: ephemeral port, IPv4 only, short DMX refresh.
pub fn config() -> ServerConfig {
    ServerConfig {
        port: 0,
        bind_ipv6: false,
        dmx_refresh: Duration::from_millis(300),
        ..ServerConfig::default()
    }
}

/// The sample Pixelmapper Node (4 ports, `{bind 1…4, index 0}`).
pub fn node() -> NodeState {
    sample_node(NODE, Ipv4Addr::new(2, 27, 44, 61), 4)
}

/// Mock with the sample node and a TOD on port 1.
pub fn mock_with_node() -> MockBackend {
    let mock = MockBackend::new();
    mock.upsert_node(node());
    mock.set_tod(Tod {
        node_id: NODE.into(),
        port: PortRef::new(1, 0),
        port_address: 1,
        available: true,
        uids: vec![TUBE],
    });
    mock
}

/// Starts a server for `mock`.
pub async fn start(mock: &MockBackend, config: ServerConfig) -> ServerHandle {
    serve(config, Arc::new(mock.clone()))
        .await
        .expect("server starts")
}

/// Polls `check` until it returns `Some` or `WAIT` elapses.
pub async fn eventually<T>(mut check: impl FnMut() -> Option<T>) -> T {
    let deadline = tokio::time::Instant::now() + WAIT;
    loop {
        if let Some(v) = check() {
            return v;
        }
        assert!(
            tokio::time::Instant::now() < deadline,
            "condition not reached in time"
        );
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}

/// What the client read from the socket.
#[derive(Debug)]
pub enum Incoming {
    /// A JSON text message.
    Json(Value),
    /// A close frame with its code.
    Close(Option<u16>),
    /// The stream ended without a close frame (or failed).
    Ended,
}

/// A WebSocket test client.
pub struct Client {
    ws: WebSocketStream<MaybeTlsStream<TcpStream>>,
    next_id: u32,
    backlog: VecDeque<Value>,
    /// The bridge's `hello` event.
    pub hello: Value,
}

impl Client {
    /// Connects (optionally with an `Origin`) and reads the `hello` event.
    pub async fn try_connect(
        addr: SocketAddr,
        origin: Option<&str>,
    ) -> Result<Client, tungstenite::Error> {
        let mut req = format!("ws://{addr}/").into_client_request()?;
        if let Some(o) = origin {
            req.headers_mut()
                .insert("Origin", HeaderValue::from_str(o).expect("header value"));
        }
        let (ws, _) = tokio_tungstenite::connect_async(req).await?;
        let mut client = Client {
            ws,
            next_id: 1,
            backlog: VecDeque::new(),
            hello: Value::Null,
        };
        match client.read().await {
            Incoming::Json(v) => client.hello = v,
            other => panic!("expected hello, got {other:?}"),
        }
        Ok(client)
    }

    /// Connects without `Origin` and reads `hello`.
    pub async fn connect(addr: SocketAddr) -> Client {
        Self::try_connect(addr, None).await.expect("connect")
    }

    /// Connects and completes the handshake (`hello` command).
    pub async fn handshaken(addr: SocketAddr) -> Client {
        let mut c = Self::connect(addr).await;
        let r = c
            .request(
                "hello",
                json!({"client": "test", "clientVersion": "0", "protocol": "1.0.0"}),
            )
            .await;
        assert_eq!(r["type"], "result", "hello failed: {r}");
        c
    }

    /// Reads the next non-ping message.
    pub async fn read(&mut self) -> Incoming {
        loop {
            let msg = match tokio::time::timeout(WAIT, self.ws.next()).await {
                Ok(m) => m,
                Err(_) => panic!("no message within {WAIT:?}"),
            };
            return match msg {
                Some(Ok(Message::Text(t))) => {
                    Incoming::Json(serde_json::from_str(t.as_str()).expect("JSON"))
                }
                Some(Ok(Message::Close(frame))) => {
                    Incoming::Close(frame.map(|f| u16::from(f.code)))
                }
                Some(Ok(_)) => continue,
                Some(Err(_)) | None => Incoming::Ended,
            };
        }
    }

    /// Reads the next JSON message (backlog first).
    pub async fn next_json(&mut self) -> Value {
        if let Some(v) = self.backlog.pop_front() {
            return v;
        }
        match self.read().await {
            Incoming::Json(v) => v,
            other => panic!("expected JSON, got {other:?}"),
        }
    }

    /// True when nothing arrives within `wait`.
    pub async fn silent_for(&mut self, wait: Duration) -> bool {
        if !self.backlog.is_empty() {
            return false;
        }
        tokio::time::timeout(wait, self.ws.next()).await.is_err()
    }

    /// Sends raw text.
    pub async fn send_text(&mut self, text: &str) {
        self.ws.send(Message::text(text)).await.expect("send");
    }

    /// Sends a binary message.
    pub async fn send_binary(&mut self, data: Vec<u8>) {
        self.ws.send(Message::binary(data)).await.expect("send");
    }

    /// Sends a command with the next id; returns the id.
    pub async fn send_command(&mut self, kind: &str, args: Value) -> u32 {
        let id = self.next_id;
        self.next_id += 1;
        let mut msg = args;
        msg["type"] = json!(kind);
        msg["id"] = json!(id);
        self.send_text(&msg.to_string()).await;
        id
    }

    /// Waits for the `result`/`error` with `id`; other messages go to the backlog.
    pub async fn reply(&mut self, id: u32) -> Value {
        if let Some(pos) = self.backlog.iter().position(|v| v["id"] == json!(id)) {
            return self.backlog.remove(pos).expect("present");
        }
        loop {
            match self.read().await {
                Incoming::Json(v)
                    if v["id"] == json!(id) && (v["type"] == "result" || v["type"] == "error") =>
                {
                    return v
                }
                Incoming::Json(v) => self.backlog.push_back(v),
                other => panic!("connection ended while waiting for reply {id}: {other:?}"),
            }
        }
    }

    /// Sends a command and waits for its reply.
    pub async fn request(&mut self, kind: &str, args: Value) -> Value {
        let id = self.send_command(kind, args).await;
        self.reply(id).await
    }

    /// Sends a command, expects a `result` and returns its payload.
    pub async fn ok(&mut self, kind: &str, args: Value) -> Value {
        let r = self.request(kind, args).await;
        assert_eq!(r["type"], "result", "{kind} failed: {r}");
        r["result"].clone()
    }

    /// Sends a command, expects an `error` and returns it.
    pub async fn err(&mut self, kind: &str, args: Value) -> Value {
        let r = self.request(kind, args).await;
        assert_eq!(r["type"], "error", "{kind} unexpectedly succeeded: {r}");
        r
    }

    /// Waits for the next event of type `kind` (backlog first; other events are kept).
    pub async fn event(&mut self, kind: &str) -> Value {
        if let Some(pos) = self.backlog.iter().position(|v| v["type"] == kind) {
            return self.backlog.remove(pos).expect("present");
        }
        loop {
            match self.read().await {
                Incoming::Json(v) if v["type"] == kind => return v,
                Incoming::Json(v) => self.backlog.push_back(v),
                other => panic!("connection ended while waiting for {kind}: {other:?}"),
            }
        }
    }

    /// Reads until the connection ends; returns the close code (None = no close frame).
    pub async fn closed(&mut self) -> Option<u16> {
        loop {
            match self.read().await {
                Incoming::Json(v) => self.backlog.push_back(v),
                Incoming::Close(code) => return code,
                Incoming::Ended => return None,
            }
        }
    }
}
