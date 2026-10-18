/* inFAMOUS 2 (BCUS98125): the skeletal-animation job, natively.
 *
 * SPU job module 2025343b09d0 (overlay 201) animates small rigs: it decodes
 * one or two compressed animation tracks at the current time, blends them,
 * turns the pose into matrices, applies the object's root matrix and writes
 * the 3x4 skinning palette, one 4x4 matrix per bone and a block of float
 * channels. About an eighth of all SPU time in the city.
 *
 * Every instance of a rig in a frame shares the animation inputs; only the
 * root matrix and the output addresses differ. So this is a cache in front
 * of the job's main (LS 0x4FE0), written from rpi/job2025-native/RESEARCH.md
 * section 8 and checked against the job's captured transfers and a replay
 * of the SPU code (work/runs/job201/oracle):
 *
 *   - the key is every input the animation can depend on: the descriptor
 *     minus the root matrix and output EAs, the LS layout, the rig, buffer 2
 *     (clip references and times) and all guest clip data the job can read;
 *   - a miss runs the SPU job and records what main's tail needs (the op-3
 *     pose, the pose buffers, the float-blend inputs). The entry is used only
 *     if the native tail, run on the LS the job started from, reproduces the
 *     job's three PUTs byte for byte;
 *   - a hit makes the job manager calls main makes, runs the tail of main
 *     natively (0x17780, the root loop, 0x18B80, 0x4F60, the float-channel
 *     copy) plus the float blend 0x17280 (the lifted function), leaves LS as
 *     the job leaves it there and PUTs the outputs.
 *
 * Jobs whose shape the key does not cover run on the SPU, unrecorded.
 *
 * The lift calls spu_native_entry at 0x4FE0 and 0x17280 (tools/lift_spu.py,
 * NATIVE_ENTRIES); the second only snapshots the blend's inputs while a miss
 * is being recorded.
 *
 * PS3RECOMP_NATIVE_J2025: 0 (default) off; 1 on; 2 runs the SPU job on every
 * hit as well and compares its PUTs with the native ones, logging any
 * difference.
 *
 * Off by default. In the city, compare mode matched every hit, but only a
 * few props hit: characters (rigs of 268 bones, 8-11 layers, per-instance
 * time) are most of the job's cost and go to the SPU. With 1, the hit path met
 * buffers the job manager placed elsewhere than the key assumed ("buffer N is
 * ..."), and the game then wrote through NULL during loading. That has to be
 * understood before this runs for real. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_coherency.h"

extern uint8_t* vm_base;
int spu_native_register(int image_id, uint32_t addr, int (*fn)(spu_context*));
void (*spu_lookup(uint32_t addr, int image_id))(spu_context*);
int spu_ea_committed(uint32_t ea, uint32_t size);

#define J2025_IMAGE 201
#define J2025_MAIN  0x4FE0u
#define J2025_BLEND 0x17280u

/* ======================= the model (oracle: j2025_core.h) ======================= */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define J_LS_SIZE   0x40000u
#define J_API_PTR   0x1E9A0u     /* the CRT stores the API table pointer here */
#define J_MOD_END   0x1EAF0u     /* end of .bss: buffer 1 starts at the next 1 KB */
#define J_FRAME     0x2A0u       /* main's frame size */
#define J_KEY_MAX   0x2000u
#define JMUT 0        /* the oracle builds mutants with other values */

static inline uint32_t j_r32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint16_t j_r16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline void j_p32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static inline u128 j_lq(const uint8_t* ls, uint32_t a)
{
    const uint8_t* p = ls + (a & (J_LS_SIZE - 16) & ~15u);
    u128 v; for (int i = 0; i < 4; i++) v._u32[i] = j_r32(p + 4 * i); return v;
}
static inline void j_sq(uint8_t* ls, uint32_t a, u128 v)
{
    uint8_t* p = ls + (a & (J_LS_SIZE - 16) & ~15u);
    for (int i = 0; i < 4; i++) j_p32(p + 4 * i, v._u32[i]);
}
static inline u128 j_v4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 v; v._u32[0] = a; v._u32[1] = b; v._u32[2] = c; v._u32[3] = d; return v; }
static inline u128 j_spl(u128 v, int i) { return j_v4(v._u32[i], v._u32[i], v._u32[i], v._u32[i]); }

/* Guest reader: pointer to `size` bytes at `ea`, or NULL if not readable. */
typedef const uint8_t* (*j_guest_fn)(void* user, uint32_t ea, uint32_t size);

typedef struct {
    uint32_t desc, b1, s1, list, slist, b2, s2, b3;   /* LS layout */
    uint32_t nent, fcnt[2], fdec[2];                  /* entries; float channels each clip writes */
    uint32_t shape;                                   /* hash of the time-independent inputs */
} j_layout;

typedef struct { uint8_t b[J_KEY_MAX]; uint32_t n; int bad; } j_key;

static void j_kput(j_key* k, const void* p, uint32_t n)
{
    if (k->n + n > J_KEY_MAX) { k->bad = 1; return; }
    memcpy(k->b + k->n, p, n); k->n += n;
}
static void j_kput32(j_key* k, uint32_t v) { uint8_t b[4]; j_p32(b, v); j_kput(k, b, 4); }

