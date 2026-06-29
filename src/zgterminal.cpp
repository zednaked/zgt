#include "zgterminal.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/font_file.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/marshalls.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifdef LINUX_ENABLED
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace godot {

// ---- parser states ----
enum {
    ST_NORMAL = 0,
    ST_ESC,
    ST_CSI,
    ST_OSC,
    ST_OSC_ESC,
    ST_CHARSET,
};

static const char *kFontCandidates[] = {
    // Prefer a Nerd Font: full box-drawing + Powerline + icon glyphs, so TUIs
    // like `claude` render their borders and symbols correctly. "Mono" variant
    // keeps every glyph (icons included) at a single cell width.
    "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
    "/usr/share/fonts/jetbrains-mono-nerd/JetBrainsMonoNerdFontMono-Regular.ttf",
    "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMonoNerdFontMono-Regular.ttf",
    "/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf",
    "/usr/share/fonts/TTF/CaskaydiaCoveNerdFontMono-Regular.ttf",
    // Plain monospace fallbacks (no icon/box-drawing guarantees).
    "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf",
    "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
    nullptr,
};

ZGTerminal::ZGTerminal()
    : master_fd(-1),
      child_pid(0),
      cols(80),
      rows(24),
      using_alt(false),
      scrollback_max(5000),
      scroll_offset(0),
      selecting(false), has_sel(false),
      sel_a_line(0), sel_a_col(0), sel_b_line(0), sel_b_col(0),
      last_click_msec(0), last_click_vi(-1), last_click_col(-1), click_count(0),
      cur_x(0), cur_y(0),
      saved_x(0), saved_y(0),
      cursor_visible(true), cursor_style(0),
      cur_fg(-1), cur_bg(-1), cur_flags(0),
      app_cursor_keys(false), bracketed_paste(false),
      mouse_report(0), mouse_sgr(false), focus_report(false),
      scroll_top(0), scroll_bottom(23),
      parse_state(ST_NORMAL),
      search_vi(-1),
      csi_cur(0), csi_has_digit(false), csi_private(false),
      utf8_remaining(0), utf8_acc(0),
      font_size(14), cell_w(8.0f), cell_h(16.0f), ascent(12.0f),
      cfg_bg(0.11f, 0.11f, 0.13f), cfg_fg(0.85f, 0.85f, 0.85f),
      cfg_opacity(1.0f), has_palette(false) {
    set_focus_mode(FOCUS_ALL);
    set_clip_contents(true);
    set_default_cursor_shape(CURSOR_IBEAM);
}

ZGTerminal::~ZGTerminal() {
    _stop_shell();
}

void ZGTerminal::_bind_methods() {
    ClassDB::bind_method(D_METHOD("start_terminal"), &ZGTerminal::start_terminal);
    ClassDB::bind_method(D_METHOD("stop_terminal"), &ZGTerminal::stop_terminal);
    ClassDB::bind_method(D_METHOD("search", "query", "forward"), &ZGTerminal::search);
    ClassDB::bind_method(D_METHOD("clear_search"), &ZGTerminal::clear_search);

    ADD_SIGNAL(MethodInfo("title_changed", PropertyInfo(Variant::STRING, "title")));
    ADD_SIGNAL(MethodInfo("search_requested"));
}

// ---------- grid helpers ----------

TermCell ZGTerminal::blank_cell() const {
    TermCell c;
    c.ch = ' ';
    c.fg = cur_fg;
    c.bg = cur_bg;
    c.flags = 0;
    return c;
}

std::vector<TermCell> &ZGTerminal::active() {
    return using_alt ? alt : primary;
}

TermCell &ZGTerminal::cell(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= cols) x = cols - 1;
    if (y >= rows) y = rows - 1;
    return active()[y * cols + x];
}

// ---------- font / grid sizing ----------

void ZGTerminal::_load_font() {
    font.unref();

    // A user-configured font path (ProjectSettings "zgt/terminal/font_path")
    // takes priority over the built-in candidates.
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (ps && ps->has_setting("zgt/terminal/font_path")) {
        String p = ps->get_setting("zgt/terminal/font_path");
        if (!p.is_empty()) {
            if (p.begins_with("res://") || p.begins_with("user://")) p = ps->globalize_path(p);
            Ref<FontFile> f;
            f.instantiate();
            if (f->load_dynamic_font(p) == OK) font = f;
        }
    }

    for (int i = 0; font.is_null() && kFontCandidates[i]; i++) {
        Ref<FontFile> f;
        f.instantiate();
        if (f->load_dynamic_font(kFontCandidates[i]) == OK) {
            font = f;
            break;
        }
    }
    if (font.is_null()) {
        font = get_theme_default_font();
    }
    if (font.is_valid()) {
        cell_w = font->get_string_size("M", (HorizontalAlignment)0, -1, font_size).x;
        cell_h = font->get_height(font_size);
        ascent = font->get_ascent(font_size);
        if (cell_w < 1.0f) cell_w = 8.0f;
        if (cell_h < 1.0f) cell_h = 16.0f;
    }
}

void ZGTerminal::_load_theme() {
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!ps) return;
    if (ps->has_setting("zgt/terminal/background_color"))
        cfg_bg = ps->get_setting("zgt/terminal/background_color");
    if (ps->has_setting("zgt/terminal/foreground_color"))
        cfg_fg = ps->get_setting("zgt/terminal/foreground_color");
    if (ps->has_setting("zgt/terminal/background_opacity"))
        cfg_opacity = std::max(0.0f, std::min(1.0f, (float)(double)ps->get_setting("zgt/terminal/background_opacity")));
    has_palette = false;
    if (ps->has_setting("zgt/terminal/palette")) {
        PackedColorArray pal = ps->get_setting("zgt/terminal/palette");
        if (pal.size() == 16) {
            for (int i = 0; i < 16; i++) cfg_palette[i] = pal[i];
            has_palette = true;
        }
    }
}

void ZGTerminal::_set_font_size(int s) {
    s = std::max(6, std::min(48, s));
    if (s == font_size) return;
    font_size = s;
    _load_font();      // re-derives cell_w/cell_h/ascent from the new size
    _recompute_grid(); // reflow cols/rows + push the new winsize
    queue_redraw();
}

