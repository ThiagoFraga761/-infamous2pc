# Edge Geometry kernels, natively — research (2026-10-18)

Job module `jobmod_6d1c65171899` (image 209), loaded at LS 0x4000..0xF6B0, is
Edge Geometry: per mesh segment it decompresses vertexes and indexes, skins,
culls and compresses for the RSX. In the city profile it is the largest SPU
cost. This note covers the hot functions, what they compute, and the host
kernels in `work/port/edge_kernels_native.c` that replace them.

Words only, no game bytes: the module image, its listing and any LS dump stay
out of the repo (`work/runs/edge/` is untracked).

| LS | profile self | what | native | speedup (harness) |
|---|---|---|---|---|
| 0xD0A8 | 3.2% | skinning (+0x6E98 renormalise, 0.66%) | yes | 4.5-6.5x on 64..256 vertices |
| 0xA798 | 1.25% | triangle culling (+0xA3C8, 0.9%) | yes | 2.2-3.1x (whole call) |
| 0xA3C8 | 0.9% | transform for culling | yes | 3.5-4x |
| 0x4448 | 1.7% | vertex decompression, special formats | no (section 5) | |
| 0x9C20 | 0.65% | inner loop of vertex compression | no (section 6) | |

All numbers are best-of-N on the harness, lifted code built with the port's
SPU flags (`-O2 -march=x86-64-v4`), same machine, `nice -n 19`.

## 1. Method

### 1.1 The harness (`work/runs/edge/harness/`)

- `lifted.o` is the module's `spu_recomp.c` compiled with the port's SPU
  flags. `hrt.c` is a minimal runtime: function registry, `spu_drain_call`,
  `spu_indirect_branch`, and channel/stop stubs that flag a fault (only the
  error paths, which print through the mailbox at 0xE6C0, reach them).
  `hrt_call_lifted` calls a lifted function the way `brsl $r0` does (link
  0x3FFF0) and drains it.
- One test per kernel (`t_skin.c`, `t_xf.c`, `t_cull.c`): build a random but
  realistic LS (the module image at 0x4000, random bytes elsewhere, a job
  context with the fields the kernel reads, realistic matrices, positions,
  normals, triangles), run the lifted function on one copy and the native
  kernel on another, then compare **the whole LS and r0, r1, r80..r127**.
  Volatile registers are counted but not compared (section 1.4). The third
  argument is a float mutation rate in 1/1000: that share of the floats is
  replaced by 0, -0, denormals, FLT_MAX, exponent-255 values or random bits.
- `b_*.c` time one case (best of N runs); `t_replay.c` replays captured
  states (section 7.3); `mut.py` is the mutation check (section 7.2);
  `live.py` lists the volatile registers a caller reads after a call returns;
  `skin_v1.c` is the first, plain version of the skinning kernel (per vertex,
  through the u128 helpers) that pinned the semantics.
- Build: `./mk.sh skin xf cull replay`, `./mkb.sh skin`; `t_skin N 1 MUT SEED`.

### 1.2 The native-entry contract

`spu_native_entry` runs at the top of the lifted function, with the caller's
registers in `ctx->gpr`; a kernel that returns 1 has done the function's work
and the lift returns through `SPU_RET` (pc = r0). So a kernel must leave LS
and the registers as the function's epilogue would: these are compiled
functions with the standard ABI, so they save r80+ below the stack pointer,
store the link and the back chain, keep locals in their frame, and restore
r0 and r80+ from LS on the way out. The kernels write those stack slots too
(identical bytes), and reload r0/r80+ from LS at the end, as the epilogue does.

A kernel returns 0 **before writing anything** when the lifted function has
to run: the job's error paths (mailbox prints), a custom transform callback
(SPU code of the game's, section 4), unaligned or overlapping buffers (an
overlap guard checks every read and written span), sizes the job would loop
on forever (zero vertexes).

### 1.3 Floats

