/* inFAMOUS 2 (BCUS98125): the BSP polygon clipper of the immediate-mode
 * geometry job, natively.
 *
 * SPU job module d7096a8ffc1c (overlay 220, Render4) turns objects made of
 * convex polygons into RSX command buffers of inline vertices, clipping them
 * through a BSP tree it builds in local store as it goes. About 2/3 of the
 * job's time is the function at LS 0x68D8 and the three it calls:
 *
 *   0x68D8  push a list of polygons through the tree: at each node split the
 *           list by the node's plane, keep the negative side for child 0
 *           (on a stack) and descend into child 1 with the positive side; at
 *           a missing child 0 the negative side is dropped (0x5A98), at a
 *           missing child 1 the positive side becomes new nodes (0x4A88) and
 *           the node links to them;
 *   0x60A0  split a polygon list by a plane into two lists;
 *   0x5A98  the same, keeping only the positive side;
 *   0x4A88  append the planes of a list as nodes; sets the overflow flag
 *           when the node table is full.
 *
 * This replaces 0x68D8 (written from rpi/job220-native/RESEARCH.md, checked
 * against the SPU code on the interpreter: work/runs/job220/oracle). Pure
 * local store: everything the four functions store, in the same places, the
 * stack frames included, and the registers a caller can see as they leave
 * them (r0, r1, r80-r127; no caller reads r3). Lists and polygons are
 * allocated downwards from a pointer at S+0x5C (S = r3) with no bound, as in
 * the SPU code. Polygons: +0x00 plane, +0x10 word 0 flags / word 1 vertex
 * count, +0x20 one quad per vertex pointer. Lists: word 0 count, then one
 * quad per entry. Nodes, 32 bytes at S+0x34: plane, then the child offsets
 * (in nodes) for the negative and the positive side.
 *
 * The SPU's single precision is host SSE under round-toward-zero, FTZ and
 * DAZ, with every zero made +0; any result at or above FLT_MAX (an operand
 * with exponent 255, or an overflow, where x86 and the SPU differ) abandons
 * the run. Abandoned or unusual runs (misaligned pointers, a polygon of no
 * vertices, a runaway walk) undo their stores and the SPU code runs.
 *
 * The lift calls spu_native_entry at 0x68D8 (tools/lift_spu.py,
 * NATIVE_ENTRIES).
 *
 * PS3RECOMP_NATIVE_J220: 1 (default) on; 0 off; 2 runs the native code on a
 * copy of the local store and registers, then the lifted function, and
 * compares the local store and r0, r1, r80-r127, logging any difference
 * (PS3RECOMP_J220_DUMP=dir also writes the state the call started from:
 * 128 registers, then the local store). The 10 s stats line counts calls,
 * fallbacks and differences.
 *
 * Built like the lifted SPU sources (x86-64-v4). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <immintrin.h>

#include "spu_context.h"
#include "spu_helpers.h"

#if !defined(__SSE4_1__) || !defined(__FMA__)
#error "job220_native.c needs SSE4.1 and FMA (the SPU sources' -march, x86-64-v3 or v4)"
#endif
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

#define J220_IMAGE 220
#define J220_BSP   0x68D8u
#define LSQ        0x3FFF0u
#define CSR_FAST   0xFFC0u               /* RZ | FZ | DAZ, all exceptions masked */
#define NOINL      __attribute__((noinline))

typedef __m128i V;                       /* a quad: lane i = SPU word i */

/* Copy-on-first-write journal of local-store quads, to undo an abandoned run */
typedef struct {
    uint64_t bm[SPU_LS_SIZE / 16 / 64];
    uint32_t n;
    uint16_t list[SPU_LS_SIZE / 16];
    uint8_t save[SPU_LS_SIZE];
} Journal;

typedef struct {
    uint8_t* ls;
    Journal* j;
    int bad;                             /* abandon: 1 float, 2 layout, 3 runaway */
} B;

