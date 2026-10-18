/* inFAMOUS 2 (BCUS98125): the scene walker that builds the visible-object
 * draw list, natively.
 *
 * SPU job module bc11d5e7b134 (overlay 217) walks a tree of command streams
 * in guest memory: groups with quantized bounding boxes, LOD switches,
 * instances with their own transform, draw items. Every box is culled against
 * a height plane, a screen-size threshold and the frame's visibility BSP
 * (four frustum planes, then occluder planes); every LOD switch tests the
 * distance to the camera. The visible draw items are bucket-sorted and PUT as
 * a 512-byte header plus a list of 8-byte (batch EA, instance EA) pairs, at
 * addresses the job takes from two atomic cursors. About 3.5% of all samples
 * in the city.
 *
 * This replaces the job's worker (LS 0x4358), written from
 * rpi/job-bc11-native/RESEARCH.md and checked against the job's captured
 * transfers and a replay of the SPU code (work/runs/job217/oracle: natdrv.c
 * runs this file offline). The worker gets every LS address from its
 * arguments (r3..r11), which the wrapper took from the job manager's
 * getbuf, so no layout is assumed here. The model works on private copies of
 * the LS it writes (the two stream buffers, the output buffer, the item
 * scratch), reads the child streams from guest memory where the job would
 * have fetched them, and only when the walk finished on a modelled path does
 * it take the cursors, write the outputs and leave LS as the job leaves it.
 * Otherwise nothing is written and the job runs on the SPU as before.
 *
 * The lift calls spu_native_entry at 0x4358 (tools/lift_spu.py,
 * NATIVE_ENTRIES).
 *
 * PS3RECOMP_NATIVE_BC11: 1 (default) on; 0 off; 2 runs the SPU worker after
 * the native model and compares the output buffer it leaves in LS (header
 * minus the list EA, and the last list) with the model's, logging any
 * difference. PS3RECOMP_BC11_OPHIST=1 adds the op behind each FB_OP fallback
 * to the 10 s stats line. */
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

#define BC11_IMAGE  217
#define BC11_WORKER 0x4358u

#define BC11_QMAX     384      /* 0x4278: the queue refuses a push at count > 383 */
#define BC11_ITEMS    1792     /* op 7 flushes past this many items */
#define BC11_BSPSTACK 60       /* 0x6220's stack holds 64 words before it runs into its caller */
#define BC11_BATCHES  32

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
 * overflow or invalid flag the walk raised (run_model). The one routine
 * overflow, a LOD radius of FLT_MAX squared, is decided without the multiply
 * (lod_in). Built without FMA (or with BC11_HELPER_FLOAT) it runs on the SPU
 * float helpers instead, exact by construction and several times slower. */
#if defined(__x86_64__) && defined(__SSE4_1__) && defined(__FMA__) && !defined(BC11_HELPER_FLOAT)
#define BC11_HOST_FLOAT 1
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
static inline u128 F4U(F4 v) { u128 u; _mm_storeu_si128((__m128i*)u._u32, _mm_castps_si128(v)); return u; }
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
static inline F4 FCVT(int32_t a, int32_t b, int32_t c, int32_t d) { return _mm_cvtepi32_ps(_mm_setr_epi32(a, b, c, d)); }
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
static inline u128 F4U(F4 v) { return v; }
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
static inline F4 FCVT(int32_t a, int32_t b, int32_t c, int32_t d) { return spu_csflt(V4((uint32_t)a, (uint32_t)b, (uint32_t)c, (uint32_t)d), 155); }
static inline int GT0(F4 a, F4 b) { return spu_fcgt(a, b)._u32[0] != 0; }
static inline int FBAD(F4 v, int lanes) { (void)v; (void)lanes; return 0; }
#endif
/* (x + y) + z of lanes 0..2: the job's shuffle-and-add horizontal sum, in lane 0 */
static inline F4 HSUM3(F4 v) { return FADD(FADD(v, FSPL(v, 1)), FSPL(v, 2)); }
/* SPU shl: counts of 32..63 give 0 */
static inline int32_t SHL(int32_t v, uint32_t s) { return (s & 63) > 31 ? 0 : (int32_t)((uint32_t)v << (s & 31)); }

enum { FB_OP = 1, FB_SKIP0 = 2, FB_BUCKET = 4, FB_BSP = 8, FB_GET = 16, FB_EA = 32, FB_LS = 64, FB_BATCH = 128,
       FB_MEM = 256, FB_FLOAT = 512 };

typedef struct { F4 x[4]; } X4;
typedef struct { uint32_t ctx0, ctx1, ea; uint16_t size; uint8_t lod; X4 xf; } QE;
typedef struct { uint32_t n; uint8_t hdr[512]; uint8_t* list; uint32_t list_size; } Batch;
typedef struct { X4 xf; uint32_t ctx0, ctx1, lsbuf, lscur, size, ea; uint8_t lod; } Slot;

typedef struct {
    spu_context* ctx;
    const uint8_t* ls;
    int fallback;
    uint32_t bad_ea, bad_size, bad_op;
    unsigned csr0;            /* MXCSR outside the walk (op 10 runs on the SPU helpers under it) */
    /* worker arguments */
    uint32_t hdr, planes, stream, ls3, ls2, ls1;
    /* header fields */
    F4 cam, h70;
    uint32_t nplanes, cur1, lim1, cur2, lim2;
    /* walk state */
    X4 C;                     /* current transform, sp+0xD0 */
    uint32_t ctx0, ctx1;      /* sp+0x20: item base EA, instance EA */
    QE q[BC11_QMAX]; uint32_t qn;
    Slot slot[2];
    uint32_t n, maxn;         /* items in the current batch; most items any batch had (LS3 entries written) */
    uint8_t ib[BC11_ITEMS]; uint32_t iw2[BC11_ITEMS], iw3[BC11_ITEMS];
    uint32_t nbatch; Batch batch[BC11_BATCHES];
    uint32_t items_total;
    /* private LS: the stream buffers ls1+0x4000..+0xC000, and the output buffer */
    uint32_t gb_lo, used_gbuf, stream_end, ticks;
    uint8_t gbuf[0x8000];
    uint8_t out[0x3C00];
} J;

