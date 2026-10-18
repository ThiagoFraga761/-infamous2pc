# Mods

The port loads player mods from the `mods/` folder next to `disc/` (or the folder
in `PS3_MODS_DIR`). Turn them on or off in the **Mods** tab of the launcher or of
the in-game menu (F1). They take effect the next time the game opens.

## Layout

```
mods/
  my-mod/
    mod.ini          optional
    files/           mirrors the game's PS3_GAME/USRDIR
      movies/...
      ui/...
      cache/psarc/...
```

`mod.ini` (all keys optional; without it the folder name is shown):

```ini
name = Better Intro
author = you
version = 1.0
description = Replaces the opening video.
```

## How files are replaced

When the game opens a file under its `USRDIR/` (the disc's, or the installed copy
under `hdd0/game/<id>/USRDIR`), the port looks for the same
relative path under `files/` of every enabled mod. The last enabled mod in the
list wins, and the disc's file is used when no mod has it. The log says which
files came from a mod (`[fs] mod file: ...`).

Only whole files are replaced. Most of the game's data lives inside `.psarc`
archives (`cache/psarc/`), so a mod that changes something inside one ships the
whole rebuilt archive. Loose files such as the Bink videos in `movies/` can be
replaced one by one.

## Picture options

The **Mods** tab also holds the options that change the picture beyond what the
PS3 drew: full-resolution particles and the anisotropic filter. They are stored
in the `[mods]` section of `user/settings.ini`, next to the list of enabled mods.

## Not yet

- Texture replacement without rebuilding archives (needs texture dumping and
  hashing in the renderer).
- Shader or post-processing mods.
- Code patches: the game's code is recompiled ahead of time, so patches to its
  machine code do not apply; data patches would need their own mechanism.
