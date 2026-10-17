# Notes

What was learned while porting inFAMOUS 2 (BCUS98125 01.00). Addresses are
effective addresses in that executable's memory map unless marked LS (SPU local
store).

## How the port is put together

- **PPU.** `ppu_loader.py` finds the functions and imports. `ppu_lifter.py`
  translates the ~39,000 functions (346 jump tables) to C in `work/src/recomp`.
  The port links that with the ps3recomp runtime. System modules are HLE.
- **SPU.** Every SPU image the game runs is lifted to C (`work/tools/lift_spu.py`):
  - the WWS job manager's policy module (EA 0x887080, at LS 0xA00);
  - the 27 job modules (`C0DEC0DE`, at LS 0x4000);
  - the 5 plugins of the ambient-occlusion job (at LS 0x6C00);
  - the 30 SPURS job-chain binaries (Wwise among them);
  - job-chain binaries the game loads from its data files instead of the
    EBOOT. A run with `SPU_DUMP_MISS=<dir>` (`work/play.sh` sets it to
    `work/runs/spujob-miss/`) writes each one out, and `lift_spu.py` lifts
    what it finds there;
  - the Bink decoder and EDGE zlib.

  The lifted code keeps SPU registers in vector locals (`--local-regs`), and
  the SPU translation units are built with clang (`work/port/tools/spu_cc.sh`).
- **RSX.** ps3recomp's draw engine with the Vulkan backend runs the game's own
  vertex and fragment programs, decompiled to HLSL and compiled to SPIR-V with
  glslang.

## The frame's critical path

A 3D frame of the opening cutscene used to spend about 7 ms in the SPU
ambient-occlusion job and 9 ms in the SPU lighting job, one after the other:

- t+4.5..5.5 ms: NV3089 copies the depth (the Z24S8 depth buffer itself, read
  as A8R8G8B8, to 0x47777B80) and then the normals (a colour target, to
  0x473F3B80) into main memory. Each is copied as two strips, 1024 and 256
  pixels wide; the second strip's source and destination are 0x1000 further
  on.
- SSAO job P075 (job module cc13c684dd3e) reads them and writes label 0x890.
- Lighting job P071 (job module 9a537c60d338) lights them in place and writes
  label 0x870.
- The RSX waits on those labels before the composite. Its wait on 0x870 is
  for the previous frame's value, so the composite can sample the lighting
  while the job still writes it.

Both jobs now have GPU replacements. Measured on the same build:

| | 3D menu | 3D cutscene |
|---|---|---|
| SPU jobs | 42 fps | ~31 fps |
| GPU SSAO | 57 fps | ~40 fps |
| GPU SSAO + lighting | ~64 fps | ~45 fps |
| G-buffer copies left out of guest memory | ~79 fps | ~52 fps |
| SPU runtime: overlay index, idle DMA, wider copies | ~88 fps | ~56 fps |

### After the GPU passes: the threads themselves

With both jobs on the GPU, no single wait dominates. `PS3_CLOCK_STATS` showed
the RSX thread 5% idle in the cutscene (65-80% turning the FIFO into Vulkan
commands, 15-29% at present waiting on the GPU). Stack samples of every thread
(`eu-stack` 24 times over 10 s) showed:

- the main PPU thread running 80% of the time;
- the SPU threads idle about half of it.

These changes followed:

- **The GPU starts on a frame while it is being recorded.** Every
  `PS3RECOMP_VK_SPLIT` operations (512) the batch so far is submitted without
  a fence. A flush waits on the last submission's fence, which covers the
  earlier ones. The wait at present went from 4.7 to 1.8 ms.
- **The window blit goes out with the frame**, in the same `vkQueueSubmit`,
  instead of a submit-and-wait of its own. Engine mode also stopped reading
  every presented frame back (only a dump needs it).
- **Short `usleep`s are exact.** The main thread polls flags with
  `usleep(1..10)` about 4,500 times a second (`PS3_POLLTOP=10` names the
  sites). Linux held each one to the 50 us timer slack, ~60 us a turn. Sleeps
  of up to 20 us now spin (yielding) to the deadline, and the slack is 1 us.
- **The store path's SPU-reservation check is inline.** As a call it was 7%
  of the main thread.

Together they added about 8% to the frames rendered over the cutscene. The
host is shared (a browser and Steam ran during the measurements), so single
runs differ by about ±5%.

### The main PPU thread and the compiler

With the geometry cache in, walking on the pier ran at 98 fps and the main
PPU thread was the limit: 96% busy in a `perf` profile, against 32% for the
RSX thread and 72% for each SPU thread. Its time went 57% to game code, 25% to
the guest memory accessors and 7% to the VMX float helpers. The accessors are
`static inline`, but GCC stops inlining once a translation unit has grown 40%,
and every lifted unit does.

- **Accessors inlined.** `ppu_recomp.h` marks them `always_inline`, and their
  guard is one compare against `g_vm_inl_lim` (0 while a memory diagnostic is
  armed, otherwise the mapped range less 16 bytes) plus the gcm control word.
  The four SIMD float ops in `ppu_vmx.h` are forced inline too.
- **Lifted PPU code compiled by clang++** (`work/port/tools/ppu_cxx.sh`).
  Every lifted function takes its context pointer `__restrict`, so a guest
  load or store through `vm_base` cannot alias a guest register. On x86 the
  inline store and its reservation re-check are volatile accesses instead of
  being separated by a compiler barrier. The barrier made every guest
  register reload from the context after every store. clang then keeps
  registers in host registers across guest memory accesses and across the
  stores' cold out-of-line calls, as long as the context has not been passed
  to a call yet. LLVM gives up on that question past 32 basic blocks, so the
  launcher raises the limit to 1000. GCC does not use `__restrict` across
  calls at all.