#define BSW _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)
static inline V LQ(const B* b, uint32_t a) { return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(b->ls + (a & LSQ))), BSW); }
static inline void SQ(B* b, uint32_t a, V v)
{
    const uint32_t q = a & LSQ, i = q >> 4;
    Journal* j = b->j;
    if (!(j->bm[i >> 6] >> (i & 63) & 1)) {
        j->bm[i >> 6] |= 1ull << (i & 63);
        j->list[j->n++] = (uint16_t)i;
        memcpy(j->save + q, b->ls + q, 16);
    }
    _mm_storeu_si128((__m128i*)(b->ls + q), _mm_shuffle_epi8(v, BSW));
}
static void j_undo(Journal* j, uint8_t* ls)
{
    for (uint32_t k = j->n; k-- > 0;) {
        const uint32_t i = j->list[k];
        memcpy(ls + 16 * i, j->save + 16 * i, 16);
        j->bm[i >> 6] = 0;
    }
    j->n = 0;
}
static void j_clear(Journal* j)
{
    for (uint32_t k = 0; k < j->n; k++) j->bm[j->list[k] >> 6] = 0;
    j->n = 0;
}

#define W(v, i)      ((uint32_t)_mm_extract_epi32((v), (i)))
#define SETW(v, i, x) _mm_insert_epi32((v), (int)(x), (i))
#define ROT1(v)      _mm_shuffle_epi32((v), _MM_SHUFFLE(0, 3, 2, 1))   /* rotqbyi 4 */
#define ROT2(v)      _mm_shuffle_epi32((v), _MM_SHUFFLE(1, 0, 3, 2))   /* rotqbyi 8 */
#define ROT3(v)      _mm_shuffle_epi32((v), _MM_SHUFFLE(2, 1, 0, 3))   /* rotqbyi 12 */
#define SPLAT(v, i)  _mm_shuffle_epi32((v), _MM_SHUFFLE(i, i, i, i))
#define ADDI(v, k)   _mm_add_epi32((v), _mm_set1_epi32((int)(k)))
static inline V splat(uint32_t x) { return _mm_set1_epi32((int)x); }

/* replace word i of the quad at a (cwd + shufb + stqd) */
static inline void SW(B* b, uint32_t a, int i, uint32_t x)
{
    V q = LQ(b, a);
    switch (i) { case 0: q = SETW(q, 0, x); break; case 1: q = SETW(q, 1, x); break;
                 case 2: q = SETW(q, 2, x); break; default: q = SETW(q, 3, x); break; }
    SQ(b, a, q);
}

/* ------------------------------------------------------------------ SPU single precision */
#define FV(v) _mm_castsi128_ps(v)
#define IV(v) _mm_castps_si128(v)
/* The arithmetic is plain SSE: with MXCSR at CSR_FAST, x86 gives the SPU's
 * result unless an operand has exponent 255 (a number to the SPU, Inf or NaN
 * to x86) or the result overflows (the SPU goes on into exponent 255). Loads
 * that feed arithmetic are checked for exponent 255 (e255), and the run is
 * abandoned on the overflow flag (j220_bsp_run). A zero may come out -0,
 * which the SPU never gives: canon() where a sign could be seen. */
static inline __m128 fs(V x, V y) { return _mm_sub_ps(FV(x), FV(y)); }
static inline __m128 fm(V x, V y) { return _mm_mul_ps(FV(x), FV(y)); }
static inline __m128 fma_(V x, V y, V z) { return _mm_fmadd_ps(FV(x), FV(y), FV(z)); }
static inline __m128 fnms(V x, V y, V z) { return _mm_fnmadd_ps(FV(x), FV(y), FV(z)); }
static inline V canon(__m128 r)
{
    const V v = IV(r);
    return _mm_andnot_si128(_mm_cmpeq_epi32(_mm_and_si128(v, splat(0x7F800000u)), _mm_setzero_si128()), v);
}
/* exponent 255 in a lane of lm */
static inline void e255(B* b, V v, int lm)
{
    const V e = _mm_cmpeq_epi32(_mm_and_si128(v, splat(0x7F800000u)), splat(0x7F800000u));
    if (__builtin_expect(_mm_movemask_ps(FV(e)) & lm, 0)) b->bad = 1;
}
#define XYZ 7
#define ALL 15
/* fcgt on two values that are results or constants (no exponent 0 but +0, no 255) */
static inline int fgt0(V x, V y) { return _mm_comigt_ss(FV(x), FV(y)); }
static inline V vabs(V x) { return _mm_and_si128(x, splat(0x7FFFFFFFu)); }
static inline V frest_fi(V x)
{
    u128 a, e; memcpy(&a, &x, 16);
    e = spu_frest(a);
    const u128 r = spu_fi(a, e);
    V o; memcpy(&o, &r, 16); return o;
}
/* cuflt(cfltu(x, 141), 123): x scaled to 32-bit fixed point and back */
static inline uint32_t fix32(uint32_t x)
{
    const uint32_t e = x >> 23 & 0xFF;
    if ((x & 0x80000000u) || e == 0) return 0;
    uint32_t u;
    if (e >= 127) u = 0xFFFFFFFFu;
    else {
        const uint32_t m = (x & 0x7FFFFF) | 0x800000;
        u = e >= 118 ? m << (e - 118) : (118 - e >= 32 ? 0 : m >> (118 - e));
    }
    if (!u) return 0;
    const int n = 31 - __builtin_clz(u);
    const uint32_t mant = n >= 23 ? u >> (n - 23) : u << (23 - n);
    return (uint32_t)(n + 127 - 32) << 23 | (mant & 0x7FFFFF);
}
static inline V vfix32(V x)
{
    return _mm_setr_epi32((int)fix32(W(x, 0)), (int)fix32(W(x, 1)), (int)fix32(W(x, 2)), (int)fix32(W(x, 3)));
}

