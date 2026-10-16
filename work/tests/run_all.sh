#!/usr/bin/env bash
# Every inFAMOUS 2 boot regression, one after the other (each boots the title).
# Usage: tests/run_all.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
fail=0
for t in zlib_task_context task_wait_signal wwise_chain_events bink_group_async \
         wws_interrupt spurs_contention first_draw second_frame; do
    out=$("$HERE/$t.sh" 2>&1 | tail -1); rc=$?
    echo "[$t] $out"
    case "$out" in PASS*) ;; *) fail=$((fail + 1)) ;; esac
done
echo "failed: $fail"
exit $((fail > 0))