/* ---------------- LS reads, exactly as the job's quadword loads ---------------- */
static inline const uint8_t* QP(const J* j, uint32_t a)
{
    a &= 0x3FFF0u;
    return a - j->gb_lo < 0x8000u ? j->gbuf + (a - j->gb_lo) : j->ls + a;
}
/* lqx/lqd of the quad holding A, rotqby by B: the preferred word */
static inline uint32_t FW(const J* j, uint32_t A, uint32_t B)
{
    const uint8_t* q = QP(j, A); const uint32_t o = B & 15;
    return (uint32_t)q[o] << 24 | (uint32_t)q[(o + 1) & 15] << 16 | (uint32_t)q[(o + 2) & 15] << 8 | q[(o + 3) & 15];
}
static inline F4 LQ(const J* j, uint32_t a) { return F4V(LQB(QP(j, a))); }
/* the record field idioms (r = record LS address, 4-aligned) */
#define BYTE1(r)  (FW(j, (r) + 1, (r) + 14) & 255)
#define HALF2(r)  (FW(j, (r) + 2, (r) + 16) & 0xFFFF)
#define WORD(r, k) FW(j, (r) + (k), (r) + (k))

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

/* ---------------- geometry ---------------- */
/* 0x66F8: quantized local box. byte 0 = center shift, bytes 1-3 signed
 * center, byte 4 = extent shift, bytes 5-7 unsigned extent. */
static void box_q(const J* j, uint32_t p, F4* mn, F4* mx)
{
    const uint32_t s0 = FW(j, p, p + 13) & 255, s1 = FW(j, p + 4, p + 17) & 255;
    const int32_t c1 = (int8_t)(FW(j, p + 1, p + 14) & 255), c2 = (int8_t)(FW(j, p + 2, p + 15) & 255),
                  c3 = (int8_t)(FW(j, p + 3, p + 16) & 255);
    const int32_t e1 = (int32_t)(FW(j, p + 5, p + 18) & 255), e2 = (int32_t)(FW(j, p + 6, p + 19) & 255),
                  e3 = (int32_t)(FW(j, p + 7, p + 20) & 255);
    const F4 c = FCVT(SHL(c1, s0), SHL(c2, s0), SHL(c3, s0), SHL(c3, s0));
    const F4 e = FCVT(SHL(e1, s1), SHL(e2, s1), SHL(e3, s1), SHL(e3, s1));
    *mx = FADD(c, e); *mn = FSUB(c, e);
}

/* 0x6650: box through the 4x4 row-vector transform X (center and |X| extent) */
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

/* 0x6220: is any part of the box in an empty leaf of the visibility BSP?
 * Four frustum planes first (node k+1 = node k + 32 * w(node k + 0x14)),
 * then the tree: node = {plane (n, d); u32 back; u32 front} in 32-byte units
 * from the node; a null front is empty space, a null back is solid. */
