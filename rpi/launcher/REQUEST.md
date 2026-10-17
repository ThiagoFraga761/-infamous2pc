# Launcher and in-game menu: design

Agreed with Thiago on 2026-10-17 (approach A of three).

## Problem

Every option of the port is an environment variable in `work/play.sh`
(`PS3RECOMP_RES_SCALE`, `PS3_SYS_LANG`, `PS3RECOMP_VK_PRESENT`, …). A
player cannot see them, change them without editing a script, or pick the
dub and the subtitles separately.

## What it is

One user interface, drawn by the port itself with Dear ImGui (MIT,
vendored into the toolkit) on the Vulkan backend, in two places:

- **Before the game.** The window opens on the options screen, and the game
  boots when the player presses "Play".
- **In the game.** F1, or Select+Start held for half a second, opens the same
  screen over the frame.

The UI works with the keyboard and mouse, and with a controller (ImGui
gamepad navigation).

## Options

Marked "live" when they apply at once; the others apply at the next start,
and the screen says so.

| Section | Option | Values | When |
|---|---|---|---|
| Video | Render resolution | 720p (1x), 1440p (2x), 4K (3x) | next start |
| | Window mode | window, full screen | live |
| | VSync | on (fifo), off (mailbox) | live |
| | Aspect | black bars, stretch | live |
| | Monitor | which display opens the window | next start |
| Language | Voices | the voice packs on the disc: Brazilian Portuguese, English, Latin American Spanish, Spanish, French, German, Italian, European Portuguese, Russian, Polish, Dutch, Turkish, Japanese | next start |
| | Subtitles and text | the same, plus British English, Korean and Chinese (text only on the disc) | next start |
| Performance | Preset | Performance (720p), Balanced (1440p), Quality (4K); each also turns the recording thread on and the FPS limit off | depends on the row |
| | FPS limit | none, 30, 60, 120, the monitor's rate | live |
| | FPS counter on screen | on, off | live |
| | Vulkan recording thread | on, off | next start |
| Controller and saves | Controller | the detected controller's name, a button test | live |
| | Saves | open the folder, back up now (a dated copy) | action |
| | Logs | open the logs folder | action |
| | Start screen | "skip this screen next time" (F1 in game reopens it) | next start |

Left to the game's own options menu: subtitles on/off, brightness, camera
sensitivity.

On the very first start (no `user/settings.ini` yet), the defaults are
today's `play.sh` behaviour: 720p, window, mailbox, black bars, Brazilian
Portuguese voices and text, the recording thread on.

## Components

1. **settings** (`work/port/settings.{h,cpp}`)
   - Reads and writes `user/settings.ini` at the repository root, beside
     `disc/`; the allowlist `.gitignore` keeps it out of git.
   - At start, before anything reads the environment, it turns each value
     into the variable the port already reads.
   - A variable already set in the environment wins over the file, so
     `play.sh`, the tests and the benchmarks behave as before.
2. **ui** (`work/port/ui/`, with ImGui under `libs/thirdparty/imgui` in the
   toolkit patch)
   - The four pages and a theme: dark, accent `#46c8ff`, Inter from the
     system through fontconfig, ImGui's default font as the fallback.
   - The background is a blurred capture of the game, from the screenshots
     the port can already take, or a plain dark gradient.
3. **Start screen** (`main.cpp`)
   - The rsx thread already opens the window before the guest boots. Between
     the backend init and the FIFO loop it runs the UI loop: clear, UI,
     present, at the monitor's rate.
   - The main thread waits on an event that "Play" sets before it calls
     `ppu_run`. Nothing of the game has run yet, so resolution and language
     chosen there take effect in this very boot.
   - `--play` or `PS3_SKIP_LAUNCHER=1` skips the screen, as does no window
     (`PS3RECOMP_VK_WINDOW` unset).
4. **In-game menu** (Vulkan backend)
   - The ImGui frame (new frame, widgets, render) runs where the window
     present runs: on the recording thread when it is on, else on the rsx
     thread. That thread owns the ImGui context.
   - It draws into the swapchain image after the game frame's blit, at the
     window's resolution. The swapchain gains
     `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT` when the surface allows it; if it
     does not, there is no in-game menu and the start screen draws into its
     own image.
5. **Input**
   - The rsx thread's SDL event pump passes keyboard, mouse and controller
     events through a queue to the thread that owns ImGui.
   - While the menu is open, `cellPad` reports a neutral pad, so the game
     does not move.
   - Opening it: F1, or Select+Start held 0.5 s.
6. **Voices and text** (`cellFs` open path)
   - The game opens `cache/psarc/<lang>.psarc` (voices) and
     `cache/psarc/<lang>_text.psarc` (text) for the language it picked.
   - With `PS3_VOICE_LANG` / `PS3_TEXT_LANG` set, the open of any language
     pack is redirected to the chosen pack of that kind, so the game's own
     choice does not matter.
   - Korean and Chinese text are marked experimental: their fonts come in the
     pack and have not been tried.

## Applying a change

- Every change writes `user/settings.ini` at once.
- The live options call the backend:
  - full screen: `SDL_SetWindowFullscreen` (exists);
  - VSync: a new `vk_set_present_mode`, which recreates the swapchain
    through the out-of-date path;
  - aspect: the `vk_window_fit` flag;
  - FPS limit: a wait in the present;
  - the counter: drawn by the UI.
- The others show "restart to apply".

## Failure behaviour

| Failure | What happens |
|---|---|
| `settings.ini` missing or unreadable | Defaults; the file is written again |
| ImGui or the font fails to start | Logged; the game starts directly, as today |
| The chosen language pack is not on the disc | The game's own file opens; logged |
| No window (benchmarks, tests) | No start screen, no menu |

## Testing

- **Settings.** A C test of `settings.ini` reading and writing, defaults, and
  the environment overriding the file.
- **Language.** A boot with English voices and Brazilian text: the log must
  show `english.psarc` and `brazilian_text.psarc` opened.
- **Look.** `PS3_UI_DUMP=<file>` writes the UI's image as a PPM, so the
  screens can be checked without a person at the machine.
- **Controller.** Thiago tries both screens with a real controller.

## Out of scope

- A separate launcher application (Qt, Tk).
- Windows.
- Remapping controls, mouse camera.
- Changing resolution or language without restarting the game once it has
  booted.
