# Recording Vulkan on a thread of its own: plan

## Research (2026-10-17)

Heavy view (heavy_bench.sh: a copy of the player's save, standing still, ~9,100
draws a frame), with `PS3RECOMP_VK_CALLSTATS=1` and
`PS3RECOMP_ENG_DRAWSTATS=1`:

- Per frame, the calls that return something:

  | Call | Per frame |
  |---|---|
  | surface_view | ~9,600 |
  | color_snapshot | 115 |
  | depth_snapshot, depth_snapshot_rgba8 | 6 each |
  | geo_put | ~7 |
  | native_ssao, native_lighting | 1 each |
  | readbacks, creates | ~0 |

- surface_view returns the same view for the same (surface, remap, format)
  until one of them is released, and the snapshots refill one texture per
  target. Both can be answered on the engine's side after the first call.
- Engine time per frame: backend draw 5.3 ms, constants and binds 1.3 ms,
  textures 2.7 ms, pipeline lookup 2.4 ms, fetch 3.8 ms.
- Borrowed data: vertices and indices of a draw (31 MB a frame on cache
  misses), VS constant rows (11 MB), FP constants, texture arrays.

GO: a wrapper around the backend interface, generic, not inside the Vulkan
backend.

## Phases

1. **Wrapper and queue** (done, `libs/video/rsx_be_thread.c`,
   `PS3RECOMP_BE_THREAD=1`).
   - An SPSC byte queue of 128 MB and a worker thread.
   - Value-returning calls are answered from caches or drain the queue.
   - geo_put is direct (the arena is locked).
   - Deferred-readback completions run on the engine's thread.
   - First run in the heavy view: ~48 -> 54.5 fps, same image.
2. **A/B and review** (done). The review found two high-severity bugs,
   both fixed: kept views and snapshots that the backend had released by
   itself (now `free_gen`), and the VS rows handed back to the producer
   before their draw ran (now `rfree`). Heavy view 47.7 -> 53.6 fps, New
   Marais 97.7 -> 100.1. On in `play.sh`.
3. **Zero-copy vertices.** The engine hands its fetch buffer over instead of
   having it copied (a pool the worker returns buffers to), if the
   producer's copies show up in the profile.
4. **Default on**, once the suite and a session of play are clean.

## Rollback

`PS3RECOMP_BE_THREAD=0` (or unset while it is opt-in) gives the engine
the backend itself, as before.
