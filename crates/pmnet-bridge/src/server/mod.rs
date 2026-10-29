//! The WebSocket server (`pmnet-bridge serve`, feature `server`): loopback listeners, the accept loop,
//! configuration and shutdown.
//!
//! A thin transport over the [`bridge`](crate::bridge) core: the server owns one [`Bridge`]; every accepted
//! connection passes the `Origin` check ([`origin`]), opens a bridge session and runs a reader (WebSocket frames →
//! [`ClientSession`](crate::bridge::ClientSession)) and a writer task (the session's
//! [`OutgoingRx`](crate::bridge::OutgoingRx) → frames, plus pings).

pub mod origin;
mod ws;

pub use crate::bridge::BRIDGE_VERSION;

use crate::backend::NetBackend;
use crate::bridge::{Bridge, BridgeConfig};
use origin::OriginPolicy;
use std::io;
use std::net::{Ipv4Addr, Ipv6Addr, SocketAddr};
use std::sync::Arc;
use std::time::Duration;
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::watch;
use tokio::task::{JoinHandle, JoinSet};

/// Default TCP port (contract §1).
pub const DEFAULT_PORT: u16 = 7777;

// ── Configuration ─────────────────────────────────────────────────────────────────────────────────────────────────

/// Server settings: the WebSocket side plus the session settings of the bridge core
/// ([`bridge_config`](Self::bridge_config)). `Default` is the production configuration of the contract; tests
/// shorten the timers.
#[derive(Debug, Clone)]
pub struct ServerConfig {
    /// TCP port on 127.0.0.1 (and ::1); 0 = ephemeral (tests).
    pub port: u16,
    /// Also listen on `[::1]` (same port) when available.
    pub bind_ipv6: bool,
    /// `Origin` allow-list.
    pub origins: OriginPolicy,
    /// Close with 4002 when no client `hello` arrives within this time (contract: 10 s).
    pub hello_timeout: Duration,
    /// WebSocket ping interval (contract: 10 s).
    pub ping_interval: Duration,
    /// Re-send a route's last DMX after this idle time (contract: 1 s).
    pub dmx_refresh: Duration,
    /// Outbound messages queued per connection before it is closed as too slow (4003).
    pub outbound_capacity: usize,
    /// Network commands running concurrently per connection; further messages wait.
    pub max_inflight: usize,
    /// Largest accepted WebSocket message (Intel HEX images travel as text).
    pub max_message_size: usize,
    /// Time allowed for the HTTP upgrade.
    pub handshake_timeout: Duration,
}

impl ServerConfig {
    /// The session settings of the bridge core.
    pub fn bridge_config(&self) -> BridgeConfig {
        BridgeConfig {
            hello_timeout: self.hello_timeout,
            dmx_refresh: self.dmx_refresh,
            outbound_capacity: self.outbound_capacity,
            max_inflight: self.max_inflight,
        }
    }
}

impl Default for ServerConfig {
    fn default() -> Self {
        let bridge = BridgeConfig::default();
        Self {
            port: DEFAULT_PORT,
            bind_ipv6: true,
            origins: OriginPolicy::with_defaults::<&str>(&[])
                .unwrap_or_else(|_| OriginPolicy::only(Vec::new())),
            hello_timeout: bridge.hello_timeout,
            ping_interval: Duration::from_secs(10),
            dmx_refresh: bridge.dmx_refresh,
            outbound_capacity: bridge.outbound_capacity,
            max_inflight: bridge.max_inflight,
            max_message_size: 4 << 20,
            handshake_timeout: Duration::from_secs(5),
        }
    }
}

// ── Shared state ──────────────────────────────────────────────────────────────────────────────────────────────────

/// State shared by the accept loop and all connections.
struct Shared<B: NetBackend> {
    bridge: Bridge<B>,
    config: ServerConfig,
}

// ── Server ────────────────────────────────────────────────────────────────────────────────────────────────────────

/// A running server.
#[derive(Debug)]
pub struct ServerHandle {
    local_addrs: Vec<SocketAddr>,
    shutdown: watch::Sender<bool>,
    task: JoinHandle<()>,
}

impl ServerHandle {
    /// The IPv4 loopback address the server listens on.
    pub fn local_addr(&self) -> SocketAddr {
        self.local_addrs[0]
    }

    /// All listen addresses (IPv4, and IPv6 when bound).
    pub fn local_addrs(&self) -> &[SocketAddr] {
        &self.local_addrs
    }

    /// Closes every connection with 1001 and stops the server.
    pub async fn shutdown(self) {
        let _ = self.shutdown.send(true);
        let _ = self.task.await;
    }
}

/// Binds the loopback listeners and starts serving `backend`. Never binds a LAN address (contract §1).
pub async fn serve<B: NetBackend>(
    config: ServerConfig,
    backend: Arc<B>,
) -> io::Result<ServerHandle> {
    let v4 = TcpListener::bind((Ipv4Addr::LOCALHOST, config.port)).await?;
    let mut local_addrs = vec![v4.local_addr()?];
    let v6 = if config.bind_ipv6 {
        match TcpListener::bind((Ipv6Addr::LOCALHOST, local_addrs[0].port())).await {
            Ok(l) => {
                local_addrs.push(l.local_addr()?);
                Some(l)
            }
            Err(e) => {
                tracing::debug!("not listening on [::1]: {e}");
                None
            }
        }
    } else {
        None
    };
    let bridge = Bridge::new(backend, config.bridge_config());
    let (shutdown_tx, shutdown_rx) = watch::channel(false);
    let shared = Arc::new(Shared { bridge, config });
    let task = tokio::spawn(accept_loop(shared, v4, v6, shutdown_rx));
    Ok(ServerHandle {
        local_addrs,
        shutdown: shutdown_tx,
        task,
    })
}

/// Accepts connections until shutdown, then waits briefly for them to close.
async fn accept_loop<B: NetBackend>(
    shared: Arc<Shared<B>>,
    v4: TcpListener,
    v6: Option<TcpListener>,
    mut shutdown: watch::Receiver<bool>,
) {
    let mut conns = JoinSet::new();
    loop {
        let accepted = tokio::select! {
            _ = async { let _ = shutdown.wait_for(|down| *down).await; } => break,
            Some(_) = conns.join_next(), if !conns.is_empty() => continue,
            r = v4.accept() => r,
            r = accept_optional(v6.as_ref()) => r,
        };
        match accepted {
            Ok((stream, _peer)) => {
                let _ = stream.set_nodelay(true);
                conns.spawn(ws::run_connection(shared.clone(), stream));
            }
            Err(e) => {
                // E.g. out of file descriptors: back off instead of spinning.
                tracing::warn!("accept failed: {e}");
                tokio::time::sleep(Duration::from_millis(50)).await;
            }
        }
    }
    drop(v4);
    drop(v6);
    // Every session queues 1001 and ends; the connections deliver it and close.
    shared.bridge.shutdown();
    let drain = async { while conns.join_next().await.is_some() {} };
    if tokio::time::timeout(Duration::from_secs(2), drain)
        .await
        .is_err()
    {
        conns.abort_all();
    }
}

/// Accepts on an optional listener (pending forever when absent).
async fn accept_optional(listener: Option<&TcpListener>) -> io::Result<(TcpStream, SocketAddr)> {
    match listener {
        Some(l) => l.accept().await,
        None => std::future::pending().await,
    }
}
