# Resolution scale: plan

## Approach: the backend scales, the engine stays in guest units
The survey's list of engine-side places that compare sizes (the
texture/surface tie-breaker, the writeback pitch check, `WB_SMALL`, the
`copy_note` strips, the depth extent, the native-pass bind checks) all compare
guest sizes to guest sizes, as long as the engine keeps seeing guest sizes.
So the scale lives in the Vulkan backend:

- a colour or depth target the engine asks for at w x h is created at
  (w N) x (h N); `vk_eng_obj` keeps the logical size in `w`/`h` and the image
  size in new `pw`/`ph`;
- every engine call stays in guest units, and the backend multiplies or
  divides at the edge.

The engine changes in one place only: WPOS. The fragment program reads
`SV_Position`, which is in image pixels. `fp_alpha.w` (unused today) carries
1/N, the decompiler emits `position.xy * fp_alpha.w`, and the engine fills it
from a new `be->res_scale`.

`PS3RECOMP_RES_SCALE=1..3`, default 1. At 1 every path is the one it is today
(N = 1 multiplies are exact).

## Phases
1. **Objects and draws.**
   - `pw`/`ph` on colour and depth targets.
   - The viewport and scissor x N.
   - Clears are whole-image already.
   - Framebuffers and render-pass extents use `pw`/`ph`.
   - Depth-bias slope x N.
   - ZPASS counts / N^2.
2. **Copies.**
   - Colour and depth snapshots copy (w N) x (h N) and stay scaled; they
     are sampled with normalised coordinates.
   - Depth-as-RGBA8 at the scaled size.
   - Colour-target uploads (the seed and `vk_eng_upload` into a render
     target) go through a guest-size staging image, blitted up.
3. **Readbacks.** Colour: blit (linear) the region down into a guest-size
   staging image, then read that. Depth and stencil: read every Nth texel;
   averaging depth would invent values.
4. **Present.** The scene surface goes straight to the swapchain at its full
   size, and the 1280x720 `s_vk.color` only stays for `PS3RECOMP_VK_DUMP`.
5. **Native passes.** SSAO and lighting take their sizes from the image
   (`pw`/`ph`), not the logical size. Check the 640/1280 scalings in the
   shaders.
6. **WPOS** through `fp_alpha.w`. The SPIR-V cache key changes with the
   shader text.

## Gates
- 1x: the boat cutscene, the pier and the Beast fight pixel-identical to
  before (`PS3RECOMP_ENG_SHOT` frames compared), and the 8 boot tests.
- 2x:
  - no "differ in size", writeback-miss or SPU-fallback lines;
  - auto-exposure brightness within 2% of 1x on the same frames;
  - shots visibly sharper;
  - fps measured.
- The particle composite (512x288) and the water copy at 0xC3746200 are
  checked by eye in the shots.

## Not in this round
- NV3089 copies on the GPU. They stay CPU copies of a downsampled readback,
  so water and blur sources are 720p.
- Lines and points x N (needs `wideLines`).
- The upscaler itself.
