#!/usr/bin/env bash
# End-to-end test: pixelmapper-net (Rust) against the virtual node (the real firmware code pm_node / pm_artnet /
# pm_rdm with a simulated RS-485 bus, node/firmware/test_host target `virtual_node`) over UDP on 127.0.0.1.
#
#   tools/e2e.sh                 build test_host (incl. virtual_node), run crates/pixelmapper-net/tests/e2e_virtual_node.rs
#   tools/e2e.sh <filter>        only tests whose name contains <filter>
#   PM_VN_LOG=1 tools/e2e.sh     packet log of the virtual nodes on stderr
#
# Findings (differences Rust <-> C <-> contract): docs/e2e-findings.md.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/node/firmware/test_host/build"
GEN=()
if [ ! -f "$BUILD/CMakeCache.txt" ] && command -v ninja >/dev/null; then GEN=(-G Ninja); fi

cmake -S "$ROOT/node/firmware/test_host" -B "$BUILD" ${GEN[@]+"${GEN[@]}"} >/dev/null
cmake --build "$BUILD" --target virtual_node

export PM_VIRTUAL_NODE="$BUILD/virtual_node"
cd "$ROOT"
LOG="$(mktemp -t pm_e2e.XXXXXX)"
trap 'rm -f "$LOG"' EXIT
set +e
cargo test -p pixelmapper-net --test e2e_virtual_node -- --test-threads 6 "$@" 2>&1 | tee "$LOG"
rc=${PIPESTATUS[0]}
set -e
summary="$(grep -E '^test result:' "$LOG" | tail -1)"
echo
echo "e2e: ${summary:-no result} (virtual node: $PM_VIRTUAL_NODE)"
exit "$rc"