static uint32_t j_hash(const uint8_t* p, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* Unsupported job shapes (reason bits). */
enum { JF_LAYOUT = 1, JF_DESC = 2, JF_CLIP = 4, JF_GUEST = 8, JF_KEY = 16, JF_ENTRY = 32, JF_BIG = 64, JF_SHAPE = 128 };
#define J_MAX_BONES 64

/* The descriptor ([API+0x20]) and the LS layout, from the module's side only:
 * the manager's own tables are the kernel's format, and its getbuf
 * allocates DMA tags, so neither is read or called. Buffer 1 (the rig)
 * starts at the first 1 KB after .bss and its size is the u16 at rig+0x1C.
 * Buffer 2 starts at the next 1 KB; for the supported shapes it holds
 * `entries` clip entries of 16 bytes (plus 16 bytes of blend parameters
 * with two entries), gathered by a one-element DMA list in the last 16
 * bytes of buffer 2's 1 KB region. Buffer 3 follows. */
static int j_desc_of(const uint8_t* ls, j_layout* L)
{
    const uint32_t api = j_r32(ls + J_API_PTR) & (J_LS_SIZE - 1);
    if (api > J_LS_SIZE - 0x40) return 0;
    L->desc = j_r32(ls + api + 0x20) & (J_LS_SIZE - 1);
    return !(L->desc < 0x50 || L->desc > J_LS_SIZE - 0xA0 || (L->desc & 15));
}
static int j_layout_of(const uint8_t* ls, j_layout* L, uint32_t entries)
{
    L->b1 = (J_MOD_END + 1023u) & ~1023u;
    if (j_r32(ls + L->b1 + 0x30) != 0x45533033) return 0;     /* the rig's format tag */
    L->s1 = j_r16(ls + L->b1 + 0x1C);
    if (L->s1 < 0x60 || L->s1 > 0x8000 || (L->s1 & 15)) return 0;
    if (j_r16(ls + L->b1) > J_MAX_BONES) return -1;           /* too big a rig for the tail */
    L->b2 = (L->b1 + L->s1 + 1023u) & ~1023u;
    L->s2 = 16u * entries + (entries == 2 ? 16u : 0u);
    L->slist = 16;
    L->list = L->b2 + ((L->s2 + 1023u) & ~1023u) - L->slist;
    if (L->list + L->slist > J_LS_SIZE - 0x8000) return 0;
    const uint8_t* l = ls + L->list;
    if ((j_r32(l) & 0x7FFF) != L->s2 || !j_r32(l + 4) || j_r32(l + 8) || j_r32(l + 12)) return 0;
    L->b3 = L->list + L->slist;
    return 1;
}

/* Build the key. Returns 0 (with *why) if the job's shape is not supported.
 * Also sets L->shape, a hash of the time-independent part (descriptor
 * flags, layout, rig, clip headers). */
static int j_make_key(const uint8_t* ls, j_guest_fn g, void* gu, j_key* k, j_layout* L, uint32_t* why)
{
    k->n = 0; k->bad = 0; *why = 0; L->shape = 0;
    if (!j_desc_of(ls, L)) { *why = JF_LAYOUT; return 0; }
    const uint8_t* D = ls + L->desc;
    const uint32_t layers = j_r16(D + 0x8C), entries = j_r16(D + 0x8E);
    if (layers > 1 || entries < 1 || entries > 2) { *why = JF_DESC; return 0; }
    const int lay = j_layout_of(ls, L, entries);
    if (lay <= 0) { *why = lay < 0 ? JF_BIG : JF_LAYOUT; return 0; }
    j_kput32(k, 0x4A323031);                            /* key version */
    j_kput(k, D, 0x40); j_kput(k, D + 0x8C, 0x14);
    j_kput32(k, L->b1); j_kput32(k, L->s1); j_kput32(k, L->b2); j_kput32(k, L->s2);
    j_kput32(k, L->list); j_kput32(k, L->slist); j_kput32(k, L->b3);
    j_kput(k, ls + L->b1, L->s1);
    uint32_t shape = j_hash(k->b, k->n);                  /* everything so far is time-independent */
    j_kput(k, ls + L->b2, L->s2);
    const uint8_t* B2 = ls + L->b2;
    for (uint32_t e = 0; e < entries; e++) {
        const uint32_t ea = j_r32(B2 + 16 * e), hs = j_r16(B2 + 16 * e + 4);
        if (hs < 0x60 || hs > 0x1000 || (ea & 15)) { *why = JF_CLIP; return 0; }
        const uint8_t* H = g(gu, ea, hs);
        if (!H) { *why = JF_GUEST; return 0; }
        if (j_r32(H) != 0x45413035) { *why = JF_CLIP; return 0; }
        j_kput32(k, ea); j_kput(k, H, hs);
        shape = (shape ^ ea ^ j_hash(H, hs)) * 16777619u;
        /* float channels: 0x158A0 clears hdr[0x30] of them; the float decoder
         * (header flag 8, 0x1D4D0) writes one more */
        L->fcnt[e] = j_r32(H + 0x30); L->fdec[e] = (j_r32(H + 0x24) >> 3) & 1;
        if (L->fcnt[e] > 64) { *why = JF_CLIP; return 0; }
        /* segment table (0x15494): count = lo16(hdr+0x10), table at
         * hdr+0x38+hdr[0x38], entries {u32 size (low 15 bits), u32 offset} */
        const uint32_t nseg = j_r16(H + 0x12), toff = j_r32(H + 0x38);
        if (!toff || 0x38 + toff + 8 * nseg > hs || nseg > 64) { *why = JF_CLIP; return 0; }
        for (uint32_t s = 0; s < nseg; s++) {
            const uint8_t* T = H + 0x38 + toff + 8 * s;
            const uint32_t sz = j_r32(T) & 0x7FFF, off = j_r32(T + 4);
            if (!sz) continue;
            const uint8_t* P = g(gu, ea + off, sz);
            if (!P) { *why = JF_GUEST; return 0; }
            j_kput32(k, off); j_kput32(k, sz); j_kput(k, P, sz);
        }
        /* frame block (0x16044): abs EA hdr[0x2C], else entry EA + hdr[0x34]; size hdr[0x28] */
        const uint32_t fea = j_r32(H + 0x2C) ? j_r32(H + 0x2C) : ea + j_r32(H + 0x34), fsz = j_r32(H + 0x28);
        if (fsz > 0x400) { *why = JF_CLIP; return 0; }
        if (fsz) {
            const uint8_t* F = g(gu, fea, fsz);
            if (!F) { *why = JF_GUEST; return 0; }
            j_kput32(k, fea); j_kput(k, F, fsz);
        }
    }
    L->nent = entries; L->shape = shape;
    if (k->bad) { *why = JF_KEY; return 0; }
    return 1;
}


/* ---------------- cache entry ---------------- */
/* A pose buffer region: `val` is what the job leaves (or what it held when
 * the float blend began), `ini` what LS held when the job started. */
typedef struct { uint32_t ls, len; uint8_t* ini; uint8_t* val; } j_region;
typedef struct j_entry {
    struct j_entry* next;
    uint32_t hash, klen; uint8_t* key;
    int valid;
    uint32_t N, trs, X, pal, vdst, vsrc, vsz;    /* LS addresses main used */
    uint32_t nent, fw[2];                        /* float words each pose buffer gets */
    uint8_t* trs_val;                            /* op-3 pose, N*48 bytes */
    j_region reg[2];                             /* pose buffers of entry 0 and 1 */
    int blend; u128 bregs[16];                   /* 0x17280's registers r0..r15 */
} j_entry;

static void j_entry_free(j_entry* e)
{
    if (!e) return;
    for (int i = 0; i < 2; i++) { free(e->reg[i].ini); free(e->reg[i].val); }
    free(e->trs_val); free(e->key); free(e);
}

/* Record from the SPU job. ini: LS the job started from (at least the window
 * [L->b3, L->b3 + 0x8000)); fin: LS when main returned; sp: r1 at main's
 * entry. For a two-entry job, pre/pregs: LS and registers at the entry of
 * the float blend 0x17280 (the job's only other writer of the pose buffers). */
static j_entry* j_record(const j_key* k, const j_layout* L, const uint8_t* ini, const uint8_t* fin,
                         const uint8_t* pre, const u128* pregs, uint32_t sp, uint32_t* why)
{
    const uint32_t f = (sp - J_FRAME) & (J_LS_SIZE - 1);
    *why = JF_ENTRY;
    if (f < 0x1000 || f > J_LS_SIZE - J_FRAME) return 0;
    if (L->nent == 2 && (!pre || !pregs)) return 0;
    const uint8_t* F = fin + f;
    j_entry* e = (j_entry*)calloc(1, sizeof *e);
    if (!e) return 0;
    e->N    = j_r32(F + 0x1D4);
    e->trs  = j_r32(F + 0x214); e->X = j_r32(F + 0x218); e->pal = j_r32(F + 0x21C);
    e->vdst = j_r32(F + 0x90);  e->vsz = j_r32(F + 0x94);
    e->vsrc = j_r32(F + 0x48);
    const uint32_t plo = j_r32(F + 0x40), phi = j_r32(F + 0x44) + 16, plen = phi - plo;
    /* main's tail branches: none of the paths the model lacks */
    const int plain = j_r32(F + 0x1D0) == e->N && j_r32(F + 0x188) == 0 &&
                      j_r32(F + 0x220) == e->vdst && j_r32(F + 0x224) == e->vsz;
    const uint32_t G = (e->N + 3) & ~3u;
    const uint32_t lo = L->b3, hi = L->b3 + 0x8000;
    #define INWIN(a, n) ((a) >= lo && (a) + (n) <= hi && !((a) & 15))
    if (!plain || !e->N || e->N > 64 || (e->vsz & 15) || !e->vsz || e->vsz > 0x100 ||
        !INWIN(e->trs, 48 * G) || !INWIN(e->X, 64 * e->N > 48 * G ? 64 * e->N : 48 * G) ||
        !INWIN(e->pal, 48 * e->N) || !INWIN(e->vdst, e->vsz) || e->vsrc < plo || e->vsrc + e->vsz > phi ||
        phi <= plo || plen > 0x1000 || !INWIN(plo, plen * L->nent)) { j_entry_free(e); return 0; }
    #undef INWIN
    e->nent = L->nent;
    for (uint32_t i = 0; i < e->nent; i++) {
        e->fw[i] = L->fcnt[i] + L->fdec[i];
        j_region* r = &e->reg[i];
        r->ls = plo + i * plen; r->len = plen;
        r->ini = (uint8_t*)malloc(plen); r->val = (uint8_t*)malloc(plen);
        if (!r->ini || !r->val) { j_entry_free(e); return 0; }
        memcpy(r->ini, ini + r->ls, plen);
        memcpy(r->val, (e->nent == 2 ? pre : fin) + r->ls, plen);
    }
    e->trs_val = (uint8_t*)malloc(48 * e->N);
    if (!e->trs_val) { j_entry_free(e); return 0; }
    memcpy(e->trs_val, fin + e->trs, 48 * e->N);
    if (e->nent == 2) { e->blend = 1; memcpy(e->bregs, pregs, sizeof e->bregs); }
    e->key = (uint8_t*)malloc(k->n);
    if (!e->key) { j_entry_free(e); return 0; }
    memcpy(e->key, k->b, k->n); e->klen = k->n; e->hash = j_hash(k->b, k->n);
    *why = 0;
    return e;
}

/* ---------------- the native tail of main ---------------- */
typedef struct {
    uint32_t pal_ea, vec_ea, mat_ea, N, vsz;
    uint8_t pal[64 * 48], vec[0x100], mat[64 * 64];
} j_out;

/* 0x17780: per bone (q, t, s) at P -> rows (m0, m1, m2, t), four bones per
 * group; the group after the current one is loaded before its stores. */
static inline void j_bone(const u128 q, const u128 t, const u128 s, u128 out[3])
{
    const u128 two = j_v4(0x40000000, 0x40000000, 0x40000000, 0x40000000);
    const u128 half = j_v4(0x3F000000, 0x3F000000, 0x3F000000, 0x3F000000);
    const u128 qx = j_spl(q, 0), qy = j_spl(q, 1), qz = j_spl(q, 2), qw = j_spl(q, 3);
    const u128 sx = spu_fm(j_spl(s, 0), two), sy = spu_fm(j_spl(s, 1), two), sz = spu_fm(j_spl(s, 2), two);
    u128 r0, r1, r2;
    r0._u32[0] = spu_fm(spu_fnms(qz, qz, spu_fnms(qy, qy, half)), sx)._u32[0];
    r0._u32[1] = spu_fm(JMUT == 1 ? spu_fma(qw, qz, spu_fm(qx, qy)) : spu_fnms(qw, qz, spu_fm(qx, qy)), sy)._u32[0];
    r0._u32[2] = spu_fm(spu_fma(qw, qy, spu_fm(qx, qz)), sz)._u32[0];
    r0._u32[3] = t._u32[0];
    r1._u32[0] = spu_fm(spu_fma(qw, qz, spu_fm(qx, qy)), sx)._u32[0];
    r1._u32[1] = spu_fm(spu_fnms(qz, qz, spu_fnms(qx, qx, half)), sy)._u32[0];
    r1._u32[2] = spu_fm(spu_fnms(qw, qx, spu_fm(qy, qz)), sz)._u32[0];
    r1._u32[3] = t._u32[1];
    r2._u32[0] = spu_fm(spu_fnms(qw, qy, spu_fm(qx, qz)), sx)._u32[0];
    r2._u32[1] = spu_fm(spu_fma(qw, qx, spu_fm(qy, qz)), sy)._u32[0];
    r2._u32[2] = spu_fm(spu_fnms(qy, qy, spu_fnms(qx, qx, half)), sz)._u32[0];
    r2._u32[3] = t._u32[2];
    out[0] = r0; out[1] = r1; out[2] = r2;
}
static void j_17780(uint8_t* ls, uint32_t X, uint32_t P, uint32_t N)
{
    const uint32_t groups = (N + 3) >> 2;
    u128 in[12], nx[12];
    for (int i = 0; i < 12; i++) in[i] = j_lq(ls, P + 16 * i);
    for (uint32_t g = 0; g < groups; g++) {
        for (int i = 0; i < 12; i++) nx[i] = j_lq(ls, P + 192 * (g + 1) + 16 * i);
        for (int b = 0; b < 4; b++) {
            u128 o[3]; j_bone(in[3 * b], in[3 * b + 1], in[3 * b + 2], o);
            for (int r = 0; r < 3; r++) j_sq(ls, X + 192 * g + 48 * b + 16 * r, o[r]);
        }
        memcpy(in, nx, sizeof in);
    }
}
/* 0x5814: rows of X[b] (3 x (c0, c1, c2, t)) by the root's rows R0..R3:
 * out_i[c] = fma(R3[i], w[c], fma(R2[i], X2[c], fma(R1[i], X1[c], fm(R0[i], X0[c])))) */
static void j_root(uint8_t* ls, uint32_t X, uint32_t N, const uint8_t* D)
{
    const u128 R[4] = { j_lq(D, 0x40), j_lq(D, 0x50), j_lq(D, 0x60), j_lq(D, 0x70) };
    const u128 w = j_v4(0, 0, 0, JMUT == 2 ? 0 : 0x3F800000);
    for (uint32_t b = 0; b < N; b++) {
        const u128 x0 = j_lq(ls, X + 48 * b), x1 = j_lq(ls, X + 48 * b + 16), x2 = j_lq(ls, X + 48 * b + 32);
        for (int i = 0; i < 3; i++) {
            const u128 o = spu_fma(j_spl(R[3], i), w, spu_fma(j_spl(R[2], i), x2,
                           spu_fma(j_spl(R[1], i), x1, spu_fm(j_spl(R[0], i), x0))));
            j_sq(ls, X + 48 * b + 16 * i, o);
        }
    }
}
/* 0x18B80, in place over the inverse bind table B:
 * out_i[c] = fma(w[c], X_i[c], fma(B2[c], X_i[2], fma(B1[c], X_i[1], fm(B0[c], X_i[0])))) */
static void j_palette(uint8_t* ls, uint32_t pal, uint32_t X, uint32_t N)
{
    const u128 w = j_v4(0, 0, 0, 0x3F800000);
    for (uint32_t b = 0; b < N; b++) {
        const u128 B0 = j_lq(ls, pal + 48 * b), B1 = j_lq(ls, pal + 48 * b + 16), B2 = j_lq(ls, pal + 48 * b + 32);
        u128 o[3];
        for (int i = 0; i < 3; i++) {
            const u128 x = j_lq(ls, X + 48 * b + 16 * i);
            o[i] = JMUT == 3 ? spu_fma(w, x, spu_fma(B2, j_spl(x, 2), spu_fma(B1, j_spl(x, 1), spu_fm(j_spl(x, 0), B0))))
                             : spu_fma(w, x, spu_fma(B2, j_spl(x, 2), spu_fma(B1, j_spl(x, 1), spu_fm(B0, j_spl(x, 0)))));
            if (JMUT == 4) o[i] = spu_fma(B2, j_spl(x, 2), spu_fma(B1, j_spl(x, 1), spu_fm(B0, j_spl(x, 0))));
        }
        for (int i = 0; i < 3; i++) j_sq(ls, pal + 48 * b + 16 * i, o[i]);
    }
}
/* 0x4F60, in place, last bone first: 3x4 rows -> 4x4 (columns become rows,
 * w = 0 for rows 0-2 and 1.0 for row 3). */
static void j_to44(uint8_t* ls, uint32_t X, uint32_t N)
{
    for (uint32_t b = N; b-- > 0;) {
        const u128 x0 = j_lq(ls, X + 48 * b), x1 = j_lq(ls, X + 48 * b + 16), x2 = j_lq(ls, X + 48 * b + 32);
        for (int k = 0; k < 4; k++)
            j_sq(ls, X + 64 * b + 16 * k, j_v4(x0._u32[k], x1._u32[k], x2._u32[k], k == 3 && JMUT != 8 ? 0x3F800000u : 0));
    }
}

/* Apply the entry to `ls` (the job's LS at main entry, live) and run the tail.
 * invbind: N*48 bytes from guest rig+0x24. Writes LS as the SPU job leaves
 * it in the regions the tail reads or writes, and fills *o. */
/* Runs guest function `pc` on `ls` with registers r0..r15 (the float blend). */
typedef int (*j_call_fn)(void* user, uint8_t* ls, uint32_t pc, const u128* regs);

/* Apply the entry to `ls` (the job's LS at main entry, live) and run the
 * tail. invbind: N*48 bytes from guest rig+0x24. Leaves LS as the SPU job
 * leaves it in the regions the tail reads, and fills *o. Returns 0 if the
 * blend could not be run. */
static int j_tail(const j_entry* e, uint8_t* ls, const uint8_t* D, const uint8_t* invbind,
                  j_call_fn call, void* cu, j_out* o)
{
    /* Pose buffers. Float channel words below fw[] are written by the job;
     * the others are never written and keep whatever LS holds (they reach
     * the 16-byte vector through the blend). Other words: those the job
     * changed. */
    for (uint32_t i = 0; i < e->nent; i++) {
        const j_region* r = &e->reg[i];
        const uint32_t fl = e->vsrc - e->reg[0].ls;            /* float block offset */
        for (uint32_t a = 0; a < r->len; a += 4) {
            const int isf = a >= fl && a < fl + e->vsz, mask = a >= r->len - 16;   /* last quad: the frame block GET */
            const uint32_t fw = JMUT == 5 ? e->fw[i] - 1 : JMUT == 6 ? e->fw[i] + 1 : e->fw[i];
            if ((mask && JMUT != 7) || (isf ? (a - fl) / 4 < fw : memcmp(r->ini + a, r->val + a, 4) != 0))
                memcpy(ls + r->ls + a, r->val + a, 4);
        }
    }
    if (e->blend && JMUT != 10 && (!call || !call(cu, ls, 0x17280, e->bregs))) return 0;
    memcpy(ls + e->trs, e->trs_val, 48 * e->N);                  /* op-3 pose */
    if (JMUT != 9) memmove(ls + e->vdst, ls + e->vsrc, e->vsz);  /* 0x1D8E8 */
    memcpy(ls + e->pal, invbind, 48 * e->N);                     /* GET at 0x564C */
    j_17780(ls, e->X, e->trs, e->N);
    j_root(ls, e->X, e->N, D);
    j_palette(ls, e->pal, e->X, e->N);
    o->N = e->N; o->vsz = e->vsz;
    o->pal_ea = j_r32(D + 0x84); o->vec_ea = j_r32(D + 0x88); o->mat_ea = j_r32(D + 0x80);
    memcpy(o->pal, ls + e->pal, 48 * e->N);                      /* PUT 0x59CC */
    j_to44(ls, e->X, e->N);
    memcpy(o->vec, ls + e->vdst, e->vsz);                        /* PUT 0x5A34 */
    memcpy(o->mat, ls + e->X, 64 * e->N);                        /* PUT 0x5AD4 */
    return 1;
}


/* ======================= in-game glue ======================= */

static const uint8_t* guest_read(void* u, uint32_t ea, uint32_t size)
{
    (void)u;
    if (ea < 0x10000u || (uint64_t)ea + size > 0x100000000ull || !spu_ea_committed(ea, size ? size : 1)) return 0;
    return vm_base + ea;
}

/* A PUT as the MFC does it: peers holding a reservation on a written line
 * lose it (spu_dma.h, mfc_do_transfer_fast). Same as ec5f_native.c. */
static void guest_put(spu_context* ctx, uint32_t ea, const uint8_t* src, uint32_t size)
{
    const uint32_t a0 = ea & ~127u, a1 = (ea + size - 1u) & ~127u;
    int reserved = 0;
    for (uint32_t a = a0; ; a += 128u) { if (spu_coh_is_reserved(a)) { reserved = 1; break; } if (a == a1) break; }
    if (reserved) {
        const uint64_t m = spu_lockline_mask_range(ea, size);
        spu_lockline_lock_mask(m);
        memcpy(vm_base + ea, src, size);
        for (uint32_t a = a0; ; a += 128u) { if (spu_coh_is_reserved(a)) spu_coh_notify_write_except(a, ctx); if (a == a1) break; }
        spu_lockline_unlock_mask(m);
    } else {
        memcpy(vm_base + ea, src, size);
        for (uint32_t a = a0; ; a += 128u) {
            if (spu_coh_is_reserved(a)) {
                const uint64_t m = spu_lockline_mask_ea(a);
                spu_lockline_lock_mask(m);
                spu_coh_notify_write_except(a, ctx);
                spu_lockline_unlock_mask(m);
            }
            if (a == a1) break;
        }
    }
    extern void (*g_spu_put_hook)(uint32_t ea, uint32_t size);   /* cellGcmSys: SPU-written pushbuffers */
    if (g_spu_put_hook) g_spu_put_hook(ea, size);
}

/* ---------------- calls into guest code, as the lift makes them ---------------- */
/* A lifted function of this overlay (the float blend, or main itself). */
static int call_lifted(spu_context* ctx, uint32_t pc, uint32_t link)
{
    void (*fn)(spu_context*) = spu_lookup(pc, ctx->resident_ovl);
    if (!fn) return 0;
    ctx->gpr[0] = spu_link(link);
    const int32_t si = (int32_t)ctx->image_id;
    ctx->host_depth++; fn(ctx); spu_drain_call(ctx, link); ctx->host_depth--;
    spu_img_restore(ctx, si);
    return 1;
}
/* The job manager's API (bisl through the table at [0x1E9A0]), with main's link. */
static void call_api(spu_context* ctx, uint32_t off, uint32_t link)
{
    const uint32_t api = j_r32(ctx->ls + J_API_PTR) & (J_LS_SIZE - 1);
    const uint32_t tgt = j_r32(ctx->ls + api + off) & (J_LS_SIZE - 1) & ~3u;
    ctx->gpr[0] = spu_link(link);
    const int32_t si = (int32_t)ctx->image_id;
    ctx->pc = tgt; ctx->host_depth++; spu_indirect_branch(ctx); spu_drain_call(ctx, link); ctx->host_depth--;
    spu_img_restore(ctx, si);
}
static int blend_cb(void* u, uint8_t* ls, uint32_t pc, const u128* regs)
{
    spu_context* ctx = (spu_context*)u;
    if (ls != ctx->ls) return 0;
    for (int i = 2; i < 16; i++) ctx->gpr[i] = regs[i];
    return call_lifted(ctx, pc, 0x147DCu);           /* main's call site of the blend */
}

/* ---------------- the cache ---------------- */
#define J_CACHE_N 128
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static j_entry* s_slot[J_CACHE_N];
static int s_ref[J_CACHE_N];
static unsigned s_next;

static int cache_get(const j_key* k, j_entry** out)          /* returns a slot, or -1 */
{
    const uint32_t h = j_hash(k->b, k->n);
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < J_CACHE_N; i++) {
        j_entry* e = s_slot[i];
        if (e && e->hash == h && e->klen == k->n && !memcmp(e->key, k->b, k->n)) {
            s_ref[i]++; *out = e; pthread_mutex_unlock(&s_lock); return i;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return -1;
}
static void cache_put_ref(int i)
{
    pthread_mutex_lock(&s_lock); s_ref[i]--; pthread_mutex_unlock(&s_lock);
}
static void cache_add(j_entry* e)
{
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < J_CACHE_N; i++)                      /* recorded meanwhile by another SPU */
        if (s_slot[i] && s_slot[i]->hash == e->hash && s_slot[i]->klen == e->klen && !memcmp(s_slot[i]->key, e->key, e->klen)) {
            pthread_mutex_unlock(&s_lock); j_entry_free(e); return;
        }
    for (unsigned n = 0; n < J_CACHE_N; n++) {               /* oldest unused slot */
        const unsigned i = (s_next + n) % J_CACHE_N;
        if (s_ref[i]) continue;
        j_entry_free(s_slot[i]); s_slot[i] = e; s_next = i + 1;
        pthread_mutex_unlock(&s_lock); return;
    }
    pthread_mutex_unlock(&s_lock);
    j_entry_free(e);
}

/* ---------------- per thread ---------------- */
typedef struct {
    j_key key; j_layout L; j_out o;
    uint8_t* ini;              /* full-size LS images; only the window is used */
    uint8_t* pre;
    uint8_t* tmp;
    u128 pregs[16];
    int rec, pre_ok;           /* recording a miss: the blend hook snapshots */
    u128 gpr[128];
} T;
static _Thread_local T* t_t;
static _Thread_local int t_bypass;
static int s_mode = -1;
static unsigned long long s_native, s_record, s_invalid, s_spu, s_cmp, s_bad, s_ns_nat, s_ns_spu;
static unsigned s_reason[8];

/* Shapes (time-independent inputs, j_layout.shape) whose recordings keep
 * failing: their jobs skip the recording and run on the SPU directly. */
#define J_SHAPE_N 256
static struct { uint32_t h; uint16_t fail, ok; } s_shape[J_SHAPE_N];
static int shape_bad(uint32_t h)
{
    pthread_mutex_lock(&s_lock);
    const unsigned i = h % J_SHAPE_N;
    const int bad = s_shape[i].h == h && s_shape[i].fail >= 2 && !s_shape[i].ok;
    pthread_mutex_unlock(&s_lock);
    return bad;
}
static void shape_note(uint32_t h, int good)
{
    pthread_mutex_lock(&s_lock);
    const unsigned i = h % J_SHAPE_N;
    if (s_shape[i].h != h) { s_shape[i].h = h; s_shape[i].fail = s_shape[i].ok = 0; }
    if (good) { if (s_shape[i].ok < 0xFFFF) s_shape[i].ok++; } else if (s_shape[i].fail < 0xFFFF) s_shape[i].fail++;
    pthread_mutex_unlock(&s_lock);
}
/* PS3RECOMP_J2025_INVDUMP=prefix: the first recordings that fail or do not
 * validate, as prefix_{fail|inval}_N_{ini|fin}.bin (LS at job entry, as far
 * as the window goes, and LS when main returned). */
static void inv_dump(spu_context* ctx, const T* t, const char* kind, uint32_t why)
{
    static int n; const char* d = getenv("PS3RECOMP_J2025_INVDUMP");
    int k;
    if (!d || (k = __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED)) >= 8) return;
    char path[512];
    snprintf(path, sizeof path, "%s_%s_%d_ini.bin", d, kind, k);
    FILE* f = fopen(path, "wb"); if (f) { fwrite(t->ini, 1, J_LS_SIZE, f); fclose(f); }
    snprintf(path, sizeof path, "%s_%s_%d_fin.bin", d, kind, k);
    f = fopen(path, "wb"); if (f) { fwrite(ctx->ls, 1, J_LS_SIZE, f); fclose(f); }
    fprintf(stderr, "[j2025] %s recording dumped (%s, why %X, r1 %08X)\n", kind, path, why, ctx->gpr[1]._u32[0]);
}

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
    char why[160] = ""; int w = 0;
    for (int i = 0; i < 8; i++)
        if (s_reason[i]) w += snprintf(why + w, sizeof why - w, " %X:%u", 1u << i, s_reason[i]);
    fprintf(stderr, "[j2025] native %llu, recorded %llu (invalid %llu), SPU %llu (reasons%s)%s",
            s_native, s_record, s_invalid, s_spu, w ? why : " none", s_mode == 2 ? "" : "\n");
    if (s_mode == 2) fprintf(stderr, ", compared %llu, differ %llu; per compared job native %.1f us, SPU %.1f us\n",
                             s_cmp, s_bad, s_cmp ? s_ns_nat / 1e3 / s_cmp : 0.0, s_cmp ? s_ns_spu / 1e3 / s_cmp : 0.0);
}

