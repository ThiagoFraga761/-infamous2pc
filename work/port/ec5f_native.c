/* inFAMOUS 2 (BCUS98125): the render-list builder, natively.
 *
 * SPU job module ec5f11015784 (overlay 225) walks the frame's render queue and
 * writes RSX command blocks into a ring: per object a material, constants,
 * vertex streams and the draw. It is about a fifth of all SPU time in the city.
 * This is its main function (LS 0x12C50) for pass types 0 and 2, written from
 * rpi/ec5f-native/RESEARCH.md section 10 and checked against the job's
 * captured transfers and a replay of the SPU code (work/runs/ec5f/oracle).
 *
 * The lift calls spu_native_entry at 0x12C50 (tools/lift_spu.py,
 * NATIVE_ENTRIES). The job is built into private blocks, reading the inputs
 * from guest memory where the job would have fetched them. If it took a
 * branch the model does not have (section 10.2), nothing has been written and
 * the job runs on the SPU as before. Otherwise the blocks get their ring
 * addresses from the same atomic the job uses, are linked and written, and
 * the 32-byte record is left in the output buffer for the job's wrapper to PUT.
 *
 * PS3RECOMP_NATIVE_EC5F: 1 (default) on; 0 off; 2 runs the SPU job and
 * compares its command blocks with the native ones, logging any difference. */
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

#define EC5F_IMAGE 225
#define EC5F_MAIN  0x12C50u
#define F1 0x3F800000u

static inline uint32_t R32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline uint16_t R16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline int16_t S16(const uint8_t* p) { return (int16_t)R16(p); }
static inline void P32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static inline u128 V4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 v; v._u32[0] = a; v._u32[1] = b; v._u32[2] = c; v._u32[3] = d; return v; }
static inline u128 LQ(const uint8_t* p) { return V4(R32(p), R32(p + 4), R32(p + 8), R32(p + 12)); }
static inline u128 SPL(u128 v, int i) { return V4(v._u32[i], v._u32[i], v._u32[i], v._u32[i]); }
static inline u128 SPLW(uint32_t x) { return V4(x, x, x, x); }

/* Fallback reasons (bits of J.fallback). */
enum { FB_SRGB = 1, FB_CONST = 2, FB_OP11B = 4, FB_OP4 = 8, FB_OP = 16, FB_KIND2 = 32, FB_KIND10 = 64,
       FB_KIND11 = 128, FB_SIDE = 256, FB_IN1_9 = 512, FB_FPUPLOAD = 1024, FB_OP5 = 2048,
       FB_TYPE = 4096, FB_EA = 8192, FB_MEM = 16384, FB_STALE = 32768 };

typedef struct {
    uint32_t ea, used;
    uint8_t data[4096];
} Block;

typedef struct {
    /* reset for every job */
    spu_context* ctx;
    const uint8_t *IN, *IN1, *IN2;
    int fallback;
    uint32_t bad_ea, bad_size;
    uint32_t matsz;
    uint32_t rec[8];
    /* command buffer (0x14160 and the emitters' room checks) */
    uint32_t cursor, has_block, ring_ea, delta, limit, done, nblocks;
    uint8_t* cb;
    /* job scratch (LS 0x27000) */
    u128 prevM[4], op5cache, tex[16];
    uint8_t texon[16], need_mat;
    uint32_t vfmt, streams[3], stencil;
    /* kept */
    Block* blocks; uint32_t cap;
    uint8_t spill[4096];
} J;

/* ---------------- guest memory ---------------- */
int spu_ea_committed(uint32_t ea, uint32_t size);
static inline int ea_ok(uint32_t ea, uint32_t size)
{
    return ea >= 0x10000u && spu_ea_committed(ea, size);
}
static const uint8_t* G(J* j, uint32_t ea, uint32_t size)
{
    static const uint8_t zero[0x10000];     /* the largest read: 100 queue records */
    if (__builtin_expect(!ea_ok(ea, size), 0)) {
        if (!(j->fallback & FB_EA)) { j->bad_ea = ea; j->bad_size = size; }
        j->fallback |= FB_EA; return zero;
    }
    return vm_base + ea;
}

/* A PUT as the MFC does it: peers holding a reservation on a written line
 * lose it (spu_dma.h, mfc_do_transfer_fast). */
static void guest_put(J* j, uint32_t ea, const uint8_t* src, uint32_t size)
{
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
        /* a reservation taken during the copy still has to hear of it */
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
    extern void (*g_spu_put_hook)(uint32_t ea, uint32_t size);   /* cellGcmSys: SPU-written pushbuffers */
    if (g_spu_put_hook) g_spu_put_hook(ea, size);
}

/* 0x190D8: atomic add on a guest word, returning the old value. Under the
 * lock-line lock a PUTLLC takes, so an SPU's GETLLAR/PUTLLC on the same line
 * fails and retries instead of losing the update. */
