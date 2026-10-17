# Recording Vulkan on a thread of its own

## Problem

In the heavy areas (the player's save past New Marais, 30-50 fps; under the
pier) the frame rate drops with the view, and the RSX thread is the limit: 96%
busy while the PPU main thread waits for it. Its time before the resident
geometry round (perf from the player's session, rsx thread only):

| Share | Work |
|---|---|
| ~18% | FIFO walk and method dispatch |
| ~20% | memcpy / memmove / memcmp / memset |
| ~18% | Vulkan backend and the radv driver |
| ~16% | vertex fetch, remap, index decode |
| ~15% | texture binding |
| ~13% | the rest of the engine, pipelines |

## Idea

The engine decides what each draw needs on the RSX thread, as now, and hands
the backend calls to a second thread that records them into Vulkan: the
backend's own CPU work, the driver's, and the ring copies run beside the next
draws instead of after them.

## What must hold

- Output identical: same images, same ZPASS reports, same readbacks.
- Every backend call that returns something the engine uses (handles,
  readbacks, snapshots) still sees the state of every call before it.
- Off switch to the current behaviour.
- No new hang: the producer never waits on a worker that waits on it.

## Measure of success

Steady fps in the heavy view (heavy_bench.sh) and under the pier, A/B against
the build without it; New Marais not slower.

## Open questions for research

- How many calls a frame return a value (surface_view, snapshots, creates,
  readbacks, geo_put)? Each one is a sync point unless it is made
  asynchronous.
- What data do the hot calls borrow from the engine (vertex and index
  pointers, constant blocks) and for how long?
- Where does the memcpy bucket come from now (call graph)?
