#!/usr/bin/env python3
"""Generate native C kernels for pure local-store SPU leaf functions of job
module 2025343b09d0 (image 201) from a listing of the module (tools/
spu_disasm.py output, or the work/runs listing) and its image.

Each kernel does what the SPU code does, instruction by instruction, on host
registers. It differs from the generic lift in three ways:

  * Single-precision arithmetic (fa fs fm fma fms fnms) is one AVX-512 op
    with embedded truncation. With denormals-are-zero that is
    runtime/spu/spu_helpers.h's result unless it is at or above FLT_MAX: an
    operand with exponent 255 (an ordinary number to the SPU, Inf/NaN to
    x86) or an overflow (x86 stops at FLT_MAX). Such a result is recomputed
    by the helpers' exact fix-up (kx_*). A result is made +0 where it is a
    zero and a use other than arithmetic can see its sign.
  * shufb with a control known at generation time (il/ila/ilhu/iohl/fsmbi or
    a quad of the module image) is a fixed byte permute, and one chosen by
    selb between two such controls is two permutes and a blend. The caller
    checks the image bytes in the live LS before each run.
  * Loads and stores go straight to the LS; if a halt or stop is reachable,
    stores are journalled (KST) so the caller can undo the run.

Estimates and conversions use four-lane forms of the helpers; everything
else calls the spu_helpers.h function the lift calls. (KGEN_POISON=1 selects
an older scheme that checks only where results leave arithmetic; unused.)

usage: kgen.py listing.asm mod.bin out.h name:lo:hi [...]"""
import re
import struct
import sys

FLOAT_ARITH = {'fa', 'fs', 'fm', 'fma', 'fms', 'fnms'}
SIGN_BLIND = FLOAT_ARITH | {'fcgt', 'fcmgt', 'fceq', 'fcmeq', 'cflts', 'cfltu'}
# lane-wise estimates: a poisoned input lane makes the output lane poisoned (NaN)
PASS = {'frest', 'frsqest', 'fi'}
WORD_COMPARE = {'fcgt', 'fcmgt', 'fceq', 'fcmeq', 'ceq', 'ceqi', 'cgt', 'cgti', 'clgt', 'clgti', 'fsm'}
BRANCH = {'br', 'brz', 'brnz', 'brhz', 'brhnz'}
HALT = {'heqi', 'hgti', 'hlgti'}
STORE = {'stqd', 'stqx', 'stqa', 'stqr'}
SKIP = {'hbrr', 'hbr', 'hbra', 'nop', 'lnop', 'sync', 'dsync'}
EXPM = 0x7F800000
# what a caller can see after `bi $r0`: link, stack, result, the callee-saved
# registers (the volatile ones are dead by the ABI the module's code follows;
# checked per call site, see RESEARCH)
LIVE_OUT = {0, 1, 3} | set(range(80, 128))
# POISON_MODE (KGEN_POISON=1): check arithmetic results only where they leave
# arithmetic, and discard the run on exponent 255 there (see the docstring).
# Default: every arithmetic op checks its result (|r| >= FLT_MAX: an
# exponent-255 operand or an overflow) and recomputes that op with the exact
# helper, so every register always holds the SPU's value.
import os
POISON_MODE = os.environ.get('KGEN_POISON') == '1'


def parse(lst, lo, hi):
    ins = []
    with open(lst) as f:
        for line in f:
            m = re.match(r'([0-9A-F]{5,8}): +([0-9A-F]{8})\s+(\S+)\s*(.*)', line)
            if not m:
                continue
            a = int(m.group(1), 16)
            if lo <= a < hi:
                ops = m.group(4).strip()
                ins.append((a, m.group(3), [t.strip() for t in re.split(r',\s*', ops)] if ops else []))
    return ins


def reg(t):
    m = re.fullmatch(r'\$r(\d+)', t)
    return int(m.group(1)) if m else None


def imm(t):
    return int(t, 0)


def memop(t):
    m = re.fullmatch(r'(-?0x[0-9A-Fa-f]+|-?\d+)\(\$r(\d+)\)', t)
    return (int(m.group(1), 0), int(m.group(2))) if m else None


