#!/usr/bin/env python3
"""Look up a lifted function, a class or a string in the symbol index.

    who.py 0x96054 | func_00096054        one function: names, evidence, callers,
                                          callees, strings, globals it touches
    who.py CBlastShard                    a class: id, parent, size, Create, ctor,
                                          vtables, script members (fields, getters...)
    who.py -s COLLECTED                   functions and members whose name or
                                          strings match (regex, case-insensitive)
    who.py -t shard                       functions carrying a tag
    who.py -d 0x96054                     annotated disassembly (bl targets named,
                                          TOC/pool loads resolved, strings shown)
    who.py --names > names.txt            "addr name" for every named function
    who.py --header > re_names.h          a comment header to read beside the lifted code

Reads work/runs/re/{symbols,index}.json (run gen_symbols.py first). Those files
hold strings from the game: keep them, and anything printed here, out of git.
"""
import argparse
import bisect
import collections
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.dirname(os.path.dirname(HERE))
DEF_DIR = os.path.join(WORK, 'runs', 're')


class DB:
    def __init__(self, d):
        s = json.load(open(os.path.join(d, 'symbols.json')))
        self.ix = json.load(open(os.path.join(d, 'index.json')))
        self.fn = {int(k, 16): v for k, v in s['functions'].items()}
        self.classes = s['classes']
        self.starts = sorted(self.fn)
        self.strings = {int(k): v for k, v in self.ix['strings'].items()}
        self.funcs = {int(k, 16): v for k, v in self.ix['funcs'].items()}
        self.units = sorted(self.funcs)
        self.stubs = {int(k): v for k, v in self.ix['stubs'].items()}
        self.callers = collections.defaultdict(set)
        for f, v in self.funcs.items():
            for t in v['calls']:
                if t != f:
                    self.callers[t].add(f)
        self.by_name = collections.defaultdict(list)
        for a, v in self.fn.items():
            self.by_name[v['name'].lower()].append(a)
        # data addresses we can label: class infos, vtables, name tables
        self.labels = {}
        for cn, c in self.classes.items():
            self.labels[int(c['info'], 16)] = 'classinfo<%s>' % cn
            for vt in c['vtables']:
                self.labels[int(vt, 16)] = 'vtable<%s>' % cn
        for t in self.ix['str_tables']:
            self.labels.setdefault(t['addr'], 'names[%d]{%s,...}' % (t['count'], t['names'][0]))

    def containing(self, ea):
        i = bisect.bisect_right(self.units, ea) - 1
        if i >= 0:
            a = self.units[i]
            if ea < self.funcs[a]['end']:
                return a
        return None

    def name(self, a):
        v = self.fn.get(a)
        return v['name'] if v else 'func_%08X' % a

    def label(self, ea):
        if ea in self.labels:
            return self.labels[ea]
        if ea in self.strings:
            return '"%s"' % self.strings[ea][:50]
        return None


def parse_addr(q):
    m = re.fullmatch(r'(?:func_)?(?:0x)?([0-9A-Fa-f]{4,8})', q)
    if m and (q.startswith(('0x', 'func_')) or re.fullmatch(r'[0-9A-Fa-f]{5,8}', q)):
        return int(m.group(1), 16)
    return None


