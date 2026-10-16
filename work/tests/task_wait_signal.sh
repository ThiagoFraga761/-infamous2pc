#!/usr/bin/env bash
# Regression: an interpreted SPURS task that blocks (WAIT_SIGNAL, taskset
# syscall 2) must park, not get 0 back and spin. inFAMOUS 2's EDGE zlib task
# spun on its LFQueue lock line (GETLLAR x millions) burning a core.
#
# PASS: no "syscall 2 not modelled", no SPU spin report, and the zlib
# pipeline still completes (event-flag wakes).
# The port runs this task lifted, so the test turns the lifts off
# (SPU_LIFT_DISABLE=1) to keep exercising the interpreted task path.
# Usage: tests/task_wait_signal.sh [seconds]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
BIN="$WORK/port/build/MyGameRecomp"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
LOG="$(mktemp -t taskwait.XXXXXX.log)"
SECS="${1:-40}"

SDL_NO_SIGNAL_HANDLERS=1 SDL_AUDIODRIVER=dummy SPU_LIFT_DISABLE=1 \
    timeout -k 5 "$SECS" "$BIN" "$ELF" > "$LOG" 2>&1

unmodelled=$(grep -c 'task syscall 2 not modelled' "$LOG")
spin=$(grep -c '\[spu-spin\] img=0 .*GETLLAR' "$LOG")
wakes=$(grep -c 'EventFlagWait WAKE' "$LOG")
if [ "$wakes" -eq 0 ]; then
    echo "FAIL: zlib pipeline never completed a block ($LOG)"; exit 2
fi
if [ "$unmodelled" -ne 0 ] || [ "$spin" -ne 0 ]; then
    echo "FAIL: $unmodelled unmodelled WAIT_SIGNALs, $spin SPU spin reports ($LOG)"; exit 1
fi
echo "PASS: task parks in WAIT_SIGNAL, no spin, $wakes zlib completions ($LOG)"
