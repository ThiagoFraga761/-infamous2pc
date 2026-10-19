#!/usr/bin/env python3
"""Build a symbol index for the inFAMOUS 2 EBOOT from the binary alone.

The executable ships without symbols and without C++ RTTI, but it still says a
lot about itself:

  * the engine's own type registry: parallel tables of names (an enum name, a
    class name, a data-struct name per entity type) indexed by a type id;
  * vtables: runs of pointers to function descriptors (.opd) in data, stored
    into objects by constructors;
  * strings referenced from code through lis/addi pairs or TOC slots, among
    them "file.cpp:line" assert strings;
  * import stubs, named through their NIDs.

Every function gets the best name the evidence supports, a class when one is
known, keyword tags and the evidence itself. The output holds strings from
the game, so it goes under work/runs/re/ (untracked), never into git.

    gen_symbols.py [--elf EBOOT.elf] [--out work/runs/re]

Outputs: symbols.json (per function) and index.json (strings, xrefs, call
graph, vtables, type tables) that who.py reads.
"""
import argparse
import bisect
import collections
import hashlib
import json
import os
import re
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.dirname(os.path.dirname(HERE))

TAG_WORDS = {
    'shard': r'shard', 'blast': r'blast', 'map': r'(?<![a-z])map|minimap|worldmap|_map|map_',
    'minimap': r'minimap|radar', 'pickup': r'pickup|pick_up', 'collect': r'collect',
    'sense': r'sense|pulse|ping', 'hud': r'(?<![a-z])hud|icon|marker|waypoint|blip',
    'save': r'save|savedata|load_game|checkpoint|profile', 'mission': r'mission',
    'xp': r'(?<![a-z])xp(?![a-z])|upgrade|karma', 'net': r'network|netplay|socket|session|ugc',
}
TAG_RE = {k: re.compile(v, re.IGNORECASE) for k, v in TAG_WORDS.items()}

MASK = 0xFFFFFFFF


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


# --------------------------------------------------------------------------
# ELF image
# --------------------------------------------------------------------------
class Image:
    def __init__(self, path):
        d = open(path, 'rb').read()
        assert d[:4] == b'\x7fELF' and d[4] == 2 and d[5] == 2, 'not a BE ELF64'
        phoff, = struct.unpack_from('>Q', d, 0x20)
        shoff, = struct.unpack_from('>Q', d, 0x28)
        phentsize, phnum, shentsize, shnum = struct.unpack_from('>HHHH', d, 0x36)
        self.segs = []
        for i in range(phnum):
            p_type, flags, off, va, pa, fsz, msz, al = struct.unpack_from('>IIQQQQQQ', d, phoff + i * phentsize)
            if p_type == 1 and msz:
                self.segs.append((va, msz, fsz, off, flags))
        lo = min(s[0] for s in self.segs)
        hi = max(s[0] + s[1] for s in self.segs)
        self.base = lo
        self.mem = bytearray(hi - lo)
        for va, msz, fsz, off, flags in self.segs:
            self.mem[va - lo:va - lo + fsz] = d[off:off + fsz]
        self.end = hi
        self.sections = []   # (addr, size, flags)
        for i in range(shnum):
            name, typ, flags, addr, off, size = struct.unpack_from('>IIQQQQ', d, shoff + i * shentsize)
            if addr and (flags & 2):
                self.sections.append((addr, size, flags, typ))
        self.exec_ranges = [(a, a + s) for a, s, f, t in self.sections if f & 4]
        self.data_ranges = [(a, a + s, bool(f & 1), t) for a, s, f, t in self.sections
                            if not (f & 4) and t == 1]

    def valid(self, ea):
        return self.base <= ea < self.end

    def u32(self, ea):
        o = ea - self.base
        return struct.unpack_from('>I', self.mem, o)[0] if 0 <= o <= len(self.mem) - 4 else None

    def is_code(self, ea):
        return any(a <= ea < b for a, b in self.exec_ranges)

    def is_rodata(self, ea):
        return any(a <= ea < b and not w for a, b, w, t in self.data_ranges)


# --------------------------------------------------------------------------
def nid_names(work):
    path = os.path.join(work, 'out', 'imports_named.json')
    if not os.path.exists(path):
        return {}
    suffix = bytes([0x67, 0x59, 0x65, 0x99, 0x04, 0x25, 0x04, 0x90,
                    0x56, 0x64, 0x27, 0x49, 0x94, 0x89, 0x74, 0x1A])
    out = {}
    for lib, names in json.load(open(path)).items():
        for n in names:
            nid = struct.unpack('<I', hashlib.sha1(n.encode() + suffix).digest()[:4])[0]
            out[(lib, nid)] = n
    return out


def scan_strings(img):
    strs = {}
    pat = re.compile(rb'[\x09\x0a\x0d\x20-\x7e]{3,}\x00')
    for a, b, w, t in img.data_ranges:
        blob = bytes(img.mem[a - img.base:b - img.base])
        for m in pat.finditer(blob):
            s = m.group()[:-1]
            # require at least two letters, drop byte soup
            if sum(c.isalpha() for c in s.decode('latin1')) < 2:
                continue
            strs[a + m.start()] = s.decode('latin1')
    return strs


