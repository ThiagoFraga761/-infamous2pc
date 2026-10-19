/* inFAMOUS 2 (BCUS98125): the hot leaf functions of the skeletal-animation
 * job (module 2025343b09d0, image 201), natively.
 *
 * Character rigs (268 bones, 8 to 11 layers) are most of this job's cost and
 * every instance has its own time, so the whole-job cache of job2025_native.c
 * never hits them. These kernels replace pure local-store functions instead:
 * whatever the rig, layout or buffer sizes, each leaves the local store and
 * the registers a caller can see exactly as the SPU function does.
 * The functions: the table g_k below.
 *
 * Two kinds of kernel body, both generated from the module (game code, so
 * the headers are generated at build time and stay out of the repository;
 * work/port/tools/gen_anim_kernels.sh):
 *
 *  - job2025_kernels_gen.h (tools/kgen.py): every function, instruction by
 *    instruction on host registers. Single-precision arithmetic is one
 *    AVX-512 op with embedded truncation; a lane whose result is at or above
 *    FLT_MAX (an operand with exponent 255, or an overflow: x86 and the SPU
 *    differ there) is recomputed by the helpers' own exact fix-up. shufb with
 *    a control known at generation time is a fixed byte permute; estimates
 *    and conversions are four-lane forms of the helpers (j2025k_selftest).
 *    Zeros a non-arithmetic use could see are made +0 (the SPU has no -0).
 *  - job2025_kernels_wide.h (tools/kwide.py): for the loops over bones whose
 *    every group of four is the same computation on its own records (the
 *    pose blends, pose to 3x4), four groups per AVX-512 register, one per
 *    128-bit lane, each node the same SPU instruction as in the SPU code.
 *    Before it starts, a wide kernel checks the layout it was derived for
 *    (in-place arrays, alignment, written ranges meeting nothing else);
 *    otherwise the instruction-by-instruction kernel runs.
 * Both rely on the module bytes they folded in (checked in the live LS) and
 * on denormals-are-zero in MXCSR, as on every SPU thread (spu_fp_enter).
 * Volatile registers (r2, r4-r79) are left as they are by the wide kernels:
 * no caller of these functions reads them (tools/kabi.py).
 *
 * The lift calls spu_native_entry at each address (tools/lift_spu.py,
 * NATIVE_ENTRIES). PS3RECOMP_NATIVE_ANIM: 1 (default) on; 0 off; 2 runs the
 * kernel on a copy of the local store and registers, then the lifted
 * function, and compares the local store and r0, r1, r80-r127 (and r3 for
 * the instruction-by-instruction kernels), logging any difference and the
 * time of each (PS3RECOMP_ANIM_DUMP=dir also writes the state the kernel
 * started from). PS3RECOMP_ANIM_CAPTURE=dir[,count[,every]] writes entry
 * states (128 registers, then the local store) for the offline tests
 * (work/runs/job201/oracle/ktest.c).
 *
 * Built like the lifted SPU sources (-march=x86-64-v4). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <immintrin.h>

#include "spu_context.h"
#include "spu_helpers.h"

#if !defined(__AVX512F__) || !defined(__AVX512VL__)
#error "job2025_kernels_native.c needs -march=x86-64-v4 (AVX-512 F/VL)"
#endif

#define ANIM_IMAGE 201
#define KNOINL __attribute__((noinline))
#define KLSQ 0x3FFF0u


/* Copy-on-first-write journal of local-store quads, to undo a discarded run;
 * for the group-parallel kernels with data-dependent addresses, the group
 * (+1) that stored to each quad during the call (kwide2.py). */
typedef struct {
    uint64_t bm[SPU_LS_SIZE / 16 / 64];
    uint32_t n;
    uint16_t list[SPU_LS_SIZE / 16];
    uint32_t tag[SPU_LS_SIZE / 16];
    uint8_t save[SPU_LS_SIZE];
} KJournal;

static inline void kj_note(KJournal* j, const uint8_t* ls, uint32_t q)
{
    const uint32_t i = q >> 4;
    const uint64_t b = 1ull << (i & 63);
    if (j->bm[i >> 6] & b) return;
    j->bm[i >> 6] |= b;
    j->list[j->n++] = (uint16_t)i;
    memcpy(j->save + q, ls + q, 16);
}
static void kj_undo(KJournal* j, uint8_t* ls)
{
    for (uint32_t k = j->n; k-- > 0;) {
        const uint32_t i = j->list[k];
        memcpy(ls + 16 * i, j->save + 16 * i, 16);
        j->bm[i >> 6] = 0; j->tag[i] = 0;
    }
    j->n = 0;
}
static inline void kj_clear(KJournal* j)
{
    for (uint32_t k = 0; k < j->n; k++) { const uint32_t i = j->list[k]; j->bm[i >> 6] = 0; j->tag[i] = 0; }
    j->n = 0;
}

/* ------------------------------------------------------------------ what the generated code uses */
#define BSW _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)
#define GL(n) _mm_loadu_si128((const __m128i*)&g[n])
#define GS(n, v) _mm_storeu_si128((__m128i*)&g[n], (v))
#define W0(v) ((uint32_t)_mm_cvtsi128_si32(v))
#define KLD(a) _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + ((a) & KLSQ))), BSW)
#define KST(a, v) do { const uint32_t q_ = (a) & KLSQ; kj_note(jr, ls, q_); \
                       _mm_storeu_si128((__m128i*)(ls + q_), _mm_shuffle_epi8((v), BSW)); } while (0)
#define KSTN(a, v) _mm_storeu_si128((__m128i*)(ls + ((a) & KLSQ)), _mm_shuffle_epi8((v), BSW))
#define F(v) _mm_castsi128_ps(v)
/* Single-precision ops truncate (embedded rounding: the MXCSR modes do not
 * matter, but denormals-are-zero must be on, as on every SPU thread).
 * Four lanes go through the 512-bit forms, which are the ones with embedded
 * rounding; the upper lanes are zero. */
