#!/usr/bin/env bash
# Milestone: the recompiled title issues real RSX draw calls (past the WWS
# SpuLight job-list wait that the SPURS contention leak wedged).
#
# PASS: at least one DRAW_INDEX_ARRAY reaches the RSX. The first one comes at
# ~100 s (the boot is load-bound), so the default leaves room for a busy host.
# Usage: tests/first_draw.sh [seconds]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
BIN="$WORK/port/build/MyGameRecomp"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
LOG="$(mktemp -t firstdraw.XXXXXX.log)"
SECS="${1:-150}"

SDL_NO_SIGNAL_HANDLERS=1 SDL_AUDIODRIVER=dummy \
    timeout -k 5 "$SECS" "$BIN" "$ELF" > "$LOG" 2>&1 &
TPID=$!
# A boot that has drawn nothing 15 s before the timeout keeps every thread's
# stack (eu-stack) beside the log: the boot hang this catches comes once in
# dozens of boots.
(
    sleep $((SECS > 30 ? SECS - 15 : SECS / 2))
    if kill -0 "$TPID" 2>/dev/null && ! grep -q '\[RSX\] DRAW_INDEX_ARRAY' "$LOG"; then
        GPID=$(pgrep -P "$TPID" | head -1)
        if [ -n "$GPID" ] && command -v eu-stack > /dev/null; then
            eu-stack -p "$GPID" -n 30 > "$LOG.stacks" 2>&1
            echo "no draw yet: stacks of pid $GPID in $LOG.stacks" >> "$LOG"
        fi
    fi
) &
WPID=$!
wait "$TPID"
rc=$?
kill "$WPID" 2>/dev/null
# The title runs until the timeout stops it (124). Anything else is a crash or
# an early exit, which the log checks below could mistake for a pass.
if [ "$rc" -ne 124 ]; then
    echo "FAIL: exited early with status $rc before the ${SECS}s timeout ($LOG)"; exit 3
fi

draws=$(grep -c '\[RSX\] DRAW_INDEX_ARRAY' "$LOG")
if [ "$draws" -eq 0 ]; then
    echo "FAIL: no RSX draw call within ${SECS}s ($LOG)"; exit 1
fi
echo "PASS: $draws draw calls logged ($LOG)"