# --------------------------------------------------------------------------
# Code analysis: one linear pass per function with a small constant tracker.
# --------------------------------------------------------------------------
WRITES_RA_31 = {28, 60, 124, 284, 316, 412, 444, 476, 24, 536, 792, 824, 27, 539, 794,
                826, 827, 26, 58, 954, 922, 986}
# vector and FP indexed loads write VRs/FPRs, not GPRs
NO_GPR_31 = {103, 359, 7, 39, 71, 6, 38, 535, 599, 151, 215, 407, 149, 663, 727, 983, 150, 214, 231, 487, 135, 167, 199,
             662, 918, 0, 32, 467, 144, 146, 178, 1014, 278, 246, 54, 86, 982, 598,
             854, 4, 68, 470, 758, 371}
UPDATE_STORE_31 = {183, 247, 439, 181, 695, 759, 567, 631}


def decode_dst(w):
    """GPRs an instruction writes (approximate, conservative)."""
    op = w >> 26
    rd = (w >> 21) & 31
    ra = (w >> 16) & 31
    if op in (14, 15, 32, 34, 40, 42, 58, 7, 8, 12, 13):
        return (rd,)
    if op in (33, 35, 41, 43):
        return (rd, ra)
    if op in (37, 39, 45, 49, 51, 53, 55):
        return (ra,)
    if op in (20, 21, 23, 24, 25, 26, 27, 28, 29, 30):
        return (ra,)
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo in WRITES_RA_31 or xo in UPDATE_STORE_31:
            return (ra,)
        if xo in NO_GPR_31:
            return ()
        return (rd,)
    return ()


VOLATILE = (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12)


