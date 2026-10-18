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

## 8. Open questions

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