static int bsp(J* j, uint32_t P, F4 mn, F4 mx, uint32_t n)
{
    if (!n) return 1;
    const F4 mn1 = FSET(mn, 3, F1), mx1 = FSET(mx, 3, F1);
    uint32_t N = P;
    for (int k = 0; k < 4; k++) {
        const F4 pl = LQ(j, N);
        if (FBAD(pl, 15)) { j->fallback |= FB_FLOAT; return 0; }
        const F4 a = FMUL(pl, mn1), b = FMUL(pl, mx1);
        const F4 hi = FSEL(b, a, FGT(a, b));
        if (GT0(FSPL(pl, 3), HSUM3(hi))) return 0;
        N += FW(j, N + 16, 4) << 5;          /* rotqbyi 4 of the quad at N+16: w(N+0x14) */
    }
    uint32_t stk[BC11_BSPSTACK]; int sp = 0;
    const F4 z = F4W(0, 0, 0, 0);
    for (uint32_t it = 0; ; it++) {
        if (it > 1000000u) { j->fallback |= FB_BSP; return 0; }
        const F4 pl = LQ(j, N);
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
                if (sp >= BC11_BSPSTACK) { j->fallback |= FB_BSP; return 0; }
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

/* 0x4030: height clip, size and distance thresholds, then the BSP */
static int cull(J* j, F4 mn, F4 mx, const X4* X)
{
    F4 wmn, wmx;
    box_xform(mn, mx, X, &wmn, &wmx);
    if (FW(j, j->hdr + 0x80, 14) & 255) {           /* byte hdr+0x81: clip at the height hdr+0x7C */
        const F4 t = FSPL(j->h70, 3);
        if (GT0(t, FSPL(wmx, 2))) return 0;
        if (GT0(t, FSPL(wmn, 2))) wmn = FSET(wmn, 2, FLANE(j->h70, 3));
    }
    const F4 e = FSUB(wmx, wmn);
    const F4 s = HSUM3(FMUL(e, e));
    if (GT0(FSPL(j->h70, 1), FSPL(s, 0))) return 0;            /* hdr+0x74: minimum size^2 */
    if (GT0(FSPL(j->h70, 2), F4W(0, 0, 0, 0))) {               /* hdr+0x78: distance^2 / size^2 ratio */
        F4 d = FMS_(FADD(wmn, wmx), F4W(FH, FH, FH, FH), j->cam);
        d = FSET(d, 2, FLANE(FMUL(FSPL(d, 2), FSPL(j->h70, 0)), 0));   /* height scaled by hdr+0x70 */
        const F4 dist = HSUM3(FMUL(d, d));
        if (GT0(FMUL(FSPL(dist, 0), FSPL(j->h70, 2)), FSPL(s, 0))) return 0;
    }
    return bsp(j, j->planes, wmn, wmx, j->nplanes) & 255;
}

/* 0x5E20: LOD band b. Out of band when lod > b and the distance (height
 * scaled by hdr+0x70) reaches the band's radius hdr+0x54[b]. */
static int lod_in(J* j, uint32_t b, uint32_t lod, F4 c, const X4* X)
{
    if (!((lod & 255) > (b & 255))) return 1;
    F4 w = FMUL(X->x[0], FSPL(c, 0));
    w = FMA_(X->x[1], FSPL(c, 1), w);
    w = FMA_(X->x[2], FSPL(c, 2), w);
    w = FADD(w, X->x[3]);
    F4 d = FSUB(w, j->cam);
    const uint32_t ta = j->hdr + 84 + ((b & 255) << 2);
    const uint32_t tw = FW(j, ta, ta);
#ifdef BC11_HOST_FLOAT
    if ((tw & 0x7F800000u) == 0x7F800000u) { j->fallback |= FB_FLOAT; return 0; }
#endif
    d = FSET(d, 2, FLANE(FMUL(FSPL(d, 2), FSPL(j->h70, 0)), 0));
    const F4 dist = HSUM3(FMUL(d, d));
#ifdef BC11_HOST_FLOAT
    /* |t| >= 2^64: t*t is at least 2^128 on the SPU, above every distance
     * the host computed without overflowing. One that overflowed raised the
     * flag that makes the job fall back, so dist must really be computed:
     * the test on its bits keeps it live (a host float is never all ones). */
    if ((tw & 0x7F800000u) >= (191u << 23)) return FLANE(dist, 0) != 0xFFFFFFFFu;
#endif
    const F4 t = F4W(tw, tw, tw, tw);
    return GT0(FMUL(t, t), FSPL(dist, 0));
}

/* op 19 (0x5258): is the center within the radius (+8)? Same distance as
 * lod_in, no band precondition; radius^2 > dist^2. */
static int sphere_in(J* j, uint32_t rw, F4 c, const X4* X)
{
    F4 w = FMUL(X->x[0], FSPL(c, 0));
    w = FMA_(X->x[1], FSPL(c, 1), w);
    w = FMA_(X->x[2], FSPL(c, 2), w);
    w = FADD(w, X->x[3]);
    F4 d = FSUB(w, j->cam);
    d = FSET(d, 2, FLANE(FMUL(FSPL(d, 2), FSPL(j->h70, 0)), 0));
    const F4 dist = HSUM3(FMUL(d, d));
#ifdef BC11_HOST_FLOAT
    if ((rw & 0x7F800000u) >= (191u << 23)) return FLANE(dist, 0) != 0xFFFFFFFFu;   /* as lod_in */
#endif
    const F4 t = F4W(rw, rw, rw, rw);
    return GT0(FMUL(t, t), FSPL(dist, 0));
}

/* ---------------- the queue (0x4278 push, 0x5EE8 pop and GET) ---------------- */
static void push(J* j, uint32_t size, uint32_t ea, uint32_t c0, uint32_t c1, const X4* X, uint32_t lod)
{
#ifdef BC11_TRACE
    fprintf(stderr, "PUSH size %X ea %08X lod %u qn %u\n", size, ea, lod, j->qn);
#endif
    if (!size || j->qn > BC11_QMAX - 1) return;
    QE* e = &j->q[j->qn++];
    e->ctx0 = c0; e->ctx1 = c1; e->ea = ea; e->size = (uint16_t)size; e->lod = (uint8_t)lod; e->xf = *X;
}
static int pop(J* j, int k)
{
    if (!j->qn) return 0;
    const QE* e = &j->q[--j->qn];
    Slot* s = &j->slot[k];
    s->xf = e->xf; s->ctx0 = e->ctx0; s->ctx1 = e->ctx1; s->ea = e->ea; s->size = e->size; s->lod = e->lod;
    s->lscur = s->lsbuf + (e->ea & 15);
#ifdef BC11_TRACE
    fprintf(stderr, "POP slot %d ea %08X size %X lod %u ctx %08X %08X\n", k, e->ea, e->size, e->lod, e->ctx0, e->ctx1);
#endif
    const uint32_t ea = e->ea & ~15u, sz = ((e->ea & 15) + e->size + 15) & ~15u;
    if (sz > 0x4000u) { j->fallback |= FB_GET; return 1; }      /* the MFC would refuse it */
    if (!sz) return 1;
    if (!ea_ok(ea, sz)) {
        if (!(j->fallback & FB_EA)) { j->bad_ea = ea; j->bad_size = sz; }
        j->fallback |= FB_EA; return 1;
    }
    /* a 128-byte GET that starts like Edge's viewport block is rewritten by the
     * MFC under resolution scaling (spu_edge_viewport_scale): leave it to the SPU */
    static const uint8_t edge_sig[16] = { 0, 0, 0, 0, 0x05, 0x00, 0x02, 0xD0, 0xBF, 0x80, 0, 0, 0x3F, 0x80, 0, 0 };
    if (sz == 0x80 && !memcmp(vm_base + ea, edge_sig, 16)) { j->fallback |= FB_GET; return 1; }
    memcpy(j->gbuf + (s->lsbuf - j->gb_lo), vm_base + ea, sz);
    j->used_gbuf = 1;
    return 1;
}

/* ---------------- output: 0x6050 bucket sort, op 7 flush ---------------- */
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
        P32(j->out + 0x380 + 8 * idx, j->iw2[i]);
        P32(j->out + 0x384 + 8 * idx, j->iw3[i]);
    }
    for (int b = 0; b < 248; b++) P16(j->out + 0x190 + 2 * b, off[b]);
}
/* the batch as the job would PUT it: header (its list EA set at publish) and
 * the list, rounded up to 128 bytes with whatever the buffer held */
static void close_batch(J* j)
{
    if (!j->n) return;
    sort_into_out(j);
    if (j->fallback) return;
    if (j->nbatch == BC11_BATCHES) { j->fallback |= FB_BATCH; return; }
    Batch* b = &j->batch[j->nbatch];
    b->n = j->n; b->list_size = (j->n * 8 + 127) & ~127u;
    memcpy(b->hdr, j->out + 0x180, 512);
    if (!b->list) b->list = (uint8_t*)malloc(BC11_ITEMS * 8);
    if (!b->list) { j->fallback |= FB_MEM; return; }
    memcpy(b->list, j->out + 0x380, b->list_size);
    j->nbatch++;
    j->items_total += j->n;
    j->n = 0;
}
static void emit(J* j, uint32_t r)
{
    if (j->n + 1 > BC11_ITEMS) { close_batch(j); if (j->fallback) return; }
    j->ib[j->n] = (uint8_t)BYTE1(r);
    j->iw2[j->n] = j->ctx0 + (HALF2(r) << 4);
    j->iw3[j->n] = j->ctx1;
    j->n++;
    if (j->n > j->maxn) j->maxn = j->n;
}

/* op 6 / op 14: the 256-bit group mask at hdr+0x10, bit (idx & 127) of quad idx >> 7 */
static inline int mask_bit(const J* j, uint32_t mb)
{
    const uint32_t idx = mb & 127;
    const uint8_t* mq = QP(j, j->hdr + 0x10 + 16 * (mb >> 7));
    return (mq[15 - (idx >> 3)] >> (idx & 7)) & 1;
}

