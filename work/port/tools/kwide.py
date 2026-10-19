#!/usr/bin/env python3
"""Group-parallel kernels for software-pipelined SPU loops of job module
2025343b09d0 (image 201).

Many of the module's leaf functions walk bones four at a time: each step of
the loop finishes the previous group of four and starts the next one (the
loop is software-pipelined), every group reading and writing only its own
records. kwide runs such a function symbolically on quads -- every SPU
instruction becomes a node, loads and stores get symbolic addresses
(entry register + offset) -- for a trip count of several groups, and slices
out what one group stores: a dataflow graph whose leaves are entry registers,
constants, loads at that group's offsets and pointers that step with the
group. If the slices of all groups are the same graph up to those steps, the
function is the same per-group computation repeated, and it is emitted as C
that runs four groups at once, one per 128-bit lane of an AVX-512 register.
Each node is the same SPU instruction with the same operands, so a lane
computes bit for bit what the SPU computes for its group.

Memory: stores must fall in group-private records (a stride per base
register), or be one of the stores the function makes outside the loop
(saved registers). A store of shufb(v, load(p), cwd(p)) to p -- the SPU's
word insertion -- is a word store. The kernel config says which bases may
be the same array at run time (in-place blends); the analysis rejects any
load that could see a store of the same group under that assumption, and
the emitted kernel checks the assumption and the alignment it relied on.

usage: kwide.py asm mod.bin out.h config.json
config: {"name", "lo", "hi", "count": "r10", "stride": {"r3": 192, ...},
         "alias": [["r3", "r5"], ...], "groups": 6}"""
import json
import re
import struct
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
from kgen import parse, reg, imm, memop, uses_defs, SKIP, BRANCH  # noqa: E402

MASK = 0xFFFFFFFF


class G:
    """hash-consed quad graph"""

    def __init__(self):
        self.nodes = []
        self.key = {}

    def mk(self, *k):
        if k in self.key:
            return self.key[k]
        self.nodes.append(k)
        self.key[k] = len(self.nodes) - 1
        return self.key[k]

    def kind(self, n):
        return self.nodes[n][0]

    def const(self, n):
        k = self.nodes[n]
        return k[1] if k[0] == 'const' else None


def sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


# concrete integer semantics, for control and constants (per word, SPU order)
def fold(mn, ops, cs):
    def w(f):
        return tuple(f(*[c[i] for c in cs]) & MASK for i in range(4))
    try:
        if mn == 'ai':
            return w(lambda a: a + imm(ops[2]))
        if mn == 'a':
            return w(lambda a, b: a + b)
        if mn == 'sf':
            return w(lambda a, b: b - a)
        if mn == 'sfi':
            return w(lambda a: imm(ops[2]) - a)
        if mn == 'and':
            return w(lambda a, b: a & b)
        if mn == 'or':
            return w(lambda a, b: a | b)
        if mn == 'xor':
            return w(lambda a, b: a ^ b)
        if mn == 'andi':
            return w(lambda a: a & (imm(ops[2]) & MASK))
        if mn == 'ori':
            return w(lambda a: a | (imm(ops[2]) & MASK))
        if mn == 'shli':
            return w(lambda a: a << (imm(ops[2]) & 0x3F) if (imm(ops[2]) & 0x3F) < 32 else 0)
        if mn == 'rotmi':
            sh = (-imm(ops[2])) & 0x3F
            return w(lambda a: a >> sh if sh < 32 else 0)
        if mn == 'rotmai':
            sh = (-imm(ops[2])) & 0x3F
            return w(lambda a: (sx(a, 32) >> min(sh, 31)) & MASK)
        if mn == 'ceqi':
            return w(lambda a: MASK if a == imm(ops[2]) & MASK else 0)
        if mn == 'cgti':
            return w(lambda a: MASK if sx(a, 32) > imm(ops[2]) else 0)
        if mn == 'clgti':
            return w(lambda a: MASK if a > imm(ops[2]) & MASK else 0)
    except Exception:
        return None
    return None


