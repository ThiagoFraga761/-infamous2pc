# job 222 native — research, first pass (2026-10-19)

Module `jobmod_e34204e872fe` (overlay/image 222, General), LS 0x4000..0x13A40
(0xFA40 bytes, .bss to 0x13BD0). CRT 0x131F0, main 0xDEC8 (via 0xE1F8), API
table pointer at 0x13A40. Listing: `work/runs/job222/
jobmod_e34204e872fe_eF1F0.asm`. Captures: `work/runs/job222/io/`
(SPU_PUT_DUMP=222,40000, heavy_bench on the Rog; 594 jobs, 5 contexts).

**What it is: a collision query against an AABB tree in main memory.** The
128-byte query (buffer 1 +0): two points (+0x00, +0x10, with flags
`02ff00b0` and a pointer), an AABB (+0x20/+0x30, here ±FLT_MAX) with the
root node EA at +0x3C, the object at +0x40, flags at +0x50 (0x80, 0xB0,
0xB2...), an id at +0x60, the result EA at +0x74 (a ring at 0xA73430 + 16k)
and a counter line at +0x6C (0xA73418). Nodes are 256-byte blocks of eight
32-byte child boxes (min xyz + flags, max xyz + child EA), fetched by DMA as
the walk goes; leaves (`02ff..b0`) point at primitive blocks (the many
32..80-byte GETs to 0x37140..0x373D0). Result: one 16-byte PUT from LS
0x14C00, (query +0x40, a 32-bit id, 0, 0), plus an atomic add on line
0xA73400. In 25 of 588 jobs the id differs from the query's: the hit.

The code is continuation-passing: a DMA queue object (0x2000+ bytes) whose
steps resume at addresses stored as data (0x5E50, 0x5E80 ... 0x6410, 0xFA18:
each pair a compute step and a channel step), so "functions" in the lift are
pieces between DMA waits.

## Replay oracle (`work/runs/job222/oracle/`)

- `harness.c` (job220's, module size/entry changed; BUF1/BUF1SZ/BUF2SZ env;
  PUTLLC logs the line).
- Layout found by matching the captured LSAs: buffer 1 = the job's region
  (0x14400, list at 0x14BE0; or 0x13C00, list at 0x143E0: the manager
  double-buffers and stages the next job before this one runs), buffer 2 =
  0x14C00 (the result), **buffer 3 = 0x18C00** (the DMA queue object is at
  buffer 3 + 0x20800 = 0x39400; its two node slots are +0 and +0x1000),
  **r1 = 0x3F990** (the root node goes to the stack, 0x3F640).
- `jobs.py`: a job = its list GET and three elements (query, 2 x 16 B); the
  window ends at its result PUT (EA = query +0x74), not at the next list.
- `runall.py`: **588/588 jobs run to the end, 588/588 result PUTs
  byte-exact, 35407/35407 mid-job GETs served exactly** (same EA, LSA, size,
  order): the walk is reproduced step for step.

## Where the time goes

Lifted, in game: 2.4% of all samples (Rog, heavy area), about half of it in
the tree step (0x10250 and its pieces, ~30% of instructions) and the
primitive tests (the 0x6238 continuation family, ~25%); the runtime's DMA
path (ll_take, mfc_do_transfer, spu_mfc_cmd) adds to it, about 60 GETs per
job. Interpreter: 12.6k instructions per job on average.

## Next

A whole-job native walk reading guest memory directly (no DMA at all) is
the natural port: the output is one 16-byte record and a counter. It needs
the node/leaf/primitive formats and the query kinds (+0x50 flags) decoded,
and the hit tests' float sequence copied exactly (the id depends on the
nearest hit).