/* (x*a + y*b) + z*c of two quads, as the SPU sums the products (lane 0; the
 * w lanes are left out, so they raise nothing) */
static inline V dot3(V x, V y)
{
    const __m128 p = fm(_mm_blend_epi32(x, _mm_setzero_si128(), 8), _mm_blend_epi32(y, _mm_setzero_si128(), 8));
    return IV(_mm_add_ss(_mm_add_ss(p, _mm_movehdup_ps(p)), _mm_movehl_ps(p, p)));
}
/* the plane's signed distance of v (lane 0); v's x, y, z checked */
static inline V pdist(B* b, V pl, V v, V dsp)
{
    e255(b, v, XYZ);
    return IV(_mm_sub_ss(FV(dot3(pl, v)), FV(dsp)));
}

/* ------------------------------------------------------------------ the four functions */
/* append entry quad e to the list at l (word 0 = count) */
static inline void l_append(B* b, uint32_t l, V e)
{
    const V h = LQ(b, l);
    const uint32_t c = W(h, 0);
    SQ(b, l, SETW(h, 0, c + 1));
    SQ(b, c * 16 + l + 16, e);
}

/* the bytes of a polygon of c vertices, lane-wise (the SPU takes c from the
 * polygon's header quad, whose other words go along in the other lanes) */
static inline V PSIZE(V c) { return ADDI(_mm_slli_epi32(ADDI(c, 2), 4), 32); }

/* copy n vertex-pointer quads */
static inline void qcopy(B* b, uint32_t dst, uint32_t src, int32_t n)
{
    for (int32_t k = 0; k < n; k++) SQ(b, dst + 16 * k, LQ(b, src + 16 * k));
}

/* Where the edges A -> C and B -> D cross the plane: the SPU computes the
 * four distances (C, A, D, B) in one quad, both parameters with one
 * reciprocal, and quantizes them to 32-bit fixed point (cfltu/cuflt). */
static inline void crossings(B* b, V pl, V A, V Bv, V C, V D, V* nv1, V* nv2)
{
    const V nx = SPLAT(pl, 0), ny = SPLAT(pl, 1), nz = SPLAT(pl, 2), dsp = SPLAT(pl, 3), one = splat(0x3F800000u);
    e255(b, A, ALL); e255(b, Bv, ALL); e255(b, C, ALL); e255(b, D, ALL);
    const V CA = IV(fs(C, A)), DB = IV(fs(D, Bv));
    const V X = _mm_setr_epi32((int)W(C, 0), (int)W(A, 0), (int)W(D, 0), (int)W(Bv, 0));
    const V Y = _mm_setr_epi32((int)W(C, 1), (int)W(A, 1), (int)W(D, 1), (int)W(Bv, 1));
    const V Z = _mm_setr_epi32((int)W(C, 2), (int)W(A, 2), (int)W(D, 2), (int)W(Bv, 2));
    const V dd = canon(fs(IV(fma_(Z, nz, IV(fma_(X, nx, IV(fm(Y, ny)))))), dsp));
    const V dr = ROT1(dd);
    /* lanes 1 and 3 are not used: 1.0 there, so they raise nothing */
    const V den = _mm_blend_epi32(canon(fs(dr, dd)), one, 0xA);
    const V e = frest_fi(den);
    e255(b, e, ALL);
    const V rcp = IV(fma_(IV(fnms(e, den, one)), e, e));
    const V t = vfix32(IV(fm(rcp, dr)));
    *nv1 = canon(fma_(SPLAT(t, 0), CA, A));
    *nv2 = canon(fma_(SPLAT(t, 2), DB, Bv));
}

