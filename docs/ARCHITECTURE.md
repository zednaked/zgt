# Architecture

How ZGT is put together, and the decisions that are load-bearing. Written for
anyone who wants to change it without breaking the editor around it.

## Why a PTY and not window embedding

ZGT started as a Kitty-window-embedding hack: reparent an X11 `kitty` window
into the editor's bottom panel. That is unreliable on Wayland compositors,
because XWayland clients cannot position their own windows — the compositor
decides, and the terminal ends up floating somewhere the editor does not
control.

So the embedding was dropped and replaced by a self-contained terminal: run the
shell on a pseudoterminal and render the character grid directly. Nothing in
the native layer touches X11 or Wayland APIs, which is why it behaves
identically under both.

## Architecture


Two cooperating layers:

1. **GDScript editor plugin** (`addons/zgt/plugin.gd`) — an `@tool EditorPlugin`. On `_enter_tree` it registers configurable ProjectSettings under `zgt/terminal/*` (font path/size, fg/bg colors, opacity, 16-color palette), loads the GDExtension via `GDExtensionManager` (path `res://zgt.gdextension`), and builds the panel (`custom_minimum_size.y = 300`): a `TabBar` of terminals with a **"+"** (new tab) and **"Restart"** button, a hidden **search bar** (Ctrl+Shift+F), and a `VBoxContainer` host holding one `ZGTerminal` per tab — only the active tab is visible (hidden children take no space). Each terminal's `title_changed` updates its tab label; `search_requested` opens the search bar (operating on the active terminal). Registered as the "ZGT" bottom-panel control.

