#!/usr/bin/env python3
"""Let native code take over lifted PPU code (port/ppu_native.cpp).

A post-pass over the lifter's output, the PPU twin of lift_spu.py's
add_native_entries. Two kinds of patch:

Native entries (NATIVE_ENTRIES). The entry of each listed function gets

    PPU_NATIVE_HOOK(0x005DEF90u);

before anything else in its body -- with --local-regs, before the guest
registers are loaded into locals. The hook asks the runtime once, on the first
call, for the native function registered at that address (ppu_native_slot)
and caches the answer in a function-local static; after that it costs a load
and a branch. A native function that returns nonzero has done the guest
function's work and left the context as the guest's `blr` would (PPC64 ELF
ABI: r1, r2, r13-r31, f14-f31, v20-v31, cr2-cr4 kept, the return value in r3);
the lifted function then returns at once. One that returns 0 declines, and
the lifted body runs as if there were no hook.

Native blocks (NATIVE_BLOCKS). For a hot loop inside a function too big to
replace whole: C put in at one of its labels, working on the --local-regs
locals, that does what a whole run of the loop does and leaves every register
as the loop would. It falls through to the lifted loop when its fast path does
not apply. Each block names the lifted lines it was written against, and the
pass stops if the lift no longer has them.

Each chunk with a patch gets the definitions they use after its preamble. The
pass is idempotent and the lists are authoritative: a patch that left them is
taken out again. Only chunks whose text changes are written, so a rebuild
recompiles just those. A relift drops every patch (the lifter does not know
about them): scripts/lift.sh runs this after the lift.

PS3RECOMP_NATIVE_PPU=0 turns both off at run time (the compare mode, 2, is
for the entries; the blocks just run). --no-blocks leaves the blocks out.

Usage: work/tools/ppu_native_entries.py [--recomp DIR] [--check] [--no-blocks]
  --check   report what would change and exit 1 if anything would
"""
import argparse
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.dirname(HERE)

# Guest functions port/ppu_native.cpp has a native version of. Every address
# here needs a lifted function of that name, and port/ppu_native.cpp needs an
# entry for it (an entry with no hook is never called; a hook with no entry
# costs one cached lookup).
NATIVE_ENTRIES = [
    0x005DEF90,     # memcpy(dst, src, n)
]

# func_00314BC8's inner loop, ~85% of the function and ~5% of the main thread
# in the city: from index r29, the first zero byte of the array whose address
# is at 0xBC(r1), stopping at r28 (signed compare, the first byte read before
# any test):
#
#   loc_00315168: r29 += 1
#   loc_0031516C: if !(r29 < r28) goto loc_003151E8
#   loc_00315174: r10 = [r1+0xBC]; r9 = (u32)(r10 + r29); r0 = byte [r9]
#                 if r0 != 0 goto loc_00315168
#
# As memchr: on a zero it resumes after the branch with r0/r9/r10/r29/cr7 as
# the loop leaves them; past the end it goes to loc_003151E8 with the last
# byte's r0/r9 and cr7 from the last compare.
BLOCK_314BC8 = [
    "        { const uint32_t _nb = vm_sread32(_r1 + 0xBC); const uint32_t _ea = (uint32_t)(_nb + _r29);",
    "          const uint64_t _n = (int32_t)_r29 < (int32_t)_r28 ? (uint64_t)((int64_t)(int32_t)_r28 - (int32_t)_r29) : 1;",
    "          if (ppu_nat_range_ok(_ea, _n)) {",
    "            const uint8_t* _p = (const uint8_t*)memchr(vm_base + _ea, 0, _n);",
    "            _r10 = _nb;",
    "            if (_p) {",
    "              const uint64_t _k = (uint64_t)(_p - (vm_base + _ea));",
    "              _r29 += _k; _r9 = (uint32_t)(_ea + _k); _r0 = 0;",
    "              _cr = (_cr & ~0xFu) | 2u | ((_xer >> 31) & 1u);",
    "              goto ppu_nat_resume_00315174;",
    "            }",
    "            _r29 += _n; _r9 = (uint32_t)(_ea + _n - 1); _r0 = vm_base[_ea + _n - 1];",
    ("            { int64_t a = (int32_t)_r29; int64_t b = (int32_t)_r28; "
     "_cr = (_cr & ~0xFu) | (((a < b) ? 8 : (a > b) ? 4 : 2) | ((_xer >> 31) & 1u)); }"),
    "            goto loc_003151E8;",
    "          } }",
]
NATIVE_BLOCKS = [
    {"func": 0x00314BC8, "label": "loc_00315174", "code": BLOCK_314BC8,
     "before": [
         "loc_00315168:",
         "        _r29 = _r29 + (int64_t)(1);",
         "loc_0031516C:",
         "        { int64_t a = (int32_t)_r29; int64_t b = (int32_t)_r28; uint32_t cr_val = ((a < b) ? 8 : (a > b) ? 4 : 2) | ((_xer >> 31) & 1u); _cr = (_cr & ~(0xFu << 0)) | (cr_val << 0); }",
         "        if ((!((_cr >> 3) & 1))) goto loc_003151E8;",
     ],
     "expect": [
         "        _r10 = vm_sread32(_r1 + 0xBC);",
         "        _r9 = _r10 + _r29;",
         "        _r9 = ppc_rldicl(_r9, 0, 32);",
         "        _r0 = vm_read8(_r9 + 0x0);",
         "        { int64_t a = (int32_t)_r0; int64_t b = (int64_t)0; uint32_t cr_val = ((a < b) ? 8 : (a > b) ? 4 : 2) | ((_xer >> 31) & 1u); _cr = (_cr & ~(0xFu << 0)) | (cr_val << 0); }",
         "        if ((!((_cr >> 1) & 1))) goto loc_00315168;",
     ]},
]

