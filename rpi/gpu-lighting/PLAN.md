# GPU SSAO + lighting: research findings and plan

## Findings (15/10/2026)

Critical path of a 3D cutscene frame (~26-31 ms), measured with PS3_TIMELINE
(SPU job sampler every 250 us, RSX semaphore waits, SPU label writes, the PPU
state machine at 0xA25A18):

- t+0..6 ms: other SPU jobs, the RSX renders the G-buffer.
- t+4.5..5.5: NV3089 copies normals (0x06FF3B80 -> 0x473F3B80) and depth
  (0x07377B80 -> 0x47777B80) to main memory. Each copy is a GPU readback.
- t+6..13: **SSAO job (P075, jobmod cc13c684dd3e, overlay 219) on all 5 SPUs,
  ~7 ms.** Its 5 kernels are the job plugins at LS 0x6C00, entry 0x6C30. On
  completion it writes label 0x890, which the RSX waits for, ~4.5 ms.
- t+13..22: **lighting job (P071, jobmod 9a537c60d338, overlay 212) on all 5
  SPUs, ~9 ms.** It writes label 0x870, which the RSX waits for, ~4.5 ms.
  PPU state 3->4 at 0xA25A18 also gates the main thread here.
- The PPU main loop polls the state with usleep(10). The producer thread
  polls with usleep(900/100/100/500) from a table at 0x6F2360.

Ceiling experiment: returning at the 5 SSAO kernel entries gives the 3D
cutscene +20-30% and the 3D menu 43 -> 58 fps. Light kernels 0x9048, 0x8A00,
0x8AE8 and 0x8EC0 stubbed on top: small extra gain. The light job still takes
~4 ms in other functions, so the stub point has to move up.

Our addresses (BCUS98125). The ttmx 1.04 table in work/ref, rpcs3/Emu/
infamous_titles.h, maps by offset:

| what | ours | ttmx 1.04 |
|---|---|---|
| normals copy (-> diffuse light) | 0x473F3B80 | 0x37400b80 |
| depth copy (-> specular light) | 0x47777B80 | 0x37784b80 |
| SSAO half-res depth (R32F 640x360) | 0x47AFBB80 (verified; never sampled by the RSX) | 0x37b08b80 |
| AO image (R8 1280x720) | 0xCF800000 | 0xcf800000 |
| lighting params (256 B, light table EA/count at +0x90) | 0x00A25A80 | 0x00a93c80 |
| SSAO parameter block (0x140 B) | 0x00A26780 (matrix at +0xA0 = 0xA26820) | matrix 0x00a94a20 |

## Plan

1. **SSAO on the GPU.** Five fragment passes in the Vulkan backend: downsample,
   occlusion, blur h, blur v, upsample. They run when both G-buffer blits of a
   frame have been recorded, from the source surfaces. Feed the result to the
   engine when it binds 0xCF800000 or the half-res depth.
   - SPU side: stub the 5 kernels through a runtime override of their
     lookup. They are entered by indirect call, so no lifter change is needed.
   - Validate visually against frames with the SPU path, mean error.
2. **Lighting on the GPU.**
   - Tile bounds, then per-tile light mask, then per-pixel point/spot
     lighting with RGBM encode.
   - Params come from 0x00A25A80 plus the light table. Substitute
     0x473F3B80/0x47777B80 when they are sampled after this frame's job.
   - SPU side: find the per-tile function, stub it through a lifter stub hook,
     keep DMA, counters and labels.
3. Drop the two readbacks + NV3089 CPU copies once nothing reads the guest
   copies (SSAO and lighting both native).

Gate between phases: same frames visually (montage vs SPU path), regression
8/8, no new hangs in 3 boots.

## Phase 1 result (15/10/2026)

GPU SSAO works behind `PS3RECOMP_NATIVE_SSAO=1`: 3D menu 42 -> 57 fps, 3D
cutscene ~31 -> ~40 fps on the same build, same picture.

What the spec got wrong, found by comparing with the SPU output
(`PS3RECOMP_NATIVE_SSAO=2`, which runs both and diffs them):

- The depth copy (0x47777B80) is not from a colour target. The blit reads the
  Z24S8 zeta itself at local 0x10000 as A8R8G8B8. The GPU pass reads the
  engine's RGBA8 snapshot of the zeta.
- The job's constants change per frame with the camera FOV. Its parameter
  block at 0x00A26780 (0x140 bytes) holds:
  - +0x00: the 1/z coefficients;
  - +0x10: the sample tables;
  - +0xA0: the matrix;
  - +0xE0: the projection scale, the sphere radius, the falloff and the depth
    clamp;
  - +0x100: the output remap and the blur and upsample weights.

  The fixed values in the reference (S = 5589.43, clamp 1050..2101) matched
  only some shots. Elsewhere they drew wide wrong halos (up to 36% of pixels
  more than 8 levels off). Read from the block, more than 8 levels off is
  0.06% of pixels at most, and the mean error is under 1.4 levels.
- The SPU's scratch after the half-res depth holds the vertically blurred
  half-res result.
- The RSX never samples the half-res depth, only the occlusion texture.

## Phase 2 design (lighting)

Spec: the reference fork's five compute passes (lights, bounds, cull, shade,
clear) and its NOTES.md. Written again here as HLSL compute.

Infrastructure the backend lacks:

- A compute stage in rsx_hlsl_to_spirv (EShLangCompute).
- vkCreateComputePipelines, vkCmdDispatch, vkCmdUpdateBuffer and
  vkCmdFillBuffer in the loader list.
- Storage buffers and RGBA8 storage images as engine textures, sampled through
  surface_view like the SSAO outputs.

Engine:

- Latch the two inputs at their NV3089 copies:
  - copy the normals colour target into a private RGBA8 image;
  - keep the zeta's RGBA8 snapshot as a private copy.

  The game draws over the G-buffer before it samples the lighting.
- Compute on the first bind of 0x473F3B80 or 0x47777B80 after the latch, then
  substitute both.

SPU side:

- Take the parameter block (256 B at 0x00A25A80) and the light table when the
  job loads them, not at bind time: by then the block can already hold the
  next frame. The hook is a DMA GET callback for overlay 212.
- Find the light table's real EA in the job's input list (SPU_OVL_DMA=212).
- Admission per frame: light kinds 1 and 2 only, count <= 256, finite values.
  Otherwise the SPU does that frame.
- Stub the per-tile function and drop its image PUTs. Keep the queue claims,
  counters and label 0x870.

Validation as in phase 1: a mode that computes without substituting and diffs
against the SPU's output in guest memory at bind time.

## Phase 2 result (15/10/2026)

GPU lighting works behind `PS3RECOMP_NATIVE_LIGHTING=1`. With the GPU SSAO on
too, the 3D menu goes 42 -> 65 fps and the 3D cutscene ~31 -> ~45 fps.

- It runs at the depth copy, like the SSAO. The parameters read at that moment
  (block 0x00A25A80, light table at params+0x94) match the job's. Mode 2 shows
  under 1% of pixels more than 1 level off in the cutscene, and those are
  tile-edge cases. No SPU-side parameter capture was needed.
- SPU side: the gate sits on the job's per-tile function, LS 0x4AC8 of image
  212. It is entered indirectly. The 50 functions below it touch no channel
  and do no DMA, so the job's transfers, queue and label stay untouched.
- Both passes fall back to the SPU per frame. The kernels only skip while the
  engine has published a successful GPU run.

Next: drop the two G-buffer readbacks (writeback + CPU copy, ~7 MB a frame)
and the SPU G-buffer DMAs, now that no job reads them while both passes are
live.
