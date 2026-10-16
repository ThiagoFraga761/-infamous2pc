# infamous2-recomp

inFAMOUS 2 (BCUS98125, PS3) running natively on Linux through static
recompilation with [ps3recomp](https://github.com/sp00nznet/ps3recomp): the
game's PowerPC and SPU code translated ahead of time to C, a Vulkan renderer for
the RSX, no emulator.

**This repository contains no part of the game.** You need your own copy,
dumped from hardware you own. The build turns *your* executable into code on
*your* machine; that code is the game's, translated, and must not be shared.

## Status

The game boots, plays its intro video, menus, the opening cutscene and gameplay
in a window with a controller and sound. Measured on a Ryzen 7 9800X3D and a
Radeon RX 9070 XT (native 1280x720):

| Scene | Before this work | Now |
|---|---|---|
| 3D main menu | ~27 fps | ~115 fps |
| Opening 3D cutscene | 9-13 fps | 47-112 fps (by shot) |
| First gameplay area (the pier, walking) | -- | ~119 fps |

The largest gains come from the following changes:

- **Two SPU jobs moved to the GPU:** the ambient occlusion and the tiled
  deferred lighting, each checked against the SPU's output.
- **No more re-hashing or re-decoding of unchanged data.** RSX local memory
  is write-tracked by page protection, so textures and geometry nobody wrote
  are not hashed or decoded again.
- **Less waiting on the GPU:** the frame goes to it in pieces while it is
  being recorded.
- **Lifted PPU code built by clang for x86-64-v3,** with guest memory
  accesses inlined and guest registers kept in host registers across them.

Rendering above 720p: `PS3RECOMP_RES_SCALE=2 work/play.sh` draws at
2560x1440 (3 for 3840x2160). It works, but costs a lot for now (the pier at
~63-89 fps at 2x): the GPU's time is not yet overlapped with the next frame.

See [docs/NOTES.md](docs/NOTES.md). `work/play.sh` documents the switches.

## Requirements

- Linux on x86-64. The lifted PPU code targets x86-64-v3 (AVX2, FMA) and the
  SPU code x86-64-v4 (AVX-512). Set `PORT_SPU_MARCH=x86-64-v3` for a CPU
  without AVX-512, and `PORT_PPU_MARCH=` (empty) for one without AVX2.
- A Vulkan 1.0 GPU with a graphics queue that also does compute.
- Fedora package names (adapt for your distribution):
  `gcc gcc-c++ clang cmake ninja-build python3 SDL2-devel vulkan-headers
  vulkan-loader-devel glslang-devel`.
- About 14 GB of free RAM for the build, and ~3 GB of disk for the lifted code
  and build trees.

## Dump and decrypt

1. Dump your disc of inFAMOUS 2 (BCUS98125) from a PS3 you own. There are
   guides for this online. This project does not help you obtain the game.
2. Extract it so that `disc/PS3_GAME/` sits at the root of this repository
   (`disc/` is ignored by git).
3. Decrypt `disc/PS3_GAME/USRDIR/EBOOT.BIN` to
   `disc/PS3_GAME/USRDIR/EBOOT.elf`, for example with RPCS3 (Utilities →
   Decrypt PS3 Binaries). The port is made for version 01.00. `scripts/lift.sh`
   checks the executable's SHA-256 and warns on any other one.

## Build

```sh
scripts/setup.sh    # ps3recomp at the base commit + work/patches/ps3recomp.patch, Python venv
scripts/lift.sh     # PPU -> work/src/recomp, SPU -> work/port/spu (from your EBOOT.elf)
scripts/build.sh    # work/port/build-vk (add --tests for the headless build the tests use)
work/play.sh        # window + controller + sound
```

Lifting takes several minutes. The first build takes a long time, because the
lifted PPU code is large.

## Repository layout

| Path | What |
|---|---|
| `work/patches/ps3recomp.patch` | All changes to ps3recomp, against upstream `42f5f8f` (MIT) |
| `work/port/` | The port: CMake project, `main.cpp`, the GPU replacements of SPU jobs (`native_passes.c`) |
| `work/tools/lift_spu.py` | Lifts every SPU image the game runs (job manager, job modules, plugins, job chain, Bink, zlib) |
| `work/tests/` | Boot regressions (`run_all.sh`, needs the `--tests` build) |
| `scripts/` | Setup, lift, build, and `check_clean.sh` |
| `docs/NOTES.md` | Reverse-engineering notes: addresses, the SPU jobs, the GPU passes, diagnostics |
| `rpi/` | Design documents of the larger changes |

## Keeping the repository clean

`.gitignore` is an allowlist: everything is ignored unless it is named there.
`scripts/install-hooks.sh` adds pre-commit and pre-push hooks that run
`scripts/check_clean.sh`, which rejects game and firmware file types, binaries,
large files, code lifted from the game, and anything that looks like a
credential.

## Credits

See [CREDITS.md](CREDITS.md). ps3recomp is by sp00nznet and its contributors.
The GPU ambient occlusion and lighting follow the reverse engineering published
in ttmx's rpcs3-infamous2.

## License

The code in this repository is under the MIT license ([LICENSE](LICENSE)).
inFAMOUS 2 is © Sony Interactive Entertainment / Sucker Punch Productions. This
project is not affiliated with or endorsed by them.
