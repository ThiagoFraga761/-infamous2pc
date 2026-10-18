# Research: --local-regs for the PPU lifter

## What the lifter emits (ppu_lifter.py)

`PPULifter._function_def_lines` emits one C line per guest instruction (after
`_fuse_cr` and the stack-accessor rewrite), plus `loc_XXXXXXXX:` labels and a
final fall-through trampoline line. Census over chunks 000-031 of the current
inFAMOUS 2 lift: inside function bodies `ctx` escapes in only these forms:

| form | meaning |
|---|---|
| `func_X(ctx); DRAIN_TRAMPOLINE(ctx);` | direct call (reads and writes ctx) |
| `ps3_indirect_call(ctx)`, `ps3_indirect_call_to(ctx, t)` | indirect call / dispatch |
| `lv2_syscall(ctx)`, `ps3_hle_call(nid, ctx)` | syscall / HLE |
| `{ g_trampoline_fn = ...; return; }`, `return;` | exits (the next fragment reads ctx) |
| `PPU_RESV_ADDR(ctx)`, `ctx->reserve_value` | lwarx/stwcx state: not a register |
| `ppu_mtfsf/mtfsb/mcrfs(ctx, ...)` | touch only fpscr |
| lswx/lswi: `ctx->gpr[r]` with a computed index | rare, handled as a sync |

Everything else is `ctx->gpr[N]`, `ctx->cr`, `ctx->xer` (incl. `&ctx->xer` for
the inline `ppc_sraw/srad` helpers), `ctx->lr`, `ctx->ctr`, plus fpr/vr/vscr/
fpscr which stay in ctx. Control flow is all `goto loc_X`, `if (...) goto`,
conditional/unconditional exits and the jump-table `switch`.

## Who else reads ctx

- Runtime: `g_active_ctx` readers are diagnostics only (guard-write/null-store
  logs, stack dumps, crash handler); `ppu_stwcx32/64` read `reserve_addr`
  (not localized). Nothing writes registers through `g_active_ctx`.
- `sys_ppu_thread_exit` longjmps, but only from inside `lv2_syscall`, a sync
  point. The write-tracking fault handler never touches ctx.
- The port (`work/port/*.c*`) does not use `g_active_ctx`.

So stale registers can be seen only by diagnostics that sample ctx from a
guest-memory slow path in the middle of a function (documented limitation).

## Model: SPU lifter `localize_registers`

Line-based post-pass: pure lines rewritten to locals, everything else is a
"sync" statement bracketed by a write-back and a reload, exits get a
write-back. It writes back every register the function writes. For the PPU
that would cost ~20-30 stores at every call site of call-heavy functions,
which today pay nothing extra, so the PPU pass tracks which locals may be
dirty (forward dataflow over the labels/gotos) and writes back only those.
Reloads load every used register; clang drops the dead ones (a load with no
use is dead code), so no liveness pass is needed for performance.

## Toolchain

The port compiles lifted chunks with clang++ (`work/port/tools/ppu_cxx.sh`),
`-O3 -march=x86-64-v3 -fno-strict-aliasing -fwrapv -ffp-contract=off -w
-DPPU_INLINE_STORES`. Locals whose address is never taken (or only passed
to an always-inline helper) become SSA values: they cannot alias guest
memory.

## Risks

- A missed write (a pattern the dirty analysis does not see) drops a
  write-back -> callee sees a stale register. Mitigation: conservative write
  detection, conservative CFG edges, and a differential test whose mocked
  callees hash the ctx they observe.
- Compile time / memory of the chunks (already 5-6 GB peak). Measure.

## Decision: GO

The emitted forms are few and regular, the observers are known, and the
SPU lifter proves the pattern. Implement behind `--local-regs` for GPR, CR,
XER, LR, CTR.
