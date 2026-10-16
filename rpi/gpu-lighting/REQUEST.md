# GPU lighting for inFAMOUS 2's tiled deferred lighting job

## Problem

At ~30 fps in the opening 3D cutscene the RSX thread spends 35-45% of its time
waiting at the semaphore ACQUIRE on label 0x870 (measured with PS3_CLOCK_STATS +
the per-label `[sema]` report). The SPU lighting job (jobmod
`9a537c60d338_e5400`, overlay 212, writes 0x870 at LS pc 0x5A40) releases it.
Around that job the frame also pays for:

- two NV3089 copies of the G-buffer (normals+material at 0x473F3B80, packed
  depth at 0x47777B80, 3.6 MB each): GPU readback, flush and wait, then a CPU
  copy;
- the SPU pixel work: ~13% of all SPU time;
- re-uploading both lit buffers as textures: decode + upload, 7.2 MB a frame.

## Goal

Compute the lighting on the GPU from the G-buffer images that are already
there. Feed the result to the passes that sample the two buffers, and let the
SPU job keep its protocol (queue claims, counters, labels) without its pixel
work.

Target: remove the 0x870 wait from the RSX critical path. The expected gain is
the largest single one left on the path to 60 fps.

## Constraints

- Same picture as now. Compare against frames rendered with the SPU job, and
  against the PS3 reference video (boat scene, 2:10-2:50).
- Opt-in switch first (env), with fallback to the SPU job whenever admission
  fails: unknown light kind, too many lights, wrong formats or sizes.
- The ttmx/rpcs3-infamous2 fork (GPL-2.0, work/ref/) documents the algorithm in
  reference/lighting/ours.py and NOTES.md. Use it as the specification. Write
  our own shader rather than copying theirs.
- SSAO (another SPU job) also reads the G-buffer copies. Phase 1 keeps the
  readbacks; removing them is phase 2.

## Phases

1. Capture per frame the job's parameter block (256 bytes) and light table (48 B
   records) on the SPU side, at job start.
2. Vulkan compute pass: positions from depth, normal decode, per-tile bounds,
   planes and cone-sphere culling, point/spot lights, RGBM diffuse in A,
   sqrt(specular) in B. Validate offline against a CPU reference on captured
   frames.
3. Texture substitution: when the engine binds 0x473F3B80 / 0x47777B80 as a
   texture after this frame's job, it gets the GPU result.
4. Skip the SPU tile work while keeping the job protocol. Measure.
5. (Later) Drop the G-buffer readbacks once SSAO is on the GPU too.
