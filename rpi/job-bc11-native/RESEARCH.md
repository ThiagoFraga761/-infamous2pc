# job bc11 native — research (2026-10-18)

Module `jobmod_bc11d5e7b134` (overlay/image 217), loaded at LS 0x4000..0x6D40
(0x2D40 bytes; .bss 0x6D60..0xE620). About 3.5% of all process samples in the
city; hottest lifted functions 0x6220, 0x5EE8, 0x66F8, 0x6650. Listing:
`work/runs/job217/jobmod_bc11d5e7b134_e2898.asm`. Captures:
`work/runs/job217/io/` (40,000 transfers, 5 SPU contexts).

**What it is, in game terms: the per-view scene walker that builds the
visible-object draw list.** One job per view of a frame (main camera, shadow
views, a reflection-style view with a height clip; see 5). It walks a tree of
command streams in guest memory (about 170 small GETs per job, 20 to 2,300
bytes each): group nodes with quantized bounding boxes, LOD switches, object
instances with their own 3x4 transform, and draw items. Every box is
transformed to world space and culled against an optional height plane, a
minimum size, a size-to-distance ratio and the frame's **visibility BSP** (four
frustum planes, then a tree of occluder planes); every LOD switch tests the
distance to the camera against one of seven radii. The visible draw items, each
a pair (batch EA, instance EA), are bucket-sorted on a one-byte key (248
buckets) and PUT as a 512-byte header plus the list, at addresses taken from two
atomic cursors in guest memory. That is the 170-GETs-per-PUT shape: a
hierarchical culling pass that gathers a little from many nodes and emits one
list.

The input layout and output buffer are shared with a second module,
`0x85EE00` (6,048 bytes, 140 descriptors in the same capture, PUTs from the
same LS addresses): a sibling walker, probably worth the same treatment next.

## 0. How this was established: an offline replay oracle

Same method as `rpi/ec5f-native/RESEARCH.md` and `rpi/job2025-native/RESEARCH.md`
section 0: the runtime's SPU interpreter linked into a harness with fake
channels and a fake job manager, GETs served from the captures, PUTs compared
with the captured ones. Files (my code, no game bytes) in
`work/runs/job217/oracle/`; build lines in the headers of `harness.c` and
`natdrv.c`, build into a scratch directory, never under `work/port`.

- `jobs.py` splits the capture into jobs (1.5) and writes `jobs.json`: 24 jobs
  of this module, 2 cut off by the end of the capture (both by GETs missing at
  the end; one of them still finishes with no output).
- `runall.py` replays every job three times: run A with the stale LS of 1.6
  preloaded (whole-transfer compare), runs B and C with LS filled with 0xAA and
  0x55 and nothing preloaded. A byte equal in B and C is one the job writes.
- **Result: 22 of 22 complete jobs run to the end; 42 of 42 captured PUTs are
  byte-identical on every byte the job writes** (35,515 of the 36,736 PUT
  bytes); 29 of 42 are identical as whole transfers too. The other 13 differ
  only in bytes the job never writes (1.6), which came from jobs of other
  modules that the capture cannot replay.
