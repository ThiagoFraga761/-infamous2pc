# Vulkan engine batching

## Problem
The draw-engine half of `libs/video/rsx_vulkan_backend.c` records, submits and
waits on a fence for every operation: each draw, clear, texture upload, target
creation. In the inFAMOUS 2 boat cutscene (~4,500 draws/frame) that costs
~205 ms of a ~240 ms frame ("backend draw" in PS3RECOMP_ENG_DRAWSTATS), so the
game renders at 3-12 fps. Goal: one command buffer per frame-ish, waiting only
where the CPU has to see GPU results.

## Research (2026-10-14)
- Every engine op calls `vk_begin()` (reset + begin) and `vk_submit_and_wait()`.
- Per-draw state that is overwritten in place and would race with pending
  work: vertex buffer `s_vk.vertices`, index buffer `s_e2.indices`, the UBO
  `s_g.ubo` (VS constants at 0, PS at ps_off, ~7.5 KB VS per draw) and the one
  descriptor set `s_g.set` (shared with the legacy guest-program path).
- Staging buffers for uploads/readbacks are created and destroyed per op.
- Object release (`vk_eng_release`, pipeline release, framebuffer cache drop,
  scratch depth regrow) destroys immediately.
- Draws only barrier their render targets: a texture uploaded by a copy in the
  same command buffer would need a barrier before the fragment shader reads it.

## Plan
1. `vk_op_begin` / `vk_op_end` / `vk_flush` for engine ops: begin continues an
   open command buffer, end records nothing more, flush = end + submit + wait +
   deferred frees + ring reset. Legacy `vk_begin` flushes a pending batch first.
2. Flush points: readbacks (colour, depth, stencil, depth-as-RGBA8 snapshot),
   present, engine `submit_and_wait`, every destroy of an object the batch may
   reference, ring or descriptor-pool exhaustion.
3. Per-draw memory from rings reset at flush: vertices+indices, uniforms (VS
   block deduplicated against the previous draw), upload staging.
4. Engine-own descriptor set layout with UNIFORM_BUFFER_DYNAMIC for bindings
   0/1 and its own pipeline layout; one set per distinct texture/sampler
   binding, reused while unchanged, from a pool reset at flush.
5. A global memory barrier before each draw and copy (replaces the ordering
   the per-op fence wait used to give).
6. `PS3RECOMP_VK_BATCH=0` keeps the old synchronous behaviour for A/B.

## Done when
- Same pixels as the synchronous path on the boat cutscene and the Beast
  fight (frame dumps compared), no validation errors, regression suite green.
- PS3RECOMP_ENG_DRAWSTATS "backend draw" drops by an order of magnitude.
