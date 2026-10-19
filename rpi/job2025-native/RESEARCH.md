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

## 8. Implemented: a cache in front of main, plus the native tail (2026-10-18)

### 8.0 Why a cache and not a port of the decoders

- The decode path is ~3,500 dense SIMD instructions:
  - 0x1B5F0, 0x1C510 and 0x1D4D0 are bit-field readers that build floats from
    sign/exponent/mantissa fields, decode smallest-three quaternions and use
    polynomial sin/cos;
  - 0x18F08 drives them per track category (header flags at clip+0x24:
    bit 1, bit 2, bit 3 = float channels, then 0x1CBB0).
  - A byte-exact port of that is a long project.
- Its result does not depend on the object: in a frame every instance of a
  rig has the same rig, clips and times. Only the root matrix
  (desc+0x40..0x7F) and the output EAs (desc+0x80..0x8B) differ.
- The capture has 19 distinct animation keys in 289 jobs. Run in capture
  order with a cache, 272 of 293 jobs take the native path.

### 8.1 Key (`j_make_key`)

All of these must match byte for byte:

- desc+0x00..0x3F and desc+0x8C..0x9F;
- the LS layout of section 1.3 (buffer 1/2/3 LS and sizes, list LS and size);
- buffer 1 (the rig) and buffer 2 (clip entries with their times, blend
  parameters);
- per clip entry, from guest memory:
  - the clip header (entry EA, size u16 at entry+4; its 4-byte format tag must match);
  - every segment of the segment table (count = lo16 at hdr+0x10, table at
    hdr + 0x38 + u32(hdr+0x38), entries {u32 size & 0x7FFF, u32 offset from
    the entry EA}). Hashing all segments makes the key independent of which
    segment the time selects (0x15494);
  - the frame block: EA = u32(hdr+0x2C) if nonzero, else entry EA +
    u32(hdr+0x34); size u32(hdr+0x28) (0x16044).
- Supported shapes: layers (desc+0x8C hi16) <= 1, entries (lo16) 1 or 2.
  Anything else (and any range that is not committed) runs on the SPU,
  unrecorded.
- Checked on the capture: every GET at 0x15270, 0x155DC and 0x16118 of
  every job lies inside the keyed ranges. The `key_*` differential tests
  patch the time, the frame block, a segment and the layer count: each
  patched job turns into a miss.

### 8.2 Miss: run the SPU job, record, validate (`j_record`)

1. Snapshot the LS window [buffer 3, buffer 3 + 0x8000).
2. Run the lifted main (as ec5f mode 2 does).
   - The native entry at 0x17280 (the float blend) snapshots the window and
     r0..r15 when the blend starts.
3. Read main's frame (r1 at entry - 0x2A0) once main has returned:

   | frame slot | content |
   |------------|---------|
   | +0x1D4 | N |
   | +0x214 | op-3 pose |
   | +0x218 | 3x4 / 4x4 buffer |
   | +0x21C | palette |
   | +0x90 / +0x94 | vector LS and size |
   | +0x48 | float channels (vector source) |
   | +0x40 / +0x44 | entry-0 pose buffer start / its last quad |

4. Refuse the entry if main took a path the tail does not model:

   | check | what it rules out |
   |-------|-------------------|
   | +0x1D0 != +0x1D4 | 0x5C58 path |
   | +0x188 != 0 | list PUT / 0x5CFC path |
   | +0x220/+0x224 not equal to +0x90/+0x94 | another vector path |

   It is also refused if any buffer leaves the window.
5. The entry holds:
   - the op-3 pose (N x 48 B);
   - per clip entry, its pose buffer (one buffer length apart). For two
     entries the values are taken at the blend's start; for one entry, at
     the end of main;
   - the blend's registers.
6. Validate: run the native tail (8.3) on the window as it was at job
   start, and compare its three PUTs with what the SPU job just wrote.
   - The entry is used only if all three are identical.
   - Otherwise it is kept as "invalid" and that key always runs on the SPU.

### 8.3 Hit: the tail of main (`j_tail`)

1. Make main's job-manager calls in main's order, from main's frame:
   API+68 (link 0x5078), then getbuf(0, i, 0, i == 3) for i = 1..3 (links
   0x50B8, 0x5240, 0x5320). The result is checked against the layout the
   key assumed, and a mismatch is logged.
2. Pose buffers. Of the float block [vector source, + vector size), entry i
   writes only the first `fw[i]` words, with
   fw = u32(hdr+0x30) + ((u32(hdr+0x24) >> 3) & 1):
   - the clear loop 0x158A0 writes hdr[0x30] words;
   - the float decoder writes one more.

   Those words come from the entry; the other float words keep whatever LS
   holds, as on the SPU. The last quad (the frame block GET) is always
   written. Any other word is written if the job changed it.