/* 0x60A0 (both = 1: negative side to list ln, positive to lp) and 0x5A98
 * (both = 0: positive side only). sp, r1: 0x68D8's stack pointer and r1. */
static void split(B* b, int both, uint32_t S, V pl, uint32_t L, uint32_t ln, uint32_t lp, V r1)
{
    const uint32_t sp = W(r1, 0);
    SQ(b, sp - 0x50, r1);
    const int32_t cnt = (int32_t)W(LQ(b, L), 0);
    if (cnt <= 0) return;
    const V dsp = SPLAT(pl, 3);
    e255(b, pl, ALL);
    const V c99 = splat(0x3F7FBE76u), c01 = splat(0x3DCCCCCCu);
    uint32_t ea = L + 16;
    for (int32_t k = 0; k < cnt && !b->bad; k++, ea += 16) {
        const V E = LQ(b, ea);
        const uint32_t p = W(E, 0);
        if (p & 15) { b->bad = 2; return; }
        /* coplanar: parallel planes and vertex 0 within 0.1 */
        const V pp = LQ(b, p);
        const V v0 = LQ(b, W(LQ(b, p + 0x20), 0));
        e255(b, pp, XYZ);
        const V dn = dot3(pl, pp);
        const V d0 = pdist(b, pl, v0, dsp);
        if (fgt0(vabs(dn), c99) && fgt0(c01, vabs(d0))) {
            l_append(b, lp, E);
            if (both) l_append(b, ln, E);
            continue;
        }
        const int32_t n = (int32_t)W(LQ(b, p + 0x10), 1);
        if (n <= 0 || n > 4096) { b->bad = 2; return; }
        const uint32_t vb = p + 0x20;
        int32_t slot[3] = { -1, -1, -1 };
        int prev = fgt0(_mm_setzero_si128(), pdist(b, pl, LQ(b, W(LQ(b, vb + 16 * (n - 1)), 0)), dsp));
        int cur = prev;
        for (int32_t i = 0; i < n; i++) {
            cur = fgt0(_mm_setzero_si128(), pdist(b, pl, LQ(b, W(LQ(b, vb + 16 * i), 0)), dsp));
            slot[cur + (prev ^ 1)] = i;
            prev = cur;
        }
        SQ(b, sp - 0x30, splat((uint32_t)slot[0]));
        SQ(b, sp - 0x20, splat((uint32_t)slot[1]));
        SQ(b, sp - 0x10, splat((uint32_t)slot[2]));
        if (b->bad) return;
        const int32_t s0 = slot[0], s2 = slot[2];
        if (s0 < 0) {                                    /* all on one side */
            if (!cur) l_append(b, lp, E);
            else if (both) l_append(b, ln, E);
            continue;
        }
        /* the two crossing edges: A -> C enters the positive side at s0, B -> D leaves it at s2 */
        const int32_t a0 = (s0 + n - 1) % n, a2 = (s2 + n - 1) % n;
        const V sq = LQ(b, S + 0x50);
        const V t32 = ADDI(ROT3(sq), -32), t16 = ADDI(ROT3(sq), -16);
        if (W(t32, 0) & 15) { b->bad = 2; return; }
        SQ(b, S + 0x50, SETW(sq, 3, W(t32, 0)));
        const uint32_t ea0 = W(LQ(b, vb + 16 * a0), 0), ea2 = W(LQ(b, vb + 16 * a2), 0);
        const uint32_t ec = W(LQ(b, vb + 16 * s0), 0), ed = W(LQ(b, vb + 16 * s2), 0);
        const V A = LQ(b, ea0), Bv = LQ(b, ea2), C = LQ(b, ec), D = LQ(b, ed);
        V nv1, nv2;
        crossings(b, pl, A, Bv, C, D, &nv1, &nv2);
        if (b->bad) return;
        SQ(b, W(t32, 0), nv1);
        SQ(b, W(t16, 0), nv2);
        V P1, P2 = _mm_setzero_si128();
        if (s2 > s0) {                                   /* positive run s0..s2-1 */
            const int32_t c1 = s2 - s0;
            P1 = ADDI(t32, -(32 + 16 * (c1 + 2)));
            const uint32_t q1 = W(P1, 0);
            const V h = LQ(b, p + 0x10);
            SQ(b, S + 0x50, SETW(LQ(b, S + 0x50), 3, q1));
            SW(b, q1 + 0x10, 0, W(h, 0));
            SW(b, q1 + 0x10, 1, (uint32_t)(c1 + 2));
            qcopy(b, q1 + 0x20, vb + 16 * s0, c1);
            SQ(b, q1 + 16 * c1 + 0x30, t32);
            SQ(b, q1 + 16 * c1 + 0x20, t16);
            if (both) {                                  /* negative: 0..s0-1, new1, new2, s2..n-1 */
                const int32_t c2 = n - c1;
                const V h2 = LQ(b, p + 0x10), sq3 = LQ(b, S + 0x50);
                P2 = _mm_sub_epi32(ROT3(sq3), PSIZE(ADDI(ROT1(h2), -c1)));
                const uint32_t q2 = W(P2, 0);
                SQ(b, S + 0x50, SETW(sq3, 3, q2));
                SW(b, q2 + 0x10, 0, W(h2, 0));
                SW(b, q2 + 0x10, 1, (uint32_t)(c2 + 2));
                qcopy(b, q2 + 0x20, vb, s0);
                qcopy(b, q2 + 0x20 + 16 * (s0 + 2), vb + 16 * s2, n - s2);
                SQ(b, q2 + 16 * s0 + 0x30, t16);
                SQ(b, q2 + 16 * s0 + 0x20, t32);
            }
        } else {                                         /* positive run s0..n-1, 0..s2-1 */
            const int32_t c1 = n - (s0 - s2);
            const V h = LQ(b, p + 0x10);
            P1 = _mm_sub_epi32(t32, PSIZE(ADDI(ROT1(h), -(s0 - s2))));
            const uint32_t q1 = W(P1, 0);
            SQ(b, S + 0x50, SETW(LQ(b, S + 0x50), 3, q1));
            SW(b, q1 + 0x10, 0, W(h, 0));
            SW(b, q1 + 0x10, 1, (uint32_t)(c1 + 2));
            qcopy(b, q1 + 0x20, vb, s2);
            qcopy(b, q1 + 0x20 + 16 * (s2 + 2), vb + 16 * s0, n - s0);
            if (both) {
                SQ(b, q1 + 16 * s2 + 0x30, t32);
                SQ(b, q1 + 16 * s2 + 0x20, t16);
                const int32_t c2 = s0 - s2;              /* negative: s2..s0-1, new1, new2 */
                const V sq3 = LQ(b, S + 0x50), h2 = LQ(b, p + 0x10);
                P2 = ADDI(ROT3(sq3), -(32 + 16 * (c2 + 2)));
                const uint32_t q2 = W(P2, 0);
                SQ(b, S + 0x50, SETW(sq3, 3, q2));
                SW(b, q2 + 0x10, 0, W(h2, 0));
                SW(b, q2 + 0x10, 1, (uint32_t)(c2 + 2));
                qcopy(b, q2 + 0x20, vb + 16 * s2, c2);
                SQ(b, q2 + 16 * c2 + 0x30, t16);
                SQ(b, q2 + 16 * c2 + 0x20, t32);
            } else {
                SQ(b, q1 + 16 * s2 + 0x20, t16);
                SQ(b, q1 + 16 * s2 + 0x30, t32);
            }
        }
        SQ(b, W(P1, 0), LQ(b, p));
        l_append(b, lp, P1);
        if (both) {
            SQ(b, W(P2, 0), LQ(b, p));
            l_append(b, ln, P2);
        }
    }
}

