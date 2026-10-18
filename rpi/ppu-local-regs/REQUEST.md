# PPU lifter: guest registers in host locals (--local-regs)

## Problem

The main PPU thread is ~80% busy at ~68 fps and is becoming the frame-rate
ceiling. Every lifted function keeps every guest register in `ppu_context`
memory. The lifted chunks build with clang and `ppu_context* __restrict ctx`,
but once a function has passed `ctx` to any call, LLVM can no longer prove
that a later guest store (through `vm_base`, a global) does not reach the
context, so it reloads and re-stores ctx fields around every guest memory
access. The hottest main-thread function, func_00314BC8 (~5.5-7% of the
thread), runs a scan loop where every iteration round-trips ctx->cr (0x400),
ctx->xer (0x418) and r28/r29 (0xE0/0xE8) through memory; store-forwarding
latency dominates.

## Goal

An opt-in lifter flag, `--local-regs`, that keeps the GPRs, CR, XER, LR and
CTR in C locals inside each lifted function and syncs them with ctx only
where something else can look at ctx. Hoped-for gain: ~8-10% of the main
thread.

Done means:
- default output (no flag) is byte-identical to today's;
- existing PPU lifter tests still pass;
- a differential test over real hot functions of the game (lifted both ways,
  run on randomized ctx/memory with mocked callees) shows identical traces;
- harness timings show the speedup per function;
- one lifted chunk compiles with the port's flags under the flag.

## Out of scope

FPR/VR localization (optional, later), changing the runtime, re-lifting or
rebuilding the port (the user does that).
