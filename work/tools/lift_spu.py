#!/usr/bin/env python3
"""Lift inFAMOUS 2's SPU code into work/port/spu/, reproducibly.

What runs on the SPUs in this title, and how each piece is lifted:

  * the WWS Job Manager policy module -- the SPURS kernel copies it from EA
    0x887080 (11904 bytes) to LS 0xA00 and finds a lift by the FNV fingerprint
    of those bytes. It is registered as image 2: the runtime's WWS handling
    (job-code overlays at LS 0x4000, code base at LS[0x1320], the stall-notify
    interrupt path) is keyed to that id, and this PM has the same layout.
  * the 27 C0DEC0DE job modules the PM streams into LS 0x4000 -- linked into
    the EBOOT uncompressed; tools/extract_jobmods.py --elf pulls them out and
    tools/lift_jobmods.py lifts them as overlays matched by their 16-byte
    header.
  * the 5 job plugins (`4 ila | 0xDA7ABA5E | entry | size`, right after job
    module cc13c684dd3e in the EBOOT): that job GETs one at a time into its
    own LS 0x6C00 and calls base + entry. Each is lifted at 0x6C00 and
    registered as a code region there, resident while its bytes are.
  * the 30 SPURS job-chain job binaries (`4 ila | 0x44012850`, from EA 0x88A180
    on): the chain stages one at LS 0 and enters its CRT there. Lifted at LS 0.
    The binary size comes from the job descriptor, not the binary, so every
    16-byte size between the last non-zero byte and the next binary is
    registered; the chain fingerprints exactly one of them. Before these were
    lifted the Wwise jobs among them ran on the interpreter, a full core.
  * the 2 SPU ELFs embedded in the EBOOT -- the Bink decoder (thread group
    RADG5) and EDGE's zlib inflate task (SPURS taskset task) -- with
    tools/build_spu_workloads.py, matched by image fingerprint, as images
    ELF_IMAGE_BASE and up (into spu_elf_workloads.c).

Usage: work/tools/lift_spu.py [--toolkit DIR] [--eboot ELF] [--pm-bin FILE]

--pm-bin is the module as the kernel fingerprints it (SPURS_DUMP_MODULES=<dir>
writes it); by default it is cut from the EBOOT, which holds the same bytes.
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.dirname(HERE)
PM_EA, PM_SIZE, PM_LSA = 0x887080, 11904, 0xA00
PM_IMAGE_ID = 2

# Entries no static scan finds, reached through computed branches. Listed by
# SPU_MISS_LOG=1 (runtime/spu/spu_channels.c): every one of them used to leave
# lifted code for the interpreter.
#   0xA2C   the PM's save-registers-and-call service; job modules call it
#           through a pointer they are handed (a stub generated at 0x3FEC0
#           does the saving, and stays interpreted -- it is written at run time)
#   0x3218.. computed `bi $r0` targets inside the PM
PM_EXTRA_FUNCS = [0xA2C, 0x3218, 0x3220, 0x3228]
# Same, for the job modules: LS addresses (all modules run at 0x4000), offered
# to every module that spans them.
JOB_EXTRA_FUNCS = [0x4068, 0x4150, 0x41A0, 0x4388, 0x15280]
# Where job cc13c684dd3e loads its plugins (SPU_GET_RANGE over them: every GET
# lands here).
PLUGIN_LSA = 0x6C00
PLUGIN_MAGIC = b"\xda\x7a\xba\x5e"
PLUGIN_IMAGE_BASE = 240     # after the job modules' ids (checked against the manifest)
JOBBIN_IMAGE_BASE = 250     # after the plugins'
# Every image is lifted with its registers in locals between the statements that
# hand the context to the runtime (spu_lifter.py localize_registers): the hottest
# job plugin loop runs 1.4x faster for it. --merge-chunks makes each run of
# contiguous chunks one C function, with gotos where there were trampolines: the
# geometry job's inner loops then stay in one frame instead of spilling every
# register to the context at each block (see docs/NOTES.md). SPU_LIFT_EXTRA adds
# lifter arguments for an experiment.
LIFT_ARGS = ["--local-regs", "--merge-chunks", "--const-bi"] + os.environ.get("SPU_LIFT_EXTRA", "").split()
# Lifted functions a native replacement may take over (port/ec5f_native.c): the
# entry calls spu_native_entry, and returns as `bi $r0` does when it reports the
# work done. Keyed by the lifted image's directory name.
NATIVE_ENTRIES = {"jobmod_ec5f11015784_e14F90": [0x12C50]}
JOBBIN_MAGIC = b"\x44\x01\x28\x50"
ELF_IMAGE_BASE = 30         # clear of the policy module (2) and the overlays (200+)
JOBMOD_IMAGE_BASE = 200     # tools/lift_jobmods.py


def fnv1a64_runtime(b):
    # runtime/spu/spu_workload.c's basis (not the textbook FNV one; the
    # registry and every generated table use this value).
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def elf_bytes_at(elf, ea, size):
    d = Path(elf).read_bytes()
    phoff = struct.unpack_from(">Q", d, 0x20)[0]
    phnum = struct.unpack_from(">H", d, 0x38)[0]
    for i in range(phnum):
        p_type, _, off, va, _, filesz, _, _ = struct.unpack_from(">IIQQQQQQ", d, phoff + i * 56)
        if p_type == 1 and va <= ea and ea + size <= va + filesz:
            return d[off + ea - va: off + ea - va + size]
    sys.exit(f"EA 0x{ea:X}+0x{size:X} is not file-backed in {elf}")


def find_plugins(elf):
    """(ea, bytes) of every job plugin in the EBOOT's file-backed segments:
    16-byte-aligned, four `ila` words, the magic, then entry offset and size."""
    d = Path(elf).read_bytes()
    phoff = struct.unpack_from(">Q", d, 0x20)[0]
    phnum = struct.unpack_from(">H", d, 0x38)[0]
    segs = []
    for n in range(phnum):
        p_type, _, off, va, _, filesz, _, _ = struct.unpack_from(">IIQQQQQQ", d, phoff + n * 56)
        if p_type == 1 and filesz:
            segs.append((off, va, filesz))
    out = []
    i = d.find(PLUGIN_MAGIC)
    while i >= 0:
        st = i - 0x10
        seg = next((s for s in segs if s[0] <= st < s[0] + s[2]), None)
        if seg and st % 16 == 0 and all(d[st + 4 * k] in (0x42, 0x43) for k in range(4)):
            off, va, filesz = seg
            size = struct.unpack_from(">I", d, st + 0x18)[0]
            if 0x30 < size <= 0x10000 and st + size <= off + filesz:
                out.append((va + st - off, d[st:st + size]))
        i = d.find(PLUGIN_MAGIC, i + 4)
    return out


def find_job_binaries(elf):
    """(ea, bytes over the whole span, smallest plausible size) of every SPURS
    job-chain binary in the EBOOT: 16-byte aligned, four `ila` words, then
    0x44012850. A binary's span runs to the next one (the last one's, to the
    end of its segment or 64 KB)."""
    d = Path(elf).read_bytes()
    phoff = struct.unpack_from(">Q", d, 0x20)[0]
    phnum = struct.unpack_from(">H", d, 0x38)[0]
    segs = []
    for n in range(phnum):
        p_type, _, off, va, _, filesz, _, _ = struct.unpack_from(">IIQQQQQQ", d, phoff + n * 56)
        if p_type == 1 and filesz:
            segs.append((off, va, filesz))
    starts = []
    i = d.find(JOBBIN_MAGIC)
    while i >= 0:
        st = i - 0x10
        seg = next((s for s in segs if s[0] <= st < s[0] + s[2]), None)
        if seg and st % 16 == 0 and all(d[st + 4 * k] in (0x42, 0x43) for k in range(4)):
            starts.append((st, seg))
        i = d.find(JOBBIN_MAGIC, i + 4)
    out = []
    for k, (st, (off, va, filesz)) in enumerate(starts):
        end = min(off + filesz, st + 0x10000)
        if k + 1 < len(starts) and starts[k + 1][0] < end:
            end = starts[k + 1][0]
        blob = d[st:end]
        last = max(j for j in range(len(blob)) if blob[j])
        out.append((va + st - off, blob, (last + 16) // 16 * 16))
    return out


def debase(path):
    """The lifter includes the runtime by a path relative to its own tree;
    the port builds with runtime/spu on the include path instead."""
    p = Path(path)
    t = p.read_text(encoding="utf-8")
    t2 = t.replace('"../../runtime/spu/', '"')
    if t2 != t:
        p.write_text(t2, encoding="utf-8", newline="\n")


def add_native_entries(spu):
    for name, addrs in NATIVE_ENTRIES.items():
        p = Path(spu, name, "spu_recomp.c")
        if not p.exists():
            continue
        src = p.read_text(encoding="utf-8")
        for a in addrs:
            head = f"void {name}_spu_func_{a:08X}(spu_context* ctx) {{\n"
            hook = (f"    {{ extern int spu_native_entry(spu_context*, uint32_t); "
                    f"if (spu_native_entry(ctx, 0x{a:X}u)) SPU_RET(ctx); }}\n")
            if head not in src:
                sys.exit(f"{name}: no function at 0x{a:X} for its native entry")
            if head + hook not in src:
                src = src.replace(head, head + hook, 1)
        p.write_text(src, encoding="utf-8", newline="\n")


def run(cmd, fatal=True):
    print("+", " ".join(cmd), flush=True)
    r = subprocess.run(cmd, check=False)
    if r.returncode and fatal:
        sys.exit(f"failed ({r.returncode}): {cmd[1]}")
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    # The patched ps3recomp checkout: $PS3RECOMP_DIR, else the one
    # scripts/setup.sh makes beside work/, else a checkout in ~/tools.
    toolkit = os.environ.get("PS3RECOMP_DIR") or (
        os.path.join(WORK, "..", "ps3recomp") if os.path.isdir(os.path.join(WORK, "..", "ps3recomp"))
        else os.path.expanduser("~/tools/ps3recomp-infamous"))
    ap.add_argument("--toolkit", default=toolkit)
    ap.add_argument("--eboot", default=os.path.join(WORK, "..", "disc", "PS3_GAME", "USRDIR", "EBOOT.elf"))
    ap.add_argument("--pm-bin", default=None)
    ap.add_argument("--spu-dir", default=os.path.join(WORK, "port", "spu"))
    ap.add_argument("--job-dumps", default=os.path.join(WORK, "runs", "spujob-miss"),
                    help="SPU_DUMP_MISS output: job-chain binaries to lift as well")
    args = ap.parse_args()

    tools = os.path.join(args.toolkit, "tools")
    py = sys.executable
    spu = os.path.abspath(args.spu_dir)
    os.makedirs(spu, exist_ok=True)

    # 1. Policy module.
    pm = Path(args.pm_bin).read_bytes() if args.pm_bin else elf_bytes_at(args.eboot, PM_EA, PM_SIZE)
    if len(pm) != PM_SIZE:
        sys.exit(f"policy module is {len(pm)} bytes, expected {PM_SIZE}")
    fp = fnv1a64_runtime(pm)
    img_dir = os.path.join(spu, "pm_images")
    os.makedirs(img_dir, exist_ok=True)
    pm_bin = os.path.join(img_dir, "wws_pm.bin")
    pm_elf = os.path.join(img_dir, "wws_pm.elf")
    Path(pm_bin).write_bytes(pm)
    run([py, os.path.join(tools, "wrap_spu_elf.py"), pm_bin, "--entry", hex(PM_LSA),
         "--base", hex(PM_LSA), "--out", pm_elf])
    pm_out = os.path.join(spu, "wws_pm")
    run([py, os.path.join(tools, "spu_lifter.py"), "--auto-functions", pm_elf,
         "--base", hex(PM_LSA), "--symbol-prefix", "wws_pm_", "-o", pm_out,
         "--source-name", "spu_recomp.c", "--header-name", "spu_recomp.h",
         "--extra-funcs", ",".join(hex(a) for a in PM_EXTRA_FUNCS)] + LIFT_ARGS)
    for f in ("spu_recomp.c", "spu_recomp.h"):
        debase(os.path.join(pm_out, f))

    # 2. Job modules.
    jobmods = os.path.join(spu, "jobmods")
    run([py, os.path.join(tools, "extract_jobmods.py"), "--elf", args.eboot, "--out", jobmods])
    run([py, os.path.join(tools, "lift_jobmods.py"), "--jobmods", jobmods, "--out", spu,
         "--register", os.path.join(spu, "jobmods_register.c"), "--symbol-prefix", "infamous2",
         "--extra-funcs", ",".join(hex(a) for a in JOB_EXTRA_FUNCS)]
        + [f"--lifter-arg={a}" for a in LIFT_ARGS])
    for name in sorted(os.listdir(spu)):
        if name.startswith("jobmod_"):
            for f in ("spu_recomp.c", "spu_recomp.h"):
                p = os.path.join(spu, name, f)
                if os.path.exists(p):
                    debase(p)

    # 3. Job plugins. Their image ids follow the job modules' (200 + n).
    n_jobmods = len(json.loads(Path(spu, "jobmods_manifest.json").read_text()))
    if JOBMOD_IMAGE_BASE + n_jobmods > PLUGIN_IMAGE_BASE:
        sys.exit(f"{n_jobmods} job modules reach image id {JOBMOD_IMAGE_BASE + n_jobmods - 1}, "
                 f"past PLUGIN_IMAGE_BASE {PLUGIN_IMAGE_BASE}")
    plugins = []
    for n, (ea, blob) in enumerate(find_plugins(args.eboot)):
        name = f"wws_plugin_{ea:08X}"
        entry = PLUGIN_LSA + struct.unpack_from(">I", blob, 0x14)[0]
        b = os.path.join(img_dir, name + ".bin")
        e = os.path.join(img_dir, name + ".elf")
        Path(b).write_bytes(blob)
        run([py, os.path.join(tools, "wrap_spu_elf.py"), b, "--entry", hex(entry),
             "--base", hex(PLUGIN_LSA), "--out", e])
        out = os.path.join(spu, name)
        run([py, os.path.join(tools, "spu_lifter.py"), "--auto-functions", e,
             "--base", hex(PLUGIN_LSA), "--symbol-prefix", name + "_", "-o", out,
             "--source-name", "spu_recomp.c", "--header-name", "spu_recomp.h",
             "--extra-funcs", hex(entry)] + LIFT_ARGS)
        for f in ("spu_recomp.c", "spu_recomp.h"):
            debase(os.path.join(out, f))
        plugins.append((name, ea, len(blob), PLUGIN_IMAGE_BASE + n))
    print(f"{len(plugins)} job plugin(s) lifted at LS 0x{PLUGIN_LSA:X}")
    if PLUGIN_IMAGE_BASE + len(plugins) > JOBBIN_IMAGE_BASE:
        sys.exit("plugin image ids run into the job binaries'")

    # 3b. Job-chain binaries, at LS 0.
    jobbins = []
    for n, (ea, blob, min_size) in enumerate(find_job_binaries(args.eboot)):
        name = f"spurs_job_{ea:08X}"
        b = os.path.join(img_dir, name + ".bin")
        e = os.path.join(img_dir, name + ".elf")
        Path(b).write_bytes(blob)
        run([py, os.path.join(tools, "wrap_spu_elf.py"), b, "--entry", "0x0",
             "--base", "0x0", "--out", e])
        out = os.path.join(spu, name)
        run([py, os.path.join(tools, "spu_lifter.py"), "--auto-functions", e,
             "--base", "0x0", "--symbol-prefix", name + "_", "-o", out,
             "--source-name", "spu_recomp.c", "--header-name", "spu_recomp.h",
             "--extra-funcs", "0x0", "--loop-budget"] + LIFT_ARGS)
        for f in ("spu_recomp.c", "spu_recomp.h"):
            debase(os.path.join(out, f))
        fps = [fnv1a64_runtime(blob[:size]) for size in range(min_size, len(blob) + 1, 16)]
        jobbins.append((name, JOBBIN_IMAGE_BASE + n, fps))
    print(f"{len(jobbins)} job-chain binar(ies) lifted at LS 0")

    # 3c. Job-chain binaries the game loads from its data files. They are not in
    #     the EBOOT; a run with SPU_DUMP_MISS=<dir> writes each one out as
    #     spujob_<fingerprint>_<size>.bin. Registered under exactly the
    #     fingerprint the runtime saw.
    dumped = []
    dump_dir = args.job_dumps
    names = sorted(os.listdir(dump_dir)) if os.path.isdir(dump_dir) else []
    for fn in names:
        m = re.fullmatch(r"spujob_([0-9A-F]{16})_(\d+)\.bin", fn)
        if not m:
            continue
        blob = Path(os.path.join(dump_dir, fn)).read_bytes()
        fp_dump = int(m.group(1), 16)
        if len(blob) != int(m.group(2)) or fnv1a64_runtime(blob) != fp_dump:
            print(f"skipping {fn}: its bytes do not match its name")
            continue
        if any(fp_dump in fps for _, _, fps in jobbins):
            continue
        name = f"spurs_job_dump_{fp_dump:016X}"
        b = os.path.join(img_dir, name + ".bin")
        e = os.path.join(img_dir, name + ".elf")
        Path(b).write_bytes(blob)
        out = os.path.join(spu, name)
        # A dump the lifter cannot take stays on the interpreter, as before.
        if not (run([py, os.path.join(tools, "wrap_spu_elf.py"), b, "--entry", "0x0",
                     "--base", "0x0", "--out", e], fatal=False) and
                run([py, os.path.join(tools, "spu_lifter.py"), "--auto-functions", e,
                     "--base", "0x0", "--symbol-prefix", name + "_", "-o", out,
                     "--source-name", "spu_recomp.c", "--header-name", "spu_recomp.h",
                     "--extra-funcs", "0x0", "--loop-budget"] + LIFT_ARGS, fatal=False)):
            print(f"skipping {fn}: the lift failed")
            shutil.rmtree(out, ignore_errors=True)
            continue
        for f in ("spu_recomp.c", "spu_recomp.h"):
            debase(os.path.join(out, f))
        dumped.append((name, JOBBIN_IMAGE_BASE + len(jobbins) + len(dumped), [fp_dump]))
    if dumped:
        print(f"{len(dumped)} dumped job-chain binar(ies) lifted at LS 0")
    # The build compiles every lifted directory; drop those of dumps gone since.
    keep = {nm for nm, *_ in dumped}
    for d in os.listdir(spu):
        if d.startswith("spurs_job_dump_") and d not in keep:
            shutil.rmtree(os.path.join(spu, d))
    if JOBBIN_IMAGE_BASE + len(jobbins) + len(dumped) > 512:
        sys.exit("image ids run past 511 (runtime tables are 512 wide)")
    jobbins += dumped

    # 4. Registration of the policy module and the plugins (the job modules
    #    register themselves from jobmods_register.c).
    plug_decl = "".join(f"extern void {nm}_spu_recomp_register(void);\n" for nm, *_ in plugins)
    plug_reg = "".join(
        f"    spu_begin_image({img}); {nm}_spu_recomp_register();\n"
        f"    spu_overlay_register_region_at(0x{ea:08X}u, 0x{PLUGIN_LSA:X}u, 0x{size:X}u, {img});\n"
        for nm, ea, size, img in plugins)
    job_decl = "".join(f"extern void {nm}_spu_recomp_register(void);\n"
                       f"extern void {nm}_spu_func_00000000(spu_context*);\n" for nm, *_ in jobbins)
    job_reg = "".join(
        f"    spu_begin_image({img}); {nm}_spu_recomp_register();\n" +
        "".join(f"    spu_workload_register_img(0x{fp:016X}ULL, {nm}_spu_func_00000000, {img}, \"{nm}\");\n"
                for fp in fps)
        for nm, img, fps in jobbins)
    src = f"""/* spu_workloads.c -- GENERATED by work/tools/lift_spu.py.
 * The WWS Job Manager policy module of inFAMOUS 2 (EA 0x{PM_EA:X}, {PM_SIZE} bytes at
 * LS 0x{PM_LSA:X}). The SPURS kernel fingerprints the module it copies into local
 * store and runs this lift when the fingerprint matches. Image {PM_IMAGE_ID}: the
 * runtime's WWS job-manager handling is keyed to it. Also the job plugins and the
 * SPURS job-chain binaries. */
