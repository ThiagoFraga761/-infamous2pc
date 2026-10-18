# job 2025 native — research (2026-10-18)

Module `jobmod_2025343b09d0` (overlay/image 201), loaded at LS 0x4000..0x1E9A0
(0x1A9A0 bytes, .bss to 0x1EAF0). About 12.6% of all SPU time in the heavy city
area. Listing: `work/runs/job201/jobmod_2025343b09d0_eFE70.asm`. Captures:
`work/runs/job201/io/` (5000 transfers, 5 SPU contexts).

**What it is, in game terms: skeletal animation for small rigs.** Per job, one
object instance: decode one or two compressed animation tracks at the current
time, blend them, turn the per-bone TRS pose into matrices, walk the parent
hierarchy, apply the object's root matrix, and write (a) the 3x4 skinning
palette (world x inverse bind, the vertex-shader constants), (b) one 4x4 matrix
per bone (for attachments / game side), and (c) a 16-byte vector. The captures
hold three rigs: 2 bones (17 instances), 3 bones (16 instances) and 10 bones
(2 instances, no animation layer: bind pose only), plus 9 jobs of a 32-bone rig
whose descriptors were not captured. Every instance keeps the same root
translation in all ~9 captured frames (~35 jobs per frame), and the 2/3-bone
rigs are rotations about one axis plus scale: my guess is animated city props
(fans, signs, flags and the like), not characters.

## 0. How this was established: an offline replay oracle

