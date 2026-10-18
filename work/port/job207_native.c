/* inFAMOUS 2 (BCUS98125): the flat instance culler, natively.
 *
 * SPU job module 5cfff962445a (overlay 207), the sibling of the bc11 scene
 * walker (jobbc11_native.c). Its input is a flat list of 28-byte entries,
 * each naming three guest blocks: a set of boxes with per-box item ranges
 * and an item table, one 8-byte record per box (LOD levels and a flag), and a
 * 4x4 transform. Every box goes through the transform and is culled exactly as
 * in bc11 (height clip, minimum size, size-to-distance ratio, the frame's
 * visibility BSP); the items of a visible box whose LOD range admits the box's
 * level are bucket-sorted and PUT as bc11's 512-byte header plus 8-byte list,
 * at addresses taken from two atomic cursors.
 *
 * This replaces the job's worker (LS 0x4030), written from
 * rpi/job207-native/RESEARCH.md and checked against the job's captured
 * transfers and a replay of the SPU code (work/runs/job207/oracle: natdrv.c
 * runs this file offline). Every LS address comes from the worker's
 * arguments (r3..r9), which the wrapper took from the manager's getbuf. The
 * model works on private copies of what the job writes (the gather buffer,
 * the output buffer, the item scratch), reads the entries' blocks from guest
 * memory where the job would have fetched them, and only when the walk
 * finished on a modelled path takes the cursors, writes the outputs and
 * leaves LS as the job leaves it. Otherwise nothing is written and the job
 * runs on the SPU as before.
 *
 * The lift calls spu_native_entry at 0x4030 (tools/lift_spu.py,
 * NATIVE_ENTRIES).
 *
 * PS3RECOMP_NATIVE_J207: 1 (default) on; 0 off; 2 runs the SPU worker after
 * the model and compares what it left in LS with the model, logging any
 * difference. The 10 s stats line counts fallbacks by reason. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>

#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_coherency.h"

extern uint8_t* vm_base;
int spu_native_register(int image_id, uint32_t addr, int (*fn)(spu_context*));
void (*spu_lookup(uint32_t addr, int image_id))(spu_context*);
int spu_ea_committed(uint32_t ea, uint32_t size);

#define J207_IMAGE  207
#define J207_WORKER 0x4030u

#define J207_ITEMS    1792     /* the 1,792nd item flushes the batch */
#define J207_BSPSTACK 60       /* 0x4ED8's stack holds 64 words before it runs into its caller */
#define J207_BATCHES  32
#define J207_G3       0x8040u  /* gather buffer: boxes +0, per-box records +0x4000, transform +0x8000 */
#define J207_OUT      0x3A80u  /* output buffer: list elements +0, header +0x80, list +0x280 */

static inline uint32_t R32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline void P32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static inline void P16(uint8_t* p, uint32_t v) { p[0] = v >> 8; p[1] = v; }
static inline u128 V4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 v; v._u32[0] = a; v._u32[1] = b; v._u32[2] = c; v._u32[3] = d; return v; }
static inline u128 LQB(const uint8_t* p) { return V4(R32(p), R32(p + 4), R32(p + 8), R32(p + 12)); }
#define F1   0x3F800000u
#define FH   0x3F000000u

/* Floats. The job's arithmetic is SPU single precision: truncating, denormals
 * as zero, no infinities (exponent 255 is a number). With host floats under
 * round-toward-zero, FTZ and DAZ that is the same for every value below 2^128,
 * so the model runs on host SSE when it can and checks its way out of the rest:
 * an input word with exponent 255 falls back (FB_FLOAT) and so does any
 * overflow or invalid flag the walk raised (run_model). Built without FMA
 * (or with J207_HELPER_FLOAT) it runs on the SPU float helpers instead,
 * exact by construction and several times slower. */
#if defined(__x86_64__) && defined(__SSE4_1__) && defined(__FMA__) && !defined(J207_HELPER_FLOAT)
#define J207_HOST_FLOAT 1
#include <immintrin.h>
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif
typedef __m128 F4;
static inline F4 F4V(u128 u) { return _mm_castsi128_ps(_mm_loadu_si128((const __m128i*)u._u32)); }
static inline F4 F4W(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return _mm_castsi128_ps(_mm_setr_epi32((int)a, (int)b, (int)c, (int)d)); }
#define FLANE(v, i) ((uint32_t)_mm_extract_ps((v), (i)))
#define FSPL(v, i) _mm_shuffle_ps((v), (v), _MM_SHUFFLE(i, i, i, i))
#define FSET(v, i, x) _mm_insert_ps((v), _mm_castsi128_ps(_mm_cvtsi32_si128((int)(x))), (i) << 4)
static inline F4 FMUL(F4 a, F4 b) { return _mm_mul_ps(a, b); }
static inline F4 FADD(F4 a, F4 b) { return _mm_add_ps(a, b); }
static inline F4 FSUB(F4 a, F4 b) { return _mm_sub_ps(a, b); }
static inline F4 FMA_(F4 a, F4 b, F4 c) { return _mm_fmadd_ps(a, b, c); }    /* a*b + c */
static inline F4 FMS_(F4 a, F4 b, F4 c) { return _mm_fmsub_ps(a, b, c); }    /* a*b - c */
static inline F4 FGT(F4 a, F4 b) { return _mm_cmpgt_ps(a, b); }
static inline F4 FSEL(F4 a, F4 b, F4 m) { return _mm_blendv_ps(a, b, m); }  /* selb: m ? b : a */
static inline F4 FABS(F4 a) { return _mm_andnot_ps(_mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u)), a); }
static inline int GT0(F4 a, F4 b) { return _mm_movemask_ps(_mm_cmpgt_ps(a, b)) & 1; }
/* lanes (bit mask) holding an exponent-255 word */
static inline int FBAD(F4 v, int lanes)
{
    const __m128i e = _mm_set1_epi32(0x7F800000);
    return _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(v), e), e))) & lanes;
}
#else
static inline u128 SPL(u128 v, int i) { return V4(v._u32[i], v._u32[i], v._u32[i], v._u32[i]); }
static inline u128 LANE(u128 v, int i, uint32_t x) { v._u32[i] = x; return v; }
typedef u128 F4;
static inline F4 F4V(u128 u) { return u; }
static inline F4 F4W(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return V4(a, b, c, d); }
#define FLANE(v, i) ((v)._u32[i])
#define FSPL(v, i) SPL((v), (i))
#define FSET(v, i, x) LANE((v), (i), (x))
static inline F4 FMUL(F4 a, F4 b) { return spu_fm(a, b); }
static inline F4 FADD(F4 a, F4 b) { return spu_fa(a, b); }
static inline F4 FSUB(F4 a, F4 b) { return spu_fs(a, b); }
static inline F4 FMA_(F4 a, F4 b, F4 c) { return spu_fma(a, b, c); }
static inline F4 FMS_(F4 a, F4 b, F4 c) { return spu_fms(a, b, c); }
static inline F4 FGT(F4 a, F4 b) { return spu_fcgt(a, b); }
static inline F4 FSEL(F4 a, F4 b, F4 m) { return spu_selb(a, b, m); }
static inline F4 FABS(F4 a) { return spu_andc(a, V4(0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u)); }
static inline int GT0(F4 a, F4 b) { return spu_fcgt(a, b)._u32[0] != 0; }
static inline int FBAD(F4 v, int lanes) { (void)v; (void)lanes; return 0; }
#endif
/* (x + y) + z of lanes 0..2: the job's shuffle-and-add horizontal sum, in lane 0 */
static inline F4 HSUM3(F4 v) { return FADD(FADD(v, FSPL(v, 1)), FSPL(v, 2)); }

