/* inFAMOUS 2 (BCUS98125): Edge Geometry's hot SPU kernels, natively.
 *
 * Job module 6d1c65171899 (image 209) is Edge Geometry: per mesh segment it
 * decompresses vertices and indexes, skins, culls and compresses for the RSX.
 * This file replaces pure local-store functions of it with host code that
 * leaves the registers the SPU ABI makes observable and the whole local store
 * exactly as the lifted code would (rpi/edge-geom-kernels/RESEARCH.md):
 *
 *   0xD0A8  skinning (blend matrices per vertex, transform positions and up to
 *           three more streams, renormalise them; calls 0x6E98, normalise)
 *
 * Floats follow the SPU: truncation, denormal operands and results as zero,
 * no infinities. The fast paths run four vertices per AVX-512 register and
 * check every result; a batch that meets an operand or result the fast path
 * does not model (denormal, exponent 255, |x| >= FLT_MAX) is recomputed by the
 * exact per-vertex path, which uses runtime/spu/spu_helpers.h like the lift.
 * A call whose buffers overlap, or that would take an error path, returns 0
 * before writing anything and the lifted function runs instead.
 *
 * The lift calls spu_native_entry at each address (tools/lift_spu.py,
 * NATIVE_ENTRIES). PS3RECOMP_NATIVE_EDGE: 1 (default) on; 0 off; 2 runs the
 * native kernel on a copy of the local store, then the lifted function, and
 * compares the local store and r0, r1, r80-r127, logging any difference.
 *
 * Built like the lifted SPU sources (-march=x86-64-v4): it needs AVX-512. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <immintrin.h>

#include "spu_context.h"
#include "spu_helpers.h"

#if !defined(__AVX512F__) || !defined(__AVX512BW__) || !defined(__AVX512VL__) || !defined(__AVX512DQ__)
#error "edge_kernels_native.c needs -march=x86-64-v4 (AVX-512 F/BW/VL/DQ)"
#endif

#define EDGE_IMAGE 209
#ifdef EDGE_HARNESS
int g_edge_stat[8];
#define EDGE_STAT(i) (g_edge_stat[i]++)
#else
#define EDGE_STAT(i) ((void)0)
#endif
#define LSM 0x3FFFFu
#define LSQ 0x3FFF0u
#define F1 0x3F800000u
#define INV255 0x3B808080u
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* ------------------------------------------------------------------ exact per-quad helpers */
typedef u128 Q;
static inline uint32_t be32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
static inline Q lq(const uint8_t* ls, uint32_t a)
{
    const __m128i v = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + (a & LSQ))),
                                       _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
    Q r; memcpy(&r, &v, 16); return r;
}
static inline void sq(uint8_t* ls, uint32_t a, Q q)
{
    __m128i v; memcpy(&v, &q, 16);
    _mm_storeu_si128((__m128i*)(ls + (a & LSQ)), _mm_shuffle_epi8(v, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
}
static inline Q V4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { Q r; r._u32[0] = a; r._u32[1] = b; r._u32[2] = c; r._u32[3] = d; return r; }
static inline Q yzx(Q v) { return V4(v._u32[1], v._u32[2], v._u32[0], v._u32[3]); }
static inline Q zxy(Q v) { return V4(v._u32[2], v._u32[0], v._u32[1], v._u32[3]); }
static inline Q spl(Q v, int i) { return V4(v._u32[i], v._u32[i], v._u32[i], v._u32[i]); }
/* rotqby by the low four bits of addr: the word at addr inside its quad */
static inline uint32_t word_at(const uint8_t* ls, uint32_t addr)
{
    const uint8_t* q = ls + (addr & LSQ); const int s = addr & 15;
    return (uint32_t)q[s] << 24 | q[(s + 1) & 15] << 16 | q[(s + 2) & 15] << 8 | q[(s + 3) & 15];
}
static inline Q quad_at(const uint8_t* ls, uint32_t addr)
{
    const __m128i rot = _mm_and_si128(_mm_add_epi8(_mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15), _mm_set1_epi8((char)(addr & 15))), _mm_set1_epi8(15));
    const __m128i v = _mm_shuffle_epi8(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + (addr & LSQ))), rot),
                                       _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
    Q r; memcpy(&r, &v, 16); return r;
}

/* ------------------------------------------------------------------ 16-lane SPU floats
 * Four quads per register, lane 4*j+i = word i of quad j.
 *
 * The SPU threads run with DAZ in MXCSR (spu_fp_enter, runtime/spu/
 * spu_context.h, SPU_FP_DAZ): the helpers of runtime/spu/spu_helpers.h then
 * read an exponent-0 operand as zero, as the SPU does. The fast paths run
 * with MXCSR = truncation, flush-to-zero and denormals-are-zero. Then an x86
 * result is the helpers' whenever no operand has exponent 255 and nothing
 * overflows -- except that a zero may carry a sign the SPU never gives it.
 * Signs of zeros never reach a nonzero value in these kernels (no division,
 * and compares treat both zeros alike), so outputs are made +0 where their
 * exponent is 0, and that is all. Exponent 255 and overflow are excluded by
 * bounding what a batch reads from the LS (RESEARCH.md section 1.3); a batch
 * outside the bound runs the exact path, at the caller's MXCSR.
 * The fast functions are noinline: the compiler does not order floating-point
 * arithmetic against MXCSR writes, but it cannot move it out of a call. */
typedef __m512i Z;
#define CSR_FAST 0xFFC0u                       /* RZ | FZ | DAZ, all exceptions masked */
#define B_IN 0x48800000                        /* 2^18 */
#define B_RSQ 0x71800000                       /* 2^100: a factor from fi */
#define NOINL __attribute__((noinline))
static inline __m512 zf(Z x) { return _mm512_castsi512_ps(x); }
static inline Z zi(__m512 x) { return _mm512_castps_si512(x); }
static inline Z FM(Z a, Z b) { return zi(_mm512_mul_ps(zf(a), zf(b))); }
static inline Z FMA(Z a, Z b, Z c) { return zi(_mm512_fmadd_ps(zf(a), zf(b), zf(c))); }
static inline Z FMS(Z a, Z b, Z c) { return zi(_mm512_fmsub_ps(zf(a), zf(b), zf(c))); }
static inline Z canon(Z r) { return _mm512_maskz_mov_epi32(_mm512_test_epi32_mask(r, _mm512_set1_epi32(0x7F800000)), r); }
/* a value read from the LS: its magnitude goes to mi; kb collects lanes
 * found bad along the way */