class Sym:
    def __init__(self, cfg, ins, mod):
        self.cfg, self.ins, self.mod = cfg, ins, mod
        self.idx = {a: i for i, (a, _, _) in enumerate(ins)}
        self.g = G()
        self.stores = []          # (seq, base, off, node, kind)  kind 'q' | 'w'
        self.mem = {}             # (base, off) -> node, for forwarding
        self.loads = []           # (seq, base, off, node)
        self.gathers = []         # (seq, node, pc): loads from data-dependent addresses
        self.alias = {}
        for cls in cfg.get('alias', []):
            for b in cls:
                self.alias[b] = cls[0]

    def canon(self, b):
        return self.alias.get(b, b)

    def rodata(self, a):
        o = (a & 0x3FFF0) - 0x4000
        if 0 <= o and o + 16 <= len(self.mod):
            return struct.unpack('>4I', self.mod[o:o + 16])
        return None

    def addr(self, n):
        """word 0 of quad n as (base, offset); base None = absolute"""
        k = self.g.nodes[n]
        if k[0] == 'const':
            return (None, k[1][0])
        if k[0] == 'reg':
            return ('r%d' % k[1], 0)
        if k[0] == 'aff':
            a = self.addr(k[1])
            return (a[0], a[1] + k[2]) if a else None
        if k[0] == 'op' and k[1] == 'shufb':
            c = self.g.const(k[3][2])
            if c is not None:
                b0 = c[0] >> 24
                if c[0] == 0x00010203:
                    return self.addr(k[3][0])
                if c[0] == 0x10111213:
                    return self.addr(k[3][1])
                del b0
        return None

    def run(self, entry_consts):
        g = self.g
        R = {}
        for r in range(128):
            R[r] = g.mk('const', entry_consts[r]) if r in entry_consts else g.mk('reg', r)
        i = 0; seq = 0; steps = 0
        while True:
            steps += 1
            if steps > 200000:
                sys.exit('runaway')
            a, mn, ops = self.ins[i]
            seq += 1
            nxt = i + 1
            if mn in SKIP:
                i = nxt; continue
            if mn == 'bi':
                self.final = dict(R)
                return
            if mn in BRANCH:
                if mn == 'br':
                    i = self.idx[imm(ops[0])]; continue
                c = g.const(R[reg(ops[0])])
                if c is None:
                    sys.exit('%05X: branch on a non-constant' % a)
                v = c[0] if mn in ('brz', 'brnz') else c[0] & 0xFFFF
                take = (v == 0) == (mn in ('brz', 'brhz'))
                i = self.idx[imm(ops[1])] if take else nxt
                continue
            if mn in ('heqi', 'hgti', 'hlgti', 'stop'):
                sys.exit('%05X: %s' % (a, mn))
            if mn in ('stqd', 'stqx', 'stqa', 'stqr'):
                v = R[reg(ops[0])]
                if mn == 'stqd':
                    off, ra = memop(ops[1]); ad = self.addr(R[ra]); ad = ad and (ad[0], ad[1] + off)
                elif mn == 'stqx':
                    a1, a2 = self.addr(R[reg(ops[1])]), self.addr(R[reg(ops[2])])
                    ad = (a1[0], a1[1] + a2[1]) if a1 and a2 and a2[0] is None else (a2[0], a2[1] + a1[1]) if a1 and a2 and a1[0] is None else None
                else:
                    ad = (None, imm(ops[1]))
                if ad is None:
                    sys.exit('%05X: store to an unknown address' % a)
                base = self.canon(ad[0]) if ad[0] else None
                kind = 'q'
                k = g.nodes[v]
                if k[0] == 'op' and k[1] == 'shufb':
                    ctl = g.nodes[k[3][2]]
                    if ctl[0] == 'op' and ctl[1] == 'cwd':
                        pa = self.addr(ctl[3][0]); pa = pa and (pa[0], pa[1] + int(ctl[2][0], 0))
                        if pa and (self.canon(pa[0]), pa[1]) == (base, ad[1]):
                            ld = g.nodes[k[3][1]]
                            if ld[0] == 'ld' and (ld[1], ld[2]) == (base, ad[1]):
                                kind = 'w'
                self.stores.append((seq, base, ad[1], v, kind, a))
                self.mem[(base, ad[1])] = v
                i = nxt; continue
            u, d = uses_defs(mn, ops)
            if mn in ('lqd', 'lqx', 'lqa', 'lqr'):
                if mn == 'lqd':
                    off, ra = memop(ops[1]); ad = self.addr(R[ra]); ad = ad and (ad[0], ad[1] + off)
                elif mn == 'lqx':
                    a1, a2 = self.addr(R[reg(ops[1])]), self.addr(R[reg(ops[2])])
                    ad = (a1[0], a1[1] + a2[1]) if a1 and a2 and a2[0] is None else (a2[0], a2[1] + a1[1]) if a1 and a2 and a1[0] is None else None
                else:
                    ad = (None, imm(ops[1]))
                if ad is None:
                    # a gather: the address is data (checked against the stores at run time)
                    if mn == 'lqd':
                        R[d] = g.mk('ldx', R[ra], off)
                    elif mn == 'lqx':
                        R[d] = g.mk('ldx2', R[reg(ops[1])], R[reg(ops[2])])
                    else:
                        sys.exit('%05X: load from an unknown address' % a)
                    self.gathers.append((seq, R[d], a))
                    i = nxt; continue
                if ad[0] is None:
                    c = self.rodata(ad[1])
                    if c is None:
                        sys.exit('%05X: absolute load outside the image' % a)
                    R[d] = g.mk('const', c); i = nxt; continue
                base = self.canon(ad[0])
                if (base, ad[1]) in self.mem:
                    R[d] = self.mem[(base, ad[1])]
                else:
                    R[d] = g.mk('ld', base, ad[1])
                    self.loads.append((seq, base, ad[1], R[d], a))
                i = nxt; continue
            # constants
            c = None
            if mn in ('il', 'ilh', 'ilhu', 'ila', 'fsmbi'):
                v = imm(ops[1])
                if mn == 'il':
                    v &= 0xFFFF; v = (v - 0x10000 if v & 0x8000 else v) & MASK; c = (v,) * 4
                elif mn == 'ilh':
                    v &= 0xFFFF; c = (v << 16 | v,) * 4
                elif mn == 'ilhu':
                    c = ((v & 0xFFFF) << 16,) * 4
                elif mn == 'ila':
                    c = (v & 0x3FFFF,) * 4
                else:
                    c = struct.unpack('>4I', bytes(0xFF if v >> (15 - k) & 1 else 0 for k in range(16)))
                R[d] = g.mk('const', c); i = nxt; continue
            if mn == 'iohl':
                pc = g.const(R[d])
                if pc is None:
                    R[d] = g.mk('op', 'iohl', (ops[1],), (R[d],))
                else:
                    R[d] = g.mk('const', tuple(x | (imm(ops[1]) & 0xFFFF) for x in pc))
                i = nxt; continue
            if mn in ('ori', 'shlqbyi', 'rotqbyi', 'ai', 'rotqbii', 'shlqbii') and imm(ops[2]) == 0:
                R[d] = R[u[0]]; i = nxt; continue
            srcs = [R[r] for r in u]
            cs = [g.const(s) for s in srcs]
            if all(x is not None for x in cs):
                f = fold(mn, ops, cs)
                if f is not None:
                    R[d] = g.mk('const', f); i = nxt; continue
            if mn == 'ai':
                k = g.nodes[srcs[0]]
                if k[0] == 'aff':
                    R[d] = g.mk('aff', k[1], k[2] + imm(ops[2]))
                else:
                    R[d] = g.mk('aff', srcs[0], imm(ops[2]))
                i = nxt; continue
            imms = tuple(t for t in ops[1:] if reg(t) is None)
            if mn in ('frest', 'frsqest'):
                imms = ()
            R[d] = g.mk('op', mn, imms, tuple(srcs))
            i = nxt