/* 0x17280 entry: while a miss is recorded, keep the blend's inputs. */
static int j2025_blend_hook(spu_context* ctx)
{
    T* t = t_t;
    if (!t || !t->rec || t->pre_ok) return 0;
    memcpy(t->pre + t->L.b3, ctx->ls + t->L.b3, 0x8000);
    memcpy(t->pregs, ctx->gpr, sizeof t->pregs);
    t->pre_ok = 1;
    return 0;                                                 /* the SPU code runs on */
}

/* The job's main on the SPU, from inside its native entry. */
static void run_spu_main(spu_context* ctx)
{
    t_bypass = 1;
    const uint32_t r0 = ctx->gpr[0]._u32[0];
    void (*fn)(spu_context*) = spu_lookup(J2025_MAIN, ctx->resident_ovl);
    const int32_t si = (int32_t)ctx->image_id;
    if (fn) { ctx->host_depth++; fn(ctx); spu_drain_call(ctx, r0); ctx->host_depth--; spu_img_restore(ctx, si); }
    t_bypass = 0;
}

/* All output ranges committed, before anything is done. */
static int outs_ok(const j_entry* e, const uint8_t* D)
{
    const uint32_t pal = j_r32(D + 0x84), vec = j_r32(D + 0x88), mat = j_r32(D + 0x80);
    return guest_read(0, pal, 48 * e->N) && guest_read(0, mat, 64 * e->N) && (!vec || guest_read(0, vec, e->vsz));
}
static int same_as_guest(const j_out* o)
{
    if (memcmp(vm_base + o->pal_ea, o->pal, 48 * o->N)) return 0;
    if (memcmp(vm_base + o->mat_ea, o->mat, 64 * o->N)) return 0;
    if (o->vec_ea && memcmp(vm_base + o->vec_ea, o->vec, o->vsz)) return 0;
    return 1;
}