Same method as `rpi/ec5f-native/RESEARCH.md` section 0: the runtime's SPU
interpreter (`ps3recomp-infamous/runtime/spu/spu_interp.c`) linked into a small
harness with fake channels and a fake job manager, GETs served from the
captures (same EA + LSA + size, in order, inside the job's window), PUTs written
out and compared with the captured ones.

- **Result: 289 of 289 complete jobs replay to the end; 867 of 867 PUTs are
  byte-identical to the captured PUTs** (whole transfer, not only a used part).
  4 more jobs are cut off by the end of the capture.
- That needs the jobs of one SPU context to be run **in order, each starting
  from the LS the previous replay left** (section 1.6). From a zeroed LS it is
  669/867: every miss is the last word of the 16-byte vector output.
- Files (my code, no game bytes) in `work/runs/job201/oracle/`:
  - `harness.c` (build line in its header comment; build it outside the repo);
  - `jobs.py` splits the capture into jobs and writes `jobs.json` (section 1.5);
  - `runall.py` replays every job (contexts in parallel, jobs of a context in
    order, `LSOUT`/`LSIN` chaining; `CHAIN=0` turns it off) and prints the counts;
  - `patchrun.py job name "[(selector, offset, hex)]"` replays one job with
    patched capture bytes and lists which PUTs changed. This is the
    differential/mutation tool used for the probes below;
  - `funcs.txt` is the list of brsl targets, for `CALLS=1`.
  - The module image comes from
    `scratchpad/jobmods/jobmod_2025343b09d0_eFE70.bin` (game code: keep it out
    of the repo; it can also be cut from the RAW column of the listing).
- Mutation sensitivity: patching the root matrix, the layer count or the
  output EAs in a descriptor changes the replayed PUTs (section 2), so the
  byte comparison is meaningful.

## 1. Job interface

### 1.1 CRT (0x13E70)

- Stores r3 (API table) at LS 0x1E9A0, clears .bss 0x1E9C0..0x1EAF0, runs the
  constructor list at 0x1E984 (terminated by -1), loads the 8 first words of the
  descriptor at `[API+0x20]` into r3..r10 and calls **main 0x4FE0** (through the
  stub 0x5D28). Main ignores those registers and copies the descriptor itself.
- r4 != 0 at entry makes the CRT `stopd` first (debugger break). Pass 0.
- Then the destructor list at 0x1E990; returns the decrementer delta in r3.
- **Stack check**: functions test `r1` lane 1 (bytes of stack left) with `heqi`
  after the frame is allocated (e.g. 0x1410C). The manager must pass the stack
  size in lane 1; the harness uses 0x8000.
- 0x4BB8 is `assert(file, line)`: prints through 0x1DBB0 and `stop 0x3FFF`.

### 1.2 Job manager API (same table as ec5f)

| call | use here |
|------|----------|
| `API+68()` | called once at the start of main (0x5074). Lets the manager prefetch the next descriptor: the next job's 240-byte descriptor GET to LS 0xC00 shows up during this job. |
| `API+52(0, i, 0, f)` i = 1, 2, 3 | buffer i. Returns w0 = DMA tag, w1 = `(LS >> 2) << 16 \| (size >> 2)`. Main uses both halves; the size feeds the scratch allocator. f = 1 for buffer 3. |

### 1.3 LS layout (set by the manager)

| LS | content |
|----|---------|
| 0x0C00..0x0C4F | manager header of the descriptor copy. +0x20: input DMA pairs `(size>>2 << 16 \| flags, EA)`: module, buffer 1 (direct), buffer 2 (**a DMA list**), buffer 3 (size 0); the flag halfword tells direct from list |
| 0x0C50..0x0CEF | job descriptor, 160 B = `[API+0x20]` (section 2.1) |
| 0x4000..0x1E9A0 | module; .bss 0x1E9C0..0x1EAF0 |
| 0x1EC00 | buffer 1: the rig (section 2.2), 0x120..0x760 bytes |
| align1024(0x1EC00 + size1) | buffer 2: gathered from its DMA list, which sits in the last bytes of buffer 2's 1 KB region (0x1F3F0 or 0x1F7F0 in the captures) |
| end of buffer 2's region | buffer 3: scratch (0x1F400 or 0x1F800). A bump allocator (0x14088 init, 0x14450 alloc) hands out the output, pose and stream buffers from it and asserts on overflow (line 101 at 0x141AC) |

Buffer 1 is cached by the manager: a job whose rig EA equals the previous
job's gets no buffer-1 GET.

### 1.4 Main's own DMA (MFC_Cmd sites)

| pc | dir | what |
|----|-----|------|
| 0x15270 (in 0x15184) | GET | per layer: track header (0xA0 / 0xB0 B) from buffer-2 entry EA |
| 0x155DC (in 0x15504) | GET | per layer: 0x40 B block at header EA + 0xB0 (size from the header; 0 for the 10-bone rig) |
| 0x16118 (in 0x16074) | GET | per layer: 0x10 B at header EA + 0xA0 |
| 0x1564C | GET | inverse bind table, N x 48 B, from rig +0x24, into the palette buffer |
| 0x59CC | PUT | palette, N x 48 B, to desc+0x84 |
| 0x5A34 | PUT | 16 B vector, to desc+0x88, only if desc+0x88 != 0 |
| 0x5A8C | PUTL (0x24) | list PUT, only if a count at sp+0x180 lane 2 is set: **never in the captures** |
| 0x5AD4 | PUT | N x 64 B matrices, to desc+0x80 |

Every GET of every captured job is reproduced in this order (logs in the replay
output dirs).

### 1.5 Splitting the capture into jobs (jobs.py)

A job starts at the GET of its buffer-2 list (EA and size from its descriptor's
pair 3). Its descriptor is the latest 240-byte 0xC00 GET whose pair 1 is this
module (EA 0x82E780) and whose pair 3 names that list. Buffer 1 is the latest
0x1EC00 GET, checked against pair 2. The window runs to the next job start.

### 1.6 State carried between jobs

The 16-byte vector output is written with 12 bytes (xyz); its w is whatever the
previous job left at that LS address (0 or 1.0 in the captures). Any native
version that PUTs this vector itself must take w from the live LS, not write 0.
Nothing else depends on earlier jobs.

## 2. Data layouts

### 2.1 Descriptor (160 B, LS 0xC50, copied to the stack at 0x5038)

| off | content | evidence |
|-----|---------|----------|
| +0x00..+0x27 | 5 x `{u32, EA}`, same in every job | no effect when patched |
| +0x34, +0x3C, +0x9C | shared EAs, same in every job | no effect when patched |
| +0x40..+0x7F | **root matrix**, 4 rows (3x3 rotation/scale; row 3 = translation, 1.0) | rows 0-2 and the translation change both matrix outputs; w of row 3 does not |
| +0x80 | EA of the N x 4x4 output | |
| +0x84 | EA of the N x 3x4 palette | |
| +0x88 | EA of the 16 B vector, 0 = not written | patched to 0: that PUT disappears |
| +0x8C | u16 layers (1 = sample + blend, 0 = bind pose only), u16 buffer-2 entries (1 or 2) | (1, 2) in 266 jobs, (0, 1) in 24 (the 10-bone rig). Layers = 0 drops the track decoding (5,733 steps). Entries = 3 reads past the list and the replay never finishes |

### 2.2 Rig, buffer 1

| off | content |
|-----|---------|
| +0x00 | u16 bones N, u16 N (again) |
| +0x02.. | u16 offsets of the sections below (relative to buffer 1) |
| +0x24 | EA of the inverse bind table (N x 48 B, GET by main) |
| +0x30 | 4-byte format tag, u32 size of what follows |
| +0x40 | u16 N, u16 count, u32 offset, then section offsets |
| +0x60.. | bind pose per bone, 3 quads: rotation quaternion, translation, scale (identity in the 2- and 3-bone rigs) |
| after the pose | u16 parent indices (an all-ones-but-sign value marks the root); then u16 flags |
| +0x100 (2/3 bones), +0x290 (10 bones) | **anim-graph program**: byte opcodes, read by the dispatcher at 0x56A8 |

### 2.3 Buffer 2 (0x10 or 0x30 B)

- `{u32 EA, u16 size, u16 0, u64 0}` per layer entry (track headers, 0xA0/0xB0 B).
- With 2 entries, +0x20: blend parameters (two flag words, a float weight of 1.0, a zero word).
  Zeroing +0x20 or +0x24 makes the replay run away (no PUTs); changing the
  float at +0x28 or +0x2C does not change the outputs of the captured jobs
  (+0x2C changes the step count only).

### 2.4 Outputs

- Matrices (desc+0x80): per bone 64 B, the bone's world matrix in row-vector
  form: rows 0-2 rotation/scale (w = 0), row 3 translation with w = 1.0. The
  root matrix is folded in (in job 4 every row 3 is the root translation).
- Palette (desc+0x84): per bone 48 B, the same transform as a column-major 3x4
  (three rows `(m0i, m1i, m2i, t_i)`), i.e. the vertex-shader constant form.
  0x18B80 builds it in place over the table it fetched from rig+0x24 (inverse
  bind matrices, by their role; identity-like in the captured rigs).
- Vector (desc+0x88): 16 B, not changed by the root matrix. Values such as
  (1.0, 0, 0) and (0.43, 1.0, 0); meaning not pinned yet. Its w: see 1.6.
- The loop at 0x5814 multiplies a 48-byte-per-bone buffer by the four root rows
  (xyz lanes masked, row 3 with w forced to 1.0): fm, fma, fma, fma per lane.
  The exact composition order is to be pinned with the oracle when modelling.

## 3. Main 0x4FE0, as pseudo-C

```c
void job_main(void) {
    memcpy(&D, (void*)API[0x20], 160);                    /* 0x5038 */
    API_68();                                              /* prefetch next */
    rig  = getbuf(1);  b2 = getbuf(2);  scr = getbuf(3);   /* LS | size     */
    pool_init(&P, scr.ls, scr.size);                       /* 0x14088       */
    /* buffers for the palette, N 4x4, and the pose, sized from N           */
    anim_setup(&J, rig, b2, &D);                           /* 0x4568: per layer
           0x42A0 -> 0x14D78/0x14DF8: track header GET (0x15184), parse
           (0x152E4/0x15314), 0x40 B block GET (0x15504), key words (0x155E4) */
    for (l = 0; l < D.layers; l++)                         /* 0x4680 .. */
        for (e = 0; e < D.entries; e++) {
            frame_get(...);                                /* 0x16074: 16 B GET */
            decode(e);   /* 0x15838 -> 0x13FC0 copy, 0x18EC0/0x18F08 track loop:
                            0x1A570, 0x19A20, 0x1C510 + 0x1B5F0 (two keys),
                            0x1D4D0, then 0x15978/0x159A0                     */
        }
    if (D.layers) blend(...);       /* 0x145C8/0x14620 -> 0x16320 (quaternions)
                                       and 0x17280 (translation/scale), 0x14950 */
    copy(scr_vec, ..., 16);                                /* 0x1D8E8 */
    get(palette, rig.invbind_ea, N * 48);                  /* 0x564C */
    graph_init(&G, &J);                                    /* 0x6648 */
    for (pc = rig + prog_off; ; ) {                        /* 0x56A8 dispatcher */
        op = *pc;                                          /* table at 0x56CC */
        if (op > 17) for (;;);
        if (op == 0) break;
        pc = handler[op](pc, &J, sp + 0x20);   /* 3: 0x4200 -> 0x179C0 pose to
                                                  local matrices; others below */
    }
    model_from_local(M4, local, N, parents);               /* 0x17780 */
    for (b = 0; b < N; b++) X[b] = X[b] x root;            /* 0x5814, 48 B/bone */
    wait(tag);
    palette_build(palette, M4, palette, N);                /* 0x18B80 */
    put(palette, D.palette_ea, N * 48);                    /* 0x59CC */
    f_4F60(...);
    if (D.vec_ea) put(vec, D.vec_ea, 16);                  /* 0x5A34 */
    if (list_n)   putl(...);                               /* 0x5A8C, unused */
    put(M4, D.mat_ea, N * 64);                             /* 0x5AD4 */
}
```

Anim-graph opcodes (table at 0x56CC, handler at the listed stub): 0 end
(0x578C), 1 and 2 back to the dispatcher (0x56A8), 3 0x4200, 4 0x4160, 5 0xF0E0,
6 0xF328, 7 0xF758, 8 0x11330, 9 0x6CE0, 10 0x7220, 11 0xB2D0, 12 0x6810,
13 0xB258, 14 0xEFD0, 15 0xA420, 16 0xA828, 17 0xBE58. **Every captured
program is `op 3, op 0`.** The other 15 opcodes (blend trees, IK or similar:
about 60% of the module's code) never run.

## 4. Function table (executed in the captures)

Steps are SPU instructions per job, averaged over 293 replays (9,659 per job).
"exec/static" is distinct pcs executed over the function's size.

| addr | steps | exec/static | role |
|------|------:|------:|------|
| 0x1B5F0 | 1135 | 965/968 | 4-lane bit-field reader (rotqbi/shlqbi, cntb, clz): reads k-bit fields at per-lane bit offsets (r3 = offsets, r4 = mask). Called 4x per job. One loop (0x1B920) runs once in 266 jobs |
| 0x18F08 | 967 | 510/710 | per-layer track loop: walks the tracks, calls the decoders |
| 0x4FE0 | 932 | 650/852 | main |
| 0x1C510 | 649 | 1006/1008 | key decoder (dequantize, int->float, frest), 4x per job |
| 0x179C0 | 611 | 600/824 | op 3: TRS pose -> local matrices |
| 0x14468 | 473 | 63/63 | pool allocator core (0x14450 entry, 0x14564/0x14598 bookkeeping) |
| 0x14DF8 | 441 | 116/227 | track header parse |
| 0x140E8 | 408 | 186/218 | allocator init (0x14088 entry) |
| 0x16320 | 407 | 449/452 | quaternion blend of the two layers |
| 0x042A0 | 231 | 173/178 | layer setup |
| 0x155E4, 0x15314, 0x15504, 0x15184, 0x15278, 0x152E4 | ~700 | | track header / block GETs and parsing |
| 0x04680 | 195 | 78/334 | per-layer sampling driver |
| 0x15838, 0x15808 | 186 | | key fetch and copy (0x13FC0) |
| 0x1D4D0 | 158 | 261/262 | third decoder (once per layer) |
| 0x17780 | 158 | 143/144 | local -> model through the parent indices |
| 0x18B80 | 153 | 71/72 | palette: (model x root) x inverse bind, transposed to 3x4 |
| 0x17280 | 144 | 159/160 | translation/scale blend |
| 0x19A20, 0x1A570, 0x15978, 0x159A0 | ~260 | | decoder glue |
| 0x14620, 0x145C8 | 139 | | blend setup |
| 0x13EA8 | 129 | | CRT |
| 0x1DAC0 | 116 | | stack memset |
| others | <100 each | | |

Total: 57 functions, 6,952 distinct pcs executed out of ~27k.

## 5. Branch coverage of the captures

- Rigs: 2 bones (136 jobs), 3 bones (130), 10 bones (24). Step counts are fixed
  per rig and layer setup: 9485/9766 (2 bones), 10168/10449 (3 bones), 7368
  (10 bones).
- Layers: 1 layer with 2 entries (266 jobs), 0 layers with 1 entry (24 jobs:
  bind pose only, no track decoding). Never 2 layers.
- Graph program: always `op 3, op 0`.
- Never taken: the list PUT (0x5A8C), the 0x5CFC and 0x5C58 paths in main, the
  copy loop at 0x5718, 0x15D20 and 0x16124 (track-header variants).
- The 32-bone rig (9 jobs: buffer 1 0x760 B, PUTs 0x600/0x60/0x800) is in the
  capture but its descriptors were prefetched outside the captured window, so
  it cannot be replayed.

## 6. Native implementation: size and expected gain (estimate)

- **Dominant path to model**: descriptor + rig parse, track header parse, the
  three key decoders (0x1B5F0, 0x1C510, 0x1D4D0) and their glue, the two-layer
  blend (0x16320, 0x17280), op 3 (0x179C0), hierarchy (0x17780), root and
  palette (0x5814, 0x18B80), and the three PUTs. The allocator is not needed
  natively (fixed buffers) as long as the outputs are the same; only the
  16-byte vector's w must be read from the live LS (1.6).
- **Size**: about 3,500 SPU instructions of real work on that path (the
  decoders alone ~2,200, straight-line SIMD bit unpacking). In C with the
  runtime's u128 helpers, the ec5f ratio (≈400 lines for its two pass types)
  suggests **1,200 to 2,000 lines** for byte-exact output, most of it the
  decoders. The decoders are almost branch-free (one loop each), so they can be transcribed mechanically
  (instruction by instruction into spu_helpers calls) and then simplified, with
  the oracle checking every step.
- **Fallback** (run the SPU job instead): opcode other than 3 or 0 in the graph
  program, layers > 1, entries not in {1, 2}, a track-header variant other than
  the captured one (0x15D20/0x16124 paths), the list PUT count set, N above the
  model's limit, or any GET outside committed guest memory.
- **Gain**: the job is small, 9.7k SPU instructions on average, ~35 jobs per
  frame here. A native version does the same arithmetic without per-instruction
  register/LS emulation and without the 108 KB module reload that happens in
  about 1 job of 4 in the captures (the manager streams the overlay back after
  other modules ran). Expect **roughly 5-10x less time on this job**, i.e. most of
  its 12.6% share of SPU time, if the lifted SPU code costs a few host
  instructions per SPU instruction. This is not measured: check per-job time of
  image 201 in the profiler before and after.

## 7. Captures that would help

- A capture that starts earlier in each SPU context (or at least includes the
  0xC00 descriptor GETs of every job), so the 32-bone rig jobs (37 windows
  contain a job without a captured descriptor) can be replayed.
- A city area with characters or vehicles, to see whether the other graph
  opcodes, 2 layers or the list PUT are ever used. If they are, they are the
  fallback set until modelled.