/* 0x4A88: the planes of list l as nodes, last entry first; word 0 of each
 * node's second quad is 0 for the last one. 0: the table is full (the
 * overflow flag is set) */
static int add_nodes(B* b, uint32_t S, uint32_t l)
{
    int32_t i = (int32_t)W(LQ(b, l), 0) - 1;
    if (i < 0) return 1;
    const uint32_t base = W(LQ(b, S + 0x30), 1);
    if (base & 15) { b->bad = 2; return 0; }
    for (;; i--) {
        const uint32_t cap = W(LQ(b, S + 0x40), 1);
        const V s30 = LQ(b, S + 0x30);
        const V e = LQ(b, l + 16 + 16 * (uint32_t)i);
        if (!((int32_t)cap > (int32_t)W(s30, 2))) {        /* full: the overflow flag, byte 0 of S+0x30 */
            const V q = LQ(b, S + 0x30);
            SQ(b, S + 0x30, SETW(q, 0, (W(q, 0) & 0x00FFFFFFu) | 0x01000000u));
            return 0;
        }
        const uint32_t c = W(s30, 2);
        SQ(b, S + 0x30, SETW(s30, 2, c + 1));
        const uint32_t rec = c * 32 + base;
        SQ(b, rec, LQ(b, W(e, 0)));
        SW(b, rec + 0x10, 0, i != 0);
        SW(b, rec + 0x10, 1, 0);
        if (i == 0) return 1;
    }
}