The lift computes every SPU float op through `runtime/spu/spu_helpers.h`.
The runtime defines `SPU_FP_DAZ`, and every entry into SPU code sets DAZ in
MXCSR (`spu_fp_enter`): the helpers then let the hardware read an exponent-0
operand as zero, as the SPU does, and test only for exponent 255. Tested over
1M random cases with denormals, signs and magnitudes mixed, **under that
caller MXCSR**: with MXCSR = round-toward-zero + flush-to-zero +
denormals-are-zero, a plain x86 `mul`/`fmadd`/`fmsub` gives the helper's
result exactly, after making exponent-0 results +0, as long as no operand has
exponent 255 and nothing overflows.

(The first version of the harness ran at the default MXCSR, where the helpers
take denormal operands at their value; the kernels followed that and differed
in game on the padding vertexes past the count, whose positions are garbage.
The harness now sets the SPU threads' MXCSR in `hrt_init`; section 7.4.)

So the fast paths run under that MXCSR and:

- check what each batch reads from the LS against a magnitude bound (the
  kernels' arithmetic is short and fixed, so a bound on inputs bounds every
  intermediate below FLT_MAX: skinning 2^18 (cofactors and renormalisation
  are the deepest: |s| < 3*(96 B^3)^2), the cull transform 2^60 plus a result
  check after the division by w, the cull 2^29 for valid triangles so the
  snapped coordinates stay int32);
- make outputs +0 where their exponent is 0. A zero's sign never reaches a
  nonzero value in these kernels (no division by a zero, compares treat both
  zeros alike), so intermediate zero signs do not matter;
- reproduce the integer estimate instructions (`frest`, `frsqest`, `fi`)
  bit for bit from the helpers' formulas; `fi(s, frsqest(s))` is folded into
  one step with two derived 64-entry tables;
- send a batch that fails the bound to an exact path: per vertex (or per
  group of four), the job's own lane layout, through the same helpers as the
  lift, at the caller's MXCSR.

The fast functions are `noinline` and MXCSR is written only outside them (the
compiler does not order float arithmetic against `ldmxcsr`; it cannot move it
out of a call).

### 1.4 Registers the callers see

Each kernel has one caller in the module (0x597C for 0xD0A8, 0x59EC for
0xA798, 0xAD40 for 0xA3C8). `live.py` from each return address: no volatile
register is read before it is written, except 0xA798's r3, the return value,
which the kernel sets exactly (the full quadword: a splat of the index count).
The callees met on the way (0xE200, 0x6E08, 0x64A8) take only r3/r4. So the
harness compares r0, r1, r3 where it is a result, and r80..r127.

## 2. 0xD0A8: skinning

Called once per segment, `r3` = the job context S (on the job's stack),
`r4` = matrix palette, `r5` = number of matrices, `r6` = per-vertex bone
indexes and weights.

### 2.1 Context fields it uses

- S+0 byte 0: flags. Bit 4: skip skinning entirely. Bit 7 must be set (the
  job's "skinning without an extra uniform table" error otherwise). Low
  nibble: which of the 16 uniform-table pointers at S+0x10 is the scratch.
- S+6 low nibble: skinning type. 1..3 are four-bone (no scaling, uniform,
  non-uniform), 4..6 the same with one bone. 0 or r5 = 0: return at once.
- S+7: matrix format. 0: 3x4 rows (48 bytes), 1: 4x4 rows (first three used),
  2: 4x4 columns, 3+: already in the job's layout.
- S+8: vertex count (u16). S+0x5C: positions. S+0x60: the "S60" stream;
  S+0x64..0x80: two lists of four stream pointers (normals, tangents...).
- S+0x58 (output): min(r4, r6), the first free byte.

### 2.2 What it does

1. Converts the palette in place to the job's layout: three quads per matrix
   holding the diagonal, the two off-diagonal rotations and the translation in
   lane 3 (q0 = (m00, m11, m22, t0), q1 = (m01, m12, m20, t1), q2 = (m02, m10,
   m21, t2)). With that layout a transform is three fmas on rotated copies of
   the vector (yzx, zxy), no transpose.
2. Builds the stream table on its stack: S60 first (when the type is not
   non-uniform), then each non-null pointer of the two lists, compacted. More
   than three: the job's "NYI" error.
3. Works in chunks: four vertices with the default scratch on its stack, or
   floor(n/3) rounded down to 4 in the extra uniform table (n = count rounded
   up to 4).
4. Per vertex: four-bone types blend `w_k * M[i_k]` row by row
   (fm, then fma for bones 1..3; weights are bytes, `cuflt` then `fm` by
   1/255; bytes alternate weight/index), one-bone types take the matrix as is.
   The matrix goes to the scratch with lane 3 replaced by 1, 0, 0; the
   position becomes `fma(zxy(p), C, fma(yzx(p), B, fma(p, A, T)))` in place.
5. Non-uniform types with S60 set transform S60 by the cofactor matrix
   (three `fms` of rotated products of A, B, C, then the same three-fma
   transform), the other streams by A, B, C without translation.
6. Types other than 1 and 4 renormalise every transformed stream (0x6E98):
   `v * (r, r, r, 1)` with `r = fi(s, frsqest(s))`, `s = z*z + (y*y + x*x)`,
   on the count rounded up to 8 (so up to 7 extra vectors are normalised).

The padding vertexes (count rounded up to 4) carry garbage indexes and
weights; they are transformed and written like the others. Their indexes can
point far past the palette (into the index stream and beyond), so the blend's
read span is checked against the written buffers only, not against the
palette conversion. A bone of weight 0 contributes nothing whatever matrix it
names (checked against the helpers: fm(+0, x) = +0 and fma(+0, x, a) =
fma(+0, 0, a) for every x), so the fast path gives it a zero matrix and only
weighted bones count for the palette range and its checks.

Everything is per vertex and in place; the job's loops are software pipelined
but read ahead only, so with the buffers disjoint (guarded) the native order is
free. The kernel fuses steps 4-5 per four vertices (matrices stay in
registers; they are still written to the scratch, chunk by chunk, so its final
contents match), and runs 6 as its own pass over 16 vectors at a time with one
estimate per 16. A zero vector, whose `s` is 0 and factor an exponent-255
value, gives (+0, +0, +0, w) on the SPU and does so in the fast path too.

## 3. 0xA3C8: transform for culling

`r3` = S. Positions (S+0x5C, rounded up to 4) go through the world matrix
(S+0xB0, 3x4) times the viewport info's 4x4 matrix (S+0xF0; the product is
taken in the prologue with the helpers), are divided by w (`frest`, `fi`, one
Newton step `fma(fnms(r, w, 1), r, r)`, and the sign of w xored back in, so it
is x/|w|), then scaled and offset (S+0x130, S+0x140). The scratch gets per
vertex (sx, sy, code, 0), code the outcode bits:
bit 7 sx > x1, 6 x0 > sx, 5 sy > y1, 4 y0 > sy, 3 sz > far, 2 near > sz,
1 w > 0 (x0, y0, x1 = x0 + w, y1 = y0 + h from the u16 scissor at S+0xE0,
near/far the smaller/larger of the two depth floats at S+0xE8). Compares are
SPU `fcgt` (both zeros equal). The job's loop is pipelined two deep; its fill
iterations store at offset 0 and are overwritten, except with 0 vertexes,
where the kernel falls back.

## 4. 0xA798: triangle culling

`r3` = S, `r4` = cull mode, `r5` = first vertex of the records.

- r4 = 0: S+0x58 = S+0x50 (unless S+0xC is -1), return the u16 at S+0x1A0.
- Otherwise: S+0x194 non-zero is a custom transform (the job calls game code:
  fall back); else 0xA3C8, then the triangles. A mode byte of 5 at S+0x150, or
  r4 outside 1..3, keeps none.
- Index count: u16 at S+0x1A0 (n/3 triangles). Records at the scratch +
  16*r5 + 16*index.
- Per triangle: valid = no vertex has bit 2 and all have bit 1; outside = the
  three codes share a bit of 7..2; area = (bx-ax)(cy-ay) - (by-ay)(cx-ax) (one
  `fms`), its sign flipped by the mode (r4 = 2: no flip) and by the index
  flavour (S+6 high nibble 0 or 2); the corners snapped to int
  (`cflts(fma(x, s, 0.5))`, s = 2 for mode bytes 3/4) cover no pixel centre
  when all three x or all three y are equal. Cull =
  (valid and covers none) or outside or (r4 = 2/3 and valid and front-facing).
  The last bit comes from an `fnms` on masks whose low bit is c | (a & b); the
  kernel takes the truth table from the helper at start and checks it.
- Survivors are compacted in place, four triangles per step: the step's 0..24
  kept bytes are stored as three quads at the output pointer, the first merged
  with what precedes the pointer in its quad, the next two shifted views
  (zeros past the kept bytes). A quad that would go past the rounded buffer
  end goes to the job's stack frame instead (sp' + 48 / + 64). The pipeline's
  two empty steps store zeros at the start first. The kernel reproduces those
  stores byte for byte; reads come from a copy of the original buffer (the job
  reads each group before any store can reach it).
- Then the u16 at S+0x1A0 = 3 * kept, S+0x58 = the end rounded to 16, r3 =
  3 * kept.

## 5. 0x4448: vertex decompression (not replaced)

The function patches the format's attribute table from the stream descriptor,
then decompresses through the generic attribute loop 0xAD48 (indirect jumps
per attribute format), or, for one format pair (primary: 2 attributes, ids 1
and 2; optional secondary: 1 attribute, id 3), through two inline loops
(0x4A30, 9 bytes per vertex; 0x4D18, 3 bytes per vertex). Those loops are the
self time. Established with `t_dec.c` (it drives the lifted code down that
path with `SHOW=1 B=.. PAT=..`):

- It allocates free uniform tables (S+0x170 slot bytes get ids 1, 2, 3),
  points S+0x5C (positions), S+0x60 (normals) and S+0x64 (tangents) at them,
  and sets S+0x58 to the input pointer.
- Position c = (u16 BE + int32 offset at X+4c) * 2^(b-16), b = byte at r4+0x1B,
  computed as csflt(arithmetic shift right by 16-b) + cuflt(shift left by
  16+b, scale 2^-32), lane 3 = 1.0.
- Normal and tangent: 24 bits, two 10-bit components dequantised through the
  mantissa of 1.0 and an fma to about [-0.707, 0.707], the third rebuilt as
  sqrt(1 - a^2 - b^2) with frsqest/fi, axis and sign selected by the low bits.
  The bit assignment is not pinned completely yet.

Not replaced in this round; the native kernel would follow the same pattern
(16 vertexes per AVX-512 step, exact path for odd floats).

## 6. 0x9C20: vertex compression inner loop (not replaced)

0x9C20 is a chunk of 0x9780 (reached through a jump table on the output
format: 0x9950, 0x99E8, 0x9D18, 0x9FD8). The case at 0x99E8 packs three
attributes (found by id in the format table) per vertex: `fma` by scale and
bias, `cflts` to 2^31 fixed point, per-lane shifts, and byte merges into an
unaligned output stream (48 bytes per 4 vertices, 40 per... with partial-quad
merges). Not done in this round.

## 7. Results

### 7.1 Differential tests

All of these report **0 differences** (whole LS, r0/r1/r80-r127, and r3
for the cull):

- skinning: 2000 cases x 4 runs (no mutation; 0.3% and 3% special floats;
  another seed), types 1..6, formats 0..3, 1..400 vertexes, 0..3 streams, S60
  on/off; ~15% of cases take the job's error paths and correctly fall back.
- cull transform: 1000 cases x 3 runs (0, 0.3%, 3% mutation).
- cull: 3000 + 2000 x 2 cases (0, 0.3%, 3%), modes 0..5, r4 0..6, r5 0..7,
  0..700 triangles, records from the native/lifted transform.
- replay of 50 dumped states through `t_replay` (checks the capture path).
- **real states from the game** (section 7.4): 400 skinning and 400 cull
  entry states captured in the city, plus the 8 states of the in-game
  compare differences: all native, 0 differences.

### 7.2 Mutation check

`mut.py` applies 44 single changes to the kernels (operand order, a lane, a
bit, a constant, a dropped store, the bound, the rounding of an estimate, the
pixel-snap offset, the slow paths, the zero-weight rule, DAZ in the fast
MXCSR, ...) and expects a test to fail. **All 44 are caught** (run at the
game's MXCSR). Survivors on the way were two equivalent mutants (a
commutative product; `cvtps` under the RZ MXCSR is `cvtt`), replaced, and
generator gaps that were closed: scissor origin always 0, depth bounds never
+-0 or denormal, no vectors small enough that s underflows to 0, and no -0
positions/translations (needed to see the +0 canonicalisation of positions).
The exact skin path is reached by the 3% mutation run.

### 7.3 Speed (best of 300 runs, hot LS)

- skinning, 256 vertexes: type 1 no streams 5.3 -> 0.78 us (6.7x); type 2,
  three streams 11.5 -> 1.9 us (6.0x); type 3, two streams and S60 13.0 -> 2.0
  us (6.6x); type 4 (one bone) one stream 3.0 -> 0.57 (5.2x). 4..12 vertexes:
  3.5-6.5x (fixed costs).
- cull transform: 8 vertexes 2.6x, 32+ 3.5-4.1x.
- cull, whole call (transform included): 150..600 triangles 2.2-3.1x;
  segments that keep nothing (mode 5) 3.5-4x.

In the randomised runs, which start from a cold LS copy, the totals are 4.5x
(skin), 3.6x (transform) and 2.8x (cull).

### 7.4 First in-game run, and what it found

Compare mode in the city: skinning fell back on every call (3.93M), the
cull differed on 143 of 3.98M calls (1-2 quads at the end of the transform's
records). Replaying the captures:

- Skinning: the overlap guard put the palette conversion and the blend's
  reads in one span; real meshes put the index stream right after the
  palette and the padding vertexes name matrices far past it. Fixed as in
  section 2. All 400 captured calls now run native: 4.3x on them.
- Cull: the 8 dumped states replayed identical offline, so the difference was
  in the environment: DAZ (section 1.3). With the harness at the game's
  MXCSR, the previous build differs on 8 of 8 dumps and the fixed one (DAZ in
  the fast paths' MXCSR) on none. Those quads are the records of the padding
  vertexes; they are real LS writes the job's next steps can read, so they had
  to be exact, not excluded. 2.6x on the 400 captured calls.

## 8. Integration

- `work/port/edge_kernels_native.c`; it needs `-march=x86-64-v4` (AVX-512
  F/BW/VL/DQ; it checks the CPU at start and turns itself off otherwise):
  add it to the SPU_SOURCES list in `work/port/CMakeLists.txt` next to
  `ec5f_native.c` and `job2025_native.c`, which gives it the lifted code's
  flags (the exact paths call the same helpers as the lift, so they must be
  built the same way).
- `work/tools/lift_spu.py`:
  `NATIVE_ENTRIES["jobmod_6d1c65171899_eA2A0"] = [0xD0A8, 0xA3C8, 0xA798]`.
- `PS3RECOMP_NATIVE_EDGE`: 1 (default) on, 0 off, 2 compare: the kernel runs
  on a copy of LS and registers, then the lifted function on the real ones,
  and the whole LS plus r0, r1, r80..r127 are compared. Every 10 s:
  `[edge] <kernel> (0x...): calls, native, lifted; compared, differ; native
  us, lifted us per compared call`. The first 8 differences are logged with
  their LS range; `PS3RECOMP_EDGE_DUMP=dir` also writes the input state of
  each (128 registers then the LS) for `t_replay`.
- `PS3RECOMP_EDGE_CAPTURE=dir[,count[,every]]` writes input states of real
  calls (default 32 per kernel, every 997th call) for `t_replay dir`.

## 9. Open

- 0x4448 and 0x9C20: same method; 0x4448 needs the special-format path
  decoded bit by bit (two loops of shifts and mantissa tricks).
- The cull fast path is bound by gathering 48 records per 16 triangles and by
  the in-place stores; an AoS formulation (area and snap tests on whole
  record quads) would cut the transposes.
- The exact paths are 2x slower than the lift; they only run for batches
  with exponent-255 or huge values.