/* op 10 (0x5570): a plane in the node's local space (+8: n, d) taken to world
 * space with the adjugate of the current transform over its determinant,
 * normalized, and tested against the camera: with header+0x38 bit 5 set, a
 * camera behind the plane culls the child. frest/fi/frsqest leave no room for
 * host floats, so this runs on the SPU float helpers, as literally as the
 * listing, under the caller's MXCSR. Returns 1 when the child is pushed. */
static u128 PRM(u128 v) { return V4(v._u32[1], v._u32[2], v._u32[0], v._u32[0]); }     /* the 0x6D00 shuffle */
static u128 ROT1(u128 v) { return V4(v._u32[1], v._u32[2], v._u32[3], v._u32[0]); }
static u128 ROT2(u128 v) { return V4(v._u32[2], v._u32[3], v._u32[0], v._u32[1]); }
static u128 SPLW(u128 v, int i) { return V4(v._u32[i], v._u32[i], v._u32[i], v._u32[i]); }
static u128 ULANE(u128 v, int i, uint32_t x) { v._u32[i] = x; return v; }
static int op10_visible(J* j, uint32_t r)
{
#ifdef BC11_HOST_FLOAT
    const unsigned in = _mm_getcsr();
    _mm_setcsr(j->csr0);
#endif
    const u128 C0 = F4U(j->C.x[0]), C1 = F4U(j->C.x[1]), C2 = F4U(j->C.x[2]), C3 = F4U(j->C.x[3]);
    const u128 one = V4(F1, F1, F1, F1), m3 = V4(~0u, ~0u, ~0u, 0), sgn = V4(0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u);
    const u128 c1p = PRM(C1), c0p = PRM(C0), c2p = PRM(C2);
    const u128 r25 = spu_fm(C0, c1p), r13 = spu_fm(C2, c0p), r18 = spu_fm(C1, c2p);
    const u128 r78 = spu_xor(C3, sgn);
    const u128 r77 = spu_fnms(C1, c0p, r25), r76 = spu_fnms(C0, c2p, r13), r74 = spu_fnms(C2, c1p, r18);
    const u128 r92 = PRM(r77), r67 = PRM(r76), r71 = PRM(r74);
    const u128 r61 = spu_fm(r92, C2);
    /* the adjugate's rows, transposed out of the three cross products */
    const u128 r5  = V4(r71._u32[0], r67._u32[0], r92._u32[0], 0);
    const u128 r36 = V4(r71._u32[1], r67._u32[1], r92._u32[1], 0);
    const u128 r26 = V4(r71._u32[2], r67._u32[2], r92._u32[2], 0);
    const u128 det = spu_fa(spu_fa(r61, ROT1(r61)), ROT2(r61));
    u128 r27 = spu_fm(r5, SPLW(r78, 0));
    r27 = spu_fma(r36, SPLW(r78, 1), r27);
    r27 = spu_fma(r26, SPLW(r78, 2), r27);
    const u128 r32 = SPLW(det, 0);
    const u128 r31 = spu_fi(r32, spu_frest(r32));
    const u128 r28 = spu_fnms(r31, r32, one);
    const u128 inv = spu_and(spu_fma(r28, r31, r31), m3);
    const u128 r20 = spu_and(spu_fm(r36, inv), m3);
    const u128 r13b = ULANE(spu_fma(r27, inv, V4(0, 0, 0, F1)), 3, F1);
    const u128 r14 = spu_and(spu_fm(r5, inv), m3), r23 = spu_and(spu_fm(r26, inv), m3);
    const u128 r74b = V4(r14._u32[0], r20._u32[0], r23._u32[0], r13b._u32[0]);
    const u128 r92b = V4(r14._u32[1], r20._u32[1], r23._u32[1], r13b._u32[1]);
    const u128 r69 = V4(r14._u32[2], r20._u32[2], r23._u32[2], r13b._u32[2]);
    const uint32_t w8 = WORD(r, 8), w12 = WORD(r, 12), w16 = FW(j, r + 16, r), w20 = WORD(r, 20);
    u128 r58 = spu_fm(r74b, V4(w8, w8, w8, w8));
    r58 = spu_fma(r92b, V4(w12, w12, w12, w12), r58);
    r58 = spu_fma(r69, V4(w16, w16, w16, w16), r58);
    const u128 r66 = ULANE(C3, 3, F1);
    const u128 r64 = spu_fm(r66, spu_and(r58, m3));
    const u128 r61b = spu_fa(spu_fa(r64, ROT1(r64)), ROT2(r64));
    const u128 r80 = spu_fa(V4(w20, w20, w20, w20), SPLW(r61b, 0));
    const u128 r15 = ULANE(r58, 3, r80._u32[3]);
    const u128 r56 = spu_fm(r15, r15);
    const u128 r22 = SPLW(spu_fa(spu_fa(r56, ROT1(r56)), ROT2(r56)), 0);
    const u128 r19 = spu_fcgt(r22, V4(0x0DA2425Fu, 0x0DA2425Fu, 0x0DA2425Fu, 0x0DA2425Fu));
    const u128 r17 = spu_fi(r22, spu_frsqest(r22));
    const u128 r21 = spu_fm(r17, r17), r49 = spu_fm(r17, V4(FH, FH, FH, FH));
    const u128 r16 = spu_fnms(r22, r21, one);
    const u128 r48 = spu_fma(r16, r49, r17);
    const u128 r46 = spu_and(r19, spu_fm(r15, r48));
    const u128 r44 = spu_selb(r15, r46, r19);
    const u128 r38 = spu_fm(r44, F4U(j->cam));
    const u128 r30 = SPLW(spu_fa(spu_fa(r38, ROT1(r38)), ROT2(r38)), 0);
    const u128 r31b = spu_fs(r30, SPLW(r44, 3));
    const int behind = spu_fcgt(V4(0, 0, 0, 0), r31b)._u32[0] != 0;
#ifdef BC11_HOST_FLOAT
    _mm_setcsr(in);
#endif
    return !(behind && (FW(j, j->hdr + 0x30, 8) & 32));
}