/* 0x68D8 */
static NOINL void bsp(B* b, const u128* g)
{
    V r[128];
    memcpy(r, g, sizeof r);
    const uint32_t sp = W(r[1], 0);
    const uint32_t S = W(r[3], 0);
    SQ(b, sp - 0x30, r[82]); SQ(b, sp + 0x10, r[0]); SQ(b, sp - 0x10, r[80]); SQ(b, sp - 0x20, r[81]);
    SQ(b, sp - 0x40, r[83]); SQ(b, sp - 0x50, r[84]); SQ(b, sp - 0x60, r[85]); SQ(b, sp - 0x70, r[86]);
    SQ(b, sp - 0x80, r[87]); SQ(b, sp - 0xA0, r[1]);
    const V r1 = ADDI(r[1], -160);
    if (S & 15) { b->bad = 2; return; }
    V N = r[4], L = r[5];
    if (!W(N, 0)) return;
    unsigned steps = 0;
    while (!(W(LQ(b, S + 0x30), 0) >> 24)) {             /* until the overflow flag */
        if (++steps > 1u << 20) { b->bad = 3; return; }
        const uint32_t n = W(N, 0);
        if (n & 15) { b->bad = 2; return; }
        /* the positive-side list */
        const V lh = LQ(b, W(L, 0)), s50 = LQ(b, S + 0x50);
        const V Lp = _mm_sub_epi32(ROT3(s50), ADDI(_mm_slli_epi32(lh, 4), 16));
        SQ(b, S + 0x50, SETW(s50, 3, W(Lp, 0)));
        if (W(Lp, 0) & 15) { b->bad = 2; return; }
        SW(b, W(Lp, 0), 0, 0);
        const V kids = LQ(b, n + 0x10);
        if (!W(kids, 0)) {
            split(b, 0, S, LQ(b, n), W(L, 0), 0, W(Lp, 0), r1);
        } else {
            const V lh2 = LQ(b, W(L, 0)), s50b = LQ(b, S + 0x50);
            const V Ln = _mm_sub_epi32(Lp, ADDI(_mm_slli_epi32(lh2, 4), 16));
            SQ(b, S + 0x50, SETW(s50b, 3, W(Ln, 0)));
            if (W(Ln, 0) & 15) { b->bad = 2; return; }
            SW(b, W(Ln, 0), 0, 0);
            split(b, 1, S, LQ(b, n), W(L, 0), W(Ln, 0), W(Lp, 0), r1);
            if (b->bad) return;
            if ((int32_t)W(LQ(b, W(Ln, 0)), 0) > 0) {   /* push (child 0, negative list) */
                const V s40 = LQ(b, S + 0x40);
                const uint32_t c = W(s40, 3);
                SQ(b, S + 0x40, SETW(s40, 3, c + 1));
                const uint32_t rec = c * 32 + W(s40, 2);
                SQ(b, rec + 0x10, Ln);
                const V k = LQ(b, n + 0x10);
                const V z = _mm_cmpeq_epi32(k, _mm_setzero_si128());
                SQ(b, rec, _mm_andnot_si128(z, _mm_add_epi32(N, _mm_slli_epi32(k, 5))));
            }
        }
        if (b->bad) return;
        V next = _mm_setzero_si128();
        if ((int32_t)W(LQ(b, W(Lp, 0)), 0) > 0) {
            const V k = LQ(b, n + 0x10);
            if (!W(k, 1)) {                              /* no child 1: the list becomes nodes */
                const uint32_t before = W(LQ(b, S + 0x30), 2);
                if (add_nodes(b, S, W(Lp, 0))) {          /* the node now links to them */
                    const uint32_t base = W(LQ(b, S + 0x30), 1);
                    const V k2 = LQ(b, n + 0x10);
                    SQ(b, n + 0x10, SETW(k2, 1, before - (uint32_t)((int32_t)(n - base) >> 5)));
                }
            } else {
                next = _mm_add_epi32(N, _mm_slli_epi32(ROT1(k), 5));
                if (W(next, 0)) { N = next; L = Lp; continue; }
            }
        }
        /* pop, or stop */
        const V s40 = LQ(b, S + 0x40);
        if ((int32_t)W(s40, 3) > 0) {
            const uint32_t c = W(s40, 3);
            SQ(b, S + 0x40, SETW(s40, 3, c - 1));
            const uint32_t rec = c * 32 + W(s40, 2);
            L = LQ(b, rec - 0x10);
            N = LQ(b, rec - 0x20);
        } else N = next;
        if (!W(N, 0)) return;
    }
}

