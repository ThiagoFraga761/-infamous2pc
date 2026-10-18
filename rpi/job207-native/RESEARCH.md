# job 207 native — research (2026-10-18)

Module `jobmod_5cfff962445a_e1390` (overlay/image 207), guest EA 0x85EE00,
loaded at LS 0x4000..0x57A0 (6,048 bytes; .bss 0x57C0..0x5850; CRT 0x5390,
API table at 0x57A0). Listing (from `tools/spu_disasm.py --base 0x4000` on the
module GET): `work/runs/job207/jobmod_5cfff962445a_e1390.asm`. Captures:
`work/runs/job207/io/` (SPU_PUT_DUMP=207,40000, heavy city, camera turning,
bc11 native on: 39,442 GETs, 558 PUTs, 5 SPU contexts).

**What it is, in game terms: the flat instance culler, bc11's sibling**
(`rpi/job-bc11-native/RESEARCH.md`). It takes a flat list of entries, each
naming three guest blocks: a set of boxes, each with an item range into an
item table; one 8-byte record per box (its LOD levels and a flag); and a 4x4
transform. Every box goes through the transform and is culled exactly as in
bc11: height clip, minimum size, size-to-distance ratio, the frame's visibility
BSP (the same code). For a visible box, each item in its range whose level
range admits the box's level goes out as a pair (item base + 16 x index,
instance word), keyed by one of four bucket bytes chosen by the header. The
output is bc11's: a 512-byte header with 248 bucket ends plus the bucket-sorted
8-byte list, at addresses from two atomic cursors. The flat shape suits sets
of instanced props, with the per-box record carrying each instance's level.

