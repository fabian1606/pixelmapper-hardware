//! `pmnet-bridge` — the WebSocket bridge (`pmnet serve`) between the Pixelmapper web app and Art-Net 4 / RDM.
//!
//! The contract is `protocol/bridge-ws.md`. Layers:
//! - [`protocol`]: the contract as Rust types (state, events, commands, errors, binary frame sets). No I/O.
//! - [`backend`]: the [`NetBackend`](backend::NetBackend) trait — the only way the server reaches the network —
//!   plus a scriptable mock and an offline backend. The `pixelmapper-net` adapter is pending (see
//!   [`backend::pixelmapper_net`]).
//! - [`server`]: loopback WebSocket server with `Origin` check, handshake, per-connection DMX routing,
//!   event fan-out and slow-client handling.
//! - [`cli`]: the `serve` command for `pmnet-bridge` (and later `pmnet serve`).

#![warn(missing_docs)]

pub mod backend;
pub mod cli;
pub mod protocol;
pub mod server;
