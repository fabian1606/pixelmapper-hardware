# CLAUDE.md – conventions for everyone working in this repo

Read first: `docs/plan.md` (binding decisions), `protocol/contract.md` (protocol behaviour, [FEST] = fixed),
`protocol/pids.toml` (all numbers). Specs win over our documents: `protocol/artnet4-digest.md`,
`protocol/rdm-digest.md` (items marked [ungeprüft] are not verified against the ESTA PDFs).

## Directory ownership (Phase 1)

Each agent changes **only its own directories**. Everything else is read-only for it. If you need a change in a
file you do not own (a header of another component, `pm_protocol`, `pids.toml`, a vector, the test framework, this
file), **do not edit it** – describe the interface change in your report (file, old → new, reason) and work around
it locally until it is merged.

| Owner | Directories / files |
|---|---|
| Architect (Phase 0) | `protocol/` (contract, digests, `pids.toml`, `gen.py`, `image-format.md`, `vectors/README.md`, `schema.json`), `node/firmware/components/pm_protocol/`, `node/firmware/test_host/CMakeLists.txt`, `test_host/tools/`, `test_host/src/test_main.c`, `test.h`, `vectors.h`, `node/firmware/CMakeLists.txt`, `sdkconfig.defaults`, `partitions.csv`, `CLAUDE.md`, `README.md`, `docs/`, root `Cargo.toml` |
| Agent `artnet` | `node/firmware/components/pm_artnet/`, `test_host/src/test_artnet*.c`, `protocol/vectors/artnet/*.json` (add new files; existing vectors only change via report) |
| Agent `rdm` | `node/firmware/components/pm_rdm/`, `test_host/src/test_rdm*.c`, `protocol/vectors/rdm/*.json` (same rule) |
| Agent `port` | `node/firmware/components/pm_port/`, `node/firmware/components/pm_board/` |
| Agent `net` | `node/firmware/components/pm_net/`, `pm_config/`, `pm_ota/`, `node/firmware/main/` |
| Agent `ui` | `node/firmware/components/pm_ui/` |
| Agent `rust` | `crates/pixelmapper-net/` (except `src/pids_generated.rs`, which `protocol/gen.py` writes) |
| nobody (frozen) | `tube/` – running on hardware; changes only on explicit request, then the tube sim tests must pass |

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
- **`pm_protocol`, `pm_artnet`, `pm_rdm` are pure C**: no ESP-IDF/FreeRTOS includes, no malloc after init, no
  global state except what the API owns. Platform access only via the ops/callback structs. They must keep building
  in `test_host`.
- Errors: `pm_err_t` in pure C, `esp_err_t` in ESP-IDF components. No `abort()`/`assert` on network input.
- Numbers from `pids.toml` only via `pm_pids.h` (never re-type a PID or constant).
- Core assignment: core 1 = `pm_port` only; everything else core 0. Cross-core only through `pm_port`'s API.

### Rust (`crates/pixelmapper-net`)
- Edition 2021, stable toolchain, `cargo fmt` clean, `cargo clippy --all-targets -- -D warnings` clean.
- No `unwrap()`/`expect()` on network input; errors via `thiserror`.
- Constants from the generated `pids` module only.

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

# Host tests of the pure-C components (no IDF environment needed; Python 3 for the vector converter)
cmake -S node/firmware/test_host -B node/firmware/test_host/build -G Ninja
cmake --build node/firmware/test_host/build && ctest --test-dir node/firmware/test_host/build --output-on-failure

# Protocol definitions
python3 protocol/gen.py --check

# Rust
cargo build && cargo test && cargo clippy --all-targets -- -D warnings && cargo fmt --check

# Tube (only when tube/ is touched)
~/.platformio/penv/bin/pio run -d tube/boot_rdm && ~/.platformio/penv/bin/pio run -d tube/firmware -e isp
python3 tube/tools/flash_rdm.py --selftest
python3 tube/tools/sim/test_app.py           # slow (minutes), see tube/tools/sim/README.md
```

Host tests: every component agent adds `test_host/src/test_<component>_*.c` files (picked up by the glob) and calls
them from its existing entry function (`test_artnet()`, `test_rdm()`). New vector files are loaded automatically.

Before reporting "done": IDF build without warnings in your component, host tests green, `gen.py --check` green,
Rust checks green (if touched).

## Chip revision

IDF v5.5 builds for ESP32-P4 rev ≥ 3.0 by default. If the Waveshare board has an older chip (`esptool.py chip_id`),
set `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` locally via `idf.py menuconfig` (not in `sdkconfig.defaults` until the
board revision is known).

## Git

- Do not commit unless asked. Never rewrite history. Commit messages end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- The archive project `~/Documents/projekte/ledTubeXBAR500` is read-only.
