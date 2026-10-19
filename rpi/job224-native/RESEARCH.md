# job 224 native — research, first pass (2026-10-19)

Module `jobmod_eac33ee65aa1` (overlay/image 224, General), LS 0x4000..0xDDD0
(0x9DD0 bytes). CRT 0xD808, main 0xBE78 (via 0xC1E8), API
table pointer at 0xDDD0. Listing: `work/runs/job224/
jobmod_eac33ee65aa1_e9808.asm`. Captures: `work/runs/job224/io/`
(SPU_PUT_DUMP=224, heavy_bench on the Rog: only 1888 transfers, 107 jobs in
200 s standing still: this job is rare there).

**What it is (not decoded yet).** No DMA inside the job: the manager stages
an 80-byte list (LS 0x2E3B0) of nine elements from 0xE000: a 32-byte header
and four tables (3568, 5328, 2080, 2080 B; the same EAs in every job), two
small blocks, then N records of 1040 B and N of 256/384 B, which the job
rewrites and PUTs back with DMA lists (0xC1F0) to the descriptor's words 0
and 1. About 23k instructions a job, a fifth of them in 0xD980/0xD9F8.

## Replay oracle (`work/runs/job224/oracle/`)

- `harness.c`: buffer 1 = 0xE000 (0x20400 bytes), buffer 2 = 0x2E400; the
  descriptor's two EAs are preloaded at 0xC60 (the dump never has the
  descriptor; `jobs.py` takes them from the list's last two entries).
- `runall.py`: **107/107 jobs to the end, 214/214 PUTs whole-transfer
  byte-exact.**

## Next

Decode the records and tables; a whole-job replacement fits (inputs and
outputs are all manager buffers). Its in-game share is small standing still
(under 0.5% of samples); capture it where it runs more before porting.
