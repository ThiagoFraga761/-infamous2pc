#!/usr/bin/env bash
# Regression: kernel-side updates to the SPURS instance's control lines must be
# atomic against SPU GETLLAR/PUTLLC on the same lines. inFAMOUS 2's WWS policy
# module PUTLLCs the scheduling line to change readyCount; a kernel contention
# decrement landing inside such a commit was overwritten, leaving a phantom SPU
# in the SpuLight workload -- the PPU never re-signalled it and the frame's job
# lists were never consumed.
#
# PASS: whenever every SPU of instance 0x009D3E80 sits in the system service
# (wid 32), its current contention is zero.
# Usage: tests/spurs_contention.sh [seconds]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
BIN="$WORK/port/build/MyGameRecomp"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
LOG="$(mktemp -t spurscont.XXXXXX.log)"
SECS="${1:-80}"

SDL_NO_SIGNAL_HANDLERS=1 SDL_AUDIODRIVER=dummy SPURS_KWATCH=10 \
    timeout -k 5 "$SECS" "$BIN" "$ELF" > "$LOG" 2>&1

python3 -I - "$LOG" <<'EOF'
import re, sys
lines = open(sys.argv[1], errors="replace").read().splitlines()
samples = bad = 0
i = 0
while i < len(lines):
    m = re.match(r"\[kwatch\] inst 0x009D3E80 ready=\w+ cont=(\w+)", lines[i])
    if not m:
        i += 1; continue
    cont = m.group(1)
    # The SPU lines follow, possibly with other threads' output in between;
    # a sample counts only with all five of them (spu0..spu4) read whole.
    wids = {}
    j = i + 1
    while j < len(lines) and not lines[j].startswith("[kwatch] inst"):
        m2 = re.match(r"\[kwatch\]   spu(\d+) wid=(\d+)", lines[j])
        if m2:
            wids[int(m2.group(1))] = int(m2.group(2))
        j += 1
    if sorted(wids) == [0, 1, 2, 3, 4] and all(w == 32 for w in wids.values()):
        samples += 1
        if int(cont, 16) != 0:
            bad += 1
    i = j
if samples == 0:
    print("FAIL: no all-idle sample of instance 0x009D3E80 (%s)" % sys.argv[1]); sys.exit(2)
if bad:
    print("FAIL: %d of %d all-idle samples show phantom contention (%s)" % (bad, samples, sys.argv[1])); sys.exit(1)
print("PASS: %d all-idle samples, contention always zero (%s)" % (samples, sys.argv[1]))
EOF