def show_func(db, a, args):
    f = db.containing(a)
    if f is None:
        print('0x%08X: not inside a known function' % a)
        return
    if f != a:
        print('0x%08X is inside func_%08X (+0x%X)' % (a, f, a - f))
    s = db.fn[f]
    x = db.funcs[f]
    print('func_%08X  %s' % (f, s['name']))
    print('  conf %.2f  class %s  size 0x%X  tags %s%s' % (
        s['conf'], s['cls'] or '-', s['size'], ','.join(s['tags']) or '-',
        '  src ' + ','.join(s['src']) if s['src'] else ''))
    for e in s['evidence']:
        print('  evidence [%s %.1f] %s' % (e['kind'], e['conf'], e['text']))
    if s['vslots']:
        print('  vtable slots: ' + ', '.join('%s#%d' % (db.labels.get(int(v['vt'], 16), v['vt']), v['slot'])
                                            for v in s['vslots']))
    cl = sorted(db.callers.get(f, ()))
    print('  callers (%d):' % len(cl))
    for c in cl[:args.limit]:
        print('    func_%08X  %s' % (c, db.name(c)))
    if len(cl) > args.limit:
        print('    ... %d more' % (len(cl) - args.limit))
    print('  callees (%d):' % len(x['calls']))
    for c in x['calls'][:args.limit]:
        nm = db.stubs.get(c) or db.name(c)
        print('    func_%08X  %s' % (c, nm))
    if x['vcalls']:
        print('  virtual calls through slots: ' + ', '.join('#%d' % (k // 4) for k in x['vcalls']))
    if x['icalls']:
        print('  other indirect calls: %d' % x['icalls'])
    if x['strs']:
        print('  strings:')
        for pc, sea, off in x['strs'][:args.limit]:
            print('    @%08X  "%s"' % (pc, db.strings.get(sea, '?')[off:][:70]))
    for kind in ('reads', 'writes'):
        lab = [(ea, db.label(ea)) for ea in x[kind]]
        named = [(ea, l) for ea, l in lab if l]
        print('  data %s: %d addresses%s' % (kind, len(lab), (', labelled: ' + ', '.join(
            '0x%X=%s' % (ea, l) for ea, l in named[:8])) if named else ''))
        if args.verbose:
            print('    ' + ' '.join('0x%X' % ea for ea, l in lab[:64]))


TOOLKIT = os.environ.get('PS3RECOMP_DIR', '/home/th/tools/ps3recomp-infamous')


def disasm(db, a, args):
    import sys
    sys.path.insert(0, HERE)
    sys.path.insert(0, os.path.join(TOOLKIT, 'tools'))
    import gen_symbols as G
    import ppu_disasm as P
    f = db.containing(a)
    if f is None:
        print('0x%08X: not inside a known function' % a)
        return
    img = G.Image(os.path.join(WORK, 'EBOOT.elf'))
    end = db.funcs[f]['end']
    ptr_ok = set(db.strings) | {int(k) for k in db.ix.get('opd', {})}
    r = G.analyze_one(img, f, end, db.ix['toc'], ptr_ok)
    notes = collections.defaultdict(list)
    for pc, v in r['refs']:
        notes[pc].append(db.label(v) or ('=0x%X' % v))
    for pc, t in r['calls']:
        notes[pc].append(db.stubs.get(t) or db.name(t))
    for pc, k in r['vcalls']:
        notes[pc].append('virtual #%d' % (k // 4))
    for pc, ea in r['reads'] + r['writes']:
        if not any(n.startswith('[') for n in notes[pc]):
            notes[pc].append('[0x%X]' % ea)
    print('func_%08X  %s' % (f, db.name(f)))
    for ea in range(f, end, 4):
        ins = P.decode(img.u32(ea), ea)
        line = '  %08X  %-10s %-24s' % (ea, ins.mnemonic, ins.operands)
        if notes.get(ea):
            line += '  ; ' + ', '.join(notes[ea])
        print(line)


def show_class(db, cn, args):
    c = db.classes[cn]
    chain = []
    p = c['parent']
    while p:
        chain.append(p)
        p = db.classes.get(p, {}).get('parent')
    print('%s  id %d (%s)  data %s' % (cn, c['id'], c['enum'], c['data'] or '-'))
    print('  parents: %s' % (' <- '.join(chain) or '-'))
    kids = sorted(k for k, v in db.classes.items() if v['parent'] == cn)
    if kids:
        print('  children: %s' % ', '.join(kids))
    print('  size %s  info %s  Create %s  ctor %s  Destroy %s' % (
        hex(c['size']) if c['size'] else '-', c['info'], c['create'], c['ctor'], c['destroy']))
    for vt in c['vtables']:
        slots = db.ix['vtables'].get(str(int(vt, 16)), {}).get('slots', [])
        print('  vtable %s: %d slots' % (vt, len(slots)))
        if args.verbose:
            for k, f in enumerate(slots):
                print('    #%-3d func_%08X  %s' % (k, f, db.name(f)))
    own = c['members']
    inherited = set()
    if c['parent'] and c['parent'] in db.classes:
        inherited = {m['name'] for m in db.classes[c['parent']]['members']}
    show = own if args.verbose else [m for m in own if m['name'] not in inherited]
    print('  script members (%d, %d shown%s):' % (len(own), len(show), '' if args.verbose else ', own only; -v for all'))
    for m in show:
        fns = ' '.join('%s=%s' % (k, m[k]) for k in ('method', 'get', 'set', 'notify') if k in m)
        off = ' off=0x%X' % m['off'] if m.get('off') is not None else ''
        print('    %-34s type %-5d kind 0x%-4X%s %s' % (m['name'], m['type'], m['kind'], off, fns))


def search(db, pat, args):
    rx = re.compile(pat, re.I)
    hits = []
    for a, s in db.fn.items():
        if rx.search(s['name']):
            hits.append((a, 'name'))
    for a, x in db.funcs.items():
        for pc, sea, off in x['strs']:
            st = db.strings.get(sea, '')[off:]
            if rx.search(st):
                hits.append((a, 'string "%s"' % st[:50]))
                break
    for cn, c in db.classes.items():
        if rx.search(cn) or rx.search(c['enum'] or ''):
            print('class %s (id %d)' % (cn, c['id']))
        for m in c['members']:
            if rx.search(m['name']) and (not c['parent'] or m['name'] not in
                                        {x['name'] for x in db.classes.get(c['parent'], {}).get('members', [])}):
                print('member %s.%s kind 0x%X%s' % (cn, m['name'], m['kind'],
                      ' off=0x%X' % m['off'] if m.get('off') is not None else ''))
    seen = set()
    for a, why in sorted(hits):
        if a in seen:
            continue
        seen.add(a)
        print('func_%08X  %-50s %s' % (a, db.name(a), why))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('query', nargs='*')
    ap.add_argument('-s', '--search', help='regex over names, strings and members')
    ap.add_argument('-t', '--tag')
    ap.add_argument('--disp', help='functions with a load/store/addi at this displacement '
                    '(field offsets: 0x338 or 0x338,0x33c); filter with -s on the function name')
    ap.add_argument('-x', '--xref', help='functions that read, write or take the address of a data EA')
    ap.add_argument('-d', '--disasm', action='store_true', help='annotated disassembly of the queried functions')
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('-n', '--limit', type=int, default=25)
    ap.add_argument('--dir', default=DEF_DIR)
    ap.add_argument('--names', action='store_true')
    ap.add_argument('--header', action='store_true')
    a = ap.parse_args()
    db = DB(a.dir)
    if a.names or a.header:
        if a.header:
            print('/* Names recovered by work/tools/re/gen_symbols.py. Generated, untracked:')
            print(' * it carries names from the game. */')
        for f in db.starts:
            s = db.fn[f]
            if s['name'].startswith('func_'):
                continue
            if a.header:
                tags = (' [%s]' % ','.join(s['tags'])) if s['tags'] else ''
                print('/* func_%08X  %s  (%.1f)%s */' % (f, s['name'], s['conf'], tags))
            else:
                print('%08X %s' % (f, s['name']))
        return
    if a.disp:
        import struct
        import sys
        sys.path.insert(0, HERE)
        import gen_symbols as G
        img = G.Image(os.path.join(WORK, 'EBOOT.elf'))
        want = {int(x, 16) for x in a.disp.split(',')}
        ops = {32: 'lwz', 34: 'lbz', 40: 'lhz', 42: 'lha', 48: 'lfs', 50: 'lfd', 36: 'stw', 38: 'stb',
               44: 'sth', 52: 'stfs', 54: 'stfd', 14: 'addi', 58: 'ld', 62: 'std'}
        rx = re.compile(a.search, re.I) if a.search else None
        hits = collections.defaultdict(list)
        for lo, hi in img.exec_ranges:
            words = struct.unpack_from('>%dI' % ((hi - lo) // 4), img.mem, lo - img.base)
            for i, w in enumerate(words):
                op = w >> 26
                if op in ops and (w & 0xFFFF) in want and ((w >> 16) & 31) not in (0, 1, 2):
                    if op in (58, 62) and w & 3:
                        continue
                    f = db.containing(lo + 4 * i)
                    if f is not None:
                        hits[f].append('%s@%X' % (ops[op], lo + 4 * i))
        for f in sorted(hits):
            if rx and not rx.search(db.name(f)) and not rx.search(db.fn[f]['cls'] or ''):
                continue
            print('func_%08X  %-50s %s' % (f, db.name(f), ' '.join(hits[f][:6])))
        return
    if a.xref:
        m = re.fullmatch(r'(0x[0-9A-Fa-f]+)(?:\+(0x[0-9A-Fa-f]+))?', a.xref)
        lo = int(m.group(1), 16)
        hi = lo + (int(m.group(2), 16) if m.group(2) else 4)
        for f in db.units:
            x = db.funcs[f]
            r = [e for e in x['reads'] if lo <= e < hi]
            w = [e for e in x['writes'] if lo <= e < hi]
            if r or w:
                print('func_%08X  %-50s %s%s' % (f, db.name(f), 'R ' + ','.join('0x%X' % e for e in r) if r else '',
                                                  ' W ' + ','.join('0x%X' % e for e in w) if w else ''))
        for k, v in db.ix['data_refs'].items():
            if lo <= int(k) < hi:
                for f in v:
                    print('func_%08X  %-50s takes address 0x%X' % (f, db.name(f), int(k)))
        return
    if a.tag:
        for f in db.starts:
            if a.tag in db.fn[f]['tags']:
                print('func_%08X  %s' % (f, db.fn[f]['name']))
        return
    if a.search:
        search(db, a.search, a)
        return
    for q in a.query:
        addr = parse_addr(q)
        if addr is not None:
            (disasm if a.disasm else show_func)(db, addr, a)
        elif q in db.classes:
            show_class(db, q, a)
        elif q.lower() in db.by_name:
            for f in db.by_name[q.lower()]:
                show_func(db, f, a)
        else:
            search(db, re.escape(q), a)
        print()


if __name__ == '__main__':
    main()
