#!/usr/bin/env bash
# Regression: the RSX FIFO must wait at a jump-to-self park until the title
# patches it. inFAMOUS 2's SPU-built frames park the FIFO at the end of what is
# ready and patch the park with the rest of the frame; the walker's "jump-park"
# resync abandoned the park for `put`, so every later draw -- and the
# end-of-frame label 0x85 the SpuLight kicker waits on -- was never executed,
# and the title stopped presenting ~100 s in.
#
# PASS: frames are still being flipped in the last two RSX_STATS intervals.
# Usage: tests/second_frame.sh [seconds]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
BIN="$WORK/port/build/MyGameRecomp"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
LOG="$(mktemp -t secondframe.XXXXXX.log)"
SECS="${1:-180}"

SDL_NO_SIGNAL_HANDLERS=1 SDL_AUDIODRIVER=dummy RSX_STATS=15 \
    timeout -k 5 "$SECS" "$BIN" "$ELF" > "$LOG" 2>&1 &
TPID=$!
# Stall watch: the first time two samples in a row show no new flip, keep
# every thread's stack (eu-stack) beside the log -- a hang that happens once
# in dozens of boots is otherwise gone by the time anyone looks.
(
    while kill -0 "$TPID" 2>/dev/null; do
        sleep 5
        if [ "$(grep '\[rsx-stats\]' "$LOG" | tail -2 | grep -c 'flips=[0-9]* (+0,')" -ge 2 ]; then
            GPID=$(pgrep -P "$TPID" | head -1)
            if [ -n "$GPID" ] && command -v eu-stack > /dev/null; then
                eu-stack -p "$GPID" -n 30 > "$LOG.stacks" 2>&1
                echo "stall: stacks of pid $GPID in $LOG.stacks" >> "$LOG"
            fi
            break
        fi
    done
) &
wait "$TPID"
rc=$?
# The title runs until the timeout stops it (124). Anything else is a crash or
# an early exit, which the log checks below could mistake for a pass.
if [ "$rc" -ne 124 ]; then
    echo "FAIL: exited early with status $rc before the ${SECS}s timeout ($LOG)"; exit 3
fi

tail2=$(grep '\[rsx-stats\]' "$LOG" | tail -2)
if [ "$(printf '%s\n' "$tail2" | grep -c .)" -lt 2 ]; then
    echo "FAIL: not enough RSX_STATS samples ($LOG)"; exit 2
fi
if printf '%s\n' "$tail2" | grep -q 'flips=[0-9]* (+0,'; then
    echo "FAIL: presentation stopped -- $(printf '%s' "$tail2" | tail -1) ($LOG)"; exit 1
fi
echo "PASS: still presenting -- $(printf '%s' "$tail2" | tail -1) ($LOG)"
