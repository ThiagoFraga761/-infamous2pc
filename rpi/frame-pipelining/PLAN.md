# Plan: frame pipelining

What the backend does today was inventoried in full before planning (the
agent report is summarised here).

## How present works today

Every present makes three CPU waits on the GPU, one after the other:

1. `vk_eng_present`:
   - blits the target into `s_vk.color`;
   - `vk_flush`, which submits the frame and waits.
2. `vk_cb_present`:
   - copies `s_vk.color` into `s_vk.readback`;
   - submits and waits.

   The result feeds `last_center` and the PPM dump. In engine mode
   `last_center` is never read, so outside a dump this is a full-frame copy and
   a wait for nothing.
3. `vk_present_window` (windowed only):
   - acquires an image and blits into it;
   - submits and waits on the shared fence;
   - presents.

Then the engine's WB_SMALL loop writes back each colour target of 1024 px or
less. Each one costs a flush plus a readback wait.

## Phase 0: fewer waits, still synchronous

- Skip the readback in engine mode unless `PS3RECOMP_VK_DUMP` is set.
- Record the window blit into a second command buffer (`cmd_present`) and hand
  it to the same `vkQueueSubmit` as the frame:
  - as its own `VkSubmitInfo`, which waits on the acquire semaphore and
    signals `sc_done`;
  - with one fence for both.

  That turns three waits into one.
- WB_SMALL is unchanged.

Check: same frames, the regression suite, and the fps and `vk-stats`
difference.

## Phase 1: two frames in flight

- **Frame slots.** Each slot k (two of them) has its own:
  - command buffer and fence;
  - vertex, uniform and staging rings;
  - descriptor pools: e2, zc, ao and lt;
  - dead-buffer list;
  - ZPASS query pool and reports;
  - pending deferred writebacks.
- **Present.** Submit the slot without waiting and switch to the other slot.
  Before recording into a slot again, wait on its fence. Then run the slot's
  completions (dead buffers, ZPASS reports, deferred writebacks) and reset its
  rings and pools.
- **`vk_flush`.** Keeps its meaning: submit what is open, wait on every slot
  and run every completion. Readbacks, destroys, and ring growth go through it
  unchanged.
- **Images, views, framebuffers and pipelines.** Destroying one still needs a
  `vk_flush` (wait all), as today. They are rare.
- **Acquire semaphores.** One per slot; the single `sem_acquire` is only safe
  because of today's wait.
- **Switch.** `PS3RECOMP_VK_PIPELINE=0` keeps the Phase 0 behaviour.

## Phase 2: deferred small writeback

Without this, the WB_SMALL readback right after present waits for the frame
that was just submitted, and nothing overlaps.

- A backend entry `readback_deferred(surface, region, cookie)` records the copy
  into the slot's staging buffer and returns.
- When the slot completes, the engine gets the bytes through a callback and
  writes guest memory as `rsx_draw_engine_writeback` does today.
- That puts one frame of latency on the luminance read. Check that the exposure
  timeline matches.

## Verification

- `PS3RECOMP_ENG_SHOT=-` brightness timeline against the current build.
- PPM frames from the boat scene.
- The suite, 8/8.
- `vk-stats`, `PS3_CLOCK_STATS` present %, and fps over the same 200 s run.
- code-reviewer in a separate context after Phase 1 and after Phase 2.