void ZGTerminal::_recompute_grid() {
    Vector2 sz = get_size();
    int new_cols = std::max(1, (int)(sz.x / cell_w));
    int new_rows = std::max(1, (int)(sz.y / cell_h));
    if (new_cols == cols && new_rows == rows &&
            (int)primary.size() == cols * rows) {
        return;
    }

    std::vector<TermCell> np(new_cols * new_rows, blank_cell());
    std::vector<TermCell> na(new_cols * new_rows, blank_cell());
    int copy_cols = std::min(cols, new_cols);
    int copy_rows = std::min(rows, new_rows);
    if ((int)primary.size() == cols * rows) {
        for (int y = 0; y < copy_rows; y++) {
            for (int x = 0; x < copy_cols; x++) {
                np[y * new_cols + x] = primary[y * cols + x];
                na[y * new_cols + x] = alt[y * cols + x];
            }
        }
    }
    primary.swap(np);
    alt.swap(na);

    cols = new_cols;
    rows = new_rows;
    scroll_top = 0;
    scroll_bottom = rows - 1;
    cur_x = std::min(cur_x, cols - 1);
    cur_y = std::min(cur_y, rows - 1);

    _set_winsize();
}

void ZGTerminal::_set_winsize() {
#ifdef LINUX_ENABLED
    if (master_fd < 0) return;
    struct winsize ws;
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    ioctl(master_fd, TIOCSWINSZ, &ws);
#endif
}

// ---------- shell lifecycle ----------

void ZGTerminal::_start_shell() {
#ifdef LINUX_ENABLED
    if (master_fd >= 0) return;

    // Fresh shell: drop any sticky terminal modes left over from a prior session.
    mouse_report = 0;
    mouse_sgr = false;
    focus_report = false;
    bracketed_paste = false;
    app_cursor_keys = false;
    cursor_style = 0;

    // Apply the configured default font size (live zoom still overrides per session).
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (ps && ps->has_setting("zgt/terminal/font_size")) {
        int fs = (int)ps->get_setting("zgt/terminal/font_size");
        if (fs >= 6 && fs <= 48) font_size = fs;
    }

    _load_theme();
    _load_font();
    _recompute_grid();

    struct winsize ws;
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    // Resolve the project root in the parent (calling into the engine from the
    // forked child would be unsafe) so the shell — and tools like `claude` — start
    // inside the project.
    CharString project_root;
    if (ProjectSettings::get_singleton()) {
        project_root = ProjectSettings::get_singleton()->globalize_path("res://").utf8();
    }

    int fd = -1;
    int pid = forkpty(&fd, nullptr, nullptr, &ws);
    if (pid < 0) {
        UtilityFunctions::print("ZGTerminal: forkpty failed");
        return;
    }
    if (pid == 0) {
        if (project_root.length() > 0) {
            (void)chdir(project_root.get_data());
            setenv("GODOT_PROJECT_PATH", project_root.get_data(), 1);
        }
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        const char *shell = getenv("SHELL");
        if (!shell || !*shell) shell = "/bin/bash";
        execlp(shell, shell, "-i", (char *)nullptr);
        _exit(127);
    }

    child_pid = pid;
    master_fd = fd;
    fcntl(master_fd, F_SETFL, O_NONBLOCK);

    set_process(true);
    queue_redraw();
#endif
}

void ZGTerminal::_stop_shell() {
#ifdef LINUX_ENABLED
    set_process(false);
    // Close the master first so the shell sees EOF/SIGHUP and starts exiting.
    if (master_fd >= 0) {
        close(master_fd);
        master_fd = -1;
    }
    // Reap only our own child. We deliberately do NOT install a process-wide
    // SIGCHLD handler: this Control runs inside the Godot editor, and a global
    // waitpid(-1) would steal the exit status of the editor's own subprocesses
    // (running the game, asset imports, ...).
    if (child_pid > 0) {
        kill(child_pid, SIGTERM);
        bool reaped = false;
        for (int i = 0; i < 100 && !reaped; i++) { // wait up to ~100ms
            if (waitpid(child_pid, nullptr, WNOHANG) != 0) reaped = true;
            else usleep(1000);
        }
        if (!reaped) {
            kill(child_pid, SIGKILL);
            waitpid(child_pid, nullptr, 0);
        }
        child_pid = 0;
    }
#endif
}