3. Two entries: run the lifted float blend 0x17280 with the recorded
   registers (link 0x147DC) on the live LS. It is a leaf, about 150
   instructions, and it carries the never-written lanes into the vector the
   way the SPU does.
4. Restore the op-3 pose. Copy the float channels to the vector buffer
   (0x1D8E8), and place the inverse bind table there from guest rig+0x24
   (the GET at 0x564C).
5. 0x17780, pose to 3x4, in groups of four bones; the next group is loaded
   before the current one is stored. Per bone (q, t, s), with
   sx2 = fm(sx, 2.0) etc.:
   - row 0 = (fm(fnms(qz,qz,fnms(qy,qy,0.5)),sx2), fm(fnms(qw,qz,fm(qx,qy)),sy2), fm(fma(qw,qy,fm(qx,qz)),sz2), tx)
   - row 1 = (fm(fma(qw,qz,fm(qx,qy)),sx2), fm(fnms(qz,qz,fnms(qx,qx,0.5)),sy2), fm(fnms(qw,qx,fm(qy,qz)),sz2), ty)
   - row 2 = (fm(fnms(qw,qy,fm(qx,qz)),sx2), fm(fma(qw,qx,fm(qy,qz)),sy2), fm(fnms(qy,qy,fnms(qx,qx,0.5)),sz2), tz)
6. The root loop 0x5814, N bones, with R = desc+0x40.. and w = (0,0,0,1):
   out_i[c] = fma(R3[i], w[c], fma(R2[i], X2[c], fma(R1[i], X1[c], fm(R0[i], X0[c])))).
7. 0x18B80, in place over the inverse bind table B:
   out_i[c] = fma(w[c], X_i[c], fma(B2[c], X_i[2], fma(B1[c], X_i[1], fm(B0[c], X_i[0])))).
   Then PUT the palette (desc+0x84, N x 48).
8. 0x4F60, in place, last bone first: 3x4 rows to a 4x4 whose rows are the
   columns (w = 0 for rows 0-2, 1.0 for row 3). Then PUT the vector
   (desc+0x88, if nonzero) and PUT the matrices (desc+0x80, N x 64).

### 8.4 Validation

- **Capture, in capture order, LS chained per SPU context**
  (`natdrv`, `j2025_core.h`):
  - 272 jobs native, 21 on the SPU, 19 entries recorded, none invalid;
  - **867/867 PUTs byte-exact, 810 of them native.**
  - The two jobs cut off by the end of the capture cannot be recorded.
- **Function-level differential** (`unittest`): 0x17780, 0x18B80 and 0x4F60
  on the interpreter against the native versions, on random inputs (unit and
  random quaternions, N = 1..33, -0, denormals, huge values):
  2000/2000 identical for each.
- **Job-level differential** (`difftest2.py`, native+cache against
  every-job-on-the-SPU on the same patched capture): 13/13.
  - Random roots, rotated roots, special floats in the root, random
    inverse bind tables.
  - Output EAs moved, vector EA 0.
  - Buffer 3 poisoned (0x7F) and the pose buffers poisoned (0xC1), which
    exercises the never-written float words.
  - Key changes (time, frame block, segment data, layer count) and a time
    change shared by several jobs.
- **Mutation checks** (`-DJMUT=n`):

  | mutant | caught by |
  |--------|-----------|
  | quaternion term sign | unittest (identity rotations only in the captures) |
  | root w | recording validation (all 19 entries invalid) |
  | palette without the w term | recording validation |
  | 4x4 w | recording validation |
  | fw+1 | capture and root differential |
  | fw-1 | validation under the poisoned LS |
  | mask quad not written | capture |
  | vector copy skipped | capture |
  | blend skipped | validation on 9 of 19 keys, the others need no blend |

  An operand swap in fm (commutative) passes, as it should.

### 8.5 What the coordinator must check in-game (not testable offline)

- **getbuf before/after API+68.** The hit path calls API+68 and then the
  three getbufs, as main does. It does not run the SPU job afterwards, so a
  layout mismatch can only be logged ("buffer i is ..."). Mode 2 shows
  whether that ever happens.
- **Lifted calls from native.** The blend (0x17280) and the API functions
  are called with the lift's own call patterns (direct for 0x17280,
  `spu_indirect_branch` for the API). Mode 2 compares every hit against the
  SPU job.
- **Concurrency.** Several SPUs can miss on the same key in the first
  frame they see it; each records, the duplicate is dropped. The cache
  holds 128 entries, oldest-first eviction, refcounted.
