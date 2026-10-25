# CLAUDE.md – conventions for everyone working in this repo

Read first: `docs/plan.md` (binding decisions), `protocol/contract.md` (protocol behaviour, [FEST] = fixed),
`protocol/pids.toml` (all numbers). Specs win over our documents: `protocol/artnet4-digest.md`,
`protocol/rdm-digest.md` (items marked [ungeprüft] are not verified against the ESTA PDFs).

## Directory ownership (Phase 2)

Each agent changes **only its own directories**. Everything else is read-only for it. If you need a change in a
file you do not own (a header of another component, `pm_protocol`, `pids.toml`, a vector, the test framework, this
file), **do not edit it** – describe the interface change in your report (file, old → new, reason) and work around
it locally until it is merged.

| Owner | Directories / files |
|---|---|
| Architect | `protocol/` (contract, digests, `pids.toml`, `gen.py`, `image-format.md`, `vectors/` incl. README and `schema.json`; existing vectors only change here), the generated `pm_pids.h` / `pids_generated.rs`, `CLAUDE.md`, `README.md`, `docs/` (except `STATUS.md`), root `Cargo.toml`, `crates/pixelmapper-net/` |
| Integrator | `docs/STATUS.md`, `node/firmware/components/pm_protocol/` (except `pm_pids.h`), `node/firmware/test_host/CMakeLists.txt`, `test_host/tools/`, `test_host/src/test_main.c`, `test.h`, `vectors.h`, `node/firmware/CMakeLists.txt`, `sdkconfig.defaults`, `partitions.csv`, `tools/` (incl. `tools/e2e.sh`), merges |
| `artnet` | `node/firmware/components/pm_artnet/`, `test_host/src/test_artnet*.c`, new files in `protocol/vectors/artnet/` |
| `rdm` | `node/firmware/components/pm_rdm/`, `test_host/src/test_rdm*.c`, new files in `protocol/vectors/rdm/` |
| `port` | `node/firmware/components/pm_port/`, `node/firmware/components/pm_board/` |
| `main/net` | `node/firmware/main/`, `components/pm_node/` (pure-C glue between pm_artnet, pm_rdm, pm_port and pm_ui; same rules as the other pure-C components), `pm_net/`, `pm_config/`, `pm_ota/` |
| `ui` | `node/firmware/components/pm_ui/` |
| Bridge session | `crates/pmnet-bridge/` (WebSocket bridge for the web app, own binary `pmnet-bridge serve`; protocol `protocol/bridge-ws.md`). `pmnet` itself gets no `serve` command |
| nobody (frozen) | `tube/` – running on hardware; changes only on explicit request, then the tube sim tests must pass |

Generated files change only through `pids.toml` + `gen.py`, and only **additively** while other packages build
against them (new defines/tables, no renamed or removed names).

Public headers in `components/*/include/` were written in Phase 0 and are the contract between agents. Implement
behind them; adding functions to your **own** header is fine, changing/removing existing ones needs a report entry.

## Code style

### C (node firmware)
- C11 (`-std=gnu17` under ESP-IDF; host tests build with `-std=c11 -Wall -Wextra -Werror -Wshadow`).
- ESP-IDF style: 4 spaces, K&R braces, `snake_case`, max ~120 columns, `/* */` comments, Doxygen `/** */` on all
  public declarations.
- Prefixes per component: `pm_artnet_*`/`pm_art*_` (packets), `pm_rdm_*`, `pm_port_*`, `pm_board_*`, `pm_net_*`,
  `pm_config_*`, `pm_ota_*`, `pm_ui_*`; shared types/macros `pm_*`/`PM_*` from `pm_protocol`. Static functions
  without prefix. Constants `PM_<COMP>_*`.
- **`pm_protocol`, `pm_artnet`, `pm_rdm`, `pm_node` are pure C**: no ESP-IDF/FreeRTOS includes, no malloc after init, no
  global state except what the API owns. Platform access only via the ops/callback structs. They must keep building
  in `test_host`.
