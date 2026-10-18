# Plan: --local-regs for the PPU lifter

## Phase 1: lifter pass (tools/ppu_lifter.py)
- `localize_ppu_registers(lines)`: module-level pass run by
  `_function_def_lines` on the final body (after CR fusion, stack accessors
  and the fall-through trampoline) when `PPULifter.local_regs` is set.
- Classify each line: label / pure / cond-exit / exit / sync.
- Dirty-set dataflow (forward, may) over the line CFG; write back only the
  dirty locals at syncs and exits; reload every used local after a sync and
  in front of each `goto` inside a sync line.
- CLI flag `--local-regs`, off by default.
- Gate: default lift of the whole game byte-identical to the backup lifter.

## Phase 2: tests
- Unit test `tools/test_ppu_local_regs.py`: classification, write-backs at
  calls/exits, reload after calls, gotos out of sync lines, compile+run of a
  small lifted function both ways.
- Existing `tools/test_ppu_*.py` still pass (two were already failing before
  the change: test_ppu_cs_epilogue_restore, test_ppu_vcmp_record_cr6).

## Phase 3: differential harness (scratch, not in the toolkit)
- Lift the game both ways into work/runs/ppuloc/{base,loc}.
- For each of ~60 hot main-thread functions: extract both definitions, build
  with the port's clang flags against mocked callees (each mock hashes the
  full ctx it sees, then scrambles volatile and some non-volatile registers
  and memory), run N random ctx/memory states, compare event traces
  (stores, calls with ctx snapshots, final ctx).
- Time both versions on the same inputs with the inline memory paths on.

## Phase 4: compile check
- One chunk with `--local-regs`, port flags, under
  `systemd-run --user --scope -p MemoryMax=12G -p MemorySwapMax=0`.
  Record time and peak RSS against the same chunk without the flag.

## Rollback
The flag is opt-in; dropping it from scripts/lift.sh restores today's output.

## Status (2026-10-18)
- Phase 1 done: `localize_ppu_registers` + `--local-regs` in tools/ppu_lifter.py.
  Default lift of the whole game byte-identical to the backup lifter (33/33
  files). With the flag: 37156 functions localized, 2195 with no register to
  localize, 0 left unsupported; lift 2m31 vs 1m54.
- Phase 2 done: tools/test_ppu_local_regs.py passes; the other test_ppu_*.py
  pass except the two that already failed before the change.
- Phase 3 done: harness sources in work/runs/ppuloc/harness_src, results in
  work/runs/ppuloc/harness2/run2.txt. 135 functions x 300 states, 0
  mismatches. Mutants (a write-back or reload dropped) are caught.
  Synthetic-input timing: +5.5% time-weighted; func_00314BC8 +16%, leaf
  functions on short paths can lose up to ~10% (entry loads, may-dirty
  write-backs).
- Phase 4 done: chunk 001 with the flag, port flags: 216 s / 2.53 GB peak
  (without: 208 s / 2.54 GB); .text +10%.
- Open: in-game measurement (user).
