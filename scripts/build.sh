#!/usr/bin/env bash
# Configure and build the port.
#
#   scripts/build.sh          work/port/build-vk, the Vulkan build you play
#   scripts/build.sh --tests  work/port/build too, the headless build tests use
#
# The lifted PPU code compiles with clang++ (work/port/tools/ppu_cxx.sh) in
# chunks of about 2 GB each, so the default is four jobs, and on systemd hosts
# the build runs in a 14 GB memory scope with no swap: a wide parallel build
# can exhaust RAM and take the desktop down. PORT_PPU_GCC=1 compiles them with
# GCC instead, ~5 GB a chunk, and two jobs. JOBS=<n> and PORT_SPU_MARCH=<march>
# override (x86-64-v3 for a CPU without AVX-512; the port checks at start-up
# that the CPU has what it was built for).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${PS3RECOMP_DIR:-$ROOT/ps3recomp}"
PORT="$ROOT/work/port"
if [ -n "${PORT_PPU_GCC:-}" ]; then JOBS="${JOBS:-2}"; else JOBS="${JOBS:-4}"; fi
MARCH="${PORT_SPU_MARCH:-x86-64-v4}"

[ -f "$ROOT/work/src/recomp/ppu_recomp.h" ] || { echo "nothing lifted yet -- run scripts/lift.sh" >&2; exit 1; }

configure() {   # <build dir> <vulkan ON|OFF>
    cmake -S "$PORT" -B "$1" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DPS3RECOMP_DIR="$DIR" -DRECOMP_DIR="$ROOT/work/src/recomp" -DSPU_DIR="$PORT/spu" \
        -DPS3RECOMP_RSX_VULKAN="$2" -DPORT_SPU_MARCH="$MARCH" \
        -DCMAKE_C_COMPILER_LAUNCHER="$PORT/tools/spu_cc.sh" \
        -DCMAKE_CXX_COMPILER_LAUNCHER="$PORT/tools/ppu_cxx.sh"
}
build() {
    if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
        systemd-run --user --scope -q -p MemoryMax=14G -p MemorySwapMax=0 nice -n 10 \
            cmake --build "$1" -j "$JOBS"
    else
        nice -n 10 cmake --build "$1" -j "$JOBS"
    fi
}

configure "$PORT/build-vk" ON
build "$PORT/build-vk"
if [ "${1:-}" = "--tests" ]; then
    configure "$PORT/build" OFF
    build "$PORT/build"
fi
echo "built: work/port/build-vk/MyGameRecomp -- run work/play.sh"