- Errors: `pm_err_t` in pure C, `esp_err_t` in ESP-IDF components. No `abort()`/`assert` on network input.
- Numbers from `pids.toml` only via `pm_pids.h` (never re-type a PID or constant).
- Core assignment: core 1 = `pm_port` only; everything else core 0 (lwIP pinned to core 0). Cross-core only through
  `pm_port`'s API.
- Console/log output on **UART0 only** (`sdkconfig.defaults`): the USB-Serial-JTAG pads (GPIO 24/25) are DMX port 1
  pins on board rev 0, so there is no USB-JTAG console.

### Rust (`crates/pixelmapper-net`, `crates/pmnet-bridge`)
- Edition 2021, stable toolchain, `cargo fmt` clean, `cargo clippy --all-targets -- -D warnings` clean.
- No `unwrap()`/`expect()` on network input; errors via `thiserror`.
- Constants (PIDs, status IDs/names, enums) from the generated `pids` module only.
- `pmnet-bridge` talks to the network only through `pixelmapper-net` (its `NetBackend` adapter), never builds packets.

### Python (tools)
- Python ≥ 3.11 for `protocol/gen.py` (tomllib), stdlib only. Tube tools: `tube/tools/requirements.txt`.

## Generated files

`python3 protocol/gen.py` writes `node/firmware/components/pm_protocol/include/pm_pids.h` and
`crates/pixelmapper-net/src/pids_generated.rs`. Never edit them by hand. After any change to `pids.toml` run the
generator and `python3 protocol/gen.py --check` (also verifies `tube/firmware/include/rdm_boot.h`, which stays
hand-written because `flash_rdm.py` parses it).

## Toolchains and test commands

```sh
# ESP-IDF v5.5.6 in ~/esp/esp-idf (target esp32p4). The PlatformIO python must not shadow the system python:
export PATH="/opt/homebrew/bin:$(echo $PATH | tr ':' '\n' | grep -v platformio | paste -sd: -)"
. ~/esp/esp-idf/export.sh
cd node/firmware && idf.py build            # first time: idf.py set-target esp32p4

# Host tests (no IDF environment needed; Python 3 for the vector converter). One ctest run covers pm_tests
# (pm_protocol, pm_artnet, pm_rdm), pm_port, pm_net_tests (pm_net/pm_config/pm_ota) and pm_ui_render (UI renderer)
cmake -S node/firmware/test_host -B node/firmware/test_host/build -G Ninja
cmake --build node/firmware/test_host/build && ctest --test-dir node/firmware/test_host/build --output-on-failure

# Protocol definitions
python3 protocol/gen.py --check

# Rust, whole workspace (pixelmapper-net + pmnet-bridge)
cargo build --workspace && cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings && cargo fmt --all --check

# End-to-end: pixelmapper-net against the virtual node
tools/e2e.sh                                 # Ende-zu-Ende, siehe docs/STATUS.md

# Tube (only when tube/ is touched)
~/.platformio/penv/bin/pio run -d tube/boot_rdm && ~/.platformio/penv/bin/pio run -d tube/firmware -e isp
python3 tube/tools/flash_rdm.py --selftest
python3 tube/tools/sim/test_app.py           # slow (minutes), see tube/tools/sim/README.md
```

Host tests: every component agent adds `test_host/src/test_<component>_*.c` files (picked up by the glob) and calls
them from its existing entry function (`test_artnet()`, `test_rdm()`). New vector files are loaded automatically (C and
Rust; the Rust vector tests warn about packet types or fields they do not compare yet).

Before reporting "done": IDF build without warnings in your component, host tests green, `gen.py --check` green,
Rust checks for the whole workspace green (if touched).

## Chip revision

IDF v5.5 builds for ESP32-P4 rev ≥ 3.0 by default. If the Waveshare board has an older chip (`esptool.py chip_id`),
set `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` locally via `idf.py menuconfig` (not in `sdkconfig.defaults` until the
board revision is known).

## Git

- Do not commit unless asked. Never rewrite history. Commit messages end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- The archive project `~/Documents/projekte/ledTubeXBAR500` is read-only.