enum { FB_GET = 1, FB_EA = 2, FB_LS = 4, FB_BUCKET = 8, FB_BSP = 16, FB_BATCH = 32, FB_MEM = 64, FB_FLOAT = 128, FB_RANGE = 256 };
#define J207_NREASON 9

typedef struct { F4 x[4]; } X4;
/* a batch as the job PUTs it; cursor 2 grows by `add` (the job reserves 8x
 * the list), the list PUT is `list_size` bytes */
typedef struct { uint32_t n, add, list_size; uint8_t hdr[512]; uint8_t* list; } Batch;

typedef struct {
    spu_context* ctx;
    const uint8_t* ls;
    int fallback;
    uint32_t bad_ea, bad_size;
    unsigned csr0;
    /* worker arguments */
    uint32_t hdr, planes, list, ls3, ls2;
    /* header */
    F4 cam, h30, h40;
    uint32_t nplanes, nent, mode, cur1, lim1, cur2, lim2;
    /* items of the current batch (LS3+0x8040: byte 0, words 2 and 3) */
    uint32_t n, maxn, items_total;
    uint8_t ib[J207_ITEMS]; uint32_t iw2[J207_ITEMS], iw3[J207_ITEMS];
    uint32_t nbatch; Batch batch[J207_BATCHES];
    uint32_t used_g3, bsp_lo, bsp_hi;
    uint32_t last_ea[3], last_sz[3];   /* the last GET into each block slot (+0, +0x4000, +0x8000) */
    /* every block GET of the walk, for mode 2's code 7 (J207_GETS max, then ngets_over) */
    uint32_t ngets, ngets_over;
    struct { uint32_t ea, sz, off; } gets[4096];
    /* PS3RECOMP_J207_DUMP: every block the walk read, with the bytes it saw */
    int log;
    uint32_t nlog, logcap, logbytes, logbcap;
    struct { uint32_t ea, sz, off, at; }* glog;
    uint8_t* glogb;
    uint8_t g3[J207_G3];
    uint8_t out[J207_OUT];
} J;

/* ---------------- LS reads, exactly as the job's quadword loads ---------------- */
static inline const uint8_t* QP(const J* j, uint32_t a)
{
    a &= 0x3FFF0u;
    return a - j->ls3 < J207_G3 ? j->g3 + (a - j->ls3) : j->ls + a;
}
/* lqx/lqd of the quad holding A, rotqby by B: the preferred word */
static inline uint32_t FW(const J* j, uint32_t A, uint32_t B)
{
    const uint8_t* q = QP(j, A); const uint32_t o = B & 15;
    return (uint32_t)q[o] << 24 | (uint32_t)q[(o + 1) & 15] << 16 | (uint32_t)q[(o + 2) & 15] << 8 | q[(o + 3) & 15];
}
static inline F4 LQ(const J* j, uint32_t a) { return F4V(LQB(QP(j, a))); }
#define WORD(r, k) FW(j, (r) + (k), (r) + (k))
/* a read the model keeps inside the private gather buffer (the SPU would see
 * whatever else is there: the item scratch the walk itself is writing) */
static inline int in_g3(const J* j, uint32_t a, uint32_t n) { return a - j->ls3 < J207_G3 && n <= J207_G3 - (a - j->ls3); }

/* ---------------- guest memory ---------------- */
static inline int ea_ok(uint32_t ea, uint32_t size) { return ea >= 0x10000u && spu_ea_committed(ea, size); }

static void guest_put(J* j, uint32_t ea, const uint8_t* src, uint32_t size)
{
    if (!size) return;
    const uint32_t a0 = ea & ~127u, a1 = (ea + size - 1u) & ~127u;
    int reserved = 0;
    for (uint32_t a = a0; ; a += 128u) { if (spu_coh_is_reserved(a)) { reserved = 1; break; } if (a == a1) break; }
    if (reserved) {
        const uint64_t m = spu_lockline_mask_range(ea, size);
        spu_lockline_lock_mask(m);
        memcpy(vm_base + ea, src, size);
        for (uint32_t a = a0; ; a += 128u) { if (spu_coh_is_reserved(a)) spu_coh_notify_write_except(a, j->ctx); if (a == a1) break; }
        spu_lockline_unlock_mask(m);
    } else {
        memcpy(vm_base + ea, src, size);
        for (uint32_t a = a0; ; a += 128u) {
            if (spu_coh_is_reserved(a)) {
                const uint64_t m = spu_lockline_mask_ea(a);
                spu_lockline_lock_mask(m);
                spu_coh_notify_write_except(a, j->ctx);
                spu_lockline_unlock_mask(m);
            }
            if (a == a1) break;
        }
    }
    extern void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
    if (g_spu_put_hook) g_spu_put_hook(ea, size);
}

