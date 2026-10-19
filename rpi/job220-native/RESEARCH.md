# job 220 native — research (2026-10-19)

Module `jobmod_d7096a8ffc1c` (overlay/image 220, Render4), loaded at LS
0x4000..0xA5D0 (0x65D0 bytes; .bss to 0xA690). CRT 0xA0C0, main 0x9530 (via
0x9938), API table pointer at 0xA5D0. Listing: `work/runs/job220/
jobmod_d7096a8ffc1c_e60C0.asm` (from `tools/spu_disasm.py --base 0x4000` on
`work/port/spu/jobmods/jobmod_d7096a8ffc1c_e60C0.bin`). Captures:
`work/runs/job220/io/` (SPU_PUT_DUMP=220,40000, heavy_bench on the Rog,
standing still; 298 jobs in 5 SPU contexts).

**What it is.** Immediate-mode RSX geometry: per object, polygons are clipped
through a BSP tree the job builds in local store, and the result is written as
RSX command buffers of inline vertices (`0x00041808` BEGIN_END,
`0x000C1500` = 3 floats of vertex attribute 0 per vertex, JUMP commands to
link 4 KB chunks). Chunks come from a cursor shared by every SPU (atomic add
of 0x1000 on line 0xA00A00 +4; the EA→RSX-offset bias and the ring limit are
in buffer 1 +0x54..+0x5C).

## 1. Replay oracle

`work/runs/job220/oracle/` (my code, no game bytes), the job207 harness
adapted:

- `harness.c` (build line in its header): interpreter + fake manager. The
  descriptor is not in the dump (fetched before the overlay is resident); the
  job reads only its first word, the EA of the final buffer-2 PUT, which
  `jobs.py` takes from the window's PUT from LS 0xF800. getbuf: 1 = 0xA800
  (list at 0xCFE0, elements from 0xA800), 2 = 0xF800, 3 = 0x1B800 (captured
  LSAs). **Buffer 3 must be at least 0x10000 bytes**: lists and polygons are
  allocated downwards from its end; with 0x3C00 or 0x8000 the pool runs into
  the GET slots and the job loops (runall.py uses 0x18000).
- `jobs.py`: a job = the 32-byte list GET to 0xCFE0 and its four elements;
  window to the next one. The chunk cursor returns the window's 4 KB PUT EAs
  in order (harness ATOMSEQ). One job of another layout (list at 0xF7E0) is
  not split out and inflates job 43's window.
- `runall.py`: runs A (zeroed LS), B/C (LS filled 0xAA/0x55). **293/293
  complete jobs run to the end; 824/824 issued PUTs byte-exact on every byte
  the job writes** (358 whole-transfer exact: chunk tails and the final PUT
  carry bytes the job never writes). The 4 captured PUTs not issued belong to
  the uncut job in job 43's window.

## 2. Where the time goes

Lifted code in game (perf, Rog, heavy area, whole process): the module is
2.7% of all samples, 5.3% of SPU time. By function: 0x60A0 33%, 0x5A98 27%,
0x68D8 6%, 0x4D80 6%, 0x7C40 6%, 0x9DF8 4%, the rest under 3% each. The
interpreter's instruction counts agree (0x60A0 37%, 0x5A98 25%).

Instruction-by-instruction kernels (tools/kgen.py) of 0x60A0, 0x5A98 and
0x4D80 were exact but no faster than the lift (0.97x..1.09x, 0.26 us a call):
the calls are short and the lift is already tight. What pays is one native
function for the whole clipper subtree, 0x68D8 with 0x60A0, 0x5A98 and
0x4A88: about two thirds of the job.

## 3. The clipper (0x68D8 and callees)

S = r3 (the object's work struct, 16-aligned; on 7C40's stack in practice).

| S+ | content |
|----|---------|
| 0x30 | byte 0: overflow flag; w1: node table base; w2: node count |
| 0x40 | w1: node table capacity; w2: stack base; w3: stack depth |
| 0x50 | w3: allocator top (lists and polygons are taken below it, no bound) |

Polygon: +0x00 plane (nx, ny, nz, d), +0x10 w0 flags / w1 vertex count n,
+0x20 n quads whose word 0 points at the vertex (x, y, z, w). List: w0 count,
entries one quad each from +0x10. Node (32 B at base + 32k): plane, then w0 /
w1 = offset in nodes to the child for the negative / positive side (0: none).
Stack record (32 B): node quad, list quad.

