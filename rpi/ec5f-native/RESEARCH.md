# ec5f native — research (2026-10-18)

Module `ec5f11015784` (overlay 225), loaded at LS 0x4000..0x19AF0. Listing:
`work/runs/ec5f/ec5f.asm`. Captures: `work/runs/ec5f/io/`.

## 0. How this was established: an offline replay oracle

Most claims below were checked by **re-running the job offline** on the
captured DMA, not only by reading the listing.

- The runtime's own SPU interpreter (`ps3recomp-infamous/runtime/spu/spu_interp.c`,
  same `spu_helpers.h` float helpers as the lift) is linked into a small harness
  with fake channels and a fake job manager. GETs are served from the capture
  files (matched by EA + LSA + size, in order, per SPU context). PUTs are written
  out and compared with the captured PUTs. The atomic ring cursor (GETLLAR/PUTLLC
  on EA 0xA00A00) is seeded with the block EAs the capture shows, in order.
- Result: **52 of 52 complete jobs** in the capture (5 SPU contexts) run to the
  end. **44 of 44 command blocks are byte-identical to the captured PUTs** over
  their used part. **52 of 52 32-byte output records are identical.** The bytes
  that differ come after the JUMP or RETURN: leftovers in the LS buffer, which the
  RSX never reads.
- This is the acceptance oracle the port needs. A native ec5f can be diffed
  against it on every captured job, without the game.
- Harness files (my own code, no game bytes) are kept, untracked, in
  `work/runs/ec5f/oracle/` (first written in the session scratchpad:
  ):
  `harness.c`, `jobs.py` (splits the capture into jobs), `runjob.sh`, `runone.sh N dir`,
  `runall.py`, `cmp.py`, `fit2.py` (matrix checks), `sym2.py` (symbolic evaluator
  over an executed trace). `mod.bin` is cut from the RAW column of `ec5f.asm`
  (game code, keep it out of the repo). Copy the scripts into `work/tools/` if
  they are worth keeping. The scratchpad does not outlive the session.
  Harness env: `TRACE=pc,..` (log regs at pcs), `SNAP=1` (LS+GPR snapshot at
  them), `CALLS=1` (every function entry), `PROV=1` (which store pc wrote each
  quad of every PUT block), `RDRANGE=lo-hi` (which pcs read an LS range),
  `TRACEPCS=lo-hi:n` (executed pc sequence of the first n calls).

## 1. Job interface (inputs, outputs, kernel ABI)

### CRT and wrapper

- 0x18F90 is the CRT entry. The job manager passes its API table in r3 (stored at
  LS 0x19AF0; it was 0x1330 in the PM dumps). The CRT clears .bss 0x19B10..0x19C60,
  runs the constructor list at 0x19AD4, and calls **0x14050 -> 0x13D98**. (The
  `stop 0xF90`-looking words at 0x4010 are header data. 0x14F90 is the middle of
  a function, not the entry.)