- The written-byte mask is also the evidence that nothing the job PUTs depends
  on LS it did not write itself (stack, .bss, the item scratch, the stream
  buffers' old contents): B and C start from different garbage and agree.
- `cov.py` sums the per-pc histograms (function table in 4), `natdrv.c` /
  `natall.py` / `mut.py` / `timeall.py` / `cmpops.py` test the native model
  (section 7).
- The module image is the 11,584-byte GET of EA 0x7D0A00 in the capture
  (identical in all four copies and to the listing's RAW column); keep it out
  of the repo.

## 1. Job interface

### 1.1 CRT (0x6898) and wrapper (0x5BC8)

- The CRT stores r3 (API table) at LS 0x6D40, clears .bss 0x6D60..0xE620 (about
  7,800 SPU steps, 3.7% of the job), runs the constructor list at 0x6D24 (one
  entry, 0x6888: constants at 0xE590..0xE610 the walker does not use), loads
  r3..r10 from the descriptor at `[API+0x20]` and branches to 0x5E18 -> **0x5BC8
  (wrapper)**. r4 != 0 at entry makes it `stopd` first. Returns the decrementer
  delta.
- **0x5BC8:** copies 16 bytes of `[API+0x20]` to its stack (never used), then
  - `API+52(0, i, 0, f)` for i = 1, 2, 3 (f = 1, 1, 0): buffer i. It keeps the
    LS (hi16 of w1, << 2) of all three and the DMA tags (lo16 of w0) of 1 and 2.
    **No buffer size is used.**
  - `API+60()` allocates the worker's tag; 0x6600 allocates a second one and
    keeps it at 0x6D10 (freed by 0x65B8 at the end); 0x6D50 = 1; `API+68()`
    (the manager's prefetch of the next job).
  - If `w(buf1+0x30)` (plane count) and `w(buf1+0x34)` (root stream length) are
    both nonzero, calls **the worker 0x4358** (section 3):
    `0x4358(hdr = buf1, planes = buf1+0x90, root = buf1+0x90+32*w(buf1+0x30),
    scratch = buf3, out = buf2, tag2, buf1, tag1, tag)`.
  - Frees the worker's tag (`API+64`); 0x65B8 frees the 0x6D10 tag and sets it
    to -1.

### 1.2 How the job finds its buffers (and what it assumes)

Everything comes from the three getbuf results; the descriptor is not read.

| from | LS | content |
|------|----|---------|
| buffer 1 | +0x0000 | header, 144 B (2.1) |
| | +0x0090 | visibility BSP, 32-byte nodes (2.2); count = header+0x30 |
| | +0x90 + 32*count | root command stream; length = header+0x34 |
| | **+0x4000, +0x8000** | the two 16 KB child-stream buffers. Fixed offsets in the code: the job assumes buffer 1's region is at least 48 KB. In the captures the root stream's tail (up to buf1+0x4860) lies inside the first of them; it is fully walked before the first child GET, so this is harmless |
| buffer 2 | +0x0E0 | MFC list elements of the list PUT (8 B per 16 KB) |
| | +0x180 | output header, 512 B (2.5) |
| | +0x380 | output list, up to 1,792 x 8 B = 14,336 B |
| buffer 3 | +16*i | item scratch, 16 B per draw item, up to 1,792 (28 KB) |

In the captures (derived from the manager's GETs and the PUT LSAs, not assumed):

- buffer 1 alternates between two 48 KB regions, A = 0xE800 and B = 0x1A800.
  The manager GETs the 32-byte DMA list to the region's last 0x20 bytes (0x1A7E0
  or 0x267E0) and gathers its three elements from the region's start: header
  0x90, BSP 0x3EC0, root stream 0xA0..0x910.
- buffer 2 = 0x26800 for region A, 0x2A400 for region B (the PUTs come from
  +0x180 and +0x380 of those).
- buffer 3 is never visible: its contents never leave LS. The harness uses
  0x2E000; the B/C test shows the outputs do not depend on its location or
  contents. **Its real location is the one layout fact still open**; the native
  version reads it from r6 and refuses overlaps (7.4), and compare mode logs
  the LS addresses of any job that differs.
- The descriptor (GET to 0xC00, 112 B) holds the manager's pairs
  `(size>>2 << 16 | flags, EA)`: the module (0x2D40 B at 0x7D0A00), the
  buffer-1 list (0x20 B), buffers 2 and 3 with size 0, then three more pairs
  the job does not read.

### 1.3 The job's own DMA (MFC_Cmd sites)

| pc | cmd | what |
|----|-----|------|
| 0x603C (in 0x5EE8) | GET | a child stream: `EA & ~15`, `align16((EA & 15) + size)`, into stream buffer k; the stream starts at buffer + (EA & 15) |
| 0x6A20 / 0x6A50 (0x69E0) | GETLLAR / PUTLLC | atomic add on a guest word (the two output cursors) |
| 0x4BDC, 0x5B84 | PUT | output header, 512 B from buf2+0x180, to the cursor-1 allocation |
| 0x65AC (0x6500) | PUTL | the list from buf2+0x380, `align128(8n)` bytes, 16 KB per element, to the cursor-2 allocation |

Every GET of every complete captured job is reproduced, in order.

### 1.4 Atomic cursors

`hdr+0x44` / `hdr+0x48`: EA of cursor 1 and the limit of its area;
`hdr+0x4C` / `hdr+0x50`: cursor 2 and its limit. Per batch (2.6) the job adds
512 to cursor 1; if the old value is 0 or old+512 > limit the batch is dropped
(no PUT). Otherwise it adds `align128(8n)` to cursor 2; if that old value is 0
or old+size > limit, the list EA is 0 and the header's bucket offsets are zeroed
(the header is still PUT). The harness seeds each cursor's line with the EA the
captured PUT went to.

### 1.5 Splitting the capture into jobs (jobs.py)

A job's descriptor is a 0xC00 GET whose pair 1 names module 0x7D0A00; its list
is the next 32-byte GET of the EA in pair 2, to 0x1A7E0 or 0x267E0. The window
runs from the list GET to the next list GET into the same region: the manager
prefetches job N+1 into the other region while N runs, and GETs are matched by
EA + LSA + size, which differ between regions. Jobs of other modules (0x85EE00
and friends) run in between and PUT from the same LS addresses; the oracle
takes the first PUT from buf2+0x180 in the window and the next one from
buf2+0x380 as this job's.

### 1.6 State carried between jobs

- The job PUTs bytes it never writes: byte 13 of the output header (buf2+0x18D)
  and the list's padding up to 128 bytes. They are whatever the previous job in
  that LS (any module) left. Run A of the oracle preloads them from the earlier
  captured PUTs of the same LS addresses, which is why 13 PUTs only match on
  written bytes: the bytes came from jobs whose PUTs are not in the capture.
- Item scratch bytes 1..7 are never written either, but never PUT.
- What this job leaves: the stream buffers hold the last two child streams;
  buffer 2 is zeroed from +0 to +0x17F at the worker's entry (0x4414), then +0
  accumulates the worker's elapsed decrementer ticks (a profiling-timer node
  labelled with buf2, 0x41C8 / 0x4C08), +0xE0 holds the list PUT's elements,
  +0x180 the last batch's header and +0x380 its list; buffer 3 holds the items;
  LS 0x80..0xFF holds the last atomic's cursor line (the GETLLAR buffer of
  0x69E0). Whether a following job of another module reads any of that is not
  known; the native version leaves all of it as the job does (7.1), except the
  timer word, which is an estimate.

## 2. Data layouts

### 2.1 Header (144 B, buffer 1 +0)

| off | content |
|-----|---------|
| +0x00 | camera position (x, y, z) |
| +0x10..+0x2F | 256-bit group mask (op 14): bit i of quad i>>7 is byte 15-((i&127)>>3), bit (i&7) |
| +0x30 | u32 BSP node count (0: the wrapper skips the worker, nothing is written) |
| +0x34 | u32 root stream length |
| +0x38 | u32 flags; bit 5 is read only by op 10 (never seen) |
| +0x3C | u32, copied to output header w0 (per-view constant: 5 distinct values in the capture) |
| +0x40 | u32, copied to output header w1 (a job counter: increases through the frame) |
| +0x44..+0x53 | cursor 1 EA, limit 1, cursor 2 EA, limit 2 (1.4) |
| +0x54..+0x6F | seven LOD radii, increasing, the last FLT_MAX; some views zero the first five |
| +0x70 | height scale for distances (z is multiplied by it before squaring) |
| +0x74 | minimum box size^2: smaller boxes are culled |
| +0x78 | size-to-distance ratio: cull when dist^2 * k > size^2 (0 = off) |
| +0x7C | clip height (with +0x81) |
| +0x80 | byte, copied to output header byte 12 |
| +0x81 | byte: nonzero = cull boxes below +0x7C and clip the rest to it |

### 2.2 Visibility BSP (buffer 1 +0x90)

32-byte nodes: plane `(nx, ny, nz, d)`, then u32 back, u32 front (offsets in
nodes from this node; 0 = leaf). The first four nodes, chained through their
front offset, are frustum planes tested first. Then a tree: box entirely in
front (min n.p > d) goes front, entirely behind (max n.p < d) goes back,
straddling pushes back and goes front. A null front is empty space: the box is
visible. A null back is solid: try the next pushed node; none left = hidden.

### 2.3 Command stream records (op = byte 0)

All fields big-endian, records 4-aligned.

| op | size | fields | action |
|----|------|--------|--------|
| 0 | u16 +2 | | skip (never seen) |
| 1 | 12 | +2 u16 skip, +4 qbox | box visible: continue inline; else skip the next `skip` bytes |
| 2 | 16 | +2 u16 size, +4 qbox, +12 EA | box visible: push child stream (EA, size) |
| 3 | 12 | +1 band, +2..+7 s16 center x4, +8 u16 skip | in LOD band: continue; else skip |
| 7 | 8 | +1 bucket, +2 u16 index | draw item: (ctx0 + 16*index, ctx1), key = bucket |
| 8, 9 | 8, 4 | | skip (table at 0x6BE0; never seen) |
| 11 | 72 | +1 cull flag, +2 u16 size, +4 EA, +8 ctx0, +12 ctx1, +16 3x3 + translation (row-vector 3x4), +64 qbox | instance: transform = local x current; if flag, cull its box first; push child with its own ctx |
| 12 | 20 | +1 band, +2 u16 size, +4 EA, +8 center | in band: push child (lod = min(lod, band)) |
| 13 | 28 | +1 band, +2 size, +4 EA near, +8 center, +20 EA far, +24 size far | LOD switch: near child (lod = min) if in band, else far child |
| 14 | 28 | as 13, +26 mask index | group bit clear: far child; else as 13 |
| 17 | 16 | +1 band, +2 u16 skip, +4 center | in band: continue; else skip |
| 18 | 8 | +1 a, +3 b, +4 EA | push child (EA, a*b+8) with lod 7 (EA 0: nothing) |
| 20 | 8 | | nothing |
| 4 | 16 | +1 band, +2..+7 s16 center x4, +8 u16 size, +12 EA | in band: push child (min lod); else nothing. Not in the captures |
| 5 | 24 | +1 band, +2..+7 s16 center x4, +8 size near, +12 EA near, +16 EA far, +20 size far | LOD switch as 13, int16 center. Not in the captures |
| 6 | 24 | as 5, +22 mask index | as 14 over op 5. Not in the captures |
| 10 | 24 | +2 size, +4 EA, +8 plane (n, d) in local space | plane to world by the adjugate of the current transform over its determinant (frest/fi), normalized (frsqest/fi); with header+0x38 bit 5 set, a camera behind it culls the child; else push (lod kept). Not in the captures |
| 15 | 12 | +8 EA | push (EA, 512) with lod 7 (EA 0: nothing). Not in the captures |
| 16 | 8 | +2 size, +4 EA | push with lod 7. Not in the captures |
| 19 | 24 | +2..+7 s16 center x4, +8 radius, +12 EA A, +16 EA B, +20 size A, +22 size B | distance as the LOD test, no band precondition: radius^2 > dist^2 pushes A, else B (lod kept). Not in the captures |

`qbox` (8 bytes, 0x66F8): byte 0 = center shift, bytes 1..3 signed center,
byte 4 = extent shift, bytes 5..7 unsigned extent; min/max = c -+ e, local.

LOD band test (0x5E20): passes if `lod <= band`; else the center through the
current transform, minus the camera, z scaled by header+0x70, squared length
against `radius[band]^2`.

Box cull (0x4030): box through the current transform (center and |M| extent,
0x6650); then the height clip (+0x81), the minimum size (+0x74), the
size-to-distance ratio (+0x78) and the BSP.

### 2.4 Queue and stream slots

A LIFO queue at .bss 0x6D80 (count, then 16-byte entries `{ctx0, ctx1, EA,
u16 size, u8 lod}` and a 64-byte transform per entry at +0x1810), at most 384
entries: a push beyond is dropped silently. Two slots (stack +0x150, +0x1B0)
hold a popped entry and its GET. The walk is not plain depth-first: when a
stream ends, the slot prefetched during it becomes current and the next entry
is popped into the slot just freed, before the new current stream pushes its
children.

### 2.5 Output header (512 B, buffer 2 +0x180)

`w0 = header+0x3C, w1 = header+0x40, w2 = list EA (0 if its allocation failed),
byte 12 = header+0x80, byte 13 = stale (1.6), bytes 14..15 = 0`, then 248 u16:
the end index of each bucket in the list (inclusive prefix sums after the
scatter).

### 2.6 List and batches

8 bytes per item: (ctx0 + 16*index, ctx1) of the op-7 record, in bucket order,
stable within a bucket. In the captures ctx0 and ctx1 look like a batch table
and an instance record (the op-11 instance's EAs). At most 1,792 items per batch:
the 1,793rd flushes the batch (sort, two allocations, two PUTs) and starts a new
one; the last batch is written when the walk ends. A job with no item writes
nothing.

## 3. The worker 0x4358, as pseudo-C

```c
void worker(hdr, planes, root, scratch, out, ...) {
    out.w0 = hdr[0x3C]; out.w1 = hdr[0x40]; out.b12 = hdr[0x80]; out.h14 = 0;
    C = identity; ctx = (0, 0); lod = 7; qn = 0; n = 0;
    slot[0].buf = buf1 + 0x4000; slot[1].buf = buf1 + 0x8000;
    r = root; end = root + hdr[0x34]; cur = 1; prefetched = 0;
    for (;;) {
        while (r < end) switch (op = r[0]) {                  /* 0x4710, table at 0x4744 */
        case 1:  r += cull(qbox(r+4), C) ? 12 : 12 + u16(r+2); break;
        case 2:  if (cull(qbox(r+4), C)) push(u16(r+2), u32(r+12), ctx, C, lod); r += 16; break;
        case 3:  r += lod_in(r[1], lod, s16x3(r+2)*4, C) ? 12 : 12 + u16(r+8); break;
        case 7:  if (n == 1792) flush(); item[n++] = (r[1], ctx0 + 16*u16(r+2), ctx1); r += 8; break;
        case 11: M = local3x4(r+16) * C;
                 if (!r[1] || cull(qbox(r+64), M)) push(u16(r+2), u32(r+4), (u32(r+8), u32(r+12)), M, lod);
                 r += 72; break;
        case 12: if (lod_in(r[1], lod, f3(r+8), C)) push(u16(r+2), u32(r+4), ctx, C, min(lod, r[1])); r += 20; break;
        case 14: if (!mask_bit(r[26])) { push(u16(r+24), u32(r+20), ctx, C, lod); r += 28; break; }
        case 13: if (lod_in(r[1], lod, f3(r+8), C)) push(u16(r+2), u32(r+4), ctx, C, min(lod, r[1]));
                 else                              push(u16(r+24), u32(r+20), ctx, C, lod);
                 r += 28; break;
        case 17: r += lod_in(r[1], lod, f3(r+4), C) ? 16 : 16 + u16(r+2); break;
        case 18: if (u32(r+4)) push(r[1]*r[3] + 8, u32(r+4), ctx, C, 7); r += 8; break;
        case 20: r += 8; break;
        /* 0, 8, 9: skips; 4, 5, 6, 10, 15, 16, 19: never seen */
        }
        if (!prefetched && !pop(cur)) break;                   /* 0x45D8: pop = GET into slot */
        prefetched = pop(1 - cur);
        wait(slot[cur]);  C = slot[cur].xf;  ctx = slot[cur].ctx;  lod = slot[cur].lod;
        r = slot[cur].buf + (slot[cur].ea & 15);  end = r + slot[cur].size;  cur = 1 - cur;
    }
    if (n) flush();          /* 0x4AC4: sort (0x6050), cursor 1, cursor 2, PUT header, PUTL list */
}
```

## 4. Function table (executed in the captures)

Steps are SPU instructions summed over 23 replays (4.83 M, about 210 k per job);
"exec/static" is distinct pcs executed over the function's size.

| addr | share | exec/static | role |
|------|------:|------:|------|
| 0x6220 | 36.9% | 183/184 | BSP box test (2.2): four frustum planes unrolled, then the tree walk with a stack in its frame |
| 0x4358 | 21.1% | 945/1564 | the worker: dispatcher, record decoding, op handlers, flush and end paths |
| 0x66F8 | 8.5% | 103/104 | qbox decode |
| 0x4030 | 7.6% | 102/102 | box cull: height clip, size, distance, then 0x6220 |
| 0x5EE8 | 6.9% | 90/90 | queue pop and child-stream GET |
| 0x6050 | 5.3% | 116/116 | bucket sort into the output header and list |
| 0x6650 | 4.7% | 41/42 | box through a transform |
| 0x4278 | 4.3% | 56/56 | queue push |
| 0x6898 | 3.7% | 72/82 | CRT (the .bss clear) |
| 0x5E20 | 0.7% | 50/50 | LOD band test |
| 0x6A70, 0x5BC8, 0x69E0, 0x6500, 0x41C8, 0x6600, 0x65B8 | 0.2% | | memset, wrapper, atomic add, list PUT, a profiling-timer list at 0x6D60, API+60/+64 stubs |

The lift's hot list (0x6220, 0x5EE8, 0x66F8, 0x6650) matches, with 0x5EE8 above
its instruction share: that is where the job's DMA and tag waits are.

## 5. Branch coverage of the captures

- Records dispatched: 12,261 (op 1: 3,949, op 7: 3,098, op 14: 2,988, op 2:
  1,181, op 11: 832 (both cull-flag values), op 13 code: 547, op 12: 159, op 18:
  40, op 20: 40, op 3: 13, op 17: 1). Never: ops 0, 4, 5, 6, 8, 9, 10, 15, 16,
  19 and anything above 20.
- Cull: the height clip and the distance ratio run in 4 jobs (the view with
  header+0x81 set), the minimum size in 6, the BSP in all.
- Output: one batch per job, 0 to 670 items. Never in the captures: a flush
  (more than 1,792 items), a dropped batch, a failed list allocation, a full
  queue, a deep BSP stack. The mutation tests (7.5) drive all of these but the
  last two.
- Views (by header+0x3C): five kinds in the capture; one has LOD radii 0 for
  bands 0..4 and a minimum size (shadow-like), one has the height clip and the
  distance ratio (reflection-like), three are full-detail.

## 6. Size and gain

- The captures execute about 2,070 distinct SPU instructions of the module's
  ~2,600 (the worker's 945 plus the helpers): record decoding, a queue, a bucket
  sort and a little SIMD float. The native version is one C file of about 760
  lines, section 7.
- Measured offline (natdrv, gcc -O2 -march=x86-64-v3, the port's SPU flags):
  **240..270 us for the 20 complete replayed jobs (clang 280..300), 0.06 ns
  per SPU instruction the job ran**; the biggest jobs (700 k SPU instructions)
  take 36..39 us, the smallest 1.5 us. On the
  SPU float helpers instead of host floats the same model takes 2,528 us
  (0.59 ns per SPU instruction): the helpers, not the decoding, were the cost,
  which is why the model uses host floats (7.2).
- The lifted SPU code has the same float helpers plus LS emulation, the MFC
  queue and the tag waits; I could not time it offline. If it runs at
  0.5..1 ns per SPU instruction (the helper-bound figure above is a lower
  bound for its float part), **the job drops from roughly 20..700 us to
  1.5..40 us, a 10..20x cut, i.e. most of its 3.5%**. Not measured in game:
  compare mode prints native and SPU microseconds per job (`[bc11] ...`).

## 7. Implemented: `work/port/jobbc11_native.c`

### 7.1 Shape

- Replaces the worker 0x4358 (the lift's `spu_native_entry` hook). The wrapper
  still does the manager calls; the worker's arguments give every LS address,
  so no layout is assumed.
- Builds privately: copies of the two stream buffers (32 KB) and buffer 2,
  its own queue and item arrays, batches kept until the walk ends. Child
  streams are read from guest memory (`vm_base`, `spu_ea_committed`) into the
  private stream buffers exactly as the GETs would (aligned, same size, same
  offset), and records are decoded with the job's own quadword/rotate
  semantics on LS addresses, so even an over-read sees what the SPU would.
- If the walk finished on a modelled path: the cursors with the same atomic
  adds in the same order, header PUT and list PUT per batch through
  `guest_put` (mirrors the MFC PUT: reservations lost, put hook), and LS left as
  the job leaves it (1.6: stream buffers, buffer 2 from +0 with an estimated
  timer word, the items in buffer 3, the cursor line at LS 0x80). Otherwise
  nothing has been written and the job runs on the SPU.
- Not reproduced, by design: the cursor atomics and PUTs happen after the whole
  walk instead of at each flush (observable only if a later child stream
  overlapped an earlier batch's output); the MFC tag mask the worker leaves in
  its channel (the wrapper does not read it); the timer word's exact value.
- `PS3RECOMP_NATIVE_BC11`: 0 (default) off, 1 on, 2 runs the model, then the
  SPU worker, and compares what the worker left in LS with the model: buffer 2
  from +4 (header minus the list EA, bucket offsets, the last list with its
  padding, the list PUT's elements), the item scratch and the stream buffers.
  It logs differences and timings every 10 s. It cannot see the PUTs
  themselves (their addresses come from the cursors) or earlier batches.

### 7.2 Floats

The job's arithmetic is SPU single precision (truncating, denormals as zero,
exponent 255 a number). Host SSE under round-toward-zero + FTZ + DAZ gives the
same bits for every value below 2^128. `run_model` sets MXCSR for the walk and
restores it; the walk falls back (`FB_FLOAT`) on any input word with exponent
255 (planes, header floats, record floats, LOD radii) and on any overflow or
invalid flag it raised. The routine overflow, `FLT_MAX^2` for the last LOD band,
is decided without the multiply. fp-contract is off for the file (a fused
multiply-add where the SPU had two roundings would change results). Built
without FMA, or with `BC11_HELPER_FLOAT`, it uses the SPU float helpers instead
(exact by construction, ~10x slower); both builds pass the same tests.

### 7.3 Fallbacks (nothing written, SPU runs)

Op 0 of size 0 (the SPU would loop) and ops above 20 (their skip size is read
past the 21-entry table at 0x6BE0, into constants and .bss; 21 would loop); a bucket key >= 248 (the job
would write past its counters into saved registers); a GET over 16 KB or
outside committed memory (the MFC would refuse it and the job would read stale
LS), or a 128-byte one that starts like Edge's viewport block (the MFC rewrites
those under resolution scaling); a cursor line outside committed memory; a BSP stack deeper than 60 or a looping tree; more
than 32 batches; exponent-255 inputs or float overflow; buffers whose used
ranges overlap each other, the module, the header, the BSP or the root stream
(`check_layout`, after the walk, on the ranges actually used).

### 7.4 Validation (offline)

- `natdrv.c` runs the model from the LS and registers run A had at the
  worker's entry (`harness TRACE=4358 SNAP=1`), on guest memory rebuilt from the
  window's GETs, with the same cursor seeds.
- `natall.py`, all captured jobs: **22 of 22 complete jobs (and the one cut off
  at the end that writes nothing) agree: 42 of 42 PUTs identical to the replay's
  as whole transfers, 42 of 42 identical to the capture on written bytes, and
  LS left as the job leaves it in all written regions.** The job cut off by
  missing GETs falls back (EA). Same with gcc, clang and the helper-float
  build.
- The same from LS filled with 0xAA and 0x55 instead of the earlier jobs'
  bytes (harness FILL, snapshot at the worker): 40 of 40 agree, PUTs and LS,
  so nothing depends on what the model assumes about old LS contents.
- A separate review of the file against the listing (fresh context) found no
  logic error in the walk; it found the LS-mirror gaps now fixed (the buffer-2
  memset and timer, the cursor line at LS 0x80), the missing check of the
  cursor lines before the host-side CAS, and the Edge viewport rewrite.
- `mut.py`: differential/mutation tests, model against the interpreter on
  mutated inputs (camera, LOD radii, height scale, size and distance
  thresholds, height clip, group mask, BSP planes, box bytes, LOD bands and
  centers, instance transforms, bucket keys, mask indices, cull flags, cursors
  at their limits, a synthetic stream of 1,876..2,008 items that forces a flush,
  exponent-255 words, denormals, radii and camera near 2^64). Results in 7.5.

### 7.5 Mutation results

Five campaigns (seeds 7, 11, 13: three mutations per kind per job; 17: two
per kind of the threshold, mask, plane, box, transform, float-edge and flush
kinds; 19: two per kind, every kind), over the 20 complete jobs:

| | runs | agree | fall back | SPU does not finish | **differ** | sensitive |
|---|---:|---:|---:|---:|---:|---:|
| seeds 7 + 11 | 2,264 | 2,040 | 103 | 121 | **0** | 815 |
| seeds 13 + 17 | 1,511 | 1,344 | 77 | 90 | **0** | 537 |
| seed 19 (after the review fixes, LS 0x80 and buf2 from +4 compared) | 754 | 665 | 45 | 44 | **0** | 283 |

- "agree" = same PUTs (EA and bytes, in order) and the same LS in the written
  regions. "sensitive" = the replay's PUTs differ from the unmutated replay's,
  so the mutation was seen. "SPU does not finish": the mutation sent the walk
  to child streams that are not in the capture (the harness serves zeros, op 0
  of size 0 loops); the model falls back there on uncommitted memory.
- Every kind is seen by the outputs except the LOD centers: in the captures
  most LOD records are op 14 with the group bit clear (far child, center
  unused), and a center that does flip a switch sends the walk to an uncaptured
  stream.
- Fallbacks are the exponent-255 words (expected), float overflow from the
  near-2^64 camera and radii, and EA fallbacks.
- One real bug was found this way and fixed: for a radius >= 2^64 the model
  decided the band without the distance, and the compiler then dropped the
  distance computation whose overflow flag was the guard (a camera near 2^64
  must fall back). The first campaign had 1 difference; none since.
- The skip ops never seen in the captures: an op-7 item turned into op 0 (size
  8), op 8, two op 9 or op 20, in two child streams of each job: 152 runs, all
  agree.
- The flush path (1,876..2,008 items, two batches: 220 runs) and the cursors at
  their limits (dropped batches, failed list allocations: 360 runs) all agree.

### 7.6 Integration (not done: tools and CMake are yours)

- `tools/lift_spu.py`, NATIVE_ENTRIES:
  `"jobmod_bc11d5e7b134_e2898": [0x4358],`
- `CMakeLists.txt`: add `"${CMAKE_CURRENT_SOURCE_DIR}/jobbc11_native.c"` to the
  SPU_SOURCES list next to `job2025_native.c` (it must be built like a lifted TU,
  `-march=x86-64-v3`, to get the host-float path).
- In game: `PS3RECOMP_NATIVE_BC11=2` first. The `[bc11]` line every 10 s gives
  native/SPU counts, fallback reasons (bits as in 7.3, 0x40 = LS layout, 0x20
  = EA), compared/differ and microseconds per job. A layout fallback prints the
  four LS addresses: that is how buffer 3's real place shows up.

## 8. Captures that would help

- Nothing is missing for the dominant path. A capture with the worker's r6
  (buffer 3) logged, or one SPU_PUT_DUMP that starts at a context's first job,
  would close 1.2's open point and give the descriptor of the first job of each
  context (its module is unknown: the capture starts inside it).
- A city area with heavier scenery or another view type, to see whether ops 4,
  5, 6, 10, 15, 16, 19 or a flush occur. If they do, they are the fallback set
  until modelled.
- The same capture for module 0x85EE00 (image number from the resident-overlay
  log), the sibling walker.

## 9. Follow-up: the other ops (2026-10-18, after the in-game run)

In game (06eef5b, mode 2, heavy city, camera turning): 408,351 jobs compared,
0 differ, native 10.9 us against 54 us on the SPU; mode 1 took heavy_bench from
62-66 to 69-82 fps. The only fallback was FB_OP, 29% of jobs: views whose
streams use ops the captures never had.

- Modelled from the listing: ops 4, 5, 6, 10, 15, 16, 19 (2.3). Op 10 is the
  only one with reciprocal estimates; it runs on the SPU float helpers
  (`op10_visible`, a literal transcription of 0x5570..0x57BC) under the caller's
  MXCSR, restoring the walk's float mode and flags after it. The others reuse
  the host-float LOD distance; op 19's radius gets the same exponent-255 check
  and 2^64 shortcut as the LOD radii.
- What still falls back with FB_OP is ops above 20 and nothing else.
  `PS3RECOMP_BC11_OPHIST=1` counts the op of every FB_OP fallback and prints
  `[bc11] FB_OP by op: opN:count ...` with the 10 s line. It costs nothing when
  unset beyond one store per fallback.
- Tests: none of these ops is in the captures, so `optest.py` rewrites real
  records into them in the child streams of every replayed job, keeping their
  children (op 2 -> op 4 / op 16 + op 20 / op 15 + op 9; op 13/14 -> op 5, 6,
  19, 10 + op 9; the x variants make the branch decisive by pairing a captured
  child with an empty one), and runs the model against the interpreter as
  mut.py does. Results, two campaigns (seed 5: 4 per op per job; seed 9: the x
  variants and op 4, 6 per job):

  | op | agree | sensitive | fall back | SPU does not finish | **differ** |
  |----|------:|------:|------:|------:|------:|
  | 4 | 185 | 59 | 1 | 14 | **0** |
  | 5 / 5x | 31 / 51 | 1 / 20 | 5 / 1 | 0 / 2 | **0** |
  | 6 / 6x | 32 / 54 | 1 / 18 | 3 / 0 | 1 / 0 | **0** |
  | 10 / 10x | 36 / 47 | 11 / 17 | 0 / 7 | 0 / 0 | **0** |
  | 15 | 65 | 38 | 3 | 8 | **0** |
  | 16 | 48 | 22 | 10 | 22 | **0** |
  | 19 / 19x | 33 / 47 | 0 / 19 | 3 / 4 | 0 / 3 | **0** |

  The fallbacks are all EA (a child stream not in the capture). Op 10 culls
  with header+0x38 bit 5 set and pushes otherwise, both seen.
- The captured jobs are unchanged: 42 of 42 PUTs and the LS still identical
  (gcc, clang, helper floats).