- **0x68D8(S, node, list)**: until the overflow flag: allocate the positive
  list Lp (top - 16 - 16·count), and if child 0 exists the negative list Ln;
  split by the node's plane (0x60A0, or 0x5A98 without child 0: the negative
  side is dropped); push (child 0, Ln) when Ln is not empty; with child 1
  descend with Lp; without it append Lp's planes as nodes (0x4A88) and set
  w1 = (count before) - (node index); then pop, or stop.
- **0x60A0(S, plane, L, neg, pos) / 0x5A98(S, plane, L, -, pos)**: per
  polygon: coplanar (|n·n'| > 0x3F7FBE76 and |dist(v0)| < 0x3DCCCCCC) goes to
  pos (and neg); else classify every vertex (dist < 0, strict), remember the
  last index of each transition kind in three stack quads (sp-0x30/-0x20/-0x10
  of the caller's frame, initialised to -1); one side → that list; otherwise
  two new vertices at top-32 / top-16 and one (5A98) or two (60A0) new
  polygons, the positive one first. The crossing points: the four distances
  (C, A, D, B) in one quad (fm, fma, fma, fs), one frest/fi/fnms/fma
  reciprocal for both, `cuflt(cfltu(t, 141), 123)` (32-bit fixed point), then
  `fma(t, C - A, A)`.
- **0x4A88(S, list)**: last entry first, each polygon's plane becomes a node
  (w0 of the second quad = 1 except for the last one); when the table is full
  sets the overflow flag and returns 0.

Exactness points found the hard way:

- List entries for new polygons are whole quads computed lane-wise: lane 0 is
  the address, the other lanes are the allocator quad's other words minus the
  size, and **the size's lanes come from the polygon header quad** (the SPU
  takes n with `rotqbyi 4` and the other header words ride along), so they are
  not a splat in the wrap case and for the 60A0 negative piece.
- The frames: 0x68D8 saves r80-r87, r0 and its back chain; 0x60A0/0x5A98
  store the back chain (68D8's r1 quad) at sp-0x50 and the three slot quads.
  All of it is written as the SPU writes it, so the LS compares whole.
- No caller reads r3 or the volatile registers after 0x68D8 (6B90 reloads).

## 4. The native function (`work/port/job220_native.c`)

Floats are host SSE at MXCSR = RZ|FZ|DAZ (0xFFC0). An operand with exponent
255 (checked on every quad loaded for arithmetic: planes, vertices, the
reciprocal estimate) or an overflow/invalid flag at the end abandons the run;
zeros that reach a store or a sign-sensitive op are made +0. The two unused
lanes of the reciprocal are set to 1.0 so they raise nothing. Stores go
through a copy-on-first-write journal; an abandoned run (float, a misaligned
pointer, n <= 0, a runaway walk) undoes them and the lifted function runs.

## 5. Validation

- `natdrv.c` (offline): 39 captured entry states (harness SNAP), 0 differences
  in the whole LS and r0/r1/r80-r127; FUZZ=40 (low bits of the vertices):
  1599 runs, 0 differences; UNIT=2000000 (the crossing arithmetic against the
  SPU helpers on random planes/vertices, specials included): 0 differences.
- `harness.c -DNATCMP` + `runall.py HARNESS=harness_nat`: every 0x68D8 call of
  the 293 jobs, 5035 calls, 0 differences, 2 abandoned (float).
- `mut.py`: 15 mutants (constants, dot order, side test, slot rule, fma vs
  fm+fa, the fixed point, a dropped +0, vertex order, lane-wise sizes, node
  flag, child link, push record, frame), all caught (two only by UNIT).
- In game, Rog, PS3RECOMP_NATIVE_J220=2: heavy_bench 173k calls (an earlier
  build) and move_bench 660k calls of the final code, **0 differences**;
  0.25% of calls abandoned (float) while moving, ~0.01% standing.

Speed: offline on captured states 4.6 us vs 14.2 us lifted per call (3.1x,
9800X3D). Compare mode times the native on a cold copy of the LS, so its
numbers (12-14 us vs 32-36 us lifted, Rog) understate it.

## 6. Left

- The other third: 0x55A0 (command emission, calls the chunk flush with DMA
  and the atomic), 0x7C40/0x6CB8 (transform, polygon setup), 0x4D80, 0x9DF8.
  0x7C40's subtree is pure LS and would be the next step (it contains
  0x6B90 → 0x68D8).