/* ---------------- the walk (0x4358) ---------------- */
static __attribute__((noinline)) void walk(J* j)
{
    const uint32_t ls1 = j->ls1;
    j->gb_lo = ls1 + 0x4000u;
    memcpy(j->gbuf, j->ls + j->gb_lo, 0x8000);
    memcpy(j->out, j->ls + j->ls2, sizeof j->out);
    memset(j->out, 0, 0x180);         /* 0x4414: memset(buf2, 0, 0x180) */
    const uint32_t H = j->hdr;
    j->cam = LQ(j, H); j->h70 = LQ(j, H + 0x70);
    if (FBAD(j->cam, 7) || FBAD(j->h70, 15)) { j->fallback |= FB_FLOAT; return; }
    j->nplanes = FW(j, H + 0x30, 0);
    j->cur1 = FW(j, H + 0x40, 4); j->lim1 = FW(j, H + 0x40, 8); j->cur2 = FW(j, H + 0x40, 12); j->lim2 = FW(j, H + 0x50, 0);
    /* the cursors' lines must be guest memory: the MFC refuses an atomic on
     * anything else and the job spins, a CAS on vm_base would fault */
    if (!ea_ok(j->cur1 & ~127u, 128) || !ea_ok(j->cur2 & ~127u, 128)) {
        j->bad_ea = j->cur1; j->bad_size = 128; j->fallback |= FB_EA; return;
    }
    /* output header as 0x4434..0x450C leaves it: w0 = hdr+0x3C, w1 = hdr+0x40, byte 12 = hdr+0x80, bytes 14-15 = 0 */
    P32(j->out + 0x180, FW(j, H + 0x30, 12));
    P32(j->out + 0x184, FW(j, H + 0x40, 0));
    j->out[0x18C] = (uint8_t)(FW(j, H + 0x80, 13) & 255);
    j->out[0x18E] = j->out[0x18F] = 0;

    X4 I4;
    I4.x[0] = F4W(F1, 0, 0, 0); I4.x[1] = F4W(0, F1, 0, 0); I4.x[2] = F4W(0, 0, F1, 0); I4.x[3] = F4W(0, 0, 0, F1);
    j->slot[0].xf = j->slot[1].xf = I4;
    j->slot[0].lsbuf = ls1 + 0x4000u; j->slot[1].lsbuf = ls1 + 0x8000u;
    j->C = I4; j->ctx0 = j->ctx1 = 0;
    uint32_t lod = 7;
    uint32_t r = j->stream, end = j->stream + FW(j, H + 0x30, 4);
    j->stream_end = end;
    int cur = 1, pre = 0;
    for (;;) {
        while (end > r) {
            if (j->fallback) return;
            const uint32_t op = FW(j, r, r + 13) & 255;
#ifdef BC11_TRACE
            fprintf(stderr, "OP %05X %u lod=%u n=%u qn=%u\n", r, op, lod, j->n, j->qn);
#endif
            switch (op) {
            case 0: { const uint32_t h = HALF2(r); if (!h) { j->fallback |= FB_SKIP0; return; } r += h; break; }
            case 1: case 2: {
                F4 mn, mx; box_q(j, r + 4, &mn, &mx);
                const int vis = cull(j, mn, mx, &j->C);
                if (op == 1) r += vis ? 12 : 12 + (FW(j, r, r + 16) & 0xFFFF);
                else { if (vis) push(j, HALF2(r), WORD(r, 12), j->ctx0, j->ctx1, &j->C, lod); r += 16; }
                break;
            }
            case 3: {
                const int32_t x = (int16_t)(FW(j, r, r + 16) & 0xFFFF), y = (int16_t)(FW(j, r + 4, r + 18) & 0xFFFF),
                              z = (int16_t)(FW(j, r + 6, r + 20) & 0xFFFF);
                const F4 c = FMUL(FCVT(x, y, z, 0), F4W(0x40800000u, 0x40800000u, 0x40800000u, 0x40800000u));
                const uint32_t b = FW(j, r, r + 14) & 255;
                if (lod_in(j, b, lod, c, &j->C)) r += 12;
                else r += 12 + (FW(j, r + 8, r + 22) & 0xFFFF);
                break;
            }
            case 4: {   /* int16 center x4 (as op 3): in band, push child (+8 size, +12 EA, min lod) */
                const int32_t x = (int16_t)(FW(j, r + 2, r + 16) & 0xFFFF), y = (int16_t)(FW(j, r + 4, r + 18) & 0xFFFF),
                              z = (int16_t)(FW(j, r + 6, r + 20) & 0xFFFF);
                const F4 c = FMUL(FCVT(x, y, z, 0), F4W(0x40800000u, 0x40800000u, 0x40800000u, 0x40800000u));
                const uint32_t b = FW(j, r + 1, r + 14) & 255;
                if (lod_in(j, b, lod, c, &j->C)) {
                    const uint32_t l = lod & 255;
                    push(j, FW(j, r + 8, r + 22) & 0xFFFF, WORD(r, 12), j->ctx0, j->ctx1, &j->C, l > b ? b : l);
                }
                r += 16;
                break;
            }
            case 6:     /* op 5 behind a group bit (+22); clear: the far child */
                if (!mask_bit(j, FW(j, r + 22, r + 35) & 255)) {
                    push(j, FW(j, r + 20, r + 34) & 0xFFFF, FW(j, r + 16, r), j->ctx0, j->ctx1, &j->C, lod);
                    r += 24;
                    break;
                }
                /* fall through */
            case 5: {   /* LOD switch on an int16 center x4: near (+8 size, +12 EA, min lod) or far (+20 size, +16 EA) */
                const int32_t x = (int16_t)(FW(j, r + 2, r + 16) & 0xFFFF), y = (int16_t)(FW(j, r + 4, r + 18) & 0xFFFF),
                              z = (int16_t)(FW(j, r + 6, r + 20) & 0xFFFF);
                const F4 c = FMUL(FCVT(x, y, z, 0), F4W(0x40800000u, 0x40800000u, 0x40800000u, 0x40800000u));
                const uint32_t b = FW(j, r + 1, r + 14) & 255;
                if (lod_in(j, b, lod, c, &j->C)) {
                    const uint32_t l = lod & 255;
                    push(j, FW(j, r + 8, r + 22) & 0xFFFF, WORD(r, 12), j->ctx0, j->ctx1, &j->C, l > b ? b : l);
                } else
                    push(j, FW(j, r + 20, r + 34) & 0xFFFF, FW(j, r + 16, r), j->ctx0, j->ctx1, &j->C, lod);
                r += 24;
                break;
            }
            case 7: emit(j, r); r += 8; break;
            case 10:    /* plane facing test, then push child (+2 size, +4 EA) */
                if (op10_visible(j, r)) push(j, HALF2(r), WORD(r, 4), j->ctx0, j->ctx1, &j->C, lod);
                r += 24;
                break;
            case 15: {  /* push 512 bytes at +8 with lod 7 (EA 0: nothing) */
                const uint32_t ea = WORD(r, 8);
                if (ea) push(j, 512, ea, j->ctx0, j->ctx1, &j->C, 7);
                r += 12;
                break;
            }
            case 16:    /* push child (+2 size, +4 EA) with lod 7 */
                push(j, HALF2(r), WORD(r, 4), j->ctx0, j->ctx1, &j->C, 7);
                r += 8;
                break;
            case 19: {  /* sphere switch: int16 center x4, radius +8; inside: +12 EA / +20 size, else +16 EA / +22 size */
                const int32_t x = (int16_t)(FW(j, r + 2, r + 16) & 0xFFFF), y = (int16_t)(FW(j, r + 4, r + 18) & 0xFFFF),
                              z = (int16_t)(FW(j, r + 6, r + 20) & 0xFFFF);
                const F4 c = FMUL(FCVT(x, y, z, 0), F4W(0x40800000u, 0x40800000u, 0x40800000u, 0x40800000u));
                const uint32_t rw = WORD(r, 8);
                if (FBAD(F4W(rw, 0, 0, 0), 1)) { j->fallback |= FB_FLOAT; return; }
                if (sphere_in(j, rw, c, &j->C))
                    push(j, FW(j, r + 20, r + 34) & 0xFFFF, WORD(r, 12), j->ctx0, j->ctx1, &j->C, lod);
                else
                    push(j, FW(j, r + 22, r + 36) & 0xFFFF, FW(j, r + 16, r), j->ctx0, j->ctx1, &j->C, lod);
                r += 24;
                break;
            }
            case 8: case 9: r += FW(j, 0x6BE0 + 4 * op, 0x6BE0 + 4 * op); break;
            case 11: {
                /* instance: local 3x4 at +16..+63, composed with C (row-vector) */
                const F4 R0 = F4W(WORD(r, 16), WORD(r, 20), WORD(r, 24), 0), R1 = F4W(WORD(r, 28), WORD(r, 32), WORD(r, 36), 0),
                         R2 = F4W(WORD(r, 40), WORD(r, 44), WORD(r, 48), 0), T = F4W(WORD(r, 52), WORD(r, 56), WORD(r, 60), F1);
                if (FBAD(R0, 7) || FBAD(R1, 7) || FBAD(R2, 7) || FBAD(T, 7)) { j->fallback |= FB_FLOAT; return; }
                const X4* Cx = &j->C; X4 M;
                M.x[3] = FADD(FMA_(Cx->x[2], FSPL(T, 2), FMA_(Cx->x[1], FSPL(T, 1), FMUL(Cx->x[0], FSPL(T, 0)))), Cx->x[3]);
                M.x[0] = FMA_(Cx->x[2], FSPL(R0, 2), FMA_(Cx->x[1], FSPL(R0, 1), FMUL(Cx->x[0], FSPL(R0, 0))));
                M.x[1] = FMA_(Cx->x[2], FSPL(R1, 2), FMA_(Cx->x[1], FSPL(R1, 1), FMUL(Cx->x[0], FSPL(R1, 0))));
                M.x[2] = FMA_(Cx->x[2], FSPL(R2, 2), FMA_(Cx->x[1], FSPL(R2, 1), FMUL(Cx->x[0], FSPL(R2, 0))));
                const uint32_t f = FW(j, r, r + 14) & 255;
                int vis = 1;
                if (f) { F4 mn, mx; box_q(j, r + 64, &mn, &mx); vis = cull(j, mn, mx, &M); }   /* byte 1 set: cull first */
                if (vis) push(j, FW(j, r, r + 16) & 0xFFFF, WORD(r, 4), FW(j, r + 8, r + 8), FW(j, r + 12, r + 12), &M, lod);
                r += 72;
                break;
            }
            case 12: {
                const F4 c = F4W(WORD(r, 8), WORD(r, 12), WORD(r, 16), WORD(r, 16));
                if (FBAD(c, 7)) { j->fallback |= FB_FLOAT; return; }
                const uint32_t b = BYTE1(r);
                if (lod_in(j, b, lod, c, &j->C)) {
                    const uint32_t l = lod & 255;
                    push(j, HALF2(r), WORD(r, 4), j->ctx0, j->ctx1, &j->C, l > b ? b : l);
                }
                r += 20;
                break;
            }
            case 14: {
                if (!mask_bit(j, FW(j, r + 26, r + 39) & 255)) {      /* group off: the far child */
                    push(j, FW(j, r + 24, r + 38) & 0xFFFF, WORD(r, 20), j->ctx0, j->ctx1, &j->C, lod);
                    r += 28;
                    break;
                }
            }   /* fall through */
            case 13: {
                const F4 c = F4W(WORD(r, 8), WORD(r, 12), WORD(r, 16), WORD(r, 16));
                if (FBAD(c, 7)) { j->fallback |= FB_FLOAT; return; }
                const uint32_t b = BYTE1(r);
                if (lod_in(j, b, lod, c, &j->C)) {
                    const uint32_t l = lod & 255;
                    push(j, HALF2(r), WORD(r, 4), j->ctx0, j->ctx1, &j->C, l > b ? b : l);
                } else
                    push(j, FW(j, r + 24, r + 38) & 0xFFFF, WORD(r, 20), j->ctx0, j->ctx1, &j->C, lod);
                r += 28;
                break;
            }
            case 17: {
                const F4 c = F4W(WORD(r, 4), WORD(r, 8), WORD(r, 12), WORD(r, 12));
                if (FBAD(c, 7)) { j->fallback |= FB_FLOAT; return; }
                const uint32_t b = FW(j, r, r + 14) & 255;
                if (lod_in(j, b, lod, c, &j->C)) r += 16;
                else r += 16 + (FW(j, r, r + 16) & 0xFFFF);
                break;
            }
            case 18: {
                const uint32_t ea = WORD(r, 4);
                if (ea) {
                    const uint32_t a = BYTE1(r), b = FW(j, r + 3, r + 16) & 255;
                    push(j, a * b + 8, ea, j->ctx0, j->ctx1, &j->C, 7);
                }
                r += 8;
                break;
            }
            case 20: r += 8; break;
            default: j->fallback |= FB_OP; j->bad_op = op; return;     /* > 20: a table skip into .bss, not modelled */
            }
        }
        if (j->fallback) return;
        /* 0x45D8: the stream is done; take the prefetched one, or pop now */
        if (!pre && !pop(j, cur)) break;
        const int other = 1 - cur;
        pre = pop(j, other);
        if (j->fallback) return;
        j->C = j->slot[cur].xf; j->ctx0 = j->slot[cur].ctx0; j->ctx1 = j->slot[cur].ctx1;
        r = j->slot[cur].lscur; end = r + j->slot[cur].size; lod = j->slot[cur].lod;
        cur = other;
    }
    close_batch(j);
}

