#!/usr/bin/env bash
# Benchmark: drive the title from boot to the first free gameplay (the pier
# after the opening, "Use the right stick to look up/left"), walk forward and
# report the frame rate there. Input comes from PAD_SCRIPT (menus) and
# PAD_FILE (the sticks); every 300th 3D frame is kept so a run can be checked.
# Usage: work/tests/bench_pier.sh [build dir, default build-vk]
# BENCH_PERF=<seconds>: also record a perf profile (user space) of that long
# from the start of the walk, to <out>/perf.data. PERF=<perf binary> when perf
# is not on PATH (LD_LIBRARY_PATH as it needs). BENCH_PERF_AT=tutorial starts
# it in the tutorial instead (~270 s, once the opening cutscene has ended), and
# BENCH_PERF_ARGS replaces the sampling options (default -F 999), e.g.
# "-b -c 20000003" for scripts/pgo.sh.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
BIN="$WORK/port/${1:-build-vk}/MyGameRecomp"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
OUT="$(mktemp -d -t pier.XXXXXX)"
PAD="$OUT/pad.txt"; : > "$PAD"
mkdir -p "$OUT/shots" "$OUT/save"
# A save directory of its own, empty: a new game every time, and a player's
# saves are never loaded or overwritten.
export PS3_SAVEDATA_ROOT="$OUT/save"
PS3_VERBOSE="${PS3_VERBOSE:-0}" SDL_NO_SIGNAL_HANDLERS=1 SDL_AUDIODRIVER=dummy PS3RECOMP_VK_GUEST_PROGRAMS=1 \
PS3RECOMP_ENG_FPSLOG=1 PS3RECOMP_ENG_SHOT="$OUT/shots" PS3RECOMP_ENG_SHOT_EVERY=300 \
PAD_FILE="$PAD" PAD_SCRIPT="45:0x4000,55:0x4000,65:0x4000,75:0x4000,85:0x4000,125:0x4000,135:0x4000,145:0x4000" \
    timeout -k 5 560 "$BIN" "$ELF" > "$OUT/game.log" 2>&1 &
TPID=$!
send() { echo "$1" > "$PAD"; }
PERF_PID=
start_perf() {
    [ -n "${BENCH_PERF:-}" ] || return 0
    local gpid
    gpid=$(pgrep -P "$TPID" -x MyGameRecomp | head -1)
    if [ -n "$gpid" ]; then
        # shellcheck disable=SC2086
        "${PERF:-perf}" record -q ${BENCH_PERF_ARGS:--F 999} -p "$gpid" -o "$OUT/perf.data" \
            -- sleep "$BENCH_PERF" > "$OUT/perf.log" 2>&1 &
        PERF_PID=$!
    else
        echo "BENCH_PERF: no game process to record" >&2
    fi
}
sleep 200
# The tutorial asks for the right stick up, then left; asking during the
# cutscene does nothing.
for i in $(seq 1 30); do
    [ "$i" = 10 ] && [ "${BENCH_PERF_AT:-}" = tutorial ] && start_perf
    send "s 128 128 128 0 60"; sleep 4
    send "s 128 128 0 128 60"; sleep 4
done
# Free roam: walk forward for the measured minute.
T0=$(date +%s)
[ "${BENCH_PERF_AT:-}" = tutorial ] || start_perf
while [ $(( $(date +%s) - T0 )) -lt 75 ]; do send "s 128 0 128 128 400"; sleep 5; done
kill -0 "$TPID" 2>/dev/null && kill "$(pgrep -P "$TPID" | head -1)" 2>/dev/null
wait "$TPID" 2>/dev/null
[ -n "$PERF_PID" ] && wait "$PERF_PID" 2>/dev/null
fps=$(grep -a "rsx fps\] [0-9]" "$OUT/game.log" | tail -7 | head -6 | awk '{s+=$3; n++} END{if(n) printf "%.1f", s/n}')
echo "pier: last minute avg ${fps} fps; all: $(grep -a 'rsx fps\] [0-9]' "$OUT/game.log" | awk '{print $3}' | tr '\n' ' ')"
echo "log and shots: $OUT"
