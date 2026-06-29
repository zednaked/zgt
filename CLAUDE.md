# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

**ZeDs Godot Terminal (ZGT)** — a Godot 4 GDExtension (C++) providing a built-in terminal emulator in the editor's bottom panel. The native `ZGTerminal` Control runs a real shell on a PTY and renders the character grid itself, so it works identically under X11 and Wayland. Linux only.

> History: this started as a Kitty-window-embedding hack (reparenting an X11 `kitty` window into the editor). That is unreliable on Wayland compositors (e.g. Hyprland) because XWayland clients can't position their own windows, so it was replaced by a self-contained PTY terminal. The whole project was then renamed to "ZGT" (display name "ZeDs Godot Terminal"). The `demo/` directory is a ready-to-use test Godot project.

## Build & Setup

```bash
./setup.sh                          # Clones godot-cpp and checks out the 4.3 branch (one-time)
scons platform=linux                # Build -> bin/libzgt.linux.template_debug.x86_64.so
scons platform=linux target=template_release   # Release build
```

`scons` may not be on PATH here. Manual build (godot-cpp must already be compiled):

```bash
g++ -std=c++17 -fPIC -shared -O2 -DLINUX_ENABLED \
  -Isrc -Igodot-cpp/include -Igodot-cpp/gen/include -Igodot-cpp/gdextension \
  src/*.cpp godot-cpp/bin/libgodot-cpp.linux.template_debug.x86_64.a -lutil \
  -o bin/libzgt.linux.template_debug.x86_64.so
```

No automated tests. To verify: build, then copy `bin/*.so` into `demo/bin/`, open `demo/project.godot` in the editor, enable the **"ZeDs Godot Terminal"** plugin (Project Settings → Plugins), click the **"ZGT"** bottom-panel tab and type into the shell. On Wayland, launch the editor with `--display-driver x11` or native; both work since nothing depends on the windowing system. Test a non-default shell with `SHELL=/usr/bin/fish godot --editor ...`.

`bin/`, `godot-cpp/`, and `.godot/` are gitignored. `godot-cpp` is pinned to branch `4.3` even though `compatibility_minimum` is `4.2`.

## Naming map (everything is "zgt")

| Thing | Value |
|---|---|
| Native class | `ZGTerminal` (`src/zgterminal.cpp/.h`) |
| Public methods | `start_terminal()`, `stop_terminal()` |
| Library | `libzgt` |
| Extension file | `zgt.gdextension`, entry symbol `zgt_library_init` |
| Addon dir | `addons/zgt/` (plugin "ZeDs Godot Terminal") |
| Panel tab | "ZGT" |

## Architecture

Two cooperating layers:

1. **GDScript editor plugin** (`addons/zgt/plugin.gd`) — an `@tool EditorPlugin`. On `_enter_tree` it loads the GDExtension via `GDExtensionManager` (path `res://zgt.gdextension`), instantiates `ZGTerminal` through `ClassDB.instantiate()`, wraps it in a panel (the panel gets `custom_minimum_size.y = 300` so it opens at a usable height, plus a "Restart" button calling `stop_terminal()`/`start_terminal()`), and registers it as the "ZGT" bottom-panel control.

2. **Native `ZGTerminal` class** (`src/zgterminal.cpp/.h`) — a `Control` that is a small terminal emulator:
   - **PTY/shell**: `_start_shell()` (on `NOTIFICATION_READY`) sizes the grid from the Control's pixel size, then `forkpty()`s and `execlp`s `$SHELL -i` with `TERM=xterm-256color`, `COLORTERM=truecolor`. Master fd is non-blocking.
   - **Read loop**: `set_process(true)` → `NOTIFICATION_PROCESS` drains the master fd each frame and feeds bytes to the parser; `queue_redraw()` on change.
   - **Parser** (`_feed`): a byte state machine (`ST_NORMAL/ESC/CSI/OSC/...`) with UTF-8 decoding. Handles a practical VT/ANSI subset: cursor moves, erase (ED/EL), insert/delete lines & chars, scroll region (DECSTBM), SGR (16/256-color **and 24-bit truecolor**, bold/inverse/underline), alt screen (`?1049`/`?47`), cursor visibility (`?25`), bracketed paste (`?2004`), application cursor keys (DECCKM `?1`). It **replies** to host queries — DSR cursor-position (`ESC[6n`) and Device Attributes (`ESC[c`) — which is required or shells like **fish** hang/misrender.
   - **Grid model**: `primary`/`alt` are `std::vector<TermCell>`; `active()` selects the current one. `TermCell` = glyph + `int32_t` fg/bg + flags. Color encoding: `-1` default, `0..255` palette, or `ZGT_TRUECOLOR | 0xRRGGBB`. `_ansi_color()` resolves it.
   - **Scrollback**: when a line scrolls off the top of the primary screen, `_scroll_up()` pushes it into a `std::deque` (`scrollback_max` lines). `scroll_offset` is how many lines the view is scrolled up; the renderer maps each screen row to a "virtual index" across scrollback+grid (`_viewport_top_vi()`). Mouse wheel scrolls the view on the primary screen, or sends arrow keys on the alt screen.
   - **Selection/clipboard**: left-drag sets a selection (stored as virtual line + column); on release `_copy_selection()` writes to the system clipboard via `DisplayServer`. `Ctrl+Shift+C` copies, `Ctrl+Shift+V` / middle-click paste (wrapped in bracketed-paste markers when the mode is on).
   - **Rendering** (`NOTIFICATION_DRAW`): background, then per visible row (via the virtual mapping) per-cell bg rect + selection tint + glyph (`draw_char`) using a monospace `FontFile` from a hardcoded candidate list (`kFontCandidates`, falls back to theme font). Cursor is a filled/outlined block, drawn only when at the live bottom (`scroll_offset == 0`).
   - **Input** (`_gui_input`, must be `public` for the virtual to register): `InputEventKey` → byte sequences (Ctrl+letter control codes, `ESC[`/`ESCO` arrows depending on DECCKM, UTF-8 text); typing snaps the view to bottom. Mouse handled here too.
   - **Resize** (`NOTIFICATION_RESIZED`): `_recompute_grid()` reflows cols/rows, preserves the top-left region, pushes new size via `TIOCSWINSZ`.

   All native logic is gated behind `#ifdef LINUX_ENABLED` (defined in `SConstruct`, which links `util` for `forkpty`). Nothing uses X11/Wayland APIs.

### Extension registration

`src/register_types.cpp` is the entry point. `zgt_library_init` (the `entry_symbol` in `zgt.gdextension`) registers `ZGTerminal` at `MODULE_INITIALIZATION_LEVEL_SCENE`. New native classes go in `zgt_initialize_types()` with sources under `src/` (auto-globbed by `SConstruct`). Note: a class overriding a virtual like `_gui_input` must declare it `public` or godot-cpp's `register_virtuals` fails to compile.

## Known limitations / next ideas

Truecolor, scrollback+wheel, selection/copy, bracketed paste, and fish-compat queries are done. Not yet: scrollback search, configurable font/size/theme, `sixel`/image protocols, OSC 52 clipboard, reflow of scrollback on resize (only the live grid reflows), and the dir/cosmetic names still saying "kitty".
