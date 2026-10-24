# pixelmapper-hardware

Monorepo for the hardware side of Pixelmapper:

- **Pixelmapper Node** – 4-port Art-Net 4 / RDM node on an ESP32-P4 (C, ESP-IDF), configurable completely via
  Art-Net and RDM and at the device.
- **LED tubes** – custom ATmega8 firmware for the XBAR500 tubes with an RDM bootloader (PlatformIO).
- **pixelmapper-net** – Rust crate + CLI `pmnet` (Art-Net 4, RDM, discovery, node configuration, firmware update),
  used by the Pixelmapper backend.

The reverse-engineering history (dumps, logs, camera captures, the ESP32 helper) stays in the archive project
`ledTubeXBAR500`; it is not part of this repository.

## Layout

```
docs/                       plan.md (decisions), ui-konzept.md (front panel)
protocol/                   the contract between all parts
  contract.md               protocol contract for Pixelmapper (what we use, how the node behaves)
  artnet4-digest.md         verified excerpt of Art-Net 4 (rev 1.4dp) with offsets
  rdm-digest.md             E1.20 / E1.37-1/-2 excerpt with sources
  pids.toml                 single source of all manufacturer PIDs + constants
  gen.py                    pids.toml -> C header + Rust module, --check
  image-format.md           tube trailer, node image description block, .alf
  vectors/                  JSON test vectors (C and Rust tests run against them)
node/firmware/              ESP-IDF project (esp32p4), components pm_*
  test_host/                host tests (CMake) for pm_protocol, pm_artnet, pm_rdm
tube/
  firmware/                 tube_fw (PlatformIO, ATmega8)
  boot_rdm/                 tube_boot_rdm (2 KB RDM bootloader)
  tools/                    flash_rdm.py, flash_dmx.py, tool.py, sim/ (AVR emulator tests)
crates/pixelmapper-net/     Rust library + pmnet
```

## Quick start

```sh
# protocol definitions
python3 protocol/gen.py --check                 # pids.toml <-> generated files <-> tube rdm_boot.h

# node firmware (ESP-IDF v5.5.6, see CLAUDE.md for the environment)
cd node/firmware && idf.py set-target esp32p4 && idf.py build

# node host tests (no ESP-IDF needed)
cmake -S node/firmware/test_host -B node/firmware/test_host/build && cmake --build node/firmware/test_host/build
ctest --test-dir node/firmware/test_host/build --output-on-failure

# tube
~/.platformio/penv/bin/pio run -d tube/boot_rdm && ~/.platformio/penv/bin/pio run -d tube/firmware -e isp
python3 tube/tools/flash_rdm.py --selftest

# Rust
cargo build && cargo test
```

Conventions for contributors (human or agent): [CLAUDE.md](CLAUDE.md).