2. **Native `ZGTerminal` class** (`src/zgterminal.cpp/.h`) — a `Control` that is a small terminal emulator:
   - **PTY/shell**: `_start_shell()` (on `NOTIFICATION_READY`) sizes the grid from the Control's pixel size, then `forkpty()`s and `execlp`s `$SHELL -i` with `TERM=xterm-256color`, `COLORTERM=truecolor`. The project root is resolved **in the parent** (`ProjectSettings::globalize_path("res://")`) and the child `chdir()`s into it and exports `GODOT_PROJECT_PATH`, so the shell — and tools like `claude` — start inside the project. Master fd is non-blocking. **Child reaping**: there is deliberately **no process-wide `SIGCHLD` handler** — this Control runs inside the Godot editor and a global `waitpid(-1)` would steal the exit status of the editor's own subprocesses (running the game, asset imports). `_stop_shell()` reaps only its own child (`SIGTERM` → ~100ms grace → `SIGKILL` → `waitpid(child_pid)`).
   - **Read loop**: `set_process(true)` → `NOTIFICATION_PROCESS` drains the master fd each frame and feeds bytes to the parser; `queue_redraw()` on change.
   - **Parser** (`_feed`): a byte state machine (`ST_NORMAL/ESC/CSI/OSC/...`) with UTF-8 decoding. Handles a practical VT/ANSI subset: cursor moves, erase (ED/EL), insert/delete lines & chars, scroll region (DECSTBM), SGR (16/256-color **and 24-bit truecolor**, bold/inverse/underline), alt screen (`?1049`/`?47`), cursor visibility (`?25`), bracketed paste (`?2004`), application cursor keys (DECCKM `?1`), mouse tracking (`?1000`/`?1002`/`?1003` + SGR `?1006`), focus reporting (`?1004`). It **replies** to host queries — DSR cursor-position (`ESC[6n`) and Device Attributes (`ESC[c`) — which is required or shells like **fish** hang/misrender. OSC handling (`ESC]…`, BEL- or ST-terminated, capped at 4 KB): titles (`0;`/`1;`/`2;`) are captured into `terminal_title` → `title_changed(String)` signal; OSC `52;c;<base64>` sets the system clipboard. Cursor shape via DECSCUSR (`ESC[Ps q`: block/underline/bar). DECCKM (`app_cursor_keys`) and all the modes above are per-instance members (no globals), so multiple terminals don't interfere.
   - **Grid model**: `primary`/`alt` are `std::vector<TermCell>`; `active()` selects the current one. `TermCell` = glyph + `int32_t` fg/bg + flags. Color encoding: `-1` default, `0..255` palette, or `ZGT_TRUECOLOR | 0xRRGGBB`. `_ansi_color()` resolves it.
   - **Wide characters** (CJK/emoji): `zgt_char_width()` (`src/wcwidth.cpp`, Kuhn `mk_wcwidth` + an emoji table) returns 0/1/2. A width-2 glyph occupies two cells — a lead cell flagged `TF_WIDE` and a placeholder to its right flagged `TF_WIDE_CONT`. `_put_char` writes the pair (and clears the partner when overwriting half of an existing pair); the renderer draws the lead at double width and skips the continuation; copy/search/highlight skip continuation cells so text stays aligned. (Width-0 combining marks are currently dropped.)
   - **Scrollback**: when a line scrolls off the top of the primary screen, `_scroll_up()` pushes it into a `std::deque` (`scrollback_max` lines). `scroll_offset` is how many lines the view is scrolled up; the renderer maps each screen row to a "virtual index" across scrollback+grid (`_viewport_top_vi()`). Mouse wheel scrolls the view on the primary screen, or sends arrow keys on the alt screen.
   - **Selection/clipboard**: left-drag sets a selection (stored as virtual line + column); on release `_copy_selection()` writes to the system clipboard. **Double-click** selects a word (`_select_word`, path/URL-friendly chars), **triple-click** a line (`_select_line`) — tracked via a manual click-count timer. **Ctrl+click** on a URL (`_url_at`) opens it with `OS::shell_open`. `Ctrl+Shift+C` copies, `Ctrl+Shift+V` / middle-click paste (bracketed when the mode is on).
   - **Search**: `search(query, forward)` / `clear_search()` (bound, driven by the plugin's search bar) scan scrollback+grid case-insensitively with wrap-around; `Ctrl+Shift+F` emits `search_requested`. Matches are highlighted in the renderer (current match in orange, others yellow).
   - **Theme** (`_load_theme`, from ProjectSettings): default fg/bg, background opacity, and an optional 16-color palette override feed `_ansi_color()`. Font path/size also come from settings (`_load_font` / `_start_shell`); `Ctrl+Shift+=/-/0` zoom live.
   - **Rendering** (`NOTIFICATION_DRAW`): background, then per visible row (via the virtual mapping) per-cell bg rect + search highlight + selection tint + glyph (`draw_char`); `TF_WIDE` cells span two columns, `TF_WIDE_CONT` cells are skipped. The default font prefers a Nerd Font (`kFontCandidates`) so box-drawing/icons render, and `_load_font` attaches CJK/emoji **fallback fonts** (`kFallbackFonts`, e.g. Noto Sans CJK / Noto Color Emoji) so wide glyphs render instead of tofu — only on a font we own, never the shared editor theme font. Both lists are probed with `zgt_path_exists` first so missing paths don't spam `load_dynamic_font` errors. Cursor shape honors DECSCUSR (block/underline/bar), drawn only at the live bottom (`scroll_offset == 0`).
   - **Input** (`_gui_input`, must be `public` for the virtual to register): `InputEventKey` → byte sequences (Ctrl+letter control codes, `ESC[`/`ESCO` arrows depending on DECCKM, UTF-8 text); typing snaps the view to bottom. `Ctrl+Shift+=`/`-`/`0` zoom the font (`_set_font_size` reloads metrics + reflows). Mouse handled here too: when an app enables mouse tracking, clicks/drag/wheel are encoded (`_mouse_report`, SGR or legacy) and sent to the shell instead of doing local selection — **holding Shift** forces the terminal's own selection/scroll (kitty/xterm convention).
   - **Resize** (`NOTIFICATION_RESIZED`): `_recompute_grid()` reflows cols/rows, preserves the top-left region, pushes new size via `TIOCSWINSZ`.

   All native logic is gated behind `#ifdef LINUX_ENABLED` (defined in `SConstruct`, which links `util` for `forkpty`). Nothing uses X11/Wayland APIs.

### Extension registration

`src/register_types.cpp` is the entry point. `zgt_library_init` (the `entry_symbol` in `zgt.gdextension`) registers `ZGTerminal` at `MODULE_INITIALIZATION_LEVEL_SCENE`. New native classes go in `zgt_initialize_types()` with sources under `src/` (auto-globbed by `SConstruct`). Note: a class overriding a virtual like `_gui_input` must declare it `public` or godot-cpp's `register_virtuals` fails to compile.

## Direction

The goal is a **single, excellent built-in terminal** — not an editor-integration framework. (An MCP-server / scene-tree-introspection angle was considered and deliberately dropped.) Effort goes into terminal quality and compatibility so tools like `claude` run well in the panel.

## Known limitations / next ideas

Done: truecolor, scrollback+wheel, selection/copy, bracketed paste, fish-compat queries, shell starts in the project root, OSC window-title, per-child reaping (no global `SIGCHLD`), mouse tracking (`?1000/1002/1003` + SGR `?1006`, Shift = local selection), focus reporting (`?1004`), font zoom, **configurable font + theme/palette** (ProjectSettings), **cursor shape** (DECSCUSR), **OSC 52 clipboard**, **smart selection** (double=word, triple=line) + **Ctrl+click URLs**, **scrollback search** (`Ctrl+Shift+F`), **multiple terminals in tabs**, and **wide characters** (CJK/emoji double-width via `wcwidth` + CJK/emoji font fallbacks). Not yet: width-0 combining marks (dropped); robust wide-pair fixup in `_delete/_insert/_erase_chars`; bold/italic font faces (bold only brightens the color); `sixel`/image protocols, OSC 8 hyperlinks, reflow of scrollback on resize (only the live grid reflows), persisted per-tab session state; opening `path:line` under the cursor in the editor (considered, deferred).