/* Run 0x68D8 on (gpr, ls). 1: done, the state is the function's result; 0:
 * nothing changed (the lifted function must run). */
static __thread Journal* t_j;
static __thread int t_why;
int j220_bsp_run(u128* gpr, uint8_t* ls)
{
    if (!t_j) { t_j = (Journal*)calloc(1, sizeof *t_j); if (!t_j) return 0; }
    B b = { ls, t_j, 0 };
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(CSR_FAST);
    bsp(&b, gpr);
    if (_mm_getcsr() & 0x9) b.bad = 1;          /* overflow or invalid */
    _mm_setcsr(csr);
    t_why = b.bad;
    if (b.bad) { j_undo(t_j, ls); return 0; }
    j_clear(t_j);
    return 1;
}
int j220_bsp_why(void) { return t_why; }

#ifdef J220_NO_RUNTIME
/* For the offline unit test (work/runs/job220/oracle/natdrv.c UNIT=): the
 * crossings on their own, from a plane and A, C, B, D (five quads); out: the
 * two new vertices. 0 if the run would be abandoned. */
static NOINL int unit_body(const uint32_t* in, uint32_t* out)
{
    B b = { 0, 0, 0 };
    V pl, A, C, Bv, D, nv1, nv2;
    memcpy(&pl, in, 16); memcpy(&A, in + 4, 16); memcpy(&C, in + 8, 16); memcpy(&Bv, in + 12, 16); memcpy(&D, in + 16, 16);
    e255(&b, pl, ALL);
    crossings(&b, pl, A, Bv, C, D, &nv1, &nv2);
    memcpy(out, &nv1, 16); memcpy(out + 4, &nv2, 16);
    return !b.bad;
}
int j220_unit(const uint32_t* in, uint32_t* out)
{
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(CSR_FAST);
    int ok = unit_body(in, out);
    if (_mm_getcsr() & 0x9) ok = 0;
    _mm_setcsr(csr);
    return ok;
}
#endif