- **0x13D98 (job wrapper):**
  - `API+52(0, i, 0, 1)` for i = 1, 2, 3 returns buffer i: tag = lo16(w0) and
    LS = hi16(w1) << 2.
    - buffer 1 = input (LS 0x1A000 or 0x1E400, alternating between jobs);
    - buffer 2 = output (0x22800 or 0x24C00);
    - buffer 3 = scratch (0x27000).
  - `API+60()` twice allocates two DMA tags, and `API+68()` is called once.
  - It then calls **main 0x12C50**(in, in_tag, tagA, tagB, scratch, scr_tag, out, out_tag).
  - After main returns, it **PUTs 32 bytes from the start of the output buffer
    (cmd 0x20)** to the EA in the job descriptor quad that `[API+0x20]` points to
    (in the captures: input#2 EA + 0x10). Then it frees the tags (`API+64`).
- Module-local wrappers: 0x18E60 allocates a tag (API+60, cached at 0x19AB0) and
  0x18E18 frees it (API+64).

### Inputs (staged by the job manager, contiguous from LS `in`)

| LS        | size  | capture example (job A) | content |
|-----------|-------|------|---------|
| in+0x000  | 0x380 | 00019 | frame/view block "IN" (below) |
| in+0x380  | 0x180 | 00022 | pass/material-class block "IN1" (below) |
| in+0x500  | 0x10  | 00024 | "IN2": (N, i, seq, 0). The output record goes to this EA + 0x10 |
| in+0x510  | 0x2400| 00027 | shared table, read only by type-4 path 0xF2C8 (0x12880); never read in captured jobs |
| in+0x2910 | 0xC00 | 00030 | shared table, read only by 0xA428 (0xA678); never read in captured jobs |

IN (896 B) fields used (offsets relative to `in`):

| off | use |
|-----|-----|
| +0x00 | EA of the render-queue table (512-byte records) |
| +0x04 | EA of that table's write cursor (4-byte GET to the stack; count = min((cursor-base)>>9, 100)) |
| +0x08 | EA of the command-ring allocation cursor (atomic add 4096 per block; EA 0xA00A04) |
| +0x0C | EA -> RSX offset delta (0xBFC00000, i.e. offset = EA - 0x40400000), used in JUMPs |
| +0x10 | ring limit EA (allocation clamped to limit-4096) |
| +0x2C, +0x30 | queue key: a queue record is taken when rec.w0 == IN[0x2C] && rec.w1 == IN[0x30] |
| +0x6C.. | constants copied into material constants (1.0, -0.0 seen) |
| +0x80..+0xBF | **VP: 4x4 view-projection, row-vector convention** (perspective in type-0 jobs, an affine light/decal projector in type-2 jobs) |
| +0xC0..+0xCB | camera-relative origin, subtracted from the object translation when IN1[0x178] != 0 |
| +0x340..+0x34B | per-axis dequantization fudge k (0x3F8001A3 = 1.00005 in all captures) |
| +0x151, +0x505 | flag bytes tested in the per-object filter (see 2.4) |

IN1 (384 B) fields used (offsets relative to in+0x380):

| off | use |
|-----|-----|
| +0x00 | bucket index b into the queue records' prefix-count arrays |
| +0x04 | **pass type 0..4**, a jump table at 0x13964: 0 -> 0xD6D8, 1 -> 0xCFA0, 2 -> 0xBC50, 3 -> 0xBAE0, 4 -> 0xF2C8 |
| +0x0A | byte: 0 = split every range among N jobs using IN2 (N, i) |
| +0xBC | material block size (rounded up to 16): 0xD0 or 0x200 in captures |
| +0xC0 | > 0: call 0x7C18 (per-object material constants) for every object |
| +0xD8 | vertex-layout table: u8 n; u8 attr[16] at +1; u8 stream[16] at +17; u8 byte_off[16] at +33 |
| +0x110 | u16 VP-constant slot per matrix kind, 0xFFFF = not emitted: kind 1 = MVP (4x4), kind 3 = rotation (3x4). Seen: (0x100, -) job A; (0x10A, 0x107) job 12; (0x103, 0x100) job 14 |
| +0x178 | byte: nonzero = camera-relative (subtract IN[0xC0]) |

### Outputs

- **4 KB command blocks**, built in two LS buffers at out+0x80 and out+0x1080,
  PUT (cmd 0x20, size 0x1000) to the EA the ring allocation returned.
- **32-byte record** at out+0, PUT by the wrapper:
  `{u32 first_block_ea; u32 bytes; u32 draws; 0; 0; u32 triangles; 0; 0}`.
  - `bytes` = bytes_in_finished_blocks + (cursor - block_base) - 4096, taken
    before the final format reset and RETURN.
  - `triangles` = the sum of rec+0x04 over the draws.
  - Checked on jobs 0, 1, 12, 14, 25 and 27. Job 0: (0x40436400, 0xB68, 19, 0, 0, 977), and the 19 draws hold 2931 indices = 977 x 3.

## 2. The main function 0x12C50, as pseudo-C

```c
void ec5f_main(IN, IN1, IN2, scr /*0x27000*/, out, tags) {
    /* 0x4328: select item ranges from the render-queue table */
    u32 cur;  get(&cur, IN.queue_cursor_ea, 4);              /* LS 0x3F070 */
    n = min((cur - IN.queue_table_ea) >> 9, 100);
    getl(scr+0x1800 /*0x28800*/, IN.queue_table_ea, n*512);  /* 0x18D60: list GET, <=16 KB elements */
    nr = 0;
    for (k = 0; k < n; k++) {
        Q *q = scr+0x1800 + 512*k;     /* {w0 key, w1 key2, w2 items_ea, w3 flags|prefix[-1]; u16 prefix[] at +0x10} */
        if (q->w0 != IN[0x2C] || q->w1 != IN[0x30]) continue;
        s = u16(q, 0x0E + 2*b);  e = u16(q, 0x10 + 2*b);     /* b = IN1[0] */
        if (IN1[0x0A] == 0) { c = ceil((e-s)/N); s = min(s + c*i, e); e = min(s + c, e); } /* IN2 = (N, i) */
        if (s == e) continue;
        range = { q->w2 + 8*s, q->w2 + 8*e };
        if (byte(q, 0x0C) != 0) ranges[front++] = range;     /* flagged records first, in table order */
        else                    ranges[--back]  = range;     /* the others after, in reverse order */
    }
    compact(ranges);                                         /* 0x19168 memmove */

    cb_init(&cb, &IN[8], out_tag, out+0x80, out+0x1080);    /* 0x14058, see 3 */
    mat_on = f_4030(IN1);                                    /* materials in use for this pass */
    state_reset(scr);                                        /* 0x19340 / 0x41D8: clear caches at scr+0x180.. */

    for (r = 0; r < nr; r++) {                               /* 0x13030 .. 0x13B78 */
        getl(scr+0x1800, ranges[r].begin & ~15, align16(end) - (begin & ~15));
        /* items: 8 bytes {u32 instance_ea; u32 header_ea}; software-pipelined:
           instance + header for item k+1 are already in flight while k is processed */
        for (k = 0; k < count; k++) {
            E *e = scr+0x200 + 16*(k%3);   /* instance entry: GET 16 B from item.instance_ea */
            H *h = scr+0x280 + 48*(k%3);   /* batch header:  GET 48 B from item.header_ea  */
            if (!object_filter(e, IN, IN2)) continue;        /* 0x13218..0x1332C, see 2.4 */
            if (mat_on && e->w3 != last_mat) {               /* material changed */
                get(scr+0xE80 + 0x300*(m++%3), e->w3, align16(IN1[0xBC]));
                last_mat = e->w3;  mat_changed = 1;
            }
            s = k & 1;
            get(scr+0x380 + 0x100*s, e->w1 & ~15, 0xB0);              /* drawable record */
            get(scr+0x580 +  48*s, h->xform_base(+0x14) + 48*e->u16[8/2], 48);  /* T: 3x4 */
            get(scr+0x5E0 +  64*s, h->sector_base(+0x1C) + 64*e->u16[10/2], 64);/* S: 4x4 */
            if (h->extra_n(+0x20))
                get(scr+0x660 + 0x400*s, h->extra_ea(+0x24), h->extra_n*64);
            wait(tag);
            if (mat_changed) { f_143F0(textures);  f_15E78(material consts); }  /* 0x13CD0 */
            if (mat_on && IN1[0xC0] > 0) f_7C18(...);        /* per-object consts c[0x104], c[0x1CA].. */
            float M[4][4] = object_matrix(rec, T, S, IN, IN1);  /* 0x136E0..0x13924 + 0x13CA8, see 4 */
            switch (IN1[4]) {                                /* jump table 0x13964 */
            case 0: f_D6D8(IN, &M, rec, e, ...); break;      /* lit geometry */
            case 1: f_CFA0(...); break;                      /* not in the captures */
            case 2: f_BC50(IN, &M, rec, e, ...); break;      /* shadow/decal-style pass (polygon offset) */
            case 3: f_BAE0(...); break;                      /* not in the captures */
            case 4: f_F2C8(...); break;                      /* not in the captures; consumes 2 items */
            }
        }
    }
    if (cb.first_block) {                                    /* 0x13B80.. */
        rec_out.bytes = f_14108(&cb);
        if (scr->vfmt_id(+0x180)) f_5B60(cb, 0);             /* reset vertex formats (0x1740 burst) */
        emit32(0x00020000);                                  /* RETURN */
        f_14388(&cb);                                        /* final PUT of the current block */
    }
    /* draws and triangles are counted into rec_out as the draws are emitted */
}
```

### 2.1 Per-object DMA (MFC_Cmd 0x40, from main)

Addresses are relative to scr = 0x27000. Each line gives the LS, size, source EA and the pc that issues the `wrch MFC_Cmd`.

- 0x200 + 16*(k%3), 16 B, item.instance_ea, pc 0x13ABC.
- 0x280 + 48*(k%3), 48 B, item.header_ea, pc 0x13B14.
- 0xE80 + 0x300*m, IN1[0xBC] bytes, e.w3, pc 0x13414. Only when the material changes.
- 0x380 + 0x100*s, 0xB0 B, e.w1 & ~15, pc 0x1347C.
- 0x580 + 48*s, 48 B, h[0x14] + 48*u16(e+8), pc 0x134D4.
- 0x5E0 + 64*s, 64 B, h[0x1C] + 64*u16(e+10), pc 0x1352C.
- 0x660 + 0x400*s, 64*h[0x20] B, h[0x24], pc 0x13D74. Only when h[0x20] != 0.

Every GET of every captured job is reproduced in this order (log of each replay).

### 2.2 Instance entry (16 B) and batch header (48 B)

| entry off | field |
|-----|-------|
| +0x00 | flags. Bits (w0 >> 5) & 7 are compared with IN2.i; other bits are tested against IN[0x34] (0xB1B0 in 0xB010) |
| +0x04 | drawable record EA. The low 4 bits are flags (2 in every capture); the GET uses `& ~15` |
| +0x08 | u16 transform index |
| +0x0A | u16 sector index |
| +0x0C | material block EA (change detection by value) |

| header off | field |
|-----|-------|
| +0x00 | 0x000A0000 (flags) |
| +0x14 | transform array base EA (48 B stride) |
| +0x18 | 1 |
| +0x1C | sector array base EA (64 B stride) |
| +0x20/+0x24 | extra data: count (in 64-byte units) and EA |

### 2.3 Drawable record (only the first 80 bytes are used by types 0 and 2)

The 176-byte GET is one record plus the beginning of the next one in memory.
Every draw in 52 jobs used the record at the slot base (0x27380 or 0x27480), never +0x50.

| off | field | evidence |
|-----|-------|----------|
| +0x04 | **u16 triangle count**: index count = 3 x this | 0x5F30 `rotqbyi 2` + `mpyui x,3`; checked on all draws of jobs 0, 12, 27 |
| +0x06 | u16 (vertex count?), unused by types 0 and 2 | |
| +0x08 | EA (mesh/material), not read by types 0 and 2 | |
| +0x0C | flags 0x0104_0500 / 0x0100_0300 | |
| +0x10..+0x27 | local bounding box (6 floats), not read by types 0 and 2 | |
| +0x28,+0x2C,+0x30 | **p: dequantization offset (xyz)** | object_matrix |
| +0x34 | **s: dequantization scale** (\|M row\| equals it) | object_matrix |
| +0x38 | vertex stream 0 offset (bit 31 = location) | 0x5E58 |
| +0x3C | vertex stream 1 offset | |
| +0x40 | vertex stream 2 offset (0xFFFFFFFF = none) | |
| +0x44 | **index offset**: bits 0..30 = offset, bit 31 = location | 0x5F3C |
| +0x48 | unknown count (not used by the draw) | |
| +0x4C | vertex-format id (cache at scr+0x180; 0x5B60 is called on change) | 0x5CAC |

### 2.4 Culling and LOD

- **No frustum or distance culling and no LOD choice happen in this job.**
  - Every fetched object produced exactly one draw: 659 records in, 659 draws out, over all 52 jobs.
  - The drawn record is always the one the instance entry points to.
  - Visibility and LOD are decided upstream: whoever fills the render queues writes the instance entries.
- The only per-object rejection is `object_filter`, at 0x13218..0x1332C. It compares the entry flag bits with IN2.i and tests flag bytes IN[0x151] and IN1[0x0A]. It never rejected an object in the captures. Its exact truth table is still open.

## 3. Command blocks: allocation, chaining and DMA

CB state (stack, 64 B; 0x3F150 in the replays):

| off | field |
|-----|-------|
| +0x00 | first_block_ea (0 until the first allocation) |
| +0x04 | end = block_base + 4092 |
| +0x08 | cursor (LS) |
| +0x0C | overflow callback = 0x14160 |
| +0x10 | ring cursor EA |
| +0x14 | EA->offset delta |
| +0x18 | ring limit |
| +0x1C | bytes in finished blocks |
| +0x20 | ? |
| +0x24 | current block EA |
| +0x28 | DMA tag |
| +0x2C | buffer index 0/1 |
| +0x30 | LS buffers [out+0x80, out+0x1080] |

- Every emitter checks `cursor + need > end` and then calls **0x18508(cb, need)**.
  - 0x18508 calls the function pointer at cb+0x0C, so it is the overflow callback, not a debug hook.
  - With no callback set, it returns 1.
  - The very first emission always overflows, because end = 0 at the start.
  - Example needs in 0x5C98: vertex offsets need 8*n; the index offset and the draw need 4*(ndraw_words + 8).
  - A byte-identical port needs the same need value per emitter, or blocks switch at different places.
- **0x14160 next_block:**
  ```c
  off = atomic_add(cb.ring_cursor_ea, 4096);         /* 0x190D8: GETLLAR/PUTLLC on LS 0x80 */
  if (off + 4096 > cb.limit) off = cb.limit - 4096;
  cb.done += 4096;
  if (!cb.first) cb.first = off;                      /* first block: nothing to PUT */
  else {
      if (off == cb.cur_ea + 4096) zero_fill(cursor, end);   /* contiguous: pad with NOPs */
      else *cursor = 0x20000000 | (off + cb.delta);          /* RSX JUMP to the new block */
      wait(1 << cb.tag);
      PUT(cb.buf[cb.idx], cb.cur_ea, 4096, cb.tag);          /* MFC_Cmd 0x20 at 0x14294 */
      cb.idx ^= 1;
  }
  cb.cur_ea = off; cb.cursor = cb.buf[cb.idx]; cb.end = cb.cursor + 4092;
  ```
  - Checked in job 1, which has 4 blocks: JUMPs 0x20093400, 0x2009E400 and 0x200A4400 sit at the cursor (block offset 4068 and so on), not at 4092.
- **0x14388 final flush**: PUT of the current block, cmd 0x20, size 4096, at pc 0x143DC.
- 0x14130(cb, p) converts an LS cursor into the block EA. After each draw, 0x5C98 stores the EA of the draw's first command into the caller's slot.

## 4. The object matrix and c[0x100] (task 2)

### 4.1 M, built in main

M is built at 0x136E0..0x13924 and stored at sp+0xB0..0xEF. 0x13CA8 then adjusts row 3.
The formula below comes from symbolic evaluation of the executed trace; `sym2.py` printed the expressions.

Notation:
- T = 48-byte transform with rows T0..T2 of 4 floats.
- S = 64-byte sector matrix with rows S0..S3.
- p = rec+0x28..0x30, s = rec+0x34, k = IN[0x340..0x348].

```c
for (c = 0; c < 3; c++)                    /* column c of T = "axis c" */
    A[c] = ((S0*T0[c] + S1*T1[c]) + S2*T2[c]);           /* fm, fma, fma, vector lanes = S row lanes */
A3 = (((S0*T0[3] + S1*T1[3]) + S2*T2[3]) + S3);          /* fm, fma, fma, fa */
M[c] = A[c] * (s * k[c])           for c = 0..2;          /* scalar s*k[c] is fm(splat(s), k) first */
M[3] = A3 + ((A[0]*p.x + A[1]*p.y) + A[2]*p.z);           /* fm, fma, fma, then fa */
M[3].w = 1.0f;                                            /* selb */
if (IN1[0x178]) M[3].xyz = M[3].xyz - IN[0xC0..0xC8];     /* fs at 0x13CAC, w kept at 1 */
```

In row-vector form: `v_world = (s*k ⊙ v_q + p, 1) · Tᵀ · S`, made camera-relative.
The record's +0x28/+0x34 are the position dequantization (offset and scale) of the vertex stream. That is the "small values" REQUEST suspected, here folded into M.

### 4.2 Emitted matrices (slots data-driven, IN1+0x110)

The emitted matrices take the w column of M rows 0..2 as 0 and of row 3 as 1.

- **MVP = M × VP**, with VP = IN[0x80..0xBF]. Lane-wise it is the same 4x4 multiply as 0x4D60:
  `R[i] = ((VP0*M[i][0] + VP1*M[i][1]) + VP2*M[i][2]) (+ VP3 for i = 3)`, with fm then fma.
  - Type 0: emitted by 0xBDD8 at 0xC30C..0xC318, via VP-constant upload method 0x1EFC, header count 17.
  - Type 2: emitted by 0xB010 at 0xB230..0xB324 + 0xB348.., same multiply. Row 3 w is forced to 1 before the multiply, so the last step is fma(VP3, 1, acc), equal to fa.
- **Rotation (normal) matrix N** (3x4, header count 13), emitted by 0xBDD8: `N[i] = M[i].xyz * r_i, w = 0`.
  - `r_i = rsq + 0.5*rsq*(1 - l*rsq*rsq)`, where `l = (x*x + y*y) + z*z` and `rsq = fi(frsqest(l))`.
  - That is one Newton step on frsqest.
- Numerical check: the M snapshot at 0x13928 (+ camera fix) is compared with the emitted constants of the same draw.

  | job | slot | content | n | max error |
  |-----|------|---------|---|-----------|
  | 0 (type 2) | c[0x100] | MVP | 19 | 9e-7 rel |
  | 12 | c[0x10A] | MVP | 26 | 2.8e-7 rel |
  | 12 | c[0x107] | N | 26 | 1.3e-7 |
  | 14 | c[0x103] | MVP | 48 | 2.8e-7 rel |
  | 14 | c[0x100] | N | 48 | 1.2e-7 |

  The float64 comparison is ULP-level. In the replay, which uses the SPU helpers, the blocks are bit-exact.
- 0xB010 and 0xBDD8 compare the new M with the previous one (fceq + gb, at 0xB120..0xB17C) and emit only when it changed. That is why some draws carry no matrix burst.
- In type-0 job 12, "c[0x100]" is not a matrix but a 2-vector material constant written by 0x15E78. So slot 0x100 can be the MVP, the normal matrix or a material parameter, depending on IN1+0x110 and the material.

## 5. Draw emission (0x5C98) and the index count (task 3)

```c
if (scr.vfmt_id != rec.u32[0x4C/4]) { f_5B60(cb, ...); scr.vfmt_id = rec[0x4C]; }  /* formats */
if (scr.streams[0..2] != rec[0x38..0x40]) {                    /* per-stream cache at scr+0x184 */
    L = IN1 + 0xD8;                                            /* vertex layout */
    for (j = 0; j < L.n; j++) {
        u32 base = rec[0x38 + 4*L.stream[j]];
        emit(0x00040000 | (0x1680 + 4*L.attr[j]),              /* VERTEX_DATA_ARRAY_OFFSET[attr] */
             (base & 0x80000000) + (base & 0x7FFFFFFF) + L.byte_off[j]);
    }
    scr.streams = rec[0x38..0x40];
}
n   = 3 * rec.u16[0x04/2];                                     /* indices */
nw  = (n + 255) >> 8;
emit(0x0008181C, rec[0x44] & 0x7FFFFFFF, 0x10 | (rec[0x44] >> 31)); /* INDEX_ARRAY_OFFSET, u16, location */
emit(0x00041808, 5);                                           /* BEGIN_END TRIANGLES */
emit(0x40001824 | (nw << 18));                                 /* DRAW_INDEX_ARRAY, non-incrementing */
for (i = 0; i < (n >> 8); i++) emit(0xFF000000 | (256*i));     /* full batches */
if (n & 255) emit((((n & 255) - 1) << 24) | (256*(n >> 8)));   /* remainder */
emit(0x00041808, 0);                                           /* BEGIN_END 0 */
*draw_slot = f_14130(cb, start_of_these_commands);
```

Examples:
- Job 12: 0x000D tris gives 26000000, 0x10 gives 2F000000, 0x38 gives A7000000.
- Job 27: 879 indices gives ff000000 ff000100 ff000200 6e000300.
- Job 12 layout table: n=4, attr {0, 10, 8, 3}, stream {0, 1, 1, 1}, off {0, 0, 4, 10}. That gives OFFSET[0] = rec+0x38 and OFFSET[10/8/3] = rec+0x3C + 0/4/0xA, which REQUEST had seen.

## 6. Per-object emitter order (from CALLS=1 replays)

- Type 0 (0xD6D8; jobs 8-14, 20-29, 39), per object:
  1. [on material change: 0x143F0 textures, 0x15E78 material constants]
  2. 0x7C18 (if IN1[0xC0] > 0): 0x5A38 / 0x5838 / 0x4158 / 0x5150
  3. 0xD6D8, which calls:
     - 0xBDD8: MVP and N
     - 0x6FE0: STENCIL_FUNC 0x0330 and BACK 0x0350 (+3)
     - 0x6AA0
     - 0x5C98: vertex arrays, index, draw
     - 0x14130
- Type 2 (0xBC50; jobs 0-4, 17, 42-58), per object:
  1. [material change: 0x143F0, 0x15E78]
  2. 0x7C18
  3. 0xBC50, which calls:
     - 0xB010: MVP, plus POLYGON_OFFSET 0x0A68 / 0x0A78 = (1.0, 2000.0) around each draw
     - 0x5C98
- Writer of each method (PROV=1 over jobs 0, 1, 8, 12, 13, 14, 17, 25, 27, 39, 43):
  - Textures: 0x1A00+0x20*i (+7), 0x1840+4*i, 0x0B00+4*i. Written by 0x143F0, copied from the material block (4 x 52-byte entries at material+0x08).
  - Material constants c[0x1CB..0x1D3], c[0x100/0x102] (type 0): written by 0x15E78, from the 512-byte material block and IN+0x6C.
  - c[0x104] (3x4, cyclic rows such as (-0.096, 0.476, 0.120)): written by 0x7C18 -> 0x5838. Its source is unknown (csflt, so integer inputs).
  - c[0x1CA]: written by 0x7C18 -> 0x5A38.
  - c[0x105], c[0x108], c[0x103]: written by 0xBDD8 in jobs whose slot table names them (more matrix kinds).
- Generic emitters: 0x55D8 (4x4, header 17), 0x5838 (3x4, header 13), 0x5A38 (1 vector, header 5). Each checks the room and calls 0x18508. 0x55D8 is used only by type 4 (0xF2C8).

## 7. Function map

Captured paths (exercised in the replays):

| addr | role |
|------|------|
| 0x18F90 | CRT entry: save API ptr at 0x19AF0, clear bss, ctors, call 0x14050 -> 0x13D98, dtors |
| 0x13D98 | job wrapper: query buffers/tags, call main, PUT the 32-byte record |
| 0x12C50 | main: queue selection, range loop, pipelined per-object DMA, M, type dispatch, finish |
| 0x4328 | read queue cursor, list-GET the queue table, build the item-range list (2.0) |
| 0x4030 | IN1 test: are materials used in this pass |
| 0x41D8, 0x19340, 0x4320 | state/cache setup and teardown (scr+0x180.. caches) |
| 0x14058 | init the CB state from IN[8..0x10] and the output buffers |
| 0x14160 | CB overflow: allocate the next 4 KB block (atomic), JUMP or NOP-pad, PUT the old block |
| 0x14388 | final PUT of the current block |
| 0x14108 | bytes written so far (for the record) |
| 0x14130 | LS cursor -> block EA |
| 0x18508 | call cb->overflow(cb, need) if set (73 call sites = every emitter's room check) |
| 0x190D8/0x190E4 | atomic add on an EA (GETLLAR 0xD0 / PUTLLC 0xB4 at LS 0x80), returns the old value |
| 0x4740 | atomic slot allocator (u16 counter/limit in a 128-B line, returns an EA of a 32-B slot); not hit in the captures |
| 0x18D60 | GET of any size via an MFC list (16 KB elements), cmd 0x44 |
| 0x19168 | memmove (range-list compaction) |
| 0x143F0 | texture state emission from the material block |
| 0x15E78 | material VP constants (c[0x1CB..0x1D3], c[0x100], c[0x102] in type 0) |
| 0x7C18 | per-object material constants (c[0x104] 3x4, c[0x1CA], ..) |
| 0x5838 / 0x5A38 / 0x55D8 | VP-constant burst emitters: 3x4 / 1 vec / 4x4 (slot, src) |
| 0x5C98 | vertex formats/offsets (cached), index offset, BEGIN, DRAW_INDEX_ARRAY, END |
| 0x5B60 | VERTEX_DATA_ARRAY_FORMAT burst for a format id (also the reset at the end) |
| 0xD6D8 | type-0 object handler |
| 0xBDD8 | type-0 matrices: MVP = M x VP, rotation N (rsqrt + Newton), other kinds by slot table |
| 0x6FE0 | per-object stencil function front/back |
| 0x6AA0 | type-0 helper with an atomic and a DMA at 0x6CC4 (no extra DMA in the captures) |
| 0xBC50 | type-2 object handler |
| 0xB010 | type-2 MVP (+ polygon offset state), change-detected |
| 0x4D60 | 4x4 multiply (fm + 3 fma per row), used by 0xF2C8 |
| 0x42C8 | small float helper (division via frest/fi + Newton), used by 0x5188 |
| 0x18540 | sin/cos (used by 0xF2C8) |

Not exercised in any capture (types 1, 3, 4 and the type-0 side branches):

| addr | role |
|------|------|
| 0xCFA0 | type-1 handler (calls 0x4EA0, 0x6AA0, 0x6FE0, 0x7160, 0x7A60, 0xA428, 0xBDD8) |
| 0xBAE0 | type-3 handler (0x7160, 0xB010) |
| 0xF2C8 | type-4 handler (3682 instr): instancing/animated, uses 0x4D60, 0x55D8, sin/cos 0x18540, 0x187E0, 0x186C0, 0x18AF0/0x18948, 0x18C58, 0xEDB8, 0x14960, 0x61E8, 0x69D8; reads the 9216-B table |
| 0xA428 | reads the 3072-B table (0xA678) |
| 0x7160, 0x61E8, 0x14130 | extra DMA users (0x725C, 0x77AC, 0x78EC, 0x79DC; 0x6654) |
| 0x15E78 side, 0x159B0, 0x152B0 | material-parameter table builders (0x152B0 is called from several handlers) |
| 0x4EA0, 0x5188, 0x5318, 0x5488, 0x186C0 | type-0/1 helpers called by 0xD6D8/0xCFA0 in branches the captures never took |
| 0x18EB0/0x18EF0/0x18F20/0x18F60 | bit-field float packing (probably float <-> half), constants at 0x19780..0x197D0. A guess from a quick read, not verified |
| 0x19430 | larger helper used by 0x6AA0 |

## 8. Open questions (superseded by 10.11)

1. object_filter (0x13218..0x1332C): the exact conditions. It never rejected anything here.
2. 0x7C18 / c[0x104]: the source of the cyclic 3x4 (a colour/hue matrix?) and of c[0x1CA].
3. 0x15E78: the full mapping from the material block to constants. Constant values come from material+0xD0.. and IN+0x6C.
4. Types 1, 3, 4 (0xCFA0, 0xBAE0, 0xF2C8) and the two big shared tables: no capture runs them. They need a capture from another area (`SPU_PUT_DUMP`) before porting. A native path should fall back to the SPU job when IN1[4] is not 0 or 2.
5. The meaning of the API+68 call and of IN2.w2 (0x40, 0x41: a sequence number?).
6. Matrix kinds 0 and 2 in the IN1+0x110 slot table (slots 0x105/0x108 seen): presumably the world matrix and the previous-frame MVP. Not yet fitted.
7. Bit-exactness: M, MVP and N must use spu_fm, spu_fma, spu_fa and spu_fs in the orders given in 4, plus frsqest/fi. Use the oracle to confirm.

## 9. Evidence index

- Replays: all 52 complete jobs (`jobs.py` split; contexts 8000, 8042, 8084, 80C6, 8108). Block compare: `cmp.py` gives 44/44.
- Job 0 (ctx 8000) inputs: 00019 / 00022 / 00024 / 00027 / 00030. Block: 00461. Record: 00465.
- Job 1 (ctx 8000): blocks 01269, 02328, 03486, 03935 (JUMP chaining). Record: 03940.
- Job 12 (ctx 8042), inputs 01849.. (IN1 = 01854): MVP @0x10A, N @0x107, vertex layout, 4 texture units.
- Job 14 (ctx 8042), IN1 = 03687: MVP @0x103, N @0x100.
- Job 27 (ctx 80C6): multi-batch DRAW_INDEX_ARRAY.
- Job 11 (ctx 8042): object/draw accounting (the extra 0xB0 GET is the item list, not a skipped object).
- Queue table sample: 00066 (14848 B = 29 records). Item list: 00071. Instance entries: 00075, 00083, 00107, 00125. Header: 00079.
- Code sites: 0x13964 (type table); 0x136E0..0x13924 and 0x13CA8 (M); 0xC30C (MVP, type 0); 0xB230..0xB4E4 (MVP, type 2); 0xC884 (N); 0x5F20..0x60F4 (draw); 0x5DD8..0x5EBC (vertex offsets); 0x14160..0x14380 (blocks); 0x13FA8..0x13FBC (32-byte PUT); 0x4468..0x46C4 (queue filter).

## 10. Implementation spec (types 0 and 2)

### 10.0 Status and how it was proven

- A native C model of pass types 0 and 2, written from this spec, is in
  `work/runs/ec5f/oracle/native.c` (my code, no game bytes; ~400 lines).
  - It reads the job's inputs from the capture files (matched by EA + size) and
    writes the command blocks and the 32-byte record.
  - **Result: 44/44 command blocks byte-identical to the captured PUTs (used
    part) and 52/52 records identical** (`ncmp.py`).
  - Mutation check: flipping the object filter, one lane of a constant, the
    END word or the range order makes it fail (28/44, 43/44, 0/44, 20/44), so
    the comparison is sensitive.
- **Differential tests on synthetic inputs** (`difftest.py`, `tests.py`). Each test
  copies a job's capture files into the scratchpad, patches some bytes, runs the
  SPU oracle and the native model on the same inputs, and compares their PUTs.
  **25/25 pass.** They cover paths the captures never take:
  - camera-relative off;
  - contiguous ring blocks and the ring-limit clamp;
  - IN2 split with N = 2 and 3;
  - selector filter (IN1[0x0A] = 1);
  - header bit 30 and bit 31 rejects;
  - polygon offset disabled by IN[0x153];
  - texture border colour;
  - op-11 identity fallback;
  - vertex-format ids 1 to 5 and 7;
  - stencil off and stencil ref changes;
  - zero-triangle records;
  - op-5 with cols/rows 3x2, -3x2 and 5x7 and frame bytes up to 0xFF;
  - matrix kind 0 (world) in a type-0 job;
  - vertex layouts with n = 0 and n = 2.
- The oracle `harness.c` got two fixes:
  - capture paths longer than 128 characters segfaulted;
  - a GET that is not captured exactly is now served from a capture that contains its range (`GET-SUB` in the log).
- Rebuild (paths as on this machine):
  - Build:
    ```sh
    gcc -O2 -I$RT/spu -I$RT -I$INC native.c -o native -lm
    ```
  - `RT=~/tools/ps3recomp-infamous/runtime`, `INC=~/tools/ps3recomp-infamous/include`.
  - The harness also needs `$RT/spu/spu_interp.c`, plus `mod.bin` cut from the RAW column of `ec5f.asm` and `funcs.txt`, the function list of section 7.

### 10.1 Conventions

- All LS/EA data is big-endian.
  - `R32(p)` reads a u32 and `S16(p)` an s16 sign-extended.
  - "Slot" values are s16. **Negative (0xFFFF) means "not emitted"** everywhere.
- Floats: use the runtime's `spu_helpers.h` u128 ops, never host float math.
  - The ops are `spu_fm`, `spu_fma(a,b,c)=a*b+c`, `spu_fnms(a,b,c)=c-a*b`, `spu_fa`, `spu_fs`, `spu_frest`, `spu_frsqest`, `spu_fi(x, est)`, `spu_csflt(v, 155)` (scale 0) and `spu_fceq`.
  - They implement the SPU rules: truncation, no denormals, saturation.
  - All change detection uses `fceq`: +0 == -0, every denormal == 0, and NaN is not special.
  - `splat(x)` means the u128 with x in all 4 lanes.
  - Vector ops are lane-wise, and only the stated lanes are used.
- `emit(w)` appends one big-endian word at the cursor.
- `room(n)` is the emitter's room check (10.4). Each check below is written next to the words it guards.

### 10.2 When the native path must fall back to the SPU job

Fall back if any of these holds. They are the branches no capture runs, and the model does not implement them:

- IN1[4] (type) is not 0 or 2.
- Type 0 only:
  - slot[k] >= 0 for k in {4, 5, 6, 7, 8, 9, 12, 13, 17, 43, 45} (D6D8 side branches; slot[k] = S16(IN1+0x110+2k));
  - IN1[0x09] != 0;
  - `(IN1[0xD4] ? 1 : IN1[0x17A]) != 0` (fragment-program upload, 10.7.9).
- Matrix kinds 2 and 10 (type 0) and kind 11 (types 0 and 2) with slot >= 0.
- Material constants (15E78): a descriptor type not in {0, 4}, or type 0 with bit 15 set (sRGB to linear, see 10.7.3).
- Per-object ops (7C18) when mat_on: an op not in {4, 5, 11}; op 4 with slot IN1+0xA6 >= 0; op 11 with slot IN1+0xB0 >= 0; op 5 with cols*rows == 0.
- Optional, but conservative: the ring returned an address whose clamp differs from the SPU's. This cannot happen if the same atomic is used.

### 10.3 Data layouts (new and corrected fields)

**IN (896 B)**, in addition to the table in section 1:

| off | use |
|---|---|
| +0x20 | EA of the fragment-program heap cursor (atomic add, 10.7.9). 0x00ABD2AC |
| +0x24 | EA->offset delta for SET_SHADER_PROGRAM (0xBFC00000) |
| +0x28 | fragment-program heap limit EA |
| +0x151 | byte: nonzero enables the "header bit 30" reject |
| +0x153 | byte: nonzero disables polygon offset in type 2 |
| +0x340 | k (per-axis fudge), used as `fm(splat(s), k)` |

IN+0x6C is **not** read by types 0 and 2 (section 1 was wrong).

**IN1 (384 B)**, all offsets relative to IN1:

| off | use |
|---|---|
| +0x00 | b: index into the queue records' u16 arrays |
| +0x04 | pass type |
| +0x09 | byte: nonzero means fallback (type 0) |
| +0x0A | byte: 0 = split every range among IN2.N jobs; nonzero = no split, filter objects by selector instead |
| +0x1C + 4i, i < [+0x54] | texture binding i: `{s16 unit; u16 tex}` |
| +0x54 | number of texture bindings |
| +0x58 + 4i, i < [+0x9C] | material constant descriptor i: `{s16 slot; u16 d}`. d bits 0..9 = word offset, 10..14 = type, 15 = sRGB flag |
| +0x9C | number of constant descriptors |
| +0xA0..+0xB1 | s16 slots for the per-object ops: +0xA6 op 4, +0xA8 op 5, +0xAE op 11 (3x4), +0xB0 op 11 second output. +0xA4/+0xAA/+0xAC are only tested by mat_on. +0xA0/+0xA2 are unused here |
| +0xB4 | ntex: number of 44-byte texture descriptors in the material |
| +0xB8 | nparam: number of float words after them |
| +0xBC | material GET size (rounded up to 16) |
| +0xC0 | nops; the op codes are at +0xC4 + 4i |
| +0xD4, +0x17A | initial fragment-program "upload" flag (10.7.9) |
| +0xD8 | vertex layout: `u8 n; u8 attr[16] @+1; u8 stream[16] @+17; u8 off[16] @+33` |
| +0x10F | byte: bits 5..7 nonzero = stencil func on; bit 2 = polygon offset allowed |
| +0x110 + 2k | slot[k] per matrix kind: 0 = M (4x4), 1 = MVP (4x4), 3 = N (3x4). Others: see 10.2 |
| +0x178 | byte: camera-relative M row 3 |

**Material block** (the GET of IN1[0xBC] bytes from E.w3):

| off | content |
|---|---|
| +0x00 | u32 key (op 11 matches it against the extra records) |
| +0x08 + 44t, t < ntex | texture descriptor t: +0 OFFSET, +4 FORMAT, +8 ADDRESS, +12 CONTROL0, +16 CONTROL1, +20 FILTER, +24 IMAGE_RECT, +28 BORDER_COLOR, +32 CONTROL3. +36..+43 unused |
| PRM = +0x08 + 44·ntex | nparam float words, read by 15E78 at PRM + 4·off |
| OPS = PRM + 4·nparam | per-object op data, read sequentially. Op 5: 8 B `{s32 cols, s32 rows}`. Op 11: 40 B `{u32 use_extra; f32 m[3][3]}` (rows). Op 4: 4 B |

Check: job 12 has ntex 8 and nparam 26, so OPS = 0x1D0 and the end is 0x200 = IN1[0xBC]. Job 0 has 3 and 4, so OPS = 0x9C and the end is 0xCC.

**Extra records** (H+0x24, H+0x20 records, 64 B each): `{u32 key; f32 m[3][3] @+4; zeros}`. Key 0xFFFFFFFF is the default record. Every captured key is 0xFFFFFFFF, and the matrices are circulant `(a b c / c a b / b c a)`, i.e. RGB hue/saturation colour matrices.

**Instance entry E (16 B)**:
- byte 0:
  - bit 3 (w0 bit 27) = reject;
  - bit 2 = polygon offset (type 2);
  - bits 5..7 = stencil ref (type 0).
- byte 1 = op-5 frame number.
- (w0 >> 5) & 7 = job selector.
- w1 & ~15 = drawable record EA; w3 = material EA.

**Header H (48 B)**:
- w0 bit 31 = reject; w0 bit 30 = reject when IN[0x151] != 0;
- +0x14 T base; +0x1C S base; +0x20 extra count; +0x24 extra EA.

**Queue record (512 B)**: a record whose byte +0x0C is nonzero is "deferred" (10.5).

**Scratch state** (scr = 0x27000, reset once per job, **not** per range):

| scr+ | init | content |
|---|---|---|
| 0x00..0x3F | 0 | previous M (4 rows), shared by BDD8 and B010 |
| 0x60 | (1,1,1,1) f | op-5 cache |
| 0x70 + 16u | 0 | texture cache per unit: {OFFSET, ADDRESS, CONTROL0, FILTER} |
| 0x170 + u | 0 | byte: CONTROL2 (0x0B00) already sent for unit u |
| 0x180 | 0 | current vertex-format id |
| 0x184..0x18C | 0x7FFFFFFF | current stream bases rec[0x38/0x3C/0x40] |
| 0x190 | 0xFFFFFFFF | current stencil ref |
| 0x19C | need_mat (byte) | need_mat = any(slot[k] >= 0, k in {0,1,2,3,10,11}) (0x41D8) |

**Per-object state P** (main's stack, sp+0xF0, used by 6AA0):
- +0 = in+0x3510 (LS buffer of a fragment program);
- +4/+8/+0xC = IN[0x20]/IN[0x24]/IN[0x28];
- +0x10 = upload flag byte; +0x14 = program id (-1); +0x18 = bound offset (0); +0x1C = wanted offset (0);
- +0x20 = cache list pointer; +0x24 = cache count (0).

### 10.4 Command buffer

```c
cb = { first=0, end=0, cursor=0, done=0, cur_ea=0, idx=0, buf={out+0x80,out+0x1080},
       ring_ea=IN[8], delta=IN[0xC], limit=IN[0x10] };
room(n_bytes):  if (cursor + n_bytes > end) next_block();   /* unsigned; end = base+4092 */
```

- **The overflow callback 0x14160 ignores the `need` argument** that 0x18508 passes, and always returns 0 (success).
  - Block boundaries therefore depend only on the threshold n_bytes of each check. That is always the exact size of the words the emitter is about to write (table 10.8).
  - Before the first block end = 0, so the first check with n_bytes > 0 allocates. A check with n_bytes = 0 (layout n = 0) does not.

```c
next_block():                                   /* 0x14160 */
  off = atomic_add_u32(ring_ea, 4096);          /* returns the old value */
  if (off + 4096 > limit) off = limit - 4096;
  done += 4096;
  if (first == 0) first = off;
  else {
    if (off == cur_ea + 4096) zero words cursor..base+4092 (inclusive);
    else *cursor = 0x20000000 | (off + delta);  /* JUMP, written at the cursor, not at +4092 */
    PUT(buf[idx] -> cur_ea, 4096);  idx ^= 1;
  }
  cur_ea = off; cursor = base = buf[idx]; end = base + 4092;
```

End of job, only if at least one range was selected:

```c
rec.w0 = first;  rec.w1 = done + (cursor - base) - 4096;     /* 0xFFFFFFFC if no block */
if (first) { if (scr.vfmt != 0) f_5B60(0);  emit(0x00020000);  /* RETURN, no room check */
             PUT(buf[idx] -> cur_ea, 4096); }
```

Record = `{first, bytes, draws, prog_uploads, prog_bytes, triangles, 0, 0}`.
- Words 3 and 4 are only changed by the 6AA0 upload path (10.7.9). They are 0 in captures.
- If no range is selected, the record is all zero.

### 10.5 Main: range selection (0x4328) and the object loop

```c
cur = R32(GET(IN.w1, 4));
n = (u32)((s32)(cur - IN.w0) >> 9);  if (n > 100) n = 100;
Q = GETL(IN.w0, 512*n);                                       /* list GET, 16 KB elements */
front = 0; back = n;
for (k = 0; k < n; k++) {
  q = Q + 512k;
  if (q.w0 != IN[0x2C] || q.w1 != IN[0x30]) continue;
  s = u16(q + 0x0E + 2b);  e = u16(q + 0x10 + 2b);
  if (IN1[0x0A] == 0) {                                       /* signed s32 math */
    c = (e - s + N - 1) / N;                                  /* N = IN2.w0, i = IN2.w1 */
    s = min(s + c*i, e);  e = min(s + c, e);
  }
  if (s == e) continue;
  idx = q[0x0C] ? --back : front++;      /* deferred records go to the back */
  range[idx] = { q.w2 + 8s, q.w2 + 8e };
}
ranges = range[0..front) ++ range[back..n)  /* normal in table order, then deferred in REVERSE table order */
if (nranges == 0) { record = 0; return; }
mat_on = ntex_bind || ndesc || any(S16(IN1+o) >= 0, o in {A4,A6,A8,AA,AC,AE,B0});   /* 0x4030 */
reset scratch (10.3); cb_init;
for each range r {
  items = GETL(r.begin & ~15, align16(r.end) - (r.begin & ~15)) + (r.begin & 15);
  last_mat_ea = 0; old_mat = NULL;                 /* per range: material state resets */
  for (k = 0; k < (r.end - r.begin) >> 3; k++) {
    E = GET(items[k].w0, 16);  H = GET(items[k].w1, 48);
    if (object_filter(E, H)) continue;             /* 10.7.1: no fetch, no state change */
    changed = 0;
    if (mat_on && E.w3 != last_mat_ea) { mat = GET(E.w3, align16(IN1[0xBC])); last_mat_ea = E.w3; changed = 1; }
    rec = GET(E.w1 & ~15, 0xB0);  T = GET(H[0x14] + 48*u16(E+8), 48);
    S = GET(H[0x1C] + 64*u16(E+10), 64);  X = H[0x20] ? GET(H[0x24], 64*H[0x20]) : NULL;
    if (mat_on) {
      if (changed) { f_143F0(mat); f_15E78(mat, old_mat); old_mat = mat; }
      if ((s32)IN1[0xC0] > 0) f_7C18(mat, H, E, X);
    }
    M = object_matrix(rec, T, S);
    if (type == 0) { if (need_mat) f_BDD8(M); f_6FE0(E); /* f_6AA0: no-op, 10.7.9 */ f_5C98(rec); }
    else           { po = f_B010(M, E); f_5C98(rec); if (po) { room(8); emit(0x00040A68); emit(0); } }
    rec_out.draws += 1;  rec_out.triangles += u16(rec + 4);
  }
}
```

Notes:
- The SPU code is software-pipelined: the instance, header and material GETs run one object ahead, with three material buffers.
- The per-object order above is still exact. The pipeline changes only LS placement, never what is emitted.
- Rejected objects are never fetched beyond E and H, and they do not touch `last_mat_ea`.

### 10.6 Emission order per object (summary)

1. Material changed: textures (10.7.2), then material constants (10.7.3), in descriptor order.
2. Per-object ops, in op order (10.7.4).
3. Type 0: BDD8 matrices (kind 0, kind 1 MVP, kind 3 N, in that order, only when M changed), then stencil, then draw.
4. Type 2: B010 MVP (when M changed), then polygon offset on (if the object is flagged), then draw, then polygon offset off.
5. The draw (10.7.8): vertex format (if the id changed), vertex offsets (if the streams changed), then INDEX, BEGIN, DRAW_INDEX_ARRAY batches, END.

### 10.7 Functions

#### 10.7.1 object_filter (0x13218..0x1332C)

```c
reject = ((H.w0 & 0x40000000) && IN[0x151])
      ||  (H.w0 & 0x80000000)
      ||  (E.w0 & 0x08000000)
      ||  (IN1[0x0A] && ((E.w0 >> 5) & 7) != IN2.w1);
```

- In the captures, 47 objects (15 jobs) are rejected by E bit 27. Section 2.4 said nothing was rejected; that was wrong.
- The other three terms were checked by differential tests.

#### 10.7.2 f_143F0: textures

- D = IN1+0x1C. Only the texture cache is used for change detection; the old-material argument is not read.

```c
for (i = 0; i < R32(IN1+0x54); i++) {
  u = S16(D+4i);  t = mat + 8 + 44*u16(D+4i+2);
  if (cache[u] == {t[0], t[8], t[12], t[20]}) continue;        /* u32 compare */
  cache[u] = {t[0], t[8], t[12], t[20]};
  if (!sent0B00[u]) { room(8);  emit(0x00040B00 + 4u); emit(0x00002DC4); sent0B00[u] = 1; }
  room(40); emit(0x001C1A00 + 0x20u);                          /* TEXTURE_OFFSET.. 7 words */
            emit(t[0]); emit(t[4]); emit(t[8]); emit(t[12]); emit(t[16]); emit(t[20]); emit(t[24]);
            emit(0x00041840 + 4u); emit(t[32]);                /* TEXTURE_CONTROL3 */
  if (any of the nibbles (t[8]&15), (t[8]>>8&15), (t[8]>>16&15) == 4)  /* wrap = BORDER */
    { room(8); emit(0x00041A1C + 0x20u); emit(t[28]); }        /* TEXTURE_BORDER_COLOR */
}
```

#### 10.7.3 f_15E78: material constants

- `old` is the previous material of the same range (NULL for the first one in a range).
- PRM(x) = x + 8 + 44·ntex.

```c
for (i = 0; i < R32(IN1+0x9C); i++) {
  e = R32(IN1+0x58+4i);  slot = (s16)(e>>16);  type = (e>>10)&31;  off = e&1023;
  np = PRM(mat) + 4off;  op = old ? PRM(old) + 4off : NULL;
  switch (type) {
  case 0:  if (op && fceq(np[0], op[0])) break;        /* first word only */
           if (e & 0x8000) FALLBACK;                   /* sRGB->linear, not captured */
           room(24); emit(0x00141EFC); emit(slot); emit(np[0..3]); break;
  case 4:  if (op && fceq4(np[0..3], op[0..3])) break; /* all 4 words */
           room(40); emit(0x00241EFC); emit(slot);
           emit(np[0], np[1], 0, 0x3F800000, np[2], np[3], 0, 0x3F800000); break;
  case 1..3, 5..11: FALLBACK;  /* jump table 0x15FB4 */
  default: break;              /* >11: skipped */
  }
}
```

- The sRGB branch uses the constants 0.04045/12.92/1.055/2.4 at 0x19900..0x199B0. It computes pow() with an exp2/log2 polynomial and is not modelled.
- Types 1..3 and 5..11 probably include the fragment-program binding, since 15E78 also receives P and E.w3.

#### 10.7.4 f_7C18: per-object ops

- d = OPS(mat), consumed in op order whether or not an op emits.

```c
for (i = 0; i < R32(IN1+0xC0); i++) switch (op = R32(IN1+0xC4+4i)) {
case 5:  /* flipbook / sprite-sheet UV offset */
  slot = S16(IN1+0xA8); a = (s32)d[0]; b = (s32)d[1]; d += 8; if (slot < 0) break;
  ab = a*b; frame = E[1] % ab; row = frame / a; col = frame % a;   /* C truncating / and % */
  A = csflt(splat(a)); B = csflt(splat(b)); rA = fi(A, frest(A)); rB = fi(B, frest(B));
  C = csflt(splat(col)); R = csflt(splat(row));
  q = fm(C, rA); x = fma(fnms(q, A, C), rA, q);          /* col / cols */
  q = fm(R, rB); y = fma(fnms(q, B, R), rB, q);          /* row / rows */
  v = (x, y, y, y);
  if (fceq(v.x, cache.x) && fceq(v.y, cache.y)) break;   /* cache = scr+0x60, init 1.0f */
  cache = v; room(24); emit(0x00141EFC); emit(slot); emit(v); break;
case 11: /* 3x4 colour matrix */
  s = S16(IN1+0xAE); s2 = S16(IN1+0xB0); D = d; d += 40;
  if (s < 0 && s2 < 0) break;
  if (D.w0 == 0) m = D.m;                                 /* the material's own matrix */
  else { r = lookup(X, H[0x20], mat.w0);                  /* 0x4158 */
         m = r ? r.m : identity; }
  rows = { (m[0][0..2],0), (m[1][0..2],0), (m[2][0..2],0) };
  if (s >= 0) { room(56); emit(0x00341EFC); emit(s); emit(rows); }
  if (s2 >= 0) FALLBACK;                                  /* 0x152B0 path, not captured */
  break;
case 4:  d += 4; if (S16(IN1+0xA6) >= 0) FALLBACK; break;
case 0..3, 6..10, 12..14: FALLBACK;   /* jump table 0x7D8C; ops 0 and 12 appear in IN1
                                         lists of jobs 31-35, 54, 58, but mat_on is 0 there */
default: break;                       /* >14 */
}
lookup(X, n, key) { fb = NULL;
  for (j = 0; j < n; j++) { if (X[j].w0 == key) return &X[j]; if (X[j].w0 == 0xFFFFFFFF) fb = &X[j]; }
  return fb; }   /* the LAST 0xFFFFFFFF record wins; n == 0 returns NULL */
```

- c[0x104] is the op-11 colour matrix, and c[0x1CA] is the op-5 UV offset (slot A8 = 0x1CA in jobs 8, 12, 39).
- csflt is used only for op 5's integer cols, rows and frame.

#### 10.7.5 object_matrix (main 0x136E0..0x13924, 0x13CA8)

Exact operation order, lane-wise on u128:

```c
T0..T2 = T rows; S0..S3 = S rows; p = (rec[0x28], rec[0x2C], rec[0x30], 0); s = rec[0x34];
for c in 0..3: A[c] = fma(S2, splat(T2[c]), fma(S1, splat(T1[c]), fm(S0, splat(T0[c]))));
A[3] = fa(A[3], S3);
sk = fm(splat(s), LQ(IN+0x340));
D0 = fm(sk,(1,0,0,0)); D1 = fm(sk,(0,1,0,0)); D2 = fm(sk,(0,0,1,0));   /* keeps the -0 lanes */
M[r] = fma(A[2], splat(Dr.z), fma(A[1], splat(Dr.y), fm(A[0], splat(Dr.x))))   r = 0,1,2;
M[3] = fa(A[3], fma(A[2], splat(p.z), fma(A[1], splat(p.y), fm(A[0], splat(p.x)))));  M[3].w = 1.0f;
if (IN1[0x178]) { M[3] = fs(M[3], LQ(IN+0xC0)); M[3].w = 1.0f; }
```

- The w lanes of M[0..2] are kept as computed; matrix kind 0 emits them raw.
- The fma with a 0 factor must stay. Do not simplify it away (signed zeros).

#### 10.7.6 Matrices: f_BDD8 (type 0) and f_B010 (type 2)

Change rule, shared by both: `changed(M)` is true if, for any row r in 0..3, `fceq((M[r].x,y,z,z), (prev[r].x,y,z,z))` fails in any lane.
- Only xyz are compared.
- prev = scr+0x00, zero at job start, never reset per range.
- When changed, prev = M (all 4 lanes).

```c
MVP(M): for r in 0..3 { w = (r == 3) ? 1.0f : 0;
          acc = fm(VP0, splat(M[r].x)); acc = fma(VP1, splat(M[r].y), acc);
          acc = fma(VP2, splat(M[r].z), acc); acc = fma(VP3, splat(w), acc); out[r] = acc; }
        /* VPi = LQ(IN+0x80+16i) */

f_BDD8(M): if (!changed(M)) return; prev = M;
  if (slot[0] >= 0) { room(72); emit(0x00441EFC); emit(slot[0]); emit(M[0..3]); }     /* raw M */
  if (slot[1] >= 0) { room(72); emit(0x00441EFC); emit(slot[1]); emit(MVP(M)); }
  if (slot[2] >= 0) FALLBACK;
  if (slot[3] >= 0) { for r in 0..2 { v = M[r]; v.w = 0; sq = fm(v, v);
                        l = splat(fa(fa(sq.x, sq.y), sq.z));            /* lane 0, then splat */
                        rs = fi(l, frsqest(l));
                        n = fma(fnms(l, fm(rs, rs), splat(1.0f)), fm(rs, splat(0.5f)), rs);
                        N[r] = fm(v, n); N[r].w = 0; }
                      room(56); emit(0x00341EFC); emit(slot[3]); emit(N[0..2]); }
  if (slot[10] >= 0 || slot[11] >= 0) FALLBACK;

f_B010(M, E) -> po:
  if (slot[1] >= 0 || slot[11] >= 0) if (changed(M)) { prev = M;
      if (slot[1] >= 0) { room(72); emit(0x00441EFC); emit(slot[1]); emit(MVP(M)); }
      if (slot[11] >= 0) FALLBACK; }
  if ((E[0] & IN1[0x10F] & 4) && IN[0x153] == 0) {
      room(8);  emit(0x00040A68); emit(1);                         /* POLY_OFFSET_FILL_ENABLE */
      room(12); emit(0x00080A78); emit(0x3F800000); emit(0x44FA0000);  /* factor 1.0, units 2000.0 */
      return 1; }
  return 0;
/* after the draw, if po: room(8); emit(0x00040A68); emit(0); */
```

- The polygon-offset words are not change-detected: they are emitted around every flagged draw (141 of 426 type-2 draws).
- BDD8 also computes a cofactor (cross-product) matrix before its change test. That result is used only by the unexecuted kind 2/10/11 paths.

#### 10.7.7 f_6FE0: stencil (type 0)

```c
if ((IN1[0x10F] & 0xE0) == 0) return;
ref = E[0] & 0xE0;  if (ref == scr.stencil) return;        /* u32 compare, init 0xFFFFFFFF */
room(32); emit(0x000C0330, 0x207, ref, 0xFF, 0x000C0350, 0x207, ref, 0xFF);   /* front, back: ALWAYS */
scr.stencil = ref;
```

#### 10.7.8 f_5C98 / f_5B60: draw

```c
f_5B60(id): v = id==0||id==2 ? 0x0C32 : id==1||id==3||id==5 ? 0x0630|id : NONE;   /* jump table 0x5BA4 */
            if (v != NONE) { room(8); emit(0x00041740); emit(v); }               /* VERTEX_DATA_ARRAY_FORMAT[0] */
f_5C98(rec):
  if (scr.vfmt != rec[0x4C]) { f_5B60(rec[0x4C]); scr.vfmt = rec[0x4C]; }   /* stored even when nothing is emitted */
  if (scr.streams != {rec[0x38], rec[0x3C], rec[0x40]}) {
    n = L[0]; room(8n);
    for j < n: { emit(0x00040000 | (0x1680 + 4*L.attr[j])); emit(rec[0x38 + 4*L.stream[j]] + L.off[j]); }
    scr.streams = {rec[0x38], rec[0x3C], rec[0x40]};
  }
  ni = 3*u16(rec+4);  if (ni == 0) return;                 /* no index/draw words at all */
  nw = (ni + 255) >> 8;  room(4*(nw + 8));
  emit(0x0008181C); emit(rec[0x44] & 0x7FFFFFFF); emit(0x10 | rec[0x44] >> 31);
  emit(0x00041808); emit(5); emit(0x40001824 | nw << 18);
  for (i = 0; i < ni >> 8; i++) emit(0xFF000000 + 256*i);
  if (ni & 255) emit(((ni & 255) - 1) << 24 | ((256*(ni >> 8)) & 0x00FFFF00));
  emit(0x00041808); emit(0);
```

- 0x5B60 emits a single word, the format of attribute 0. Section 6 called it a "burst"; that was wrong.
- The "format table" is the code itself: the jump table at 0x5BA4. Id 4 or id > 5 emits nothing.
- The value is stride<<8 | size<<4 | type: id 0/2 = (12, 3, float); ids 1/3/5 = (6, 3, type id).

#### 10.7.9 f_6AA0: fragment program (type 0)

`f_6AA0(P, cb, tag, out)`:

- **flag = P[0x10] == 0** (the case in every capture):
  - if P[0x18] != P[0x1C]: room(8); emit(0x000408E4) (SET_SHADER_PROGRAM); emit((IN[0x24] + P[0x1C]) | 2); then P[0x18] = P[0x1C].
  - In captures both are 0, so it is a no-op.
- **flag != 0**:
  - Clear the flag. If P[0x14] (program id, init -1) != 0, P[0x11] == 0 and P[0x24] > 0, look the id up in the cache list `{u32 id, u32 offset}[P[0x24]]` at P[0x20].
  - On a hit: if the offset != P[0x1C], emit SET_SHADER_PROGRAM. Either way P[0x18] = P[0x1C] = offset.
  - Otherwise (no lookup, or a miss), upload:
    - size = align128(*(u32*)P.w0);
    - `off = atomic_add(IN[0x20], size)` (the 0x190D8 GETLLAR/PUTLLC);
    - if `off && off + size <= IN[0x28]`: **PUT** `size` bytes from P.w0+0x100 to EA off (cmd 0x20, tag = the scratch tag);
    - out.w3 += 1; out.w4 += size;
    - emit SET_SHADER_PROGRAM (off + IN[0x24]) | 2, then P[0x18] = P[0x1C] = off;
    - insert {id, off} at index count++. When count > 31, the index is 0x19430() % count instead (a pseudo-random replacement).
- So the atomic and DMA are an **on-demand fragment-program upload** into a heap at IN[0x20..0x28].
- The flag starts as `IN1[0xD4] ? 1 : IN1[0x17A]`. P[0x14], P[0x1C] and the ucode at in+0x3510 are only set by the unexecuted 15E78/7C18 branches. That is why native type 0 must fall back when the flag can be set (10.2).

### 10.8 Room-check thresholds (bytes; need = bytes/4 is what is passed to 0x18508, but ignored)

| emitter | check | words written |
|---|---|---|
| 1 constant vector (15E78 type 0, op 5, 0x5A38) | 24 | 6 |
| 2 vectors (15E78 type 4) | 40 | 10 |
| 3x4 (op 11, N, 0x5838) | 56 | 14 |
| 4x4 (M, MVP) | 72 | 18 |
| CONTROL2 / border / format / poly-offset enable or disable | 8 each | 2 |
| texture burst (0x1A00 + 0x1840) | 40 | 10 |
| polygon offset factors | 12 | 3 |
| stencil | 32 | 8 |
| vertex offsets | 8·L.n (one check for all) | 2·L.n |
| index + draw | 4·(nw + 8) | nw + 8 |
| RETURN | none | 1 |

### 10.9 Validation coverage

- **Exercised by the 52 captured jobs (byte-exact):**
  - range split with N = 1;
  - deferred-range ordering (18 deferred, 60 normal ranges);
  - E bit-27 reject;
  - per-range material reset;
  - textures: 201 emits, 9 cache hits, 32 CONTROL2;
  - constant type 0: 72 emits with no old material, 54 changed, 114 skipped;
  - constant type 4: 29 emits, 15 skipped;
  - op 5: 12 emits, 101 cache hits;
  - op 11: 21 own matrices, 20 0xFFFFFFFF records, 1 identity;
  - BDD8: 106 changed, 127 unchanged; kind 0 in job 11; N;
  - B010: 186 unchanged; polygon offset 141;
  - stencil: 8 emits, 105 skips;
  - vertex format ids 0 and 2;
  - 570 vertex-offset bursts;
  - multi-batch draws (258); an exact multiple of 256 (1);
  - JUMP chaining (11);
  - camera-relative on.
- **Exercised only by the differential tests (10.0):**
  - contiguous blocks and the clamp;
  - split with N > 1;
  - selector filter;
  - H bit 30/31;
  - IN[0x153];
  - border colour;
  - vertex format ids 1, 3, 4, 5, 7;
  - stencil changes and stencil off;
  - ni = 0;
  - op 5 with non-trivial and negative cols;
  - kind 0 in a 48-draw job;
  - L.n = 0 and 2;
  - camera-relative off.
- **Read but never executed (no oracle check):**
  - the op-11 key match `X[j].w0 == mat.w0`: every captured record count is 1, so a key hit and the 0xFFFFFFFF fallback cannot be told apart;
  - 6AA0's upload path;
  - the room checks that land exactly on a block edge for emitters other than texture, vector and draw. The rule is uniform, so this is low risk.

### 10.10 Errata to sections 1-8

- 2: the range order is reversed. Records with byte +0x0C **== 0** come first in table order; flagged ones come last in reverse table order. The model fails 20/44 with the order the other way round.
- 2.4: object_filter does reject objects: 47 in the captures, by E bit 27. See 10.7.1. Its inputs are IN[0x151] and IN1[0x0A]; IN[0x34] is not used.
- 2.1: the "material change" detection is reset at the start of every range.
- 1 / IN table: IN+0x6C is not used by types 0 and 2.
- 6: texture entries are **44 bytes**, at mat+8+44t, and only the bound ones are emitted (the 4 entries in job 12 are the 4 bindings at IN1+0x1C).
- 6: c[0x104] is not integer-derived. It is the op-11 3x4 colour matrix, from the material or the extra record. c[0x1CA] is the op-5 UV offset, and that is where the csflt is.
- 6: 0x5B60 emits one word (attribute-0 format), not a burst.
- 3: the `need` argument of 0x18508 is ignored by the callback, so only the byte threshold matters.
- 4.1: the formula is right. 10.7.5 adds the diagonal scale as three separate fm/fma chains (signed zeros) and the full w-lane behaviour.

### 10.11 Open questions (updated; supersedes section 8)

1. Fallback paths, which need captures from other areas (`SPU_PUT_DUMP`) before porting:
   - pass types 1, 3, 4;
   - 15E78 types 1..3 and 5..11, and the sRGB bit;
   - 7C18 ops 0..3, 6..10, 12..14 (ops 0 and 12 are listed in captured IN1 blocks but gated off by mat_on = 0), op 4's emission, and op 11's second output (slot +0xB0, 0x152B0);
   - matrix kinds 2, 10, 11 and the D6D8 side kinds (4..9, 12, 13, 17, 43, 45, IN1[9]).
2. The fragment-program upload in 6AA0: where the ucode at in+0x3510 and the program id come from, presumably from a 15E78/7C18 branch. It is the only place type 0 does an extra atomic and DMA, and it bumps record words 3 and 4.
3. The op-11 key-match path (`mat.w0 == record.w0`) was read but not exercised. Every captured extra list has one 0xFFFFFFFF record.
4. 0x19430 (the replacement index for the program cache) is not decoded. It looks like a PRNG.
5. API+68 and IN2.w2 (0x40, 0xD9: a sequence?) are still unknown. Neither affects the command words.
6. IN1+0xA0, +0xA2 and +0xA4, +0xAA, +0xAC: their op owners are unknown (only mat_on tests the last three).
7. Methods 0x0B00 (value 0x2DC4) and the meaning of format types 1/3/5 rest on the usual NV40/RSX method map. Bit-exactness does not depend on it.