class Slice:
    """one group's part of the graph, with its leaves made relative to the group"""

    def __init__(self, s, grp):
        self.s, self.grp = s, grp
        self.memo = {}
        self.bad = []

    def step_of(self, n):
        a = self.s.addr(n)
        if a and a[0]:
            return self.s.cfg['stride'].get(self.s.canon(a[0]), 0)
        return 0

    def norm(self, n):
        """structural key of node n for this group (memoised)"""
        if n in self.memo:
            return self.memo[n]
        g = self.s.g
        k = g.nodes[n]
        st = self.s.cfg['stride']
        if k[0] == 'ld':
            sd = st.get(k[1])
            if sd:
                o = k[2] - sd * self.grp
                r = ('ld', k[1], o)
            else:
                r = ('ld', k[1], k[2])
        elif k[0] == 'aff':
            sd = self.step_of(k[1])
            r = ('aff', self.norm(k[1]), k[2] - sd * self.grp)
        elif k[0] == 'op':
            r = ('op', k[1], k[2], tuple(self.norm(x) for x in k[3]))
        else:
            r = k
        self.memo[n] = r
        return r

    def signature(self, stores):
        return tuple((b, o - self.s.cfg['stride'][b] * self.grp, kd, self.norm(v)) for _, b, o, v, kd, _ in sorted(stores, key=lambda x: x[0]))