/* The walk under the SPU's float rules (see the float layer): round toward
 * zero, FTZ, DAZ; an overflow or invalid operation it raised falls back. */
static void run_model(J* j)
{
#ifdef BC11_HOST_FLOAT
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
    uint32_t line = 0;                  /* the last atomic's line: the job leaves it at LS 0x80 */
    for (uint32_t k = 0; k < j->nbatch; k++) {
        Batch* b = &j->batch[k];
        const uint32_t hea = guest_atomic_add(j, j->cur1, 512);
        line = j->cur1;
        if (hea == 0 || hea + 512 > j->lim1) {                     /* dropped, as the job drops it */
            memcpy(j->out + 0x190, b->hdr + 0x10, 496);
            continue;
        }
        uint32_t lea = guest_atomic_add(j, j->cur2, b->list_size);
        line = j->cur2;
        if (lea + b->list_size > j->lim2) lea = 0;
        if (!lea) memset(b->hdr + 0x10, 0, 496);
        P32(b->hdr + 8, lea);
        if (ea_ok(hea, 512)) guest_put(j, hea, b->hdr, 512);
        memcpy(j->out + 0x180, b->hdr, 512);
        if (lea) {
            uint32_t left = b->list_size, o = 0, e = 0;
            while (left) {
                const uint32_t c = left > 16384 ? 16384 : left;
                P32(j->out + 0xE0 + e, c); P32(j->out + 0xE4 + e, lea + o);
                if (ea_ok(lea + o, c)) guest_put(j, lea + o, b->list + o, c);
                o += c; left -= c; e += 8;
            }
        }
    }
    /* LS: the output buffer, the last batch's items in the scratch, the stream buffers */
    /* buf2+0: the worker's profiling-timer accumulator (0x4C08: += elapsed
     * decrementer ticks after the memset); nothing reads it, an estimate */
    P32(j->out, j->ticks);
    memcpy(ctx->ls + j->ls2, j->out, sizeof j->out);
    if (line) memcpy(ctx->ls + 0x80, vm_base + (line & ~127u), 128);
    for (uint32_t i = 0; i < j->maxn; i++) {
        uint8_t* it = ctx->ls + j->ls3 + 16 * i;
        it[0] = j->ib[i]; P32(it + 8, j->iw2[i]); P32(it + 12, j->iw3[i]);
    }
    if (j->used_gbuf) memcpy(ctx->ls + j->gb_lo, j->gbuf, 0x8000);
}

