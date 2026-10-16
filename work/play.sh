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
# Mailbox presentation by default: no tearing, no wait on the display, and no
# stall while the window is hidden. PS3RECOMP_VK_PRESENT=fifo is vsync.
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
# The tee below makes stderr a pipe, and with stderr not a terminal the
# runtime logs every SPU job and wait (thousands of lines a second, through
# one lock): quiet unless asked for.
export PS3_VERBOSE="${PS3_VERBOSE:-0}"
LOG="$HERE/runs/play-$(date +%Y%m%d-%H%M%S).log"
echo "log: $LOG"
# A hang leaves the thread stacks behind: when no "[rsx fps]" line has come
# for 30 s, eu-stack (elfutils) writes every thread's stack -- lifted guest
# functions included, as func_<address> -- next to the log, once.
watch_stall() {
    set +e                             # a failing probe must not end the watcher
    local gpid="" off=0 same=0 seen=0 size
    for _ in $(seq 1 100); do
        gpid=$(pgrep -P $$ -x MyGameRecomp | head -1); [ -n "$gpid" ] && break; sleep 0.2
    done
    [ -n "$gpid" ] && command -v eu-stack > /dev/null || return 0
    while kill -0 "$gpid" 2>/dev/null; do
        sleep 5
        # Only what was logged since the last look.
        size=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
        if [ "$size" -lt "$off" ]; then off=0; fi
        if tail -c +$((off + 1)) "$LOG" 2>/dev/null | head -c $((size - off)) | grep -aq "rsx fps\] [0-9]"; then
            same=0; seen=1
        elif [ "$seen" = 1 ]; then
            same=$((same + 5))
        fi
        off=$size
        if [ "$same" -eq 30 ] && kill -0 "$gpid" 2>/dev/null; then
            local out="${LOG%.log}.stall-$(date +%H%M%S).stacks"
            eu-stack -p "$gpid" -n 40 > "$out" 2>&1
            echo "[play.sh] no frame for 30 s -- thread stacks in $out" >&2
        fi
    done
}
watch_stall &
WATCH=$!
cd "$ROOT"
"$BIN" disc/PS3_GAME/USRDIR/EBOOT.elf 2>&1 | tee "$LOG"
kill "$WATCH" 2>/dev/null || true