static uint32_t guest_atomic_add(J* j, uint32_t ea, uint32_t v)
{
    const uint64_t m = spu_lockline_mask_ea(ea);
    spu_lockline_lock_mask(m);
    /* CAS as well: a plain PPU store to the word takes no lock */
    uint32_t* p = (uint32_t*)(vm_base + ea);
    uint32_t raw = __atomic_load_n(p, __ATOMIC_RELAXED), old;
    do old = __builtin_bswap32(raw);
    while (!__atomic_compare_exchange_n(p, &raw, __builtin_bswap32(old + v), 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
    if (spu_coh_is_reserved(ea & ~127u)) spu_coh_notify_write_except(ea, j->ctx);
    spu_lockline_unlock_mask(m);
    return old;
}

/* ---------------- command buffer ---------------- */
static void W(J* j, uint32_t v) { P32(j->cb + j->cursor, v); j->cursor += 4; }
/* 0x14160, without the ring: publish() places the blocks once the job is done */
static void next_block(J* j)
{
    if (j->has_block && j->nblocks) j->blocks[j->nblocks - 1].used = j->cursor;
    j->done += 4096; j->cursor = 0; j->has_block = 1;
    if (j->nblocks == j->cap) {
        uint32_t nc = j->cap ? 2 * j->cap : 16;
        Block* nb = (Block*)realloc(j->blocks, nc * sizeof *nb);
        if (!nb) { j->fallback |= FB_MEM; j->cb = j->spill; return; }
        j->blocks = nb; j->cap = nc;
    }
    j->cb = j->blocks[j->nblocks++].data;
}
/* The ring addresses, as the job's allocations would return them one by one
 * (clamped to the limit), the links, and the writes. Block k ends in a JUMP
 * to block k+1, or in NOPs when k+1 follows it in memory. */
static void publish(J* j)
{
    const uint32_t n = j->nblocks;
    uint32_t base = guest_atomic_add(j, j->ring_ea, 4096 * n);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t off = base + 4096 * k;
        if (off + 4096 > j->limit) off = j->limit - 4096;
        j->blocks[k].ea = off;
    }
    for (uint32_t k = 0; k < n; k++) {
        Block* b = &j->blocks[k];
        uint32_t size = b->used;
        if (k + 1 < n) {
            if (j->blocks[k + 1].ea == b->ea + 4096) { memset(b->data + b->used, 0, 4096 - b->used); size = 4096; }
            else { P32(b->data + b->used, 0x20000000u | (j->blocks[k + 1].ea + j->delta)); size = b->used + 4; }
        }
        if (!ea_ok(b->ea, size)) {
            static int warned;
            if (!warned++) fprintf(stderr, "[ec5f] ring block 0x%08X outside guest memory\n", b->ea);
            continue;
        }
        guest_put(j, b->ea, b->data, size);
    }
    j->rec[0] = j->blocks[0].ea;
}
/* every emitter's room check: cursor + bytes > end (end = base + 4092; 0 before the first block) */
static inline void room(J* j, uint32_t bytes)
{
    if (j->has_block ? j->cursor + bytes > 4092 : bytes > 0) next_block(j);
}

#define IN  (j->IN)
#define IN1 (j->IN1)
#define IN2 (j->IN2)
#define SLOT(kind) S16(IN1 + 0x110 + 2 * (kind))

static void emit_vec(J* j, int slot, const uint32_t* w, int nvec)   /* 0x1EFC burst */
{
    room(j, 8 + 16 * nvec);
    W(j, ((1 + 4 * nvec) << 18) | 0x1EFC); W(j, (uint32_t)slot);
    for (int i = 0; i < 4 * nvec; i++) W(j, w[i]);
}
/* Is [p, p+n) inside the material the job fetched? Past it the SPU reads
 * stale local store, which no native pass can reproduce: the job falls back. */
static inline int inmat(J* j, const uint8_t* mat, const uint8_t* p, uint32_t n)
{
    if (p >= mat && (uint64_t)(p - mat) + n <= j->matsz) return 1;
    j->fallback |= FB_STALE;
    return 0;
}

/* 0x143F0: textures */
static void f_143F0(J* j, const uint8_t* mat)
{
    const uint8_t* D = IN1 + 0x1C;
    uint32_t n = R32(D + 0x38);
    if (n > 14) n = 14;
    for (uint32_t i = 0; i < n; i++) {
        int unit = S16(D + 4 * i) & 15; uint16_t ti = R16(D + 4 * i + 2);
        const uint8_t* t = mat + 8 + 44 * (uint32_t)ti;
        if (!inmat(j, mat, t, 36)) return;
        uint32_t w0 = R32(t), w4 = R32(t + 4), w8 = R32(t + 8), w12 = R32(t + 12), w16 = R32(t + 16),
                 w20 = R32(t + 20), w24 = R32(t + 24), w28 = R32(t + 28), w32 = R32(t + 32);
        u128* c = &j->tex[unit];
        if (c->_u32[0] == w0 && c->_u32[1] == w8 && c->_u32[2] == w12 && c->_u32[3] == w20) continue;
        *c = V4(w0, w8, w12, w20);
        if (!j->texon[unit]) { room(j, 8); W(j, 0x00040B00 + 4 * unit); W(j, 0x2DC4); j->texon[unit] = 1; }
        room(j, 40);
        W(j, 0x001C1A00 + 0x20 * unit); W(j, w0); W(j, w4); W(j, w8); W(j, w12); W(j, w16); W(j, w20); W(j, w24);
        W(j, 0x00041840 + 4 * unit); W(j, w32);
        if ((w8 & 15) == 4 || ((w8 >> 8) & 15) == 4 || ((w8 >> 16) & 15) == 4) {
            room(j, 8); W(j, 0x00041A1C + 0x20 * unit); W(j, w28);
        }
    }
}

/* 0x15E78: material constants */
static void f_15E78(J* j, const uint8_t* mat, const uint8_t* old)
{
    uint32_t n = R32(IN1 + 0x9C), ntex = R32(IN1 + 0xB4);
    if (n > 17) n = 17;
    if (ntex > 16) { j->fallback |= FB_STALE; return; }
    const uint8_t* nb = mat + 8 + 44 * ntex; const uint8_t* ob = old ? old + 8 + 44 * ntex : 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = R32(IN1 + 0x58 + 4 * i); int slot = (int16_t)(e >> 16);
        uint32_t type = (e >> 10) & 31, off = e & 1023;
        const uint8_t* np = nb + 4 * off; const uint8_t* op = ob ? ob + 4 * off : 0;
        if (type > 11) continue;
        if (!inmat(j, mat, np, 16)) return;
        if (type == 0) {
            if (op && spu_fceq(LQ(np), LQ(op))._u32[0]) continue;      /* first word only */
            if (e & 0x8000) { j->fallback |= FB_SRGB; continue; }
            uint32_t w[4] = { R32(np), R32(np + 4), R32(np + 8), R32(np + 12) };
            emit_vec(j, slot, w, 1);
        } else if (type == 4) {
            if (op) { u128 m = spu_fceq(LQ(np), LQ(op));
                if (m._u32[0] && m._u32[1] && m._u32[2] && m._u32[3]) continue; }
            uint32_t w[8] = { R32(np), R32(np + 4), 0, F1, R32(np + 8), R32(np + 12), 0, F1 };
            emit_vec(j, slot, w, 2);
        } else j->fallback |= FB_CONST;
    }
}

