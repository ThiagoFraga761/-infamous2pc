#!/usr/bin/env bash
# Regression: SPURS job-chain completions must not reach queues that speak
# another protocol. inFAMOUS 2's Wwise chain ("Ak") used to be skipped (no
# lifted image) and replaced by a synthetic "job done" broadcast to every
# attached queue; the WWS job-manager handlers on queues 1/2 (tids 2/3) read it
# as a message and called through NULL in a loop.
#
# PASS: the Wwise jobs run (interpreted) and the WWS handler threads make no
# NULL calls.
# Usage: tests/wwise_chain_events.sh [seconds]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
BIN="$WORK/port/build/MyGameRecomp"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
LOG="$(mktemp -t wwisechain.XXXXXX.log)"
SECS="${1:-40}"

SDL_NO_SIGNAL_HANDLERS=1 SDL_AUDIODRIVER=dummy \
    timeout -k 5 "$SECS" "$BIN" "$ELF" > "$LOG" 2>&1

handler_null=$(grep -c -E 'bctr to NULL.*tid=(2|3)\)' "$LOG")
synthetic=$(grep -c -E 'chain 0x[0-9A-F]+ job [0-9]+ done -> event queue' "$LOG")
if ! grep -q 'JobGuardNotify' "$LOG"; then
    echo "FAIL: boot never reached the Wwise job chain ($LOG)"; exit 2
fi
if [ "$handler_null" -ne 0 ] || [ "$synthetic" -ne 0 ]; then
    echo "FAIL: $handler_null NULL calls in WWS handler threads, $synthetic synthetic job-done events ($LOG)"
    exit 1
fi
echo "PASS: Wwise chain ran with no synthetic events and no handler NULL calls ($LOG)"
