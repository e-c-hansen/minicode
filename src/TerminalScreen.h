// TerminalScreen.h — pure C++. A cell-grid terminal screen: what full-screen
// programs (vim, less, htop, man) draw, as rows of styled cells plus a cursor.
//
// It reads the same bytes as TerminalStream through the shared TermParser and
// implements the xterm subset those programs use: cursor addressing, erase,
// insert/delete of lines and characters, scroll regions, the alternate screen,
// saved cursors, autowrap with the pending-wrap rule, tab stops, DEC line
// drawing, and the mode flags the host needs to encode keys (application
// cursor keys, keypad mode, bracketed paste). Replies the program asks for
// (cursor position, device attributes) are queued for the host to write back.
//
// It also encodes keys into the bytes a program expects, so the GUI layer only
// maps its key events onto TermKey.
#pragma once
#include "TerminalStream.h"
#include <cstdint>
#include <string>
#include <vector>

struct TermCell {
    std::string ch = " ";   // one code point plus combining marks; "" for the
                            // right half of a wide character
    TermStyle style;
    uint8_t width = 1;      // 1, 2 (left half of a wide character), 0 (right half)
};

enum class TermKey {
    Up, Down, Right, Left, Home, End, PageUp, PageDown, Insert, Delete,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Enter, KeypadEnter, Backspace, Tab, Escape,
};

// Modifier bits, numbered as xterm encodes them (the parameter is 1 + mods).
enum TermMod { TermModShift = 1, TermModAlt = 2, TermModCtrl = 4 };

class TerminalScreen {
public:
    explicit TerminalScreen(int cols = 80, int rows = 24);

    void feed(const char* data, size_t length);
    void feed(const std::string& s) { feed(s.data(), s.size()); }

    // Change the size. Columns are cut or padded (no reflow); when rows are
    // removed, lines scroll off the top as needed to keep the cursor's line.
    void resize(int cols, int rows);

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    const TermCell& cell(int row, int col) const { return grid()[row][col]; }

    // One row as UTF-8, trailing blanks dropped.
    std::string rowText(int row) const;
    // Every row, joined with "\n", trailing blank rows dropped.
    std::string text() const;
    // The text between two cells, inclusive and in reading order, as a
    // selection copies it: each row's trailing blanks dropped.
    std::string textBetween(int row0, int col0, int row1, int col1) const;

    int cursorRow() const { return y_; }
    int cursorCol() const { return x_; }
    bool cursorVisible() const { return cursorVisible_; }
    bool altScreen() const { return onAlt_; }
    bool appCursorKeys() const { return appCursor_; }
    bool appKeypad() const { return appKeypad_; }
    bool bracketedPaste() const { return bracketedPaste_; }
    bool autowrap() const { return autowrap_; }
    int scrollTop() const { return top_; }
    int scrollBottom() const { return bottom_; }
    const std::string& title() const { return title_; }

    // Bytes the program asked the terminal to send back (DSR, DA). The host
    // writes them to the pty; taking them clears the queue.
    std::string takeReplies();

    // The bytes for a key. `appCursor` is appCursorKeys(), `appKeypad` is
    // appKeypad(); mods is a TermMod mask.
    static std::string encodeKey(TermKey key, int mods, bool appCursor,
                                 bool appKeypad = false);
    // The bytes for a typed character: Ctrl turns letters and a few symbols
    // into control codes, Alt (Option as Meta) prefixes ESC.
    static std::string encodeChar(uint32_t cp, int mods);
    // Pasted text, wrapped in the bracketed-paste markers when the program
    // asked for them. Newlines become carriage returns, as typed.
    static std::string encodePaste(const std::string& text, bool bracketed);

private:
    using Row = std::vector<TermCell>;
    using Grid = std::vector<Row>;
    struct Saved {
        int x = 0, y = 0;
        TermStyle style;
        bool originMode = false, autowrap = true, wrapPending = false;
        int g0 = 0, g1 = 0, gl = 0;
    };
    struct Sink;

    TermParser parser_;
    int cols_, rows_;
    Grid main_, alt_;
    bool onAlt_ = false;              // showing the alternate screen
    Grid& grid() { return onAlt_ ? alt_ : main_; }
    const Grid& grid() const { return onAlt_ ? alt_ : main_; }
    int x_ = 0, y_ = 0;
    bool wrapPending_ = false;
    TermStyle style_;
    int top_ = 0, bottom_ = 0;        // scroll region, inclusive
    std::vector<bool> tabs_;
    Saved savedMain_, savedAlt_;
    bool autowrap_ = true, originMode_ = false, insertMode_ = false;
    bool cursorVisible_ = true, appCursor_ = false, appKeypad_ = false;
    bool bracketedPaste_ = false;
    int g0_ = 0, g1_ = 0, gl_ = 0;    // charsets: 0 ASCII, 1 DEC line drawing
    std::string lastChar_;            // for REP
    uint32_t lastCp_ = 0;
    std::string replies_;
    std::string title_;

    TermCell blank() const;           // an erased cell in the current colors
    void resetTabs();
    void fullReset();
    void print(uint32_t cp, const std::string& utf8);
    void control(unsigned char c);
    void csi(const std::string& params, unsigned char final);
    void esc(const std::string& inter, unsigned char final);
    void osc(const std::string& body);
    void setMode(const std::string& params, bool on);

    void lineFeed();
    void reverseIndex();
    void scrollUp(int n);             // within the scroll region
    void scrollDown(int n);
    void moveTo(int x, int y);        // absolute; clamps, clears pending wrap
    void clearWide(int y, int x);     // blank the other half of a wide char
    void eraseCells(int y, int from, int to);   // [from, to)
    void switchScreen(bool alt, bool saveCursor, bool clearAlt);
    void saveCursor();
    void restoreCursor();
};