/* 0x7C18: per-object constants */
static void f_7C18(J* j, const uint8_t* mat, const uint8_t* E, const uint8_t* extra, uint32_t nx)
{
    uint32_t nops = R32(IN1 + 0xC0);
    if (nops > 20) nops = 20;
    if (R32(IN1 + 0xB4) > 16 || R32(IN1 + 0xB8) > 0x100) { j->fallback |= FB_STALE; return; }
    const uint8_t* d = mat + 8 + 44 * R32(IN1 + 0xB4) + 4 * R32(IN1 + 0xB8);
    for (uint32_t i = 0; i < nops; i++) {
        uint32_t op = R32(IN1 + 0xC4 + 4 * i);
        if (op > 14) continue;
        if (op == 5) {
            if (!inmat(j, mat, d, 8)) return;
            int slot = S16(IN1 + 0xA8); int32_t a = (int32_t)R32(d), b = (int32_t)R32(d + 4); d += 8;
            if (slot < 0) continue;
            int32_t k = E[1], ab = (int32_t)((uint32_t)a * (uint32_t)b);
            if (!ab || !a) { j->fallback |= FB_OP5; continue; }
            int32_t frame = k % ab, row = frame / a, col = frame % a;
            u128 fa = spu_csflt(SPLW((uint32_t)a), 155), fb = spu_csflt(SPLW((uint32_t)b), 155);
            u128 ra = spu_fi(fa, spu_frest(fa)), rb = spu_fi(fb, spu_frest(fb));
            u128 fc = spu_csflt(SPLW((uint32_t)col), 155), fr = spu_csflt(SPLW((uint32_t)row), 155);
            u128 q1 = spu_fm(fc, ra); u128 x = spu_fma(spu_fnms(q1, fa, fc), ra, q1);
            u128 q2 = spu_fm(fr, rb); u128 y = spu_fma(spu_fnms(q2, fb, fr), rb, q2);
            uint32_t w[4] = { x._u32[0], y._u32[0], y._u32[0], y._u32[0] };
            u128 c = j->op5cache;
            if (spu_fceq(SPLW(w[0]), SPLW(c._u32[0]))._u32[0] && spu_fceq(SPLW(w[1]), SPLW(c._u32[1]))._u32[0]) continue;
            j->op5cache = V4(w[0], w[1], w[2], w[3]);
            emit_vec(j, slot, w, 1);
        } else if (op == 11) {
            if (!inmat(j, mat, d, 40)) return;
            int s3 = S16(IN1 + 0xAE), sB0 = S16(IN1 + 0xB0); uint32_t flag = R32(d); const uint8_t* dd = d; d += 40;
            if (s3 < 0 && sB0 < 0) continue;
            uint32_t m[12];
            const uint8_t* src = dd;
            if (flag != 0) {
                uint32_t key = R32(mat); const uint8_t* hit = 0;
                for (uint32_t q = 0; q < nx && extra; q++) {
                    uint32_t k0 = R32(extra + 64 * q);
                    if (k0 == key) { hit = extra + 64 * q; break; }
                    if (k0 == 0xFFFFFFFFu) hit = extra + 64 * q;
                }
                src = hit;
            }
            if (!src) { const uint32_t I[12] = { F1, 0, 0, 0, 0, F1, 0, 0, 0, 0, F1, 0 }; memcpy(m, I, sizeof m); }
            else for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++) m[4 * r + c] = R32(src + 4 + 12 * r + 4 * c);
                m[4 * r + 3] = 0;
            }
            if (s3 >= 0) emit_vec(j, s3, m, 3);
            if (sB0 >= 0) j->fallback |= FB_OP11B;
        } else if (op == 4) {
            d += 4;
            if (S16(IN1 + 0xA6) >= 0) j->fallback |= FB_OP4;
        } else j->fallback |= FB_OP;
    }
}

