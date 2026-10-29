//! The transport-independent bridge core: everything `protocol/bridge-ws.md` asks of the bridge except the
//! WebSocket itself.
//!
//! A [`Bridge`] owns the backend, the event [`hub`] (backend mirror and fan-out) and the shared poll targets.
//! [`Bridge::connect`] opens one client session and returns its two ends: a [`ClientSession`] that takes the
//! client's messages (JSON commands, binary DMX frame sets) and an [`OutgoingRx`] that yields what the bridge sends
//! back ([`Outgoing::Text`] and a final [`Outgoing::Close`]). The session does the handshake, command parsing,
//! validation and dispatch, `id` correlation, the ordered outbound queue with the slow-client policy, event
//! fan-out and per-session DMX routing ([`dmx`]) with its `Physical`.
//!
//! Transports are thin adapters: the WebSocket server ([`crate::server`], feature `server`) maps frames to and
//! from these calls and adds the `Origin` check and pings; an embedding app (e.g. the Pixelmapper desktop app
//! over Tauri IPC) forwards the same messages in-process. The messages are identical on every transport.
//!
//! ```no_run
//! # async fn demo() {
//! use pmnet_bridge::backend::mock::MockBackend;
//! use pmnet_bridge::bridge::{Bridge, BridgeConfig, ClientInfo, Outgoing};
//! use std::sync::Arc;
//!
//! let bridge = Bridge::new(Arc::new(MockBackend::demo()), BridgeConfig::default());
//! let (session, mut outgoing) = bridge.connect(ClientInfo::new("example"));
//! tokio::spawn(async move {
//!     while let Some(msg) = outgoing.recv().await {
//!         match msg {
//!             Outgoing::Text(json) => println!("{json}"),
//!             Outgoing::Close { code, reason } => println!("closed {code}: {reason}"),
//!         }
//!     }
//! });
//! session
//!     .handle_text(r#"{"type":"hello","id":1,"client":"x","clientVersion":"0","protocol":"1.0.0"}"#)
//!     .await
//!     .ok();
//! # }
//! ```

pub mod dmx;
pub mod hub;
pub mod outbound;
mod session;
pub mod validate;

pub use outbound::{
    Outgoing, OutgoingRx, CLOSE_GOING_AWAY, CLOSE_INCOMPATIBLE, CLOSE_NO_HELLO, CLOSE_TOO_SLOW,
};
pub use session::{ClientSession, SessionClosed};

use crate::backend::NetBackend;
use crate::protocol::BridgeStatus;
use hub::{HubTx, SharedMirror};
use std::collections::BTreeMap;
use std::net::Ipv4Addr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio::sync::watch;

/// Version reported as `hello.bridge`.
pub const BRIDGE_VERSION: &str = env!("CARGO_PKG_VERSION");

// ── Configuration ─────────────────────────────────────────────────────────────────────────────────────────────────

/// Session settings. `Default` is the production configuration of the contract; tests shorten the timers.
#[derive(Debug, Clone)]
pub struct BridgeConfig {
    /// Close with 4002 when no client `hello` arrives within this time (contract: 10 s).
    pub hello_timeout: Duration,
    /// Re-send a route's last DMX after this idle time (contract: 1 s).
    pub dmx_refresh: Duration,
    /// Outbound messages queued per session before it is closed as too slow (4003).
    pub outbound_capacity: usize,
    /// Network commands running concurrently per session; further messages wait.
    pub max_inflight: usize,
}

impl Default for BridgeConfig {
    fn default() -> Self {
        Self {
            hello_timeout: Duration::from_secs(10),
            dmx_refresh: Duration::from_secs(1),
            outbound_capacity: 1024,
            max_inflight: 256,
        }
    }
}

/// Who connects (for logs only; it does not change behaviour).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ClientInfo {
    /// Transport name, e.g. `websocket` or `tauri`.
    pub transport: String,
    /// WebSocket `Origin` of the client, when the transport has one.
    pub origin: Option<String>,
}

impl ClientInfo {
    /// A client on the named transport.
    pub fn new(transport: impl Into<String>) -> Self {
        Self {
            transport: transport.into(),
            origin: None,
        }
    }

    /// Sets the client's `Origin`.
    pub fn with_origin(mut self, origin: Option<String>) -> Self {
        self.origin = origin;
        self
    }
}

// ── Shared state ──────────────────────────────────────────────────────────────────────────────────────────────────

/// State shared by the bridge handle and all sessions.
struct Shared<B> {
    backend: Arc<B>,
    config: BridgeConfig,
    hub: HubTx,
    mirror: SharedMirror,
    /// Poll targets with the number of sessions that added them.
    poll_targets: Mutex<BTreeMap<Ipv4Addr, usize>>,
    /// Becomes true on shutdown; sessions close with 1001.
    shutdown: watch::Sender<bool>,
    /// Number of the next session.
    next_id: AtomicU64,
}

// ── Bridge ────────────────────────────────────────────────────────────────────────────────────────────────────────

/// The bridge core: one backend, any number of client sessions. Cheap to clone (all clones are the same bridge).
pub struct Bridge<B: NetBackend> {
    shared: Arc<Shared<B>>,
}

impl<B: NetBackend> Clone for Bridge<B> {
    fn clone(&self) -> Self {
        Self {
            shared: self.shared.clone(),
        }
    }
}

impl<B: NetBackend> std::fmt::Debug for Bridge<B> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Bridge")
            .field("config", &self.shared.config)
            .finish_non_exhaustive()
    }
}

impl<B: NetBackend> Bridge<B> {
    /// Starts the event hub for `backend`. Must be called inside a Tokio runtime; the hub and every session run
    /// as tasks on it.
    pub fn new(backend: Arc<B>, config: BridgeConfig) -> Self {
        let (hub, mirror) = hub::start(backend.clone());
        let (shutdown, _) = watch::channel(false);
        Self {
            shared: Arc::new(Shared {
                backend,
                config,
                hub,
                mirror,
                poll_targets: Mutex::new(BTreeMap::new()),
                shutdown,
                next_id: AtomicU64::new(0),
            }),
        }
    }

    /// Opens a client session: the bridge `hello` event is already queued on the returned [`OutgoingRx`].
    /// The client then has `hello_timeout` to send its `hello` command. Must be called inside a Tokio runtime.
    ///
    /// The transport must drain the `OutgoingRx` promptly (a full queue closes the session with 4003) and forward
    /// client messages to the `ClientSession` in order. Origin checks are the transport's business.
    pub fn connect(&self, info: ClientInfo) -> (ClientSession, OutgoingRx) {
        let id = self.shared.next_id.fetch_add(1, Ordering::Relaxed);
        session::open(self.shared.clone(), id, info)
    }

    /// The backend.
    pub fn backend(&self) -> &Arc<B> {
        &self.shared.backend
    }

    /// The bridge state as the sessions see it (interfaces and Art-Net socket).
    pub fn status(&self) -> BridgeStatus {
        self.shared.mirror.read().status.clone()
    }

    /// Closes every open session with 1001 ("bridge shutting down") after its queued messages. Sessions opened
    /// afterwards are closed the same way at once. Does not stop the backend.
    pub fn shutdown(&self) {
        self.shared.shutdown.send_replace(true);
    }
}