#define KRZ (_MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC)
#define KZX(v) _mm512_zextps128_ps512(v)
#define KLO(v) _mm512_castps512_ps128(v)
#define KADD(a, b) KLO(_mm512_add_round_ps(KZX(a), KZX(b), KRZ))
#define KSUB(a, b) KLO(_mm512_sub_round_ps(KZX(a), KZX(b), KRZ))
#define KMUL(a, b) KLO(_mm512_mul_round_ps(KZX(a), KZX(b), KRZ))
#define KFMA(a, b, c) KLO(_mm512_fmadd_round_ps(KZX(a), KZX(b), KZX(c), KRZ))
#define KFMS(a, b, c) KLO(_mm512_fmsub_round_ps(KZX(a), KZX(b), KZX(c), KRZ))
#define KFNMA(a, b, c) KLO(_mm512_fnmadd_round_ps(KZX(a), KZX(b), KZX(c), KRZ))
#define I(v) _mm_castps_si128(v)
/* x86's result is not the SPU's: an operand with exponent 255, or an overflow */
#define KBAD(t) __builtin_expect(_mm_cmpge_epu32_mask(_mm_and_si128((t), _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7F7FFFFF)) != 0, 0)
#define CANON(v) _mm_maskz_mov_epi32(_mm_test_epi32_mask((v), _mm_set1_epi32(0x7F800000)), (v))
#define VC(a, b, c, d) _mm_setr_epi32((int)(a), (int)(b), (int)(c), (int)(d))
#define VB(...) _mm_setr_epi8(__VA_ARGS__)
static inline __m128i H(u128 x) { __m128i v; memcpy(&v, &x, 16); return v; }
static inline u128 U(__m128i v) { u128 x; memcpy(&x, &v, 16); return x; }

/* The exact single-precision op, for the lanes x86 got wrong: the helpers'
 * own fix-up (spu_helpers.h), which forms the result exactly in double
 * precision or integers and so does not depend on the MXCSR modes. It is
 * handed the lanes x86 got right, with their zeros made +0 as the SPU's are. */
static inline __m128i kx_ok(__m128i t)
{
    return _mm_maskz_mov_epi32(_mm_cmplt_epu32_mask(_mm_and_si128(t, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7F7FFFFF)), _mm_set1_epi32(-1));
}
static KNOINL __m128i kx_fa(__m128i a, __m128i b) { const __m128i t = CANON(I(KADD(F(a), F(b)))); return spu__av_fa_fix(t, kx_ok(t), a, b, 0); }
static KNOINL __m128i kx_fs(__m128i a, __m128i b) { const __m128i t = CANON(I(KSUB(F(a), F(b)))); return spu__av_fa_fix(t, kx_ok(t), a, b, 0x80000000u); }
static KNOINL __m128i kx_fm(__m128i a, __m128i b) { const __m128i t = CANON(I(KMUL(F(a), F(b)))); return spu__av_fm_fix(t, kx_ok(t), a, b); }
static KNOINL __m128i kx_fma(__m128i a, __m128i b, __m128i c) { const __m128i t = CANON(I(KFMA(F(a), F(b), F(c)))); return spu__av_fma_fix(t, kx_ok(t), a, b, c, 0, 0); }
static KNOINL __m128i kx_fms(__m128i a, __m128i b, __m128i c) { const __m128i t = CANON(I(KFMS(F(a), F(b), F(c)))); return spu__av_fma_fix(t, kx_ok(t), a, b, c, 0, 0x80000000u); }
static KNOINL __m128i kx_fnms(__m128i a, __m128i b, __m128i c) { const __m128i t = CANON(I(KFNMA(F(a), F(b), F(c)))); return spu__av_fma_fix(t, kx_ok(t), a, b, c, 0x80000000u, 0); }

/* Estimates and conversions, four lanes at once, exactly the helpers' results
 * (the helpers do them a lane at a time through memory, and every reload of
 * the four stored lanes stalls on store forwarding). j2025k_selftest checks
 * them against the helpers. */
static const uint32_t k_frest_lut[32] __attribute__((aligned(64))) = {
    0x7FFBE0, 0x7F87A6, 0x70EF72, 0x708B40, 0x638B12, 0x633AEA, 0x5792C4, 0x574AA0,
    0x4CCA7E, 0x4C9262, 0x430A44, 0x42D62A, 0x3A2E12, 0x39FDFA, 0x3215E4, 0x31F1D2,
    0x2AA9BE, 0x2A85AC, 0x23D59A, 0x23BD8E, 0x1D8576, 0x1D8576, 0x17AD5A, 0x17AD5A,
    0x124543, 0x124543, 0x0D392D, 0x0D392D, 0x08851A, 0x08851A, 0x041D07, 0x041D07 };
static const uint32_t k_frsq_lut[64] __attribute__((aligned(64))) = {
    0x350160, 0x34E954, 0x2F993D, 0x2F993D, 0x2AA523, 0x2AA523, 0x26190D, 0x26190D,
    0x21E4F9, 0x21E4F9, 0x1E00E9, 0x1E00E9, 0x1A5CD9, 0x1A5CD9, 0x16F8CB, 0x16F8CB,
    0x13CCC0, 0x13CCC0, 0x10CCB3, 0x10CCB3, 0x0E00AA, 0x0E00AA, 0x0B58A1, 0x0B58A1,
    0x08D498, 0x08D498, 0x067491, 0x067491, 0x043089, 0x043089, 0x020C83, 0x020C83,
    0x7FFDF4, 0x7FD1DE, 0x7859C8, 0x783DBA, 0x71559C, 0x71559C, 0x6AE57C, 0x6AE57C,
    0x64F561, 0x64F561, 0x5F7149, 0x5F7149, 0x5A4D33, 0x5A4D33, 0x55811F, 0x55811F,
    0x51050F, 0x51050F, 0x4CC8FE, 0x4CC8FE, 0x48D0F0, 0x48D0F0, 0x4510E4, 0x4510E4,
    0x4180D7, 0x4180D7, 0x3E24CC, 0x3E24CC, 0x3AF4C3, 0x3AF4C3, 0x37E8BA, 0x37E8BA };