/* object matrix (main 0x136E0..0x13924, 0x13CA8) */
static void object_matrix(J* j, u128 M[4], const uint8_t* rec, const uint8_t* T, const uint8_t* S)
{
    u128 T0 = LQ(T), T1 = LQ(T + 16), T2 = LQ(T + 32);
    u128 S0 = LQ(S), S1 = LQ(S + 16), S2 = LQ(S + 32), S3 = LQ(S + 48);
    u128 A[4];
    for (int c = 0; c < 4; c++)
        A[c] = spu_fma(S2, SPLW(T2._u32[c]), spu_fma(S1, SPLW(T1._u32[c]), spu_fm(S0, SPLW(T0._u32[c]))));
    A[3] = spu_fa(A[3], S3);
    u128 p = V4(R32(rec + 0x28), R32(rec + 0x2C), R32(rec + 0x30), 0);
    u128 sk = spu_fm(SPLW(R32(rec + 0x34)), LQ(IN + 0x340));
    u128 D[3] = { spu_fm(sk, V4(F1, 0, 0, 0)), spu_fm(sk, V4(0, F1, 0, 0)), spu_fm(sk, V4(0, 0, F1, 0)) };
    for (int r = 0; r < 3; r++)
        M[r] = spu_fma(A[2], SPLW(D[r]._u32[2]), spu_fma(A[1], SPLW(D[r]._u32[1]), spu_fm(A[0], SPLW(D[r]._u32[0]))));
    u128 t = spu_fma(A[2], SPL(p, 2), spu_fma(A[1], SPL(p, 1), spu_fm(A[0], SPL(p, 0))));
    u128 m3 = spu_fa(A[3], t); m3._u32[3] = F1;
    if (IN1[0x178]) { m3 = spu_fs(m3, LQ(IN + 0xC0)); m3._u32[3] = F1; }
    M[3] = m3;
}
static int m_changed(J* j, const u128 M[4])     /* xyz of the 4 rows against scr+0 */
{
    for (int r = 0; r < 4; r++) {
        u128 a = M[r], b = j->prevM[r]; a._u32[3] = a._u32[2]; b._u32[3] = b._u32[2];
        u128 m = spu_fceq(a, b);
        if (!(m._u32[0] && m._u32[1] && m._u32[2] && m._u32[3])) return 1;
    }
    return 0;
}
static void mvp(J* j, uint32_t out[16], const u128 M[4])
{
    u128 VP0 = LQ(IN + 0x80), VP1 = LQ(IN + 0x90), VP2 = LQ(IN + 0xA0), VP3 = LQ(IN + 0xB0);
    for (int r = 0; r < 4; r++) {
        u128 m = M[r]; uint32_t w = r == 3 ? F1 : 0;
        u128 acc = spu_fm(VP0, SPLW(m._u32[0]));
        acc = spu_fma(VP1, SPLW(m._u32[1]), acc);
        acc = spu_fma(VP2, SPLW(m._u32[2]), acc);
        acc = spu_fma(VP3, SPLW(w), acc);
        for (int i = 0; i < 4; i++) out[4 * r + i] = acc._u32[i];
    }
}
/* 0xBDD8: type-0 matrices */
static void f_BDD8(J* j, const u128 M[4])
{
    if (!m_changed(j, M)) return;
    for (int r = 0; r < 4; r++) j->prevM[r] = M[r];
    if (SLOT(0) >= 0) {
        uint32_t w[16];
        for (int r = 0; r < 4; r++) for (int i = 0; i < 4; i++) w[4 * r + i] = M[r]._u32[i];
        emit_vec(j, SLOT(0), w, 4);
    }
    if (SLOT(1) >= 0) { uint32_t w[16]; mvp(j, w, M); emit_vec(j, SLOT(1), w, 4); }
    if (SLOT(2) >= 0) j->fallback |= FB_KIND2;
    if (SLOT(3) >= 0) {
        uint32_t w[12];
        for (int r = 0; r < 3; r++) {
            u128 v = M[r]; v._u32[3] = 0;
            u128 sq = spu_fm(v, v);
            u128 l = spu_fa(spu_fa(sq, V4(sq._u32[1], sq._u32[2], sq._u32[3], 0)), V4(sq._u32[2], sq._u32[3], 0, 0));
            l = SPL(l, 0);
            u128 rs = spu_fi(l, spu_frsqest(l));
            u128 nr = spu_fma(spu_fnms(l, spu_fm(rs, rs), SPLW(F1)), spu_fm(rs, SPLW(0x3F000000u)), rs);
            u128 o = spu_fm(v, nr); o._u32[3] = 0;
            for (int i = 0; i < 4; i++) w[4 * r + i] = o._u32[i];
        }
        emit_vec(j, SLOT(3), w, 3);
    }
    if (SLOT(10) >= 0 || SLOT(11) >= 0) j->fallback |= FB_KIND10;
}
/* 0xB010: type-2 matrix and polygon offset; returns whether the offset is on */
static int f_B010(J* j, const u128 M[4], const uint8_t* E)
{
    if (SLOT(11) >= 0 || SLOT(1) >= 0) {
        if (m_changed(j, M)) {
            for (int r = 0; r < 4; r++) j->prevM[r] = M[r];
            if (SLOT(1) >= 0) { uint32_t w[16]; mvp(j, w, M); emit_vec(j, SLOT(1), w, 4); }
            if (SLOT(11) >= 0) j->fallback |= FB_KIND11;
        }
    }
    if ((E[0] & IN1[0x10F] & 4) && IN[0x153] == 0) {
        room(j, 8); W(j, 0x00040A68); W(j, 1);
        room(j, 12); W(j, 0x00080A78); W(j, F1); W(j, 0x44FA0000);
        return 1;
    }
    return 0;
}
/* 0x5B60: vertex attribute 0 format */
static void f_5B60(J* j, uint32_t id)
{
    uint32_t v;
    switch (id) {
    case 0: case 2: v = 0x0C32; break;
    case 1: case 3: case 5: v = 0x0630 | id; break;
    default: return;
    }
    room(j, 8); W(j, 0x00041740); W(j, v);
}
/* 0x5C98: streams and the draw */
static void f_5C98(J* j, const uint8_t* rec)
{
    const uint8_t* L = IN1 + 0xD8;
    if (j->vfmt != R32(rec + 0x4C)) { f_5B60(j, R32(rec + 0x4C)); j->vfmt = R32(rec + 0x4C); }
    if (j->streams[0] != R32(rec + 0x38) || j->streams[1] != R32(rec + 0x3C) || j->streams[2] != R32(rec + 0x40)) {
        uint32_t n = L[0];
        if (n > 16) n = 16;
        room(j, 8 * n);
        for (uint32_t q = 0; q < n; q++) {
            uint32_t base = R32(rec + 0x38 + 4 * (L[17 + q] & 3));
            W(j, 0x00040000 | (0x1680 + 4 * L[1 + q])); W(j, base + L[33 + q]);
        }
        for (int i = 0; i < 3; i++) j->streams[i] = R32(rec + 0x38 + 4 * i);
    }
    uint32_t nidx = 3 * R16(rec + 4);
    if (nidx) {
        uint32_t nw = (nidx + 255) >> 8, ix = R32(rec + 0x44);
        room(j, 4 * (nw + 8));
        W(j, 0x0008181C); W(j, ix & 0x7FFFFFFF); W(j, 0x10 | (ix >> 31));
        W(j, 0x00041808); W(j, 5); W(j, 0x40001824 | (nw << 18));
        uint32_t full = nidx >> 8, i;
        for (i = 0; i < full; i++) W(j, 0xFF000000u + 256 * i);
        if (nidx & 255) W(j, (((nidx & 255) - 1) << 24) | ((256 * full) & 0x00FFFF00));
        W(j, 0x00041808); W(j, 0);
    }
}
/* 0x6FE0: stencil reference */
static void f_6FE0(J* j, const uint8_t* E)
{
    if ((IN1[0x10F] & 0xE0) == 0) return;
    uint32_t ref = E[0] & 0xE0;
    if (j->stencil == ref) return;
    room(j, 32);
    W(j, 0x000C0330); W(j, 0x207); W(j, ref); W(j, 0xFF);
    W(j, 0x000C0350); W(j, 0x207); W(j, ref); W(j, 0xFF);
    j->stencil = ref;
}

