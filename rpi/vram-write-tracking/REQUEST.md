# Skip unchanged RSX local memory: write tracking by page protection

## Problem

In gameplay the RSX thread is the bottleneck. A 20 s profile of a live session
during a 25 fps stretch had it 97.5% busy. `PS3RECOMP_ENG_DRAWSTATS` puts a 3D
frame of the cutscene at about 17 ms on that thread, split as follows:

- textures: 4.9 ms, of which 3.7 ms are cache lookups that hash every texture
  in use once a frame (35-90 MB a frame);
- vertices: 5.1 ms (index decode, remap, fetch plan, fetch and decode);
- backend draw: 1.9 ms; the rest is smaller.

All hashed textures are in RSX local memory (`PS3RECOMP_ENG_HASHSTATS`).
Most of them, and most static geometry, do not change from one frame to the
next. The work is redone because nothing tells the engine whether those bytes
were written.

## Goal

Know, for any range of local memory, whether it was written since a given
moment, without reading it. Use that to:

1. skip the per-frame hash of unchanged textures (this request);
2. later, cache decoded vertex and index data the same way (a follow-up).

## Approach

Page protection. Once a range has been read (hashed), its pages become
read-only. The first write by anyone faults, and a SIGSEGV handler:

- makes that page writable again;
- stamps it with a global, monotonically increasing counter.

A range is clean since stamp S if every page in it carries a stamp ≤ S.

That covers every writer at once, with no change to the lifted code: PPU
stores (out-of-line, inline and vector), dcbz, SPU DMA, the RSX transfer
engines, and the HLE libraries.

The one exception is the kernel. A read()/recv() into a protected page fails
with EFAULT instead of faulting. Those paths unprotect their target range
first, under the same lock the engine takes to protect pages.

## Constraints

- Opt-in switch first (`PS3RECOMP_WTRACK=1`). With `=2`, every texture is still
  hashed, and the log reports any change the tracking did not see. That is how
  missed writers get found.
- The fault handler is async-signal-safe: only atomics and mprotect.
- Any fault it does not own is chained to the previous handler, or the default.
- The 8 regression tests still pass, and frames are unchanged
  (`PS3RECOMP_ENG_SHOT`).
- Measure the cost of the faults and of the VMA splitting. If a page keeps
  faulting every frame, stop protecting it.