- **`reserve_addr` is volatile.** Another thread breaks a reservation by
  writing it, and `ppu_stwcx32` reads it through `g_active_ctx`. With a
  restrict-qualified context, the store `lwarx` makes could otherwise be
  dropped as dead.

clang also builds a lifted unit in about 70 s and 2 GB, where GCC takes about
4 minutes and 5 GB, so a full build at `-j 4` takes about 8 minutes. With
these changes the pier runs at about 119 fps and the tutorial at 122. The
main thread is still about 95% busy.

A second round, measured on the idle 3D main menu (the steadiest scene the
title has; the main PPU thread is ~99% busy there), three boots of each build
alternating: 109.9 against 105.6 fps, ahead in every pair.

- **Stack accesses skip the checks.** The lifter sends a load or store whose
  address comes from r1 (prologues, epilogues, spills: ~40% of the guest's
  memory operations) to accessors that test nothing but the diagnostic switch;
  stores keep the SPU-reservation check. `PPU_LIFT_STACK=0` lifts without.
- **Indirect calls.** With no call diagnostic armed, a call to a lifted
  function goes straight from the hash lookup to the call; the full dispatcher
  read a dozen variables per call (~3% of the thread).
- **The profiler breadcrumb** every syscall and HLE call leaves is the host
  return address; it is turned into a guest function (a binary search) only
  when something reads it.
- **Short usleeps spin with PAUSE**, not a `sched_yield` syscall per turn.
- **More VMX float ops in SIMD:** vmaxfp, vminfp, vrefp, vrsqrtefp, vcfsx,
  vctsxs, and the int/float conversions scale by a power of two built from
  its bits instead of calling `ldexpf`. `runtime/ppu/tests/test_vmx_simd.c`
  checks them against the scalar loops bit for bit (and the conversions
  against the old `ldexpf` spelling).

Then (not yet measured on its own): vperm as two byte shuffles and a blend,
vsldoi as one `palignr` (they were a 16-step byte loop and a round trip
through a 32-byte stack buffer; ~10,000 and ~28,000 sites in the lift), the
float compares and the four roundings in SIMD, and lvebx/lvehx/lvewx as the
quadword load the Cell makes. That last one also helps the vspltw that
usually follows: it read 16 bytes over a narrow store at a variable offset,
which the CPU cannot forward. About half of the main thread's guest time is
in functions that use VMX.

Logging cost more than expected. `work/play.sh` pipes the game through `tee`,
and with stderr not a terminal the runtime logs every SPU job and every wait:
thousands of lines a second, all through one lock. That gave 84 fps at the
pier against 98 without it. `play.sh` and the benchmark now set
`PS3_VERBOSE=0`.

### The main thread waiting on the SPUs

After the compiler work, a profile of the pier walk put a quarter of the main
thread's samples in `sys_timer_usleep`. `PS3_POLLTOP=10` names the loop:
`func_00371000` calls `func_00504D90`, which takes the minimum of seven
per-SPU progress counters, with `usleep(1)` between tries, about 220,000
times a second. The main thread waits for SPU work for about a fifth of the
frame, while each SPU thread is busy about 80% of the time. Two things in the
SPU runtime made that wait longer than the work itself:

- **Idle SPUs slept through new work.** An idle SPURS kernel sleeps until
  the PPU library wakes it, or for 1 ms. inFAMOUS 2 also hands work over
  without a library call: readyCount is changed by its own atomics and by
  the WWS policy module's PUTLLC. `SPURS_IDLE_STATS=1` counted about 100
  sleeps a second that ran to the timeout and then found work. An idle SPU
  now polls for 200 us before it sleeps (`SPURS_IDLE_SPIN_US`, set by
  `main.cpp`). On the idle 3D menu, three alternating pairs went from 110.1
  to 114.3 fps. 50 us and 1000 us did no better.
- **WWS job modules always took the slow branch resolver.** Every indirect
  branch made while a job-module overlay (id 200 and up) was resident went
  through `spu_indirect_branch_slow`, for the sake of LBP-era job hooks that
  are all diagnostics. Those modules do most of the SPU work here. The fast
  path now takes them when no diagnostic is on.

With those two in, the pier walk ran at ~122 fps, and the main thread still
spent about 30% of its time in that loop. Sampled together, the SPU threads
were busy 91% of the milliseconds the main thread waited and 88% of the rest:
the wait is SPU throughput now, not latency. Two cuts to what each SPU pays
per DMA:

- **Fences.** Every MFC command went through four full fences: two in the
  lock-line path, which every `MFC_Cmd` write enters first, and two around
  the DMA itself. On x86, which only lets a load pass an earlier store, the
  lock-line pair is now kept to lock-line commands, and a GET (loads only)
  skips the fence after it. About 70% of `spu_wrch`'s samples sat on those
  fences.
- **A channel read built its 128-bit result as a 4-byte store under an
  8-byte reload**, which the CPU cannot forward: three quarters of
  `spu_rdch`. It is now written as two 64-bit halves.

**The integer helpers were scalar on x86.** Past the pier tutorial (the
drive in `work/runs/drive_tut.sh` hangs from a ledge at ~65-85 fps) one job
module, `jobmod_6d1c65171899` (image 209), is 38% of all SPU time. By its
DMA it reads mesh data from main memory and PUTs vertex data into RSX local
memory: geometry processing.