/* The branches the model lacks that IN1 alone decides, checked before any
 * work (section 10.2). Conservative: a job that might never reach one of
 * them still goes to the SPU. */
static void precheck(J* j, uint32_t type, int mat_on)
{
    if (mat_on) {
        uint32_t n = R32(IN1 + 0x9C);
        if (n > 17) n = 17;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t e = R32(IN1 + 0x58 + 4 * i), t = (e >> 10) & 31;
            if (t > 11) continue;
            if (t == 0 && (e & 0x8000)) j->fallback |= FB_SRGB;
            else if (t != 0 && t != 4) j->fallback |= FB_CONST;
        }
        if ((int32_t)R32(IN1 + 0xC0) > 0) {
            uint32_t nops = R32(IN1 + 0xC0);
            if (nops > 20) nops = 20;
            for (uint32_t i = 0; i < nops; i++) {
                uint32_t op = R32(IN1 + 0xC4 + 4 * i);
                if (op > 14 || op == 5) continue;
                if (op == 11) { if (S16(IN1 + 0xB0) >= 0) j->fallback |= FB_OP11B; }
                else if (op == 4) { if (S16(IN1 + 0xA6) >= 0) j->fallback |= FB_OP4; }
                else j->fallback |= FB_OP;
            }
        }
    }
    if (type == 0) {
        static const int ks[] = { 43, 12, 4, 17, 13, 45, 7, 6, 5, 9, 8 };
        for (unsigned i = 0; i < sizeof ks / sizeof *ks; i++) if (SLOT(ks[i]) >= 0) j->fallback |= FB_SIDE;
        if (IN1[9]) j->fallback |= FB_IN1_9;
        if (IN1[0xD4] ? 1 : IN1[0x17A]) j->fallback |= FB_FPUPLOAD;
        if (SLOT(2) >= 0) j->fallback |= FB_KIND2;
        if (SLOT(10) >= 0 || SLOT(11) >= 0) j->fallback |= FB_KIND10;
    } else if (SLOT(11) >= 0) j->fallback |= FB_KIND11;
}

