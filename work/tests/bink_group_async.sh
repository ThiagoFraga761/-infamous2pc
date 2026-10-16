#!/usr/bin/env bash
# Regression: an interpreted SPU thread with a bound receive queue is a service
# worker waiting on the PPU, so it must not run synchronously inside
# sys_spu_thread_group_start. inFAMOUS 2's Bink group (RADG5) did, blocked in
# receive_event on the PPU thread that had to send it, and the boot stopped
# before the first RSX command.
#
# PASS: the boot reaches the first RSX surface clear without RD_SPU_INTERP_ASYNC.
# Usage: tests/bink_group_async.sh [seconds]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
BIN="$WORK/port/build/MyGameRecomp"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
LOG="$(mktemp -t binkasync.XXXXXX.log)"
SECS="${1:-60}"

env -u RD_SPU_INTERP_ASYNC SDL_NO_SIGNAL_HANDLERS=1 SDL_AUDIODRIVER=dummy \
    timeout -k 5 "$SECS" "$BIN" "$ELF" > "$LOG" 2>&1

if ! grep -q 'name=RADG5' "$LOG"; then
    echo "FAIL: boot never created the Bink SPU group ($LOG)"; exit 2
fi
if ! grep -q '\[RSX\] CLEAR_SURFACE' "$LOG"; then
    echo "FAIL: no RSX surface clear after the Bink group started ($LOG)"; exit 1
fi
echo "PASS: boot passed the Bink group start and reached the RSX ($LOG)"