def uses_defs(mn, ops):
    """registers read, register written"""
    if mn in SKIP or mn == 'stop' or mn == 'br':
        return [], None
    if mn == 'stqd':
        return [reg(ops[0]), memop(ops[1])[1]], None
    if mn == 'stqx':
        return [reg(ops[0]), reg(ops[1]), reg(ops[2])], None
    if mn in ('stqa', 'stqr'):
        return [reg(ops[0])], None
    if mn in BRANCH or mn == 'bi':
        return [reg(ops[0])], None
    if mn in HALT:
        return [reg(ops[1])], None
    if mn == 'lqd':
        return [memop(ops[1])[1]], reg(ops[0])
    if mn == 'lqx':
        return [reg(ops[1]), reg(ops[2])], reg(ops[0])
    if mn in ('lqa', 'lqr'):
        return [], reg(ops[0])
    rt = reg(ops[0])
    srcs = [reg(t) for t in ops[1:] if reg(t) is not None]
    if mn == 'iohl':
        srcs = [rt]
    if mn in ('frest', 'frsqest'):
        srcs = srcs[:1]          # the listing shows a second, unused operand
    return srcs, rt


def is_move(mn, ops):
    """whole-quad copy of one register"""
    return mn in ('ori', 'ai', 'shlqbyi', 'rotqbyi', 'shlqbii', 'rotqbii') and imm(ops[2]) == 0