/* The job's main, 0x12C50. */
static void run(J* j)
{
    uint32_t type = R32(IN1 + 4);
    if (type != 0 && type != 2) { j->fallback |= FB_TYPE; return; }
    const uint8_t* D = IN1 + 0x1C;
    const int mat_on = R32(D + 0x38) || R32(D + 0x80) || S16(IN1 + 0xA4) >= 0 || S16(IN1 + 0xA6) >= 0 ||
                       S16(IN1 + 0xA8) >= 0 || S16(IN1 + 0xAC) >= 0 || S16(IN1 + 0xAA) >= 0 ||
                       S16(IN1 + 0xAE) >= 0 || S16(IN1 + 0xB0) >= 0;
    precheck(j, type, mat_on);
    if (j->fallback) return;

    /* 0x4328: range selection */
    uint32_t cur = R32(G(j, R32(IN + 4), 4));
    uint32_t n = (uint32_t)((int32_t)(cur - R32(IN + 0)) >> 9);
    if (n > 100) n = 100;
    const uint8_t* Q = n ? G(j, R32(IN + 0), n * 512) : 0;
    if (j->fallback) return;
    uint32_t rb[100], re[100]; int front = 0, back = (int)n;
    uint32_t b = R32(IN1 + 0) & 0xFF;
    for (uint32_t k = 0; k < n; k++) {
        const uint8_t* r = Q + 512 * k;
        if (R32(r) != R32(IN + 0x2C) || R32(r + 4) != R32(IN + 0x30)) continue;
        int32_t s = R16(r + 0x0E + 2 * b), e = R16(r + 0x10 + 2 * b);
        if (IN1[0x0A] == 0) {
            int32_t N = (int32_t)R32(IN2), i = (int32_t)R32(IN2 + 4);
            if (N <= 0) { j->fallback |= FB_TYPE; return; }
            int32_t c = (e - s + N - 1) / N; int32_t s2 = (int32_t)((uint32_t)s + (uint32_t)c * (uint32_t)i); if (!(e > s2)) s2 = e;
            int32_t e2 = (int32_t)((uint32_t)s2 + (uint32_t)c); if (!(e > e2)) e2 = e; s = s2; e = e2;
        }
        if (s == e) continue;
        int idx = r[12] ? --back : front++;
        rb[idx] = R32(r + 8) + 8 * s; re[idx] = R32(r + 8) + 8 * e;
    }
    int nr = front;
    for (int i = back; i < (int)n; i++) { rb[nr] = rb[i]; re[nr] = re[i]; nr++; }
    if (!nr) return;

    j->ring_ea = R32(IN + 8); j->delta = R32(IN + 0xC); j->limit = R32(IN + 0x10);
    if (!ea_ok(j->ring_ea, 4) || j->limit < 4096) { j->bad_ea = j->ring_ea; j->bad_size = 0xF1; j->fallback |= FB_EA; return; }
    j->op5cache = SPLW(F1); j->streams[0] = j->streams[1] = j->streams[2] = 0x7FFFFFFF; j->stencil = 0xFFFFFFFF;
    j->need_mat = SLOT(0) >= 0 || SLOT(1) >= 0 || SLOT(2) >= 0 || SLOT(3) >= 0 || SLOT(10) >= 0 || SLOT(11) >= 0;
    const uint32_t matsz = (R32(IN1 + 0xBC) + 15) & ~15u;
    j->matsz = matsz;
    if (matsz > 0x300) { j->bad_ea = matsz; j->bad_size = 0xF2; j->fallback |= FB_EA; return; }

    for (int r = 0; r < nr; r++) {
        if (re[r] < rb[r] || re[r] - rb[r] > 0x100000) { j->bad_ea = rb[r]; j->bad_size = 0xF3; j->fallback |= FB_EA; return; }
        const uint8_t* items = G(j, rb[r], re[r] - rb[r]);
        uint32_t cnt = (re[r] - rb[r]) >> 3;
        uint32_t last_mat = 0; const uint8_t* old_mat = 0; const uint8_t* mat = 0;
        for (uint32_t k = 0; k < cnt; k++) {
            const uint8_t* E = G(j, R32(items + 8 * k), 16);
            const uint8_t* H = G(j, R32(items + 8 * k + 4), 48);
            uint32_t ew0 = R32(E), hw0 = R32(H);
            int reject = ((hw0 & 0x40000000) && IN[0x151]) || (hw0 & 0x80000000) || (ew0 & 0x08000000) ||
                         (IN1[0x0A] && ((ew0 >> 5) & 7) != R32(IN2 + 4));
            if (reject) continue;
            int changed = 0;
            if (mat_on && R32(E + 12) != last_mat) { mat = G(j, R32(E + 12), matsz); last_mat = R32(E + 12); changed = 1; }
            const uint8_t* rec = G(j, R32(E + 4) & ~15u, 0xB0);
            const uint8_t* T = G(j, R32(H + 0x14) + 48 * R16(E + 8), 48);
            const uint8_t* S = G(j, R32(H + 0x1C) + 64 * R16(E + 10), 64);
            uint32_t nx = R32(H + 0x20);
            if (nx > 16) { j->bad_ea = nx; j->bad_size = 0xF4; j->fallback |= FB_EA; return; }
            const uint8_t* extra = nx ? G(j, R32(H + 0x24), 64 * nx) : 0;
            if (j->fallback) return;
            if (mat_on) {
                if (changed) { f_143F0(j, mat); f_15E78(j, mat, old_mat); old_mat = mat; }
                if ((int32_t)R32(IN1 + 0xC0) > 0) f_7C18(j, mat, E, extra, nx);
            }
            u128 M[4]; object_matrix(j, M, rec, T, S);
            if (type == 0) {
                if (j->need_mat) f_BDD8(j, M);
                f_6FE0(j, E);
                f_5C98(j, rec);
            } else {
                int po = f_B010(j, M, E);
                f_5C98(j, rec);
                if (po) { room(j, 8); W(j, 0x00040A68); W(j, 0); }
            }
            j->rec[2] += 1; j->rec[5] += R16(rec + 4);
            if (j->fallback) return;
        }
    }
    j->rec[1] = j->has_block ? j->done + j->cursor - 4096 : 0xFFFFFFFCu;
    if (j->nblocks) {
        if (j->vfmt) f_5B60(j, 0);
        W(j, 0x00020000);
        j->blocks[j->nblocks - 1].used = j->cursor;
    }
}
#undef IN
#undef IN1
#undef IN2