- **Expected gain.**
  - Per hit: the key (~0.7 to 2.6 KB of memcmp + hash), five guest calls
    (the API and the 150-instruction blend) and the tail (~25 vector ops per
    bone). That replaces about 9,700 SPU instructions.
  - A miss costs the SPU job plus about 0.1 MB of memcpy.
  - Measure with PS3RECOMP_NATIVE_J2025=2 (prints per-job native vs SPU
    time) and the 10 s stats line.

### 8.6 Erratum: LS layout in game (2026-10-18)

- In game, LS 0xC00..0xC4F (the manager header of section 1.3) is zero, so
  every job fell back with JF_LAYOUT. The real manager keeps its input DMA
  list in its own tables (0xEB0, indexed through 0xDF0 and the slot at
  0x12E0), in a different encoding, and getbuf (0x2EB0) also allocates DMA
  tags, so it cannot be called early.
- `j_layout_of` now uses only the module side:
  - buffer 1 is at the first 1 KB after .bss (0x1EC00). Its format tag is at
    +0x30 and its size is the u16 at rig+0x1C;
  - buffer 2 starts at the next 1 KB;
  - its 16-byte DMA list is the last 16 bytes of buffer 2's region, and the
    region size is the list's total rounded up to 1 KB;
  - buffer 3 starts right after the list.
- Checked on the game's LS dump (a 32-bone rig: layout found, then the job is
  refused as JF_DESC, 5 layers / 6 entries) and on the oracle (unchanged:
  867/867, differential tests pass).

### 8.7 Second in-game round: fallback reasons (2026-10-18)

- **What the live-LS dumps at the hook show:**
  - JF_LAYOUT and JF_DESC: characters, a 268-bone rig with 8 to 11 layers
    and 9 to 12 entries;
  - JF_DESC: a 32-bone rig with 5 layers and 6 entries;
  - JF_KEY: a 268-bone rig with 1 layer and 2 entries (rig 0x32C0 bytes,
    larger than the key buffer);
  - JF_CLIP: a 269-bone rig with 1 layer and 2 entries, whose second clip
    header is 0x440 bytes.
  - In all of them the times differ per instance and advance every frame,
    so a cache cannot hit.
- **The layout derivation of 8.6 was fragile.** It looked for the buffer-2
  list by trying region sizes, and on unknown shapes it could match data
  that only looked like a list.
- **Order of checks in `j_make_key` now:**
  1. descriptor pointer;
  2. shape (layers <= 1, entries 1..2), otherwise JF_DESC;
  3. rig tag and size;
  4. rig bones <= 64, otherwise JF_BIG (0x40, new);
  5. buffer 2 = 16 x entries (+16 with two entries), with its one-element
     DMA list exactly in the last 16 bytes of its 1 KB region, otherwise
     JF_LAYOUT.

  Clip headers may now be up to 0x1000 bytes.
- **`j_layout.shape`:** a hash of the time-independent inputs (descriptor
  minus root/EAs, layout, rig, clip header bytes).
  - The in-game glue counts recording failures and invalid entries per
    shape. After two, with no success, the shape skips recording and runs on
    the SPU directly (JF_SHAPE, 0x80).
  - `PS3RECOMP_J2025_INVDUMP=prefix` dumps the LS at entry (window) and at
    main's return for the first 8 failed or invalid recordings.
- **Cost of the old path:** JF_ENTRY jobs paid the recording (32 KB
  snapshot, frame read) and still ran on the SPU, every time. Big rigs no
  longer get that far. Invalid entries mean the native tail and the SPU
  differ on the same input, which is a model gap, not a key problem. The
  likely candidates are graph programs with opcodes other than 3, or tail
  paths the frame checks do not see. Dumps are needed to say which.

## 9. Kernels for character rigs (2026-10-18)

The cache of section 8 never hits characters (per-instance times), so the
hot leaf functions are replaced one by one instead: `work/port/
job2025_kernels_native.c`, off by default (`PS3RECOMP_NATIVE_ANIM=0/1/2`).
Each kernel leaves the local store and the registers a caller can see
exactly as the SPU function does, whatever the rig or the buffers.

### 9.1 How the kernels are made

Both kinds are generated from the module (game code: generated at build
time, `work/port/tools/gen_anim_kernels.sh`, never committed).

- `tools/kgen.py`, instruction by instruction (all eight functions):
  host registers instead of the context, float ops as one AVX-512 op with
  embedded truncation (DAZ must be on, as on every SPU thread), a lane at or
  above FLT_MAX (exponent-255 operand or overflow) recomputed by the
  helpers' exact fix-up, shufb with a known control as fixed permutes,
  frest/frsqest/cuflt/csflt/cflts as four-lane forms of the helpers, zeros
  made +0 where a non-arithmetic use can see the sign. About 1.2-1.6x the
  lifted code (clang); the lift already keeps registers in locals, so the
  gain is in the float ops, the shuffles and the scalar estimate helpers
  (their store-then-reload of four lanes stalls on store forwarding).
