# Resolution scale and 4K output

## Problem
The goal is 120+ fps at 4K with FSR. The renderer draws every target at the
guest's size (1280x720) and the present path squeezes the frame to 720p
before the window blit, so a 4K window shows a 720p image stretched. Any
upscaler, spatial or temporal, needs the scene at a higher internal size
first (1440p or 4K), and a present path that keeps it.

The FSR route is still open (the user's call):

- **FSR 4.1:** temporal and ML-based, needs motion vectors. AMD ships it for
  D3D12; on Linux that means the D3D12 backend under Proton/vkd3d-proton.
- **FSR 3.1:** temporal, has a Vulkan path. It also needs motion vectors.
- **FSR 1 (EASU + RCAS):** spatial, Vulkan, no motion vectors. It would work
  today.

inFAMOUS 2 writes no motion vectors. Camera motion could be rebuilt from depth
and the frame's view-projection matrices (found in the vertex constants of the
world pass). Skinned characters, particles and water would ghost without
per-object vectors. So the internal resolution scale is the prerequisite on
every route; the upscaler choice comes after.

## Research (2026-10-16, read-only survey of libs/video and native_passes.c)
Guest pixels == host pixels almost everywhere. Already safe to scale:

- the vertex viewport transform (normalised by the guest size);
- unnormalised texture coordinates (scaled by 1/guest size);
- the shadow PCF (`GetDimensions`);
- clears;
- the lighting tile grid (40x18 tiles divide any size; the pixel-centre
  mapping is computed from w/1280, h/720);
- the SSAO shaders, which scale by w/640.

To change, by risk:

- **Low:**
  - keep the guest and host size per surface and per zeta, keyed by guest
    size (`eng_surface_get`, the depth path, DE:597-784);
  - Vulkan viewport and scissor x N (DE:3800-3823);
  - WPOS x 1/N through the unused `fp_alpha.w`, which also changes the
    SPIR-V cache key;
  - compare guest sizes, not host sizes, in the texture/surface tie-breaker
    (DE:3280-3302) and in the native-pass bind checks (DE:2942, 3225);
  - the depth extent and snapshots in host units;
  - a present that blits the scene surface straight to the swapchain
    instead of through the 1280x720 `s_vk.color`;
  - raise the 4096 size limits (3x = 3840 fits; 4x does not).
- **Medium:**
  - writeback with a downsample, colour and depth (DE:4518-4664). It fails
    silently today on a pitch check;
  - `WB_SMALL` and the 20x11 luminance target the CPU reads for
    auto-exposure. Small targets should stay 1x, like RPCS3's minimum
    scalable size;
  - surface seeding (upload) upscaled;
  - ZPASS counts / N^2;
  - `copy_note` strip and size logic;
  - a scale policy for shadow maps (probably 1x, or their own setting) and
    the depth-bias slope x N.
- **High:**
  - NV3089/NV309E copies are CPU copies in guest memory, and the result is
    sampled as a guest texture: the scene copy to 0xC3746200 (water,
    effects) and the blur/mip pyramid. They either stay 720p, which costs a
    readback a frame and looks soft, or need a GPU surface-to-texture path;
  - effects whose shader constants hold 1/1280 texel offsets (blur taps)
    under-sample at N x;
  - DDX/DDY shrink by N;
  - 1-pixel lines and points;
  - any frame the native passes fall back to the SPU, which reads a
    guest-size G-buffer.

## Proposed phases
1. **Output path.** Window/swapchain at the display size, the scene surface
   blitted (linear) straight to it, and the canvas size separated from the
   output size. Gives a sharp 720p-to-4K blit and the hook for FSR 1.
2. **Scale 1 to 3, colour and depth together.** The low-risk list plus the
   writeback downsample and the small-target rule. Validate on the boat
   cutscene, the pier and the Beast fight: the same image at 1x; at 2x no
   SPU fallback frames, no "differ in size" lines, and auto-exposure the
   same.
3. **NV3089 on the GPU** for the scene copy and the pyramid, so water and
   blur run at the scaled size without a readback.
4. **Upscaler**, per the route chosen: FSR 1 first (no vectors), then a
   temporal one with rebuilt camera vectors if the route allows.

## Done when
- `PS3RECOMP_RES_SCALE=2` at the pier and in the Beast fight: no SPU
  fallback, no writeback misses, and an image visibly sharper than 1x with
  the same exposure.
- The fps cost of 2x and 3x is measured; the CPU-bound frame should barely
  move, the GPU time grows.
