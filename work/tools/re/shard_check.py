#!/usr/bin/env python3
"""Check a re_trace log against the blast-shard hypotheses.

    shard_check.py work/runs/re/trace-1.log [--csv work/runs/re/shards.csv]

Reads the table dumps re_trace.c writes for

    PS3RECOMP_RE_TABLE="0x9F7C20;0x9F7C24;8|0x9BFED0>0x14>0x44;0x9BFED0>0x14>0x48;32"

(the persisted collected list, {category, id} pairs, and the shard manager's
32-byte entries whose first three words are a float position), recomputes
each shard's id the way the game does (func_000952AC: truncate x, y, z to
int, id = x ^ y*2089 ^ z*17939) and reports how many category-2 entries of
the collected list match a shard of the manager's table. All matching is the
confirmation; a CSV of positions with a collected flag is what a map mod needs.
The CSV holds game data: keep it under work/runs/ (untracked).
"""
import argparse
import math
import re
import struct
import sys

CAT_SHARD = 2


def f32(w):
    return struct.unpack('>f', struct.pack('>I', w))[0]


def trunc32(f):
    # fctiwz: round toward zero, saturate, NaN -> 0x80000000
    if math.isnan(f) or f <= -2 ** 31:
        return 0x80000000
    if f >= 2 ** 31 - 1:
        return 0x7FFFFFFF
    return int(f) & 0xFFFFFFFF


def shard_id(x, y, z):
    return (trunc32(x) ^ (trunc32(y) * 2089) ^ (trunc32(z) * 17939)) & 0xFFFFFFFF


def last_tables(path):
    """The last dump of each table in the log: name -> list of word lists."""
    tables, cur = {}, None
    head = re.compile(r'\[re\] \d+ table (\S+): (\d+) elements at ([0-9A-F]+)')
    row = re.compile(r'\[re\]   \[(\d+)\]((?: [0-9A-F]{8})+)')
    for line in open(path, errors='replace'):
        m = head.match(line)
        if m:
            cur = m.group(1)
            tables[cur] = []
            continue
        # rows carry their index, so lines other threads print in between
        # do not matter; a table ends at the next table header
        m = row.match(line)
        if m and cur is not None:
            tables[cur].append([int(x, 16) for x in m.group(2).split()])
    return tables


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('--csv')
    a = ap.parse_args()
    t = last_tables(a.log)
    coll = next((v for k, v in t.items() if k.upper().startswith('009F7C20')), None)
    shards = next((v for k, v in t.items() if k.upper().startswith('009BFED0>0X14>0X44')), None)
    if shards is None:
        sys.exit('no dump of the shard manager table (009BFED0>0x14>0x44) in the log')
    ids = {}
    rows = []
    for i, w in enumerate(shards):
        x, y, z = f32(w[0]), f32(w[1]), f32(w[2])
        sid = shard_id(x, y, z)
        ids.setdefault(sid, []).append(i)
        rows.append((i, x, y, z, sid))
    print('shard table: %d entries, %d distinct ids' % (len(rows), len(ids)))
    got = set()
    if coll is None:
        print('no dump of the collected list (009F7C20) in the log')
    else:
        cats = {}
        for w in coll:
            cats.setdefault(w[0], []).append(w[1])
        print('collected list: %d entries; by category: %s' % (
            len(coll), ', '.join('%d: %d' % (c, len(v)) for c, v in sorted(cats.items()))))
        mine = cats.get(CAT_SHARD, [])
        got = {i for i in mine if i in ids}
        print('category %d entries matching a shard of the table: %d of %d' % (CAT_SHARD, len(got), len(mine)))
    if a.csv:
        with open(a.csv, 'w') as f:
            f.write('index,x,y,z,id,collected\n')
            for i, x, y, z, sid in rows:
                f.write('%d,%.3f,%.3f,%.3f,0x%08X,%d\n' % (i, x, y, z, sid, sid in got))
        print('wrote', a.csv)


if __name__ == '__main__':
    main()