def sig_eq(a, ref, first):
    """a == ref, where in the first group a stepping pointer may be the bare
    base: the code adds nothing to it there (offset 0 of group 0)"""
    if a == ref:
        return True
    if type(a) != tuple or type(ref) != tuple:
        return False
    if first and len(ref) == 3 and ref[0] == 'aff' and ref[2] == 0 and sig_eq(a, ref[1], first):
        return True
    if len(a) != len(ref):
        return False
    return all(sig_eq(x, y, first) for x, y in zip(a, ref))


def diff(a, b, depth=0):
    if a == b:
        return
    if type(a) != tuple or type(b) != tuple or len(a) != len(b) or a[:2] != b[:2] or depth > 60:
        print('   ' * min(depth, 20) + 'A: %s' % str(a)[:300], file=sys.stderr)
        print('   ' * min(depth, 20) + 'B: %s' % str(b)[:300], file=sys.stderr)
        return
    for x, y in zip(a, b):
        if x != y:
            if type(x) == tuple and type(y) == tuple:
                diff(x, y, depth + 1)
            else:
                print('   ' * min(depth, 20) + 'A: %s' % str(a)[:300], file=sys.stderr)
                print('   ' * min(depth, 20) + 'B: %s' % str(b)[:300], file=sys.stderr)
            return



SIGN_BLIND_Z = {'fa', 'fs', 'fm', 'fma', 'fms', 'fnms', 'fcgt', 'fceq', 'fcmgt', 'fcmeq', 'cflts', 'cfltu'}
WORD_MASK_OPS = {'fcgt', 'fceq', 'fcmgt', 'fcmeq', 'ceq', 'ceqi', 'cgt', 'cgti', 'clgt', 'clgti', 'fsm'}


def shuf_words(c):
    from kgen import shuf_words as sw
    return sw(c)