In this capture bc11 runs natively, so every captured job transfer is 207's
(the manager's own staging for other modules shows up too).

## 0. How this was established: an offline replay oracle

Same method as bc11 (its section 0). Files (my code, no game bytes) in
`work/runs/job207/oracle/`; build lines in the headers of `harness.c` and
`natdrv.c`, built into a scratch directory. Replay outputs are in
`work/runs/job207/replay/` (untracked, ~750 MB).

- `jobs.py` splits the capture into **998 jobs**: one per 32-byte list GET to
  LS 0xE7E0, with its three elements (header 0x50 B at 0x5C00, BSP 0x3EC0 B at
  0x5C50, entry list at 0x9B10). The window runs to the next such list in the
  context. 4 jobs are cut off by the end of the capture.
- `runall.py` replays each job three times (stale output LS preloaded from
  earlier PUTs of the same LS; LS filled with 0xAA; LS filled with 0x55).
- **Result: 994 of 994 complete jobs run to the end. Every PUT the replays
  issue, 412, is byte-identical to the captured one on every byte the job
  writes** (172,594 of 187,648 PUT bytes); 293 of 412 also as whole transfers.
  The rest of each transfer is stale LS, as in bc11.
- The other 116 captured PUTs (in 58 windows) belong to 207 jobs whose staging
  is not in the dump: their entry lists appear only in other contexts, staged
  while another module was resident. Those jobs cannot be replayed from this
  capture.
- The module image is the 6,048-byte GET of EA 0x85EE00 (keep it out of the
  repo).

## 1. Job interface

### 1.1 CRT (0x5390) and wrapper (0x4B48)

- The CRT stores the API table at 0x57A0, clears .bss 0x57C0..0x5850, runs the
  constructor at 0x5318 and calls the wrapper.
- **0x4B48:** `API+52(0, i, 0, f)` for i = 1, 2, 3 (f = 0, 1, 1): it keeps the
  LS of all three and the tags of 2 and 3; no size is used. `API+68()`. It then
  restores its frame and **tail-branches to the worker 0x4030** with
  `r3 = hdr = buf1, r4 = planes = buf1+0x50, r5 = list = align16(buf1+0x50+32*w(hdr+0x10)),
  r6 = buf3 (gather), r7 = tag3, r8 = buf2 (output), r9 = tag2`.
  The worker returns to the CRT.

### 1.2 How the job finds its buffers

| from | LS | content |
|------|----|---------|
| buffer 1 | +0 | header, 0x50 B (2.1) |
| | +0x50 | visibility BSP, 32-byte nodes; count = header+0x10 |
| | align16(+0x50 + 32 x count) | entry list, 28 B per entry; count = header+0x14 |
| buffer 2 | +0 | MFC list elements of the list PUT |
| | +0x80 | output header, 512 B |
| | +0x280 | output list, up to 14,336 B |
| buffer 3 | +0 | block 1 of the current entry: boxes and item table (GET, up to 16 KB) |
| | +0x4000 | block 2: one 8-byte record per box |
| | +0x8000 | block 3: the entry's 4x4 transform, 64 B |
| | +0x8040 | item scratch, 16 B per item, up to 1,792 |

In the captures: buffer 1 = 0x5C00 (region 0x5C00..0xE800, list at its last
0x20 bytes), buffer 2 = 0x17400 or 0x1B000 (from the PUT LSAs), buffer 3 =
0x1EC00 (from the GET LSAs). Blocks are not double-buffered: three GETs, a tag
wait, then the entry's boxes.

### 1.3 The job's DMA

| pc | what |
|----|------|
| 0x426C, 0x42C4, 0x4304 | the entry's three GETs (block 1: EA +16, `align16(u16 +26)` bytes; block 2: EA +20, `align16(8 x u16 +24)`; block 3: EA +8, 64 B) |
| 0x5518 / 0x5548 (0x54D8) | atomic add on a cursor (GETLLAR to LS 0x80), as bc11 |
| 0x45DC, 0x4AA4 | header PUT (512 B from buf2+0x80) |
| 0x5264 (0x51B8) | list PUT (MFC list, 16 KB elements at buf2+0) |

### 1.4 Cursors and batches

Header +0x28 / +0x2C: cursor 1 and its limit; +0x30 / +0x34: cursor 2 and its
limit. As in bc11, a batch is dropped if cursor 1 returns 0 or runs past its
limit, and its list EA is 0 (bucket ends zeroed) if cursor 2 fails. Two
differences:
- **Cursor 2 grows by 8x the list**: by `8 x align128(8n)` at the end, by
  0x1C000 at a flush, while the list PUT is `align128(8n)` and 14,336 bytes.
- The flush comes right after the 1,792nd item is stored (bc11: before the
  1,793rd).

## 2. Data layouts

### 2.1 Header (0x50 B)

| off | content |
|-----|---------|
| +0x00 | camera position |
| +0x10 | BSP node count |
| +0x14 | entry count (0: the worker writes the output header's fixed words and returns) |
| +0x1C, +0x20 | copied to output header w0, w1 |
| +0x24 | mode: which bucket byte (item +3+mode) and which level test (2.3) |
| +0x28..+0x37 | cursors and limits (1.4) |
| +0x38 | minimum box size^2 |
| +0x3C | size-to-distance ratio (0 = off) |
| +0x40 | height scale for distances |
| +0x44 | clip height (with +0x48) |
| +0x48 | byte: nonzero = height clip |
| +0x49 | byte, copied to output header byte 12 |

### 2.2 Entry (28 B) and blocks

- Entry: +4 instance word (item word 3), +8 transform EA, +12 item base (item
  word 2 = base + 16 x index), +16 block-1 EA, +20 block-2 EA, +24 u16 box
  count of block 2, +26 u16 block-1 bytes.
- Block 1: u32 box count n, u32 item total, n boxes of 28 B (min xyz, max xyz,
  +24 unused here, +26 s16 first item), then the item table.
- Item (16 B): +0 s16 index, +2 level range byte (low nibble = upper level,
  high nibble = lower level, both signed), +3..+6 bucket bytes (255 = skip),
  +9 flag byte (mode 3).
- Block 2, per box (8 B): +0 flag, +1 second level, +2 level.

Box i's items run from its first item to box i+1's first item; the last box
runs to the item total.

### 2.3 Level tests (per item, box level v = block-2 +2)

| mode | item emitted when |
|------|------|
| 2 | lower <= v <= upper |
| 3 | lower <= 6 and upper > 5, or the item's +9 flag is set |
| other | lower <= v <= upper, or (block-2 flag == 1 and lower <= second level <= upper) |

## 3. The worker 0x4030, as pseudo-C

```c
void worker(hdr, planes, list, g /*buf3*/, out /*buf2*/) {
    out[0x80] = hdr[0x1C]; out[0x84] = hdr[0x20]; out[0x8C] = hdr[0x49]; out[0x8E] = 0;
    if (!hdr.nent) return;
    for (e = list; e < list + 28 * hdr.nent; e += 28) {
        get(g, e.ea1, align16(e.size1)); get(g + 0x4000, e.ea2, align16(8 * e.n2));
        get(g + 0x8000, e.ea3, 64); wait();
        n = g.w0; total = g.w1; T = g + 8 + 28 * n;
        for (i = 0; i < n; i++) {
            B = g + 8 + 28 * i; P = g + 0x4000 + 8 * i;
            if (!cull(box(B) * M(g + 0x8000))) continue;   /* 0x5270, 0x445C..0x47D4, 0x4ED8 */
            end = i + 1 < n ? s16(B[54]) : total;
            for (k = s16(B[26]); k < end; k++) {
                I = T + 16 * k; b = I[3 + mode];
                if (b != 255 && level_ok(I, P, mode))         /* 2.3 */
                    item[cnt++] = (b, e.base + 16 * s16(I[0]), e.inst);
                if (cnt == 1792) flush();                        /* 0x49B8 */
            }
        }
    }
    if (cnt) flush_end();                                         /* 0x44BC */
}
```

## 4. Function table (executed in the captures)

Steps summed over 993 complete replays (55.3 M, about 56 k per job).

| addr | share | exec/static | role |
|------|------:|------:|------|
| 0x4ED8 | 53.1% | 183/184 | BSP box test (the same code as bc11 0x6220) |
| 0x4030 | 32.3% | 606/710 | the worker |
| 0x5270 | 12.0% | 41/42 | box through the transform (bc11 0x6650) |
| 0x4D08 | 2.1% | 116/116 | bucket sort (bc11 0x6050) |
| others | 0.6% | | wrapper, CRT, ctor 0x5318, atomic 0x54D8, list PUT 0x51B8, memset 0x5568 |

## 5. Branch coverage of the captures

- Modes: 2 (590 jobs), 0 (214), 3 (194). Height clip and distance ratio in
  194 jobs, minimum size in 195.
- Never executed in the worker: the flush (0x49B8..0x4AF4), the mode-3 level
  test (0x4AF8..0x4B38: the mode-3 jobs had no visible box with items), the
  failed list allocation at the end (0x4588, 0x4B3C). The mutation tests drive
  all of them (7.3).

## 6. Size and gain

- About 1,260 distinct SPU instructions executed; the native version is one
  C file of ~670 lines, most of it shared in shape with bc11.
- Offline (natdrv, gcc -O2 -march=x86-64-v3), on every 5th job: **4.0 us per
  job, 0.078 ns per SPU instruction the job ran** (198 jobs, 10.2 M SPU
  instructions). The same model on the SPU float helpers: 35 us per job. In
  game, bc11 measured 30 us native against 147 us on the SPU; this job's SPU
  time per job is not measured. Compare mode prints both.

## 7. Implemented: `work/port/job207_native.c`

### 7.1 Shape

bc11's design (its 7.1, 7.2): the worker at 0x4030 is replaced. Private copies
of buffer 3 up to the item scratch (0x8040 B) and of buffer 2; the three blocks
per entry read from guest memory exactly as the GETs would (same LS offset,
same size); host floats under round-toward-zero, FTZ and DAZ, with a fallback
on exponent-255 inputs and on overflow or invalid flags; cursors, PUTs and the
LS mirror (buffer 2, item scratch, buffer 3, the cursor line at LS 0x80) only
after a walk that finished on a modelled path.

`PS3RECOMP_NATIVE_J207`: 0 (default) off, 1 on, 2 runs the model, then the SPU
worker, and compares what the worker left in LS (buffer 2 with the list PUT's
elements at the SPU's list EA, the header minus the list EA, bucket ends when
that EA is set, the last list, the item scratch, buffer 3). The 10 s line
`[j207] native N, SPU N (fallbacks by reason: GET:n EA:n ...)` counts the
fallbacks by reason; in mode 2 it adds compared/differ and microseconds.

### 7.2 Fallbacks (nothing written, SPU runs)

| reason | when |
|--------|------|
| GET | a block GET the MFC would refuse (EA not 16-aligned, over 16 KB) or rewrite (Edge viewport signature) |
| EA | a block outside committed guest memory, or (when there is output) a cursor line |
| LS | buffers misaligned or past LS, or used ranges overlapping each other, the module, the header, the BSP nodes actually read or the entry list |
| BUCKET | a bucket byte 248..254 (the sort would write past its counters) |
| BSP | a BSP stack deeper than 60 or a looping tree |
| BATCH, MEM | more than 32 batches, allocation failure |
| FLOAT | an exponent-255 input word, or a float overflow or invalid flag |
| RANGE | a box, box record or item read outside the private buffer-3 copy (block contents that do not fit their GETs) |

### 7.3 Validation (offline)

- `natall.py`, every captured job: **996 of 996 jobs agree: 412 of 412 PUTs
  identical to the replay's (whole transfers) and to the capture on every
  written byte, LS left as the job leaves it in every written region.** The 2
  that fall back are cut off by the end of the capture (EA). Same with gcc,
  clang and the helper-float build.
- `mut.py`: the model against the interpreter on mutated inputs: camera,
  height scale, thresholds, height clip, mode (0..5), BSP planes, a BSP that
  passes every box, cursors at their limits, exponent-255 words, box floats,
  transforms, item ranges, level ranges, box records, bucket bytes, item
  indices, the mode-3 flag, and a synthetic entry list of 2..5 entries x
  700..950 items that forces one or two flushes (alone and with a cursor at
  its limit), every box visible under each mode with the level bytes stirred.
  Results (64 jobs: 48 with output, 16 without):

  | campaign | runs | agree | fall back | **differ** | sensitive |
  |----------|-----:|------:|------:|------:|------:|
  | seed 3: all 21 kinds, 2 per job | 2,688 | 2,570 | 118 | **0** | 742 |
  | seed 4: mode 3, flush at the limits, every box visible, 3 per job | 576 | 576 | 0 | **0** | 432 |

  The fallbacks are all FLOAT: the exponent-255 words. An earlier campaign
  (seed 1, 2,560 runs) also had 0 differences. The first run of the
  every-box-visible kind showed 23 differences that were the test's own: a
  patched block had copies of other sizes in the window that the patch did not
  reach, so the replay and the model saw different bytes; patching every GET
  that covers a patched byte removed them all (192 of 192 agree).
- With these kinds, every instruction of the worker runs at least once in the
  replays (pc histogram), including the flush with both cursor failures and
  the mode-3 flag path.
- From LS filled with 0xAA and 0x55 instead of the earlier jobs' bytes: 40 of
  40 agree (20 jobs with output).
- Mode 2's `compare()` run offline (natdrv `CMPLS`: the model, then compare
  against the replay's LS at the end): 0 on all 993 complete jobs (206 with
  output).
- A separate review against the listing (fresh context) found no error in the
  worker's decoding, level tests, flush or end paths. Fixed from it: compare()
  no longer checks the list elements when the last list PUT is ambiguous (more
  than one batch, or no list EA), the BSP nodes the walk actually reads are
  part of the overlap check, header and list PUTs to a misaligned EA are
  skipped as the MFC would, and the cursor lines are checked only when there
  is output. 448 more mutation runs on the fixed build (cursors, flushes,
  every box visible, mode 3, planes): all agree.
- Known limits: compare() can still report a difference when the single
  batch was dropped (cursor 1 exhausted) and buffer 2 held an older list EA;
  the LS 0x80 cursor line is copied as it is at publish time.

### 7.4 Integration (not done: tools and CMake are yours)

- `tools/lift_spu.py`, NATIVE_ENTRIES: `"jobmod_5cfff962445a_e1390": [0x4030],`
  The worker is entered by a tail branch from the wrapper (a trampoline in the
  lift); it returns through r0 to the CRT, which `SPU_RET` does.
- `CMakeLists.txt`: `"${CMAKE_CURRENT_SOURCE_DIR}/job207_native.c"` in
  SPU_SOURCES next to `jobbc11_native.c` (built like a lifted TU,
  `-march=x86-64-v3`).
- First run with `PS3RECOMP_NATIVE_J207=2`.

## 8. Mode 2 in game, and its compare codes (2026-10-18)

Runs in the heavy city with the camera turning:

| run | compared | differ | code 7 (blocks changed) | native / SPU per job |
|-----|---------:|------:|------:|------|
| 1 | 614,135 | 27 (all code 6) | (not separated yet) | 6.3 / 38.9 us |
| 2, code 7 on the last GET only | 660,170 | 41 (all code 6) | 5 | |

No fallbacks in either run. `compare()` returns the first mismatch it finds,
in this order:

| code | compares (SPU's LS after its run against the model) |
|------|------|
| 1 | buffer 2 +0..+0x7F: the list PUT's MFC elements (single batch with a list EA only) |
| 2 | output header w0, w1 and bytes 12..15 (w2 is the SPU's list EA, not comparable) |
| 3 | the 248 bucket ends, when the SPU's list EA is nonzero |
| 4 | the last batch's list with its padding |
| 5 | the item scratch (byte 0, words 2 and 3 of each item) |
| 6 | buffer 3 up to the scratch (0x8040 B): the bytes the entries' block GETs left there |
| 7 | as 6, but for every differing byte the latest GET that wrote it shows the SPU's copy equal to guest memory now: the block changed between the model's read and the SPU's GET |

**What the 41 code-6 jobs were.** The 16 dumps of run 2
(`work/runs/job207/ingame_dump`) contain 70 differing buffer-3 bytes. For all
70, the model's byte is what guest memory held when it read it and the SPU's
byte is what guest memory held after the SPU ran. The blocks changed between
the two reads. Most are flag and level bytes of the per-box records (block 2,
buffer 3 +0x4000), which another job or the PPU is updating during the frame.
They were missed by the first code-7 rule because they sit in an earlier
entry's GET: a later, smaller GET into the same slot leaves the tail of the
earlier one in place. Codes 1..5 were clean in every dump, so the outputs
matched. No model error was involved. In mode 1 the model and the job each
take their own snapshot of data that is being written, as two SPU runs would.

- Fix: code 7 now looks up, for each differing byte, the latest GET of the walk
  that wrote it (all GETs are recorded, up to 4,096 per job; beyond that the
  job stays code 6).
- Offline on the dumps (`dumpdrv.c`: the model on the dump's LS-before and
  the bytes it read, then compare() against the SPU's LS with guest memory as
  it was after the SPU): the model reproduces the dumped native buffers
  exactly in all 16, and all 16 give code 7. Run on the post-SPU bytes
  instead (`AFTER=1`), 15 give 0; the 16th is the run-2 code-7 job whose block
  changed again before the dump was written.
- Offline race test (`PATCHAFTER`: one byte of a random earlier block changed
  after the model's read, the replay seeing the new value): 56 code 7, 137 code 0
  (the byte was overwritten by a later GET), 4 codes 1/3/4 (the byte also feeds
  the outputs: a real input difference), no code 6.
- The model itself is unchanged by this. Rebuilt and rerun after the fix:
  996 of 996 captured jobs identical (gcc, clang, helper floats), compare() 0
  on all 993 complete jobs, and a mutation campaign over all 23 kinds (seed 6,
  one per job, 1,472 runs): 1,416 agree, 56 exponent-255 fallbacks, 0 differ.

`PS3RECOMP_J207_DUMP=dir` (with `PS3RECOMP_J207_DUMP_N`, default 16) saves
the first differing jobs (codes 1..7): the LS before and after the SPU, the
model's buffers, items and batches, and every block GET with the bytes the
model read and guest memory after the SPU ran.
