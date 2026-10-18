# job 207 (5cff) native — first look (2026-10-18)

Module `jobmod_5cfff962445a_e1390` (overlay/image 207), guest EA 0x85EE00,
loaded at LS 0x4000..0x57A0 (6,048 bytes; CRT 0x5390, API table at 0x57A0).
Listing (made with `tools/spu_disasm.py --base 0x4000` from the module GET in
`work/runs/job217/io`): `work/runs/job207/jobmod_5cfff962445a_e1390.asm`.
Status: read, not modelled. **It needs its own capture** (end of this note).

## What it looks like

The sibling of bc11 (`rpi/job-bc11-native/RESEARCH.md`): same job manager
interface, same output (a 512-byte header plus a bucket-sorted list of 8-byte
pairs, two atomic cursors, PUTs from buffer 2 +0x180 / +0x380), but a flat
walk instead of a tree:

- Wrapper 0x4B48: getbuf 1, 2, 3 as in bc11, then the worker
  `0x4030(hdr = buf1, planes = buf1+0x50, list = align16(buf1+0x50+32*w(hdr+0x10)), ...)`:
  the header is 0x50 bytes (bc11: 0x90), the BSP count is at +0x10.
- Worker 0x4030: for each 28-byte entry of the list (count in the header),
  three GETs into buffer 2 (+0, +0x4000, +0x8000, double-buffered with the
  next entry): a list of 28-byte boxes (EA +16, size +26), a list of 8-byte
  items (EA +20, count +24), and 64 bytes at +8 (a transform). Then per box:
  transform (0x5270), a height test on header+0x40, the BSP (0x4ED8), and the
  item into the output (bucket sort 0x4D08, list PUT 0x51B8, atomic add 0x54D8,
  memset 0x5568).
- My reading: per-instance lists (props, foliage-like sets) culled box by box
  against the same visibility BSP. ~700 instructions in the worker, about half
  of bc11's code.

In the bc11 capture it has 140 descriptors to bc11's 24, so it may well cost
more than bc11 did. That is a guess: its sample share is not measured.

## Why no oracle yet

Most of its jobs in `work/runs/job217/io` have no buffer-1 staging in the
capture (no list GET after the descriptor; only 5 jobs use the 0x173E0 list
with a 0x50-byte header at 0xE800). Its inputs cannot be rebuilt from that
capture, so a replay would only cover those 5 jobs.

## Capture wanted

`SPU_PUT_DUMP=207,40000,<dir>` in the heavy city area, camera turning, any
bc11 mode. Plus the sample share of image 207 from the profiler, to decide
whether it is worth the port.