class Emit:
    def __init__(self, s, sl, stores, glob, name):
        self.s, self.sl, self.stores, self.glob, self.name = s, sl, stores, glob, name
        self.g = s.g
        self.L = []
        self.hoist = []
        self.var = {}
        self.uses = {}
        self.wm = {}

    def is_wm(self, n):
        if n in self.wm:
            return self.wm[n]
        k = self.g.nodes[n]
        r = False
        if k[0] == 'const':
            r = all(x in (0, MASK) for x in k[1])
        elif k[0] == 'op':
            if k[1] in WORD_MASK_OPS:
                r = True
            elif k[1] == 'selb':
                r = all(self.is_wm(x) for x in k[3])
            elif k[1] == 'shufb':
                c = self.g.const(k[3][2])
                ws = shuf_words(c) if c is not None else None
                if ws is not None:
                    r = all(v in (0, MASK) if t == 'k' else self.is_wm(k[3][0] if t == 'a' else k[3][1]) for t, v in ws)
        self.wm[n] = r
        return r

    def invariant(self, n):
        k = self.g.nodes[n]
        if k[0] in ('reg', 'const'):
            return True
        if k[0] == 'ld':
            return False
        if k[0] == 'aff':
            return False
        return all(self.invariant(x) for x in k[3])

    def bconst(self, c):
        return 'ZC(0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu)' % c

    def shuf_const(self, a, b, c, same):
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
        if same:
            ia = [ib[h] if ia[h] == 0x80 else ia[h] for h in range(16)]
            ib = [0x80] * 16

        def bl(x):
            return 'ZBYTES(' + ', '.join('0x%02X' % v for v in x) + ')'
        parts = []
        if any(v != 0x80 for v in ia):
            parts.append('_mm512_shuffle_epi8(%s, %s)' % (a, bl(ia)))
        if any(v != 0x80 for v in ib):
            parts.append('_mm512_shuffle_epi8(%s, %s)' % (b, bl(ib)))
        if any(orc):
            parts.append(bl(orc))
        if not parts:
            return '_mm512_setzero_si512()'
        e = parts[0]
        for p in parts[1:]:
            e = '_mm512_or_si512(%s, %s)' % (e, p)
        return e

    def topo(self, roots):
        order = []; seen = set()
        stack = [(r, 0) for r in reversed(roots)]
        while stack:
            n, st = stack.pop()
            if st == 0:
                if n in seen:
                    continue
                seen.add(n)
                stack.append((n, 1))
                k = self.g.nodes[n]
                kids = k[3] if k[0] == 'op' else (k[1],) if k[0] == 'aff' else ()
                for x in reversed(kids):
                    if x not in seen:
                        stack.append((x, 0))
            else:
                order.append(n)
        return order

    def gen(self):
        g = self.g; s = self.s
        roots = []
        outs = []
        for seq, b, o, v, kd, a in self.stores:
            if kd == 'w':
                v = g.nodes[v][3][0]               # the inserted value; its word 0 is stored
            roots.append(v); outs.append((b, o, v, kd))
        order = self.topo(roots)
        for n in order:
            k = g.nodes[n]
            kids = k[3] if k[0] == 'op' else (k[1],) if k[0] == 'aff' else ()
            for x in kids:
                self.uses.setdefault(x, []).append(n)
        sens = set(v for _, _, v, _ in outs)
        for n in order:
            k = g.nodes[n]
            if k[0] == 'op' and k[1] not in SIGN_BLIND_Z:
                sens.update(k[3])
            elif k[0] == 'op' and k[1] == 'fi':
                sens.update(k[3])
        st = s.cfg['stride']
        body = []
        for n in order:
            k = g.nodes[n]
            v = 'n%d' % n
            if k[0] == 'const':
                self.hoist.append('    const Z %s = %s;' % (v, self.bconst(k[1])))
                self.var[n] = v; continue
            if k[0] == 'reg':
                self.hoist.append('    const Z %s = ZB(GL(%d));' % (v, k[1]))
                self.var[n] = v; continue
            if k[0] == 'ld':
                base, off = k[1], k[2]
                if base not in st:
                    sys.exit('%s: load from %s+%X, not a group record' % (self.name, base, off))
                if not (0 <= off < max(st[base], 16)):
                    sys.exit('%s: load outside the group record (%s+%X)' % (self.name, base, off))
                body.append('        const Z %s = zld4(ls, B_%s + %d + g0 * %d, %d);' % (v, base, off, st[base], st[base]))
                self.var[n] = v; continue
            if k[0] == 'aff':
                x = k[1]; d = k[2]
                a = s.addr(x)
                sd = st.get(s.canon(a[0]), 0) if a and a[0] else 0
                body.append('        const Z %s = _mm512_add_epi32(%s, zlaneoff(g0, %d, %d));' % (v, self.var[x], sd, d))
                self.var[n] = v; continue
            mn, imms, args = k[1], k[2], k[3]
            A = [self.var[x] for x in args]
            e = None
            if mn in ('fa', 'fs', 'fm', 'fma', 'fms', 'fnms'):
                e = 'z%s(%s)' % (mn, ', '.join(A))
                if n in sens:
                    e = 'ZCANON(%s)' % e
            elif mn in ('fcgt', 'fceq', 'fcmgt', 'frest', 'frsqest'):
                e = 'z%s(%s)' % (mn, ', '.join(A))
            elif mn == 'fi':
                e = 'zfi(%s, %s)' % (A[0], A[1])
            elif mn in ('cuflt', 'csflt') and 29 <= imm(imms[0]) <= 250:
                e = 'z%s(%s, %d)' % (mn, A[0], imm(imms[0]))
            elif mn == 'cflts' and 46 <= imm(imms[0]) <= 255:
                e = 'zcflts(%s, %d)' % (A[0], imm(imms[0]))
            elif mn == 'selb':
                e = '_mm512_ternarylogic_epi32(%s, %s, %s, 0xCA)' % (A[2], A[1], A[0])
            elif mn == 'shufb':
                c = g.const(args[2])
                ck = g.nodes[args[2]]
                if c is not None:
                    e = self.shuf_const(A[0], A[1], c, args[0] == args[1])
                elif ck[0] == 'op' and ck[1] == 'selb' and g.const(ck[3][0]) is not None and g.const(ck[3][1]) is not None and self.is_wm(ck[3][2]):
                    ca, cb = g.const(ck[3][0]), g.const(ck[3][1])
                    # selb(ca, cb, m): words of cb where m is set
                    e = '_mm512_mask_blend_epi32(_mm512_cmpeq_epi32_mask(%s, %s), %s, %s)' % (
                        A[2], self.bconst(ca), self.shuf_const(A[0], A[1], cb, args[0] == args[1]),
                        self.shuf_const(A[0], A[1], ca, args[0] == args[1]))
                else:
                    e = 'zshufb(%s, %s, %s)' % tuple(A)
            elif mn == 'rotqby':
                e = 'zrotqby(%s, %s)' % (A[0], A[1])
            elif mn in ('ceqi', 'cgti', 'clgti'):
                iv = imm(imms[0])
                cmp = {'ceqi': '_mm512_cmpeq_epi32_mask', 'cgti': '_mm512_cmpgt_epi32_mask', 'clgti': '_mm512_cmpgt_epu32_mask'}[mn]
                e = 'ZM(%s(%s, _mm512_set1_epi32((int)0x%08Xu)))' % (cmp, A[0], iv & MASK)
            elif mn in ('ceq', 'cgt', 'clgt'):
                cmp = {'ceq': '_mm512_cmpeq_epi32_mask', 'cgt': '_mm512_cmpgt_epi32_mask', 'clgt': '_mm512_cmpgt_epu32_mask'}[mn]
                e = 'ZM(%s(%s, %s))' % (cmp, A[0], A[1])
            elif mn in ('and', 'or', 'xor'):
                e = '_mm512_%s_si512(%s, %s)' % (mn, A[0], A[1])
            elif mn == 'andc':
                e = '_mm512_andnot_si512(%s, %s)' % (A[1], A[0])
            elif mn == 'a':
                e = '_mm512_add_epi32(%s, %s)' % (A[0], A[1])
            elif mn == 'sf':
                e = '_mm512_sub_epi32(%s, %s)' % (A[1], A[0])
            elif mn == 'andi':
                e = '_mm512_and_si512(%s, _mm512_set1_epi32((int)0x%08Xu))' % (A[0], imm(imms[0]) & MASK)
            elif mn == 'ori':
                e = '_mm512_or_si512(%s, _mm512_set1_epi32((int)0x%08Xu))' % (A[0], imm(imms[0]) & MASK)
            elif mn == 'iohl':
                e = '_mm512_or_si512(%s, _mm512_set1_epi32(0x%X))' % (A[0], imm(imms[0]) & 0xFFFF)
            elif mn == 'shli':
                sh = imm(imms[0]) & 0x3F
                e = '_mm512_slli_epi32(%s, %d)' % (A[0], sh) if sh < 32 else '_mm512_setzero_si512()'
            elif mn == 'rotmi':
                sh = (-imm(imms[0])) & 0x3F
                e = '_mm512_srli_epi32(%s, %d)' % (A[0], sh) if sh < 32 else '_mm512_setzero_si512()'
            elif mn == 'rotmai':
                sh = min((-imm(imms[0])) & 0x3F, 31)
                e = '_mm512_srai_epi32(%s, %d)' % (A[0], sh)
            elif mn == 'cwd':
                e = 'ZH1(spu_cwd, %s, %s)' % (A[0], imms[0])
            else:
                hargs = ', '.join(list(A) + list(imms))
                e = 'ZH%d(spu_%s, %s)' % (len(A), mn, hargs)
            body.append('        const Z %s = %s; /* %s */' % (v, e, mn))
            self.var[n] = v
        for b, o, v, kd in outs:
            fn = 'zst4' if kd == 'q' else 'zstw4'
            body.append('        %s(ls, B_%s + %d + g0 * %d, %d, %s, nl);' % (fn, b, o, st[b], st[b], self.var[v]))
        return body