void ZGTerminal::_read_pty() {
#ifdef LINUX_ENABLED
    if (master_fd < 0) return;
    uint8_t buf[8192];
    bool changed = false;
    for (;;) {
        ssize_t n = read(master_fd, buf, sizeof(buf));
        if (n > 0) {
            _feed(buf, (int)n);
            changed = true;
            continue;
        }
        if (n == 0) {
            _stop_shell();
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        _stop_shell();
        break;
    }
    if (changed) {
        if (scroll_offset > (int)scrollback.size()) scroll_offset = scrollback.size();
        queue_redraw();
    }
#endif
}

// ---------- escape-sequence parser ----------

void ZGTerminal::_feed(const uint8_t *data, int len) {
    for (int i = 0; i < len; i++) {
        uint8_t b = data[i];

        switch (parse_state) {
            case ST_NORMAL: {
                if (utf8_remaining > 0) {
                    if ((b & 0xC0) == 0x80) {
                        utf8_acc = (utf8_acc << 6) | (b & 0x3F);
                        if (--utf8_remaining == 0) _put_char(utf8_acc);
                    } else {
                        utf8_remaining = 0;
                        i--;
                    }
                    break;
                }
                if (b == 0x1B) {
                    parse_state = ST_ESC;
                } else if (b == '\n') {
                    _line_feed();
                } else if (b == '\r') {
                    _carriage_return();
                } else if (b == '\b') {
                    _backspace();
                } else if (b == '\t') {
                    _tab();
                } else if (b == 0x07) {
                    // bell
                } else if (b < 0x20) {
                    // other control, ignore
                } else if (b < 0x80) {
                    _put_char(b);
                } else if ((b & 0xE0) == 0xC0) {
                    utf8_acc = b & 0x1F; utf8_remaining = 1;
                } else if ((b & 0xF0) == 0xE0) {
                    utf8_acc = b & 0x0F; utf8_remaining = 2;
                } else if ((b & 0xF8) == 0xF0) {
                    utf8_acc = b & 0x07; utf8_remaining = 3;
                }
            } break;

            case ST_ESC: {
                if (b == '[') {
                    csi_params.clear();
                    csi_cur = 0;
                    csi_has_digit = false;
                    csi_private = false;
                    parse_state = ST_CSI;
                } else if (b == ']') {
                    osc_buf.clear();
                    parse_state = ST_OSC;
                } else if (b == '7') {
                    saved_x = cur_x; saved_y = cur_y; parse_state = ST_NORMAL;
                } else if (b == '8') {
                    cur_x = saved_x; cur_y = saved_y; parse_state = ST_NORMAL;
                } else if (b == 'M') {
                    _reverse_index(); parse_state = ST_NORMAL;
                } else if (b == '(' || b == ')' || b == '*' || b == '+') {
                    parse_state = ST_CHARSET;
                } else {
                    parse_state = ST_NORMAL;
                }
            } break;

            case ST_CHARSET: {
                parse_state = ST_NORMAL;
            } break;

            case ST_CSI: {
                if (b >= '0' && b <= '9') {
                    csi_cur = csi_cur * 10 + (b - '0');
                    csi_has_digit = true;
                } else if (b == ';') {
                    csi_params.push_back(csi_has_digit ? csi_cur : -1);
                    csi_cur = 0;
                    csi_has_digit = false;
                } else if (b == '?' || b == '>' || b == '!') {
                    csi_private = true;
                } else if (b >= 0x40 && b <= 0x7E) {
                    csi_params.push_back(csi_has_digit ? csi_cur : -1);
                    _handle_csi((char)b);
                    parse_state = ST_NORMAL;
                }
            } break;

            case ST_OSC: {
                if (b == 0x07) {
                    _handle_osc();
                    parse_state = ST_NORMAL;
                } else if (b == 0x1B) {
                    parse_state = ST_OSC_ESC;
                } else if (osc_buf.size() < 4096) {
                    osc_buf.push_back((char)b);
                }
            } break;

            case ST_OSC_ESC: {
                // ST terminator is ESC '\'; anything else just ends the OSC.
                if (b == '\\') _handle_osc();
                parse_state = ST_NORMAL;
            } break;
        }
    }
}

void ZGTerminal::_put_char(char32_t c) {
    if (cur_x >= cols) {
        cur_x = 0;
        _line_feed();
    }
    TermCell &cl = cell(cur_x, cur_y);
    cl.ch = c;
    cl.fg = cur_fg;
    cl.bg = cur_bg;
    cl.flags = cur_flags;
    cur_x++;
}

void ZGTerminal::_line_feed() {
    if (cur_y == scroll_bottom) {
        _scroll_up(1);
    } else if (cur_y < rows - 1) {
        cur_y++;
    }
}

void ZGTerminal::_reverse_index() {
    if (cur_y == scroll_top) {
        _scroll_down(1);
    } else if (cur_y > 0) {
        cur_y--;
    }
}

void ZGTerminal::_carriage_return() { cur_x = 0; }

void ZGTerminal::_backspace() { if (cur_x > 0) cur_x--; }

void ZGTerminal::_tab() {
    cur_x = ((cur_x / 8) + 1) * 8;
    if (cur_x >= cols) cur_x = cols - 1;
}

void ZGTerminal::_push_scrollback_line(int grid_row) {
    std::vector<TermCell> &g = active();
    std::vector<TermCell> line(g.begin() + grid_row * cols, g.begin() + grid_row * cols + cols);
    scrollback.push_back(std::move(line));
    if (scroll_offset > 0) scroll_offset++;
    while ((int)scrollback.size() > scrollback_max) {
        scrollback.pop_front();
        if (scroll_offset > 0) scroll_offset--;
    }
    if (scroll_offset > (int)scrollback.size()) scroll_offset = scrollback.size();
}

void ZGTerminal::_scroll_up(int n) {
    if (n <= 0) return;
    std::vector<TermCell> &g = active();
    // Lines that scroll off the top of the primary full screen go to scrollback.
    if (!using_alt && scroll_top == 0) {
        for (int k = 0; k < n && k < rows; k++) _push_scrollback_line(k);
    }
    for (int y = scroll_top; y <= scroll_bottom; y++) {
        int src = y + n;
        for (int x = 0; x < cols; x++) {
            g[y * cols + x] = (src <= scroll_bottom) ? g[src * cols + x] : blank_cell();
        }
    }
}

void ZGTerminal::_scroll_down(int n) {
    if (n <= 0) return;
    std::vector<TermCell> &g = active();
    for (int y = scroll_bottom; y >= scroll_top; y--) {
        int src = y - n;
        for (int x = 0; x < cols; x++) {
            g[y * cols + x] = (src >= scroll_top) ? g[src * cols + x] : blank_cell();
        }
    }
}

void ZGTerminal::_handle_csi(char final) {
    auto getp = [&](int i, int def) -> int {
        if (i < (int)csi_params.size() && csi_params[i] > 0) return csi_params[i];
        return def;
    };
    int p0 = getp(0, 1);

    switch (final) {
        case 'A': cur_y = std::max(0, cur_y - p0); break;
        case 'B': cur_y = std::min(rows - 1, cur_y + p0); break;
        case 'C': cur_x = std::min(cols - 1, cur_x + p0); break;
        case 'D': cur_x = std::max(0, cur_x - p0); break;
        case 'E': cur_y = std::min(rows - 1, cur_y + p0); cur_x = 0; break;
        case 'F': cur_y = std::max(0, cur_y - p0); cur_x = 0; break;
        case 'G': case '`': cur_x = std::min(cols - 1, std::max(0, p0 - 1)); break;
        case 'd': cur_y = std::min(rows - 1, std::max(0, p0 - 1)); break;
        case 'H': case 'f': {
            cur_y = std::min(rows - 1, std::max(0, getp(0, 1) - 1));
            cur_x = std::min(cols - 1, std::max(0, getp(1, 1) - 1));
        } break;
        case 'J': _erase_display(getp(0, 0)); break;
        case 'K': _erase_line(getp(0, 0)); break;
        case 'L': _insert_lines(p0); break;
        case 'M': _delete_lines(p0); break;
        case 'P': _delete_chars(p0); break;
        case '@': _insert_chars(p0); break;
        case 'X': _erase_chars(p0); break;
        case 'S': _scroll_up(p0); break;
        case 'T': _scroll_down(p0); break;
        case 'm': _handle_sgr(); break;
        case 'r': {
            scroll_top = std::max(0, getp(0, 1) - 1);
            scroll_bottom = std::min(rows - 1, getp(1, rows) - 1);
            if (scroll_top >= scroll_bottom) { scroll_top = 0; scroll_bottom = rows - 1; }
            cur_x = 0; cur_y = scroll_top;
        } break;
        case 'q': // DECSCUSR — cursor shape (the SP intermediate is skipped by the parser)
            if (!csi_private) cursor_style = getp(0, 1);
            break;
        case 'h': _set_mode(true); break;
        case 'l': _set_mode(false); break;
        case 's': saved_x = cur_x; saved_y = cur_y; break;
        case 'u': cur_x = saved_x; cur_y = saved_y; break;
        case 'n': { // Device Status Report
            if (!csi_private) {
                int p = getp(0, 0);
                if (p == 6) {
                    _send(String::chr(0x1B) + "[" + String::num_int64(cur_y + 1) +
                        ";" + String::num_int64(cur_x + 1) + "R");
                } else if (p == 5) {
                    _send(String::chr(0x1B) + "[0n");
                }
            }
        } break;
        case 'c': { // Device Attributes
            if (csi_private) {
                _send(String::chr(0x1B) + "[>0;276;0c"); // secondary DA
            } else {
                _send(String::chr(0x1B) + "[?6c");       // primary DA (VT102)
            }
        } break;
        default: break;
    }
}

void ZGTerminal::_handle_osc() {
    // OSC payload is "Ps;Pt".
    size_t sep = osc_buf.find(';');
    if (sep == std::string::npos) return;
    std::string ps = osc_buf.substr(0, sep);

    if (ps == "0" || ps == "1" || ps == "2") { // window title
        String title = String::utf8(osc_buf.c_str() + sep + 1);
        if (title != terminal_title) {
            terminal_title = title;
            emit_signal("title_changed", terminal_title);
        }
    } else if (ps == "52") { // clipboard set: "52;<sel>;<base64>"
        size_t sep2 = osc_buf.find(';', sep + 1);
        if (sep2 == std::string::npos) return;
        String b64 = String::utf8(osc_buf.c_str() + sep2 + 1);
        if (b64.is_empty() || b64 == "?") return; // ignore clipboard *queries*
        String text = Marshalls::get_singleton()->base64_to_utf8(b64);
        if (!text.is_empty()) {
            DisplayServer::get_singleton()->clipboard_set(text);
        }
    }
}

void ZGTerminal::_set_mode(bool enable) {
    if (!csi_private) return;
    // A single sequence may carry several modes (e.g. htop sends ESC[?1006;1000h),
    // so handle every parameter, not just the first.
    for (size_t i = 0; i < csi_params.size(); i++) {
        int p = csi_params[i];
        switch (p) {
            case 1: app_cursor_keys = enable; break;       // DECCKM
            case 25: cursor_visible = enable; break;
            case 47:
            case 1047:
            case 1049: _switch_alt(enable); break;
            case 1000:                                     // report button press/release
            case 1002:                                     // + button-held motion
            case 1003: mouse_report = enable ? p : 0; break; // + any motion
            case 1004: focus_report = enable; break;       // focus in/out reporting
            case 1006: mouse_sgr = enable; break;          // SGR-encoded reports
            case 2004: bracketed_paste = enable; break;
            default: break;
        }
    }
}

void ZGTerminal::_switch_alt(bool enable) {
    if (enable == using_alt) return;
    if (enable) {
        saved_x = cur_x; saved_y = cur_y;
        using_alt = true;
        std::fill(alt.begin(), alt.end(), blank_cell());
        cur_x = 0; cur_y = 0;
        scroll_offset = 0;
    } else {
        using_alt = false;
        cur_x = saved_x; cur_y = saved_y;
        scroll_offset = 0;
    }
}

void ZGTerminal::_handle_sgr() {
    if (csi_params.empty() || (csi_params.size() == 1 && csi_params[0] < 0)) {
        cur_fg = -1; cur_bg = -1; cur_flags = 0;
        return;
    }
    for (size_t i = 0; i < csi_params.size(); i++) {
        int p = csi_params[i] < 0 ? 0 : csi_params[i];
        if (p == 0) { cur_fg = -1; cur_bg = -1; cur_flags = 0; }
        else if (p == 1) cur_flags |= TF_BOLD;
        else if (p == 4) cur_flags |= TF_UNDERLINE;
        else if (p == 7) cur_flags |= TF_INVERSE;
        else if (p == 22) cur_flags &= ~TF_BOLD;
        else if (p == 24) cur_flags &= ~TF_UNDERLINE;
        else if (p == 27) cur_flags &= ~TF_INVERSE;
        else if (p >= 30 && p <= 37) cur_fg = p - 30;
        else if (p == 39) cur_fg = -1;
        else if (p >= 40 && p <= 47) cur_bg = p - 40;
        else if (p == 49) cur_bg = -1;
        else if (p >= 90 && p <= 97) cur_fg = (p - 90) + 8;
        else if (p >= 100 && p <= 107) cur_bg = (p - 100) + 8;
        else if (p == 38 || p == 48) {
            bool fg = (p == 38);
            int mode = (i + 1 < csi_params.size()) ? csi_params[i + 1] : -1;
            if (mode == 5) {
                int idx = (i + 2 < csi_params.size()) ? csi_params[i + 2] : 0;
                if (idx < 0) idx = 0;
                if (fg) cur_fg = idx; else cur_bg = idx;
                i += 2;
            } else if (mode == 2) {
                int r = (i + 2 < csi_params.size()) ? std::max(0, csi_params[i + 2]) : 0;
                int g = (i + 3 < csi_params.size()) ? std::max(0, csi_params[i + 3]) : 0;
                int bl = (i + 4 < csi_params.size()) ? std::max(0, csi_params[i + 4]) : 0;
                int32_t packed = ZGT_TRUECOLOR | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (bl & 0xFF);
                if (fg) cur_fg = packed; else cur_bg = packed;
                i += 4;
            }
        }
    }
}

void ZGTerminal::_erase_display(int mode) {
    std::vector<TermCell> &g = active();
    int start = 0, end = cols * rows;
    if (mode == 0) start = cur_y * cols + cur_x;
    else if (mode == 1) end = cur_y * cols + cur_x + 1;
    for (int i = start; i < end && i < (int)g.size(); i++) g[i] = blank_cell();
}

void ZGTerminal::_erase_line(int mode) {
    int x0 = 0, x1 = cols;
    if (mode == 0) x0 = cur_x;
    else if (mode == 1) x1 = cur_x + 1;
    for (int x = x0; x < x1; x++) cell(x, cur_y) = blank_cell();
}

void ZGTerminal::_insert_lines(int n) {
    if (cur_y < scroll_top || cur_y > scroll_bottom) return;
    std::vector<TermCell> &g = active();
    for (int y = scroll_bottom; y >= cur_y; y--) {
        int src = y - n;
        for (int x = 0; x < cols; x++) {
            g[y * cols + x] = (src >= cur_y) ? g[src * cols + x] : blank_cell();
        }
    }
}

void ZGTerminal::_delete_lines(int n) {
    if (cur_y < scroll_top || cur_y > scroll_bottom) return;
    std::vector<TermCell> &g = active();
    for (int y = cur_y; y <= scroll_bottom; y++) {
        int src = y + n;
        for (int x = 0; x < cols; x++) {
            g[y * cols + x] = (src <= scroll_bottom) ? g[src * cols + x] : blank_cell();
        }
    }
}

void ZGTerminal::_delete_chars(int n) {
    for (int x = cur_x; x < cols; x++) {
        int src = x + n;
        cell(x, cur_y) = (src < cols) ? cell(src, cur_y) : blank_cell();
    }
}

void ZGTerminal::_insert_chars(int n) {
    for (int x = cols - 1; x >= cur_x; x--) {
        int src = x - n;
        cell(x, cur_y) = (src >= cur_x) ? cell(src, cur_y) : blank_cell();
    }
}

void ZGTerminal::_erase_chars(int n) {
    for (int x = cur_x; x < cur_x + n && x < cols; x++) cell(x, cur_y) = blank_cell();
}

// ---------- colors ----------

Color ZGTerminal::_ansi_color(int32_t idx, bool is_fg) const {
    static const Color base16[16] = {
        Color(0.0, 0.0, 0.0), Color(0.8, 0.0, 0.0), Color(0.0, 0.8, 0.0), Color(0.8, 0.8, 0.0),
        Color(0.16, 0.39, 0.86), Color(0.8, 0.0, 0.8), Color(0.0, 0.8, 0.8), Color(0.78, 0.78, 0.78),
        Color(0.34, 0.34, 0.34), Color(1.0, 0.33, 0.33), Color(0.33, 1.0, 0.33), Color(1.0, 1.0, 0.33),
        Color(0.4, 0.6, 1.0), Color(1.0, 0.33, 1.0), Color(0.33, 1.0, 1.0), Color(1.0, 1.0, 1.0),
    };
    if (idx < 0) {
        if (is_fg) return cfg_fg;
        return Color(cfg_bg.r, cfg_bg.g, cfg_bg.b, cfg_opacity);
    }
    if (idx & ZGT_TRUECOLOR) {
        float r = ((idx >> 16) & 0xFF) / 255.0f;
        float g = ((idx >> 8) & 0xFF) / 255.0f;
        float b = (idx & 0xFF) / 255.0f;
        return Color(r, g, b);
    }
    if (idx < 16) return has_palette ? cfg_palette[idx] : base16[idx];
    if (idx < 232) {
        int i = idx - 16;
        int r = i / 36, g = (i / 6) % 6, b = i % 6;
        auto comp = [](int v) { return v == 0 ? 0.0f : (55.0f + 40.0f * v) / 255.0f; };
        return Color(comp(r), comp(g), comp(b));
    }
    float v = (8.0f + (idx - 232) * 10.0f) / 255.0f;
    return Color(v, v, v);
}

// ---------- view / selection ----------

int ZGTerminal::_viewport_top_vi() const {
    return (int)scrollback.size() - scroll_offset;
}

void ZGTerminal::_scroll_view(int delta_lines) {
    int maxoff = (int)scrollback.size();
    scroll_offset += delta_lines;
    if (scroll_offset < 0) scroll_offset = 0;
    if (scroll_offset > maxoff) scroll_offset = maxoff;
    queue_redraw();
}

void ZGTerminal::_snap_to_bottom() {
    if (scroll_offset != 0) {
        scroll_offset = 0;
        queue_redraw();
    }
}

void ZGTerminal::_get_line_cells(int vi, std::vector<TermCell> &out) const {
    int n = (int)scrollback.size();
    if (vi >= 0 && vi < n) {
        out = scrollback[vi];
        return;
    }
    int r = vi - n;
    const std::vector<TermCell> &g = using_alt ? alt : primary;
    out.assign(cols, TermCell());
    if (r >= 0 && r < rows && (int)g.size() == cols * rows) {
        for (int x = 0; x < cols; x++) out[x] = g[r * cols + x];
    }
}

void ZGTerminal::_begin_selection(const Vector2 &local) {
    int sx = std::min(cols - 1, std::max(0, (int)(local.x / cell_w)));
    int sy = std::min(rows - 1, std::max(0, (int)(local.y / cell_h)));
    int vi = _viewport_top_vi() + sy;
    sel_a_line = sel_b_line = vi;
    sel_a_col = sel_b_col = sx;
    selecting = true;
    has_sel = false;
    queue_redraw();
}

void ZGTerminal::_update_selection(const Vector2 &local) {
    int sx = std::min(cols - 1, std::max(0, (int)(local.x / cell_w)));
    int sy = std::min(rows - 1, std::max(0, (int)(local.y / cell_h)));
    sel_b_line = _viewport_top_vi() + sy;
    sel_b_col = sx;
    has_sel = (sel_b_line != sel_a_line || sel_b_col != sel_a_col);
    queue_redraw();
}

void ZGTerminal::_cell_at(const Vector2 &local, int &vi, int &col) const {
    col = std::min(cols - 1, std::max(0, (int)(local.x / cell_w)));
    int sy = std::min(rows - 1, std::max(0, (int)(local.y / cell_h)));
    vi = _viewport_top_vi() + sy;
}

static bool zgt_is_word_char(char32_t c) {
    if (c == 0 || c == ' ') return false;
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
    for (const char *p = "._-/~:@%+=#?&"; *p; p++) if (c == (char32_t)*p) return true;
    return c > 127; // non-ASCII counts as part of a word
}

void ZGTerminal::_select_word(int vi, int col) {
    std::vector<TermCell> line;
    _get_line_cells(vi, line);
    int n = (int)line.size();
    if (n == 0) return;
    if (col >= n) col = n - 1;
    if (col < 0) col = 0;
    int l = col, r = col;
    if (zgt_is_word_char(line[col].ch)) {
        while (l > 0 && zgt_is_word_char(line[l - 1].ch)) l--;
        while (r < n - 1 && zgt_is_word_char(line[r + 1].ch)) r++;
    }
    sel_a_line = sel_b_line = vi;
    sel_a_col = l;
    sel_b_col = r;
    has_sel = true;
    _copy_selection();
    queue_redraw();
}

void ZGTerminal::_select_line(int vi) {
    std::vector<TermCell> line;
    _get_line_cells(vi, line);
    int n = (int)line.size();
    int last = 0;
    for (int i = 0; i < n; i++)
        if (line[i].ch != ' ' && line[i].ch != 0) last = i;
    sel_a_line = sel_b_line = vi;
    sel_a_col = 0;
    sel_b_col = last;
    has_sel = true;
    _copy_selection();
    queue_redraw();
}

bool ZGTerminal::_url_at(int vi, int col, String &out) const {
    std::vector<TermCell> line;
    _get_line_cells(vi, line);
    int n = (int)line.size();
    if (col < 0 || col >= n) return false;
    char32_t here = line[col].ch;
    if (here <= ' ' || here == 0) return false;
    int l = col, r = col;
    while (l > 0 && line[l - 1].ch > ' ') l--;
    while (r < n - 1 && line[r + 1].ch > ' ') r++;
    String tok;
    for (int i = l; i <= r; i++) tok += String::chr(line[i].ch);
    // trim trailing punctuation that's usually not part of the link
    while (tok.length() > 0) {
        char32_t last = tok[tok.length() - 1];
        if (last == ')' || last == ']' || last == '.' || last == ',' ||
                last == ';' || last == ':' || last == '!' || last == '?' ||
                last == '"' || last == '\'' || last == '>') {
            tok = tok.substr(0, tok.length() - 1);
        } else {
            break;
        }
    }
    if (tok.begins_with("http://") || tok.begins_with("https://") || tok.begins_with("ftp://")) {
        out = tok;
        return true;
    }
    if (tok.begins_with("www.")) {
        out = "https://" + tok;
        return true;
    }
    return false;
}

bool ZGTerminal::_in_selection(int vi, int col) const {
    if (!has_sel) return false;
    int s_line = sel_a_line, s_col = sel_a_col, e_line = sel_b_line, e_col = sel_b_col;
    if (e_line < s_line || (e_line == s_line && e_col < s_col)) {
        std::swap(s_line, e_line);
        std::swap(s_col, e_col);
    }
    if (vi < s_line || vi > e_line) return false;
    if (vi == s_line && col < s_col) return false;
    if (vi == e_line && col > e_col) return false;
    return true;
}

void ZGTerminal::_copy_selection() {
    if (!has_sel) return;
    int s_line = sel_a_line, s_col = sel_a_col, e_line = sel_b_line, e_col = sel_b_col;
    if (e_line < s_line || (e_line == s_line && e_col < s_col)) {
        std::swap(s_line, e_line);
        std::swap(s_col, e_col);
    }
    String text;
    std::vector<TermCell> line;
    for (int vi = s_line; vi <= e_line; vi++) {
        _get_line_cells(vi, line);
        int c0 = (vi == s_line) ? s_col : 0;
        int c1 = (vi == e_line) ? e_col : (int)line.size() - 1;
        String row;
        for (int c = c0; c <= c1 && c < (int)line.size(); c++) {
            char32_t ch = line[c].ch;
            row += String::chr(ch == 0 ? ' ' : ch);
        }
        row = row.rstrip(" \t");
        text += row;
        if (vi < e_line) text += "\n";
    }
    if (!text.is_empty()) {
        DisplayServer::get_singleton()->clipboard_set(text);
    }
}

void ZGTerminal::_paste_clipboard() {
    String t = DisplayServer::get_singleton()->clipboard_get();
    if (t.is_empty()) return;
    _snap_to_bottom();
    if (bracketed_paste) {
        _send(String::chr(0x1B) + "[200~");
        _send(t);
        _send(String::chr(0x1B) + "[201~");
    } else {
        _send(t);
    }
}

// ---------- drawing ----------

void ZGTerminal::_notification(int p_what) {
    switch (p_what) {
        case NOTIFICATION_READY:
            _start_shell();
            break;
        case NOTIFICATION_PROCESS:
            _read_pty();
            break;
        case NOTIFICATION_RESIZED:
            _recompute_grid();
            queue_redraw();
            break;
        case NOTIFICATION_FOCUS_ENTER:
            if (focus_report) _send(String::chr(0x1B) + "[I");
            queue_redraw();
            break;
        case NOTIFICATION_FOCUS_EXIT:
            if (focus_report) _send(String::chr(0x1B) + "[O");
            queue_redraw();
            break;
        case NOTIFICATION_DRAW: {
            draw_rect(Rect2(Vector2(0, 0), get_size()), _ansi_color(-1, false), true);
            if (primary.empty() || font.is_null()) break;

            int top_vi = _viewport_top_vi();
            int n_back = (int)scrollback.size();
            Color sel_color(0.20, 0.40, 0.85, 0.55);
            int qlen = search_query.length();

            std::vector<TermCell> rowbuf;
            std::vector<bool> hl;
            for (int y = 0; y < rows; y++) {
                int vi = top_vi + y;
                const TermCell *line = nullptr;
                int line_len = 0;
                if (vi >= 0 && vi < n_back) {
                    line = scrollback[vi].data();
                    line_len = (int)scrollback[vi].size();
                } else {
                    int r = vi - n_back;
                    if (r >= 0 && r < rows) {
                        line = active().data() + r * cols;
                        line_len = cols;
                    }
                }

                // Search highlight mask for this row.
                hl.clear();
                if (qlen > 0 && line && line_len > 0) {
                    String text;
                    for (int x = 0; x < line_len; x++)
                        text += String::chr(line[x].ch == 0 ? ' ' : line[x].ch);
                    String lt = text.to_lower();
                    int pos = lt.find(search_query);
                    if (pos != -1) {
                        hl.assign(line_len, false);
                        while (pos != -1) {
                            for (int k = 0; k < qlen && pos + k < line_len; k++) hl[pos + k] = true;
                            pos = lt.find(search_query, pos + qlen);
                        }
                    }
                }
                bool current_match = (qlen > 0 && vi == search_vi);

                float py = y * cell_h;
                for (int x = 0; x < cols; x++) {
                    float px = x * cell_w;
                    TermCell cl;
                    if (line && x < line_len) cl = line[x];

                    int32_t fg = cl.fg, bg = cl.bg;
                    if (cl.flags & TF_INVERSE) std::swap(fg, bg);
                    if ((cl.flags & TF_BOLD) && fg >= 0 && fg < 8) fg += 8;

                    if (bg >= 0 || (cl.flags & TF_INVERSE)) {
                        draw_rect(Rect2(px, py, cell_w + 1, cell_h), _ansi_color(bg, false), true);
                    }
                    if (!hl.empty() && x < (int)hl.size() && hl[x]) {
                        Color hc = current_match ? Color(1.0, 0.55, 0.0, 0.65)
                                                 : Color(0.95, 0.85, 0.1, 0.45);
                        draw_rect(Rect2(px, py, cell_w + 1, cell_h), hc, true);
                    }
                    if (_in_selection(vi, x)) {
                        draw_rect(Rect2(px, py, cell_w + 1, cell_h), sel_color, true);
                    }
                    if (cl.ch != ' ' && cl.ch != 0) {
                        draw_char(font, Vector2(px, py + ascent), String::chr(cl.ch),
                            font_size, _ansi_color(fg, true));
                    }
                    if (cl.flags & TF_UNDERLINE) {
                        draw_rect(Rect2(px, py + cell_h - 1, cell_w, 1), _ansi_color(fg, true), true);
                    }
                }
            }

            // cursor (only when viewing the live bottom)
            if (cursor_visible && scroll_offset == 0) {
                float px = cur_x * cell_w;
                float py = cur_y * cell_h;
                Color cc = _ansi_color(-1, true);
                if (has_focus()) {
                    if (cursor_style <= 2) {            // block
                        draw_rect(Rect2(px, py, cell_w, cell_h), cc, true);
                        const TermCell &cl = cell(cur_x, cur_y);
                        if (cl.ch != ' ' && cl.ch != 0) {
                            draw_char(font, Vector2(px, py + ascent), String::chr(cl.ch),
                                font_size, _ansi_color(-1, false));
                        }
                    } else if (cursor_style <= 4) {     // underline
                        float h = std::max(2.0f, cell_h * 0.12f);
                        draw_rect(Rect2(px, py + cell_h - h, cell_w, h), cc, true);
                    } else {                            // bar
                        float w = std::max(2.0f, cell_w * 0.15f);
                        draw_rect(Rect2(px, py, w, cell_h), cc, true);
                    }
                } else {
                    draw_rect(Rect2(px, py, cell_w, cell_h), cc, false, 1.0);
                }
            }
        } break;
        case NOTIFICATION_EXIT_TREE:
        case NOTIFICATION_PREDELETE:
            _stop_shell();
            break;
    }
}

// ---------- input ----------

void ZGTerminal::_send(const String &s) {
#ifdef LINUX_ENABLED
    if (master_fd < 0 || s.is_empty()) return;
    CharString cs = s.utf8();
    ssize_t to_write = cs.length();
    const char *p = cs.get_data();
    while (to_write > 0) {
        ssize_t n = write(master_fd, p, to_write);
        if (n <= 0) break;
        p += n;
        to_write -= n;
    }
#endif
}

// Map a Godot mouse button to its base xterm mouse-report code.
static int zgt_mouse_base_code(MouseButton b) {
    switch (b) {
        case MOUSE_BUTTON_LEFT: return 0;
        case MOUSE_BUTTON_MIDDLE: return 1;
        case MOUSE_BUTTON_RIGHT: return 2;
        case MOUSE_BUTTON_WHEEL_UP: return 64;
        case MOUSE_BUTTON_WHEEL_DOWN: return 65;
        default: return 0;
    }
}

void ZGTerminal::_mouse_report(int code, const Vector2 &local, bool pressed) {
    int col = std::min(cols - 1, std::max(0, (int)(local.x / cell_w)));
    int row = std::min(rows - 1, std::max(0, (int)(local.y / cell_h)));
    if (mouse_sgr) {
        _send(String::chr(0x1B) + "[<" + String::num_int64(code) + ";" +
            String::num_int64(col + 1) + ";" + String::num_int64(row + 1) +
            (pressed ? "M" : "m"));
    } else {
        // Legacy X10/normal encoding: each value offset by 32, release = button 3.
        // Sent as raw bytes (values can exceed 127, which String::utf8() would mangle).
        int cb = pressed ? code : ((code & 0xFC) | 3);
        uint8_t seq[6] = {
            0x1B, '[', 'M',
            (uint8_t)std::min(255, (cb & 0xFF) + 32),
            (uint8_t)std::min(255, col + 1 + 32),
            (uint8_t)std::min(255, row + 1 + 32),
        };
        _send_raw(seq, 6);
    }
}

void ZGTerminal::_send_raw(const uint8_t *bytes, int n) {
#ifdef LINUX_ENABLED
    if (master_fd < 0 || n <= 0) return;
    int off = 0;
    while (off < n) {
        ssize_t w = write(master_fd, bytes + off, n - off);
        if (w <= 0) break;
        off += w;
    }
#endif
}

void ZGTerminal::_gui_input(const Ref<InputEvent> &p_event) {
    // mouse buttons
    Ref<InputEventMouseButton> mb = p_event;
    if (mb.is_valid()) {
        MouseButton b = mb->get_button_index();
        // Shift forces the terminal's own selection/scroll even while an app is
        // tracking the mouse (xterm/kitty convention).
        bool report = (mouse_report != 0) && !mb->is_shift_pressed();
        int mods = (mb->is_shift_pressed() ? 4 : 0) |
                   (mb->is_alt_pressed() ? 8 : 0) |
                   (mb->is_ctrl_pressed() ? 16 : 0);

        if (mb->is_pressed()) {
            if (b == MOUSE_BUTTON_WHEEL_UP || b == MOUSE_BUTTON_WHEEL_DOWN) {
                if (report) {
                    _mouse_report(zgt_mouse_base_code(b) + mods, mb->get_position(), true);
                } else if (using_alt) {
                    const char *ar = (b == MOUSE_BUTTON_WHEEL_UP) ? "OA" : "OB";
                    for (int i = 0; i < 3; i++) _send(String::chr(0x1B) + ar);
                } else {
                    _scroll_view(b == MOUSE_BUTTON_WHEEL_UP ? 3 : -3);
                }
                accept_event();
                return;
            }
            if (b == MOUSE_BUTTON_LEFT) {
                grab_focus();
                int vi, col;
                _cell_at(mb->get_position(), vi, col);
                // Ctrl+click opens a URL under the cursor (kitty-style).
                String url;
                if (mb->is_ctrl_pressed() && _url_at(vi, col, url)) {
                    OS::get_singleton()->shell_open(url);
                    accept_event();
                    return;
                }
                if (report) {
                    _mouse_report(zgt_mouse_base_code(b) + mods, mb->get_position(), true);
                    accept_event();
                    return;
                }
                // Multi-click: 2 = word, 3 = line, otherwise start a drag selection.
                uint64_t now = Time::get_singleton()->get_ticks_msec();
                if (now - last_click_msec < 400 && vi == last_click_vi && col == last_click_col) {
                    click_count++;
                } else {
                    click_count = 1;
                }
                last_click_msec = now;
                last_click_vi = vi;
                last_click_col = col;
                if (click_count == 2) {
                    selecting = false;
                    _select_word(vi, col);
                } else if (click_count >= 3) {
                    selecting = false;
                    _select_line(vi);
                } else {
                    _begin_selection(mb->get_position());
                }
                accept_event();
                return;
            }
            if (b == MOUSE_BUTTON_MIDDLE || b == MOUSE_BUTTON_RIGHT) {
                grab_focus();
                if (report) {
                    _mouse_report(zgt_mouse_base_code(b) + mods, mb->get_position(), true);
                } else if (b == MOUSE_BUTTON_MIDDLE) {
                    _paste_clipboard();
                }
                accept_event();
                return;
            }
        } else {
            if (report && (b == MOUSE_BUTTON_LEFT || b == MOUSE_BUTTON_MIDDLE || b == MOUSE_BUTTON_RIGHT)) {
                _mouse_report(zgt_mouse_base_code(b) + mods, mb->get_position(), false);
                accept_event();
                return;
            }
            if (b == MOUSE_BUTTON_LEFT) {
                selecting = false;
                if (has_sel) _copy_selection();
                accept_event();
                return;
            }
        }
        return;
    }

    // mouse motion
    Ref<InputEventMouseMotion> mm = p_event;
    if (mm.is_valid()) {
        bool report = (mouse_report != 0) && !mm->is_shift_pressed();
        if (report) {
            BitField<MouseButtonMask> mask = mm->get_button_mask();
            bool left = mask.has_flag(MOUSE_BUTTON_MASK_LEFT);
            bool middle = mask.has_flag(MOUSE_BUTTON_MASK_MIDDLE);
            bool right = mask.has_flag(MOUSE_BUTTON_MASK_RIGHT);
            bool any_button = left || middle || right;
            // 1003 reports all motion; 1002 only while a button is held.
            if (mouse_report == 1003 || (mouse_report == 1002 && any_button)) {
                int base = 32; // motion flag
                if (left) base += 0;
                else if (middle) base += 1;
                else if (right) base += 2;
                else base += 3; // no button (1003 hover)
                _mouse_report(base, mm->get_position(), true);
            }
            accept_event();
            return;
        }
        if (selecting) {
            _update_selection(mm->get_position());
            accept_event();
        }
        return;
    }

    // keyboard
    Ref<InputEventKey> k = p_event;
    if (k.is_null() || !k->is_pressed()) return;

    Key kc = k->get_keycode();
    bool ctrl = k->is_ctrl_pressed();
    bool alt = k->is_alt_pressed();
    bool shift = k->is_shift_pressed();

    // copy / paste shortcuts
    if (ctrl && shift && kc == KEY_C) { _copy_selection(); accept_event(); return; }
    if (ctrl && shift && kc == KEY_V) { _paste_clipboard(); accept_event(); return; }

    // font zoom (kitty-style)
    if (ctrl && shift && (kc == KEY_EQUAL || kc == KEY_PLUS)) { _set_font_size(font_size + 1); accept_event(); return; }
    if (ctrl && shift && (kc == KEY_MINUS || kc == KEY_UNDERSCORE)) { _set_font_size(font_size - 1); accept_event(); return; }
    if (ctrl && shift && kc == KEY_0) { _set_font_size(14); accept_event(); return; }

    // scrollback search
    if (ctrl && shift && kc == KEY_F) { emit_signal("search_requested"); accept_event(); return; }

    String seq;
    String ck = app_cursor_keys ? "O" : "[";

    switch (kc) {
        case KEY_ENTER:
        case KEY_KP_ENTER: seq = String::chr('\r'); break;
        case KEY_BACKSPACE: seq = String::chr(0x7F); break;
        case KEY_TAB: seq = String::chr('\t'); break;
        case KEY_ESCAPE: seq = String::chr(0x1B); break;
        case KEY_UP: seq = String::chr(0x1B) + ck + "A"; break;
        case KEY_DOWN: seq = String::chr(0x1B) + ck + "B"; break;
        case KEY_RIGHT: seq = String::chr(0x1B) + ck + "C"; break;
        case KEY_LEFT: seq = String::chr(0x1B) + ck + "D"; break;
        case KEY_HOME: seq = String::chr(0x1B) + "[H"; break;
        case KEY_END: seq = String::chr(0x1B) + "[F"; break;
        case KEY_PAGEUP: seq = String::chr(0x1B) + "[5~"; break;
        case KEY_PAGEDOWN: seq = String::chr(0x1B) + "[6~"; break;
        case KEY_INSERT: seq = String::chr(0x1B) + "[2~"; break;
        case KEY_DELETE: seq = String::chr(0x1B) + "[3~"; break;
        default: {
            char32_t u = k->get_unicode();
            if (ctrl && kc >= KEY_A && kc <= KEY_Z) {
                seq = String::chr((char32_t)(kc - KEY_A + 1));
            } else if (ctrl && kc == KEY_SPACE) {
                seq = String::chr((char32_t)0);
            } else if (u != 0) {
                if (alt) seq = String::chr(0x1B);
                seq += String::chr(u);
            }
        } break;
    }

    if (!seq.is_empty()) {
        _snap_to_bottom();
        _send(seq);
        accept_event();
    }
}

// ---------- public (plugin API) ----------

void ZGTerminal::start_terminal() { _start_shell(); }

void ZGTerminal::stop_terminal() { _stop_shell(); }

void ZGTerminal::search(const String &query, bool forward) {
    search_query = query.to_lower();
    if (search_query.is_empty()) {
        search_vi = -1;
        queue_redraw();
        return;
    }
    int total = (int)scrollback.size() + rows;
    if (total <= 0) return;
    int start = (search_vi >= 0 && search_vi < total) ? search_vi : total - 1;
    std::vector<TermCell> line;
    for (int step = 1; step <= total; step++) {
        int vi = forward ? (start + step) : (start - step);
        while (vi < 0) vi += total;
        while (vi >= total) vi -= total;
        _get_line_cells(vi, line);
        String text;
        for (size_t i = 0; i < line.size(); i++)
            text += String::chr(line[i].ch == 0 ? ' ' : line[i].ch);
        if (text.to_lower().find(search_query) != -1) {
            search_vi = vi;
            // Bring the match roughly to the middle of the viewport.
            int off = (int)scrollback.size() - vi + rows / 2;
            if (off < 0) off = 0;
            if (off > (int)scrollback.size()) off = (int)scrollback.size();
            scroll_offset = off;
            queue_redraw();
            return;
        }
    }
    queue_redraw(); // no match: keep highlighting whatever is on screen
}

void ZGTerminal::clear_search() {
    search_query = "";
    search_vi = -1;
    queue_redraw();
}

} // namespace godot
