# ZGT — ZeDs Godot Terminal (source)

A Godot 4 GDExtension (C++) that adds a real, built-in terminal to the editor's bottom panel. It runs your shell (`$SHELL`) on a pseudoterminal and renders the character grid itself, so it works identically on **X11 and Wayland**. Good enough to run full TUIs like `claude`, `nvim`, `htop` and `lazygit`.

![Godot 4.2+](https://img.shields.io/badge/Godot-4.2%2B-478cbf) ![Linux](https://img.shields.io/badge/Linux-only-fcc624)

![ZGT in the Godot editor](screenshot.png)

> This is the **source** repo. Prebuilt binaries and install instructions live in **[zgt-bin](https://github.com/zednaked/zgt-bin)** (public).

| Tabs | Scrollback search | Configuration |
|---|---|---|
| ![tabs](screenshot-tabs.png) | ![search](screenshot-search.png) | ![config](screenshot-config.png) |

## Features

- Real PTY shell (`forkpty` + `$SHELL -i`), starts in the project root (`res://`)
- Practical VT/ANSI parser: cursor/erase/scroll-region, SGR (16/256/**truecolor**), alt-screen, bracketed paste, DSR/DA replies (fish-compatible)
- **Multiple terminals in tabs**, each with its OSC window title
- **Mouse tracking** (`?1000/1002/1003` + SGR `?1006`), Shift = local selection
- **Scrollback search** (`Ctrl+Shift+F`), **smart selection** (double=word, triple=line), **Ctrl+click URLs**
- **Configurable** font / size / colors / 16-color palette (Project Settings), live font zoom
- Cursor shapes (DECSCUSR), OSC 52 clipboard, focus reporting

## Build

```bash
./setup.sh                                      # clone godot-cpp (4.3 branch), one-time
scons platform=linux                            # -> bin/libzgt.linux.template_debug.x86_64.so
scons platform=linux target=template_release    # release build
```

Then drop the `.so` into a project's `bin/` next to `zgt.gdextension`, enable the **ZeDs Godot Terminal** plugin, and open the **ZGT** bottom-panel tab. See [`CLAUDE.md`](CLAUDE.md) for architecture details.

A test project lives in [`demo/`](demo/).

## License

MIT