static inline __m128i k_lut32(const uint32_t* t, __m128i idx)
{
    return _mm512_castsi512_si128(_mm512_permutex2var_epi32(_mm512_load_si512(t), _mm512_castsi128_si512(idx), _mm512_load_si512(t + 16)));
}
static inline __m128i k_frest(__m128i x)
{
    const __m128i e = _mm_and_si128(_mm_srli_epi32(x, 23), _mm_set1_epi32(0xFF));
    __m128i re = _mm_sub_epi32(_mm_set1_epi32(253), e);
    re = _mm_maskz_mov_epi32(_mm_cmplt_epu32_mask(e, _mm_set1_epi32(253)), re);              /* exp >= 253: 0 */
    re = _mm_mask_mov_epi32(re, _mm_cmpeq_epi32_mask(e, _mm_setzero_si128()), _mm_set1_epi32(255));
    const __m128i f = _mm_and_si128(k_lut32(k_frest_lut, _mm_and_si128(_mm_srli_epi32(x, 18), _mm_set1_epi32(0x1F))), _mm_set1_epi32(0x7FFFFF));
    return _mm_ternarylogic_epi32(_mm_and_si128(x, _mm_set1_epi32((int)0x80000000u)), _mm_slli_epi32(re, 23), f, 0xFE);
}
static inline __m128i k_frsqest(__m128i x)
{
    const __m128i e = _mm_and_si128(_mm_srli_epi32(x, 23), _mm_set1_epi32(0xFF));
    __m128i re = _mm_sub_epi32(_mm_set1_epi32(190), _mm_srli_epi32(_mm_add_epi32(e, _mm_set1_epi32(1)), 1));
    re = _mm_mask_mov_epi32(re, _mm_cmpeq_epi32_mask(e, _mm_setzero_si128()), _mm_set1_epi32(255));
    const __m128i i = _mm_and_si128(_mm_srli_epi32(x, 18), _mm_set1_epi32(0x3F));
    const __m128i lo = k_lut32(k_frsq_lut, i), hi = k_lut32(k_frsq_lut + 32, i);
    const __m128i f = _mm_mask_mov_epi32(lo, _mm_test_epi32_mask(i, _mm_set1_epi32(32)), hi);
    return _mm_or_si128(_mm_slli_epi32(re, 23), _mm_and_si128(f, _mm_set1_epi32(0x7FFFFF)));
}
/* int -> float, times 2^-(155-i8): RZ conversion is the SPU's truncation, and
 * the power of two is exact for the scales the generator sends here */
static inline __m128i k_cuflt(__m128i x, int i8)
{
    return I(KMUL(KLO(_mm512_cvt_roundepu32_ps(_mm512_zextsi128_si512(x), KRZ)), _mm_castsi128_ps(_mm_set1_epi32((127 - (155 - i8)) << 23))));
}
static inline __m128i k_csflt(__m128i x, int i8)
{
    return I(KMUL(KLO(_mm512_cvt_roundepi32_ps(_mm512_zextsi128_si512(x), KRZ)), _mm_castsi128_ps(_mm_set1_epi32((127 - (155 - i8)) << 23))));
}
/* float -> int, times 2^(173-i8), truncated, saturated: x86 gives 0x80000000
 * for anything out of range (overflow, exponent 255): positive lanes saturate up */
static inline __m128i k_cflts(__m128i x, int i8)
{
    const __m128i r = _mm_cvttps_epi32(KMUL(F(x), _mm_castsi128_ps(_mm_set1_epi32((127 + (173 - i8)) << 23))));
    const __mmask8 up = _mm_cmpeq_epi32_mask(r, _mm_set1_epi32((int)0x80000000u)) & _mm_cmpgt_epi32_mask(x, _mm_set1_epi32(-1));
    return _mm_mask_mov_epi32(r, up, _mm_set1_epi32(0x7FFFFFFF));
}


/* ------------------------------------------------------------------ four groups per register
 * The group-parallel kernels (tools/kwide.py) run one group of four bones in
 * each 128-bit lane of a zmm register: every SPU instruction of the group's
 * graph becomes the same operation on all four lanes. Lane order inside a
 * 128-bit lane is the u128 layout (host words, SPU word i = dword i). */
