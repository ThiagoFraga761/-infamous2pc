#!/usr/bin/env bash
# Play the inFAMOUS 2 port in a window, with the game's own shaders on Vulkan.
#
#   work/play.sh            window + controller (SDL GameController) + sound
#   work/play.sh --dump N   also keep every N-th frame as work/runs/play/f_*.ppm
#
# A controller must be connected before launch (SDL picks it up at start).
# Close the window to quit.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
BIN="$HERE/port/build-vk/MyGameRecomp"
[ -x "$BIN" ] || { echo "missing $BIN -- build work/port/build-vk first" >&2; exit 1; }

export PS3RECOMP_VK_GUEST_PROGRAMS=1
# On by default in the port: the GPU does the ambient occlusion and the
# lighting the SPU jobs did. PS3RECOMP_NATIVE_SSAO=0 / PS3RECOMP_NATIVE_LIGHTING=0
# turn either off; =2 compares the GPU's result with the SPU's (a diagnostic).
export PS3RECOMP_VK_WINDOW=1
export SDL_NO_SIGNAL_HANDLERS=1
# Every 10 s, "[rsx fps] N fps ..." in this terminal.
export PS3RECOMP_ENG_FPSLOG=1
if [ "${1:-}" = "--dump" ]; then
    mkdir -p "$HERE/runs/play"
    export PS3RECOMP_VK_DUMP="$HERE/runs/play/f"
    export PS3RECOMP_VK_DUMP_EVERY="${2:-300}"
fi
# Each run's output also goes to work/runs/play-<time>.log, and any SPU job
# binary the port has no lift for to work/runs/spujob-miss/ -- both local and
# never committed: what a crash report needs.
mkdir -p "$HERE/runs/spujob-miss" "$HERE/runs/spujob-stuck"
export SPU_DUMP_MISS="${SPU_DUMP_MISS:-$HERE/runs/spujob-miss}"
# A job that gets stuck (past its step budget) leaves its staged local store
# here, for PS3RECOMP_SPU_REPLAY=<file> work/port/build-vk/MyGameRecomp.
export SPU_DUMP_STUCK="${SPU_DUMP_STUCK:-$HERE/runs/spujob-stuck}"
# Every 200 laps of a job chain: how long a lap runs and how long it waits at
# its guard (the audio chain has to keep up with the title's ~10 ms frames).
export SPURS_JC_TIMING="${SPURS_JC_TIMING:-1}"
LOG="$HERE/runs/play-$(date +%Y%m%d-%H%M%S).log"
echo "log: $LOG"
cd "$ROOT"
"$BIN" disc/PS3_GAME/USRDIR/EBOOT.elf 2>&1 | tee "$LOG"