- `tools/kwide.py`, four groups per AVX-512 register: the loop is run
  symbolically on quads for a few groups, each group's stores are sliced out
  and their leaves made group-relative; when every group is the same graph
  (pipelined prologue/epilogue included), it is emitted for four groups at
  once, one per 128-bit lane, each node the same SPU instruction. The kernel
  checks before starting the layout the graph assumed (in-place arrays,
  alignment, written ranges disjoint from everything else read or written,
  stack included); otherwise the instruction-by-instruction kernel runs.
  Used for 0x16320, 0x16A30, 0x17780.
- Volatile registers: the wide kernels leave r2, r4-r79 untouched; no
  caller reads them after these calls (`tools/kabi.py`; 0x16320's caller
  then calls 0x17280, which takes r3-r11 only).

### 9.2 Results (Rog Ally, Zen 5, harness, machine shared with a running game)

| function | role | kernel | lifted | speed-up |
|---|---|---:|---:|---:|
| 0x16320 | pose blend (slerp), 268 bones | 2.9 us | 7.2 us | 2.5x (wide) |
| 0x16A30 | pose blend, modes 1-6, 268 bones | 4.8 us | 10.9 us | 2.3x (wide) |
| 0x17780 | pose to 3x4, 268 bones | 0.72 us | 2.2 us | 3.0x (wide) |
| 0x179C0 | anim-graph op 3, 256 entries | 15.4 us | 22.7 us | 1.5x |
| 0x1A570 | 48-bit rotation keys, 268 bones | 0.33 us | 0.51 us | 1.6x |
| 0x19A20 | key decoder, 256 | 7.8 us | 10.1 us | 1.3x |
| 0x1B5F0 | key decoder, 256 | 15.9 us | 20.9 us | 1.3x |
| 0x1C510 | key decoder, 256 | 7.8 us | 10.4 us | 1.3x |

Decoder and op-3 sizes come from captured states with the count raised and
what they read zero-filled (`ktest SCALE= ZFILL=1`): their loops have no
data-dependent branches, but the float fix-up paths run more on zeros.

### 9.3 Validation

- Replay of the 289 captured jobs with the kernels in place (`kharness`
  KERN=1): 867/867 PUTs byte-exact; KERN=2 (kernel on a copy, compared with
  the SPU code at the return): 4257 calls, 0 differences.
- `ktest`: captured states, synthetic 268-bone states (`mkslerp.py`,
  `mkdec.py`: random/equal/negated quaternions, weights or none, separate or
  in-place arrays, repeated indices) and fuzzing of everything the function
  reads (special floats, random bits, low-bit flips at scale): about 16k
  runs, 0 differences against the interpreter. Runs where fuzzed indices
  make the SPU code store into its own code are skipped (the interpreter
  then runs other code; the static lift would not).
- The fuzzer found one real bug during development (the exact fix-up kept
  -0 from x86), since fixed.
- Mutants (`oracle/mut.sh`): operand swap, wrong op, wrong lane step,
  dropped +0, permute byte, frest table entry, skipped fix-up, cflts
  saturation, dropped in-place check: all caught. Not caught: two of three
  `fm`->`fa` mutants in 0x1C510, whose results the synthetic headers always
  discard by selb: the decoders' data paths are covered only as far as the
  synthetic inputs go (no character capture exists).

### 9.4 What did not work, and what is left

- Group-parallel decoders: their loops carry a bit position that each group
  advances by widths decoded from the stream (popcounts), so group k needs
  groups 0..k-1. A two-phase kernel (positions first, sequentially; then
  the groups four at a time) fits: the recurrence is 4 of 165 nodes per
  group in 0x1C510, 4 of 95 in 0x1D4D0, ~20% in 0x19A20/0x1B5F0.
  `tools/kwide2.py` has the start of it (gathers, scatters, per-quad
  ownership tags with the SPU's visibility distances); unfinished, unused.
- Op 3 (0x179C0) four groups at a time: works in principle (gathers by bone
  index, last group from the epilogue code), but on real lists parents sit
  within the pipeline distance and the tag checks fall back nearly always.
  Its SPU loop reads a parent's record before an entry less than 4 groups
  back has stored it: whatever the data, a kernel must reproduce that.
- 0x17280 (the float-channel blend) is already hooked by job2025_native.c,
  and spu_native_entry serves the first registration only.
- The runtime has 16 native-entry slots (SPU_NATIVE_MAX) and the other
  native files use 8: this file registers 8 and fills the table. Raising
  SPU_NATIVE_MAX would allow 0x1D4D0/0x1CBB0/0x1A870/0x1AF00 (generated the
  same way, not measured).
