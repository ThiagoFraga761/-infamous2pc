#!/usr/bin/env python3
"""Which volatile registers (r2-r79) do the callers of an SPU function read
after it returns, before writing them? The group-parallel kernels leave the
volatile registers as they found them, so they must be dead at every return.

usage: kabi.py listing.asm ADDR..."""
import re
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
from kgen import parse, uses_defs, reg, imm  # noqa: E402

ins = parse(sys.argv[1], 0, 0x40000)
idx = {a: i for i, (a, _, _) in enumerate(ins)}
for t in sys.argv[2:]:
    T = int(t, 16)
    sites = [i for i, (a, mn, ops) in enumerate(ins) if mn in ('brsl', 'brasl') and imm(ops[1]) == T]
    for i in sites:
        live = set()
        seen = set()
        work = [(i + 1, frozenset())]
        while work:
            j, dead = work.pop()
            dead = set(dead)
            while j < len(ins):
                if (j, frozenset(dead)) in seen:
                    break
                seen.add((j, frozenset(dead)))
                a, mn, ops = ins[j]
                if mn in ('brsl', 'brasl'):
                    # the callee may take arguments in r3..r12
                    live.update(r for r in range(3, 13) if r not in dead)
                    dead.update(range(2, 80))        # what it returns is its own
                    j += 1; continue
                u, d = uses_defs(mn, ops)
                for r in u:
                    if r is not None and 2 <= r <= 79 and r not in dead:
                        live.add(r)
                if d is not None:
                    dead.add(d)
                if mn == 'bi' or mn == 'stop':
                    break
                if mn == 'br':
                    j = idx[imm(ops[0])]; continue
                if mn in ('brz', 'brnz', 'brhz', 'brhnz'):
                    work.append((idx[imm(ops[1])], frozenset(dead)))
                j += 1
        print('%s: call at %05X, volatile registers its callers may read after it: %s' % (t, ins[i][0], sorted(live) or 'none'))