typedef __m512i Z;
#define ZF(v) _mm512_castsi512_ps(v)
#define ZI(v) _mm512_castps_si512(v)
#define ZB(q) _mm512_broadcast_i32x4(q)
#define ZC(a, b, c, d) _mm512_broadcast_i32x4(VC(a, b, c, d))
#define ZBYTES(...) _mm512_broadcast_i32x4(_mm_setr_epi8(__VA_ARGS__))
#define ZCANON(v) _mm512_maskz_mov_epi32(_mm512_test_epi32_mask((v), _mm512_set1_epi32(0x7F800000)), (v))
#define ZBADM(t) _mm512_cmpge_epu32_mask(_mm512_and_si512((t), _mm512_set1_epi32(0x7FFFFFFF)), _mm512_set1_epi32(0x7F7FFFFF))
#define ZM(m) _mm512_movm_epi32(m)
static inline __m128i zlane(Z v, int i)
{
    switch (i) { case 0: return _mm512_extracti32x4_epi32(v, 0); case 1: return _mm512_extracti32x4_epi32(v, 1);
                 case 2: return _mm512_extracti32x4_epi32(v, 2); default: return _mm512_extracti32x4_epi32(v, 3); }
}
static inline Z zset(Z v, int i, __m128i q)
{
    switch (i) { case 0: return _mm512_inserti32x4(v, q, 0); case 1: return _mm512_inserti32x4(v, q, 1);
                 case 2: return _mm512_inserti32x4(v, q, 2); default: return _mm512_inserti32x4(v, q, 3); }
}
/* the exact op for lanes x86 got wrong (any word of the 128-bit lane) */
static KNOINL Z zx_2(int op, Z a, Z b, Z t, __mmask16 m)
{
    for (int i = 0; i < 4; i++) if ((m >> (4 * i)) & 0xF) {
        const __m128i x = zlane(a, i), y = zlane(b, i);
        __m128i r;
        switch (op) { case 0: r = kx_fa(x, y); break; case 1: r = kx_fs(x, y); break; default: r = kx_fm(x, y); break; }
        t = zset(t, i, r);
    }
    return t;
}
static KNOINL Z zx_3(int op, Z a, Z b, Z c, Z t, __mmask16 m)
{
    for (int i = 0; i < 4; i++) if ((m >> (4 * i)) & 0xF) {
        const __m128i x = zlane(a, i), y = zlane(b, i), w = zlane(c, i);
        __m128i r;
        switch (op) { case 0: r = kx_fma(x, y, w); break; case 1: r = kx_fms(x, y, w); break; default: r = kx_fnms(x, y, w); break; }
        t = zset(t, i, r);
    }
    return t;
}
static inline Z zfa(Z a, Z b) { Z t = ZI(_mm512_add_round_ps(ZF(a), ZF(b), KRZ)); const __mmask16 m = ZBADM(t); return __builtin_expect(m != 0, 0) ? zx_2(0, a, b, t, m) : t; }
static inline Z zfs(Z a, Z b) { Z t = ZI(_mm512_sub_round_ps(ZF(a), ZF(b), KRZ)); const __mmask16 m = ZBADM(t); return __builtin_expect(m != 0, 0) ? zx_2(1, a, b, t, m) : t; }
static inline Z zfm(Z a, Z b) { Z t = ZI(_mm512_mul_round_ps(ZF(a), ZF(b), KRZ)); const __mmask16 m = ZBADM(t); return __builtin_expect(m != 0, 0) ? zx_2(2, a, b, t, m) : t; }
static inline Z zfma(Z a, Z b, Z c) { Z t = ZI(_mm512_fmadd_round_ps(ZF(a), ZF(b), ZF(c), KRZ)); const __mmask16 m = ZBADM(t); return __builtin_expect(m != 0, 0) ? zx_3(0, a, b, c, t, m) : t; }
static inline Z zfms(Z a, Z b, Z c) { Z t = ZI(_mm512_fmsub_round_ps(ZF(a), ZF(b), ZF(c), KRZ)); const __mmask16 m = ZBADM(t); return __builtin_expect(m != 0, 0) ? zx_3(1, a, b, c, t, m) : t; }
static inline Z zfnms(Z a, Z b, Z c) { Z t = ZI(_mm512_fnmadd_round_ps(ZF(a), ZF(b), ZF(c), KRZ)); const __mmask16 m = ZBADM(t); return __builtin_expect(m != 0, 0) ? zx_3(2, a, b, c, t, m) : t; }
/* compares as the helpers: zeros alike, magnitudes in bit order */
static inline Z zfkey(Z x)
{
    const __mmask16 nz = _mm512_test_epi32_mask(x, _mm512_set1_epi32(0x7F800000));
    const Z m = _mm512_maskz_and_epi32(nz, x, _mm512_set1_epi32(0x7FFFFFFF));
    return _mm512_mask_sub_epi32(m, _mm512_movepi32_mask(x), _mm512_setzero_si512(), m);
}
static inline Z zfcgt(Z a, Z b) { return ZM(_mm512_cmpgt_epi32_mask(zfkey(a), zfkey(b))); }
static inline Z zfceq(Z a, Z b) { return ZM(_mm512_cmpeq_epi32_mask(zfkey(a), zfkey(b))); }
static inline Z zfmag(Z x) { return _mm512_maskz_and_epi32(_mm512_test_epi32_mask(x, _mm512_set1_epi32(0x7F800000)), x, _mm512_set1_epi32(0x7FFFFFFF)); }
static inline Z zfcmgt(Z a, Z b) { return ZM(_mm512_cmpgt_epi32_mask(zfmag(a), zfmag(b))); }
static inline Z zfrest(Z x)
{
    const Z e = _mm512_and_si512(_mm512_srli_epi32(x, 23), _mm512_set1_epi32(0xFF));
    Z re = _mm512_maskz_sub_epi32(_mm512_cmplt_epu32_mask(e, _mm512_set1_epi32(253)), _mm512_set1_epi32(253), e);
    re = _mm512_mask_mov_epi32(re, _mm512_cmpeq_epi32_mask(e, _mm512_setzero_si512()), _mm512_set1_epi32(255));
    const Z f = _mm512_and_si512(_mm512_permutex2var_epi32(_mm512_load_si512(k_frest_lut), _mm512_and_si512(_mm512_srli_epi32(x, 18), _mm512_set1_epi32(0x1F)),
                                                           _mm512_load_si512(k_frest_lut + 16)), _mm512_set1_epi32(0x7FFFFF));
    return _mm512_ternarylogic_epi32(_mm512_and_si512(x, _mm512_set1_epi32((int)0x80000000u)), _mm512_slli_epi32(re, 23), f, 0xFE);
}
static inline Z zfrsqest(Z x)
{
    const Z e = _mm512_and_si512(_mm512_srli_epi32(x, 23), _mm512_set1_epi32(0xFF));
    Z re = _mm512_sub_epi32(_mm512_set1_epi32(190), _mm512_srli_epi32(_mm512_add_epi32(e, _mm512_set1_epi32(1)), 1));
    re = _mm512_mask_mov_epi32(re, _mm512_cmpeq_epi32_mask(e, _mm512_setzero_si512()), _mm512_set1_epi32(255));
    const Z i = _mm512_and_si512(_mm512_srli_epi32(x, 18), _mm512_set1_epi32(0x3F));
    const Z lo = _mm512_permutex2var_epi32(_mm512_load_si512(k_frsq_lut), i, _mm512_load_si512(k_frsq_lut + 16));
    const Z hi = _mm512_permutex2var_epi32(_mm512_load_si512(k_frsq_lut + 32), i, _mm512_load_si512(k_frsq_lut + 48));
    const Z f = _mm512_mask_mov_epi32(lo, _mm512_test_epi32_mask(i, _mm512_set1_epi32(32)), hi);
    return _mm512_or_si512(_mm512_slli_epi32(re, 23), _mm512_and_si512(f, _mm512_set1_epi32(0x7FFFFF)));
}
/* fi: spu_helpers.h's four-lane form, sixteen lanes */
static inline Z zfi(Z ya, Z est)
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
    return _mm512_maskz_ternarylogic_epi32((__mmask16)~zero, _mm512_and_si512(est, _mm512_set1_epi32((int)0x80000000u)),
                                           _mm512_slli_epi32(e2, 23), frac, 0xFE);
}
static inline Z zcuflt(Z x, int i8) { return ZI(_mm512_mul_round_ps(_mm512_cvt_roundepu32_ps(x, KRZ), _mm512_castsi512_ps(_mm512_set1_epi32((127 - (155 - i8)) << 23)), KRZ)); }
static inline Z zcsflt(Z x, int i8) { return ZI(_mm512_mul_round_ps(_mm512_cvt_roundepi32_ps(x, KRZ), _mm512_castsi512_ps(_mm512_set1_epi32((127 - (155 - i8)) << 23)), KRZ)); }
static inline Z zcflts(Z x, int i8)
{
    const Z r = _mm512_cvttps_epi32(_mm512_mul_round_ps(ZF(x), _mm512_castsi512_ps(_mm512_set1_epi32((127 + (173 - i8)) << 23)), KRZ));
    const __mmask16 up = _mm512_cmpeq_epi32_mask(r, _mm512_set1_epi32((int)0x80000000u)) & _mm512_cmpgt_epi32_mask(x, _mm512_set1_epi32(-1));
    return _mm512_mask_mov_epi32(r, up, _mm512_set1_epi32(0x7FFFFFFF));
}
/* rotqby by the low four bits of word 0 of each lane's count */
static inline Z zrotqby(Z x, Z cnt)
{
    const Z n = _mm512_shuffle_epi8(cnt, _mm512_setzero_si512());               /* host byte 0 of dword 0 = cnt.w0 & 0xFF */
    const Z h3 = ZBYTES(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);  /* host byte h -> SPU byte h ^ 3 */
    const Z idx = _mm512_xor_si512(_mm512_and_si512(_mm512_add_epi8(h3, n), _mm512_set1_epi8(15)), _mm512_set1_epi8(3));
    return _mm512_shuffle_epi8(x, idx);
}
/* shufb with a control known only at run time, each 128-bit lane as the helper */
static inline Z zshufb(Z a, Z b, Z c)
{
    const Z sw = ZBYTES(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    const Z A = _mm512_shuffle_epi8(a, sw), B = _mm512_shuffle_epi8(b, sw), C = _mm512_shuffle_epi8(c, sw);
    const Z idx = _mm512_and_si512(C, _mm512_set1_epi8(0x0F));
    Z r = _mm512_mask_blend_epi8(_mm512_test_epi8_mask(C, _mm512_set1_epi8(0x10)), _mm512_shuffle_epi8(A, idx), _mm512_shuffle_epi8(B, idx));
    const __mmask64 m80 = _mm512_test_epi8_mask(C, _mm512_set1_epi8((char)0x80));
    const __mmask64 mC0 = _mm512_cmpeq_epi8_mask(_mm512_and_si512(C, _mm512_set1_epi8((char)0xC0)), _mm512_set1_epi8((char)0xC0));
    const __mmask64 mE0 = _mm512_cmpeq_epi8_mask(_mm512_and_si512(C, _mm512_set1_epi8((char)0xE0)), _mm512_set1_epi8((char)0xE0));
    r = _mm512_mask_mov_epi8(r, m80, _mm512_setzero_si512());
    r = _mm512_mask_mov_epi8(r, mC0, _mm512_set1_epi8((char)0xFF));
    r = _mm512_mask_mov_epi8(r, mE0, _mm512_set1_epi8((char)0x80));
    return _mm512_shuffle_epi8(r, sw);
}
/* any other instruction: the helper on each lane */
#define ZH1(fn, a, ...) ({ Z r_ = (a); for (int i_ = 0; i_ < 4; i_++) r_ = zset(r_, i_, H(fn(U(zlane((a), i_)), ##__VA_ARGS__))); r_; })
#define ZH2(fn, a, b, ...) ({ Z r_ = (a); for (int i_ = 0; i_ < 4; i_++) r_ = zset(r_, i_, H(fn(U(zlane((a), i_)), U(zlane((b), i_)), ##__VA_ARGS__))); r_; })
#define ZH3(fn, a, b, c) ({ Z r_ = (a); for (int i_ = 0; i_ < 4; i_++) r_ = zset(r_, i_, H(fn(U(zlane((a), i_)), U(zlane((b), i_)), U(zlane((c), i_))))); r_; })
/* local store: quad i of the register at a + i * stride (each address & ~15) */
static inline Z zld4(const uint8_t* ls, uint32_t a, uint32_t stride)
{
    const Z sw = ZBYTES(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    Z v = _mm512_castsi128_si512(_mm_loadu_si128((const __m128i*)(ls + (a & KLSQ))));
    v = _mm512_inserti32x4(v, _mm_loadu_si128((const __m128i*)(ls + ((a + stride) & KLSQ))), 1);
    v = _mm512_inserti32x4(v, _mm_loadu_si128((const __m128i*)(ls + ((a + 2 * stride) & KLSQ))), 2);
    v = _mm512_inserti32x4(v, _mm_loadu_si128((const __m128i*)(ls + ((a + 3 * stride) & KLSQ))), 3);
    return _mm512_shuffle_epi8(v, sw);
}
static inline void zst4(uint8_t* ls, uint32_t a, uint32_t stride, Z v, int n)
{
    v = _mm512_shuffle_epi8(v, ZBYTES(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
    _mm_storeu_si128((__m128i*)(ls + (a & KLSQ)), _mm512_castsi512_si128(v));
    if (n > 1) _mm_storeu_si128((__m128i*)(ls + ((a + stride) & KLSQ)), _mm512_extracti32x4_epi32(v, 1));
    if (n > 2) _mm_storeu_si128((__m128i*)(ls + ((a + 2 * stride) & KLSQ)), _mm512_extracti32x4_epi32(v, 2));
    if (n > 3) _mm_storeu_si128((__m128i*)(ls + ((a + 3 * stride) & KLSQ)), _mm512_extracti32x4_epi32(v, 3));
}
/* word 0 of quad i, big-endian, at (a + i * stride) & ~3: shufb(v, lqd(p), cwd(p)) stored to p */
static inline void zstw4(uint8_t* ls, uint32_t a, uint32_t stride, Z v, int n)
{
    uint32_t w[16]; _mm512_storeu_si512(w, v);
    for (int i = 0; i < n; i++) { const uint32_t x = __builtin_bswap32(w[4 * i]); memcpy(ls + ((a + i * stride) & 0x3FFFCu), &x, 4); }
}
/* the group number of each lane, times a stride, in every word, plus d */
static inline Z zlaneoff(uint32_t g0, uint32_t stride, int32_t d)
{
    return _mm512_add_epi32(_mm512_set1_epi32((int32_t)(g0 * stride) + d),
                            _mm512_mullo_epi32(_mm512_set_epi32(3, 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0), _mm512_set1_epi32((int)stride)));
}
static inline int k_disjoint(uint32_t a, uint32_t na, uint32_t b, uint32_t nb)
{
    return a + na <= b || b + nb <= a;
}
/* ---- tagged accesses (kwide2.py tag mode)
 * Each quad a group stores to is tagged (group + 1) << 8 | the rank of the
 * earliest of that group's stores to it (SPU order). A store to a quad of
 * another group, or a load from it -- or from a quad its own group stored
 * to before that load in SPU order: the kernel loads first -- is a conflict. */
#define W0L(z) ((uint32_t)_mm_cvtsi128_si32(_mm512_castsi512_si128(z)))
#ifdef J2025K_HARNESS
static uint32_t g_kt_why[4];                       /* first conflict: kind, address, group, tag */
#define KT_BAD(kind, a, gid, t) do { if (!*bad && getenv("KDEBUG")) { g_kt_why[0] = (kind); g_kt_why[1] = (a); g_kt_why[2] = (gid); g_kt_why[3] = (t); \
        fprintf(stderr, "[anim] conflict %s at %05X: group %u, tag %08X\n", (kind) ? "load" : "store", (a) & 0x3FFFF, (gid), (t)); } *bad = 1; } while (0)
#else
#define KT_BAD(kind, a, gid, t) (*bad = 1)
#endif
#define KT_OUTSIDE 0xFFFFFF00u
static inline void kt_own(KJournal* j, const uint8_t* ls, uint32_t q, uint32_t tv, int* bad)
{
    const uint32_t t = j->tag[q >> 4];
    if (!t) { kj_note(j, ls, q); j->tag[q >> 4] = tv; return; }
    if ((t >> 8) != (tv >> 8)) { KT_BAD(0, q, tv >> 8, t); return; }
    if ((tv & 0xFF) < (t & 0xFF)) j->tag[q >> 4] = tv;
}
static inline void kt_stq(uint8_t* ls, KJournal* j, uint32_t a, __m128i v, uint32_t tv, int* bad)
{
    const uint32_t q = a & KLSQ;
    kt_own(j, ls, q, tv, bad);
    _mm_storeu_si128((__m128i*)(ls + q), _mm_shuffle_epi8(v, BSW));
}
#define KT_TV(g0, i, rank) ((uint32_t)((g0) + (i) + 1) << 8 | (uint32_t)(rank))
static inline void kt_st4(uint8_t* ls, KJournal* j, uint32_t a, uint32_t stride, Z v, uint32_t g0, int n, int rank, int* bad)
{
    for (int i = 0; i < n; i++) kt_stq(ls, j, a + i * stride, zlane(v, i), KT_TV(g0, i, rank), bad);
}
static inline void kt_st4x(uint8_t* ls, KJournal* j, Z base, int32_t off, Z v, uint32_t g0, int n, int rank, int* bad)
{
    uint32_t w[16]; _mm512_storeu_si512(w, base);
    for (int i = 0; i < n; i++) kt_stq(ls, j, w[4 * i] + off, zlane(v, i), KT_TV(g0, i, rank), bad);
}
static inline void kt_stw(uint8_t* ls, KJournal* j, uint32_t a, uint32_t x, uint32_t tv, int* bad)
{
    kt_own(j, ls, a & KLSQ, tv, bad);
    x = __builtin_bswap32(x); memcpy(ls + (a & 0x3FFFCu), &x, 4);
}
static inline void kt_stw4(uint8_t* ls, KJournal* j, uint32_t a, uint32_t stride, Z v, uint32_t g0, int n, int rank, int* bad)
{
    uint32_t w[16]; _mm512_storeu_si512(w, v);
    for (int i = 0; i < n; i++) kt_stw(ls, j, a + i * stride, w[4 * i], KT_TV(g0, i, rank), bad);
}
static inline void kt_stw4x(uint8_t* ls, KJournal* j, Z base, int32_t off, Z v, uint32_t g0, int n, int rank, int* bad)
{
    uint32_t b[16], w[16]; _mm512_storeu_si512(b, base); _mm512_storeu_si512(w, v);
    for (int i = 0; i < n; i++) kt_stw(ls, j, b[4 * i] + off, w[4 * i], KT_TV(g0, i, rank), bad);
}
/* After the batch's stores: a load of group gid, which came before rl of its
 * own group's stores (SPU order), from a quad tagged t. The kernel did every
 * load of the batch first, so the load saw stores of earlier batches and none
 * of this one. On the SPU it sees the stores of groups dist >= D before it,
 * maybe some of the groups up to F after it, and none further (kwide2.py
 * works D and F out). Anything else differs. */
static inline void kt_chkq(const KJournal* j, uint32_t a, uint32_t gid, uint32_t gfirst, int rl, uint32_t D, uint32_t F, int* bad)
{
    const uint32_t t = j->tag[(a & KLSQ) >> 4];
    if (!t) return;
    const uint32_t tg = t >> 8;
    if (tg == gid) { if ((int)(t & 0xFF) < rl) KT_BAD(1, a, gid, t); return; }
    if (tg > gid) { if (tg == (KT_OUTSIDE >> 8) || tg - gid <= F) KT_BAD(1, a, gid, t); return; }
    const uint32_t dist = gid - tg;
    if (tg >= gfirst) { if (dist >= D) KT_BAD(1, a, gid, t); }         /* this batch: the SPU saw it, the kernel did not */
    else if (dist < D) KT_BAD(1, a, gid, t);                           /* an earlier batch: the kernel saw it, the SPU did not */
}
static inline void kt_chk4(const KJournal* j, uint32_t a, uint32_t stride, uint32_t g0, int n, int rl, uint32_t D, uint32_t F, int* bad)
{
    for (int i = 0; i < n; i++) kt_chkq(j, a + i * stride, g0 + i + 1, g0 + 1, rl, D, F, bad);
}
static inline void kt_chkx(const KJournal* j, Z base, int32_t off, uint32_t g0, int n, int rl, uint32_t D, uint32_t F, int* bad)
{
    uint32_t w[16]; _mm512_storeu_si512(w, base);
    for (int i = 0; i < n; i++) kt_chkq(j, w[4 * i] + off, g0 + i + 1, g0 + 1, rl, D, F, bad);
}
static inline void kt_chk1(const KJournal* j, uint32_t a, uint32_t g0, int n, int* bad)
{
    (void)g0; (void)n;
    if (j->tag[(a & KLSQ) >> 4]) KT_BAD(1, a, 0, j->tag[(a & KLSQ) >> 4]);
}
/* quad i of the register: the quad at word 0 of lane i of base, plus off */
static inline Z zldx(const uint8_t* ls, Z base, int32_t off)
{
    uint32_t w[16]; _mm512_storeu_si512(w, base);
    const Z sw = ZBYTES(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    Z v = _mm512_castsi128_si512(_mm_loadu_si128((const __m128i*)(ls + ((w[0] + off) & KLSQ))));
    v = _mm512_inserti32x4(v, _mm_loadu_si128((const __m128i*)(ls + ((w[4] + off) & KLSQ))), 1);
    v = _mm512_inserti32x4(v, _mm_loadu_si128((const __m128i*)(ls + ((w[8] + off) & KLSQ))), 2);
    v = _mm512_inserti32x4(v, _mm_loadu_si128((const __m128i*)(ls + ((w[12] + off) & KLSQ))), 3);
    return _mm512_shuffle_epi8(v, sw);
}
#include "job2025_kernels_wide.h"
#include "job2025_kernels_gen.h"

typedef int (*KFn)(u128*, uint8_t*, KJournal*, __m128i*);
typedef int (*KWide)(u128*, uint8_t*);
typedef int (*KWideT)(u128*, uint8_t*, KJournal*);
typedef struct {
    uint32_t addr; const char* name; KFn fn;
    const uint8_t* img; uint32_t img_ls, img_n;   /* the module bytes the kernel folded in */
    KWide wide;             /* four groups per register (kwide.py); 0: none */
    KWideT widet;           /* the same with tagged accesses (kwide2.py); 0: none */
    int ret_r3;             /* r3 is a result the caller reads */
    unsigned long long calls, native, fallback, cmp, bad, ns_nat, ns_spu, captured, wide_n;
} KDef;
#define KDEF(a, nm, w, wt, r3) { 0x##a, nm, k_##a, k_##a##_img, k_##a##_IMG_LS, sizeof k_##a##_img, w, wt, r3, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
/* At most eight: the runtime's table of native entries (SPU_NATIVE_MAX,
 * runtime/spu/spu_channels.c) has 16 slots and the other native files take 8. */
static KDef g_k[] = {
    KDEF(16320, "slerp", w_16320, 0, 0),                   /* blend of two poses, rotations by slerp */
    KDEF(16A30, "slerp, blend modes 1-6", w_16A30, 0, 0),
    KDEF(17780, "pose to 3x4", w_17780, 0, 0),
    KDEF(179C0, "anim-graph op 3", 0, 0, 1),
    KDEF(1A570, "rotation keys (48-bit)", 0, 0, 1),
    KDEF(19A20, "key decoder (19A20)", 0, 0, 1),
    KDEF(1B5F0, "key decoder (1B5F0)", 0, 0, 1),
    KDEF(1C510, "key decoder (1C510)", 0, 0, 1),
};
#define NK ((int)(sizeof g_k / sizeof g_k[0]))

/* the module bytes a kernel relies on, in the live local store */
static inline int img_ok(const KDef* k, const uint8_t* ls)
{
    const uint8_t* p = ls + k->img_ls;
    __mmask64 d = 0;
    for (uint32_t o = 0; o < k->img_n; o += 64)
        d |= _mm512_cmpneq_epi8_mask(_mm512_loadu_si512(p + o), _mm512_load_si512(k->img + o));
    return d == 0;
}

/* harness switch: 1 = the instruction-by-instruction kernels only */
static _Thread_local int t_kw_off;
static KJournal* kj_get(void)
{
    static _Thread_local KJournal* j;
    if (!j) j = (KJournal*)calloc(1, sizeof *j);
    return j;
}

/* Run kernel k on (gpr, ls). 1: done, the state is the function's result.
 * 0: nothing changed (the lifted function must run). The kernels truncate by
 * embedded rounding and need only denormals-are-zero, which every SPU thread
 * runs with (spu_fp_enter). */
static int k_run(const KDef* k, u128* gpr, uint8_t* ls)
{
    if (__builtin_expect(!(_mm_getcsr() & 0x0040u), 0)) return 0;
    if (!img_ok(k, ls)) {
#ifdef J2025K_HARNESS
        if (getenv("KDEBUG")) fprintf(stderr, "[anim] %s: module bytes differ\n", k->name);
#endif
        return 0;
    }
    if (k->wide && !t_kw_off && k->wide(gpr, ls)) {
        __atomic_fetch_add(&((KDef*)k)->wide_n, 1, __ATOMIC_RELAXED);
        return 1;
    }
    KJournal* j = kj_get();
    if (!j) return 0;
    if (k->widet && !t_kw_off) {
        if (k->widet(gpr, ls, j)) {
            kj_clear(j);
            __atomic_fetch_add(&((KDef*)k)->wide_n, 1, __ATOMIC_RELAXED);
            return 1;
        }
        kj_undo(j, ls);                       /* groups not independent here: start again */
    }
    __m128i pk = _mm_setzero_si128();
    const int ok = k->fn(gpr, ls, j, &pk);
    if (!ok) {
        kj_undo(j, ls);
#ifdef J2025K_HARNESS
        if (getenv("KDEBUG")) fprintf(stderr, "[anim] %s declined (halt or stop)\n", k->name);
#endif
        return 0;
    }
    if (j->n) kj_clear(j);
    return 1;
}

#ifdef J2025K_HARNESS
/* the four-lane estimates and conversions against the helpers; returns the
 * number of differences */
#define KST_N 27
static KNOINL void st_fast(__m128i v, __m128i* o)
{
    int n = 0;
    o[n++] = k_frest(v); o[n++] = k_frsqest(v);
    for (int i8 = 29; i8 <= 250; i8 += 37) { o[n++] = k_cuflt(v, i8); o[n++] = k_csflt(v, i8); }
    for (int i8 = 46; i8 <= 255; i8 += 23) o[n++] = k_cflts(v, i8);
    o[n++] = k_cflts(v, 165); o[n++] = k_cuflt(v, 147); o[n++] = k_csflt(v, 155);
}
static KNOINL void st_ref(__m128i v, __m128i* o)
{
    const u128 u = U(v);
    int n = 0;
    o[n++] = H(spu_frest(u)); o[n++] = H(spu_frsqest(u));
    for (int i8 = 29; i8 <= 250; i8 += 37) { o[n++] = H(spu_cuflt(u, i8)); o[n++] = H(spu_csflt(u, i8)); }
    for (int i8 = 46; i8 <= 255; i8 += 23) o[n++] = H(spu_cflts(u, i8));
    o[n++] = H(spu_cflts(u, 165)); o[n++] = H(spu_cuflt(u, 147)); o[n++] = H(spu_csflt(u, 155));
}
int j2025k_selftest(unsigned long long n)
{
    uint64_t x = 0x243F6A8885A308D3ull;
    unsigned bad = 0;
    const unsigned csr = _mm_getcsr();
    for (unsigned long long i = 0; i < n; i++) {
        uint32_t w[4];
        for (int k = 0; k < 4; k++) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            uint32_t v = (uint32_t)x;
            switch ((x >> 40) % 8) {
            case 0: v &= 0x807FFFFFu; break;                                  /* exponent 0 */
            case 1: v |= 0x7F800000u; break;                                  /* exponent 255 */
            case 2: v = (v & 0x80FFFFFFu) | 0x3E000000u; break;               /* near 1 */
            case 3: v = (uint32_t)(i & 0xFFFFFF) << 8 | (v & 0xFF); break;    /* sweeps exponent/index bits */
            case 4: v = (uint32_t)(i * 4 + k) << 18 | (v & 0x3FFFF); break;   /* every sign/exponent/index */
            default: break;
            }
            w[k] = v;
        }
        const __m128i v = VC(w[0], w[1], w[2], w[3]);
        __m128i a[KST_N + 4], b[KST_N + 4];
        st_fast(v, a);
        st_ref(v, b);
        for (int k = 0; k < KST_N; k++)
            if (_mm_movemask_epi8(_mm_cmpeq_epi8(a[k], b[k])) != 0xFFFF && bad++ < 8)
                fprintf(stderr, "selftest op %d: %08X %08X %08X %08X\n", k, w[0], w[1], w[2], w[3]);
    }
    return (int)bad;
}
int j2025k_count(void) { return NK; }
uint32_t j2025k_addr(int i) { return g_k[i].addr; }
const char* j2025k_name(int i) { return g_k[i].name; }
int j2025k_find(uint32_t addr) { for (int i = 0; i < NK; i++) if (g_k[i].addr == addr) return i; return -1; }
int j2025k_run(int i, u128* gpr, uint8_t* ls) { return k_run(&g_k[i], gpr, ls); }
void j2025k_wide(int on) { t_kw_off = !on; }
int j2025k_ret_r3(int i) { return g_k[i].ret_r3; }
unsigned long long j2025k_wide_count(int i) { return g_k[i].wide_n; }
#else
void (*spu_lookup(uint32_t addr, int image_id))(spu_context*);
int spu_native_register(int image_id, uint32_t addr, int (*fn)(spu_context*));
static int s_mode = -1;                 /* PS3RECOMP_NATIVE_ANIM */
static const char* s_cap_dir;           /* PS3RECOMP_ANIM_CAPTURE=dir[,count[,every]] */
static unsigned s_cap_max = 32, s_cap_every = 101;
static _Thread_local int t_bypass;
static inline uint64_t now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void k_init(void)
{
    const char* e = getenv("PS3RECOMP_NATIVE_ANIM");
    int m = e ? atoi(e) : 1;
    if (m < 0 || m > 2) m = 0;
    if (!__builtin_cpu_supports("avx512f") || !__builtin_cpu_supports("avx512vl")) m = 0;
    const char* c = getenv("PS3RECOMP_ANIM_CAPTURE");
    if (c && *c) {
        static char dir[512];
        snprintf(dir, sizeof dir, "%s", c);
        char* comma = strchr(dir, ',');
        if (comma) { *comma = 0; s_cap_max = (unsigned)atoi(comma + 1); char* c2 = strchr(comma + 1, ','); if (c2) s_cap_every = (unsigned)atoi(c2 + 1); }
        if (!s_cap_every) s_cap_every = 1;
        s_cap_dir = dir;
    }
    fprintf(stderr, "[anim] native animation kernels (image 201): %s%s%s\n",
            m == 2 ? "compare with the lifted code" : m ? "on" : "off", s_cap_dir ? ", capturing to " : "", s_cap_dir ? s_cap_dir : "");
    __atomic_store_n(&s_mode, m, __ATOMIC_RELEASE);
}
static void k_stats(void)
{
    static uint64_t next;
    const uint64_t now = now_ns() / 1000000000ull;
    uint64_t n = __atomic_load_n(&next, __ATOMIC_RELAXED);
    if (now < n || !__atomic_compare_exchange_n(&next, &n, now + 10, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    if (!n) return;
    for (int i = 0; i < NK; i++) {
        const KDef* k = &g_k[i];
        if (!k->calls) continue;
        fprintf(stderr, "[anim] %s (0x%X): %llu calls, %llu native, %llu lifted", k->name, k->addr, k->calls, k->native, k->fallback);
        if (s_mode == 2)
            fprintf(stderr, "; compared %llu, differ %llu; native %.2f us, lifted %.2f us per compared call",
                    k->cmp, k->bad, k->cmp ? k->ns_nat / 1e3 / k->cmp : 0.0, k->cmp ? k->ns_spu / 1e3 / k->cmp : 0.0);
        fprintf(stderr, "\n");
    }
}
static void k_dump(const char* dir, const char* tag, const KDef* k, unsigned long long id, const u128* gpr, const uint8_t* ls)
{
    char path[640];
    snprintf(path, sizeof path, "%s/%s_%05X_%06llu.bin", dir, tag, k->addr, id);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(gpr, sizeof(u128), 128, f);
    fwrite(ls, 1, SPU_LS_SIZE, f);
    fclose(f);
}
/* the registers a caller can see: the link, the stack pointer, the result
 * (for the kernels whose callers read r3), r80..r127 */
static int k_regs_differ(const u128* a, const u128* b, int r3)
{
    if (memcmp(&a[0], &b[0], 32) || (r3 && memcmp(&a[3], &b[3], 16))) return 1;
    return memcmp(&a[80], &b[80], 48 * sizeof(u128)) != 0;
}
static int k_entry(spu_context* ctx, KDef* k)
{
    if (t_bypass) return 0;
    if (__builtin_expect(__atomic_load_n(&s_mode, __ATOMIC_ACQUIRE) < 0, 0)) k_init();
    if (!s_mode) return 0;
    const unsigned long long id = __atomic_fetch_add(&k->calls, 1, __ATOMIC_RELAXED);
    k_stats();
    if (s_cap_dir && id % s_cap_every == 0 && __atomic_fetch_add(&k->captured, 1, __ATOMIC_RELAXED) < s_cap_max)
        k_dump(s_cap_dir, "in", k, id, ctx->gpr, ctx->ls);
    if (s_mode != 2) {
        if (k_run(k, ctx->gpr, ctx->ls)) { __atomic_fetch_add(&k->native, 1, __ATOMIC_RELAXED); return 1; }
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
    const int ok = k_run(k, gwork, work);
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
    const int regs = k_regs_differ(ctx->gpr, gwork, k->ret_r3);
    if (regs || memcmp(ctx->ls, work, SPU_LS_SIZE)) {
        const unsigned long long nb = __atomic_add_fetch(&k->bad, 1, __ATOMIC_RELAXED);
        if (nb <= 8) {
            uint32_t first = SPU_LS_SIZE, last = 0, nq = 0;
            for (uint32_t q = 0; q < SPU_LS_SIZE; q += 16)
                if (memcmp(ctx->ls + q, work + q, 16)) { if (first == SPU_LS_SIZE) first = q; last = q; nq++; }
            fprintf(stderr, "[anim] %s call %llu differs: %u LS quads (0x%05X..0x%05X)%s\n", k->name, id, nq,
                    first == SPU_LS_SIZE ? 0 : first, last, regs ? ", registers" : "");
            const char* dir = getenv("PS3RECOMP_ANIM_DUMP");
            if (dir) k_dump(dir, "diff", k, id, gpre, pre);
        }
    }
    ctx->pc = ctx->gpr[0]._u32[0];
    return 1;
}
#define KHOOK(i) static int k_hook_##i(spu_context* ctx) { return k_entry(ctx, &g_k[i]); }
KHOOK(0) KHOOK(1) KHOOK(2) KHOOK(3) KHOOK(4) KHOOK(5) KHOOK(6) KHOOK(7)
__attribute__((constructor)) static void k_register(void)
{
    static int (*const hooks[])(spu_context*) = { k_hook_0, k_hook_1, k_hook_2, k_hook_3, k_hook_4, k_hook_5, k_hook_6, k_hook_7 };
    _Static_assert(sizeof hooks / sizeof hooks[0] == NK, "one hook per kernel");
    for (int i = 0; i < NK; i++) spu_native_register(ANIM_IMAGE, g_k[i].addr, hooks[i]);
}
#endif
