#ifndef ZGTERMINAL_H
#define ZGTERMINAL_H

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event.hpp>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace godot {

// Bit set on a packed color value to mark it as 24-bit truecolor (low 24 bits
// are 0xRRGGBB). Otherwise a color is -1 (default) or a 0..255 palette index.
static const int32_t ZGT_TRUECOLOR = 0x40000000;

// One terminal cell: a glyph plus its colors/attributes.
struct TermCell {
    char32_t ch = ' ';
    int32_t fg = -1; // -1 == default foreground
    int32_t bg = -1; // -1 == default background
    uint8_t flags = 0;
};

enum TermFlags {
    TF_BOLD = 1,
    TF_UNDERLINE = 2,
    TF_INVERSE = 4,
    TF_WIDE = 8,       // lead cell of a double-width glyph (CJK/emoji)
    TF_WIDE_CONT = 16, // placeholder cell to the right of a TF_WIDE lead
};

// A self-contained terminal emulator Control: it runs a real shell on a PTY,
// parses a useful subset of ANSI/VT escape sequences and draws the character
// grid itself. No external window, so it works the same on X11 and Wayland.
class ZGTerminal : public Control {
    GDCLASS(ZGTerminal, Control)

private:
    // PTY / child process
    int master_fd;
    int child_pid;

    // Grid
    int cols;
    int rows;
    std::vector<TermCell> primary;
    std::vector<TermCell> alt;
    bool using_alt;

    // Scrollback (primary screen only) + view offset (lines scrolled up)
    std::deque<std::vector<TermCell>> scrollback;
    int scrollback_max;
    int scroll_offset;

    // Mouse selection (coordinates are virtual line index + column)
    bool selecting;
    bool has_sel;
    int sel_a_line, sel_a_col;
    int sel_b_line, sel_b_col;

    // Multi-click detection (double = word, triple = line)
    uint64_t last_click_msec;
    int last_click_vi, last_click_col;
    int click_count;

    // Cursor + saved cursor
    int cur_x, cur_y;
    int saved_x, saved_y;
    bool cursor_visible;
    int cursor_style;   // DECSCUSR: 0/1/2 block, 3/4 underline, 5/6 bar

    // Current pen attributes
    int32_t cur_fg, cur_bg;
    uint8_t cur_flags;

    // Modes
    bool app_cursor_keys; // DECCKM (?1)
    bool bracketed_paste;
    int mouse_report;   // 0 = off, else 1000/1002/1003 (which events to report)
    bool mouse_sgr;     // ?1006 — SGR-encoded mouse reports
    bool focus_report;  // ?1004 — send ESC[I / ESC[O on focus change

    // Scroll region (inclusive rows)
    int scroll_top, scroll_bottom;

    // Escape-sequence parser
    int parse_state;
    std::vector<int> csi_params;
    int csi_cur;
    bool csi_has_digit;
    bool csi_private;

    // OSC accumulation (window title etc.)
    std::string osc_buf;
    String terminal_title;

    // Scrollback search
    String search_query;  // lowercased; empty = inactive
    int search_vi;        // virtual line of the current match

    // UTF-8 decoding (NORMAL state)
    int utf8_remaining;
    char32_t utf8_acc;

    // Font / metrics
    Ref<Font> font;
    int font_size;
    float cell_w, cell_h, ascent;

    // Theme (configurable via ProjectSettings "zgt/terminal/*")
    Color cfg_bg, cfg_fg;
    float cfg_opacity;
    bool has_palette;
    Color cfg_palette[16];

    // helpers
    std::vector<TermCell> &active();
    TermCell &cell(int x, int y);
    TermCell blank_cell() const;

    void _start_shell();
    void _stop_shell();
    void _read_pty();
    void _feed(const uint8_t *data, int len);

    void _put_char(char32_t c);
    void _line_feed();
    void _reverse_index();
    void _carriage_return();
    void _backspace();
    void _tab();
    void _scroll_up(int n);
    void _scroll_down(int n);
    void _push_scrollback_line(int grid_row);

    void _handle_csi(char final);
    void _handle_osc();
    void _handle_sgr();
    void _erase_display(int mode);
    void _erase_line(int mode);
    void _insert_lines(int n);
    void _delete_lines(int n);
    void _delete_chars(int n);
    void _insert_chars(int n);
    void _erase_chars(int n);
    void _set_mode(bool enable);
    void _switch_alt(bool enable);

    void _recompute_grid();
    void _set_winsize();
    void _load_font();
    void _load_theme();
    void _set_font_size(int s);
    void _mouse_report(int code, const Vector2 &local, bool pressed);
    Color _ansi_color(int32_t idx, bool is_fg) const;

    // view / selection / clipboard
    int _viewport_top_vi() const;
    void _scroll_view(int delta_lines);
    void _snap_to_bottom();
    void _get_line_cells(int vi, std::vector<TermCell> &out) const;
    void _begin_selection(const Vector2 &local);
    void _update_selection(const Vector2 &local);
    bool _in_selection(int vi, int col) const;
    void _copy_selection();
    void _paste_clipboard();
    void _select_word(int vi, int col);
    void _select_line(int vi);
    bool _url_at(int vi, int col, String &out) const;
    void _cell_at(const Vector2 &local, int &vi, int &col) const;

    void _send(const String &s);
    void _send_raw(const uint8_t *bytes, int n);

protected:
    static void _bind_methods();
    void _notification(int p_what);

public:
    ZGTerminal();
    ~ZGTerminal();

    virtual void _gui_input(const Ref<InputEvent> &p_event) override;

    // Public API used by the editor plugin.
    void start_terminal();
    void stop_terminal();
    void search(const String &query, bool forward);
    void clear_search();
};

}

#endif
