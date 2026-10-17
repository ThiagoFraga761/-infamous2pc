# RSX draw cost: plan and status

## Round 1 (2026-10-16, done)

Output-identical cuts on the RSX thread:

- VS constants by range (`bind_vs_constants_range` in the Vulkan backend).
- Vertex fetch: a consecutive-element walk, per-format loops for SNORM16,
  SINT16 and UINT8, and one shared element range per draw.
- A per-frame texture binding memo.
- The legacy `rsx_state` machine fed only the methods that matter outside it.
- A per-thread cache in front of `spu_lookup` (SPU side).

Results:

| Scene | Before | After |
|---|---|---|
| Under the pier | 27-29 fps | 33-37 fps |
| New Marais | ~74 fps | ~80 fps |

New Marais turned out to wait on SPU throughput (see docs/NOTES.md, "What
limits the frame rate where").

## Where the RSX thread's time goes now (under the pier)

| Share | Work |
|---|---|
| ~30% | Vertex conversion (fetch, remap, index decode, topology) |
| ~15% | memmove, memcmp, memset (ring copies of expanded vertices) |
| ~11% | Texture binding |
| ~11% | FIFO walk and dispatch |
| ~10% | Vulkan recording |

## Candidates for round 2, biggest first

1. **Vertex pulling on the GPU.**
   - What: upload the guest's vertex bytes as they are and convert in the
     vertex shader (big-endian loads by format, stride and offset from a
     UBO).
   - Gain: removes most of the conversion and halves the bytes copied.
   - Cost: the VP translator, pipeline keys and the backend all change.
     Days, not hours.
2. **Persistent GPU copies of cached geometry.**
   - What: a geometry-cache hit binds a resident buffer instead of copying
     the expanded vertices into the ring every draw. New Marais: 99k hits a
     frame, 22 MB of copies.
   - Cost: sub-allocation and lifetime across frames in flight.
3. **Texture state dirty tracking.**
   - What: skip the whole texture pass when no texture register was written
     since the last draw and nothing in `s_tex_gen` moved.
4. **A recording thread for the Vulkan backend.**
   - Gain: ~10%.
   - Cost: every synchronous backend call (readbacks, snapshots, creates)
     needs a fence with it.

Out of this plan: the SPU throughput limit (job code and the SPU runtime's
dispatch), which needs its own request.
