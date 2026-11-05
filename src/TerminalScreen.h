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
#include <deque>
#include <string>
#include <vector>

struct TermCell {
    std::string ch = " ";   // one code point plus combining marks; "" for the
                            // right half of a wide character
    TermStyle style;
    uint8_t width = 1;      // 1, 2 (left half of a wide character), 0 (right half)
};

// A place the find bar found its text: `width` cells from `col` of `line`,
// a line as TerminalScreen::lineCell numbers them (negative: the scrollback).
struct TermMatch {
    int line = 0;
    int col = 0;
    int width = 0;
    bool operator==(const TermMatch& o) const {
        return line == o.line && col == o.col && width == o.width;
    }
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
    // removed, lines scroll off the top as needed to keep the cursor's line
    // (into the scrollback, on the main screen). When rows are added to the
    // main screen and the scrollback has lines, they come back down from it,
    // as in Terminal.app, so making a pane smaller and larger again loses
    // nothing; otherwise blank rows are added below.
    void resize(int cols, int rows);

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    const TermCell& cell(int row, int col) const { return grid()[row][col]; }

    // Scrollback: lines that scrolled off the top of the main screen (a line
    // feed at the bottom with the scroll region starting at the top row, as
    // xterm saves them), oldest first, up to a limit; the alternate screen
    // never adds any. The limit is 0 (none kept) until the host sets one.
    // Lines keep the width they had; a lower limit drops the oldest at once.
    void setScrollbackLimit(int lines);
    int scrollbackLimit() const { return historyLimit_; }
    int historySize() const { return (int)history_.size(); }
    // How many lines have ever gone into the scrollback, so the number of
    // screen row 0 if every line were counted from the first. A line's
    // number (historyBase() + line) stays the same as it scrolls up, which
    // lets a host keep a scrolled-back view or a selection on its text.
    long long historyBase() const { return historyBase_; }
    // A cell of a line of the scrollback and screen together: lines
    // -historySize() .. -1 are the scrollback (-1 the newest), 0 .. rows()-1
    // the screen. A blank past the end of a line or outside them. A copy,
    // since scrollback lines are stored packed (see HistLine).
    TermCell lineCell(int line, int col) const;
    // Characters in a line: cols() on the screen, its own width above it.
    int lineWidth(int line) const;
    // A line as UTF-8, trailing blanks dropped (rowText for screen rows).
    std::string lineText(int line) const;
    void clearHistory() { history_.clear(); }
    // Bytes the scrollback takes (its lines' own storage, as allocated),
    // and what the same cells would take as TermCells, for the tests.
    size_t historyBytes() const;
    size_t historyCellBytes() const;

    // Every place `query` appears, in reading order: the scrollback first
    // (on the main screen; the alternate screen has none), then the screen.
    // Case is ignored unless `matchCase` (ASCII, Latin-1, Greek and Cyrillic
    // letters). A match never runs from one line into the next, even where
    // a program's long line wrapped. At most `limit` are returned.
    std::vector<TermMatch> find(const std::string& query, bool matchCase,
                                size_t limit = 10000) const;

    // Cmd+K. clearAll: the scrollback and the screen are erased and the
    // cursor goes home, for a shell at its prompt, which then draws the
    // prompt again. clearToCursorLine: the scrollback goes and the cursor's
    // line becomes the top row, the rows below coming up with it, as VS
    // Code's Clear does, for a program that is running and draws below
    // its last line. On the alternate screen both clear only the scrollback.
    void clearAll();
    void clearToCursorLine();

    // One row as UTF-8, trailing blanks dropped.
    std::string rowText(int row) const;
    // Every row, joined with "\n", trailing blank rows dropped.
    std::string text() const;
    // The text between two cells, inclusive and in reading order, as a
    // selection copies it: each row's trailing blanks dropped. Rows are
    // lines as lineCell numbers them, so negative ones are the scrollback.
    // A selection that reaches the right edge of a scrollback line wider
    // than the screen takes the whole line.
    std::string textBetween(int row0, int col0, int row1, int col1) const;

    int cursorRow() const { return y_; }
    int cursorCol() const { return x_; }
    bool cursorVisible() const { return cursorVisible_; }
    bool altScreen() const { return onAlt_; }
    bool appCursorKeys() const { return appCursor_; }
    bool appKeypad() const { return appKeypad_; }
    bool bracketedPaste() const { return bracketedPaste_; }
    // Synchronized output (DEC private mode 2026): the program is in the
    // middle of drawing a frame, so the host should not show the screen
    // until it ends (with a time limit, since a program may never end it).
    bool synchronizedOutput() const { return syncOutput_; }
    // How many synchronized frames have ended (the mode reset while set),
    // so a host that gave up waiting on one frame can tell when the next
    // one comes.
    int synchronizedFramesEnded() const { return syncEnds_; }
    bool autowrap() const { return autowrap_; }
    int scrollTop() const { return top_; }
    int scrollBottom() const { return bottom_; }
    const std::string& title() const { return title_; }
    // Shell integration, for a host that has only the grid (Android): the
    // directory from the last OSC 7, and how many OSC 133;D (command
    // finished) marks have arrived. A full reset keeps both, since they
    // describe the shell rather than the screen.
    const std::string& directory() const { return directory_; }
    int commandsEnded() const { return commandsEnded_; }

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
    // A scrollback line, packed: a TermCell is about 48 bytes, and a
    // terminal keeps thousands of lines. The cells' text is one string;
    // the style is a run per change. `cells` is empty when every cell is one
    // byte of width 1 (most lines), else a cell's offset into `text` << 8 |
    // its width.
    struct StyleRun {
        uint32_t col;             // the first cell in this style
        TermStyle style;
    };
    struct HistLine {
        std::string text;
        std::vector<uint32_t> cells;
        std::vector<StyleRun> runs;   // by col, the first at 0
        int width = 0;                // cells in the line
    };
    static HistLine pack(const Row& row, size_t width);
    static Row unpack(const HistLine& line);
    static TermCell histCell(const HistLine& line, int col);
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
    std::deque<HistLine> history_;    // above main_, oldest first
    int historyLimit_ = 0;
    long long historyBase_ = 0;
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
    bool syncOutput_ = false;
    int syncEnds_ = 0;
    int g0_ = 0, g1_ = 0, gl_ = 0;    // charsets: 0 ASCII, 1 DEC line drawing
    std::string lastChar_;            // for REP
    uint32_t lastCp_ = 0;
    std::string replies_;
    std::string title_;
    std::string directory_;
    int commandsEnded_ = 0;

    TermCell blank() const;          // an erased cell in the current colors
    void resetTabs();
    void fullReset();
    void print(uint32_t cp, const std::string& utf8);
    void control(unsigned char c);
    void csi(const std::string& params, unsigned char final);
    void esc(const std::string& inter, unsigned char final);
    void osc(const std::string& body);
    void setMode(const std::string& params, bool on);
    void reportMode(const std::string& params);   // DECRQM

    void lineFeed();
    void reverseIndex();
    void scrollUp(int n, bool save = false);   // within the scroll region;
                                      // save: what leaves the top goes to the
                                      // scrollback (main screen, region at top)
    void pushHistory(const Row& row);
    void scrollDown(int n);
    void moveTo(int x, int y);        // absolute; clamps, clears pending wrap
    void clearWide(int y, int x);     // blank the other half of a wide char
    void eraseCells(int y, int from, int to);   // [from, to)
    void switchScreen(bool alt, bool saveCursor, bool clearAlt);
    void saveCursor();
    void restoreCursor();
};