def analyze_one(img, fa, fe, toc, ptr_ok):
    words = struct.unpack_from('>%dI' % ((fe - fa) // 4), img.mem, fa - img.base)
    labels = set()
    ndef = [0] * 32
    for i, w in enumerate(words):
        op = w >> 26
        if op == 16:
            t = (fa + 4 * i + s16(w & 0xFFFC)) & MASK
            if fa <= t < fe:
                labels.add(t)
        elif op == 18 and not (w & 1):
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            t = (fa + 4 * i + li) & MASK
            if fa <= t < fe:
                labels.add(t)
        if op == 18 and (w & 1) or op == 19 and (w & 1):
            for r in VOLATILE:
                ndef[r] += 1
        if op in (32, 58) and ((w >> 16) & 31) == 1 and ((w >> 21) & 31) >= 14:
            continue      # epilogue restore of a callee-saved register
        for r in decode_dst(w):
            ndef[r] += 1
    # At a join point only registers defined once in the whole function keep
    # their value: section anchors loaded from the TOC in the prologue and
    # held in r14-r31 stay valid in every block.
    multi = [r for r in range(32) if ndef[r] > 1 and r != 2]
    val = [None] * 32
    tag = [None] * 32
    val[2] = toc
    calls, vcalls, refs, reads, writes, stores = [], [], [], [], [], []
    icalls = 0
    ret_const = None
    ret_val = None
    ctr_tag = None
    first = [None] * 32
    pend = ()
    for i, w in enumerate(words):
        pc = fa + 4 * i
        for r in pend:
            if ndef[r] == 1 and first[r] is None:
                first[r] = val[r]
        pend = decode_dst(w)
        if pc in labels:
            for r in multi:
                val[r] = None
                tag[r] = None
            for r in range(14, 32):
                if ndef[r] == 1 and first[r] is not None:
                    val[r] = first[r]
        op = w >> 26
        rd = (w >> 21) & 31
        ra = (w >> 16) & 31
        rb = (w >> 11) & 31
        imm = w & 0xFFFF
        if op == 14 or op == 15:          # addi / addis
            sh = s16(imm) << 16 if op == 15 else s16(imm)
            if ra == 0:
                val[rd] = sh & MASK
            elif val[ra] is not None:
                v = (val[ra] + sh) & MASK
                val[rd] = v
                if op == 14 and ra != 1:
                    refs.append((pc, v))
            else:
                val[rd] = None
            tag[rd] = None
            continue
        if op == 24 or op == 25:          # ori / oris
            if val[rd] is not None:
                v = val[rd] | (imm << 16 if op == 25 else imm)
                val[ra] = v
                if op == 24 and imm:
                    refs.append((pc, v))
            else:
                val[ra] = None
            tag[ra] = None
            continue
        if op in (32, 33, 34, 35, 40, 41, 42, 43, 58, 48, 49, 50, 51):
            disp = s16(imm & ~3) if op == 58 else s16(imm)
            base = val[ra] if ra else 0
            ea = (base + disp) & MASK if base is not None else None
            if ea is not None and ra not in (0, 1):
                reads.append((pc, ea))
            if op not in (48, 49, 50, 51):
                nv = None
                ntag = None
                if op == 32 and ea is not None:
                    v = img.u32(ea)
                    # TOC slots, and slots of the constant pools the TOC
                    # anchors, hold pointers that never change
                    if v is not None and (ra == 2 or v in ptr_ok or img.is_rodata(ea)
                                          or (img.valid(v) and not img.is_code(v))):
                        nv = v
                        refs.append((pc, v))
                elif op == 32 and ra not in (1, 2):
                    t = tag[ra]
                    if disp == 0 and t is None:
                        ntag = ('vptr',)
                    elif t == ('vptr',):
                        ntag = ('slot', disp)
                    elif t and t[0] == 'slot' and disp == 0:
                        ntag = ('fn', t[1])
                val[rd] = nv
                tag[rd] = ntag
            if op in (33, 35, 41, 43, 49, 51):
                val[ra] = ea
                tag[ra] = None
            continue
        if op in (36, 37, 38, 39, 44, 45, 62, 52, 53, 54, 55):
            disp = s16(imm & ~3) if op == 62 else s16(imm)
            base = val[ra] if ra else 0
            ea = (base + disp) & MASK if base is not None else None
            if ea is not None and ra not in (0, 1):
                writes.append((pc, ea))
            if op in (36, 37) and val[rd] is not None and ra != 1:
                stores.append((pc, val[rd], disp))
            if op in (37, 39, 45, 53, 55):
                val[ra] = ea
                tag[ra] = None
            continue
        if op == 18:
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            t = (pc + li) & MASK
            if w & 1:
                calls.append((pc, t))
                for r in VOLATILE:
                    val[r] = None
                    tag[r] = None
            elif not (fa <= t < fe):
                calls.append((pc, t))   # tail call
            continue
        if op == 19:
            xo = (w >> 1) & 0x3FF
            if xo == 528 and (w & 1):    # bcctrl
                if ctr_tag and ctr_tag[0] == 'fn':
                    vcalls.append((pc, ctr_tag[1]))
                else:
                    icalls += 1
                for r in VOLATILE:
                    val[r] = None
                    tag[r] = None
            elif xo == 16 and rd == 20 and ret_val is None and val[3] is not None:
                ret_val = val[3]
            if xo == 16 and rd == 20 and i >= 1 and len(words) <= 3:
                pw = words[i - 1]
                if pw >> 26 == 14 and ((pw >> 21) & 31) == 3 and ((pw >> 16) & 31) == 0:
                    ret_const = s16(pw & 0xFFFF)
            continue
        if op == 31:
            xo = (w >> 1) & 0x3FF
            if xo == 467 and (((w >> 16) & 31) | (((w >> 11) & 31) << 5)) == 9:   # mtctr
                ctr_tag = tag[rd]
                continue
            if xo == 444:                         # or / mr
                if rd == rb:
                    val[ra] = val[rd]
                    tag[ra] = tag[rd]
                else:
                    val[ra] = None
                    tag[ra] = None
                continue
        if op == 30:                              # clrldi rA,rS,32 is a move
            sh = ((w >> 11) & 31) | (((w >> 1) & 1) << 5)
            mb = ((w >> 6) & 31) | (((w >> 5) & 1) << 5)
            if (w >> 2) & 7 == 0 and sh == 0 and mb == 32:
                val[ra] = val[rd]
                tag[ra] = tag[rd]
                continue
        for r in decode_dst(w):
            val[r] = None
            tag[r] = None
        if op == 17:
            val[3] = None
    return dict(calls=calls, vcalls=vcalls, icalls=icalls, refs=refs, reads=reads,
                writes=writes, stores=stores, ret_const=ret_const, ret_val=ret_val,
                size=fe - fa)


def analyze(img, funcs, ptr_ok):
    starts = sorted(funcs)
    toc = funcs[starts[0]]['toc']
    res = {}
    t0 = time.time()
    for n, fa in enumerate(starts):
        res[fa] = analyze_one(img, fa, funcs[fa]['end'], toc, ptr_ok)
        if n % 10000 == 0:
            print('  analyzed %d/%d (%.0fs)' % (n, len(starts), time.time() - t0), file=sys.stderr)
    return res


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--elf', default=os.path.join(WORK, 'EBOOT.elf'))
    ap.add_argument('--functions', default=os.path.join(WORK, 'out', 'EBOOT.functions.json'))
    ap.add_argument('--imports', default=os.path.join(WORK, 'out', 'EBOOT.imports.json'))
    ap.add_argument('--lifted', default=os.path.join(WORK, 'src', 'recomp'))
    ap.add_argument('--out', default=os.path.join(WORK, 'runs', 're'))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    img = Image(a.elf)
    flist = json.load(open(a.functions))
    funcs = {}
    for f in flist:
        s = int(f['start'], 16)
        funcs[s] = dict(end=int(f['end'], 16), toc=int(f['toc'], 16), opd=int(f['opd'], 16))
    toc = funcs[min(funcs)]['toc']
    # Analysis units: the loader's functions (reachable through .opd) plus
    # every bl target. The lifter splits further (any address something may
    # jump to becomes a func_<addr>); those extra entries are mapped onto the
    # unit that contains them so their names still line up with the lifted code.
    for ra, rb in img.exec_ranges:
        words = struct.unpack_from('>%dI' % ((rb - ra) // 4), img.mem, ra - img.base)
        for i, w in enumerate(words):
            if w >> 26 == 18 and (w & 3) == 1:
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                t = (ra + 4 * i + li) & MASK
                if t not in funcs and img.is_code(t):
                    funcs[t] = dict(end=0, toc=toc, opd=None)
    lifted = set()
    hdr = os.path.join(a.lifted, 'ppu_recomp.h')
    if os.path.exists(hdr):
        for m in re.finditer(r'^void func_([0-9A-F]{8})\(ppu_context\* ctx\);', open(hdr).read(), re.M):
            v = int(m.group(1), 16)
            if img.is_code(v):
                lifted.add(v)
    # a lifter entry right after a blr/bctr/b is a function of its own (one
    # reached only through a pointer the loader did not list)
    for v in lifted:
        if v not in funcs:
            pw = img.u32(v - 4)
            if pw in (0x4E800020, 0x4E800420) or (pw is not None and pw >> 26 == 18 and not pw & 1):
                funcs[v] = dict(end=0, toc=toc, opd=None)
    code_end = max(b for a_, b in img.exec_ranges)
    starts = sorted(funcs)
    for i, s0 in enumerate(starts):
        nxt = starts[i + 1] if i + 1 < len(starts) else code_end
        e = funcs[s0]['end']
        funcs[s0]['end'] = min(e, nxt) if e > s0 else nxt
    entries = {}
    for v in sorted(lifted - set(funcs)):
        i = bisect.bisect_right(starts, v) - 1
        if i >= 0:
            entries[v] = starts[i]

    # .opd: descriptor address -> code address
    opd = {}
    ld = json.load(open(os.path.join(WORK, 'out', 'EBOOT.loader.json')))
    ob, osz = int(ld['opd_base'], 16), int(ld['opd_size'], 16)
    for ea in range(ob, ob + osz, 8):
        code, t = img.u32(ea), img.u32(ea + 4)
        if code in funcs and t == toc:
            opd[ea] = code
    code_to_opd = {}
    for k, v in opd.items():
        code_to_opd.setdefault(v, k)

    # imports
    nn = nid_names(WORK)
    stubs = {}
    for e in json.load(open(a.imports)):
        st = int(e['stub'], 16)
        nid = int(e['nid'], 16)
        stubs[st] = nn.get((e['library'], nid), '%s_%08X' % (e['library'], nid))

    print('strings...', file=sys.stderr)
    strs = scan_strings(img)
    sstarts = sorted(strs)

    def str_at(ea):
        """(string start, offset) when ea lies inside a known string."""
        if ea in strs:
            return ea, 0
        i = bisect.bisect_right(sstarts, ea) - 1
        if i >= 0:
            s = sstarts[i]
            if s < ea < s + len(strs[s]):
                return s, ea - s
        return None

    # ---- data pointer scan -------------------------------------------------
    print('data pointers...', file=sys.stderr)
    data_ptrs = {}       # data ea -> target
    for lo, hi, w, t in img.data_ranges:
        if ob <= lo < ob + osz:
            continue
        for ea in range(lo, hi - 3, 4):
            v = img.u32(ea)
            if v and img.valid(v) and (v in strs or v in opd or v in funcs):
                data_ptrs[ea] = v

    # runs of consecutive string pointers: name tables
    str_tables = []
    run = []
    for ea in sorted(data_ptrs):
        v = data_ptrs[ea]
        if v in strs and (ea < 0x96b528 or ea >= toc + 0x8000):
            if run and ea == run[-1] + 4:
                run.append(ea)
                continue
            if len(run) >= 4:
                str_tables.append(run)
            run = [ea]
        else:
            if len(run) >= 4:
                str_tables.append(run)
            run = []
    if len(run) >= 4:
        str_tables.append(run)
    # runs of descriptor pointers: vtables and callback tables
    fn_tables = []
    run = []
    for ea in sorted(data_ptrs):
        v = data_ptrs[ea]
        if v in opd:
            if run and ea == run[-1] + 4:
                run.append(ea)
                continue
            if len(run) >= 2:
                fn_tables.append(run)
            run = [ea]
        else:
            if len(run) >= 2:
                fn_tables.append(run)
            run = []
    if len(run) >= 2:
        fn_tables.append(run)

    # ---- code ----------------------------------------------------------------
    print('code (%d functions)...' % len(funcs), file=sys.stderr)
    an = analyze(img, funcs, set(strs) | set(opd))

    callers = collections.defaultdict(set)
    for fa, r in an.items():
        for pc, t in r['calls']:
            if t in funcs and t != fa:
                callers[t].add(fa)

    # string xrefs from code
    str_refs = collections.defaultdict(set)      # string ea -> funcs
    fn_strs = collections.defaultdict(list)      # func -> [(pc, sea, off)]
    data_refs = collections.defaultdict(set)     # data ea (non-string) -> funcs
    fnptr_refs = collections.defaultdict(set)    # code ea (callback) -> funcs taking its address
    for fa, r in an.items():
        seen = set()
        for pc, v in r['refs']:
            if v in seen:
                continue
            seen.add(v)
            hit = str_at(v)
            if hit:
                str_refs[hit[0]].add(fa)
                fn_strs[fa].append((pc, hit[0], hit[1]))
            elif v in opd:
                fnptr_refs[opd[v]].add(fa)
            elif img.valid(v) and not img.is_code(v):
                data_refs[v].add(fa)

    # ---- vtables: tables whose start (or start-k) a function stores into an object
    tbl_by_ea = {}
    for t in fn_tables:
        for k, ea in enumerate(t):
            tbl_by_ea[ea] = (t[0], k)
    vt_stores = collections.defaultdict(set)      # vtable start -> funcs storing it at disp 0
    for fa, r in an.items():
        for pc, v, disp in r['stores']:
            if v in tbl_by_ea and tbl_by_ea[v][1] == 0:
                vt_stores[v].add(fa)
            elif v + 8 in tbl_by_ea and tbl_by_ea[v + 8][1] == 0:
                vt_stores[v + 8].add(fa)
    vtables = {}
    for t in fn_tables:
        s = t[0]
        if s in vt_stores:
            vtables[s] = dict(slots=[opd[data_ptrs[ea]] for ea in t], ctors=sorted(vt_stores[s]))

    # ---- the engine's type registry -----------------------------------------
    print('type registry...', file=sys.stderr)
    slot4 = set()
    for t in fn_tables:
        if len(t) > 4:
            rv = an.get(opd[data_ptrs[t[4]]], {}).get('ret_val')
            if rv:
                slot4.add(rv)
    reg = read_registry(img, strs, opd, slot4)
    infos = reg['infos']          # class-info ea -> dict

    def info_of_slot4(vslots):
        if len(vslots) > 4:
            rv = an.get(vslots[4], {}).get('ret_val')
            if rv in infos:
                return rv
        return None

    # every descriptor run whose slot 4 returns a class info is a vtable too
    for t in fn_tables:
        if t[0] not in vtables and len(t) > 4:
            sl = [opd[data_ptrs[ea]] for ea in t]
            if info_of_slot4(sl):
                vtables[t[0]] = dict(slots=sl, ctors=[])
    for vs, vt in vtables.items():
        ci = info_of_slot4(vt['slots'])
        vt['cls'] = infos[ci]['name'] if ci else None
        vt['info'] = ci
        if ci:
            infos[ci]['vtables'].append(vs)

    # string tables: split the runs where code points into them
    tables = []
    cuts = set(data_refs)
    for t in str_tables:
        cur = []
        for ea in t:
            if ea in cuts and cur:
                tables.append(cur)
                cur = []
            cur.append(ea)
        tables.append(cur)
    tables = [dict(addr=t[0], count=len(t), names=[strs[data_ptrs[ea]] for ea in t],
                   users=sorted(data_refs.get(t[0], ()))) for t in tables if len(t) >= 2]

    # ---- naming -----------------------------------------------------------
    print('naming...', file=sys.stderr)
    sym = {}
    for fa in starts:
        sym[fa] = dict(name=None, conf=0.0, cls=None, tags=set(), evidence=[],
                       size=funcs[fa]['end'] - fa)

    def ev(fa, kind, conf, text):
        if fa in sym:
            sym[fa]['evidence'].append(dict(kind=kind, conf=conf, text=text))

    def give(fa, name, conf, cls=None):
        """Name fa unless a better-supported name is already there."""
        if fa in sym and conf > sym[fa]['conf']:
            sym[fa]['name'] = name
            sym[fa]['conf'] = conf
            if cls:
                sym[fa]['cls'] = cls

    for st, nm in stubs.items():
        ev(st, 'import', 1.0, nm)
        give(st, nm, 1.0)

    # class infos: Create / Destroy / constructor
    alloc_c = collections.Counter()
    for ci, inf in infos.items():
        cr = inf['create']
        if cr in an:
            cs = [t for pc, t in an[cr]['calls'] if t in funcs]
            if cs:
                alloc_c[cs[0]] += 1
    # Create allocates, then constructs: a function that is the first call
    # of many Create functions is an allocator, not a constructor
    allocators = {f for f, c in alloc_c.items() if c >= 5}
    for f in allocators:
        give(f, 'ObjectAlloc_%X' % f, 0.7)
        ev(f, 'registry', 0.7, 'first call of %d Create functions' % alloc_c[f])
    for ci, inf in infos.items():
        cn = inf['name']
        if inf['create'] in an:
            give(inf['create'], '%s::Create' % cn, 0.95, cn)
            ev(inf['create'], 'registry', 0.95, 'class info 0x%X +0x10 (type %d %s)' % (ci, inf['id'], cn))
            cs = [t for pc, t in an[inf['create']]['calls'] if t in funcs and t not in allocators]
            if cs:
                inf['ctor'] = cs[0]
                give(cs[0], '%s::%s' % (cn, cn), 0.85, cn)
                ev(cs[0], 'registry', 0.85, 'called by %s::Create after the allocator' % cn)
        if inf['destroy'] in an:
            give(inf['destroy'], '%s::Destroy' % cn, 0.9, cn)
            ev(inf['destroy'], 'registry', 0.9, 'class info 0x%X +0x14' % ci)

    # vtable slots, named after the base-most class that has them
    depth = {ci: reg_depth(infos, ci) for ci in infos}
    owners = collections.defaultdict(list)
    for vs, vt in vtables.items():
        for k, f in enumerate(vt['slots']):
            owners[f].append((vs, k))
    for f, lst in owners.items():
        named = [(depth[vtables[vs]['info']], vtables[vs]['cls'], k, vs) for vs, k in lst if vtables[vs]['info']]
        if named:
            named.sort()
            d, cn, k, vs = named[0]
            if len({n[2] for n in named}) == 1:
                give(f, '%s::vf%d' % (cn, k), 0.8 if len(named) == 1 else 0.7, cn)
            else:
                give(f, '%s::vf%d' % (cn, k), 0.5, cn)
            ev(f, 'vtable-slot', 0.8, 'slot %d of %s%s' % (
                k, cn, ' (+%d more classes)' % (len(named) - 1) if len(named) > 1 else ''))
        else:
            vs, k = lst[0]
            give(f, 'vt_%08X::vf%d' % (vs, k), 0.3)
            ev(f, 'vtable-slot', 0.3, 'slot %d of unnamed vtable 0x%X' % (k, vs))
    # secondary vtables (an interface inside an object): their slots are
    # `addi r3,r3,-K; b method` thunks into a named class
    thunk = {}
    for fa in starts:
        if funcs[fa]['end'] - fa == 8:
            w0, w1 = img.u32(fa), img.u32(fa + 4)
            if w0 >> 16 == 0x3863 and w1 >> 26 == 18 and not w1 & 1:
                li = w1 & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                thunk[fa] = (-s16(w0 & 0xFFFF), (fa + 4 + li) & MASK)
    for vs, vt in vtables.items():
        if vt['cls']:
            continue
        tc = collections.Counter()
        for f in vt['slots']:
            if f in thunk and sym.get(thunk[f][1], {}).get('cls'):
                tc[(sym[thunk[f][1]]['cls'], thunk[f][0])] += 1
        if tc:
            (cn, adj), nt = tc.most_common(1)[0]
            if nt * 2 >= len(vt['slots']) - 2:
                vt['cls'] = '%s@0x%X' % (cn, adj)
                for k, f in enumerate(vt['slots']):
                    if f in thunk:
                        tgt = thunk[f][1]
                        give(f, '%s::vf%d' % (vt['cls'], k), 0.75, cn)
                        ev(f, 'thunk', 0.75, 'this-=0x%X, then %s' % (thunk[f][0], sym[tgt]['name'] or 'func_%08X' % tgt))
                        ev(tgt, 'thunk-target', 0.7, 'reached through %s slot %d (vtable 0x%X)' % (vt['cls'], k, vs))
    for vs, vt in vtables.items():
        for c in vt['ctors']:
            ev(c, 'vtable-store', 0.6, 'stores vtable 0x%X (%s)' % (vs, vt['cls'] or '?'))
            if vt['cls'] and c in sym and not sym[c]['cls']:
                sym[c]['cls'] = vt['cls']

    # script-visible members: getters, setters, methods
    for m in reg['members']:
        cn = m['owner']
        for key, pre in (('method', ''), ('get', 'get_'), ('set', 'set_'), ('notify', 'on_')):
            fa = m.get(key)
            if fa in an:
                give(fa, '%s::%s%s' % (cn, pre, m['name']), 0.9 if key != 'get' or m['shared'] < 4 else 0.6, cn)
                ev(fa, 'member', 0.9, '%s %s.%s (type %d, kind 0x%X)' % (key, cn, m['name'], m['type'], m['kind']))

    # enum-to-string helpers: tiny functions that index a name table
    for t in tables:
        for u in t['users']:
            if u in an and an[u]['size'] <= 0x60:
                give(u, 'EnumName_%s' % re.sub(r'\W+', '_', t['names'][0])[:32], 0.5)
                ev(u, 'enum-table', 0.5, 'indexes name table 0x%X (%d: %s, ...)' % (
                    t['addr'], t['count'], ', '.join(t['names'][:3])))

    # source-file asserts
    src_re = re.compile(r'^([A-Za-z0-9_]+\.(?:cpp|c|h|hpp)):?(\d+)?')
    for fa, lst in fn_strs.items():
        for pc, sea, off in lst:
            m = src_re.match(strs[sea][off:])
            if m:
                ev(fa, 'source', 0.7, strs[sea][off:][:60])
                sym[fa].setdefault('src', set()).add(m.group(1))

    # string-derived names and tags
    for fa, lst in fn_strs.items():
        for pc, sea, off in lst:
            st = strs[sea][off:]
            for t, rx in TAG_RE.items():
                if rx.search(st):
                    sym[fa]['tags'].add(t)
        best = None
        for pc, sea, off in lst:
            st = strs[sea][off:]
            if src_re.match(st) or len(st) < 5 or not re.search(r'[A-Za-z]{3}', st):
                continue
            key = (len(str_refs[sea]), -len(st))
            if best is None or key < best[0]:
                best = (key, st)
        if best and best[0][0] <= 3:
            nm = re.sub(r'[^A-Za-z0-9]+', '_', best[1]).strip('_')[:40]
            if nm:
                give(fa, 'str_' + nm, 0.2)
                ev(fa, 'string-name', 0.2, 'uses "%s"' % best[1][:60])
    for fa in sym:
        nm = sym[fa]['name'] or ''
        for t, rx in TAG_RE.items():
            if rx.search(nm.replace('::', ' ')):
                sym[fa]['tags'].add(t)

    # imports called
    for fa, r in an.items():
        for pc, t in r['calls']:
            nm = stubs.get(t)
            if nm and re.search(r'SaveData', nm):
                sym[fa]['tags'].add('save')
                ev(fa, 'import-call', 0.8, 'calls %s' % nm)

    # hand-checked names win (work/tools/re/manual_names.json)
    mpath = os.path.join(HERE, 'manual_names.json')
    if os.path.exists(mpath):
        for k, v in json.load(open(mpath)).items():
            if k.startswith('0x') and int(k, 16) in sym:
                give(int(k, 16), v, 0.99)
                ev(int(k, 16), 'manual', 0.99, 'manual_names.json')

    # ---- output -----------------------------------------------------------
    def addr_s(x):
        return '0x%08X' % x

    out_syms = {}
    for fa in starts:
        sm = sym[fa]
        out_syms[addr_s(fa)] = dict(
            name=sm['name'] or 'func_%08X' % fa, conf=round(sm['conf'], 2), cls=sm['cls'],
            tags=sorted(sm['tags']), src=sorted(sm.get('src', ())), size=sm['size'],
            evidence=sm['evidence'][:12],
            vslots=[dict(vt=addr_s(vs), slot=k) for vs, k in owners.get(fa, ())][:8],
        )
    for v, u in entries.items():
        su = out_syms[addr_s(u)]
        out_syms[addr_s(v)] = dict(
            name='%s+0x%X' % (su['name'], v - u), conf=su['conf'], cls=su['cls'], tags=su['tags'],
            src=su['src'], size=0, entry_of=addr_s(u),
            evidence=[dict(kind='entry', conf=1.0, text='lifter entry inside func_%08X' % u)], vslots=[])
    classes = {}
    for ci, inf in infos.items():
        classes[inf['name']] = dict(
            id=inf['id'], enum=inf['enum'], data=inf['data'], info=addr_s(ci),
            parent=infos[inf['parent']]['name'] if inf['parent'] in infos else None,
            size=reg_create_size(img, inf['create']) if inf['create'] else None,
            create=addr_s(inf['create']) if inf['create'] else None,
            destroy=addr_s(inf['destroy']) if inf['destroy'] else None,
            ctor=addr_s(inf['ctor']) if inf.get('ctor') else None,
            vtables=[addr_s(v) for v in inf['vtables']],
            members=[dict(name=m['name'], kind=m['kind'], type=m['type'], off=m.get('off'),
                          **{k: addr_s(m[k]) for k in ('method', 'get', 'set', 'notify') if m.get(k)})
                     for m in reg['by_class'].get(inf['id'], [])])
    json.dump(dict(binary=os.path.basename(a.elf), toc=addr_s(toc), functions=out_syms,
                   classes=classes),
              open(os.path.join(a.out, 'symbols.json'), 'w'), indent=1)

    index = dict(
        toc=toc,
        funcs={addr_s(fa): dict(end=funcs[fa]['end'],
                                calls=sorted({t for pc, t in an[fa]['calls']}),
                                vcalls=sorted({k for pc, k in an[fa]['vcalls']}),
                                icalls=an[fa]['icalls'],
                                strs=[(pc, sea, off) for pc, sea, off in fn_strs.get(fa, ())],
                                reads=sorted({ea for pc, ea in an[fa]['reads'] if not img.is_code(ea)})[:400],
                                writes=sorted({ea for pc, ea in an[fa]['writes'] if not img.is_code(ea)})[:400],
                                ret_const=an[fa]['ret_const'], ret_val=an[fa]['ret_val'])
               for fa in starts},
        strings={str(k): v for k, v in strs.items() if k in str_refs},
        str_refs={str(k): sorted(v) for k, v in str_refs.items()},
        fnptr_refs={str(k): sorted(v) for k, v in fnptr_refs.items()},
        data_refs={str(k): sorted(v) for k, v in data_refs.items()},
        stubs={str(k): v for k, v in stubs.items()},
        opd={str(k): v for k, v in opd.items()},
        vtables={str(k): v for k, v in vtables.items()},
        str_tables=tables,
    )
    for t in tables:
        for k in range(t['count']):
            index['strings'][str(data_ptrs[t['addr'] + 4 * k])] = t['names'][k]
    json.dump(index, open(os.path.join(a.out, 'index.json'), 'w'))
    named = sum(1 for v in out_syms.values() if not v['name'].startswith('func_'))
    print('functions %d, named %d; classes %d, vtables %d (%d with class), members %d, '
          'string tables %d, strings referenced from code %d'
          % (len(out_syms), named, len(classes), len(vtables),
             sum(1 for v in vtables.values() if v['cls']), len(reg['members']), len(tables),
             len(str_refs)), file=sys.stderr)


# --------------------------------------------------------------------------
# The engine's type registry (BCUS98125 01.00). Four tables indexed by type
# id: enum names (BLAST_SHARD), class names (CBlastShard), data-struct names
# (SBlastShardData, one slot behind) and {count, member list} pairs. A class
# info struct {parent info, id, id, ?, Create, Destroy} is what vtable slot 4
# returns. Addresses are checked against the strings before use.
# --------------------------------------------------------------------------
REG_ENUM = 0x9806AC
REG_CLASS = 0x981780
REG_DATA = 0x982040
REG_MEMBERS = 0x92F3FC
REC_SIZE = 0x34


def read_registry(img, strs, opd, extra=()):
    def sp(ea):
        return strs.get(img.u32(ea))
    assert sp(REG_CLASS) == 'CBasic' and sp(REG_ENUM) == 'BASIC', 'registry tables moved'
    n = 0
    while True:
        c = sp(REG_CLASS + 4 * n)
        if not c or not re.match(r'C[A-Z]\w*$', c):
            break
        n += 1
    names = [sp(REG_CLASS + 4 * i) for i in range(n)]
    enums = [sp(REG_ENUM + 4 * i) for i in range(n)]
    datas = [sp(REG_DATA + 4 * i) for i in range(n)]
    datas = [d if d and d.endswith('Data') and i else None for i, d in enumerate(datas)]
    # class infos {parent, id, last id, ?, Create, Destroy}: the ones vtable
    # slot 4 returns (`extra`), the concrete ones found by shape (both
    # descriptors set), and the parents of all of them
    def shape(ea):
        i, hi_id, par = img.u32(ea + 4), img.u32(ea + 8), img.u32(ea)
        # +4/+8: the class id and the last id of its subtree (ids are handed
        # out depth first, so IS_DERIVED_FROM is a range test)
        if i is None or hi_id is None or not (i <= hi_id < n):
            return None
        if not (par == 0 or img.valid(par)) or (i == 0) != (par == 0):
            return None
        c, d = img.u32(ea + 0x10), img.u32(ea + 0x14)
        if not ((c in opd or c == 0) and (d in opd or d == 0)):
            return None
        return dict(id=i, last=hi_id, name=names[i], enum=enums[i], data=datas[i],
                    parent=par, create=opd.get(c), destroy=opd.get(d), vtables=[])
    infos = {}
    for lo, hi, w, t in img.data_ranges:
        if not w:
            continue
        for ea in range(lo, hi - 0x18, 4):
            if img.u32(ea + 0x10) in opd and img.u32(ea + 0x14) in opd:
                sh = shape(ea)
                if sh:
                    infos[ea] = sh
    todo = list(extra) + [v['parent'] for v in infos.values()]
    while todo:
        ea = todo.pop()
        if ea and ea not in infos:
            sh = shape(ea)
            if sh:
                infos[ea] = sh
                todo.append(sh['parent'])
    # one info per id: keep the one other infos or vtables point at
    pointed = collections.Counter([v['parent'] for v in infos.values()] + list(extra))
    best = {}
    for ea, v in infos.items():
        k = v['id']
        if k not in best or pointed[ea] > pointed[best[k]]:
            best[k] = ea
    infos = {ea: infos[ea] for ea in best.values()}
    # members
    lists = {}
    for i in range(n):
        cnt, p = img.u32(REG_MEMBERS + 8 * i), img.u32(REG_MEMBERS + 8 * i + 4)
        if p and cnt and cnt < 4000:
            lists[i] = [img.u32(p + 4 * k) for k in range(cnt)]
    owner = {}
    for i, lst in lists.items():
        for r in lst:
            if r not in owner or len(lst) < len(lists[owner[r]]):
                owner[r] = i
    shared = collections.Counter()
    members = []
    rec_info = {}
    for r, i in owner.items():
        U = lambda d: img.u32(r + d)
        kind = U(8)
        m = dict(rec=r, owner=names[i], owner_id=i, name=strs.get(U(0x2C), '?'), sym=U(0),
                 type=U(4), kind=kind)
        if kind & 1 and U(0xC) in opd:
            m['method'] = opd[U(0xC)]
        if kind & 2:
            m['off'] = U(0x10)
        if kind & 0x10 and U(0x14) in opd:
            m['set'] = opd[U(0x14)]
        if kind & 4 and U(0x1C) in opd:
            m['get'] = opd[U(0x1C)]
        if kind & 0x100 and U(0x24) in opd:
            m['notify'] = opd[U(0x24)]
        for k in ('method', 'get', 'set', 'notify'):
            if k in m:
                shared[m[k]] += 1
        members.append(m)
        rec_info[r] = m
    for m in members:
        m['shared'] = max([shared[m[k]] for k in ('method', 'get', 'set', 'notify') if k in m] or [0])
    by_class = {i: [rec_info[r] for r in lst if r in rec_info] for i, lst in lists.items()}
    return dict(count=n, names=names, enums=enums, infos=infos, members=members,
                by_class=by_class)


def reg_depth(infos, ci):
    d = 0
    seen = set()
    while ci in infos and ci not in seen:
        seen.add(ci)
        ci = infos[ci]['parent']
        d += 1
    return d


def reg_create_size(img, create):
    """Object size: the `li r4, N` passed to the allocator in Create."""
    for ea in range(create, create + 0x40, 4):
        w = img.u32(ea)
        if w is not None and w >> 16 == 0x3880:     # li r4, imm
            return w & 0xFFFF
    return None


if __name__ == '__main__':
    main()