/* 0x69E0: atomic add on the guest word at ea & ~3, returning the old value */
static uint32_t guest_atomic_add(J* j, uint32_t ea, uint32_t v)
{
    ea &= ~3u;
    const uint64_t m = spu_lockline_mask_ea(ea);
    spu_lockline_lock_mask(m);
    uint32_t* p = (uint32_t*)(vm_base + ea);
    uint32_t raw = __atomic_load_n(p, __ATOMIC_RELAXED), old;
    do old = __builtin_bswap32(raw);
    while (!__atomic_compare_exchange_n(p, &raw, __builtin_bswap32(old + v), 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
    if (spu_coh_is_reserved(ea & ~127u)) spu_coh_notify_write_except(ea, j->ctx);
    spu_lockline_unlock_mask(m);
    return old;
}

/* ---------------- geometry (the same code as bc11) ---------------- */
/* 0x5270 (bc11 0x6650): box through the 4x4 row-vector transform X (center and |X| extent) */
static void box_xform(F4 mn, F4 mx, const X4* X, F4* wmn, F4* wmx)
{
    const F4 h = F4W(FH, FH, FH, FH);
    const F4 c = FMUL(FADD(mn, mx), h);
    const F4 e = FSUB(mx, c);
    F4 w = FMUL(X->x[0], FSPL(c, 0));
    w = FMA_(X->x[1], FSPL(c, 1), w);
    const F4 r10 = FMUL(FABS(X->x[1]), FSPL(e, 1));
    w = FMA_(X->x[2], FSPL(c, 2), w);
    F4 r5 = FMA_(FSPL(e, 0), FABS(X->x[0]), r10);
    w = FADD(w, X->x[3]);
    r5 = FMA_(FSPL(e, 2), FABS(X->x[2]), r5);
    *wmx = FADD(w, r5); *wmn = FSUB(w, r5);
}

/* 0x4ED8 (bc11 0x6220): is any part of the box in an empty leaf of the visibility BSP?
 * Four frustum planes first (node k+1 = node k + 32 * w(node k + 0x14)),
 * then the tree: node = {plane (n, d); u32 back; u32 front} in 32-byte units
 * from the node; a null front is empty space, a null back is solid. */
/* the nodes the walk read, for check_layout: the tree's offsets can point
 * anywhere in LS, including what the job writes */
static inline void bsp_note(J* j, uint32_t N)
{
    N &= 0x3FFF0u;
    if (N < j->bsp_lo) j->bsp_lo = N;
    if (N > j->bsp_hi) j->bsp_hi = N;
}
static int bsp(J* j, uint32_t P, F4 mn, F4 mx, uint32_t n)
{
    if (!n) return 1;
    const F4 mn1 = FSET(mn, 3, F1), mx1 = FSET(mx, 3, F1);
    uint32_t N = P;
    for (int k = 0; k < 4; k++) {
        const F4 pl = LQ(j, N); bsp_note(j, N);
        if (FBAD(pl, 15)) { j->fallback |= FB_FLOAT; return 0; }
        const F4 a = FMUL(pl, mn1), b = FMUL(pl, mx1);
        const F4 hi = FSEL(b, a, FGT(a, b));
        if (GT0(FSPL(pl, 3), HSUM3(hi))) return 0;
        N += FW(j, N + 16, 4) << 5;          /* rotqbyi 4 of the quad at N+16: w(N+0x14) */
    }
    uint32_t stk[J207_BSPSTACK]; int sp = 0;
    const F4 z = F4W(0, 0, 0, 0);
    for (uint32_t it = 0; ; it++) {
        if (it > 1000000u) { j->fallback |= FB_BSP; return 0; }
        const F4 pl = LQ(j, N); bsp_note(j, N);
        if (FBAD(pl, 15)) { j->fallback |= FB_FLOAT; return 0; }
        const F4 a = FMUL(pl, mn), b = FMUL(pl, mx);
        const F4 g = FGT(a, b);
        const F4 hi = FSEL(b, a, g), lo = FSEL(a, b, g);
        const F4 d = FSPL(pl, 3);
        const F4 mind = FSUB(HSUM3(lo), d), maxd = FSUB(HSUM3(hi), d);    /* lanes 0 of r3 and its rotate */
        const uint32_t wback = FW(j, N + 16, 0), wfront = FW(j, N + 16, 4);
        int front;
        if (GT0(mind, z)) front = 1;
        else if (GT0(z, maxd)) front = 0;
        else {                                  /* straddles: the back half later */
            if (wback) {
                if (sp >= J207_BSPSTACK) { j->fallback |= FB_BSP; return 0; }
                stk[sp++] = N + (wback << 5);
            }
            front = 1;
        }
        if (front) {
            if (!wfront) return 1;
            N += wfront << 5;
        } else {
            N += wback << 5;
            if (!wback) {
                if (!sp) return 0;
                N = stk[--sp];
            }
        }
    }
}


/* the worker's inline cull (0x445C..0x47D4): bc11's 0x4030 with the header
 * fields at other offsets. +0x48 byte: clip at the height +0x44; +0x38
 * minimum size^2; +0x3C size-to-distance ratio, height scaled by +0x40. */
static int cull(J* j, F4 mn, F4 mx, const X4* X)
{
    F4 wmn, wmx;
    box_xform(mn, mx, X, &wmn, &wmx);
    if (FW(j, j->hdr + 0x40, 5) & 255) {
        const F4 t = FSPL(j->h40, 1);
        if (GT0(t, FSPL(wmx, 2))) return 0;
        if (GT0(t, FSPL(wmn, 2))) wmn = FSET(wmn, 2, FLANE(j->h40, 1));
    }
    const F4 e = FSUB(wmx, wmn);
    const F4 s = HSUM3(FMUL(e, e));
    if (GT0(FSPL(j->h30, 2), FSPL(s, 0))) return 0;
    if (GT0(FSPL(j->h30, 3), F4W(0, 0, 0, 0))) {
        F4 d = FMS_(FADD(wmn, wmx), F4W(FH, FH, FH, FH), j->cam);
        d = FSET(d, 2, FLANE(FMUL(FSPL(d, 2), FSPL(j->h40, 0)), 0));
        const F4 dist = HSUM3(FMUL(d, d));
        if (GT0(FMUL(FSPL(dist, 0), FSPL(j->h30, 3)), FSPL(s, 0))) return 0;
    }
    return bsp(j, j->planes, wmn, wmx, j->nplanes) & 255;
}

/* ---------------- guest blocks (the three GETs per entry) ---------------- */
static int get3(J* j, uint32_t off, uint32_t ea, uint32_t sz)
{
    if (!sz) return 1;
    /* what the MFC refuses (the SPU then reads stale LS) or rewrites */
    if (sz > 0x4000u || (ea & 15)) { j->fallback |= FB_GET; return 0; }
    if (!ea_ok(ea, sz)) {
        if (!(j->fallback & FB_EA)) { j->bad_ea = ea; j->bad_size = sz; }
        j->fallback |= FB_EA; return 0;
    }
    static const uint8_t edge_sig[16] = { 0, 0, 0, 0, 0x05, 0x00, 0x02, 0xD0, 0xBF, 0x80, 0, 0, 0x3F, 0x80, 0, 0 };
    if (sz == 0x80 && !memcmp(vm_base + ea, edge_sig, 16)) { j->fallback |= FB_GET; return 0; }
    memcpy(j->g3 + off, vm_base + ea, sz);
    j->used_g3 = 1;
    const int slot = off == 0 ? 0 : off == 0x4000 ? 1 : 2;
    j->last_ea[slot] = ea; j->last_sz[slot] = sz;
    if (j->ngets < 4096) { j->gets[j->ngets].ea = ea; j->gets[j->ngets].sz = sz; j->gets[j->ngets].off = off; j->ngets++; }
    else j->ngets_over = 1;
    if (j->log) {
        if (j->nlog == j->logcap) {
            const uint32_t nc = j->logcap ? 2 * j->logcap : 256;
            void* nl = realloc(j->glog, nc * sizeof *j->glog);
            if (!nl) { j->log = 0; return 1; }
            j->glog = nl; j->logcap = nc;
        }
        if (j->logbytes + sz > j->logbcap) {
            uint32_t nc = j->logbcap ? 2 * j->logbcap : 1u << 20;
            while (nc < j->logbytes + sz) nc *= 2;
            uint8_t* nb = (uint8_t*)realloc(j->glogb, nc);
            if (!nb) { j->log = 0; return 1; }
            j->glogb = nb; j->logbcap = nc;
        }
        j->glog[j->nlog].ea = ea; j->glog[j->nlog].sz = sz; j->glog[j->nlog].off = off; j->glog[j->nlog].at = j->logbytes;
        memcpy(j->glogb + j->logbytes, vm_base + ea, sz);
        j->nlog++; j->logbytes += sz;
    }
    return 1;
}

/* ---------------- output: 0x4D08 bucket sort, flushes ---------------- */
static void sort_into_out(J* j)
{
    uint16_t cnt[248], off[248];
    memset(cnt, 0, sizeof cnt);
    for (uint32_t i = 0; i < j->n; i++) {
        if (j->ib[i] >= 248) { j->fallback |= FB_BUCKET; return; }
        cnt[j->ib[i]]++;
    }
    uint16_t acc = 0;
    for (int b = 0; b < 248; b++) { off[b] = acc; acc = (uint16_t)(acc + cnt[b]); }
    for (uint32_t i = 0; i < j->n; i++) {
        const uint32_t idx = off[j->ib[i]]++;
        P32(j->out + 0x280 + 8 * idx, j->iw2[i]);
        P32(j->out + 0x284 + 8 * idx, j->iw3[i]);
    }
    for (int b = 0; b < 248; b++) P16(j->out + 0x90 + 2 * b, off[b]);
}
static void close_batch(J* j, int flush)
{
    if (!j->n) return;
    sort_into_out(j);
    if (j->fallback) return;
    if (j->nbatch == J207_BATCHES) { j->fallback |= FB_BATCH; return; }
    Batch* b = &j->batch[j->nbatch];
    b->n = j->n;
    if (flush) { b->list_size = 14336; b->add = 0x1C000; }               /* 0x4A30, 0x4AC0 */
    else { b->list_size = (j->n * 8 + 127) & ~127u; b->add = b->list_size << 3; }   /* 0x4548..0x4558 */
    memcpy(b->hdr, j->out + 0x80, 512);
    if (!b->list) b->list = (uint8_t*)malloc(J207_ITEMS * 8);
    if (!b->list) { j->fallback |= FB_MEM; return; }
    memcpy(b->list, j->out + 0x280, b->list_size);
    j->nbatch++;
    j->items_total += j->n;
    j->n = 0;
}
static void emit(J* j, uint32_t bucket, uint32_t w2, uint32_t w3)
{
    j->ib[j->n] = (uint8_t)bucket; j->iw2[j->n] = w2; j->iw3[j->n] = w3;
    j->n++;
    if (j->n > j->maxn) j->maxn = j->n;
    if (j->n == J207_ITEMS) close_batch(j, 1);
}
static inline int32_t SX4(uint32_t v) { return (int32_t)(v << 28) >> 28; }

/* ---------------- the worker (0x4030) ---------------- */
static __attribute__((noinline)) void walk(J* j)
{
    const uint32_t H = j->hdr;
    memcpy(j->g3, j->ls + j->ls3, J207_G3);
    memcpy(j->out, j->ls + j->ls2, J207_OUT);
    /* output header as 0x4134..0x418C leaves it: w0 = hdr+0x1C, w1 = hdr+0x20,
     * byte 12 = hdr+0x49, bytes 14-15 = 0 */
    P32(j->out + 0x80, FW(j, H + 0x10, 12));
    P32(j->out + 0x84, FW(j, H + 0x20, 0));
    j->out[0x8C] = (uint8_t)(FW(j, H + 0x40, 6) & 255);
    j->out[0x8E] = j->out[0x8F] = 0;
    j->nplanes = FW(j, H + 0x10, 0); j->nent = FW(j, H + 0x10, 4);
    j->mode = FW(j, H + 0x20, 4); j->cur1 = FW(j, H + 0x20, 8); j->lim1 = FW(j, H + 0x20, 12);
    j->cur2 = FW(j, H + 0x30, 0); j->lim2 = FW(j, H + 0x30, 4);
    if (!j->nent) return;                              /* 0x41BC: nothing at all */
    j->cam = LQ(j, H); j->h30 = LQ(j, H + 0x30); j->h40 = LQ(j, H + 0x40);
    if (FBAD(j->cam, 7) || FBAD(j->h30, 12) || FBAD(j->h40, 3)) { j->fallback |= FB_FLOAT; return; }
    const uint32_t L3 = j->ls3, T0 = L3 + 8u;
    uint32_t E = j->list;
    for (uint32_t e = 0; e != j->nent; e++, E += 28) {
        if (j->fallback) return;
        /* the three GETs (0x4228..0x4304), then the tag wait */
        const uint32_t ea1 = FW(j, E + 16, E), sz1 = ((FW(j, E + 26, E + 40) & 0xFFFF) + 15) & ~15u;
        const uint32_t ea2 = FW(j, E + 20, E + 20), sz2 = (((FW(j, E + 24, E + 38) & 0xFFFF) << 3) + 15) & ~15u;
        const uint32_t ea3 = FW(j, E + 8, E + 8);
        if (!get3(j, 0, ea1, sz1) || !get3(j, 0x4000, ea2, sz2) || !get3(j, 0x8000, ea3, 64)) return;
        const uint32_t nb = FW(j, L3, L3), total = FW(j, L3, 4);
        const uint32_t w12 = FW(j, E + 12, E + 12), w4 = FW(j, E + 4, E + 4);
        if (!nb) continue;
        const uint32_t T = T0 + (nb << 5) - (nb << 2);    /* item table after the boxes */
        X4 M;
        for (int r = 0; r < 4; r++) {
            M.x[r] = LQ(j, L3 + 0x8000 + 16 * r);
            if (FBAD(M.x[r], 7)) { j->fallback |= FB_FLOAT; return; }
        }
        for (uint32_t i = 0; nb > i; i++) {
            const uint32_t B = T0 + (i << 5) - (i << 2), P8 = L3 + 0x4000 + (i << 3);
            if (!in_g3(j, B, 56) || !in_g3(j, P8, 8)) { j->fallback |= FB_RANGE; return; }
            const F4 mn = F4W(WORD(B, 0), WORD(B, 4), WORD(B, 8), WORD(B, 8));
            const F4 mx = F4W(WORD(B, 12), WORD(B, 16), WORD(B, 20), WORD(B, 20));
            if (FBAD(mn, 7) || FBAD(mx, 7)) { j->fallback |= FB_FLOAT; return; }
            if (!cull(j, mn, mx, &M)) continue;
            if (j->fallback) return;
            /* items [start, end): end is the next box's start, or the total */
            const uint32_t start = (uint32_t)(int32_t)(int16_t)(FW(j, B + 26, B + 40) & 0xFFFF);
            const uint32_t next = (uint32_t)(int32_t)(int16_t)(FW(j, B + 54, B + 68) & 0xFFFF);
            const uint32_t end = (nb > i + 1) ? next : total;
            if (!(end > start)) continue;
            const int32_t v2 = (int8_t)(FW(j, P8, P8 + 15) & 255);           /* the box's level */
            for (uint32_t k = start; end > k; k++) {
                const uint32_t I = T + (k << 4);
                if (!in_g3(j, I, 16) || !in_g3(j, I + j->mode + 3, 1)) { j->fallback |= FB_RANGE; return; }
                const uint32_t bucket = FW(j, I + j->mode + 3, I + j->mode + 3 + 13) & 255;
                if (bucket == 255) continue;
                const uint32_t rb = FW(j, I, I + 15);
                const int32_t lo = SX4(rb), hi = SX4(rb >> 4);    /* the item's level range: hi <= v <= lo */
                int take;
                if (j->mode == 2) take = !(v2 > lo || hi > v2);
                else if (j->mode == 3) take = (hi <= 6 && lo > 5) || (FW(j, I + 9, I + 22) & 255);
                else if (v2 <= lo && v2 >= hi) take = 1;
                else {
                    const int32_t v1 = (int8_t)(FW(j, P8, P8 + 14) & 255);
                    take = (FW(j, P8, P8 + 13) & 255) == 1 && !(v1 > lo) && !(hi > v1);
                }
                if (!take) continue;
                const int32_t idx = (int16_t)(FW(j, I, I + 14) & 0xFFFF);
                emit(j, bucket, w12 + ((uint32_t)idx << 4), w4);
                if (j->fallback) return;
            }
        }
    }
    if (j->fallback) return;
    close_batch(j, 0);
}

/* The walk under the SPU's float rules: round toward zero, FTZ, DAZ; an
 * overflow or invalid operation it raised falls back. */
static void run_model(J* j)
{
#ifdef J207_HOST_FLOAT
    const unsigned csr = _mm_getcsr();
    j->csr0 = csr & ~0x003Fu;
    _mm_setcsr((csr & ~0x003Fu) | 0x6000u | 0x8000u | 0x0040u | 0x1F80u);
    walk(j);
    const unsigned fl = _mm_getcsr();
    _mm_setcsr(csr);
    if (fl & 0x09u) j->fallback |= FB_FLOAT;
#else
    walk(j);
#endif
}

/* ---------------- publish: the cursors, the PUTs, LS as the job leaves it ---------------- */
static void publish(J* j)
{
    spu_context* ctx = j->ctx;
    uint32_t line = 0;
    for (uint32_t k = 0; k < j->nbatch; k++) {
        Batch* b = &j->batch[k];
        const uint32_t hea = guest_atomic_add(j, j->cur1, 512);
        line = j->cur1;
        if (hea == 0 || hea + 512 > j->lim1) {                   /* dropped */
            memcpy(j->out + 0x90, b->hdr + 0x10, 496);
            continue;
        }
        uint32_t lea = guest_atomic_add(j, j->cur2, b->add);
        line = j->cur2;
        if (lea + b->add > j->lim2) lea = 0;
        if (!lea) memset(b->hdr + 0x10, 0, 496);
        P32(b->hdr + 8, lea);
        if (!(hea & 15) && ea_ok(hea, 512)) guest_put(j, hea, b->hdr, 512);   /* what the MFC would do */
        memcpy(j->out + 0x80, b->hdr, 512);
        if (lea) {
            uint32_t left = b->list_size, o = 0, e = 0;
            while (left) {
                const uint32_t c = left > 16384 ? 16384 : left;
                P32(j->out + e, c); P32(j->out + e + 4, lea + o);
                if (!((lea + o) & 15) && ea_ok(lea + o, c)) guest_put(j, lea + o, b->list + o, c);
                o += c; left -= c; e += 8;
            }
        }
    }
    memcpy(ctx->ls + j->ls2, j->out, J207_OUT);
    for (uint32_t i = 0; i < j->maxn; i++) {
        uint8_t* it = ctx->ls + j->ls3 + 0x8040 + 16 * i;
        it[0] = j->ib[i]; P32(it + 8, j->iw2[i]); P32(it + 12, j->iw3[i]);
    }
    if (j->used_g3) memcpy(ctx->ls + j->ls3, j->g3, J207_G3);
    if (line) memcpy(ctx->ls + 0x80, vm_base + (line & ~127u), 128);
}

static int job_init(J* j, spu_context* ctx)
{
    j->fallback = 0; j->bad_ea = j->bad_size = 0;
    j->n = j->maxn = 0; j->nbatch = 0; j->items_total = 0; j->used_g3 = 0; j->bsp_lo = ~0u; j->bsp_hi = 0;
    j->nlog = 0; j->logbytes = 0; j->ngets = j->ngets_over = 0;
    for (int k = 0; k < 3; k++) j->last_ea[k] = j->last_sz[k] = 0;
    j->ctx = ctx; j->ls = ctx->ls;
    j->hdr = ctx->gpr[3]._u32[0] & 0x3FFFFu; j->planes = ctx->gpr[4]._u32[0] & 0x3FFFFu;
    j->list = ctx->gpr[5]._u32[0] & 0x3FFFFu; j->ls3 = ctx->gpr[6]._u32[0] & 0x3FFFFu;
    j->ls2 = ctx->gpr[8]._u32[0] & 0x3FFFFu;
    if ((j->ls2 | j->ls3 | j->hdr) & 15 || j->ls3 + 0x8040u + 16u * J207_ITEMS > 0x40000u ||
        j->ls2 + J207_OUT > 0x40000u || j->hdr + 0x50u > 0x40000u) {
        j->fallback |= FB_LS; return 0;
    }
    return 1;
}

/* After the walk: what the job wrote must not overlap what it read or the
 * module (0x4000..0x5850), or the SPU would have seen its own writes. */
static int ovl(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) { return a0 < b1 && b0 < a1 && a0 < a1 && b0 < b1; }
static void check_layout(J* j)
{
    /* the cursors' lines must be guest memory before any atomic: the MFC
     * refuses an atomic on anything else and the job spins, a host CAS faults */
    if (j->nbatch) {
        const uint32_t bad = !ea_ok(j->cur1 & ~127u, 128) ? j->cur1 : !ea_ok(j->cur2 & ~127u, 128) ? j->cur2 : 0;
        if (bad) { j->bad_ea = bad; j->bad_size = 128; j->fallback |= FB_EA; return; }
    }
    uint32_t maxlist = 0;
    for (uint32_t k = 0; k < j->nbatch; k++) if (j->batch[k].list_size > maxlist) maxlist = j->batch[k].list_size;
    const uint32_t w[3][2] = { { j->used_g3 ? j->ls3 : 0, j->used_g3 ? j->ls3 + J207_G3 : 0 },
                               { j->ls2, j->ls2 + 0x280u + maxlist },
                               { j->ls3 + 0x8040u, j->ls3 + 0x8040u + 16u * j->maxn } };
    const uint32_t r[5][2] = { { 0x4000u, 0x5850u }, { j->hdr, j->hdr + 0x50u },
                               { j->planes, j->planes + 32u * (j->nplanes > 4 ? j->nplanes : 5) },
                               { j->list, j->list + 28u * j->nent },
                               { j->bsp_lo, j->bsp_hi >= j->bsp_lo ? j->bsp_hi + 32u : 0 } };
    for (int a = 0; a < 3; a++) {
        for (int b = 0; b < 5; b++) if (ovl(w[a][0], w[a][1], r[b][0], r[b][1])) j->fallback |= FB_LS;
        for (int b = a + 1; b < 3; b++) if (ovl(w[a][0], w[a][1], w[b][0], w[b][1])) j->fallback |= FB_LS;
    }
}

/* Mode 2: what the SPU worker left in LS against the model: buffer 2 (list
 * elements at the SPU's list EA, header minus the list EA, bucket offsets
 * when that EA is set, the last list with its padding), the item scratch and
 * the gather buffer. The PUTs themselves are not compared: their addresses
 * come from the cursors. */
static int compare(J* j, const uint8_t* ls)
{
    const uint8_t* o = ls + j->ls2;
    uint8_t lo[0x80];
    memcpy(lo, j->out, 0x80);
    /* the elements are the last list PUT's; which batch that was depends on
     * cursor results only the SPU saw: compared for a single batch with a list */
    if (j->nbatch > 1 || (j->nbatch == 1 && !R32(o + 0x88))) memcpy(lo, o, 0x80);
    else if (j->nbatch && R32(o + 0x88)) {
        const uint32_t lea = R32(o + 0x88), ls_ = j->batch[j->nbatch - 1].list_size;
        for (uint32_t off = 0, e = 0; off < ls_; off += 16384, e += 8) {
            const uint32_t left = ls_ - off;
            P32(lo + e, left > 16384 ? 16384 : left); P32(lo + e + 4, lea + off);
        }
    }
    if (memcmp(o, lo, 0x80)) return 1;
    const uint8_t* h = j->nbatch ? j->batch[j->nbatch - 1].hdr : j->out + 0x80;
    if (memcmp(o + 0x80, h, 8) || memcmp(o + 0x8C, h + 12, 4)) return 2;
    if (j->nbatch) {
        const Batch* b = &j->batch[j->nbatch - 1];
        if (R32(o + 0x88) && memcmp(o + 0x90, b->hdr + 0x10, 496)) return 3;
        if (memcmp(o + 0x280, b->list, b->list_size)) return 4;
    }
    for (uint32_t i = 0; i < j->maxn; i++) {
        const uint8_t* it = ls + j->ls3 + 0x8040 + 16 * i;
        if (it[0] != j->ib[i] || R32(it + 8) != j->iw2[i] || R32(it + 12) != j->iw3[i]) return 5;
    }
    if (j->used_g3 && memcmp(ls + j->ls3, j->g3, J207_G3)) {
        /* Code 7, not a model difference: for every differing byte, the latest
         * GET that wrote it (any entry's: a later, smaller GET into the same
         * slot leaves an earlier one's tail in place) shows that the SPU's copy
         * equals guest memory now while the model's is what guest memory held
         * when it read it. The block changed between the two reads (another
         * SPU or the PPU writing it); the outputs above already matched. */
        if (j->ngets_over) return 6;
        const uint8_t* sp = ls + j->ls3;
        for (uint32_t a = 0; a < J207_G3; a++) {
            if (sp[a] == j->g3[a]) continue;
            int g = (int)j->ngets - 1;
            while (g >= 0 && !(a >= j->gets[g].off && a < j->gets[g].off + j->gets[g].sz)) g--;
            if (g < 0) return 6;                                       /* not from a GET: a model difference */
            if (sp[a] != vm_base[j->gets[g].ea + (a - j->gets[g].off)]) return 6;
        }
        return 7;
    }
    return 0;
}

#ifndef J207_NO_RUNTIME
static _Thread_local J* t_j;
static _Thread_local int t_bypass;
static int s_mode = -1;
static unsigned long long s_native, s_fallback, s_cmp, s_bad, s_ns_nat, s_ns_spu, s_items, s_raced;
/* PS3RECOMP_J207_DUMP=dir (mode 2): the first PS3RECOMP_J207_DUMP_N (16)
 * differing jobs, as dir/NNN_*: info.txt (code, registers, header fields,
 * counts), ls_before.bin and ls_spu.bin (the whole LS before the worker and
 * after the SPU ran it), native_g3.bin / native_out.bin / native_items.bin /
 * native_batches.bin (the model's buffer 3 up to the scratch, buffer 2, items
 * as 16-byte scratch entries, each batch's 512-byte header and list),
 * gets.txt (every block GET the model made: index, EA, size, slot offset,
 * offset in the next two files), gets_native.bin (the bytes it read) and
 * gets_guest_after.bin (guest memory at those EAs after the SPU ran). Code 7
 * jobs are dumped too. */
static const char* s_dump;
static int s_dump_n = 16, s_dumped;
static _Thread_local uint8_t* t_lsb;
static void wfile(const char* dir, int k, const char* name, const void* p, size_t n)
{
    char path[600]; snprintf(path, sizeof path, "%s/%03d_%s", dir, k, name);
    FILE* f = fopen(path, "wb"); if (f) { fwrite(p, 1, n, f); fclose(f); }
}
static void dump(J* j, spu_context* ctx, int code)
{
    const int k = __atomic_fetch_add(&s_dumped, 1, __ATOMIC_RELAXED);
    if (k >= s_dump_n) return;
    char info[2048]; int w = 0;
    w += snprintf(info + w, sizeof info - w, "code %d\nhdr %05X planes %05X list %05X ls2 %05X ls3 %05X\n", code, j->hdr, j->planes, j->list, j->ls2, j->ls3);
    w += snprintf(info + w, sizeof info - w, "nplanes %u nent %u mode %u cur1 %08X lim1 %08X cur2 %08X lim2 %08X\n",
                  j->nplanes, j->nent, j->mode, j->cur1, j->lim1, j->cur2, j->lim2);
    w += snprintf(info + w, sizeof info - w, "batches %u items %u maxn %u gets %u (log %s)\n", j->nbatch, j->items_total, j->maxn, j->nlog, j->log ? "complete" : "off or truncated");
    for (int k2 = 0; k2 < 3; k2++) w += snprintf(info + w, sizeof info - w, "last GET slot %d: %08X size %X\n", k2, j->last_ea[k2], j->last_sz[k2]);
    for (int r = 0; r < 12; r++) w += snprintf(info + w, sizeof info - w, "r%d %08X %08X %08X %08X\n", r, ctx->gpr[r]._u32[0], ctx->gpr[r]._u32[1], ctx->gpr[r]._u32[2], ctx->gpr[r]._u32[3]);
    wfile(s_dump, k, "info.txt", info, (size_t)w);
    if (t_lsb) wfile(s_dump, k, "ls_before.bin", t_lsb, SPU_LS_SIZE);
    wfile(s_dump, k, "ls_spu.bin", ctx->ls, SPU_LS_SIZE);
    wfile(s_dump, k, "native_g3.bin", j->g3, J207_G3);
    wfile(s_dump, k, "native_out.bin", j->out, J207_OUT);
    uint8_t* it = (uint8_t*)calloc(j->maxn ? j->maxn : 1, 16);
    if (it) { for (uint32_t i = 0; i < j->maxn; i++) { it[16 * i] = j->ib[i]; P32(it + 16 * i + 8, j->iw2[i]); P32(it + 16 * i + 12, j->iw3[i]); }
              wfile(s_dump, k, "native_items.bin", it, 16u * j->maxn); free(it); }
    char path[600]; snprintf(path, sizeof path, "%s/%03d_native_batches.bin", s_dump, k);
    FILE* f = fopen(path, "wb");
    if (f) { for (uint32_t b = 0; b < j->nbatch; b++) { fwrite(j->batch[b].hdr, 1, 512, f); fwrite(j->batch[b].list, 1, j->batch[b].list_size, f); } fclose(f); }
    snprintf(path, sizeof path, "%s/%03d_gets.txt", s_dump, k);
    f = fopen(path, "w");
    if (f) { for (uint32_t g = 0; g < j->nlog; g++) fprintf(f, "%u %08X %X %X %X\n", g, j->glog[g].ea, j->glog[g].sz, j->glog[g].off, j->glog[g].at); fclose(f); }
    if (j->nlog) {
        wfile(s_dump, k, "gets_native.bin", j->glogb, j->logbytes);
        snprintf(path, sizeof path, "%s/%03d_gets_guest_after.bin", s_dump, k);
        f = fopen(path, "wb");
        if (f) { for (uint32_t g = 0; g < j->nlog; g++) fwrite(vm_base + j->glog[g].ea, 1, j->glog[g].sz, f); fclose(f); }
    }
    fprintf(stderr, "[j207] dumped job %d (code %d) to %s\n", k, code, s_dump);
}
static unsigned s_reason[16];
static inline uint64_t now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void stats(void)
{
    static uint64_t next;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = (uint64_t)ts.tv_sec;
    uint64_t n = __atomic_load_n(&next, __ATOMIC_RELAXED);
    if (now < n || !__atomic_compare_exchange_n(&next, &n, now + 10, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    if (!n) return;
    static const char* const name[J207_NREASON] = { "GET", "EA", "LS", "BUCKET", "BSP", "BATCH", "MEM", "FLOAT", "RANGE" };
    char why[300] = ""; int w = 0;
    for (int i = 0; i < J207_NREASON; i++)
        if (s_reason[i]) w += snprintf(why + w, sizeof why - w, " %s:%u", name[i], s_reason[i]);
    fprintf(stderr, "[j207] native %llu, SPU %llu (fallbacks by reason:%s), items %llu%s", s_native, s_fallback,
            w ? why : " none", s_items, s_mode == 2 ? "" : "\n");
    if (s_mode == 2) fprintf(stderr, ", compared %llu, differ %llu, blocks changed under the job %llu; per compared job native %.1f us, SPU %.1f us\n",
                             s_cmp, s_bad, s_raced, s_cmp ? s_ns_nat / 1e3 / s_cmp : 0.0, s_cmp ? s_ns_spu / 1e3 / s_cmp : 0.0);
}

static int j207_worker(spu_context* ctx)
{
    if (t_bypass) return 0;
    if (__builtin_expect(s_mode < 0, 0)) {
        const char* e = getenv("PS3RECOMP_NATIVE_J207");
        s_mode = e ? atoi(e) : 1;
        fprintf(stderr, "[j207] native instance culler: %s\n", s_mode == 2 ? "compare with the SPU job" : s_mode ? "on" : "off");
        s_dump = getenv("PS3RECOMP_J207_DUMP");
        if (getenv("PS3RECOMP_J207_DUMP_N")) s_dump_n = atoi(getenv("PS3RECOMP_J207_DUMP_N"));
    }
    if (!s_mode) return 0;
    if (!t_j) { t_j = (J*)calloc(1, sizeof(J)); if (!t_j) return 0; }
    J* j = t_j;
    stats();
    const uint64_t t0 = now_ns();
    const int dumping = s_mode == 2 && s_dump && __atomic_load_n(&s_dumped, __ATOMIC_RELAXED) < s_dump_n;
    if (dumping) {
        if (!t_lsb) t_lsb = (uint8_t*)malloc(SPU_LS_SIZE);
        if (t_lsb) memcpy(t_lsb, ctx->ls, SPU_LS_SIZE);
    }
    j->log = dumping;
    if (job_init(j, ctx)) { run_model(j); if (!j->fallback) check_layout(j); }
    const uint64_t t1 = now_ns();
    if (j->fallback) {
        __atomic_fetch_add(&s_fallback, 1, __ATOMIC_RELAXED);
        for (int i = 0; i < J207_NREASON; i++) if (j->fallback >> i & 1) __atomic_fetch_add(&s_reason[i], 1, __ATOMIC_RELAXED);
        if (j->fallback & FB_EA) { static int nl;
            if (nl++ < 6) fprintf(stderr, "[j207] EA fallback: 0x%08X size 0x%X\n", j->bad_ea, j->bad_size); }
        if (j->fallback & FB_LS) { static int nl;
            if (nl++ < 6) fprintf(stderr, "[j207] LS layout fallback: hdr %05X list %05X ls2 %05X ls3 %05X\n",
                                  j->hdr, j->list, j->ls2, j->ls3); }
        return 0;
    }
    if (s_mode == 2) {
        void (*fn)(spu_context*) = spu_lookup(J207_WORKER, ctx->resident_ovl);
        if (!fn) return 0;
        const int32_t si = (int32_t)ctx->image_id;
        t_bypass = 1;
        const uint32_t r0 = ctx->gpr[0]._u32[0];
        ctx->host_depth++; fn(ctx); spu_drain_call(ctx, r0); ctx->host_depth--;
        spu_img_restore(ctx, si);
        t_bypass = 0;
        __atomic_fetch_add(&s_ns_nat, t1 - t0, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_ns_spu, now_ns() - t1, __ATOMIC_RELAXED);
        const int bad = compare(j, ctx->ls);
        __atomic_fetch_add(&s_cmp, 1, __ATOMIC_RELAXED);
        if (bad && dumping) dump(j, ctx, bad);
        if (bad == 7) {
            unsigned long long nr = __atomic_add_fetch(&s_raced, 1, __ATOMIC_RELAXED);
            if (nr <= 4) fprintf(stderr, "[j207] job's blocks changed between the model's read and the SPU's GET (7): %u batches, %u items; outputs equal\n",
                                 j->nbatch, j->items_total);
        } else if (bad) {
            unsigned long long nb = __atomic_add_fetch(&s_bad, 1, __ATOMIC_RELAXED);
            if (nb <= 8) fprintf(stderr, "[j207] job differs (%d): %u batches, %u items; hdr %05X ls2 %05X ls3 %05X\n",
                                 bad, j->nbatch, j->items_total, j->hdr, j->ls2, j->ls3);
        }
        ctx->pc = ctx->gpr[0]._u32[0];
        return 1;
    }
    publish(j);
    __atomic_fetch_add(&s_native, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_items, j->items_total, __ATOMIC_RELAXED);
    return 1;
}

__attribute__((constructor)) static void j207_register(void)
{
    spu_native_register(J207_IMAGE, J207_WORKER, j207_worker);
}
#endif
