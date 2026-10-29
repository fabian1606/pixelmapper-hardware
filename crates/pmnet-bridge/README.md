# pmnet-bridge

Bridge between the Pixelmapper app and Art-Net 4 / RDM. The web app connects to `ws://127.0.0.1:7777/`,
sends intents (RDM GET/SET, node configuration, firmware updates, DMX frame sets) and receives state (nodes, TODs,
progress). The contract is [`protocol/bridge-ws.md`](../../protocol/bridge-ws.md); everything on the wire is done
by [`pixelmapper-net`](../pixelmapper-net). The desktop app embeds the same bridge core in-process (see
[Embedding](#embedding-without-websocket)), so the bridge logic exists once for both.

## Running

```sh
cargo run -p pmnet-bridge -- serve                 # Art-Net on UDP 6454, all interfaces
cargo run -p pmnet-bridge -- serve --bind 2.0.0.1  # one interface only (its broadcast)
cargo run -p pmnet-bridge -- serve --mock          # simulated installation, no network (app development)
cargo run -p pmnet-bridge -- serve --offline       # no network; the app sees udp.bound = false
```

| Option | Meaning |
|---|---|
| `--port` | WebSocket port on 127.0.0.1 / ::1 (default 7777) |
| `--allow-origin` | extra allowed `Origin` (repeatable) |
| `--bind`, `--udp-port` | local address of the Art-Net socket (default 0.0.0.0:6454; `--udp-port 0` = any free port) |
| `--node-port` | UDP port of the nodes (ArtPoll broadcasts, poll targets, manual DMX targets; default 6454) |
| `--broadcast ADDR` | ArtPoll/ArtSync destination instead of the interface broadcasts (repeatable) |
| `-v`, `-vv` | debug / trace logging |

If the Art-Net socket cannot be bound (e.g. a specific `--bind` address that is not up), the bridge still serves:
`hello.udp` reports `bound: false` with the reason, network commands answer `udpUnavailable`, and binding is
retried every 5 s (`bridgeStatus` when it works).

### Against the simulator

```sh
cargo run -p pixelmapper-net --bin pmnet -- sim --listen 127.0.0.1:6455 --tubes 2 --fixtures 1
cargo run -p pmnet-bridge -- serve --port 7799 --bind 127.0.0.1 --udp-port 0 --broadcast 127.0.0.1:6455
```

## Embedding without WebSocket

`pmnet_bridge::bridge` is the transport-independent core. A `Bridge` owns the backend and the event hub; every
client is one session with two ends:

```rust
use pmnet_bridge::backend::pixelmapper_net::{BackendConfig, PixelmapperNetBackend};
use pmnet_bridge::bridge::{Bridge, BridgeConfig, ClientInfo, Outgoing};
use std::sync::Arc;

// Inside a Tokio runtime. Same defaults as `serve`: UDP 6454 on all interfaces; an unbindable socket is not
// fatal (the client sees `udp.bound = false`, binding is retried).
let backend = Arc::new(PixelmapperNetBackend::start(BackendConfig::default()).await);
let bridge = Bridge::new(backend, BridgeConfig::default());

let (session, mut outgoing) = bridge.connect(ClientInfo::new("tauri"));
// Bridge → client: the `hello` event first, then results/events; at most one final `Close`.
tokio::spawn(async move {
    while let Some(msg) = outgoing.recv().await {
        match msg {
            Outgoing::Text(json) => { /* deliver one JSON message */ }
            Outgoing::Close { code, reason } => { /* 4001, 4002, 4003 or 1001 */ }
        }
    }
});
// Client → bridge, in order:
session.handle_text(r#"{"type":"hello","id":1,"protocol":"1.0.0"}"#).await?;
session.handle_binary(frame_set_bytes).await?;     // DMX frame set (contract §7)
session.close();                                   // client went away
```

The messages are exactly those of `protocol/bridge-ws.md` (§1 “Other transports”); the transport only moves
them. Origin checks and pings are WebSocket concerns and stay in `server`. Depend on the crate with
`default-features = false` to leave out the WebSocket server, the CLI and their dependencies (feature `server`,
on by default; the `pmnet-bridge` binary requires it):

```toml
pmnet-bridge = { git = "https://github.com/fabian1606/pixelmapper-hardware", rev = "…", default-features = false }
```

The Pixelmapper desktop app (`src-tauri/src/artnet.rs` in the Pixelmapper repo) does exactly this and forwards the
messages over Tauri IPC.

## Structure

- `src/protocol/` – the contract as Rust types (no I/O).
- `src/bridge/` – the transport-independent core: `Bridge`, `ClientSession`/`OutgoingRx`, handshake, command
  dispatch, argument validation, event hub, per-session DMX routing and refresh, slow-client policy.
- `src/server/` (feature `server`) – loopback WebSocket server: `Origin` check, pings, frames ↔ bridge session.
- `src/backend/` – the `NetBackend` trait (the only way to the network) and its implementations:
  `pixelmapper_net/` (the real backend; mapping table in its module docs), `mock.rs`, `offline.rs`.
- `src/cli.rs` – `serve` (reusable by a later `pmnet serve`).

Where the real backend differs from or narrows the contract, see `protocol/bridge-ws.md` §11.

## Tests

```sh
cargo test -p pmnet-bridge            # unit tests, session and server tests on the mock, end-to-end tests on the simulator
cargo test -p pmnet-bridge --no-default-features   # the core alone (tests/session.rs)
cargo clippy -p pmnet-bridge --all-targets -- -D warnings
```

`tests/sim.rs` runs the real backend against `pixelmapper-net`'s simulated node (UDP on localhost, simulated tubes
and a third-party fixture) with a WebSocket client: discovery, TODs, RDM, `rdmDescribe`, node configuration, DMX,
tube firmware jobs and an unbound socket.
