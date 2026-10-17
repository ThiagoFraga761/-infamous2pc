# Launcher and in-game menu: plan

Design: `rpi/launcher/REQUEST.md`. Each phase ends with a check that proves
it, a commit, and nothing of the next phase half done.

## Research (2026-10-17)

- **ImGui.** v1.91.5 (MIT), cloned into the toolkit at
  `libs/thirdparty/imgui`.
  - It is not put in the patch: `scripts/setup.sh` clones the same tag, and
    `make_patch.sh` excludes the directory.
  - Backends: `imgui_impl_sdl2` (input, gamepad) and `imgui_impl_vulkan`. The
    latter builds without prototypes (`IMGUI_IMPL_VULKAN_NO_PROTOTYPES`) and
    takes our dlopen'd loader through `ImGui_ImplVulkan_LoadFunctions`.
- **Threads.** The rsx thread (`frame_clock` in `work/port/main.cpp`)
  creates the SDL window in the backend init and pumps its events
  (`rsx_vulkan_backend_pump_messages`). The window present runs on that
  thread, or on the recording thread with `PS3RECOMP_BE_THREAD=1`.
  - So the ImGui frame is built on the rsx thread: SDL calls stay on the
    window's thread.
  - The presenting thread records the draw data into the swapchain image,
    from a copy taken under a lock.
- **Swapchain.** Created with `VK_IMAGE_USAGE_TRANSFER_DST_BIT` only
  (`vk_create_swapchain`), and the frame is blitted in (`vk_window_record`,
  `vk_present_window`).
  - The UI needs `COLOR_ATTACHMENT` on top of that, a render pass with
    `LOAD_OP_LOAD`, and one framebuffer per swapchain image, made again
    with the swapchain.
- **Files.** The guest path becomes a host path in `host_path()`
  (`runtime/ppu/ppu_fs.cpp`), which every open and stat goes through. The
  voice and text redirect goes there, so the sizes a stat reports match the
  file that is opened.
- **Settings that are read once.**
  - `PS3RECOMP_BE_THREAD` is read when the engine registers its backend, in
    the backend init, before any UI exists.
  - `PS3RECOMP_RES_SCALE` and `PS3_SYS_LANG` are read lazily, but nothing
    guarantees it.
  - So the start screen applies a changed restart-required option by
    starting the process again (`execv` with `PS3_SKIP_LAUNCHER=1`), not by
    hoping nothing has read it yet.

## Phases

### 1. Settings and the voice/text redirect (no UI)

- `work/port/settings.{h,cpp}`:
  - `settings_load()` at the top of `main()`, before the rsx thread starts.
  - INI sections `[video]`, `[language]`, `[performance]`, `[launcher]`.
  - Each value is exported to its variable unless the environment already
    has it.
  - `settings_save()` is used by the UI.
- `host_path()`: `PS3_VOICE_LANG` / `PS3_TEXT_LANG` redirect
  `cache/psarc/<lang>.psarc` and `<lang>_text.psarc`.
  - A missing target keeps the original and is logged once.
- **Check:**
  - `work/port/tests/settings_test.cpp`: round trip, defaults, the
    environment wins, a corrupt file falls back to defaults.
  - A boot with `PS3_VOICE_LANG=english PS3_TEXT_LANG=brazilian` logs
    `english.psarc` and `brazilian_text.psarc`.
  - The heavy view still runs at its rate (no regression).

### 2. ImGui on the window (the FPS counter only)

- **Build.** ImGui and its two backends build into the runtime library,
  Vulkan builds only.
- **Swapchain:** `COLOR_ATTACHMENT` when the surface supports it; the
  render pass and framebuffers are made with the swapchain and made again
  with it.
- **`ui_frame()` on the rsx thread, once per pump:**
  - `ImGui_ImplSDL2_NewFrame`, the widgets, `ImGui::Render`;
  - a deep copy of `ImDrawData` under `s_ui_lock`.
- **Window present** (both `vk_window_record` and `vk_present_window`):
  after the blit, if there is a copy, a render pass over the swapchain
  image and `ImGui_ImplVulkan_RenderDrawData`.
- **FPS counter:** `PS3RECOMP_UI_FPS=1`, top right.
- **`PS3_UI_DUMP=<file>`:** the UI drawn into an offscreen image and written
  as a PPM, so the UI can be checked without a person.
- **Check:**
  - The counter is visible in a `PS3_UI_DUMP`.
  - The heavy view is not slower with the counter off.
  - No validation errors with `VK_LAYER_KHRONOS_validation` for a short run.

### 3. The in-game menu

- **Opening:** F1, or Select+Start held 0.5 s, toggles the menu.
  - While open, `cellPad` reports a neutral pad (`g_ui_capture`), and ImGui
    gets the controller (gamepad navigation).
- **Pages:** Video, Language, Performance, Controller and saves, with the
  options of the design.
  - Restart-required rows say so.
  - Every change calls `settings_save()`.
- **Live options:**
  - full screen: `SDL_SetWindowFullscreen`;
  - VSync: `vk_set_present_mode`, which recreates the swapchain;
  - aspect: the `vk_window_fit` flag;
  - FPS limit: a wait before the present;
  - counter.
- **Actions:**
  - open the saves or logs folder (`xdg-open`);
  - back up the saves (a dated copy under `user/backups/`).
- **Check:**
  - A `PS3_UI_DUMP` of each page.
  - A boot that opens the menu through `PS3_PAD_FILE` (Select+Start) and
    sees the pad go neutral.
  - Thiago, with a controller.

### 4. The start screen

- **Before the guest boots.** In `frame_clock`, after the backend init and
  before the FIFO loop, the start screen runs the same UI with a "Play"
  button:
  - a dark gradient background;
  - the last game frame, if one was saved at the previous exit
    (`user/last_frame.ppm`).
- **Waiting.** `main()` waits on an event "Play" sets before `ppu_run`.
- **Restart-required options.** If one changed on the start screen, "Play"
  saves and calls `execv` on the process with `PS3_SKIP_LAUNCHER=1`.
- **Skipping.** `--play`, `PS3_SKIP_LAUNCHER=1`, no window, or "skip this
  screen" skip the start screen.
- **Check:**
  - A boot that presses "Play" through `PS3_PAD_FILE` reaches the game.
  - A boot that changes the resolution first comes back at 1440p.
  - The tests and benchmarks (no window) are unchanged.

### 5. Polish and documentation

- **Theme:** Inter through fontconfig, colours, spacing, the controller
  glyphs in the hints.
- **Docs:** the README's "How to play" gains the screens (screenshots from
  `PS3_UI_DUMP`), and `play.sh` drops the variables the settings now hold.
- **Review:** a code review in a separate context, then commit and push.

## Rollback

- `PS3_SKIP_LAUNCHER=1` and no `PS3RECOMP_UI_FPS` give today's behaviour.
- Deleting `user/settings.ini` gives the defaults.