typedef struct { Z mi; __mmask16 kb; } Chk;
static inline void chk_init(Chk* c) { c->mi = _mm512_setzero_si512(); c->kb = 0; }
static inline Z chk(Z x, Chk* c)
{
    c->mi = _mm512_max_epu32(c->mi, _mm512_and_si512(x, _mm512_set1_epi32(0x7FFFFFFF)));
    return x;
}
static inline int chk_bad(const Chk* c)
{
    return (_mm512_cmpge_epu32_mask(c->mi, _mm512_set1_epi32(B_IN)) | c->kb) != 0;
}
static inline Z bswap16(Z v)
{
    return _mm512_shuffle_epi8(v, _mm512_broadcast_i32x4(_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
}
/* four consecutive quads at a (lqd semantics: a & ~15; the caller keeps them inside the LS) */
static inline Z ld16(const uint8_t* ls, uint32_t a) { return bswap16(_mm512_loadu_si512(ls + (a & LSQ))); }
static inline void st16(uint8_t* ls, uint32_t a, Z v) { _mm512_storeu_si512(ls + (a & LSQ), bswap16(v)); }
/* 48 bytes at p as quads 0..2, quad 3 zero (the span check leaves 16 readable bytes after) */
static inline Z ld12(const uint8_t* p) { return _mm512_maskz_mov_epi32(0x0FFF, bswap16(_mm512_loadu_si512(p))); }
#define YZX(v) _mm512_shuffle_epi32((v), (_MM_PERM_ENUM)0xC9)
#define ZXY(v) _mm512_shuffle_epi32((v), (_MM_PERM_ENUM)0xD2)

/* fi (runtime/spu/spu_helpers.h), sixteen lanes; k_rsq_lut is frsqest's table */
static const uint32_t k_rsq_lut[64] = {
    0x350160, 0x34E954, 0x2F993D, 0x2F993D, 0x2AA523, 0x2AA523, 0x26190D, 0x26190D,
    0x21E4F9, 0x21E4F9, 0x1E00E9, 0x1E00E9, 0x1A5CD9, 0x1A5CD9, 0x16F8CB, 0x16F8CB,
    0x13CCC0, 0x13CCC0, 0x10CCB3, 0x10CCB3, 0x0E00AA, 0x0E00AA, 0x0B58A1, 0x0B58A1,
    0x08D498, 0x08D498, 0x067491, 0x067491, 0x043089, 0x043089, 0x020C83, 0x020C83,
    0x7FFDF4, 0x7FD1DE, 0x7859C8, 0x783DBA, 0x71559C, 0x71559C, 0x6AE57C, 0x6AE57C,
    0x64F561, 0x64F561, 0x5F7149, 0x5F7149, 0x5A4D33, 0x5A4D33, 0x55811F, 0x55811F,
    0x51050F, 0x51050F, 0x4CC8FE, 0x4CC8FE, 0x48D0F0, 0x48D0F0, 0x4510E4, 0x4510E4,
    0x4180D7, 0x4180D7, 0x3E24CC, 0x3E24CC, 0x3AF4C3, 0x3AF4C3, 0x37E8BA, 0x37E8BA
};
static inline Z fi16(Z ya, Z est)
{
    const Z expo = _mm512_and_si512(_mm512_srli_epi32(est, 23), _mm512_set1_epi32(0xFF));
    const Z base = _mm512_and_si512(_mm512_srli_epi32(est, 10), _mm512_set1_epi32(0x1FFF));
    const Z step = _mm512_and_si512(est, _mm512_set1_epi32(0x3FF));
    const Z y = _mm512_and_si512(ya, _mm512_set1_epi32(0x7FFFF));
    const Z mh = _mm512_sub_epi32(_mm512_slli_epi32(_mm512_or_si512(base, _mm512_set1_epi32(0x2000)), 11),
                                  _mm512_srli_epi32(_mm512_add_epi32(_mm512_mullo_epi32(step, y), _mm512_set1_epi32(255)), 8));
    const __mmask16 below1 = _mm512_cmpgt_epi32_mask(_mm512_set1_epi32(0x1000000), mh);
    const Z frac = _mm512_and_si512(_mm512_mask_mov_epi32(_mm512_srli_epi32(mh, 1), below1, mh), _mm512_set1_epi32(0x7FFFFF));
    const Z e2 = _mm512_mask_sub_epi32(expo, below1, expo, _mm512_set1_epi32(1));
    const __mmask16 zero = _mm512_cmpeq_epi32_mask(expo, _mm512_setzero_si512()) | _mm512_cmpeq_epi32_mask(e2, _mm512_setzero_si512());
    return _mm512_maskz_mov_epi32((__mmask16)~zero, _mm512_or_si512(_mm512_and_si512(est, _mm512_set1_epi32((int)0x80000000u)),
                                                                     _mm512_or_si512(_mm512_slli_epi32(e2, 23), frac)));
}
/* fi(s, frsqest(s)) in one step. frsqest's estimate has no sign and an
 * exponent of at least 62, so fi's zero cases never arise; the estimate's
 * fractions enter through two tables derived from k_rsq_lut. */
static uint32_t k_rsq_a[64], k_rsq_s[64];
static void rsq_init(void)
{
    for (int i = 0; i < 64; i++) { k_rsq_a[i] = (((k_rsq_lut[i] >> 10) & 0x1FFF) | 0x2000) << 11; k_rsq_s[i] = k_rsq_lut[i] & 0x3FF; }
}
static inline Z lut64(const uint32_t* t, Z idx, __mmask16 hi)
{
    const Z lo = _mm512_permutex2var_epi32(_mm512_loadu_si512(t), idx, _mm512_loadu_si512(t + 16));
    return _mm512_mask_mov_epi32(lo, hi, _mm512_permutex2var_epi32(_mm512_loadu_si512(t + 32), idx, _mm512_loadu_si512(t + 48)));
}
static inline Z rsq16(Z s)
{
    const Z e = _mm512_and_si512(_mm512_srli_epi32(s, 23), _mm512_set1_epi32(0xFF));
    const Z idx = _mm512_srli_epi32(s, 18);
    const __mmask16 hi = _mm512_test_epi32_mask(idx, _mm512_set1_epi32(32));
    const Z A = lut64(k_rsq_a, idx, hi), St = lut64(k_rsq_s, idx, hi);
    const Z y = _mm512_and_si512(s, _mm512_set1_epi32(0x7FFFF));
    const Z mh = _mm512_sub_epi32(A, _mm512_srli_epi32(_mm512_add_epi32(_mm512_mullo_epi32(St, y), _mm512_set1_epi32(255)), 8));
    const __mmask16 below1 = _mm512_cmpgt_epi32_mask(_mm512_set1_epi32(0x1000000), mh);
    const Z frac = _mm512_and_si512(_mm512_mask_mov_epi32(_mm512_srli_epi32(mh, 1), below1, mh), _mm512_set1_epi32(0x7FFFFF));
    Z rexp = _mm512_sub_epi32(_mm512_set1_epi32(190), _mm512_srli_epi32(_mm512_add_epi32(e, _mm512_set1_epi32(1)), 1));
    rexp = _mm512_mask_mov_epi32(rexp, _mm512_testn_epi32_mask(e, e), _mm512_set1_epi32(0xFF));
    rexp = _mm512_mask_sub_epi32(rexp, below1, rexp, _mm512_set1_epi32(1));
    return _mm512_or_si512(_mm512_slli_epi32(rexp, 23), frac);
}

/* 0x6E98's arithmetic: v * (r, r, r, 1), r = fi(s, frsqest(s)), s = z*z + (y*y + x*x).
 * sumsq16 gives s of each of four vectors in every lane of its block. */
static inline Z sumsq16(Z v)
{
    const Z x = _mm512_shuffle_epi32(v, (_MM_PERM_ENUM)0x00), y = _mm512_shuffle_epi32(v, (_MM_PERM_ENUM)0x55),
            z = _mm512_shuffle_epi32(v, (_MM_PERM_ENUM)0xAA);
    return FMA(z, z, FMA(y, y, FM(x, x)));
}
/* v * (r, r, r, 1), r per block. s = 0 makes r an exponent-255 value, which
 * the fast path cannot multiply: where v's lane is zero the SPU gives +0 and
 * so does a zero factor; any other lane goes the exact way. Not canonical. */
static inline Z scale16(Z v, Z r, Chk* ck)
{
    const __mmask16 big = _mm512_cmpge_epu32_mask(_mm512_and_si512(r, _mm512_set1_epi32(0x7FFFFFFF)), _mm512_set1_epi32(B_RSQ)) & 0x7777;
    ck->kb |= big & ~_mm512_testn_epi32_mask(v, _mm512_set1_epi32(0x7FFFFFFF));
    r = _mm512_mask_mov_epi32(_mm512_maskz_mov_epi32((__mmask16)~big, r), 0x8888, _mm512_set1_epi32(F1));
    return FM(v, r);
}
static inline Z norm16(Z v, Chk* ck)
{
    const Z s = sumsq16(v);
    return scale16(v, rsq16(s), ck);
}
/* ------------------------------------------------------------------ exact per-vector paths */
static inline Q norm_q(Q v)
{
    const Q s = spu_fma(spl(v, 2), spl(v, 2), spu_fma(spl(v, 1), spl(v, 1), spu_fm(spl(v, 0), spl(v, 0))));
    const Q r = spu_fi(s, spu_frsqest(s));
    return spu_fm(v, V4(r._u32[0], r._u32[0], r._u32[0], F1));
}
static inline Q xform_q(Q x, Q A, Q B, Q C) { return spu_fma(zxy(x), C, spu_fma(yzx(x), B, spu_fm(x, A))); }
static inline Q cofactor_q(Q x, Q A, Q B, Q C)
{
    const Q d1 = spu_fms(yzx(A), zxy(A), spu_fm(zxy(C), yzx(B)));
    const Q d2 = spu_fms(zxy(B), yzx(B), spu_fm(yzx(C), zxy(A)));
    const Q d3 = spu_fms(yzx(C), zxy(C), spu_fm(zxy(B), yzx(A)));
    return spu_fma(d3, zxy(x), spu_fma(d2, yzx(x), spu_fm(d1, x)));
}
/* 0x6E98 on vectors [from, n8) at p (it rounds the count up to eight) */
static NOINL int norm_block16(uint8_t* ls, uint32_t a)
{
    Chk ck; chk_init(&ck);
    const Z o = canon(norm16(chk(ld16(ls, a), &ck), &ck));
    if (unlikely(chk_bad(&ck))) return 0;
    st16(ls, a, o);
    return 1;
}
/* sixteen vectors at a: x, y, z of all of them in three registers, one estimate */
static NOINL int norm_group16(uint8_t* ls, uint32_t a)
{
    Chk ck; chk_init(&ck);
    Z V[4];
    for (int m = 0; m < 4; m++) V[m] = chk(ld16(ls, a + 64 * m), &ck);
    Z X[3];
    for (int c = 0; c < 3; c++) {
        const Z ix = _mm512_add_epi32(_mm512_setr_epi32(0, 4, 8, 12, 16, 20, 24, 28, 0, 4, 8, 12, 16, 20, 24, 28), _mm512_set1_epi32(c));
        X[c] = _mm512_mask_mov_epi32(_mm512_permutex2var_epi32(V[0], ix, V[1]), 0xFF00, _mm512_permutex2var_epi32(V[2], ix, V[3]));
    }
    const Z R = rsq16(FMA(X[2], X[2], FMA(X[1], X[1], FM(X[0], X[0]))));
    Z o[4];
    for (int m = 0; m < 4; m++)
        o[m] = canon(scale16(V[m], _mm512_permutexvar_epi32(_mm512_setr_epi32(4 * m, 4 * m, 4 * m, 4 * m, 4 * m + 1, 4 * m + 1, 4 * m + 1, 4 * m + 1,
                                                                             4 * m + 2, 4 * m + 2, 4 * m + 2, 4 * m + 2, 4 * m + 3, 4 * m + 3, 4 * m + 3, 4 * m + 3), R), &ck));
    if (unlikely(chk_bad(&ck))) return 0;
    for (int m = 0; m < 4; m++) st16(ls, a + 64 * m, o[m]);
    return 1;
}
/* MXCSR is CSR_FAST here; csr is the caller's */
static void normalize_ls(uint8_t* ls, uint32_t p, uint32_t from, uint32_t n8, uint32_t csr)
{
    uint32_t k = from;
    for (; k + 16 <= n8; k += 16)
        if (unlikely(!norm_group16(ls, p + 16 * k)))
            for (uint32_t j = 0; j < 16; j += 4)
                if (!norm_block16(ls, p + 16 * (k + j))) {
                    _mm_setcsr(csr);
                    for (uint32_t t = 0; t < 4; t++) sq(ls, p + 16 * (k + j + t), norm_q(lq(ls, p + 16 * (k + j + t))));
                    _mm_setcsr(CSR_FAST);
                }
    for (; k < n8; k += 4) {
        if (likely(norm_block16(ls, p + 16 * k))) continue;
        _mm_setcsr(csr);
        for (uint32_t j = 0; j < 4; j++) sq(ls, p + 16 * (k + j), norm_q(lq(ls, p + 16 * (k + j))));
        _mm_setcsr(CSR_FAST);
    }
}

/* ------------------------------------------------------------------ overlap guard */
typedef struct { uint32_t lo, hi; int w; } Span;
static int spans_ok(const Span* s, int n)
{
    for (int i = 0; i < n; i++) {
        if (s[i].hi > SPU_LS_SIZE || s[i].lo > s[i].hi) return 0;
        for (int j = i + 1; j < n; j++)
            if ((s[i].w || s[j].w) && s[i].lo < s[j].hi && s[j].lo < s[i].hi) return 0;
    }
    return 1;
}

/* fm(cuflt(w, 155), 1/255) for every weight byte, through the helpers */
static uint32_t g_wtab[256];
static void wtab_init(void)
{
    for (int w = 0; w < 256; w += 4) {
        const Q r = spu_fm(spu_cuflt(V4(w, w + 1, w + 2, w + 3), 155), V4(INV255, INV255, INV255, INV255));
        for (int i = 0; i < 4; i++) g_wtab[w + i] = r._u32[i];
    }
}

/* ================================================================== 0xD0A8: skinning
 *
 * r3 the job context, r4 the matrix palette, r5 the number of matrices, r6
 * the per-vertex bone indexes and weights. RESEARCH.md section 2. */
typedef struct {
    uint32_t M, IW, pos, scr, s60, st[3];
    int nst, multi, r105, renorm;
    uint32_t maxi;
    const uint8_t* okm;   /* matrix m may take the fast path */
    const Z* pal;         /* matrix m in host order, quad 3 zero; 256..511 all zero */
} Skin;

/* one vertex, exactly: blend (or pick) the matrix, write it to the scratch,
 * transform the position and the streams */
static NOINL void skin_vertex_q(uint8_t* ls, const Skin* k, uint32_t v, uint32_t sv, uint32_t iw)
{
    Q A, B, C;
    if (k->multi) {
        const uint32_t a = (iw & LSQ) + 8 * v;
        const uint8_t* b = ls + a;
        Q w = spu_fm(spu_cuflt(V4(b[0], b[2], b[4], b[6]), 155), V4(INV255, INV255, INV255, INV255));
        Q q[3];
        for (int r = 0; r < 3; r++) {
            Q acc = spu_fm(spl(w, 0), lq(ls, k->M + 16 * r + 48 * b[1]));
            acc = spu_fma(spl(w, 1), lq(ls, k->M + 16 * r + 48 * b[3]), acc);
            acc = spu_fma(spl(w, 2), lq(ls, k->M + 16 * r + 48 * b[5]), acc);
            q[r] = spu_fma(spl(w, 3), lq(ls, k->M + 16 * r + 48 * b[7]), acc);
        }
        A = q[0]; B = q[1]; C = q[2];
    } else {
        const uint32_t a = iw + (v & ~1u);
        const uint32_t i = ls[(v & 1) ? ((a & ~15u) + ((a + 1) & 15)) & LSM : a & LSM];
        A = lq(ls, k->M + 48 * i); B = lq(ls, k->M + 16 + 48 * i); C = lq(ls, k->M + 32 + 48 * i);
    }
    const Q T = V4(A._u32[3], B._u32[3], C._u32[3], 0);
    A._u32[3] = F1; B._u32[3] = 0; C._u32[3] = 0;
    const Q p = lq(ls, k->pos + 16 * v);
    sq(ls, k->scr + 48 * sv, A); sq(ls, k->scr + 48 * sv + 16, B); sq(ls, k->scr + 48 * sv + 32, C);
    sq(ls, k->pos + 16 * v, spu_fma(zxy(p), C, spu_fma(yzx(p), B, spu_fma(p, A, T))));
    if (k->r105) {
        sq(ls, k->s60 + 16 * v, cofactor_q(lq(ls, k->s60 + 16 * v), A, B, C));
    }
    for (int s = 0; s < k->nst; s++) {
        sq(ls, k->st[s] + 16 * v, xform_q(lq(ls, k->st[s] + 16 * v), A, B, C));
    }
}

/* four vertices v..v+3 (the scratch slots sv..sv+3); 0 if the batch must go the exact way */
static inline __attribute__((always_inline)) int skin_batch16(uint8_t* ls, const Skin* k, uint32_t v, uint32_t sv, uint32_t iw,
                                                              const int MULTI, const int RENORM, const int R105, const int NST)
{
    Chk ck; chk_init(&ck);
    Z q[4];
    if (MULTI) {
        const uint8_t* b = ls + (iw & LSQ) + 8 * v;
        /* a bone of weight 0 takes matrix 256 + index, all zero (see the maxi scan) */
#define EIX(w, i) ((unsigned)(i) | (unsigned)((w) == 0) << 8)
        if (!(k->okm[EIX(b[0], b[1])] & k->okm[EIX(b[2], b[3])] & k->okm[EIX(b[4], b[5])] & k->okm[EIX(b[6], b[7])] &
              k->okm[EIX(b[8], b[9])] & k->okm[EIX(b[10], b[11])] & k->okm[EIX(b[12], b[13])] & k->okm[EIX(b[14], b[15])] &
              k->okm[EIX(b[16], b[17])] & k->okm[EIX(b[18], b[19])] & k->okm[EIX(b[20], b[21])] & k->okm[EIX(b[22], b[23])] &
              k->okm[EIX(b[24], b[25])] & k->okm[EIX(b[26], b[27])] & k->okm[EIX(b[28], b[29])] & k->okm[EIX(b[30], b[31])])) return 0;
#pragma GCC unroll 4
        for (int j = 0; j < 4; j++) {
            const uint8_t* bj = b + 8 * j;
            Z acc = FM(_mm512_set1_epi32(g_wtab[bj[0]]), k->pal[EIX(bj[0], bj[1])]);
            acc = FMA(_mm512_set1_epi32(g_wtab[bj[2]]), k->pal[EIX(bj[2], bj[3])], acc);
            acc = FMA(_mm512_set1_epi32(g_wtab[bj[4]]), k->pal[EIX(bj[4], bj[5])], acc);
            q[j] = canon(FMA(_mm512_set1_epi32(g_wtab[bj[6]]), k->pal[EIX(bj[6], bj[7])], acc));
        }
#undef EIX
    } else {
        /* the matrix as it is: no arithmetic, so no canonical form either */
#pragma GCC unroll 4
        for (int j = 0; j < 4; j++) {
            const uint32_t a = iw + ((v + j) & ~1u);
            const uint32_t i = ls[(j & 1) ? ((a & ~15u) + ((a + 1) & 15)) & LSM : a & LSM];
            if (!k->okm[i]) return 0;
            q[j] = k->pal[i];
        }
    }
    /* q[j] = (row0, row1, row2, 0) of vertex j. T = (row0.w, row1.w, row2.w, 0) per vertex. */
    const Z t01 = _mm512_permutex2var_epi32(q[0], _mm512_setr_epi32(3, 7, 11, 0, 19, 23, 27, 0, 0, 0, 0, 0, 0, 0, 0, 0), q[1]);
    const Z t23 = _mm512_permutex2var_epi32(q[2], _mm512_setr_epi32(0, 0, 0, 0, 0, 0, 0, 0, 3, 7, 11, 0, 19, 23, 27, 0), q[3]);
    const Z T = _mm512_maskz_mov_epi32(0x7777, _mm512_mask_mov_epi32(t01, 0xFF00, t23));
#pragma GCC unroll 4
    for (int j = 0; j < 4; j++) q[j] = _mm512_mask_mov_epi32(q[j], 0x0888, _mm512_setr_epi32(0, 0, 0, F1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    const Z a01 = _mm512_shuffle_i64x2(q[0], q[1], 0x44), a23 = _mm512_shuffle_i64x2(q[2], q[3], 0x44);
    const Z c01 = _mm512_shuffle_i64x2(q[0], q[1], 0xEE), c23 = _mm512_shuffle_i64x2(q[2], q[3], 0xEE);
    const Z A = _mm512_shuffle_i64x2(a01, a23, 0x88), B = _mm512_shuffle_i64x2(a01, a23, 0xDD), C = _mm512_shuffle_i64x2(c01, c23, 0x88);
    const Z p = chk(ld16(ls, k->pos + 16 * v), &ck);
    const Z po = canon(FMA(ZXY(p), C, FMA(YZX(p), B, FMA(p, A, T))));
    Z o[4];                                   /* the streams, then S60 */
#pragma GCC unroll 3
    for (int s = 0; s < NST; s++) {
        const Z x = chk(ld16(ls, k->st[s] + 16 * v), &ck);
        o[s] = FMA(ZXY(x), C, FMA(YZX(x), B, FM(x, A)));
    }
    if (R105) {
        const Z x = chk(ld16(ls, k->s60 + 16 * v), &ck);
        const Z d1 = FMS(YZX(A), ZXY(A), FM(ZXY(C), YZX(B)));
        const Z d2 = FMS(ZXY(B), YZX(B), FM(YZX(C), ZXY(A)));
        const Z d3 = FMS(YZX(C), ZXY(C), FM(ZXY(B), YZX(A)));
        o[NST] = FMA(d3, ZXY(x), FMA(d2, YZX(x), FM(d1, x)));
    }
    if (unlikely(chk_bad(&ck))) return 0;
    {   /* the scratch takes 12 consecutive quads: A0 B0 C0 A1 | B1 C1 A2 B2 | C2 A3 B3 C3 */
        uint8_t* d = ls + ((k->scr + 48 * sv) & LSQ);
        _mm512_storeu_si512(d,       bswap16(_mm512_permutex2var_epi64(q[0], _mm512_setr_epi64(0, 1, 2, 3, 4, 5, 8, 9), q[1])));
        _mm512_storeu_si512(d + 64,  bswap16(_mm512_permutex2var_epi64(q[1], _mm512_setr_epi64(2, 3, 4, 5, 8, 9, 10, 11), q[2])));
        _mm512_storeu_si512(d + 128, bswap16(_mm512_permutex2var_epi64(q[2], _mm512_setr_epi64(4, 5, 8, 9, 10, 11, 12, 13), q[3])));
    }
    st16(ls, k->pos + 16 * v, po);
#pragma GCC unroll 3
    for (int s = 0; s < NST; s++) st16(ls, k->st[s] + 16 * v, canon(o[s]));
    if (R105) st16(ls, k->s60 + 16 * v, canon(o[NST]));
    return 1;
}

/* the exact path for four vertices, at the caller's MXCSR */
static NOINL void skin_slow4(uint8_t* ls, const Skin* k, uint32_t v, uint32_t sv, uint32_t iw, uint32_t csr)
{
    EDGE_STAT(1);
    _mm_setcsr(csr);
    for (uint32_t t = 0; t < 4; t++) skin_vertex_q(ls, k, v + t, sv + t, iw);
    _mm_setcsr(CSR_FAST);
}
/* every chunk; MXCSR is CSR_FAST (set by the caller: nothing in here writes
 * it). One instance per kind of call, so the batch keeps its streams in registers. */
static inline __attribute__((always_inline)) void skin_run_t(uint8_t* ls, const Skin* k, uint32_t n4, uint32_t chunk, uint32_t iw, uint32_t csr,
                                                             const int MULTI, const int RENORM, const int R105, const int NST)
{
    for (uint32_t rem = n4, base = 0; rem; ) {
        const uint32_t cnt = rem > chunk ? chunk : rem;
        for (uint32_t j = 0; j < cnt; j += 4)
            if (unlikely(!skin_batch16(ls, k, base + j, j, iw, MULTI, RENORM, R105, NST))) skin_slow4(ls, k, base + j, j, iw, csr);
        base += cnt;
        rem -= cnt;
    }
}
typedef void (*SkinRun)(uint8_t*, const Skin*, uint32_t, uint32_t, uint32_t, uint32_t);
#define SKIN_RUN(m, r, x, n) \
    static NOINL void skin_run_##m##r##x##n(uint8_t* ls, const Skin* k, uint32_t n4, uint32_t c, uint32_t iw, uint32_t csr) \
    { skin_run_t(ls, k, n4, c, iw, csr, m, r, x, n); }
#define SKIN_RUN4(m, r, x) SKIN_RUN(m, r, x, 0) SKIN_RUN(m, r, x, 1) SKIN_RUN(m, r, x, 2) SKIN_RUN(m, r, x, 3)
SKIN_RUN4(1, 0, 0) SKIN_RUN4(1, 1, 0) SKIN_RUN4(1, 1, 1) SKIN_RUN4(0, 0, 0) SKIN_RUN4(0, 1, 0) SKIN_RUN4(0, 1, 1)
#define SKIN_ROW(m, r, x) { skin_run_##m##r##x##0, skin_run_##m##r##x##1, skin_run_##m##r##x##2, skin_run_##m##r##x##3 }
/* [multi][renorm][r105][nst]; r105 without renorm does not occur (types 3 and 6 renormalise) */
static const SkinRun k_skin_run[2][2][2][4] = {
    { { SKIN_ROW(0, 0, 0), SKIN_ROW(0, 0, 0) }, { SKIN_ROW(0, 1, 0), SKIN_ROW(0, 1, 1) } },
    { { SKIN_ROW(1, 0, 0), SKIN_ROW(1, 0, 0) }, { SKIN_ROW(1, 1, 0), SKIN_ROW(1, 1, 1) } },
};

static int edge_skin_core(u128* gpr, uint8_t* ls)
{
    const uint32_t S = gpr[3]._u32[0], M = gpr[4]._u32[0], NM = gpr[5]._u32[0], IW = gpr[6]._u32[0];
    const uint32_t sp = gpr[1]._u32[0], sp2 = sp - 816;
#ifdef EDGE_HARNESS
    if (unlikely(!g_wtab[255])) { rsq_init(); wtab_init(); }
#endif
    if ((sp & 15) || sp < 0x400 || sp + 0x20 > SPU_LS_SIZE || (S & 15) || S + 0x90 > SPU_LS_SIZE) return 0;
    const Q s0 = lq(ls, S);
    const uint8_t b0 = s0._u32[0] >> 24, type = (s0._u32[1] >> 8) & 15, fmt = s0._u32[1] & 0xFF;
    const int body = !(b0 & 0x10) && NM && type;
    Skin k; memset(&k, 0, sizeof k);
    uint32_t n4 = 0, chunk = 4, nm_conv = 0, stride = 48;
    if (body) {
        if (!(b0 & 0x80)) return 0;                       /* the job's "no extra uniform table" error */
        const int nonuni = type == 3 || type == 6;
        const uint32_t s60 = be32(ls + S + 0x60);
        int n = s60 == 0 ? 0 : (nonuni ? 0 : 1);
        uint32_t ptr[9]; ptr[0] = s60;
        for (int i = 0; i < 4; i++) {
            const uint32_t a = word_at(ls, S + 4 * i + 100), b = word_at(ls, S + 4 * i + 116);
            ptr[n] = a; n += a != 0;
            ptr[n] = b; n += b != 0;
        }
        if (n > 3) return 0;                              /* "more than 3 uniforms" error */
        const uint32_t numv = (s0._u32[2] >> 16) & 0xFFFF;
        if (!numv) return 0;
        k.nst = n; for (int i = 0; i < n; i++) k.st[i] = ptr[i];
        k.r105 = nonuni && s60 != 0; k.s60 = s60;
        k.multi = (uint8_t)(type - 4) > 2;
        k.renorm = !(type == 4 || type == 1);
        k.M = M & LSQ; k.IW = IW; k.pos = word_at(ls, S + 0x5C);
        n4 = (numv + 3) & ~3u;
        k.scr = sp2 + 176;
        if (n4 > 11) {
            const uint32_t r39 = 4 * (b0 & 15) + S;
            k.scr = word_at(ls, r39 + 16);
            chunk = (uint32_t)(((uint64_t)n4 * 0xAAAAAAABull) >> 33) & ~3u;
        }
        if (fmt == 2) { nm_conv = NM; stride = 64; }
        else if (fmt <= 1) { nm_conv = NM; stride = fmt ? 64 : 48; }
        /* every buffer, for the overlap guard; the fast path also wants them 16-aligned */
        if ((k.pos | k.scr | k.s60 | k.st[0] | k.st[1] | k.st[2]) & 15) return 0;
        uint32_t maxi = 0;
        if (k.multi) {
            /* odd bytes of 8 per vertex, where the weight before them is not
             * zero: a zero weight makes the matrix irrelevant (fm(+0, x) is +0
             * and fma(+0, x, a) is fma(+0, 0, a) for every x, checked against
             * the helpers), and real meshes leave garbage indexes there */
            const uint8_t* p = ls + (IW & LSQ); __m256i m = _mm256_setzero_si256();
            for (uint32_t i = 0; i < 8 * n4; i += 32) {
                const __m256i h = _mm256_loadu_si256((const __m256i*)(p + i));
                m = _mm256_mask_max_epu16(m, _mm256_test_epi16_mask(h, _mm256_set1_epi16(0xFF)), m, _mm256_and_si256(h, _mm256_set1_epi16((short)0xFF00)));
            }
            uint16_t t[16]; _mm256_storeu_si256((__m256i*)t, m);
            for (int i = 0; i < 16; i++) maxi = t[i] > maxi ? t[i] : maxi;
            maxi >>= 8;
        } else if (!(IW & 1)) {                           /* bytes IW .. IW+n4-1 */
            for (uint32_t i = 0; i < n4; i++) { uint32_t x = ls[(IW + i) & LSM]; maxi = x > maxi ? x : maxi; }
        } else for (uint32_t i = 0; i < n4; i += 2) {   /* the pair reads wrap inside a quad */
            const uint32_t a = IW + i, x = ls[a & LSM], y = ls[((a & ~15u) + ((a + 1) & 15)) & LSM];
            maxi = x > maxi ? x : maxi; maxi = y > maxi ? y : maxi;
        }
        const uint32_t n8 = (numv + 7) & ~7u, nn = k.renorm ? n8 : n4;
        Span sp_[12]; int ns = 0;
        sp_[ns++] = (Span){ sp2, sp + 0x20, 1 };
        sp_[ns++] = (Span){ S, S + 0x90, 1 };
        /* the palette: converted in place, then read as 48-byte matrices. The
         * fast path loads 64 bytes per matrix and drops the last 16, so those
         * only have to be inside the LS (the next buffer usually starts there). */
        const uint32_t mhi = k.M + 48 * (maxi + 1), mc = k.M + stride * nm_conv;
        if (mhi + 16 > SPU_LS_SIZE) return 0;
        /* span 2: the conversion; span 3: what the blend reads, which can run
         * past the palette (padding vertexes carry garbage indexes) but is only read */
        sp_[ns++] = (Span){ k.M, mc > k.M ? mc : k.M + 1, nm_conv != 0 };
        sp_[ns++] = (Span){ k.M, mhi, 0 };
        sp_[ns++] = (Span){ IW & LSQ, k.multi ? (IW & LSQ) + 8 * n4 : IW + n4 + 2, 0 };
        sp_[ns++] = (Span){ k.pos, k.pos + 16 * n4, 1 };
        if (k.scr != sp2 + 176) sp_[ns++] = (Span){ k.scr, k.scr + 48 * chunk + 16, 1 };
        else if (k.scr + 48 * chunk > sp + 0x20) return 0;
        if (k.r105) sp_[ns++] = (Span){ k.s60, k.s60 + 16 * nn, 1 };
        for (int i = 0; i < k.nst; i++) sp_[ns++] = (Span){ k.st[i], k.st[i] + 16 * nn, 1 };
        /* every pair but the palette's two spans with each other */
        for (int i = 0; i < ns; i++)
            for (int j = i + 1; j < ns; j++) {
                if (i == 2 && j == 3) continue;
                const Span pr[2] = { sp_[i], sp_[j] };
                if (!spans_ok(pr, 2)) return 0;
            }
        k.maxi = maxi;
    }

    /* prologue: callee-saved registers, the link, the back chain */
    for (int r = 80; r <= 107; r++) sq(ls, sp - 16 * (r - 79), gpr[r]);
    sq(ls, sp + 0x10, gpr[0]);
    sq(ls, sp2, gpr[1]);
    if (body) {
        /* the palette into the job's layout, in place. Words only move, so
         * they move in LS byte order. */
        for (uint32_t m = 0; m < nm_conv; m++) {
            uint8_t* src = ls + k.M + stride * m;
            uint8_t* dst = ls + k.M + 48 * m;
            const __m128i a = _mm_loadu_si128((const __m128i*)src), b = _mm_loadu_si128((const __m128i*)(src + 16)),
                          c = _mm_loadu_si128((const __m128i*)(src + 32));
            __m128i q0, q1, q2;
            if (fmt == 2) {   /* (a0 b1 c2 d0) (b0 c1 a2 d1) (c0 a1 b2 d2) */
                const __m128i d = _mm_loadu_si128((const __m128i*)(src + 48));
                q0 = _mm_blend_epi32(_mm_blend_epi32(a, b, 2), _mm_blend_epi32(c, _mm_shuffle_epi32(d, 0x00), 8), 12);
                q1 = _mm_blend_epi32(_mm_blend_epi32(b, c, 2), _mm_blend_epi32(a, _mm_shuffle_epi32(d, 0x55), 8), 12);
                q2 = _mm_blend_epi32(_mm_blend_epi32(c, a, 2), _mm_blend_epi32(b, _mm_shuffle_epi32(d, 0xAA), 8), 12);
            } else {          /* (a0 b1 c2 a3) (a1 b2 c0 b3) (a2 b0 c1 c3) */
                q0 = _mm_blend_epi32(_mm_blend_epi32(a, b, 2), c, 4);
                q1 = _mm_blend_epi32(_mm_blend_epi32(_mm_shuffle_epi32(a, 0x01), _mm_shuffle_epi32(b, 0xE8), 10), _mm_shuffle_epi32(c, 0x00), 4);
                q2 = _mm_blend_epi32(_mm_blend_epi32(_mm_shuffle_epi32(a, 0x02), _mm_shuffle_epi32(b, 0x00), 2), _mm_shuffle_epi32(c, 0xD4), 12);
            }
            _mm_storeu_si128((__m128i*)dst, q0); _mm_storeu_si128((__m128i*)(dst + 16), q1); _mm_storeu_si128((__m128i*)(dst + 32), q2);
        }
        /* the stream table the job keeps on its stack */
        {
            int n = k.s60 == 0 ? 0 : ((type == 3 || type == 6) ? 0 : 1);
            sq(ls, sp2 + 0x20, lq(ls, S + 0x60));
            for (int i = 0; i < 4; i++) {
                const uint32_t a = S + 4 * i + 100, b = S + 4 * i + 116;
                const Q qa = quad_at(ls, a); sq(ls, sp2 + 32 + 16 * n, qa); n += qa._u32[0] != 0;
                const Q qb = quad_at(ls, b); sq(ls, sp2 + 32 + 16 * n, qb); n += qb._u32[0] != 0;
            }
        }
        /* the palette in host order for the fast path, and which matrices it may take */
        static _Thread_local Z pal[512];          /* 256..511 stay zero */
        uint8_t okm[512];
        memset(okm + 256, 1, 256);
        for (uint32_t m = 0; m <= k.maxi; m++) {
            Chk ck; chk_init(&ck);
            pal[m] = chk(ld12(ls + k.M + 48 * m), &ck);
            okm[m] = !chk_bad(&ck);
        }
        k.okm = okm; k.pal = pal;
        const uint32_t csr = _mm_getcsr();
        _mm_setcsr(CSR_FAST);
        k_skin_run[k.multi][k.renorm][k.r105][k.nst](ls, &k, n4, chunk, IW, csr);
        if (k.renorm) {
            const uint32_t numv = (be32(ls + S + 8) >> 16) & 0xFFFF, n8 = (numv + 7) & ~7u;
            /* 0x6E98 over each stream, as the job does once all chunks are skinned */
            for (int s = k.nst - 1; s >= 0; s--) normalize_ls(ls, k.st[s], 0, n8, csr);
            if (k.r105) normalize_ls(ls, k.s60, 0, n8, csr);
        }
        _mm_setcsr(csr);
        {
            Q q = lq(ls, S + 0x50);
            q._u32[((sp2 + 8) >> 2) & 3] = M > IW ? IW : M;
            sq(ls, S + 0x50, q);
        }
    }
    /* epilogue */
    for (int r = 80; r <= 107; r++) gpr[r] = lq(ls, sp - 16 * (r - 79));
    gpr[0] = lq(ls, sp + 0x10);
    return 1;
}


/* ================================================================== 0xA3C8: transform for culling
 *
 * r3 the job context. Each position goes through the world matrix (S+0xB0,
 * 3x4) and the viewport info's 4x4 matrix (S+0xF0), is divided by w (frest,
 * fi, one Newton step) and mapped to the screen (S+0x130 scales, S+0x140
 * offsets). The scratch (the extra uniform table) gets (sx, sy, code, 0) per
 * vertex, code the outcode bits (RESEARCH.md section 3). */
static const uint32_t k_rcp_lut[32] = {
    0x7FFBE0, 0x7F87A6, 0x70EF72, 0x708B40, 0x638B12, 0x633AEA, 0x5792C4, 0x574AA0,
    0x4CCA7E, 0x4C9262, 0x430A44, 0x42D62A, 0x3A2E12, 0x39FDFA, 0x3215E4, 0x31F1D2,
    0x2AA9BE, 0x2A85AC, 0x23D59A, 0x23BD8E, 0x1D8576, 0x1D8576, 0x17AD5A, 0x17AD5A,
    0x124543, 0x124543, 0x0D392D, 0x0D392D, 0x08851A, 0x08851A, 0x041D07, 0x041D07
};
static inline Z frest16(Z a)
{
    const Z e = _mm512_and_si512(_mm512_srli_epi32(a, 23), _mm512_set1_epi32(0xFF));
    const Z f = _mm512_permutex2var_epi32(_mm512_loadu_si512(k_rcp_lut), _mm512_srli_epi32(a, 18), _mm512_loadu_si512(k_rcp_lut + 16));
    Z rexp = _mm512_maskz_sub_epi32(_mm512_cmplt_epu32_mask(e, _mm512_set1_epi32(253)), _mm512_set1_epi32(253), e);
    rexp = _mm512_mask_mov_epi32(rexp, _mm512_testn_epi32_mask(e, e), _mm512_set1_epi32(255));
    return _mm512_or_si512(_mm512_and_si512(a, _mm512_set1_epi32((int)0x80000000u)),
                           _mm512_or_si512(_mm512_slli_epi32(rexp, 23), _mm512_and_si512(f, _mm512_set1_epi32(0x7FFFFF))));
}
/* fcgt's order: exponent-0 values are 0, the rest order as sign-magnitude */
static inline Z fkey16(Z x)
{
    const Z m = _mm512_maskz_and_epi32(_mm512_test_epi32_mask(x, _mm512_set1_epi32(0x7F800000)), x, _mm512_set1_epi32(0x7FFFFFFF));
    return _mm512_mask_sub_epi32(m, _mm512_movepi32_mask(x), _mm512_setzero_si512(), m);
}
static inline int32_t fkey(uint32_t x) { int32_t m = (x & 0x7F800000u) ? (int32_t)(x & 0x7FFFFFFFu) : 0; return (x >> 31) ? -m : m; }

typedef struct {
    uint32_t pos, scr, n4;
    Q P[4], sc, of;                 /* P = view-projection x world, exact */
    uint32_t x0, y0, x1, y1, dmin, dmax;
} Xf;

/* vertices 4g..4g+3, exactly as the job computes them (its own lane layout) */
static NOINL void xf_group_q(uint8_t* ls, const Xf* k, uint32_t g)
{
    Q v[4];
    for (int j = 0; j < 4; j++) v[j] = lq(ls, k->pos + 64 * g + 16 * j);
    const Q X = V4(v[0]._u32[0], v[1]._u32[0], v[2]._u32[0], v[3]._u32[0]);
    const Q Y = V4(v[0]._u32[1], v[1]._u32[1], v[2]._u32[1], v[3]._u32[1]);
    const Q Zc = V4(v[0]._u32[2], v[1]._u32[2], v[2]._u32[2], v[3]._u32[2]);
    Q c[4];
    for (int r = 0; r < 4; r++)
        c[r] = spu_fma(Zc, spl(k->P[r], 2), spu_fma(Y, spl(k->P[r], 1), spu_fma(X, spl(k->P[r], 0), spl(k->P[r], 3))));
    const Q W = c[3];
    const Q r10 = spu_fi(W, spu_frest(W));
    const Q iw = spu_fma(spu_fnms(r10, W, V4(F1, F1, F1, F1)), r10, r10);
    Q o[3];
    for (int i = 0; i < 3; i++) {
        Q t = spu_fm(c[i], iw);
        for (int j = 0; j < 4; j++) t._u32[j] ^= W._u32[j] & 0x80000000u;
        o[i] = spu_fma(t, spl(k->sc, i), spl(k->of, i));
    }
    for (int j = 0; j < 4; j++) {
        const int32_t sx = fkey(o[0]._u32[j]), sy = fkey(o[1]._u32[j]), sz = fkey(o[2]._u32[j]);
        const uint32_t code = (sx > fkey(k->x1)) << 7 | (fkey(k->x0) > sx) << 6 | (sy > fkey(k->y1)) << 5 | (fkey(k->y0) > sy) << 4 |
                              (sz > fkey(k->dmax)) << 3 | (fkey(k->dmin) > sz) << 2 | (fkey(W._u32[j]) > 0) << 1;
        sq(ls, k->scr + 64 * g + 16 * j, V4(o[0]._u32[j], o[1]._u32[j], code, 0));
    }
}

/* sixteen vertices from 16*b (n of them valid, a multiple of 4); 0: do them exactly */
static inline __attribute__((always_inline)) int xf_batch16(uint8_t* ls, const Xf* k, uint32_t b, uint32_t n)
{
    const __mmask16 lanes = (__mmask16)(n >= 16 ? 0xFFFF : (1u << n) - 1);
    Z V[4];
    for (int m = 0; m < 4; m++) V[m] = 4 * m < (int)n ? ld16(ls, k->pos + 256 * b + 64 * m) : _mm512_setzero_si512();
    Z C[3];
    for (int c = 0; c < 3; c++) {
        const Z ix = _mm512_add_epi32(_mm512_setr_epi32(0, 4, 8, 12, 16, 20, 24, 28, 0, 4, 8, 12, 16, 20, 24, 28), _mm512_set1_epi32(c));
        C[c] = _mm512_mask_mov_epi32(_mm512_permutex2var_epi32(V[0], ix, V[1]), 0xFF00, _mm512_permutex2var_epi32(V[2], ix, V[3]));
    }
    Z mi = _mm512_setzero_si512(), mo = _mm512_setzero_si512();
    for (int c = 0; c < 3; c++) mi = _mm512_mask_max_epu32(mi, lanes, mi, _mm512_and_si512(C[c], _mm512_set1_epi32(0x7FFFFFFF)));
    Z c[4];
    for (int r = 0; r < 4; r++)
        c[r] = FMA(C[2], _mm512_set1_epi32(k->P[r]._u32[2]), FMA(C[1], _mm512_set1_epi32(k->P[r]._u32[1]),
                   FMA(C[0], _mm512_set1_epi32(k->P[r]._u32[0]), _mm512_set1_epi32(k->P[r]._u32[3]))));
    const Z W = canon(c[3]);
    const Z r10 = fi16(W, frest16(W));
    const Z iw = FMA(zi(_mm512_fnmadd_ps(zf(r10), zf(W), _mm512_set1_ps(1.0f))), r10, r10);
    mo = _mm512_mask_max_epu32(mo, lanes, mo, _mm512_and_si512(r10, _mm512_set1_epi32(0x7FFFFFFF)));
    mo = _mm512_mask_max_epu32(mo, lanes, mo, _mm512_and_si512(iw, _mm512_set1_epi32(0x7FFFFFFF)));
    const Z sgn = _mm512_and_si512(W, _mm512_set1_epi32((int)0x80000000u));
    Z o[3];
    for (int i = 0; i < 3; i++) {
        const Z t = _mm512_xor_si512(canon(FM(c[i], iw)), sgn);
        o[i] = canon(FMA(t, _mm512_set1_epi32(k->sc._u32[i]), _mm512_set1_epi32(k->of._u32[i])));
        mo = _mm512_mask_max_epu32(mo, lanes, mo, _mm512_and_si512(t, _mm512_set1_epi32(0x7FFFFFFF)));
        mo = _mm512_mask_max_epu32(mo, lanes, mo, _mm512_and_si512(o[i], _mm512_set1_epi32(0x7FFFFFFF)));
    }
    if (unlikely((_mm512_cmpge_epu32_mask(mi, _mm512_set1_epi32(0x5D800000)) | _mm512_cmpge_epu32_mask(mo, _mm512_set1_epi32(0x7F7FFFFF))) != 0)) return 0;
    const Z kx = fkey16(o[0]), ky = fkey16(o[1]), kz = fkey16(o[2]);
    Z code = _mm512_maskz_mov_epi32(_mm512_cmpgt_epi32_mask(kx, _mm512_set1_epi32(fkey(k->x1))), _mm512_set1_epi32(0x80));
    code = _mm512_mask_or_epi32(code, _mm512_cmpgt_epi32_mask(_mm512_set1_epi32(fkey(k->x0)), kx), code, _mm512_set1_epi32(0x40));
    code = _mm512_mask_or_epi32(code, _mm512_cmpgt_epi32_mask(ky, _mm512_set1_epi32(fkey(k->y1))), code, _mm512_set1_epi32(0x20));
    code = _mm512_mask_or_epi32(code, _mm512_cmpgt_epi32_mask(_mm512_set1_epi32(fkey(k->y0)), ky), code, _mm512_set1_epi32(0x10));
    code = _mm512_mask_or_epi32(code, _mm512_cmpgt_epi32_mask(kz, _mm512_set1_epi32(fkey(k->dmax))), code, _mm512_set1_epi32(0x08));
    code = _mm512_mask_or_epi32(code, _mm512_cmpgt_epi32_mask(_mm512_set1_epi32(fkey(k->dmin)), kz), code, _mm512_set1_epi32(0x04));
    code = _mm512_mask_or_epi32(code, _mm512_cmpgt_epi32_mask(fkey16(W), _mm512_setzero_si512()), code, _mm512_set1_epi32(0x02));
    /* back to one quad per vertex: q[i] block j = vertex 4j+i, then a 4x4 block transpose */
    const Z xy_lo = _mm512_unpacklo_epi32(o[0], o[1]), xy_hi = _mm512_unpackhi_epi32(o[0], o[1]);
    const Z c_lo = _mm512_unpacklo_epi32(code, _mm512_setzero_si512()), c_hi = _mm512_unpackhi_epi32(code, _mm512_setzero_si512());
    const Z q0 = _mm512_unpacklo_epi64(xy_lo, c_lo), q1 = _mm512_unpackhi_epi64(xy_lo, c_lo);
    const Z q2 = _mm512_unpacklo_epi64(xy_hi, c_hi), q3 = _mm512_unpackhi_epi64(xy_hi, c_hi);
    const Z t0 = _mm512_shuffle_i64x2(q0, q1, 0x44), t1 = _mm512_shuffle_i64x2(q2, q3, 0x44);
    const Z t2 = _mm512_shuffle_i64x2(q0, q1, 0xEE), t3 = _mm512_shuffle_i64x2(q2, q3, 0xEE);
    const Z out[4] = { _mm512_shuffle_i64x2(t0, t1, 0x88), _mm512_shuffle_i64x2(t0, t1, 0xDD),
                       _mm512_shuffle_i64x2(t2, t3, 0x88), _mm512_shuffle_i64x2(t2, t3, 0xDD) };
    for (int m = 0; 4 * m < (int)n && m < 4; m++) st16(ls, k->scr + 256 * b + 64 * m, out[m]);
    return 1;
}

/* the exact path for a batch, at the caller's MXCSR */
static NOINL void xf_slow(uint8_t* ls, const Xf* k, uint32_t b, uint32_t n, uint32_t csr)
{
    EDGE_STAT(3);
    _mm_setcsr(csr);
    for (uint32_t g = 0; g < n / 4; g++) xf_group_q(ls, k, 4 * b + g);
    _mm_setcsr(CSR_FAST);
}
/* MXCSR is CSR_FAST */
static NOINL void xf_run(uint8_t* ls, const Xf* k, uint32_t csr)
{
    for (uint32_t b = 0; 16 * b < k->n4; b++) {
        const uint32_t n = k->n4 - 16 * b > 16 ? 16 : k->n4 - 16 * b;
        if (unlikely(!xf_batch16(ls, k, b, n))) xf_slow(ls, k, b, n, csr);
    }
}

/* 0 if the job's code has to run; nothing written */
static int xf_prepare(const u128* gpr, const uint8_t* ls, Xf* k)
{
    const uint32_t S = gpr[3]._u32[0], sp = gpr[1]._u32[0];
    if ((sp & 15) || sp < 0x100 || sp > SPU_LS_SIZE || (S & 15) || S + 0x150 > SPU_LS_SIZE) return 0;
    const Q s0 = lq(ls, S);
    const uint32_t numv = (s0._u32[2] >> 16) & 0xFFFF;
    if (!numv) return 0;                                  /* the pipelined loop would store its fill-in */
    k->n4 = ((numv + 3) >> 2) << 2;
    k->pos = lq(ls, S + 0x50)._u32[3];
    k->scr = word_at(ls, S + 4 * (s0._u32[0] >> 24 & 15) + 16);
    if ((k->pos & 15) || (k->scr & 15)) return 0;
    const uint32_t nr = (k->n4 + 15) & ~15u;              /* the fast path reads whole batches */
    Span sp_[4];
    sp_[0] = (Span){ sp - 0x30, sp, 1 };
    sp_[1] = (Span){ S, S + 0x150, 0 };
    sp_[2] = (Span){ k->pos, k->pos + 16 * nr, 0 };
    sp_[3] = (Span){ k->scr, k->scr + 16 * k->n4, 1 };
    return spans_ok(sp_, 4);
}
/* the matrices and bounds as the job's prologue makes them, the frame, every vertex */
static void xf_exec(u128* gpr, uint8_t* ls, Xf* k)
{
    const uint32_t S = gpr[3]._u32[0], sp = gpr[1]._u32[0];
    const Q n0 = lq(ls, S + 0xB0), n1 = lq(ls, S + 0xC0), n2 = lq(ls, S + 0xD0), n3 = V4(0, 0, 0, F1);
    for (int r = 0; r < 4; r++) {
        const Q m = lq(ls, S + 0xF0 + 16 * r);
        k->P[r] = spu_fma(spl(m, 3), n3, spu_fma(spl(m, 2), n2, spu_fma(spl(m, 1), n1, spu_fm(spl(m, 0), n0))));
    }
    k->sc = lq(ls, S + 0x130); k->of = lq(ls, S + 0x140);
    const Q e = lq(ls, S + 0xE0);
    const uint32_t h0 = e._u32[0] >> 16, h1 = e._u32[0] & 0xFFFF, h2 = e._u32[1] >> 16, h3 = e._u32[1] & 0xFFFF;
    const Q bf = spu_cuflt(V4(h0, h1, (h0 + h2) & 0xFFFF, (h1 + h3) & 0xFFFF), 155);
    k->x0 = bf._u32[0]; k->y0 = bf._u32[1]; k->x1 = bf._u32[2]; k->y1 = bf._u32[3];
    const int gt = fkey(e._u32[2]) > fkey(e._u32[3]);
    k->dmin = gt ? e._u32[3] : e._u32[2]; k->dmax = gt ? e._u32[2] : e._u32[3];
    sq(ls, sp - 0x10, gpr[80]);
    sq(ls, sp - 0x30, gpr[1]);
    /* the fast path takes these as operands unchecked */
    int ok = 1;
    for (int r = 0; r < 4 && ok; r++) for (int i = 0; i < 4; i++) if ((k->P[r]._u32[i] & 0x7FFFFFFFu) >= 0x5D800000u) ok = 0;
    for (int i = 0; i < 3 && ok; i++)
        if ((k->sc._u32[i] & 0x7FFFFFFFu) >= 0x5D800000u || (k->of._u32[i] & 0x7FFFFFFFu) >= 0x5D800000u) ok = 0;
    if (ok) {
        const uint32_t csr = _mm_getcsr();
        _mm_setcsr(CSR_FAST);
        xf_run(ls, k, csr);
        _mm_setcsr(csr);
    } else
        for (uint32_t g = 0; g < k->n4 / 4; g++) xf_group_q(ls, k, g);
    gpr[80] = lq(ls, sp - 0x10);
}
static int edge_xf_core(u128* gpr, uint8_t* ls)
{
    Xf k;
    if (!xf_prepare(gpr, ls, &k)) return 0;
    xf_exec(gpr, ls, &k);
    return 1;
}

/* ================================================================== 0xA798: triangle culling
 *
 * r3 the job context, r4 the cull mode, r5 the first vertex of the records.
 * Transforms the vertices (0xA3C8, natively), then tests each triangle of the
 * index buffer (S+0x50, u16 x3 each) against the records and compacts the
 * survivors in place, four triangles per step (RESEARCH.md section 4). */
static void cull_tables_init(void);
static uint8_t g_fnms_lsb[8];               /* LSB of fnms(a, b, c) on masks: [a<<2 | b<<1 | c] */
static int g_fast_cull;                     /* the table is c | (a & b), as cull_batch16 assumes */
static void fnms_init(void)
{
    for (int i = 0; i < 8; i++) {
        const uint32_t a = (i & 4) ? ~0u : 0, b = (i & 2) ? ~0u : 0, c = (i & 1) ? ~0u : 0;
        g_fnms_lsb[i] = spu_fnms(V4(a, a, a, a), V4(b, b, b, b), V4(c, c, c, c))._u32[0] & 1;
    }
    cull_tables_init();
    g_fast_cull = 1;
    for (int i = 0; i < 8; i++) if (g_fnms_lsb[i] != ((i & 1) | ((i >> 2) & (i >> 1) & 1))) g_fast_cull = 0;
}
typedef struct {
    uint32_t B, nt, ng, end, rec, sp2, snap, wind, m65, kept;
    uint8_t* idx;                            /* the index buffer as it was */
} Cull;
static inline uint32_t be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }

/* the cull bits of group g (bit 3 = its first triangle), exactly: the job's lanes are its four triangles */
static NOINL unsigned cull_group_q(const uint8_t* ls, const Cull* k, uint32_t g)
{
    Q r[3][4];                               /* [vertex of the triangle][triangle] */
    for (int t = 0; t < 4; t++)
        for (int v = 0; v < 3; v++) r[v][t] = lq(ls, k->rec + 16 * be16(k->idx + 24 * g + 6 * t + 2 * v));
    Q sx[3], sy[3];
    uint32_t orc[4], andc[4];
    for (int v = 0; v < 3; v++) {
        sx[v] = V4(r[v][0]._u32[0], r[v][1]._u32[0], r[v][2]._u32[0], r[v][3]._u32[0]);
        sy[v] = V4(r[v][0]._u32[1], r[v][1]._u32[1], r[v][2]._u32[1], r[v][3]._u32[1]);
    }
    for (int t = 0; t < 4; t++) { orc[t] = r[0][t]._u32[2] | r[1][t]._u32[2] | r[2][t]._u32[2]; andc[t] = r[0][t]._u32[2] & r[1][t]._u32[2] & r[2][t]._u32[2]; }
    const Q area = spu_fms(spu_fs(sx[1], sx[0]), spu_fs(sy[2], sy[0]), spu_fm(spu_fs(sy[1], sy[0]), spu_fs(sx[2], sx[0])));
    const Q S4 = V4(k->snap, k->snap, k->snap, k->snap), H = V4(0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u);
    Q X[3], Y[3];
    for (int v = 0; v < 3; v++) { X[v] = spu_cflts(spu_fma(sx[v], S4, H), 173); Y[v] = spu_cflts(spu_fma(sy[v], S4, H), 173); }
    unsigned m = 0;
    for (int t = 0; t < 4; t++) {
        const int valid = !(orc[t] & 4) && (int32_t)(andc[t] & 2) > 0;
        const int outside = (andc[t] & 252) != 0;
        const int front = fkey(area._u32[t] ^ k->wind) > 0;
        const int nocover = (X[0]._u32[t] == X[1]._u32[t] && X[1]._u32[t] == X[2]._u32[t]) ||
                            (Y[0]._u32[t] == Y[1]._u32[t] && Y[1]._u32[t] == Y[2]._u32[t]);
        const int cull = (valid && nocover) | g_fnms_lsb[(valid && front) << 2 | k->m65 << 1 | outside];
        m |= (unsigned)cull << (3 - t);
    }
    return m;
}

/* sixteen triangles (groups 4b..4b+3) at once; MXCSR is CSR_FAST. 0: the batch needs the exact way.
 * A triangle whose projection is not valid is culled or kept by its outcodes
 * alone, so its area and snapped corners are not checked (nor used). */
static inline __attribute__((always_inline)) int cull_batch16(const uint8_t* ls, const Cull* k, uint32_t b, uint16_t* masks)
{
    const uint8_t* ip = k->idx + 96 * b;
    const Z w0 = _mm512_loadu_si512(ip), w1 = _mm512_loadu_si512(ip + 64);
    Z sx[3], sy[3], cd[3];
    for (int v = 0; v < 3; v++) {
        const Z wi = _mm512_add_epi16(_mm512_setr_epi32(0, 3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36, 39, 42, 45), _mm512_set1_epi32(v));
        Z ix = _mm512_and_si512(_mm512_permutex2var_epi16(w0, wi, w1), _mm512_set1_epi32(0xFFFF));
        ix = _mm512_or_si512(_mm512_srli_epi32(ix, 8), _mm512_and_si512(_mm512_slli_epi32(ix, 8), _mm512_set1_epi32(0xFF00)));
        uint32_t a[16];
        _mm512_storeu_si512(a, _mm512_and_si512(_mm512_add_epi32(_mm512_set1_epi32(k->rec), _mm512_slli_epi32(ix, 4)), _mm512_set1_epi32(LSQ)));
        /* the records, four per register: R[m] block j = record of triangle 4m+j */
        Z R[4];
        for (int m = 0; m < 4; m++) {
            Z r = _mm512_castsi128_si512(_mm_loadu_si128((const __m128i*)(ls + a[4 * m])));
            r = _mm512_inserti32x4(r, _mm_loadu_si128((const __m128i*)(ls + a[4 * m + 1])), 1);
            r = _mm512_inserti32x4(r, _mm_loadu_si128((const __m128i*)(ls + a[4 * m + 2])), 2);
            R[m] = bswap16(_mm512_inserti32x4(r, _mm_loadu_si128((const __m128i*)(ls + a[4 * m + 3])), 3));
        }
        const Z ix0 = _mm512_setr_epi32(0, 4, 8, 12, 16, 20, 24, 28, 0, 4, 8, 12, 16, 20, 24, 28);
        sx[v] = _mm512_mask_mov_epi32(_mm512_permutex2var_epi32(R[0], ix0, R[1]), 0xFF00, _mm512_permutex2var_epi32(R[2], ix0, R[3]));
        const Z ix1 = _mm512_add_epi32(ix0, _mm512_set1_epi32(1)), ix2 = _mm512_add_epi32(ix0, _mm512_set1_epi32(2));
        sy[v] = _mm512_mask_mov_epi32(_mm512_permutex2var_epi32(R[0], ix1, R[1]), 0xFF00, _mm512_permutex2var_epi32(R[2], ix1, R[3]));
        cd[v] = _mm512_mask_mov_epi32(_mm512_permutex2var_epi32(R[0], ix2, R[1]), 0xFF00, _mm512_permutex2var_epi32(R[2], ix2, R[3]));
    }
    const Z orc = _mm512_or_si512(_mm512_or_si512(cd[0], cd[1]), cd[2]), andc = _mm512_and_si512(_mm512_and_si512(cd[0], cd[1]), cd[2]);
    const __mmask16 valid = _mm512_testn_epi32_mask(orc, _mm512_set1_epi32(4)) & _mm512_cmpgt_epi32_mask(_mm512_and_si512(andc, _mm512_set1_epi32(2)), _mm512_setzero_si512());
    const __mmask16 outside = _mm512_test_epi32_mask(andc, _mm512_set1_epi32(252));
    Z mx = _mm512_setzero_si512();
    for (int v = 0; v < 3; v++) {
        mx = _mm512_mask_max_epu32(mx, valid, mx, _mm512_and_si512(sx[v], _mm512_set1_epi32(0x7FFFFFFF)));
        mx = _mm512_mask_max_epu32(mx, valid, mx, _mm512_and_si512(sy[v], _mm512_set1_epi32(0x7FFFFFFF)));
    }
    if (unlikely(_mm512_cmpge_epu32_mask(mx, _mm512_set1_epi32(0x4E000000)))) return 0;   /* 2^29: the snap stays an int32 */
    const Z area = FMS(zi(_mm512_sub_ps(zf(sx[1]), zf(sx[0]))), zi(_mm512_sub_ps(zf(sy[2]), zf(sy[0]))),
                       FM(zi(_mm512_sub_ps(zf(sy[1]), zf(sy[0]))), zi(_mm512_sub_ps(zf(sx[2]), zf(sx[0])))));
    const __mmask16 front = _mm512_cmpgt_epi32_mask(fkey16(_mm512_xor_si512(area, _mm512_set1_epi32(k->wind))), _mm512_setzero_si512());
    Z X[3], Y[3];
    for (int v = 0; v < 3; v++) {
        X[v] = _mm512_cvttps_epi32(_mm512_fmadd_ps(zf(sx[v]), _mm512_set1_ps(k->snap == F1 ? 1.0f : 2.0f), _mm512_set1_ps(0.5f)));
        Y[v] = _mm512_cvttps_epi32(_mm512_fmadd_ps(zf(sy[v]), _mm512_set1_ps(k->snap == F1 ? 1.0f : 2.0f), _mm512_set1_ps(0.5f)));
    }
    const __mmask16 nocover = (_mm512_cmpeq_epi32_mask(X[0], X[1]) & _mm512_cmpeq_epi32_mask(X[1], X[2])) |
                              (_mm512_cmpeq_epi32_mask(Y[0], Y[1]) & _mm512_cmpeq_epi32_mask(Y[1], Y[2]));
    const __mmask16 cull = (valid & nocover) | outside | (k->m65 ? (valid & front) : 0);
    /* lane 4g+t is triangle t of group g; the job's bits run the other way */
    for (int g = 0; g < 4; g++) {
        const unsigned c4 = (cull >> (4 * g)) & 15;
        masks[g] = (uint16_t)((c4 & 1) << 3 | (c4 & 2) << 1 | (c4 & 4) >> 1 | (c4 & 8) >> 3);
    }
    return 1;
}

/* the job's stores for one step: m the cull bits of the group (15: nothing kept) */
/* pshufb controls for the stores: the kept entries of a group (from its two
 * quads), and the views of them shifted by the output's offset in its quad */
static uint8_t g_cmp[16][4][16];             /* [mask][comp1 from G0, comp1 from G1, comp2 from G0, comp2 from G1] */
static uint8_t g_shl[16][16], g_hi[16][16], g_rot[16][16];
static void cull_tables_init(void)
{
    for (int m = 0; m < 16; m++) {
        int src[32], n = 0;
        for (int t = 0; t < 4; t++) if (!(m >> (3 - t) & 1)) for (int j = 0; j < 6; j++) src[n++] = 6 * t + j;
        for (; n < 32; n++) src[n] = -1;
        for (int j = 0; j < 16; j++) {
            g_cmp[m][0][j] = src[j] >= 0 && src[j] < 16 ? src[j] : 0x80;
            g_cmp[m][1][j] = src[j] >= 16 ? src[j] - 16 : 0x80;
            g_cmp[m][2][j] = src[16 + j] >= 0 && src[16 + j] < 16 ? src[16 + j] : 0x80;
            g_cmp[m][3][j] = src[16 + j] >= 16 ? src[16 + j] - 16 : 0x80;
        }
    }
    for (int off = 0; off < 16; off++)
        for (int i = 0; i < 16; i++) {
            g_shl[off][i] = i >= off ? i - off : 0x80;
            g_hi[off][i] = i < off ? 16 - off + i : 0x80;
            g_rot[off][i] = (i - off) & 15;
        }
}
static inline __m128i ld128(const uint8_t* p) { return _mm_loadu_si128((const __m128i*)p); }
static inline void cull_store(uint8_t* ls, Cull* k, uint32_t g, unsigned m)
{
    const __m128i G0 = ld128(k->idx + 24 * g), G1 = ld128(k->idx + 24 * g + 16);
    const __m128i c1 = _mm_or_si128(_mm_shuffle_epi8(G0, ld128(g_cmp[m][0])), _mm_shuffle_epi8(G1, ld128(g_cmp[m][1])));
    const __m128i c2 = _mm_or_si128(_mm_shuffle_epi8(G0, ld128(g_cmp[m][2])), _mm_shuffle_epi8(G1, ld128(g_cmp[m][3])));
    const uint32_t ptr = k->B + 6 * k->kept, off = ptr & 15;
    const __m128i lt = _mm_cmpgt_epi8(_mm_set1_epi8((char)off), _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15));
    const __m128i shl = ld128(g_shl[off]);
    uint8_t* q0 = ls + (ptr & LSQ);
    _mm_storeu_si128((__m128i*)q0, _mm_blendv_epi8(_mm_shuffle_epi8(c1, shl), ld128(q0), lt));
    _mm_storeu_si128((__m128i*)(ls + ((((int32_t)ptr > (int32_t)(k->end - 17) ? k->sp2 + 32 : ptr) + 16) & LSQ)),
                     _mm_or_si128(_mm_shuffle_epi8(c1, ld128(g_hi[off])), _mm_shuffle_epi8(c2, shl)));
    _mm_storeu_si128((__m128i*)(ls + ((((int32_t)ptr > (int32_t)(k->end - 33) ? k->sp2 + 32 : ptr) + 32) & LSQ)),
                     _mm_shuffle_epi8(c2, ld128(g_rot[off])));
    k->kept += 4 - __builtin_popcount(m);
}

/* a group the exact way, at the caller's MXCSR */
static NOINL unsigned cull_slow(const uint8_t* ls, const Cull* k, uint32_t g, uint32_t csr)
{
    _mm_setcsr(csr);
    const unsigned m = cull_group_q(ls, k, g);
    _mm_setcsr(CSR_FAST);
    return m;
}
/* every group; MXCSR is CSR_FAST */
static NOINL void cull_run(uint8_t* ls, Cull* k, uint32_t csr)
{
    const unsigned last = (k->nt & 3) ? 15u >> (k->nt & 3) : 0;
    for (uint32_t g = 0; g < k->ng; ) {
        uint16_t m4[4];
        if (g + 4 <= k->ng && g_fast_cull && likely(cull_batch16(ls, k, g / 4, m4))) {
            for (int j = 0; j < 4; j++, g++) cull_store(ls, k, g, g + 1 == k->ng ? (m4[j] | last) : m4[j]);
            continue;
        }
        if (g + 4 <= k->ng && g_fast_cull) EDGE_STAT(4);
        unsigned m = cull_slow(ls, k, g, csr);
        if (g + 1 == k->ng) m |= last;
        cull_store(ls, k, g, m);
        g++;
    }
}

static int edge_cull_core(u128* gpr, uint8_t* ls)
{
    const uint32_t S = gpr[3]._u32[0], R4 = gpr[4]._u32[0], R5 = gpr[5]._u32[0], sp = gpr[1]._u32[0], sp2 = sp - 128;
    if ((sp & 15) || sp < 0x200 || sp + 0x20 > SPU_LS_SIZE || (S & 15) || S + 0x1B0 > SPU_LS_SIZE) return 0;
    const Q s0 = lq(ls, S);
    Cull k; memset(&k, 0, sizeof k);
    Xf x;
    u128 g2[128];
    uint32_t r10 = 0;
    if (R4) {
        if (!(s0._u32[0] >> 31)) return 0;                     /* the job's "culling without ..." error */
        if (word_at(ls, S + 0x194)) return 0;                  /* a custom transform: SPU code of the game's */
        memcpy(g2, gpr, sizeof g2);
        for (int i = 0; i < 4; i++) g2[1]._u32[i] = gpr[1]._u32[i] - 128;
        g2[80] = gpr[4];
        if (!xf_prepare(g2, ls, &x)) return 0;
        const uint32_t mode = lq(ls, S + 0x150)._u32[0] >> 24;
        r10 = R4 | (mode == 5 ? 0x80000000u : 0);
        if (r10 >= 1 && r10 <= 3) {
            k.B = word_at(ls, S + 0x50);
            if (k.B & 15) return 0;
            const uint32_t nidx = lq(ls, S + 0x1A0)._u32[0] >> 16;   /* the halfword the count goes back to */
            k.nt = (uint32_t)(((uint64_t)nidx * 0xAAAAAAABull) >> 33);
            k.ng = (k.nt + 3) >> 2;
            k.end = k.B + ((6 * k.nt + 15) & ~15u);
            k.rec = x.scr + 16 * R5;
            k.sp2 = sp2;
            k.snap = (mode == 3 || mode == 4) ? 0x40000000u : F1;
            const uint32_t flavor = (s0._u32[1] >> 12) & 15;
            k.wind = (r10 == 2 ? 0 : 0x80000000u) ^ ((flavor == 0 || flavor == 2) ? 0x80000000u : 0);
            k.m65 = r10 >= 2;
            /* the records the triangles name */
            uint32_t maxi = 0;
            for (uint32_t i = 0; i < 3 * k.nt; i++) { const uint32_t v = be16(ls + ((k.B + 2 * i) & LSM)); maxi = v > maxi ? v : maxi; }
            if (k.B + 24 * k.ng + 64 > SPU_LS_SIZE || k.rec + 16 * (maxi + 1) > SPU_LS_SIZE) return 0;
            /* the records are the transform's output: they may overlap it, nothing else */
            const Span w1[5] = { { sp2 - 0x30, sp + 0x20, 1 }, { S, S + 0x1B0, 1 }, { k.B, k.B + 24 * k.ng + 48, 1 },
                                 { x.pos, x.pos + 16 * ((x.n4 + 15) & ~15u), 0 }, { x.scr, x.scr + 16 * x.n4, 1 } };
            const Span w2[4] = { { sp2 - 0x30, sp + 0x20, 1 }, { S, S + 0x1B0, 1 }, { k.B, k.B + 24 * k.ng + 48, 1 },
                                 { k.rec, k.rec + 16 * (maxi + 1), 0 } };
            if (!spans_ok(w1, 5) || !spans_ok(w2, 4)) return 0;
        }
    }
    /* prologue */
    sq(ls, sp - 0x10, gpr[80]); sq(ls, sp - 0x20, gpr[81]); sq(ls, sp - 0x30, gpr[82]);
    sq(ls, sp + 0x10, gpr[0]); sq(ls, sp - 0x80, gpr[1]);
    Q r3;
    if (!R4) {
        if (word_at(ls, S + 0xC) != 0xFFFFFFFFu) {
            const Q v = lq(ls, S + 0x50);
            Q q = lq(ls, S + 0x50); q._u32[((sp2 + 8) >> 2) & 3] = v._u32[0]; sq(ls, S + 0x50, q);
        }
        const Q h = quad_at(ls, S + 0x1A0 + 14);
        r3 = V4(h._u32[0] & 0xFFFF, h._u32[1] & 0xFFFF, h._u32[2] & 0xFFFF, h._u32[3] & 0xFFFF);
    } else {
        xf_exec(g2, ls, &x);
        if (r10 >= 1 && r10 <= 3) {
            static _Thread_local uint8_t idx[24 * 5462 + 128];   /* 65535 indexes at most */
            memcpy(idx, ls + k.B, 24 * k.ng + 64);
            k.idx = idx;
            cull_store(ls, &k, 0, 15);                          /* the pipeline's two empty steps */
            cull_store(ls, &k, 0, 15);
            const uint32_t csr = _mm_getcsr();
            _mm_setcsr(CSR_FAST);
            cull_run(ls, &k, csr);
            _mm_setcsr(csr);
        }
        const uint32_t n = 3 * k.kept;
        Q h = lq(ls, S + 0x1A0);
        {
            uint8_t b[16]; for (int i = 0; i < 4; i++) { b[4 * i] = h._u32[i] >> 24; b[4 * i + 1] = h._u32[i] >> 16; b[4 * i + 2] = h._u32[i] >> 8; b[4 * i + 3] = h._u32[i]; }
            const int slot = (sp2 & 0xE);
            b[slot] = n >> 8; b[slot + 1] = n;
            sq(ls, S + 0x1A0, V4(be32(b), be32(b + 4), be32(b + 8), be32(b + 12)));
        }
        const uint32_t B = word_at(ls, S + 0x50);
        Q q = lq(ls, S + 0x50); q._u32[((sp2 + 8) >> 2) & 3] = (B + 6 * k.kept + 15) & ~15u; sq(ls, S + 0x50, q);
        r3 = V4(n, n, n, n);
    }
    gpr[3] = r3;
    gpr[80] = lq(ls, sp - 0x10); gpr[81] = lq(ls, sp - 0x20); gpr[82] = lq(ls, sp - 0x30);
    gpr[0] = lq(ls, sp + 0x10);
    return 1;
}

/* ================================================================== the hooks
 *
 * A kernel is int core(u128* gpr, uint8_t* ls): it reads and writes only the
 * registers and the local store it is given, and returns 0 (having written
 * nothing) where the lifted function has to run. */
typedef int (*EdgeCore)(u128* gpr, uint8_t* ls);
typedef struct {
    uint32_t addr; const char* name; EdgeCore core;
    unsigned long long calls, native, fallback, cmp, bad, ns_nat, ns_spu, captured;
} EdgeKernel;
static EdgeKernel g_kern[] = {
    { 0xD0A8, "skin", edge_skin_core, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0xA3C8, "cull transform", edge_xf_core, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0xA798, "cull", edge_cull_core, 0, 0, 0, 0, 0, 0, 0, 0 },
};
#define NKERN ((int)(sizeof g_kern / sizeof g_kern[0]))

#ifdef EDGE_HARNESS
int edge_skin_native(spu_context* c) { return edge_skin_core(c->gpr, c->ls); }
EdgeCore edge_core_for(uint32_t addr)
{
    if (!g_wtab[255]) { rsq_init(); wtab_init(); fnms_init(); }
    for (int i = 0; i < NKERN; i++) if (g_kern[i].addr == addr) return g_kern[i].core;
    return 0;
}
#endif

#ifndef EDGE_HARNESS
void (*spu_lookup(uint32_t addr, int image_id))(spu_context*);
static int s_mode = -1;                 /* PS3RECOMP_NATIVE_EDGE */
static const char* s_cap_dir;           /* PS3RECOMP_EDGE_CAPTURE=dir[,count[,every]] */
static unsigned s_cap_max = 32, s_cap_every = 997;
static _Thread_local int t_bypass;
static inline uint64_t now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void edge_init(void)
{
    const char* e = getenv("PS3RECOMP_NATIVE_EDGE");
    int m = e ? atoi(e) : 1;
    if (!__builtin_cpu_supports("avx512f") || !__builtin_cpu_supports("avx512bw") || !__builtin_cpu_supports("avx512vl") ||
        !__builtin_cpu_supports("avx512dq")) m = 0;
    const char* c = getenv("PS3RECOMP_EDGE_CAPTURE");
    if (c && *c) {
        static char dir[512];
        snprintf(dir, sizeof dir, "%s", c);
        char* comma = strchr(dir, ',');
        if (comma) { *comma = 0; s_cap_max = (unsigned)atoi(comma + 1); char* c2 = strchr(comma + 1, ','); if (c2) s_cap_every = (unsigned)atoi(c2 + 1); }
        if (!s_cap_every) s_cap_every = 1;
        s_cap_dir = dir;
    }
    rsq_init(); wtab_init(); fnms_init();
    fprintf(stderr, "[edge] native Edge Geometry kernels: %s%s%s\n",
            m == 2 ? "compare with the lifted code" : m ? "on" : "off", s_cap_dir ? ", capturing to " : "", s_cap_dir ? s_cap_dir : "");
    __atomic_store_n(&s_mode, m, __ATOMIC_RELEASE);
}
static void edge_stats(void)
{
    static uint64_t next;
    const uint64_t now = now_ns() / 1000000000ull;
    uint64_t n = __atomic_load_n(&next, __ATOMIC_RELAXED);
    if (now < n || !__atomic_compare_exchange_n(&next, &n, now + 10, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    if (!n) return;
    for (int i = 0; i < NKERN; i++) {
        const EdgeKernel* k = &g_kern[i];
        if (!k->calls) continue;
        fprintf(stderr, "[edge] %s (0x%X): %llu calls, %llu native, %llu lifted", k->name, k->addr, k->calls, k->native, k->fallback);
        if (s_mode == 2)
            fprintf(stderr, "; compared %llu, differ %llu; native %.2f us, lifted %.2f us per compared call",
                    k->cmp, k->bad, k->cmp ? k->ns_nat / 1e3 / k->cmp : 0.0, k->cmp ? k->ns_spu / 1e3 / k->cmp : 0.0);
        fprintf(stderr, "\n");
    }
}
/* the state a kernel starts from: the 128 registers, then the local store */
static void edge_dump(const char* dir, const char* tag, const EdgeKernel* k, unsigned long long id, const u128* gpr, const uint8_t* ls)
{
    char path[640];
    snprintf(path, sizeof path, "%s/%s_%04X_%06llu.bin", dir, tag, k->addr, id);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(gpr, sizeof(u128), 128, f);
    fwrite(ls, 1, SPU_LS_SIZE, f);
    fclose(f);
}
/* the registers a caller can see: the link, the stack pointer, r80..r127 */
static int edge_regs_differ(const u128* a, const u128* b)
{
    if (memcmp(&a[0], &b[0], 32)) return 1;
    return memcmp(&a[80], &b[80], 48 * sizeof(u128)) != 0;
}
static int edge_entry(spu_context* ctx, EdgeKernel* k)
{
    if (t_bypass) return 0;
    if (unlikely(__atomic_load_n(&s_mode, __ATOMIC_ACQUIRE) < 0)) edge_init();
    if (!s_mode) return 0;
    const unsigned long long id = __atomic_fetch_add(&k->calls, 1, __ATOMIC_RELAXED);
    edge_stats();
    if (s_cap_dir && id % s_cap_every == 0 && __atomic_fetch_add(&k->captured, 1, __ATOMIC_RELAXED) < s_cap_max)
        edge_dump(s_cap_dir, "in", k, id, ctx->gpr, ctx->ls);
    if (s_mode != 2) {
        if (k->core(ctx->gpr, ctx->ls)) { __atomic_fetch_add(&k->native, 1, __ATOMIC_RELAXED); return 1; }
        __atomic_fetch_add(&k->fallback, 1, __ATOMIC_RELAXED);
        return 0;
    }
    /* Mode 2: the kernel on a copy, then the lifted function on the real state. */
    static _Thread_local uint8_t* pre; static _Thread_local uint8_t* work;
    static _Thread_local u128 gpre[128], gwork[128];
    if (!pre) { pre = (uint8_t*)aligned_alloc(64, SPU_LS_SIZE); work = (uint8_t*)aligned_alloc(64, SPU_LS_SIZE); if (!pre || !work) return 0; }
    memcpy(pre, ctx->ls, SPU_LS_SIZE); memcpy(gpre, ctx->gpr, sizeof gpre);
    memcpy(work, pre, SPU_LS_SIZE); memcpy(gwork, gpre, sizeof gwork);
    const uint64_t t0 = now_ns();
    const int ok = k->core(gwork, work);
    const uint64_t t1 = now_ns();
    void (*fn)(spu_context*) = spu_lookup(k->addr, ctx->resident_ovl);
    if (!fn) return 0;
    if (!ok) { __atomic_fetch_add(&k->fallback, 1, __ATOMIC_RELAXED); return 0; }
    const int32_t si = (int32_t)ctx->image_id;
    const uint32_t r0 = ctx->gpr[0]._u32[0];
    t_bypass = 1;
    const uint64_t t2 = now_ns();
    ctx->host_depth++; fn(ctx); spu_drain_call(ctx, r0); ctx->host_depth--;
    const uint64_t t3 = now_ns();
    spu_img_restore(ctx, si);
    t_bypass = 0;
    __atomic_fetch_add(&k->native, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&k->cmp, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&k->ns_nat, t1 - t0, __ATOMIC_RELAXED);
    __atomic_fetch_add(&k->ns_spu, t3 - t2, __ATOMIC_RELAXED);
    const int regs = edge_regs_differ(ctx->gpr, gwork);
    if (regs || memcmp(ctx->ls, work, SPU_LS_SIZE)) {
        const unsigned long long nb = __atomic_add_fetch(&k->bad, 1, __ATOMIC_RELAXED);
        if (nb <= 8) {
            uint32_t first = SPU_LS_SIZE, last = 0, nq = 0;
            for (uint32_t q = 0; q < SPU_LS_SIZE; q += 16)
                if (memcmp(ctx->ls + q, work + q, 16)) { if (first == SPU_LS_SIZE) first = q; last = q; nq++; }
            fprintf(stderr, "[edge] %s call %llu differs: %u LS quads (0x%05X..0x%05X)%s\n", k->name, id, nq,
                    first == SPU_LS_SIZE ? 0 : first, last, regs ? ", registers" : "");
            const char* dir = getenv("PS3RECOMP_EDGE_DUMP");
            if (dir) edge_dump(dir, "diff", k, id, gpre, pre);
        }
    }
    ctx->pc = ctx->gpr[0]._u32[0];
    return 1;
}
#define EDGE_HOOK(i) static int edge_hook_##i(spu_context* ctx) { return edge_entry(ctx, &g_kern[i]); }
EDGE_HOOK(0)
EDGE_HOOK(1)
EDGE_HOOK(2)
int spu_native_register(int image_id, uint32_t addr, int (*fn)(spu_context*));
__attribute__((constructor)) static void edge_register(void)
{
    static int (*const hooks[])(spu_context*) = { edge_hook_0, edge_hook_1, edge_hook_2 };
    for (int i = 0; i < NKERN; i++) spu_native_register(EDGE_IMAGE, g_kern[i].addr, hooks[i]);
}
#endif
#ifdef EDGE_HARNESS
/* micro-benchmark hook: time cull_batch16 alone on the state of a cull call */
double edge_bench_cull_batch(u128* gpr, uint8_t* ls, int reps)
{
    const uint32_t S = gpr[3]._u32[0];
    Cull k; memset(&k, 0, sizeof k);
    Xf x; u128 g2[128]; memcpy(g2, gpr, sizeof g2);
    for (int i = 0; i < 4; i++) g2[1]._u32[i] -= 128;
    if (!xf_prepare(g2, ls, &x)) return -1;
    xf_exec(g2, ls, &x);
    k.B = word_at(ls, S + 0x50);
    const uint32_t nidx = lq(ls, S + 0x1A0)._u32[0] >> 16;
    k.nt = (uint32_t)(((uint64_t)nidx * 0xAAAAAAABull) >> 33); k.ng = (k.nt + 3) >> 2;
    k.rec = x.scr + 16 * gpr[5]._u32[0]; k.snap = F1; k.m65 = 1;
    static uint8_t idx[24 * 5462 + 128];
    memcpy(idx, ls + k.B, 24 * k.ng + 64); k.idx = idx;
    struct timespec t0, t1; uint16_t m4[4]; unsigned acc = 0;
    const uint32_t nb = k.ng / 4;
    if (!nb) return -1;
    const uint32_t csr = _mm_getcsr(); _mm_setcsr(CSR_FAST);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int r = 0; r < reps; r++) for (uint32_t b = 0; b < nb; b++) { cull_batch16(ls, &k, b, m4); acc += m4[0] + m4[1] + m4[2] + m4[3]; }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    _mm_setcsr(csr);
    static volatile unsigned sink; sink = acc;
    return ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / ((double)reps * nb);
}
#endif
