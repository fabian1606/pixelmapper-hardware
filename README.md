# pixelmapper-hardware

Monorepo for the hardware side of Pixelmapper:

- **Pixelmapper Node** – 4-port Art-Net 4 / RDM node on an ESP32-P4 (C, ESP-IDF), configurable completely via
  Art-Net and RDM and at the device.
- **LED tubes** – custom ATmega8 firmware for the XBAR500 tubes with an RDM bootloader (PlatformIO).
- **pixelmapper-net** – Rust crate + CLI `pmnet` (Art-Net 4, RDM, discovery, node configuration, firmware update,
  simulated node and tubes), used by the Pixelmapper backend.
- **pmnet-bridge** – Pixelmapper bridge: WebSocket server between the Pixelmapper web app and Art-Net/RDM
  (own binary `pmnet-bridge serve`, protocol in `protocol/bridge-ws.md`).

The reverse-engineering history (dumps, logs, camera captures, the ESP32 helper) stays in the archive project
`ledTubeXBAR500`; it is not part of this repository.

## Layout

```
docs/                       plan.md (decisions), ui-konzept.md (front panel), STATUS.md (current state)
protocol/                   the contract between all parts
  contract.md               protocol contract for Pixelmapper (what we use, how the node behaves)
  artnet4-digest.md         verified excerpt of Art-Net 4 (rev 1.4dp) with offsets
  rdm-digest.md             E1.20 / E1.37-1/-2 excerpt with sources
  pids.toml                 single source of all manufacturer PIDs + constants
  gen.py                    pids.toml -> C header + Rust module, --check
  image-format.md           tube trailer, node image description block, .alf
  bridge-ws.md              WebSocket protocol of pmnet-bridge (web app <-> bridge)
  vectors/                  JSON test vectors (C and Rust tests run against them)
node/firmware/              ESP-IDF project (esp32p4), components pm_*
  components/pm_node/      pure-C glue between pm_artnet, pm_rdm, pm_port and pm_ui (Phase 2)
  test_host/                host tests (CMake): pm_protocol, pm_artnet, pm_rdm, pm_port, pm_net/config/ota, UI renderer
tube/
  firmware/                 tube_fw (PlatformIO, ATmega8)
  boot_rdm/                 tube_boot_rdm (2 KB RDM bootloader)
  tools/                    flash_rdm.py, flash_dmx.py, tool.py, sim/ (AVR emulator tests)
crates/pixelmapper-net/     Rust library + pmnet
crates/pmnet-bridge/        WebSocket bridge for the web app (pmnet-bridge serve)
tools/e2e.sh                end-to-end test (see docs/STATUS.md)
```

## Quick start

```sh
# protocol definitions
python3 protocol/gen.py --check                 # pids.toml <-> generated files <-> tube rdm_boot.h

# node firmware (ESP-IDF v5.5.6, see CLAUDE.md for the environment)
cd node/firmware && idf.py set-target esp32p4 && idf.py build

# node host tests (no ESP-IDF needed): pm_tests, pm_port, pm_net_tests, pm_ui_render in one ctest run
cmake -S node/firmware/test_host -B node/firmware/test_host/build -G Ninja && cmake --build node/firmware/test_host/build
ctest --test-dir node/firmware/test_host/build --output-on-failure

# tube
~/.platformio/penv/bin/pio run -d tube/boot_rdm && ~/.platformio/penv/bin/pio run -d tube/firmware -e isp
python3 tube/tools/flash_rdm.py --selftest

# Rust (whole workspace: pixelmapper-net + pmnet-bridge)
cargo build --workspace && cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings && cargo fmt --all --check

# end-to-end: pixelmapper-net against the virtual node
tools/e2e.sh                                    # end-to-end, see docs/STATUS.md
```

Conventions for contributors (human or agent): [CLAUDE.md](CLAUDE.md).
