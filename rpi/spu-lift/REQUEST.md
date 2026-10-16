# SPU lift for inFAMOUS 2 (BCUS98125)

## Problem

Once the RSX FIFO park fix landed, the title renders gameplay-scale frames
(~2000 draws per frame), but only at 3-4 fps. Profiling on 2026-10-14
(per-thread CPU + eu-stack samples, `work/runs/prof1.log`):

- 5 SPURS kernel threads at ~97% CPU, all in `spu_step` under
  `spu_pm_enter` -> `spu_interp_run_until`: the WWS Job Manager policy module
  (EA 0x887080, 11904 bytes, fp 0xB3523C4B17650EF8) and the job modules it
  streams into local store, interpreted.
- 1 job-chain thread at ~77%, interpreting SPURS jobs (fp 0x64D9D14299A2B7AB,
  272 B; fp 0x054414E2CCD626FF, 1040 B).
- 1 taskset task interpreted (taskset 0x47D7E380 task 0).
- Main PPU thread ~84% in lifted game code (real work, not the bottleneck yet).

`SPU_INTERP_HIST=1` (`work/runs/hist1.log`, 11.2 G steps): ~18% of the
interpreted steps are in LS pages 0x1000-0x2FFF (the PM), ~73% in pages
0x5000-0xFFFF (job code the PM streams in). The interpreter runs ~10 M
SPU instructions/s per thread, two orders of magnitude below the hardware.

## Goal

Run the SPU code natively (lifted with `tools/spu_lifter.py`), the way the PPU
side already is, so the SPU side stops being the frame-rate ceiling.

Done means:
- the WWS PM runs lifted (kernel log no longer says "not lifted: interpreting"
  for 0x887080);
- the job modules the PM streams in run lifted when resident;
- the job-chain jobs and the taskset task run lifted;
- `work/tests/run_all.sh` stays green and RSX_STATS shows a clear fps gain in
  the gameplay phase (baseline: 3.4-4.3 fps at t=120-225 s).

## Out of scope

Rendering backend, frame pacing to 120 fps, FSR. Those come after this.

## Known prior art in the toolkit

- `tools/spu_lifter.py`, `find_spu_functions.py`, `wrap_spu_elf.py`,
  `build_spu_workloads.py`, `lift_jobmods.py`, `lift_firmware_spu.py --body`.
- Runtime: `spu_workload_register_img` (kernel finds a lifted PM by content
  fingerprint), overlay regions/signatures for streamed code
  (`spu_overlay_register_region`, `spu_overlay_match_sig`), interpreter <->
  lifted rejoin.
- A WWS special case for another title is hard-coded in
  `runtime/spu/spu_channels.c` (`image_id == 2`, code base read from
  LS[0x1320]); inFAMOUS 2's PM is a different build.
