# RSX thread cost per draw

## Problem
The goal is 120+ fps. Where the game draws a lot, one thread caps the frame
rate: the RSX thread, which decodes the FIFO, converts vertices and records
Vulkan for every draw.

- **Under the Empire City pier** ("Blast open the gate", prologue): 23-28 fps.
  - `[clock]` shows the RSX thread draining 97% of the time, and the GPU wait
    is only 15 ms/s.
  - `[drawstats]` counts ~10,500 draws a frame, against ~4,500 at the pier,
    at ~3.3 us each.
  - Time per frame: textures 8.1 ms, backend draw 9.0 ms, fetch+decode 5.6 ms,
    constants+binds 3.7 ms, pipeline 3.0 ms, remap 2.0 ms, plan 1.5 ms,
    refs 0.9 ms.
- **The player's slow zones in New Marais (auto-profile, 16/10 20:59).** The
  RSX thread's time divides as:
  - memmove/memcmp/memset: 18%;
  - vertex fetch and decode: 29%;
  - FIFO and method dispatch: 15%;
  - texture binding: 12%;
  - radv: 5%.

## What is known
- The Vulkan backend has no `bind_vs_constants_range`, so `(ranged 0)`: every
  draw copies the whole 8 KB VS register file. The engine copies it into
  `g.vp_cb`, the backend into `vs_cpu`, and then a memcmp against `vs_last`
  plus a ring copy when it changed. Metal has the range path.
- `sink_bind_textures` walks every surface for each enabled unit of each
  draw. Consecutive draws mostly repeat the same texture registers.
- The geometry cache misses most draws in these scenes: vertices come from
  main memory, rewritten by the SPUs each frame ("untrackable"), so the
  conversion runs every frame.

## Goal
Halve the RSX thread's time per draw, with the same pixels. Each change is
measured on the under-pier scene (`work/runs/drive_tut.sh`), New Marais
(`work/runs/nm_bench.sh`) and the pier (`work/tests/bench_pier.sh`).

## Out of scope here
The main thread waiting on SPU job latency: a separate bottleneck, next.