static void job_init(J* j, spu_context* ctx)
{
    memset(j, 0, offsetof(J, blocks));
    j->ctx = ctx;
    uint32_t in = ctx->gpr[3]._u32[0] & SPU_LS_MASK & ~15u;
    if (in > SPU_LS_SIZE - 0x520) { j->fallback |= FB_EA; in = 0; }
    j->IN = ctx->ls + in; j->IN1 = j->IN + 0x380; j->IN2 = j->IN + 0x500;
}

static _Thread_local J* t_j;
static _Thread_local int t_bypass;
static int s_mode = -1;
static unsigned long long s_native, s_fallback, s_cmp, s_bad, s_ns_nat, s_ns_spu;
static inline uint64_t now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static unsigned s_reason[16];

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
    fprintf(stderr, "[ec5f] native %llu, SPU %llu (reasons%s)%s",
            s_native, s_fallback, w ? why : " none", s_mode == 2 ? "" : "\n");
    if (s_mode == 2) fprintf(stderr, ", compared %llu, differ %llu; per compared job native %.1f us, SPU %.1f us\n",
                             s_cmp, s_bad, s_cmp ? s_ns_nat / 1e3 / s_cmp : 0.0, s_cmp ? s_ns_spu / 1e3 / s_cmp : 0.0);
}

/* Mode 2: the SPU job's blocks against the native ones. Block k of the ring
 * follows the JUMP or the padding at the end of block k-1, as the RSX reads it. */