MARK = "/* ppu-native: tools/ppu_native_entries.py */"
MARK_END = "/* ppu-native end */"
PREAMBLE_HOOK = [
    'extern "C" int (*ppu_native_slot(uint32_t addr))(ppu_context*);',
    ("#define PPU_NATIVE_HOOK(a) do { static int (*const s_nat)(ppu_context*) = ppu_native_slot(a); "
     "if (s_nat && s_nat(ctx)) return; } while (0)"),
]
# Blocks on (port/ppu_native.cpp, PS3RECOMP_NATIVE_PPU not 0) and the range
# one the inline accessors would take access by access (ppu_recomp.h vm_inl_ok).
PREAMBLE_BLOCK = [
    'extern "C" int g_ppu_native_blocks;',
    "static inline int ppu_nat_range_ok(uint32_t ea, uint64_t n)",
    "{",
    "    const uint64_t ctl = ppu_hle_inject_base + 0x2008u;",
    "    return __builtin_expect(g_ppu_native_blocks && g_vm_inl_lim != 0, 1) && ea - 0x10000u < g_vm_inl_lim &&",
    "           ea + n <= 0x10000ull + g_vm_inl_lim && (ctl + 4 <= ea || ctl >= ea + n);",
    "}",
]
BLOCK_TAG = "  /* ppu-native block */"
HDR = re.compile(r"^void (?:__attribute__\(\(weak\)\) )?func_([0-9A-F]{8})\(ppu_context\* __restrict ctx\) \{$")
HOOK = re.compile(r"^    PPU_NATIVE_HOOK\(0x([0-9A-F]{8})u\);$")
RESUME = re.compile(r"^ppu_nat_resume_[0-9A-F]{8}:$")


def strip(lines):
    if MARK in lines:
        k = lines.index(MARK)
        e = lines.index(MARK_END, k)
        del lines[k:e + 2]
    return [l for l in lines if not HOOK.match(l) and not l.endswith(BLOCK_TAG) and not RESUME.match(l)]


def apply_blocks(lines, i, blocks):
    """Patch the function whose header is lines[i]; returns the end index."""
    j = i + 1
    while lines[j] != "}":
        j += 1
    for b in blocks:
        try:
            at = lines.index(b["label"] + ":", i, j)
        except ValueError:
            sys.exit(f"func_{b['func']:08X}: no {b['label']} for its native block")
        n, m = len(b["expect"]), len(b["before"])
        if lines[at + 1:at + 1 + n] != b["expect"] or lines[at - m:at] != b["before"]:
            sys.exit(f"func_{b['func']:08X} {b['label']}: the lifted loop is not the one its native block was "
                     "written for (tools/ppu_native_entries.py NATIVE_BLOCKS)")
        lines[at + 1 + n:at + 1 + n] = [f"ppu_nat_resume_{b['label'][4:]}:"]
        lines[at + 1:at + 1] = [c + BLOCK_TAG for c in b["code"]]
        j += 1 + len(b["code"])
    return j


def patch_chunk(text, want, blocks):
    lines = strip(text.split("\n"))
    found, nblk = set(), 0
    i = 0
    while i < len(lines):
        m = HDR.match(lines[i])
        if not m:
            i += 1
            continue
        a = int(m.group(1), 16)
        if a in want:
            found.add(a)
            lines.insert(i + 1, f"    PPU_NATIVE_HOOK(0x{a:08X}u);")
        mine = [b for b in blocks if b["func"] == a]
        if mine:
            found.add(("block", a))
            nblk += len(mine)
        i = apply_blocks(lines, i, mine) + 1 if mine else i + 1
    if found:
        pre = [MARK] + PREAMBLE_HOOK + (PREAMBLE_BLOCK if nblk else []) + [MARK_END, ""]
        first = next(i for i, l in enumerate(lines) if HDR.match(l))
        while first > 0 and lines[first - 1].startswith("/*"):
            first -= 1
        lines[first:first] = pre
    return "\n".join(lines), found


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--recomp", default=os.path.join(WORK, "src", "recomp"))
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--no-blocks", action="store_true")
    a = ap.parse_args()
    want = set(NATIVE_ENTRIES)
    blocks = [] if a.no_blocks else NATIVE_BLOCKS
    chunks = sorted(glob.glob(os.path.join(a.recomp, "ppu_recomp_*.cpp")))
    if not chunks:
        sys.exit(f"no lifted chunks in {a.recomp}")
    found_all, changed = set(), []
    for ch in chunks:
        with open(ch, encoding="utf-8") as f:
            old = f.read()
        new, found = patch_chunk(old, want, blocks)
        found_all |= found
        if new != old:
            changed.append(ch)
            if not a.check:
                with open(ch, "w", encoding="utf-8", newline="\n") as f:
                    f.write(new)
    missing = (want | {("block", b["func"]) for b in blocks}) - found_all
    if missing:
        sys.exit("no lifted function for " + ", ".join(
            f"0x{x[1]:08X}'s native block" if isinstance(x, tuple) else f"native entry 0x{x:08X}"
            for x in sorted(missing, key=lambda x: x[1] if isinstance(x, tuple) else x)))
    verb = "would change" if a.check else "changed"
    print(f"ppu native: {len(want)} entr{'y' if len(want) == 1 else 'ies'}, {len(blocks)} block(s); "
          f"{verb} {len(changed)} chunk(s)" + "".join(f"\n  {os.path.basename(c)}" for c in changed))
    return 1 if a.check and changed else 0


if __name__ == "__main__":
    sys.exit(main())
