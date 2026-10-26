# pmnet-bridge

WebSocket bridge between the Pixelmapper web app and Art-Net 4 / RDM. The app connects to `ws://127.0.0.1:7777/`,
sends intents (RDM GET/SET, node configuration, firmware updates, DMX frame sets) and receives state (nodes, TODs,
progress). The contract is [`protocol/bridge-ws.md`](../../protocol/bridge-ws.md); everything on the wire is done
by [`pixelmapper-net`](../pixelmapper-net).

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

## Structure

- `src/protocol/` – the contract as Rust types (no I/O).
- `src/server/` – loopback WebSocket server: handshake, `Origin` check, event fan-out, per-connection DMX routing
  and refresh, argument validation.
- `src/backend/` – the `NetBackend` trait (the only way to the network) and its implementations:
  `pixelmapper_net/` (the real backend; mapping table in its module docs), `mock.rs`, `offline.rs`.
- `src/cli.rs` – `serve` (reusable by a later `pmnet serve`).

Where the real backend differs from or narrows the contract, see `protocol/bridge-ws.md` §11.

## Tests

```sh
cargo test -p pmnet-bridge            # unit tests, server tests on the mock, end-to-end tests on the simulator
cargo clippy -p pmnet-bridge --all-targets -- -D warnings
```

`tests/sim.rs` runs the real backend against `pixelmapper-net`'s simulated node (UDP on localhost, simulated tubes
and a third-party fixture) with a WebSocket client: discovery, TODs, RDM, `rdmDescribe`, node configuration, DMX,
tube firmware jobs and an unbound socket.
