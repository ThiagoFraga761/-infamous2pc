#!/usr/bin/env bash
# Regression: the SPU interrupt facility must work on the interpreter. inFAMOUS
# 2's WWS job manager (policy module 0x887080) idles in bie/bid waiting for its
# DMA-list stall-and-notify interrupt; only the lifted dispatch took
# interrupts, so -- interpreted -- loadJobState never left kReadCommands and
# the SpuLight kicker waited forever for its job list. The port now runs that
# module lifted, so the test turns the lifts off (SPU_LIFT_DISABLE=1) to keep
# exercising the interpreter's path.
#
# PASS: the interpreter takes interrupts (logged as "[spu-int] interp TAKE").
# Usage: tests/wws_interrupt.sh [seconds]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
BIN="$WORK/port/build/MyGameRecomp"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
LOG="$(mktemp -t wwsirq.XXXXXX.log)"
SECS="${1:-45}"

SDL_NO_SIGNAL_HANDLERS=1 SDL_AUDIODRIVER=dummy SPU_LIFT_DISABLE=1 \
    timeout -k 5 "$SECS" "$BIN" "$ELF" > "$LOG" 2>&1
rc=$?
# The title runs until the timeout stops it (124). Anything else is a crash or
# an early exit, which the log checks below could mistake for a pass.
if [ "$rc" -ne 124 ]; then
    echo "FAIL: exited early with status $rc before the ${SECS}s timeout ($LOG)"; exit 3
fi

if ! grep -q 'name=RADG5' "$LOG"; then
    echo "FAIL: boot never reached the Bink group ($LOG)"; exit 2
fi
taken=$(grep -c '\[spu-int\] interp TAKE' "$LOG")
if [ "$taken" -eq 0 ]; then
    echo "FAIL: no interrupt taken on the interpreter ($LOG)"; exit 1
fi
echo "PASS: interpreter took interrupts ($taken logged) ($LOG)"