static int compare(J* j, spu_context* ctx, const uint8_t* rec_ls)
{
    uint32_t rec[8];
    for (int i = 0; i < 8; i++) rec[i] = R32(rec_ls + 4 * i);
    if (memcmp(rec + 1, j->rec + 1, 7 * 4) != 0 || (!rec[0]) != (!j->nblocks)) return 1;
    uint32_t ea = rec[0];
    for (uint32_t k = 0; k < j->nblocks; k++) {
        const Block* b = &j->blocks[k];
        if (!ea_ok(ea, 4096)) return 2;
        const uint8_t* g = vm_base + ea;
        if (memcmp(g, b->data, b->used) != 0) {
            static int nd;
            const char* dir = getenv("PS3RECOMP_EC5F_DUMP");
            if (dir && __atomic_fetch_add(&nd, 1, __ATOMIC_RELAXED) < 12) {
                char path[512]; int id = nd;
                snprintf(path, sizeof path, "%s/diff%02d_spu.bin", dir, id);
                FILE* f = fopen(path, "wb"); if (f) { fwrite(g, 1, b->used, f); fclose(f); }
                snprintf(path, sizeof path, "%s/diff%02d_nat.bin", dir, id);
                f = fopen(path, "wb"); if (f) { fwrite(b->data, 1, b->used, f); fclose(f); }
                snprintf(path, sizeof path, "%s/diff%02d_in.bin", dir, id);
                f = fopen(path, "wb"); if (f) { fwrite(j->IN, 1, 0x510, f); fclose(f); }
            }
            return 3;
        }
        if (k + 1 == j->nblocks) break;
        uint32_t tail = R32(g + b->used);
        /* JUMP = 0x20000000 | (ea + delta), delta the ring's EA-to-offset bias */
        ea = (tail & 0xE0000000u) == 0x20000000u ? (tail & 0x1FFFFFFFu) - j->delta : ea + 4096;
    }
    (void)ctx;
    return 0;
}

static int ec5f_main(spu_context* ctx)
{
    if (t_bypass) return 0;
    if (__builtin_expect(s_mode < 0, 0)) {
        const char* e = getenv("PS3RECOMP_NATIVE_EC5F");
        s_mode = e ? atoi(e) : 1;
        fprintf(stderr, "[ec5f] native render-list builder: %s\n",
                s_mode == 2 ? "compare with the SPU job" : s_mode ? "on" : "off");
    }
    if (!s_mode) return 0;
    if (!t_j) { t_j = (J*)calloc(1, sizeof(J)); if (!t_j) return 0; }
    J* j = t_j;
    stats();

    const uint64_t t0 = s_mode == 2 ? now_ns() : 0;
    job_init(j, ctx);
    run(j);
    const uint64_t t1 = s_mode == 2 ? now_ns() : 0;
    if (j->fallback) {
        __atomic_fetch_add(&s_fallback, 1, __ATOMIC_RELAXED);
        for (int i = 0; i < 16; i++) if (j->fallback >> i & 1) __atomic_fetch_add(&s_reason[i], 1, __ATOMIC_RELAXED);
        if (j->fallback & FB_EA) { static int nl;
            if (nl++ < 6) fprintf(stderr, "[ec5f] EA fallback: 0x%08X size 0x%X (type %u)\n", j->bad_ea, j->bad_size, R32(j->IN1 + 4)); }
        return 0;
    }
    const uint32_t out = ctx->gpr[9]._u32[0] & SPU_LS_MASK & ~15u;
    if (out > SPU_LS_SIZE - 32) return 0;
    if (s_mode == 2) {
        /* Run the SPU job as its wrapper would, then compare. */
        void (*fn)(spu_context*) = spu_lookup(EC5F_MAIN, ctx->resident_ovl);
        if (!fn) return 0;
        const int32_t si = (int32_t)ctx->image_id;
        t_bypass = 1;
        const uint32_t r0 = ctx->gpr[0]._u32[0];
        ctx->host_depth++; fn(ctx); spu_drain_call(ctx, r0); ctx->host_depth--;
        spu_img_restore(ctx, si);
        t_bypass = 0;
        __atomic_fetch_add(&s_ns_nat, t1 - t0, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_ns_spu, now_ns() - t1, __ATOMIC_RELAXED);
        const int bad = compare(j, ctx, ctx->ls + out);
        __atomic_fetch_add(&s_cmp, 1, __ATOMIC_RELAXED);
        if (bad) {
            unsigned long long nb = __atomic_add_fetch(&s_bad, 1, __ATOMIC_RELAXED);
            if (nb <= 8) fprintf(stderr, "[ec5f] job differs (%d): SPU record %08X %08X %08X, native %08X %08X %08X, %u blocks\n",
                                 bad, R32(ctx->ls + out), R32(ctx->ls + out + 4), R32(ctx->ls + out + 8),
                                 j->rec[0], j->rec[1], j->rec[2], j->nblocks);
        }
        ctx->pc = ctx->gpr[0]._u32[0];
        return 1;
    }
    if (j->nblocks) publish(j);
    for (int i = 0; i < 8; i++) P32(ctx->ls + out + 4 * i, j->rec[i]);
    __atomic_fetch_add(&s_native, 1, __ATOMIC_RELAXED);
    return 1;
}

__attribute__((constructor)) static void ec5f_register(void)
{
    spu_native_register(EC5F_IMAGE, EC5F_MAIN, ec5f_main);
}
