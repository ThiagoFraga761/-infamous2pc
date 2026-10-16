#!/usr/bin/env bash
# Regression: the interpreted SPURS task path must give a task its taskset
# context, or inFAMOUS 2's EDGE zlib task fails its blocking LFQueue pop with
# CELL_SPURS_TASK_ERROR_PERM and dies on the assert at
# edgezlib_inflate_task.cpp:270 (LS 0x3304) before the first push.
#
# PASS: the task never reaches LS 0x3304 and gets past the blocking pop check.
# The port runs this task lifted, so the test turns the lifts off
# (SPU_LIFT_DISABLE=1) to keep exercising the interpreted task path.
# Usage: tests/zlib_task_context.sh [seconds]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
BIN="$WORK/port/build/MyGameRecomp"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
LOG="$(mktemp -t zlibctx.XXXXXX.log)"
SECS="${1:-25}"

# SDL turns SIGTERM into SDL_QUIT, which a hung boot never drains: without
# the hint (and the -k backstop) timeout leaves the process running.
SDL_NO_SIGNAL_HANDLERS=1 SDL_AUDIODRIVER=dummy SPU_LIFT_DISABLE=1 \
SPU_TRACE_INTERP=1 SPU_TRACE=1 SPU_TRACE_MAX=200000 \
    timeout -k 5 "$SECS" "$BIN" "$ELF" > "$LOG" 2>&1

pcs=$(grep -c -E '^[0-9A-F]{5}$' "$LOG")
if [ "$pcs" -eq 0 ]; then
    echo "FAIL: no interpreted SPU instructions traced ($LOG)"; exit 2
fi
if grep -q -E '^03304$' "$LOG"; then
    echo "FAIL: zlib task hit the assert at edgezlib_inflate_task.cpp:270 after $pcs SPU instructions ($LOG)"
    exit 1
fi
echo "PASS: zlib task passed the blocking pop check ($pcs SPU instructions traced; $LOG)"
