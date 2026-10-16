# Overlap the GPU with the next frame (Vulkan frame pipelining)

## Problem

In the opening 3D cutscene the RSX thread is the bottleneck. `PS3_CLOCK_STATS`
puts it at 5% idle: 64-81% drain (CPU work turning the FIFO into Vulkan
commands) and 15-29% present. `PS3RECOMP_VK_STATS` puts the GPU wait at
190-260 ms/s, 3.3-4.9 ms per flush, ~55 flushes/s. That is one flush a frame,
at present.

At present the backend:

1. flushes the frame's batch;
2. records the blit to the swapchain;
3. submits it;
4. waits on a fence for both, every frame.

So the frame time is the CPU's work plus the GPU's work, end to end, with no
overlap. Right after present, the engine also reads back every colour target of
1024 px or less into guest memory (`PS3RECOMP_ENG_WB_SMALL`). inFAMOUS 2 reads
its 20x11 luminance reduction from there for auto-exposure.

## Goal

Submit a frame and go on recording the next one while the GPU renders it. Wait
only when the CPU needs a GPU result, or before reusing resources the GPU may
still read.

Target: give back most of the 15-29% the RSX thread spends in present.

## Constraints

- Same picture. Compare frames (`PS3RECOMP_ENG_SHOT`) and the brightness
  timeline against the current build.
- Every readback the game can observe keeps its meaning:
  - NV3089 copies to main memory;
  - the depth snapshot;
  - ZPASS reports;
  - the small-target writeback.

  Where one gets a frame of latency (the writeback, ZPASS), say so and check
  the game still reads the right thing: same exposure, same halos.
- One env switch turns it off and restores today's behaviour.
- No new waits in the hot path. The 8 regression tests still pass.

## Measured before

- 3D menu ~84-88 fps.
- Cutscene 38-63 fps over the same 200 s run.
- `PS3RECOMP_VK_STATS`: 230-257 ms/s waiting on the GPU.
