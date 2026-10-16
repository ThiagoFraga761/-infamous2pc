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
# PS3RECOMP_RES_SCALE=2 (or 3) renders at that multiple of 1280x720.
# PS3RECOMP_VK_FULLSCREEN=1 opens the window fullscreen.
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
# A slow stretch leaves a profile behind: after three fps lines in a row under
# PLAY_PROFILE_BELOW (70), counting only windows that are mostly 3D frames
# (not the videos and menus), `perf record` samples the game for 20 s into
# <log>.perf-<time>.data -- at most three a session, two minutes apart.
# PLAY_PROFILE=0 turns it off; PERF=<binary> when perf is not on PATH.
PERF="${PERF:-$(command -v perf || true)}" PERF_LIBS="${LD_LIBRARY_PATH:-}"
if [ -z "$PERF" ] && [ -x "$HOME/tools/perf-rpm/root/usr/bin/perf" ]; then   # unpacked from the rpm
    PERF="$HOME/tools/perf-rpm/root/usr/bin/perf"
    PERF_LIBS="$HOME/tools/perf-rpm/root/usr/lib64${PERF_LIBS:+:$PERF_LIBS}"
fi
watch_stall() {
    set +e                             # a failing probe must not end the watcher
    local gpid="" off=0 same=0 seen=0 size chunk fps slow=0 profiles=0 last_prof=0
    for _ in $(seq 1 100); do
        gpid=$(pgrep -P $$ -x MyGameRecomp | head -1); [ -n "$gpid" ] && break; sleep 0.2
    done
    [ -n "$gpid" ] || return 0
    while kill -0 "$gpid" 2>/dev/null; do
        sleep 5
        # Only what was logged since the last look.
        size=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
        if [ "$size" -lt "$off" ]; then off=0; fi
        chunk=$(tail -c +$((off + 1)) "$LOG" 2>/dev/null | head -c $((size - off)) | grep -a "rsx fps\] [0-9]")
        if [ -n "$chunk" ]; then
            same=0; seen=1
            # "[rsx fps] F fps (N frames in 10s: W world, L light)": F, or 999 when W < 80% of N.
            for fps in $(echo "$chunk" | awk '{w = $9 + 0; n = substr($5, 2) + 0; print (n && w * 5 >= n * 4) ? int($3) : 999}'); do
                if [ "$fps" -lt "${PLAY_PROFILE_BELOW:-70}" ]; then slow=$((slow + 1)); else slow=0; fi
            done
        elif [ "$seen" = 1 ]; then
            same=$((same + 5))
        fi
        off=$size
        if [ "${PLAY_PROFILE:-1}" != 0 ] && [ -n "$PERF" ] && [ "$slow" -ge 3 ] && [ "$profiles" -lt 3 ] &&
           [ $(( $(date +%s) - last_prof )) -ge 120 ]; then
            local prof="${LOG%.log}.perf-$(date +%H%M%S).data"
            LD_LIBRARY_PATH="$PERF_LIBS" "$PERF" record -q -F 499 -p "$gpid" -o "$prof" -- sleep 20 > /dev/null 2>&1 &
            profiles=$((profiles + 1)); last_prof=$(date +%s); slow=0
            echo "[play.sh] fps under ${PLAY_PROFILE_BELOW:-70} for 30 s -- profiling 20 s into $prof" >&2
        fi
        if [ "$same" -eq 30 ] && kill -0 "$gpid" 2>/dev/null && command -v eu-stack > /dev/null; then
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
