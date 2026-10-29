//! `pmnet-bridge` — the bridge between the Pixelmapper app and Art-Net 4 / RDM: a WebSocket server for the web
//! app (`pmnet-bridge serve`) and an embeddable, transport-independent core for the desktop app.
//!
//! The contract is `protocol/bridge-ws.md`. Layers:
//! - [`protocol`]: the contract as Rust types (state, events, commands, errors, binary frame sets). No I/O.
//! - [`backend`]: the [`NetBackend`](backend::NetBackend) trait — the only way the bridge reaches the network —
//!   with the real backend on `pixelmapper-net`
//!   ([`PixelmapperNetBackend`](backend::pixelmapper_net::PixelmapperNetBackend)), a scriptable mock and an
//!   offline backend.
//! - [`bridge`]: the transport-independent core — [`Bridge`](bridge::Bridge) owns the backend and the event hub;
//!   every client is a [`ClientSession`](bridge::ClientSession) (handshake, commands, events, DMX routing) that
//!   any transport can drive. The desktop app embeds this layer and forwards messages over its own IPC.
//! - [`server`] (feature `server`, default): loopback WebSocket server with `Origin` check and pings, a thin
//!   transport over [`bridge`].
//! - [`cli`] (feature `server`): the `serve` command for `pmnet-bridge` (and later `pmnet serve`).
//!
//! Embedding without the WebSocket server: `default-features = false`.

#![warn(missing_docs)]

pub mod backend;
pub mod bridge;
#[cfg(feature = "server")]
pub mod cli;
pub mod protocol;
#[cfg(feature = "server")]
pub mod server;