Its hottest function counted bits one at a time. `spu_cntb` had NEON code
and, on x86, only the per-byte reference loop. The same was true of 45 more
integer ops: `xsbh`, `ceqbi`, `xshw`, the immediate rotates, `fsm`, `clz`,
`gb` and the rest of `runtime/spu/spu_helpers.h`'s NEON list. Their
references are loops over the `u128` union. The compiler turned them into
general-register code, or into 30 instructions where one does, and every
call paid a round trip through the stack from the vector registers the
lifted code keeps its operands in.

They now have SSE4.1 forms, with AVX2 for the per-word shifts and AVX-512
BW/VL for the per-halfword shifts and `lzcnt`. `tests/test_spu_int_neon.c`
checks every one against its reference: 2.8 million random cases, gcc and
clang, x86-64-v2/v3/v4. Two alternating pier runs: gameplay 112 -> 123 fps,
cutscene 83 -> 88.

The Wwise chain still falls behind under load (see "Falling behind under
load"): a job then runs a descriptor the title has already refilled, loops
past its step budget, loops on the interpreter as well, and the chain used to
report it with a synthetic completion. The WWS handlers took that for a
message and called through NULL, as with the empty slots before. With
`SPURS_JC_NO_SYNTH=1` (set by `main.cpp`) such a job is dropped silently too.

**Profile-guided optimisation: tried, no gain.** Two attempts on the lifted
code, which both run through clang:

- **Instrumented** (`-fprofile-instr-generate`). The five SPU threads bump
  the same counters, and the game ran at 2-7 fps: no profile of anything
  real.
- **Sampled** (AutoFDO). `perf record -b` on the Zen 5's LbrExtV2 during
  the tutorial and the walk, `llvm-profgen` to a 10 MB profile, then
  `-fprofile-sample-use`. In two alternating pairs of pier benchmarks against
  the same build without the profile, gameplay ran at 121.5 and 123.0 fps
  against 123.1 and 123.4, and the cutscene at 82.8 and 82.4 against 83.4
  and 83.1.

The plumbing was removed. `bench_pier.sh` keeps `BENCH_PERF_AT=tutorial` and
`BENCH_PERF_ARGS` from it.

### Local memory write tracking (`PS3RECOMP_WTRACK`)

A gameplay profile had the RSX thread 97% busy, and most of that time went to
textures and vertices it had already seen.

**Textures.** The engine hashed every texture in use once a frame (35-90 MB)
to notice changes. Every one of them lives in RSX local memory.

**How the tracking works** (`runtime/memory/guest_wtrack.c`):

- Once a range has been read, its pages become read-only.
- The first write, from any thread and by any path, faults. The SIGSEGV
  handler makes the page writable again and stamps it from a global clock.
- A range is unchanged since stamp S while no page in it carries a later
  stamp.
- Host code that has the kernel write guest memory (fread, recv) brackets the
  call. Otherwise the kernel would fail with EFAULT instead of faulting.

**Results.**

- Texture cache lookups went from 3.7 ms to 0.3 ms a frame.
- `=2` keeps hashing and reports any change the tracking missed. The
  cutscene reports none.

### Geometry cache (`PS3RECOMP_GEO_CACHE`)

The same tracking validates a cache of each draw's decoded vertices and
rebuilt indices:

- The key is everything else the result depends on.
- A draw over unwritten memory skips the index decode, the remap, the fetch
  and the topology rebuild.
- Geometry the SPUs rewrite every frame is let go after two misses, so its
  pages are not armed to fault every frame.

**Results.**

- In the cutscene, 87% of draws hit.
- Vertex work went from 3.7 ms to 1.5 ms a frame.
- `=2` rebuilds every hit and compares the results; the cutscene reports
  none differing.

### The Wwise job chain and its guard

The gameplay freezes all came from the audio job chain. A lifted Wwise job
would loop forever, and a NULL call followed in the WWS handlers.

**What `SPURS_JC_TRACE=1` showed.** The audio thread sometimes notifies the
chain's job guard two or three times in a burst. The runtime banked each
notify as a credit, so the chain ran extra laps. Those laps read descriptors
the title was still writing for its next frame, sometimes only half written.

**The fixes.**

- **Guard count as on hardware:** a notify never takes it below zero, and
  a notify while the guard is already open is lost.
- **The chain wakes on a condition variable**, instead of polling every
  1 ms.
- **The job manager's grab:** a lap's descriptors are copied the moment the
  guard opens.
- **Each job reads its descriptor once.** It used to read it twice: once to
  pick the lift, again to stage. In between, the title could swap the
  binary.

**Falling behind under load.** The audio thread queues work two submissions
deep (two descriptor banks, alternating) and notifies in bursts, a few
submissions within a millisecond. At the pier a lap takes ~0.5 ms and the
walker always waits at the guard when the notify comes (`SPURS_JC_TRACE`:
notify to open under 100 us, no descriptor or command rewritten during a
lap). In the Beast fight, laps reached 8-10 ms; the thread refilled a bank
the walker had not finished, and the walker read one slot zeroed. Two
things then made a skipped job fatal, and both are gone:

- the walker sent a synthetic "job done" for the job it skipped, to every
  queue attached to the chain; the WWS job-manager threads never get an
  event from this chain, read it as a message and called through NULL;
- a guard closed for 20 s ended the walk with another synthetic event.

A garbage slot is now skipped with no event, and a guard is waited on for as
long as its chain runs. The heaviest jobs are Wwise DSP binaries at
0x008AE400 (~0.7 ms), 0x008A7080 and 0x00898880 (~0.35 ms each); a lap that
runs many of them is where the time goes.

**Diagnostics.**

- A job that runs past its step budget (`SPURS_JOB_LIFT_BUDGET`) is run
  again on the interpreter, from the very local store it was given.
- Its store is kept (`SPU_DUMP_STUCK`) for `PS3RECOMP_SPU_REPLAY=<file>`.

### Ambient occlusion on the GPU (`PS3RECOMP_NATIVE_SSAO`)

- **Passes.** Five fragment passes:
  - half-size linear depth;
  - 12 samples per pixel;
  - a depth-aware 5-tap blur across;
  - the same blur down;
  - a depth-aware upsample to the R8 image at 0xCF800000 (B8, linear,
    1280x720).

  They run at the depth copy, from the normals target and an RGBA8 snapshot of
  the depth buffer. The texture unit that samples 0xCF800000 gets the result.
- **SPU side.** The plugins' entry (LS 0x6C30, images 240-244) is gated. It
  returns at once while the game samples the GPU's result, and runs as usual
  otherwise.
- **Parameters change every frame with the camera's field of view.** They sit
  in the job's 0x140-byte block at 0x00A26780:
  - +0x00: 1/z = a·d24/(2^24−1) + b;
  - +0x10 / +0x40 / +0x70: the sample tables;
  - +0xA0: the camera matrix, whose first three columns rotate a normal into
    pixel offsets;
  - +0xE0: the unoccluded total, the projection scale, the sphere radius, the
    falloff and the depth clamp;
  - +0x100: the output remap and the blur and upsample weights.

  Fixed constants taken from one capture draw wrong halos in half the scenes.
- **Checked** with `=2`, which computes without substituting and diffs against
  the SPU's output: at most 0.08% of pixels are more than 8 levels off, and the
  mean error is under 1.4 levels (SPU float truncation).

### Lighting on the GPU (`PS3RECOMP_NATIVE_LIGHTING`)

- **Passes.** Five compute passes:
  - per light;
  - bounds of the view-space positions per fifth of a tile;
  - the lights that reach each of the 40x18 tiles;
  - per pixel;
  - clearing tiles where no pixel was in range.

  Up to 256 point and spot lights. Output:
  - diffuse: RGB square-root encoded with a shared scale in A;
  - specular: the intensity, in A.

  Both are written over the copies, as the job does.
- **Inputs.**
  - The two images are latched (privately copied) at their NV3089 copies.
  - The parameters are taken when the job loads them: a DMA watch on its
    256-byte block at 0x00A25A80. The block holds the position matrix at
    +0x10, the clip matrix at +0x50, the light count at +0x90, the light table
    at +0x94 (48-byte records) and the colour scale at +0x98.
  - The lighting is computed when the game first samples one of the two
    copies.
- **SPU side.** The job's per-tile function (LS 0x4AC8 of image 212) is gated.
  Nothing below it touches a channel or DMA, so the job's transfers, queue and
  label stay as they are.
- **Checked** with `=2`:
  - Most frames have under 0.2% of pixels more than 1 level off.
  - In a few frames, up to ~0.4% of pixels are more than 4 off, at the edges
    of lit objects.
  - Some tiles disagree on "lit by nothing" against "empty". Both decode to
    black.
  - Taking the job's own parameters instead of reading them at the copy did
    not change these numbers, and neither did disabling the tile culling.

### What the GPU passes made unnecessary

While both passes are live, the copies have no reader left.
`rsx_draw_engine_copy_note` returns nonzero and the NV3089 copy skips the guest
side: no GPU readback (each one was a flush and a wait, five per frame), no
pixel conversion, no CPU copy. That took the frame from about six GPU flushes
to one.

The gated jobs still run their DMA. `spu_dma_skip_range` makes their
transfers of the G-buffer, the SSAO scratch and the occlusion image complete
without moving bytes, which removes ~20 MB of copies a frame.

A looser rule for the second strip, which accepted any destination in the
first row, broke the video segment. That memory is reused during loading. The
strip must come from the same source at the same offset.

### Gates and fallback

`spu_gate_function` (ps3recomp patch) makes a lifted SPU function return as
soon as it is entered, but only while a flag says the GPU's result is what the
game samples:

- The flag is set by a successful substitution and cleared by any failure: a
  backend refusal, an unsupported light, or a texture bound in a format or
  size the GPU image does not fit.
- When the flag is clear, the SPU does that frame's work.

## Particles and fragment depth export

The game draws its particles (smoke, fire, sparks, fog sheets) into a 512x288
colour target (`0xB9E000`) over a 512x288 depth buffer (`0x0F770000`), then
blends the result over the 1280x720 picture in the final composite. The
particle shader reads that depth too and discards a fragment behind the scene
(soft particles). The depth buffer is filled each frame by one full-screen
pass that writes depth from the fragment program: `r1.z`, with
SHADER_CONTROL's depth-export bits (`0x0E`) set.

The fragment program translator had no depth export, so that pass left the
buffer at its clear value. Particles were depth-tested and soft-tested against
a flat 0.5, so most of them were dropped and large sprites were cut along
straight lines -- the likely source of the flat, dark rectangles reported over
the water in a cutscene (not yet checked in that scene). Now `r1.z` becomes `SV_Depth` (as RPCS3 does, from r1 whatever
the colour export width), and the downsampled buffer matches the scene.

`PS3RECOMP_ENG_DRAWTRACE_PX=<fx>,<fy>` names the draws that change one pixel,
which is how the pass was found.

## Water

The water is drawn after the lit scene (`0x011C0000`) and its 256x256
reflection (`0x0F6F0000`), in two passes over the same mesh:

1. A shading pass writes the water's colour into `0x003B0000`. It reads the
   scene, the reflection, the scene depth and a 512x512 normal map.
2. A copy pass draws the mesh again into the scene. Its fragment program only
   samples `0x003B0000` at the fragment's own place.

For that place, the vertex program writes the screen uv times w into a
texcoord. The fragment program reads it with SRC2 bit 31 set, the
perspective-correction flag, which multiplies the interpolated value by 1/w.
RPCS3 does the same, as `tc * gl_FragCoord.w`. The translator ignored the flag,
so the copy sampled uv x w, outside the water, where `0x003B0000` is black. The
New Marais swamp and the sea at the Empire City pier came out black.

The flag now becomes `tc * RSX_FRAG_RCP_W`. That is `SV_Position.w` under
glslang, which follows Vulkan's `FragCoord`, and `1 / SV_Position.w` under
D3DCompile, which defines `RSX_DX_POSITION_W`. In a New Marais frame, 183 of
the 507 fragment programs read a texcoord this way. Most of them are
full-screen passes, where w is 1.

`TEX_COORD_CONTROL` (`0x0B40 + 4i`, bit 0: texcoord i is 2D, its z and w read
0 and the fragment's w) is not modelled. The drawtrace logs where the game
sets it.

## Resolution scale (`PS3RECOMP_RES_SCALE`)

`PS3RECOMP_RES_SCALE=2` (or 3, 4) draws every colour and depth target that
many times the guest's size each way; textures stay at the guest's size. The
engine still deals in guest pixels everywhere -- target sizes, viewports,
scissors, readback rectangles, snapshot extents -- and the Vulkan backend
converts at the edge, so every size the engine compares (the texture/surface
match, the writeback pitch check, the small-target writeback, the G-buffer
copy checks of the native passes) stays guest against guest. What the
backend does:

- the viewport and scissor x N, and the depth-bias slope x N (a scaled pixel
  sees 1/N of the slope);
- a readback, which is how the CPU sees a target (auto-exposure, NV3089
  copies), filtered down to guest pixels: colour through a linear blit,
  depth and stencil by taking the middle texel of each N x N block;
- a colour target seeded from guest memory goes up through a nearest blit;
- ZPASS counts divided by N^2;
- the native lighting's image size words rewritten for the scaled images
  (SSAO already scales with its image);
- the window gets the full-size frame; `s_vk.color` stays the guest-size one
  the dumps read.

The one engine-side change is WPOS: the GPU hands the fragment program image
pixels, so `fp_alpha.w` carries 1/N.

At 2x the pier, the tutorial and the cutscene render with the same light,
particles and exposure as 1x, and nothing falls back to the SPU. At first it
cost a lot: the pier walk at ~63 fps and the tutorial at ~89, against ~123.
The RSX thread waited for the GPU at the end of every frame (46% of its time
at 2x, 18-22% at 1x), so GPU time added to the frame instead of overlapping
it. With frame pipelining (below) the pier walk runs at ~114 fps at 2x
(2560x1440), and the 3D menu at ~65 fps at 3x (3840x2160), where the GPU
becomes the limit.

## Frame pipelining (`PS3RECOMP_VK_PIPELINE`)

A present used to submit the frame and wait for the GPU to finish it. Now it
submits and goes on: the next frame records into a second set of everything
a batch holds while the GPU renders the first, and a set is waited on only
before it is recorded into again (two frames in flight). The set is:

- the command-buffer chain and its fence;
- the window blit's command buffer and acquire semaphore;
- the vertex, uniform, staging and readback rings;
- the dead buffers;
- the four descriptor pools;
- the ZPASS queries and reports;
- the deferred readbacks.

`vk_slot_swap` exchanges the two sets, so nothing that uses the globals
changed. `vk_flush` keeps its meaning: it submits what is open and waits for
both. Two things had to change with it:

- **ZPASS.** A report recorded before any CLEAR in its batch also counts what
  the batches before it counted since the clear. That count is only known
  once they have completed, so it travels as a carry from batch to batch.
- **The small-target writeback** (the 20x11 luminance the CPU reads) is now
  recorded into the frame's own batch before the present, and the rows reach
  guest memory when that batch completes (`readback_deferred`). Before, it
  forced a wait right after every present.

The parked frame is also polled (`vkGetFenceStatus`, every 64 operations) and
completed as soon as the GPU is done with it, so the reports and the
luminance arrive about a frame later, which is when the game reads them.

Effect: the RSX thread's time at present went from 21-33% to 0-4% at 1x, the
2x pier walk from ~63 to ~114 fps, and the 3D menu at 1x from ~117 to
~120-125. `PS3RECOMP_VK_PIPELINE=0` restores the wait.

Not scaled yet: lines and points (1 pixel), NV3089 copies (CPU copies of a
downsampled readback, so water and blur sources stay at 720p).

## What limits the frame rate where (2026-10-16)

Two scenes, two different limits:

- **Under the Empire City pier** (the "Blast open the gate" sequence, reached
  by `work/runs/drive_tut.sh`). The RSX thread drains 97% of the time
  (`PS3_CLOCK_STATS`). The frame has ~10,500 draws, against ~4,500 at the
  pier and ~3,700 in New Marais.
- **New Marais** (`work/runs/nm_bench.sh`, from a copy of a save). The RSX
  thread is busy about 70% of the time, and the main PPU thread spends ~40%
  of its time in `usleep` polls. Waits at `0x370640`/`0x370668` and
  `0x3710B8`/`0x3710E4` make up ~55% of the frame, and all five SPURS SPUs are
  busy through them: the frame waits on SPU throughput.
  - SPU time divides into job code (~50%: `jobmod_6d1c` geometry 16%, `ec5f`
    8%, `bc11` 8%, `5cff` 7%), dispatch (~17%: `spu_drain_call`,
    `spu_task_launch_check`, `spu_lookup`, `spu_indirect_branch`) and DMA
    (~11%).
  - `SPURS_NSPUS_MIN=7` changes nothing, nor does raising every workload's
    maximum contention to 7 with it: the game's work does not spread over
    SPUs it did not ask for.
  - A longer idle spin (`SPURS_IDLE_SPIN_US=2000`) is worth ~3%.

`PS3_TIMELINE=<frame>,<n>` now also records each `usleep` (thread, caller)
and the SPURS idle mask every 250 us, which is how the second picture was
taken.

### Cheaper draws on the RSX thread

All of these leave the output unchanged:

- **VS constants by range.** The Vulkan backend now takes
  `bind_vs_constants_range`: only the rows the vertex program reads, and the
  viewport tail, are compared and copied. Before, every draw copied the whole
  8 KB register file three times, and compared it once; under the pier that
  was 86 MB a frame. `PS3RECOMP_ENG_VPC_FULL=1` forces the whole block.
- **Vertex fetch.** A draw fetched over its whole index range (the remap's
  ranged path) has consecutive elements: the fetch walks the source by stride
  instead of resolving each reference. SNORM16, SINT16 and UINT8 got their
  own loops; they went through the per-element reference decoder, and SNORM16
  is the most common format under the pier. The per-vertex element range is
  computed once per draw instead of once per attribute.
  `tests/test_vertex_fetch.c` covers the consecutive case now.
- **Texture binding memo.** A unit whose registers repeat within a frame, and
  which names a plain texture (no colour or depth target at its address),
  reuses the handle and sampler it got earlier in the frame. The texture
  cache checks a texture for changes once a frame anyway. `s_tex_gen` drops
  the memo when a target is created or a texture handle is replaced or
  evicted. `PS3RECOMP_ENG_TEXMEMO=0` turns it off.
- **The FIFO feeds the old state machine less.** With the register-file
  engine drawing, `rsx_process_method` (the vtable path's `rsx_state`) has no
  backend and no reader. It is now fed only the methods with effects outside
  that state: the reference, the back-end labels and the draws. It was ~8%
  of the RSX thread. `PS3RECOMP_FIFO_LEGACY=1` feeds it everything.
- **SPU function lookup.** Each SPU thread caches `spu_lookup`'s answers. The
  registry only changes while it is built.

Measured: under the pier, 27-29 fps became 33-37. In New Marais, ~74 became
~80.

### The rings that flushed every frame

The engine's per-draw data lives in host-visible rings (vertices and indices,
uniform blocks, staging). The rings are reset when a batch completes. When one
filled up mid-batch, `vk_ring_alloc` or `vk_eng_draw` called `vk_flush`, which
submits the batch and waits for the GPU. That wait in the middle of the frame
also undid the frame pipelining for that frame.

Each draw writes its 8.7 KB VS block into the 16 MB uniform ring when the block
changed. `PS3RECOMP_VK_FLUSH_CALLERS=1` with `PS3RECOMP_VK_STATS=1` named the
draw as the flusher:

| Scene | Draws | Flushes | GPU wait |
|---|---|---|---|
| New Marais | ~3,700 | 1 per frame | ~1.2 ms |
| Under the pier | ~10,000 | several per frame | — |

A ring that runs out now doubles at its next empty start, up to 1 GB. The
descriptor pool's per-batch set count went from 4,096 to 16,384 for the same
reason.

| Scene | Before | After |
|---|---|---|
| New Marais | ~79 fps | 85-88 fps |
| Hanging under the pier | 27 fps (33-37 after round 1) | ~105-110 fps |

`[vk-stats]` now shows 0.1 flushes a second.

### SPU: fewer trips through the drain loop

- **`--merge-chunks`.** The SPU lifter splits code at every branch target.
  Without this option, each split became a separate C function, joined by
  trampolines. Each hop returned to `spu_drain_call`, wrote every register
  back to the context, looked up the next function and reloaded the
  registers. The geometry job's inner loops (`jobmod_6d1c`, 0xD7F4 ->
  0xD8A8 -> 0xD9E8) paid this on every block. `--merge-chunks` (an option of
  the toolkit's `spu_lifter.py`) emits each run of contiguous chunks as one C
  function, with gotos where the trampolines were. Every chunk is still
  registered for indirect entry. `work/tools/lift_spu.py` now passes it, and
  the lift is still reproducible (a relift diffs clean). Effects:
  - in New Marais, the SPUs are busy 3.2 of 5 during the main thread's
    waits, against 4.0 before;
  - the pier tutorial runs at 125-134 fps, against ~122-127.
- **What came with the merge.**
  - Frames get bigger, up to ~7.7 KB in one job function, with recursion on
    top. The SPURS kernel threads and the job-chain thread went from 1 MB to
    16 MB of stack, as lv2's SPU threads have.
  - The lifter no longer turns a branch to LS 0 into a goto inside a group: a
    job lifted at 0 ends there, and the drain has to see it.
  - The lifter's register localisation no longer spills around the MFC
    parameter writes.
  - Still open: the nops-plus-self-branch halt idiom is only recognised for a
    whole function, not for a chunk inside a group. Today's lift has none.
- **Inline hop check.** `spu_task_launch_check` now has an inline fast path,
  `spu_task_launch_hop`, for when no diagnostic is on and no stack-reset
  entry is registered.
- **Inline MFC parameter writes.** The writes to the MFC parameter channels
  (`MFC_LSA`, `EAH`, `EAL`, `Size`, `TagID`), which only latch a value, are
  done inline in lifted code (`spu_wrch_fast`).

### What the main thread waits for in New Marais

The `PS3_TIMELINE` usleep events name the main thread's three waits:

- **`0x370640`/`0x370668`:** the SPU job progress poll (`func_00504D90`). It
  is busy with all SPUs running: throughput.
- **`0x3710B8`/`0x3710E4`:** the same kind of wait, mixed.
- **`0x133640` (`func_001320A8`), ~20% of the time with every SPU idle:** a
  loop on `CellGcmControl.ref` (0x20002008). It reads `while ((s32)(ref -
  target) < 0) usleep(5)`. The game writes one `SET_REFERENCE` per frame,
  into a ring of four command buffer segments (IO 0x10000..0x13000), and
  waits for the RSX to have consumed the segment it is about to reuse. That
  wait is on the RSX thread's throughput, not the SPUs'.

`GCM_REF_PACE_US` (default 200) is the minimum spacing between two fences
the poll path publishes. 0 hangs the boot: the loading screens have
equality waiters. In game there is only one fence a frame, so the pacing
does not matter there.

### Vertex pulling (`PS3RECOMP_ENG_VPULL=1`, experimental, off)

The vertex shader can read the guest's vertex bytes itself:

- `rsx_pull` decodes them in HLSL as `rsx_vertex_decode_element` does, from a
  `ByteAddressBuffer` over the vertex ring (binding 2). The upload offset
  comes as a push constant, and per-attribute descriptors ride after the
  viewport transform in the VS block.
- A triangle list from one index run with no restart in it, or one array run,
  is drawn without a host index buffer: `rsx_index` reads the big-endian
  index from the uploaded run.
- Other rebuilt topologies keep the CPU's index rebuild, but upload raw
  vertices.
- `dc_pull_raw` / `dc_pull` / `dc_pull_ranges` in `rsx_draw_engine.c`, and
  `vk_eng_draw_pulled` in the Vulkan backend.

Status: it renders New Marais and the pier correctly. It uploads ~9 MB of raw
bytes a frame there, against 22 MB of expanded vertices. It bypasses the
geometry cache, though: in New Marais it is ~3% slower than the cache. It
stays off until it wins somewhere. Known gaps before it can be the default:

- The shader's divisions (SNORM16, UNORM8, CMP32) may round differently
  from the CPU decoder's, by up to 2.5 ULP. A pass drawn pulled and another
  drawn fetched could then disagree on depth by one ULP.
- A pulled pipeline that fails to build drops its draws, instead of falling
  back to the fetch.

## Saves

The game loads its save icon once, at boot, through the raw `sys_fs`
syscalls, as the relative path `./USRDIR/ui/xmb/savedata/icon.png`. That
layer translates paths on its own and defaulted to the working directory,
which `play.sh` puts one level above the disc tree, so the open missed. The
game then handed `cellSaveData` an ICON0 write with no buffer, which the
library refuses (as the firmware does), and every save stopped after
`PARAM.SFO`: a save directory existed with its subtitle ("(Autosave) 2.
Breaking into New Marais") and no data. The game showed "There was an error
while saving." (the dialog is answered on the console, not drawn) and retried
`ListSave2` for the rest of the session, thousands of times.

`main.cpp` now points `sys_fs` at the same root as `cellFs`, and reads the
title id from `PARAM.SFO` (cellGame had been reporting the placeholder
`BLES00000`).

## Long sessions

Twenty minutes into the game, past the opening, showed what a short benchmark
never fills:

- **Pipeline lookup.** The draw engine found a draw's pipeline with a linear
  scan over every pipeline built so far, up to 8192, on every draw. It is
  now a hash index, with the last hit tried first.
- **Samplers.** The Vulkan backend cached 64 sampler states. Past that every
  new state got the default sampler, so later areas sampled with the wrong
  filtering, wrap modes and LOD range. The cache now holds 4096, capped by
  the device's `maxSamplerAllocationCount`, and is hash-indexed.
- **Framebuffers.** 64 cached, then all dropped after a flush; now 512,
  indexed.
- **Logs.** Several "log the first N, then go quiet" counters were
  incremented unconditionally. After 2^31 events they wrapped negative and
  logging started again; one session wrote a 2.3 GB log. They saturate now.

## Stuck SPURS jobs

A lifted Wwise job once looped forever in gameplay after 66,000 normal runs,
and the audio chain froze the game with it.

- **Step budget.** Every lifted job now has one (`SPURS_JOB_LIFT_BUDGET`,
  default 50 million). It counts trampoline hops, plus the loop heads of the
  job binaries, which are lifted with `--loop-budget`; a loop inside one
  function never comes back to the trampoline. The longest real job measured
  is ~274,000 steps.
- **Retry.** A job that runs past its budget is run again on the interpreter,
  but only when it has written nothing outside its local store (no PUT, atomic
  put or outbound mailbox), so nothing is repeated:
  - If the interpreter finishes it, the lift was looping, and that binary runs
    on the interpreter for the rest of the session.
  - If the interpreter does not finish it either, the input is wrong.

  The log says which one happened.

## Diagnostics worth knowing

| Variable | What it does |
|---|---|
| `PS3RECOMP_ENG_FPSLOG=1` | frames per 10 s |
| `PS3RECOMP_ENG_SHOT=<dir>` | the presented image of every Nth 3D frame, as PPM (`-` logs mean brightness) |
| `PS3RECOMP_NATIVE_SSAO=2`, `PS3RECOMP_NATIVE_LIGHTING=2` | compute on the GPU and diff against the SPU, once a second |
| `PS3RECOMP_NATIVE_SSAO_DUMP=<dir>`, `PS3RECOMP_NATIVE_LIGHTING_DUMP=<dir>` | images of those comparisons |
| `PS3_CLOCK_STATS=1` | where the RSX thread's time goes, with per-label waits |
| `SPU_OVL_DMA=<overlay>,<n>,<skip>` | the DMA transfers of one job module |
| `PS3RECOMP_VK_STATS=1` | Vulkan flushes and time spent waiting on the GPU |
| `SPURS_JOB_STEPS=1` | each lifted job image's longest run, in steps |
| `SPU_DUMP_MISS=<dir>` | write out job binaries that have no lift |
| `PS3_POLLTOP=<s>` | the guest's busiest `usleep` poll sites, per thread |
| `PS3RECOMP_VK_SPLIT=<n>` | operations per command buffer before it goes to the GPU (0: one per frame) |
| `PS3RECOMP_ENG_HASHSTATS=1` | texture bytes hashed per frame, by memory and span size |
| `PS3RECOMP_ENG_DRAWTRACE_PX=<fx>,<fy>` | with `PS3RECOMP_ENG_DRAWTRACE`: every draw that changes the pixel at that place (fractions of the target) |
| `PS3RECOMP_VK_PIPELINE=0` | wait for the GPU at every present instead of keeping a frame in flight |
| `PS3RECOMP_RES_SCALE=<n>` | render targets n times the guest's size each way (1-4); see "Resolution scale" |
| `PS3RECOMP_VK_PRESENT=mailbox\|fifo\|immediate` | presentation mode (mailbox by default: no tearing, no stall while the window is hidden) |
| `PS3_VERBOSE=1` | log every SPU job dispatch and every wait (on by default when stderr is not a terminal; costs ~15% at the pier) |
| `BENCH_PERF=<s>` | `work/tests/bench_pier.sh`: also a `perf` profile of the walk; threads are named (`spurs0`..`spurs4`, `rsx`, `p:<guest name>`) |
| `PLAY_PROFILE=0`, `PLAY_PROFILE_BELOW=<fps>` | `work/play.sh` records 20 s of `perf` after 30 s of 3D frames under 70 fps (at most three a session); off, or another threshold |
| `SPURS_IDLE_STATS=1` | what ends each idle SPU's wait (a notify, a timeout onto work, its spin), every 10 s |
| `SPURS_IDLE_SPIN_US=<us>` | how long an idle SPU polls for work before it sleeps (200 in the port) |
| `PS3_SAVEDATA_ROOT=<dir>` | the savedata directory itself; every script that boots the game points it at a fresh directory, so a test never loads or overwrites a player's saves |
| `PS3RECOMP_ENG_SHADER_DUMP=<dir>` | each new pipeline's HLSL as `p<N>_vs.hlsl` / `p<N>_ps.hlsl`; the `p<N>` matches the drawtrace's only within the same run |
| `PS3RECOMP_ENG_DRAWTRACE_AT=<file>` | with `PS3RECOMP_ENG_DRAWTRACE`: trace the frame after `<file>` appears (a script picks the moment) |
| `PS3RECOMP_ENG_SHOT_FROM=<frame>,<n>`, `PS3RECOMP_ENG_SHOT_ALL=1` | shots only in that window of frames (with `SHOT_EVERY` small, to look for a flicker); light frames too |
| `PS3RECOMP_ENG_STALE=1` | skip a colour target whose guest bytes were written after it was drawn (2 also logs); off by default, page-granular and racy with deferred writebacks |
| `PS3RECOMP_ENG_TEXMEMO=0` | bind every texture unit afresh (no per-frame memo) |
| `PS3RECOMP_FIFO_LEGACY=1` | feed every FIFO method to the vtable path's `rsx_state` too |
| `PS3_TIMELINE=<frame>,<n>` | events over n engine frames: RSX waits, SPU labels, each SPU's job every 250 us, the SPURS idle mask, every `usleep` (thread, caller) |

## Lessons

- **Brightness wrong overall:** suspect a GPU readback the CPU makes. The game
  reads a 20x11 luminance reduction from VRAM for its auto-exposure, so render
  targets up to 1024 px are written back to guest memory every frame.
- **SPU float:** truncating, no denormals, extended range. AVX-512's `{rz-sae}`
  reproduces it bit-exactly. Set DAZ in MXCSR but not FTZ: FTZ hangs the game.
- **Parallel builds:** each chunk of the lifted PPU code takes about 2 GB to
  compile with clang (5 GB with GCC). Build in a memory-capped scope without
  swap: a build that filled zram froze the desktop.