/* The native tail on the live LS, as of `start` (the window's contents at
 * job entry); LS is left as it was. Used to validate an entry and in mode 2. */
static int tail_dry(spu_context* ctx, T* t, const j_entry* e, const uint8_t* start)
{
    const uint8_t* D = ctx->ls + t->L.desc;
    const uint8_t* ib = guest_read(0, j_r32(ctx->ls + t->L.b1 + 0x24), 48 * e->N);
    if (!ib) return 0;
    memcpy(t->tmp + t->L.b3, ctx->ls + t->L.b3, 0x8000);
    memcpy(ctx->ls + t->L.b3, start + t->L.b3, 0x8000);
    memcpy(t->gpr, ctx->gpr, sizeof t->gpr);
    const int ok = j_tail(e, ctx->ls, D, ib, blend_cb, ctx, &t->o);
    memcpy(ctx->gpr, t->gpr, sizeof t->gpr);
    memcpy(ctx->ls + t->L.b3, t->tmp + t->L.b3, 0x8000);
    return ok;
}

static int j2025_main(spu_context* ctx)
{
    if (t_bypass) return 0;
    if (__builtin_expect(s_mode < 0, 0)) {
        const char* m = getenv("PS3RECOMP_NATIVE_J2025");
        s_mode = m ? atoi(m) : 0;   /* off by default: see the header */
        fprintf(stderr, "[j2025] native animation job: %s\n",
                s_mode == 2 ? "compare with the SPU job" : s_mode ? "on" : "off");
    }
    if (!s_mode) return 0;
    if (!t_t) {
        T* t = (T*)calloc(1, sizeof(T));
        if (t) { t->ini = (uint8_t*)malloc(J_LS_SIZE); t->pre = (uint8_t*)malloc(J_LS_SIZE); t->tmp = (uint8_t*)malloc(J_LS_SIZE); }
        if (!t || !t->ini || !t->pre || !t->tmp) { if (t) { free(t->ini); free(t->pre); free(t->tmp); free(t); } return 0; }
        t_t = t;
    }
    T* t = t_t;
    stats();

    /* ---- key: pure reads of LS and guest memory ---- */
    uint32_t why = 0;
    if (!j_make_key(ctx->ls, guest_read, 0, &t->key, &t->L, &why)) {
        { static int dumped[8]; const char* d = getenv("PS3RECOMP_J2025_LSDUMP");
          int bit = __builtin_ctz(why | 256); int n;
          if (d && bit < 8 && (n = __atomic_fetch_add(&dumped[bit], 1, __ATOMIC_RELAXED)) < 6) {
              char path[512]; snprintf(path, sizeof path, "%s_why%X_%d.bin", d, why, n);
              FILE* f = fopen(path, "wb"); if (f) { fwrite(ctx->ls, 1, J_LS_SIZE, f); fclose(f); }
              fprintf(stderr, "[j2025] LS dumped to %s, r3=%08X r4=%08X\n", path, ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0]); } }
        for (int i = 0; i < 8; i++) if (why >> i & 1) __atomic_fetch_add(&s_reason[i], 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_spu, 1, __ATOMIC_RELAXED);
        return 0;
    }
    j_entry* e = 0;
    const int slot = cache_get(&t->key, &e);
    if (slot >= 0 && !e->valid) {                             /* a shape the tail does not model */
        cache_put_ref(slot);
        __atomic_fetch_add(&s_spu, 1, __ATOMIC_RELAXED);
        return 0;
    }
    const uint8_t* D = ctx->ls + t->L.desc;
    const uint32_t sp = ctx->gpr[1]._u32[0];

    if (slot >= 0 && s_mode == 1) {
        /* ---- hit ---- */
        const uint8_t* ib = guest_read(0, j_r32(ctx->ls + t->L.b1 + 0x24), 48 * e->N);
        if (!ib || !outs_ok(e, D) || (e->blend && !spu_lookup(J2025_BLEND, ctx->resident_ovl))) {
            cache_put_ref(slot); __atomic_fetch_add(&s_reason[3], 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&s_spu, 1, __ATOMIC_RELAXED);
            return 0;
        }
        memcpy(t->gpr, ctx->gpr, sizeof t->gpr);
        /* main's job-manager calls, in main's order, from main's frame */
        ctx->gpr[1]._u32[0] = sp - J_FRAME; ctx->gpr[1]._u32[1] -= J_FRAME;
        call_api(ctx, 0x44, 0x5078u);                         /* API+68: lets the manager prefetch */
        static const uint32_t link[3] = { 0x50B8u, 0x5240u, 0x5320u };
        const uint32_t want[3] = { (t->L.b1 >> 2) << 16 | (t->L.s1 >> 2), (t->L.b2 >> 2) << 16 | (t->L.s2 >> 2), (t->L.b3 >> 2) << 16 };
        for (int i = 1; i <= 3; i++) {                        /* getbuf(0, i, 0, i == 3) */
            memset(&ctx->gpr[3], 0, 16); memset(&ctx->gpr[5], 0, 16);
            ctx->gpr[4]._u32[0] = i; ctx->gpr[6]._u32[0] = i == 3;
            call_api(ctx, 0x34, link[i - 1]);
            /* the layout the key was built from (buffer 3: LS only) */
            const uint32_t got = ctx->gpr[3]._u32[1];
            if (i == 3 ? (got & 0xFFFF0000u) != want[2] : got != want[i - 1]) {
                static int nl; if (nl++ < 8) fprintf(stderr, "[j2025] buffer %d is %08X, the key assumed %08X\n", i, got, want[i - 1]);
            }
        }
        ctx->gpr[1] = t->gpr[1];
        const int ok = j_tail(e, ctx->ls, D, ib, blend_cb, ctx, &t->o);
        memcpy(ctx->gpr, t->gpr, sizeof t->gpr);
        cache_put_ref(slot);
        if (!ok) { static int nl; if (nl++ < 4) fprintf(stderr, "[j2025] float blend not run on a hit\n"); }
        guest_put(ctx, t->o.pal_ea, t->o.pal, 48 * t->o.N);
        if (t->o.vec_ea) guest_put(ctx, t->o.vec_ea, t->o.vec, t->o.vsz);
        guest_put(ctx, t->o.mat_ea, t->o.mat, 64 * t->o.N);
        __atomic_fetch_add(&s_native, 1, __ATOMIC_RELAXED);
        ctx->pc = ctx->gpr[0]._u32[0];
        return 1;
    }

    if (slot >= 0) {
        /* ---- mode 2 on a hit: native tail dry, then the SPU job, then compare ---- */
        memcpy(t->ini + t->L.b3, ctx->ls + t->L.b3, 0x8000);
        const uint64_t t0 = now_ns();
        const int ok = tail_dry(ctx, t, e, t->ini);
        const uint64_t t1 = now_ns();
        cache_put_ref(slot);
        run_spu_main(ctx);
        const uint64_t t2 = now_ns();
        __atomic_fetch_add(&s_ns_nat, t1 - t0, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_ns_spu, t2 - t1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_cmp, 1, __ATOMIC_RELAXED);
        if (!ok || !same_as_guest(&t->o)) {
            unsigned long long nb = __atomic_add_fetch(&s_bad, 1, __ATOMIC_RELAXED);
            if (nb <= 8) fprintf(stderr, "[j2025] job differs (%d): palette EA %08X, %u bones\n", ok, t->o.pal_ea, t->o.N);
        }
        ctx->pc = ctx->gpr[0]._u32[0];
        return 1;
    }

    /* ---- miss: the SPU job, recorded ---- */
    if (shape_bad(t->L.shape)) {
        __atomic_fetch_add(&s_reason[7], 1, __ATOMIC_RELAXED);    /* JF_SHAPE */
        __atomic_fetch_add(&s_spu, 1, __ATOMIC_RELAXED);
        return 0;
    }
    memset(t->ini, 0, J_LS_SIZE);
    memcpy(t->ini + t->L.b3, ctx->ls + t->L.b3, 0x8000);
    t->rec = 1; t->pre_ok = 0;
    run_spu_main(ctx);
    t->rec = 0;
    j_entry* ne = j_record(&t->key, &t->L, t->ini, ctx->ls, t->pre_ok ? t->pre : 0, t->pregs, sp, &why);
    if (ne) {
        ne->valid = tail_dry(ctx, t, ne, t->ini) && same_as_guest(&t->o);
        __atomic_fetch_add(&s_record, 1, __ATOMIC_RELAXED);
        if (!ne->valid) { __atomic_fetch_add(&s_invalid, 1, __ATOMIC_RELAXED); inv_dump(ctx, t, "inval", 0); }
        shape_note(t->L.shape, ne->valid);
        cache_add(ne);
    } else {
        __atomic_fetch_add(&s_reason[5], 1, __ATOMIC_RELAXED);
        inv_dump(ctx, t, "fail", why);
        shape_note(t->L.shape, 0);
    }
    ctx->pc = ctx->gpr[0]._u32[0];
    return 1;
}

__attribute__((constructor)) static void j2025_register(void)
{
    spu_native_register(J2025_IMAGE, J2025_MAIN, j2025_main);
    spu_native_register(J2025_IMAGE, J2025_BLEND, j2025_blend_hook);
}