#include "spu_workload.h"

extern void spu_begin_image(int image_id);
extern void spu_overlay_register_region_at(uint32_t content_ea, uint32_t lsa, uint32_t span, int image_id);
extern void wws_pm_spu_recomp_register(void);
extern void wws_pm_spu_func_{PM_LSA:08X}(spu_context*);
{plug_decl}{job_decl}
void port_spu_register_all(void)
{{
    spu_begin_image({PM_IMAGE_ID}); wws_pm_spu_recomp_register();
    spu_workload_register_img(0x{fp:016X}ULL, wws_pm_spu_func_{PM_LSA:08X}, {PM_IMAGE_ID}, "wws_pm");
    /* Job plugins: lifted at LS 0x{PLUGIN_LSA:X}, where job cc13c684dd3e loads them. */
{plug_reg}    /* Job-chain binaries: lifted at LS 0; one fingerprint per size the
     * descriptor might give. */
{job_reg}    spu_begin_image(0);
}}

__attribute__((constructor)) static void port_spu_register_all_ctor(void)
{{
    port_spu_register_all();
}}
"""
    Path(spu, "spu_workloads.c").write_text(src, encoding="utf-8", newline="\n")
    print(f"policy module fp 0x{fp:016X}; wrote {os.path.join(spu, 'spu_workloads.c')}")

    # 5. The SPU ELFs the EBOOT carries.
    elf_dir = os.path.join(spu, "elf_images")
    run([py, os.path.join(tools, "extract_spu_images.py"), args.eboot, "--output", elf_dir])
    run([py, os.path.join(tools, "build_spu_workloads.py"), "--images", elf_dir, "--lifted", spu,
         "--out", os.path.join(spu, "spu_elf_workloads.c"), "--register-fn", "port_spu_elf_register_all",
         "--constructor", "--title", "inFAMOUS 2", "--first-image-id", str(ELF_IMAGE_BASE),
         "--eboot", args.eboot, "--relift"] + [f"--lift-arg={a}" for a in LIFT_ARGS])
    add_native_entries(spu)


if __name__ == "__main__":
    main()