def shuf_words(c):
    """per result word: ('a'|'b', src word) or ('k', constant word); None if not word-granular"""
    ctl = struct.pack('>4I', *c)
    out = []
    for w in range(4):
        b = ctl[4 * w:4 * w + 4]
        if all(x & 0x80 for x in b):
            v = bytes(0x80 if (x & 0xE0) == 0xE0 else 0xFF if (x & 0xC0) == 0xC0 else 0 for x in b)
            out.append(('k', struct.unpack('>I', v)[0]))
        elif all(not (x & 0x80) for x in b) and b[0] % 4 == 0 and all(b[k] == b[0] + k for k in range(4)):
            out.append(('b' if b[0] & 0x10 else 'a', (b[0] & 15) // 4))
        else:
            return None
    return out


class St:
    """per register: known constant; may hold an x86 arithmetic result; is a
    word mask; each word is the same word of one of two constants (cc)"""
    __slots__ = ('c', 'fa', 'wm', 'cc')

    def __init__(self):
        self.c = {}
        self.fa = set()
        self.wm = set()
        self.cc = {}

    def copy(self):
        s = St(); s.c = dict(self.c); s.fa = set(self.fa); s.wm = set(self.wm); s.cc = dict(self.cc); return s

    def meet(self, o):
        """self ^= o; True if self changed"""
        c = {r: v for r, v in self.c.items() if o.c.get(r) == v}
        cc = {r: v for r, v in self.cc.items() if o.cc.get(r) == v}
        fa = self.fa | o.fa
        wm = self.wm & o.wm
        ch = c != self.c or fa != self.fa or wm != self.wm or cc != self.cc
        self.c, self.fa, self.wm, self.cc = c, fa, wm, cc
        return ch


def is_wordmask_const(c):
    return all(x in (0, 0xFFFFFFFF) for x in c)


class Gen:
    def __init__(self, mod, name, ins):
        self.mod, self.name, self.ins = mod, name, ins
        self.idx = {a: i for i, (a, _, _) in enumerate(ins)}
        self.labels = set()
        for a, mn, ops in ins:
            if mn in BRANCH:
                t = imm(ops[-1])
                if t not in self.idx:
                    sys.exit('%s: branch at %05X leaves the function (%05X)' % (name, a, t))
                self.labels.add(t)
            if mn == 'bi' and reg(ops[0]) != 0:
                sys.exit('%s: indirect branch at %05X' % (name, a))
            if mn in ('brsl', 'bisl', 'brasl', 'bisled', 'iret'):
                sys.exit('%s: call at %05X' % (name, a))
        self.rodq = []

    def rodata(self, a):
        o = (a & 0x3FFF0) - 0x4000
        if 0 <= o and o + 16 <= len(self.mod):
            return struct.unpack('>4I', self.mod[o:o + 16])
        return None

    # ---------------------------------------------------------------- transfer
    def step(self, s, i):
        """state before instruction i -> state after (in place)"""
        a, mn, ops = self.ins[i]
        u, d = uses_defs(mn, ops)
        if d is None:
            return
        c = None; fa = False; wm = False; cc = None
        if mn in ('il', 'ilh', 'ilhu', 'ila', 'fsmbi'):
            v = imm(ops[1])
            if mn == 'il':
                v &= 0xFFFF; v = (v - 0x10000 if v & 0x8000 else v) & 0xFFFFFFFF; c = (v,) * 4
            elif mn == 'ilh':
                v &= 0xFFFF; c = (v << 16 | v,) * 4
            elif mn == 'ilhu':
                c = ((v & 0xFFFF) << 16,) * 4
            elif mn == 'ila':
                c = (v & 0x3FFFF,) * 4
            else:
                c = struct.unpack('>4I', bytes(0xFF if v >> (15 - k) & 1 else 0 for k in range(16)))
        elif mn == 'iohl':
            if d in s.c:
                c = tuple(x | (imm(ops[1]) & 0xFFFF) for x in s.c[d])
        elif mn in ('lqa', 'lqr'):
            c = self.rodata(imm(ops[1]))
        elif is_move(mn, ops):
            r = u[0]; c = s.c.get(r); fa = r in s.fa; wm = r in s.wm; cc = s.cc.get(r)
        elif mn in FLOAT_ARITH or mn in PASS:
            fa = True
        elif mn in ('rotqbyi', 'shlqbyi') and imm(ops[2]) % 4 == 0:
            r = u[0]; fa = r in s.fa; wm = r in s.wm
        elif mn == 'selb':
            ra, rb, rc = u
            if rc in s.wm:
                fa = ra in s.fa or rb in s.fa
                wm = ra in s.wm and rb in s.wm
                A, B = s.cc.get(ra), s.cc.get(rb)
                if A and B and A[0] == A[1] and B[0] == B[1]:
                    cc = (A[0], B[0])
                elif A and A == B:
                    cc = A
        elif mn == 'shufb':
            ra, rb, rc = u
            ws = self.shuf_ctl_words(s, rc)
            if ws is not None:
                srcs = {ra if k == 'a' else rb for w in ws for k, _ in w if k != 'k'}
                fa = any(r in s.fa for r in srcs)
                wm = all(r in s.wm for r in srcs) and all(v in (0, 0xFFFFFFFF) for w in ws for k, v in w if k == 'k')
        elif mn in WORD_COMPARE:
            wm = True
        if c is not None and is_wordmask_const(c):
            wm = True
        if c is not None:
            cc = (c, c)
        s.c.pop(d, None); s.fa.discard(d); s.wm.discard(d); s.cc.pop(d, None)
        if c is not None:
            s.c[d] = c
        if cc is not None:
            s.cc[d] = cc
        if fa:
            s.fa.add(d)
        if wm:
            s.wm.add(d)

    def shuf_ctl_words(self, s, rc):
        """word maps of the possible controls in rc, if all are word-granular"""
        if rc not in s.cc:
            return None
        ws = [shuf_words(x) for x in s.cc[rc]]
        return None if any(w is None for w in ws) else ws

    def sinks(self, s, i):
        """registers instruction i uses in a way an x86 arithmetic poison must not reach"""
        a, mn, ops = self.ins[i]
        u, d = uses_defs(mn, ops)
        if mn in FLOAT_ARITH or mn in PASS or is_move(mn, ops):
            return []
        if mn in ('rotqbyi', 'shlqbyi') and imm(ops[2]) % 4 == 0:
            return []
        if mn == 'selb':
            ra, rb, rc = u
            if rc in s.wm:
                return [rc] if rc in s.fa else []
            return [r for r in u if r in s.fa]
        if mn == 'shufb':
            ra, rb, rc = u
            if self.shuf_ctl_words(s, rc) is not None:
                return [rc] if rc in s.fa else []
            return [r for r in u if r in s.fa]
        if mn == 'bi':
            return [r for r in sorted(s.fa) if r in LIVE_OUT]
        return [r for r in u if r in s.fa]

    # ---------------------------------------------------------------- analysis
    def analyse(self):
        n = len(self.ins)
        succ = []
        for i, (a, mn, ops) in enumerate(self.ins):
            if mn == 'br':
                succ.append([self.idx[imm(ops[0])]])
            elif mn in BRANCH:
                succ.append([self.idx[imm(ops[-1])]] + ([i + 1] if i + 1 < n else []))
            elif mn in ('bi', 'stop'):
                succ.append([])
            else:
                succ.append([i + 1] if i + 1 < n else [])
        pre = [None] * n
        pre[0] = St()
        work = [0]
        while work:
            i = work.pop()
            s = pre[i].copy()
            self.step(s, i)
            for j in succ[i]:
                if pre[j] is None:
                    pre[j] = s.copy(); work.append(j)
                elif pre[j].meet(s):
                    work.append(j)
        self.pre = pre

    # ---------------------------------------------------------------- emission
    def need_canon(self, i, r):
        """does the arithmetic result written to r by instruction i reach a sign-sensitive use?"""
        for j in range(i + 1, len(self.ins)):
            a, mn, ops = self.ins[j]
            if a in self.labels:
                return True
            u, d = uses_defs(mn, ops)
            if r in u and mn not in SIGN_BLIND:
                return True
            if mn in BRANCH or mn in HALT or mn in ('bi', 'stop'):
                return True
            if d == r:
                return False
        return True

    def shuf_const(self, ra, rb, c):
        """result host byte h <- control SPU byte h ^ 3"""
        ctl = struct.pack('>4I', *c)
        ia = [0x80] * 16; ib = [0x80] * 16; orc = [0] * 16
        for h in range(16):
            x = ctl[h ^ 3]
            if x & 0x80:
                orc[h] = 0x80 if (x & 0xE0) == 0xE0 else 0xFF if (x & 0xC0) == 0xC0 else 0x00
            elif x & 0x10:
                ib[h] = (x & 15) ^ 3
            else:
                ia[h] = (x & 15) ^ 3
        if ra == rb:
            ia = [ib[h] if ia[h] == 0x80 else ia[h] for h in range(16)]
            ib = [0x80] * 16

        def bl(x):
            return 'VB(' + ', '.join('0x%02X' % v for v in x) + ')'
        parts = []
        if any(v != 0x80 for v in ia):
            parts.append('_mm_shuffle_epi8(r%d, %s)' % (ra, bl(ia)))
        if any(v != 0x80 for v in ib):
            parts.append('_mm_shuffle_epi8(r%d, %s)' % (rb, bl(ib)))
        if any(orc):
            parts.append(bl(orc))
        if not parts:
            return '_mm_setzero_si128()'
        e = parts[0]
        for p in parts[1:]:
            e = '_mm_or_si128(%s, %s)' % (e, p)
        return e

    def emit(self, out):
        ins = self.ins
        # a run can stop half way (halt, stop) only if such an instruction is reachable:
        # then its stores must be undoable
        self.journal = any(self.pre[i] is not None and (mn in HALT or mn == 'stop') for i, (a, mn, ops) in enumerate(ins))
        used = set(); defd = set()
        for a, mn, ops in ins:
            u, d = uses_defs(mn, ops)
            used.update(x for x in u if x is not None)
            if d is not None:
                defd.add(d)
        L = []
        E = L.append
        checked = set()
        npk = 0
        for i, (a, mn, ops) in enumerate(ins):
            s = self.pre[i]
            if a in self.labels:
                E('L_%05X:' % a)
                checked = set()
            if s is None:
                continue                                  # unreachable
            if mn in SKIP:
                continue
            cm = '/* %05X %s %s */' % (a, mn, ', '.join(ops))
            for r in (self.sinks(s, i) if POISON_MODE else []):
                if r not in checked:
                    E('    PK(r%d, 0x%05X);' % (r, a)); checked.add(r); npk += 1
            u, d = uses_defs(mn, ops)
            if d is not None:
                checked.discard(d)
            if mn == 'stop':
                E('    return 0; %s' % cm); continue
            if mn in STORE:
                if mn == 'stqd':
                    off, ra = memop(ops[1]); addr = 'W0(r%d) + (%d)' % (ra, off)
                elif mn == 'stqx':
                    addr = 'W0(r%d) + W0(r%d)' % (reg(ops[1]), reg(ops[2]))
                else:
                    addr = '0x%X' % imm(ops[1])
                E('    %s(%s, r%d); %s' % ('KST' if self.journal else 'KSTN', addr, reg(ops[0]), cm)); continue
            if mn == 'br':
                E('    goto L_%05X; %s' % (imm(ops[0]), cm)); continue
            if mn in ('brz', 'brnz'):
                E('    if (W0(r%d) %s 0) goto L_%05X; %s' % (reg(ops[0]), '==' if mn == 'brz' else '!=', imm(ops[1]), cm)); continue
            if mn in ('brhz', 'brhnz'):
                E('    if ((W0(r%d) & 0xFFFF) %s 0) goto L_%05X; %s' % (reg(ops[0]), '==' if mn == 'brhz' else '!=', imm(ops[1]), cm)); continue
            if mn == 'bi':
                E('    goto L_ret; %s' % cm); continue
            if mn in HALT:
                ra = reg(ops[1]); v = imm(ops[2])
                cond = {'heqi': '(int32_t)W0(r%d) == (int32_t)(%d)', 'hgti': '(int32_t)W0(r%d) > (int32_t)(%d)',
                        'hlgti': 'W0(r%d) > (uint32_t)(%d)'}[mn] % (ra, v)
                E('    if (%s) return 0; %s' % (cond, cm)); continue
            rt = d
            post = s.copy(); self.step(post, i)
            if rt in post.c:
                c = post.c[rt]
                if mn in ('lqa', 'lqr'):
                    ad = imm(ops[1]) & 0x3FFF0
                    if (ad, c) not in self.rodq:
                        self.rodq.append((ad, c))
                E('    r%d = VC(0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu); %s' % ((rt,) + c + (cm,))); continue
            if mn == 'lqd':
                off, ra = memop(ops[1])
                E('    r%d = KLD(W0(r%d) + (%d)); %s' % (rt, ra, off, cm)); continue
            if mn == 'lqx':
                E('    r%d = KLD(W0(r%d) + W0(r%d)); %s' % (rt, reg(ops[1]), reg(ops[2]), cm)); continue
            if mn in ('lqa', 'lqr'):
                E('    r%d = KLD(0x%X); %s' % (rt, imm(ops[1]) & 0x3FFF0, cm)); continue
            if mn == 'iohl':
                E('    r%d = _mm_or_si128(r%d, _mm_set1_epi32(0x%X)); %s' % (rt, rt, imm(ops[1]) & 0xFFFF, cm)); continue
            if is_move(mn, ops):
                E('    r%d = r%d; %s' % (rt, u[0], cm)); continue
            if mn in FLOAT_ARITH:
                A = tuple('F(r%d)' % r for r in u)
                e = {'fa': 'KADD(%s, %s)', 'fs': 'KSUB(%s, %s)', 'fm': 'KMUL(%s, %s)',
                     'fma': 'KFMA(%s, %s, %s)', 'fms': 'KFMS(%s, %s, %s)',
                     'fnms': 'KFNMA(%s, %s, %s)'}[mn] % A
                if not POISON_MODE:
                    ex = 'kx_%s(%s)' % (mn, ', '.join('r%d' % r for r in u))
                    E('    { const __m128i t_ = I(%s); r%d = KBAD(t_) ? %s : %s; } %s'
                      % (e, rt, ex, 'CANON(t_)' if self.need_canon(i, rt) else 't_', cm))
                elif self.need_canon(i, rt):
                    E('    r%d = CANON(I(%s)); %s' % (rt, e, cm))
                else:
                    E('    r%d = I(%s); %s' % (rt, e, cm))
                continue
            if mn == 'shufb':
                ra, rb, rc = u
                if rc in s.c:
                    E('    r%d = %s; %s' % (rt, self.shuf_const(ra, rb, s.c[rc]), cm))
                elif rc in s.cc:
                    cA, cB = s.cc[rc]
                    E('    r%d = _mm_mask_blend_epi32(_mm_cmpeq_epi32_mask(r%d, VC(0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu)), %s, %s); %s'
                      % ((rt, rc) + cA + (self.shuf_const(ra, rb, cB), self.shuf_const(ra, rb, cA), cm)))
                else:
                    E('    r%d = H(spu_shufb(U(r%d), U(r%d), U(r%d))); %s' % (rt, ra, rb, rc, cm))
                continue
            if mn == 'selb':
                ra, rb, rc = u
                E('    r%d = _mm_ternarylogic_epi32(r%d, r%d, r%d, 0xCA); %s' % (rt, rc, rb, ra, cm)); continue
            if mn in ('and', 'or', 'xor'):
                op = {'and': '_mm_and_si128', 'or': '_mm_or_si128', 'xor': '_mm_xor_si128'}[mn]
                E('    r%d = %s(r%d, r%d); %s' % (rt, op, reg(ops[1]), reg(ops[2]), cm)); continue
            args = []
            for t in ops[1:]:
                r = reg(t)
                args.append('U(r%d)' % r if r is not None else t)
            if mn in ('frest', 'frsqest'):
                args = args[:1]
            fast = None
            if mn in ('frest', 'frsqest'):
                fast = 'k_%s(r%d)' % (mn, u[0])
            elif mn in ('cuflt', 'csflt') and 29 <= imm(ops[2]) <= 250:
                fast = 'k_%s(r%d, %d)' % (mn, u[0], imm(ops[2]))
            elif mn == 'cflts' and 46 <= imm(ops[2]) <= 255:
                fast = 'k_cflts(r%d, %d)' % (u[0], imm(ops[2]))
            if fast and not POISON_MODE:
                E('    r%d = %s; %s' % (rt, fast, cm)); continue
            if POISON_MODE and mn in PASS and any(r in s.fa for r in u):
                src = sorted({r for r in u if r in s.fa})
                E('    r%d = POISON(H(spu_%s(%s)), %s); %s' % (rt, mn, ', '.join(args), ' | '.join('PKM(r%d)' % r for r in src), cm))
            else:
                E('    r%d = H(spu_%s(%s)); %s' % (rt, mn, ', '.join(args), cm))
        hdr = ['/* %s: SPU function 0x%05X..0x%05X of image 201 */' % (self.name, ins[0][0], ins[-1][0] + 4)]
        # the image quads the kernel folded in, as one span of the image (checked in the live LS)
        if self.rodq:
            lo_ = min(a for a, _ in self.rodq) & ~63; hi_ = (max(a for a, _ in self.rodq) + 16 + 63) & ~63
        else:
            lo_ = hi_ = 0x1E000
        span = self.mod[lo_ - 0x4000:hi_ - 0x4000]
        hdr.append('static const uint8_t %s_img[%d] __attribute__((aligned(64))) = {' % (self.name, max(len(span), 1)))
        for k in range(0, len(span), 16):
            hdr.append('    ' + ', '.join('0x%02X' % b for b in span[k:k + 16]) + ',')
        if not span:
            hdr.append('    0')
        hdr.append('};')
        hdr.append('#define %s_IMG_LS 0x%05X' % (self.name, lo_))
        hdr += [
                'static KNOINL int %s(u128* g, uint8_t* ls, KJournal* jr, __m128i* pkp)' % self.name,
                '{',
                '    __m128i pk = _mm_setzero_si128();']
        for r in sorted(used | defd):
            hdr.append('    __m128i r%d = GL(%d);' % (r, r))
        tail = ['L_ret:']
        for r in sorted(defd):
            tail.append('    GS(%d, r%d);' % (r, r))
        tail += ['    *pkp = pk;', '    return 1;', '}', '']
        out.write('\n'.join(hdr + L + tail) + '\n')
        return npk


def main():
    lst, modp, outp = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(modp, 'rb') as f:
        mod = f.read()
    with open(outp, 'w') as out:
        out.write('/* Generated by work/port/tools/kgen.py from the listing of job module\n'
                  ' * 2025343b09d0 (image 201). Do not edit: change the generator. */\n\n')
        for spec in sys.argv[4:]:
            name, lo, hi = spec.split(':')
            g = Gen(mod, name, parse(lst, int(lo, 16), int(hi, 16)))
            g.analyse()
            npk = g.emit(out)
            print('%s: %d instructions, %d image quads, %d poison checks%s' % (name, len(g.ins), len(g.rodq), npk, ', stores journalled' if g.journal else ''), file=sys.stderr)


if __name__ == '__main__':
    main()