static int job_init(J* j, spu_context* ctx)
{
    j->fallback = 0; j->bad_ea = j->bad_size = 0; j->bad_op = 0;
    j->qn = 0; j->n = j->maxn = 0; j->nbatch = 0; j->items_total = 0; j->used_gbuf = 0; j->ticks = 0;
    j->ctx = ctx; j->ls = ctx->ls;
    j->hdr = ctx->gpr[3]._u32[0] & 0x3FFFFu; j->planes = ctx->gpr[4]._u32[0] & 0x3FFFFu;
    j->stream = ctx->gpr[5]._u32[0] & 0x3FFFFu; j->ls3 = ctx->gpr[6]._u32[0] & 0x3FFFFu;
    j->ls2 = ctx->gpr[7]._u32[0] & 0x3FFFFu; j->ls1 = ctx->gpr[9]._u32[0] & 0x3FFFFu;
    if ((j->ls1 | j->ls2 | j->ls3 | j->hdr) & 15 || j->ls1 + 0xC000u > 0x40000u || j->ls2 + (uint32_t)sizeof j->out > 0x40000u ||
        j->ls3 + 16u * BC11_ITEMS > 0x40000u || j->hdr + 0x90u > 0x40000u) {
        j->fallback |= FB_LS; return 0;
    }
    return 1;
}

/* After the walk: the LS the job wrote must not overlap what it read or the
 * module, or the SPU would have seen its own writes where the model did not. */
static int ovl(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) { return a0 < b1 && b0 < a1 && a0 < a1 && b0 < b1; }
static void check_layout(J* j)
{
    uint32_t maxlist = 0;
    for (uint32_t k = 0; k < j->nbatch; k++) if (j->batch[k].list_size > maxlist) maxlist = j->batch[k].list_size;
    const uint32_t w[3][2] = { { j->used_gbuf ? j->gb_lo : 0, j->used_gbuf ? j->gb_lo + 0x8000u : 0 },
                               { j->ls2, j->ls2 + 0x380u + maxlist },
                               { j->ls3, j->ls3 + 16u * j->maxn } };
    const uint32_t r[4][2] = { { 0x4000u, 0xE620u }, { j->hdr, j->hdr + 0x90u },
                               { j->planes, j->planes + 32u * (j->nplanes > 4 ? j->nplanes : 5) },
                               { j->stream, j->stream_end } };
    for (int a = 0; a < 3; a++) {
        for (int b = 0; b < 4; b++) if (ovl(w[a][0], w[a][1], r[b][0], r[b][1]) && !(a == 0 && b == 3)) j->fallback |= FB_LS;
        for (int b = a + 1; b < 3; b++) if (ovl(w[a][0], w[a][1], w[b][0], w[b][1])) j->fallback |= FB_LS;
    }
}

#ifndef BC11_NO_RUNTIME
static _Thread_local J* t_j;
static _Thread_local int t_bypass;
static int s_mode = -1;
static unsigned long long s_native, s_fallback, s_cmp, s_bad, s_ns_nat, s_ns_spu, s_items;
static unsigned s_reason[16];
/* PS3RECOMP_BC11_OPHIST=1: which op sent each FB_OP job back to the SPU,
 * printed with the 10 s stats line. Off: nothing is counted. */