#ifndef J220_NO_RUNTIME
void (*spu_lookup(uint32_t addr, int image_id))(spu_context*);
int spu_native_register(int image_id, uint32_t addr, int (*fn)(spu_context*));
static int s_mode = -1;                 /* PS3RECOMP_NATIVE_J220 */
static _Thread_local int t_bypass;
static unsigned long long s_calls, s_native, s_fallback, s_fb[4], s_cmp, s_bad, s_ns_nat, s_ns_spu;
static inline uint64_t now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void stats(void)
{
    static uint64_t next;
    const uint64_t now = now_ns() / 1000000000ull;
    uint64_t n = __atomic_load_n(&next, __ATOMIC_RELAXED);
    if (now < n || !__atomic_compare_exchange_n(&next, &n, now + 10, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    if (!n || !s_calls) return;
    fprintf(stderr, "[j220] 0x68D8: %llu calls, %llu native, %llu lifted (float %llu, layout %llu, runaway %llu)",
            s_calls, s_native, s_fallback, s_fb[1], s_fb[2], s_fb[3]);
    if (s_mode == 2)
        fprintf(stderr, "; compared %llu, differ %llu; native %.2f us, lifted %.2f us per compared call",
                s_cmp, s_bad, s_cmp ? s_ns_nat / 1e3 / s_cmp : 0.0, s_cmp ? s_ns_spu / 1e3 / s_cmp : 0.0);
    fprintf(stderr, "\n");
}
static void dump(const char* dir, unsigned long long id, const u128* gpr, const uint8_t* ls)
{
    char path[640];
    snprintf(path, sizeof path, "%s/in_%05X_%06llu.bin", dir, J220_BSP, id);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(gpr, sizeof(u128), 128, f);
    fwrite(ls, 1, SPU_LS_SIZE, f);
    fclose(f);
}
static int regs_differ(const u128* a, const u128* b)
{
    return memcmp(&a[0], &b[0], 32) || memcmp(&a[80], &b[80], 48 * sizeof(u128));
}
static int j220_entry(spu_context* ctx)
{
    if (t_bypass) return 0;
    if (__builtin_expect(__atomic_load_n(&s_mode, __ATOMIC_ACQUIRE) < 0, 0)) {
        const char* e = getenv("PS3RECOMP_NATIVE_J220");
        int m = e ? atoi(e) : 1;
        if (m < 0 || m > 2) m = 0;
        fprintf(stderr, "[j220] native BSP clipper (image 220, 0x68D8): %s\n",
                m == 2 ? "compare with the lifted code" : m ? "on" : "off");
        __atomic_store_n(&s_mode, m, __ATOMIC_RELEASE);
    }
    if (!s_mode) return 0;
    const unsigned long long id = __atomic_fetch_add(&s_calls, 1, __ATOMIC_RELAXED);
    stats();
    if (s_mode != 2) {
        if (j220_bsp_run(ctx->gpr, ctx->ls)) { __atomic_fetch_add(&s_native, 1, __ATOMIC_RELAXED); return 1; }
        __atomic_fetch_add(&s_fallback, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_fb[t_why & 3], 1, __ATOMIC_RELAXED);
        return 0;
    }
    /* Mode 2: native on a copy, then the lifted function on the real state. */
    static _Thread_local uint8_t* pre; static _Thread_local uint8_t* work;
    static _Thread_local u128 gpre[128], gwork[128];
    if (!pre) { pre = (uint8_t*)aligned_alloc(64, SPU_LS_SIZE); work = (uint8_t*)aligned_alloc(64, SPU_LS_SIZE); if (!pre || !work) return 0; }
    memcpy(pre, ctx->ls, SPU_LS_SIZE); memcpy(gpre, ctx->gpr, sizeof gpre);
    memcpy(work, pre, SPU_LS_SIZE); memcpy(gwork, gpre, sizeof gwork);
    const uint64_t t0 = now_ns();
    const int ok = j220_bsp_run(gwork, work);
    const uint64_t t1 = now_ns();
    void (*fn)(spu_context*) = spu_lookup(J220_BSP, ctx->resident_ovl);
    if (!fn) return 0;
    if (!ok) {
        __atomic_fetch_add(&s_fallback, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_fb[t_why & 3], 1, __ATOMIC_RELAXED);
        return 0;
    }
    const int32_t si = (int32_t)ctx->image_id;
    const uint32_t r0 = ctx->gpr[0]._u32[0];
    t_bypass = 1;
    const uint64_t t2 = now_ns();
    ctx->host_depth++; fn(ctx); spu_drain_call(ctx, r0); ctx->host_depth--;
    const uint64_t t3 = now_ns();
    spu_img_restore(ctx, si);
    t_bypass = 0;
    __atomic_fetch_add(&s_native, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_cmp, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_ns_nat, t1 - t0, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_ns_spu, t3 - t2, __ATOMIC_RELAXED);
    const int regs = regs_differ(ctx->gpr, gwork);
    if (regs || memcmp(ctx->ls, work, SPU_LS_SIZE)) {
        const unsigned long long nb = __atomic_add_fetch(&s_bad, 1, __ATOMIC_RELAXED);
        if (nb <= 8) {
            uint32_t first = SPU_LS_SIZE, last = 0, nq = 0;
            for (uint32_t q = 0; q < SPU_LS_SIZE; q += 16)
                if (memcmp(ctx->ls + q, work + q, 16)) { if (first == SPU_LS_SIZE) first = q; last = q; nq++; }
            fprintf(stderr, "[j220] call %llu differs: %u LS quads (0x%05X..0x%05X)%s\n", id, nq,
                    first == SPU_LS_SIZE ? 0 : first, last, regs ? ", registers" : "");
            const char* dir = getenv("PS3RECOMP_J220_DUMP");
            if (dir) dump(dir, id, gpre, pre);
        }
    }
    ctx->pc = ctx->gpr[0]._u32[0];
    return 1;
}
__attribute__((constructor)) static void j220_register(void)
{
    spu_native_register(J220_IMAGE, J220_BSP, j220_entry);
}
#endif