def emit_kernel(s, cfg, groups, glob, out):
    name = cfg['name']
    st = cfg['stride']
    ref = sorted(groups)[len(groups) // 2]
    sl = Slice(s, ref)
    # stores of the middle group, offsets relative to it
    stores = [(seq, b, o - st[b] * ref, v, kd, a) for seq, b, o, v, kd, a in sorted(groups[ref], key=lambda x: x[0])]
    # the graph of the middle group, as a group-relative graph: rebuild nodes through norm()
    rel = G()

    def build(key):
        if key[0] == 'op':
            return rel.mk('op', key[1], key[2], tuple(build(x) for x in key[3]))
        if key[0] == 'aff':
            return rel.mk('aff', build(key[1]), key[2])
        return rel.mk(*key)
    rstores = [(seq, b, o, build(sl.norm(v)), kd, a) for seq, b, o, v, kd, a in stores]
    rs = Sym.__new__(Sym)
    rs.cfg, rs.g, rs.alias, rs.mod = s.cfg, rel, s.alias, s.mod
    e = Emit(rs, sl, rstores, glob, name)
    body = e.gen()
    L = ['/* %s: SPU function 0x%s, four groups of the loop per register (kwide.py) */' % (name, cfg['lo']),
         'static KNOINL int %s(u128* g, uint8_t* ls)' % name, '{']
    cnt = int(cfg['count'][1:])
    L.append('    const uint32_t N = W0(GL(%d));' % cnt)
    L.append('    const uint32_t NG = %s;' % cfg['groups_c'])
    bases = sorted(st)
    for b in bases:
        L.append('    const uint32_t B_%s = W0(GL(%s));' % (b, b[1:]))
    for cls in cfg.get('alias', []):
        for b in cls[1:]:
            L.append('    if (W0(GL(%s)) != B_%s) return 0;   /* %s is %s */' % (b[1:], cls[0], b, cls[0]))
    for b in bases:
        al = 15 if st[b] % 16 == 0 else 3
        L.append('    if (B_%s & %d) return 0;' % (b, al))
    # what each base's records are written and read over, all groups: a
    # range written through one base must not meet anything another base reaches
    for _, b, o, _, _, _ in glob:
        if b != 'r1':
            sys.exit('%s: store outside the groups to %s' % (name, b))
    wr = {}; rd = {}
    for _, b, o, v, kd, _ in rstores:
        lo_, hi_ = wr.get(b, (1 << 30, -(1 << 30)))
        wr[b] = (min(lo_, o), max(hi_, o + (4 if kd == 'w' else 16)))
    for n in range(len(rel.nodes)):
        k = rel.nodes[n]
        if k[0] == 'ld':
            lo_, hi_ = rd.get(k[1], (1 << 30, -(1 << 30)))
            rd[k[1]] = (min(lo_, k[2]), max(hi_, k[2] + 16))
    L.append('    const uint32_t SP = W0(GL(1));')
    L.append('    if (NG > 0x3FFFF / %d) return 0;' % max(st.values()))
    ents = []
    for b in bases:
        for kind, m in (('w', wr), ('r', rd)):
            if b in m:
                lo_, hi_ = m[b]
                # reads: whole quads (the SPU loads ignore the low four bits)
                a0 = '((B_%s + %d) & ~15u)' % (b, lo_) if kind == 'r' else '(B_%s + %d)' % (b, lo_)
                a1 = '((B_%s + (NG - 1) * %du + %d + 15) & ~15u)' % (b, st[b], hi_) if kind == 'r' else '(B_%s + (NG - 1) * %du + %d)' % (b, st[b], hi_)
                ents.append((b, kind, a0, a1))
    stk = [o for _, b, o, _, _, _ in glob]
    if stk:
        ents.append(('r1', 'w', '(SP - %d)' % -min(stk), '(SP - %d)' % (-max(stk) - 16)))
    L.append('    if (NG) {')
    L.append('        const struct { int base, w; uint32_t a0, a1; } R_[] = {')
    for b, kind, a0, a1 in ents:
        L.append('            { %s, %d, %s, %s },' % (b[1:], kind == 'w', a0, a1))
    L.append('        };')
    L.append('        for (unsigned i = 0; i < sizeof R_ / sizeof R_[0]; i++)')
    L.append('            if (R_[i].a1 > 0x40000u || R_[i].a0 > R_[i].a1) return 0;    /* no wrap around the LS */')
    L.append('        for (unsigned i = 0; i < sizeof R_ / sizeof R_[0]; i++)')
    L.append('            for (unsigned j = 0; j < sizeof R_ / sizeof R_[0]; j++)')
    L.append('                if (R_[i].w && R_[i].base != R_[j].base && R_[i].a0 < R_[j].a1 && R_[j].a0 < R_[i].a1) return 0;')
    L.append('    }')
    # stores outside the loop (saved registers): their values are entry registers
    for seq, b, o, v, kd, a in glob:
        k = s.g.nodes[v]
        if k[0] != 'reg':
            sys.exit('%s: a store outside the groups of something other than an entry register' % name)
        L.append('    KSTN(SP + (%d), GL(%d));' % (o, k[1]))
    L += e.hoist
    L.append('    for (uint32_t g0 = 0; g0 < NG; g0 += 4) {')
    L.append('        const int nl = NG - g0 < 4 ? (int)(NG - g0) : 4;')
    L += body
    L.append('    }')
    L.append('    return 1;')
    L.append('}')
    out.write('\n'.join(L) + '\n\n')


def main():
    asm, modp, outp, cfgp = sys.argv[1:5]
    cfg = json.load(open(cfgp))
    mod = open(modp, 'rb').read()
    ins = parse(asm, int(cfg['lo'], 16), int(cfg['hi'], 16))
    G_ = cfg.get('groups', 6)
    cnt = int(cfg['count'][1:])
    s = Sym(cfg, ins, mod)
    s.run({cnt: (4 * G_, 0, 0, 0)})
    g = s.g
    stride = cfg['stride']
    # assign stores to groups
    groups = {}
    glob = []
    for st in s.stores:
        seq, base, off, v, kind, a = st
        if base in stride:
            grp, o = divmod(off, stride[base])
            groups.setdefault(grp, []).append(st)
        else:
            glob.append(st)
    print('stores: %d in groups %s, %d outside' % (sum(len(x) for x in groups.values()), sorted(groups), len(glob)), file=sys.stderr)
    for grp in sorted(groups):
        print('  group %d: %s' % (grp, ' '.join('%s+%X%s' % (b, o, '' if k == 'q' else 'w') for _, b, o, _, k, _ in groups[grp])), file=sys.stderr)
    print('  outside: %s' % ' '.join('%s+%X' % (b, o) for _, b, o, _, _, _ in glob[:40]), file=sys.stderr)
    sig = {}
    for grp in sorted(groups):
        sig[grp] = Slice(s, grp).signature(groups[grp])
    ref = sig[sorted(sig)[len(sig) // 2]]
    same = [grp for grp in sig if sig_eq(sig[grp], ref, grp == 0)]
    print('groups with the middle group\'s graph: %s of %s' % (sorted(same), sorted(sig)), file=sys.stderr)
    if len(same) != len(sig):
        sys.exit('the groups differ')
    for r in [0, 1] + list(range(80, 128)):
        if s.g.nodes[s.final[r]] != ('reg', r):
            sys.exit('r%d is not the entry value at the return' % r)
    # the same groups for every bone count that gives that many groups
    for extra in (1, 2, 3):
        s2 = Sym(cfg, ins, mod)
        s2.run({cnt: (4 * G_ - extra, 0, 0, 0)})
        g2 = {}
        for st_ in s2.stores:
            if st_[1] in stride:
                g2.setdefault(st_[2] // stride[st_[1]], []).append(st_)
        if sorted(g2) != sorted(groups) or any(not sig_eq(Slice(s2, k).signature(g2[k]), sig[k], False) for k in g2):
            sys.exit('the stores depend on the bone count beyond the group count')
    with open(outp, 'a') as out:
        emit_kernel(s, cfg, groups, glob, out)
    for grp in sorted(sig):
        if not sig_eq(sig[grp], ref, grp == 0):
            for x, y in zip(sig[grp], ref):
                if x != y:
                    print('group %d differs at store %s+%X:' % (grp, x[0], x[1]), file=sys.stderr)
                    diff(x[3], y[3])
                    break


if __name__ == '__main__':
    main()