static int s_ophist = -1;
static unsigned s_ops[256];
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
    char why[200] = ""; int w = 0;
    for (int i = 0; i < 16; i++)
        if (s_reason[i]) w += snprintf(why + w, sizeof why - w, " %X:%u", 1u << i, s_reason[i]);
    fprintf(stderr, "[bc11] native %llu, SPU %llu (reasons%s), items %llu%s", s_native, s_fallback, w ? why : " none",
            s_items, s_mode == 2 ? "" : "\n");
    if (s_mode == 2) fprintf(stderr, ", compared %llu, differ %llu; per compared job native %.1f us, SPU %.1f us\n",
                             s_cmp, s_bad, s_cmp ? s_ns_nat / 1e3 / s_cmp : 0.0, s_cmp ? s_ns_spu / 1e3 / s_cmp : 0.0);
    if (s_ophist > 0) {
        char ops[600] = ""; int k = 0;
        for (int i = 0; i < 256 && k < (int)sizeof ops - 16; i++)
            if (s_ops[i]) k += snprintf(ops + k, sizeof ops - k, " op%d:%u", i, s_ops[i]);
        fprintf(stderr, "[bc11] FB_OP by op:%s\n", k ? ops : " none");
    }
}

/* Mode 2: what the SPU worker left against the model. buf2 (from +4: +0 is
 * the timer), header minus the list EA (the cursors differ), bucket offsets
 * when the SPU's list EA is set, the last batch's list with its padding; the
 * item scratch (bytes 0 and 8..15 of each item); the stream buffers. The
 * PUTs themselves are not compared: their addresses come from the cursors. */
static int compare(J* j, const uint8_t* ls)
{
    const uint8_t* o = ls + j->ls2;
    uint8_t lo[0x180];
    memcpy(lo, j->out, 0x180);
    if (j->nbatch && R32(o + 0x188)) {   /* the last list PUT's elements, at the SPU's list EA */
        const uint32_t lea = R32(o + 0x188);
        for (uint32_t off = 0, e = 0; off < j->batch[j->nbatch - 1].list_size; off += 16384, e += 8) {
            const uint32_t left = j->batch[j->nbatch - 1].list_size - off;
            P32(lo + 0xE0 + e, left > 16384 ? 16384 : left); P32(lo + 0xE4 + e, lea + off);
        }
    }
    if (memcmp(o + 4, lo + 4, 0x180 - 4)) return 1;
    const uint8_t* h = j->nbatch ? j->batch[j->nbatch - 1].hdr : j->out + 0x180;
    if (memcmp(o + 0x180, h, 8) || memcmp(o + 0x18C, h + 12, 4)) return 2;
    if (j->nbatch) {
        const Batch* b = &j->batch[j->nbatch - 1];
        if (R32(o + 0x188) && memcmp(o + 0x190, b->hdr + 0x10, 496)) return 3;
        if (memcmp(o + 0x380, b->list, b->list_size)) return 4;
    }
    for (uint32_t i = 0; i < j->maxn; i++) {
        const uint8_t* it = ls + j->ls3 + 16 * i;
        if (it[0] != j->ib[i] || R32(it + 8) != j->iw2[i] || R32(it + 12) != j->iw3[i]) return 5;
    }
    if (j->used_gbuf && memcmp(ls + j->gb_lo, j->gbuf, 0x8000)) return 6;
    return 0;
}

static int bc11_worker(spu_context* ctx)
{
    if (t_bypass) return 0;
    if (__builtin_expect(s_mode < 0, 0)) {
        const char* e = getenv("PS3RECOMP_NATIVE_BC11");
        s_mode = e ? atoi(e) : 1;
        fprintf(stderr, "[bc11] native scene walker: %s\n", s_mode == 2 ? "compare with the SPU job" : s_mode ? "on" : "off");
        const char* h = getenv("PS3RECOMP_BC11_OPHIST");
        s_ophist = h && atoi(h) > 0;
    }
    if (!s_mode) return 0;
    if (!t_j) { t_j = (J*)calloc(1, sizeof(J)); if (!t_j) return 0; }
    J* j = t_j;
    stats();
    const uint64_t t0 = now_ns();
    if (job_init(j, ctx)) { run_model(j); if (!j->fallback) check_layout(j); }
    const uint64_t t1 = now_ns();
    j->ticks = (uint32_t)((t1 - t0) * 798 / 10000);   /* 79.8 MHz timebase */
    if (j->fallback) {
        __atomic_fetch_add(&s_fallback, 1, __ATOMIC_RELAXED);
        for (int i = 0; i < 16; i++) if (j->fallback >> i & 1) __atomic_fetch_add(&s_reason[i], 1, __ATOMIC_RELAXED);
        if (s_ophist > 0 && (j->fallback & FB_OP)) __atomic_fetch_add(&s_ops[j->bad_op & 255], 1, __ATOMIC_RELAXED);
        if (j->fallback & FB_EA) { static int nl;
            if (nl++ < 6) fprintf(stderr, "[bc11] EA fallback: 0x%08X size 0x%X\n", j->bad_ea, j->bad_size); }
        if (j->fallback & FB_LS) { static int nl;
            if (nl++ < 6) fprintf(stderr, "[bc11] LS layout fallback: hdr %05X ls1 %05X ls2 %05X ls3 %05X\n",
                                  j->hdr, j->ls1, j->ls2, j->ls3); }
        return 0;
    }
    if (s_mode == 2) {
        void (*fn)(spu_context*) = spu_lookup(BC11_WORKER, ctx->resident_ovl);
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
        if (bad) {
            unsigned long long nb = __atomic_add_fetch(&s_bad, 1, __ATOMIC_RELAXED);
            if (nb <= 8) fprintf(stderr, "[bc11] job differs (%d): %u batches, %u items; ls1 %05X ls2 %05X ls3 %05X\n",
                                 bad, j->nbatch, j->items_total, j->ls1, j->ls2, j->ls3);
        }
        ctx->pc = ctx->gpr[0]._u32[0];
        return 1;
    }
    publish(j);
    __atomic_fetch_add(&s_native, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_items, j->items_total, __ATOMIC_RELAXED);
    return 1;
}

__attribute__((constructor)) static void bc11_register(void)
{
    spu_native_register(BC11_IMAGE, BC11_WORKER, bc11_worker);
}
#endif
