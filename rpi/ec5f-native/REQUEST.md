# ec5f native: the render-list job, in C on the host

## Why

Job module `ec5f11015784` (overlay 225) is the busiest SPU job in the city
(~20% of SPURS time with its runtime overhead) and the one the main thread
waits on: during the main thread's job-list polls (0x370640/0x3710B8) it is
the job running on most SPUs. SPU time is what holds the heavy city view at
~60-65 fps. Replacing this job with host code (or a GPU pass) cuts the
critical path and the SPU load at once.

## What it does (measured 2026-10-18)

Per object, with double-buffered local-store slots:

- GETs: 176 bytes (0xB0, a scene node), 48 bytes, 64 bytes from a fixed
  per-batch address (frame constants), 16 + 48 bytes (material/texture refs).
- PUTs 4 KB blocks of RSX commands to IO 0x20000.. (EA 0x404xxxxx), 32-byte
  PUTs (bookkeeping), occasionally others.
- A block holds, per object: 4 texture units (7 words + filter, copied
  verbatim from the inputs), vertex-program constant bursts (material
  parameters at c[0x1D1..0x1D3], c[0x102]; object matrices at c[0x104],
  c[0x107], c[0x10A]: computed), 4 vertex array offsets, the index array
  offset and a DRAW_INDEX_ARRAY (computed), BEGIN/END. No program changes:
  objects arrive grouped by material.

Tools: `SPU_PUT_DUMP=225,<n>,<dir>` (GETs and PUTs in order),
`work/tools/rsx_decode.py` (command decoder).

## Acceptance

- Byte-identical command blocks against the SPU job on captured inputs.
- Same frame output (screenshots) in the city, the pier and New Marais.
- The SPU job is skipped only when the native path took the whole job;
  `PS3RECOMP_NATIVE_EC5F=0` keeps the SPU job.

## Out of scope

Other jobs; changing what is drawn.

## Research notes (2026-10-18, first pass)

- Per object, in order: 176-byte node at LS 0x27380/0x27480 (alternating),
  48 bytes at 0x27580/0x275B0, 64 bytes at 0x275E0/0x27620 (the same EA for
  a run of objects: per-batch constants), 16 bytes at 0x27200.., 48 bytes at
  0x27280.., then occasional larger reads (896 B at LS 0x1A000, 9216 B at
  0x1A510, 3072 B at 0x1C910, 10752 B at 0x28800: tables).
- Texture register blocks are copied verbatim from the inputs (26/27).
- c[0x100..0x103] (a 4x4 matrix per object) are computed; only scattered
  words equal input words (the translation column partly from the 48-byte
  read at +8/+40). c[0x104..] partly from a 512-byte table.
- Consequence: a bit-identical native version has to reproduce the SPU's
  float order of operations and truncating rounding (spu_helpers.h fma), or
  matrices differ by an ULP from what other passes compute for the same
  object (depth fighting).

## Drawable record (2026-10-18, second pass)

The 176-byte GET is two 80-byte drawable records (+16 bytes of a third).
660 of 660 emitted draws match a record by index offset.

| off  | field |
|------|-------|
| +00  | id |
| +04  | two u16 (orientation index?, flags) |
| +08  | pointer (material/mesh) |
| +0C  | flags (0x01000500/0x01000600) |
| +10  | 6 floats: local bounding box (min xyz, max xyz) |
| +28  | 4 floats: position xyz, scale? |
| +38  | VERTEX_DATA_ARRAY_OFFSET[0] (positions) |
| +3C  | second stream: attributes 10, 8, 3 at +0, +4, +0xA |
| +40  | 0xFFFFFFFF |
| +44  | INDEX_ARRAY_OFFSET |
| +48  | a count (not the index count: 0x1BB -> 648 indices) |
| +4C  | 1 |

Next: the 48-byte read (index count / LOD?), the orientation (the 512-byte
table read before c[0x104]), and the matrix arithmetic.

## Per-object inputs (2026-10-18, third pass)

| LS              | size | content |
|-----------------|------|---------|
| 0x27200/10/20   | 16   | instance-list entry: (count/flags, record ptr + 2, count, ptr) |
| 0x27280/B0/E0   | 48   | batch/material header (0x000A0000, pointers) -- few distinct |
| 0x27380/0x27480 | 176  | two 80-byte drawable records (+16 of the next) |
| 0x27580/0x275B0 | 48   | the object's 3x4 world transform (rotation rows + translation) |
| 0x275E0/0x27620 | 64   | a 4x4 sector/cell transform, shared by a run of objects |

So c[0x100..0x103] is presumably view-projection x sector x object; the
view-projection should be in one of the per-job tables (896 B at LS 0x1A000,
10752 B at 0x28800). A bit-identical host version can call the runtime's own
SPU float helpers (spu_fma & co., spu_helpers.h) in the order the lifted
matrix routine uses.

## c[0x100] is not view-projection (2026-10-18, fourth pass)

Captures now name the SPU context (`_c<ctx>_` in the file name: five run this
job at once and their transfers interleave). Fitting c[0x100] = VP x sector x
object (and transposes, both orders, lags 1-4) leaves no constant VP. Its
values are small (~0.01-1, translation row ~(-0.15, -0.52, 0.10), w column
0,0,0,1): it looks like the dequantization of 16-bit normalized positions
(the record's box at +0x10) folded into the object transform. Next: read the
lifted routine that writes the 0x100 burst instead of fitting formulas.
