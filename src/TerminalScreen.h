// TerminalScreen.h — pure C++. A cell-grid terminal screen: what full-screen
// programs (vim, less, htop, man) draw, as rows of styled cells plus a cursor.
//
// It reads the same bytes as TerminalStream through the shared TermParser and
// implements the xterm subset those programs use: cursor addressing, erase,
// insert/delete of lines and characters, scroll regions, the alternate screen,
// saved cursors, autowrap with the pending-wrap rule, tab stops, DEC line
// drawing, and the mode flags the host needs to encode keys (application
// cursor keys, keypad mode, bracketed paste). Replies the program asks for
// (cursor position, device attributes, the terminal's name and version,
// its colors, the state of a mode) are queued for the host to write back.
//
// Conformance, as xterm's control sequence document and the kitty keyboard
// protocol's specification describe them (term:vt-conformance tests each):
// DA1 answers a VT220-class terminal with ANSI color ("CSI ? 62 ; 22 c"),
// DA2 "CSI > 1 ; version ; 0 c", XTVERSION "DCS > | minicode version ST";
// OSC 4, 10, 11 and 12 with "?" answer the palette and the host's
// foreground, background and cursor colors (setThemeColors); DECRQM
// answers every mode kept; DECSCUSR's cursor shapes; SGR 8 (hidden), 4:0
// to 4:5 and 21 (underline styles), 58 and 59 (underline color), 53 and 55
// (overline); the kitty keyboard flags (push, pop, set, query, a stack per
// screen; flag 1 only); and mode 2027 (grapheme clusters, below).
//
// Widths (mode 2027): a character takes termCharWidth's columns and emoji
// sequences join into one cell (grapheme clusters, as Claude Code's Ink
// counts them), except while zsh's line editor has the main screen, from
// the shell integration's prompt mark (133;A) to its command mark (133;C):
// there each code point takes termCodepointWidth's columns, zsh's own, so
// what is typed at the prompt lands where zsh thinks it is. A program may
// choose: DECSET 2027 asks for clusters, DECRST 2027 for code points, both
// for good (until a full reset); DECRQM 2027 answers what is in effect.
//
// It also encodes keys and mouse events into the bytes a program expects, so
// the GUI layer only maps its events onto TermKey and TermMouse.
//
// Soft wraps: a row that ended because the text ran past the right edge
// (auto-wrap) is marked as continuing on the next row, and a line of the
// scrollback keeps the mark. A width change rejoins such rows and wraps
// them again at the new width (reflow), on the main screen and in the
// scrollback, as iTerm2, Terminal.app and VS Code do; the alternate screen
// is cut or padded, since the program there draws it again. Copying,
// find and the host's link lookup read a wrapped line as one.
#pragma once
#include "TerminalStream.h"
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

struct TermCell {
    std::string ch = " ";   // one code point plus combining marks; "" for the
                            // right half of a wide character
    TermStyle style;
    uint8_t width = 1;      // 1, 2 (left half of a wide character), 0 (right half)
};

// A place the find bar found its text: from `col` of `line` to `endCol`
// (exclusive) of `endLine`, lines as TerminalScreen::lineCell numbers them
// (negative: the scrollback). endLine is past line when the text runs over
// a soft wrap; `width` is the cells it covers in all.
struct TermMatch {
    int line = 0;
    int col = 0;
    int width = 0;
    int endLine = 0;
    int endCol = 0;
    bool operator==(const TermMatch& o) const {
        return line == o.line && col == o.col && width == o.width &&
               endLine == o.endLine && endCol == o.endCol;
    }
};

// A place in the scrollback and screen (lines as lineCell numbers them),
// which resize() carries along with its text when it reflows.
struct TermPos {
    int line = 0;
    int col = 0;
};

// A command the shell integration marked (OSC 133): where its prompt began
// (A), where its output began (C, once it ran), and where it ended (D,
// with the exit status). Lines as TerminalScreen::lineCell numbers them.
struct TermCommand {
    int promptLine = 0, promptCol = 0;
    bool ran = false;                 // a C came: the command was run
    int outputLine = 0, outputCol = 0;
    bool ended = false;               // a D came after the C
    int endLine = 0, endCol = 0;
    int status = 0;
};

// Mouse events a program can ask for (DECSET 9, 1000, 1002, 1003).
enum class TermMouse { Press, Release, Motion, WheelUp, WheelDown };

enum class TermKey {
    Up, Down, Right, Left, Home, End, PageUp, PageDown, Insert, Delete,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Enter, KeypadEnter, Backspace, Tab, Escape,
};

// Modifier bits, numbered as xterm encodes them (the parameter is 1 + mods).
enum TermMod { TermModShift = 1, TermModAlt = 2, TermModCtrl = 4 };

// The cursor's shape a program asked for (DECSCUSR, "CSI Ps SP q"):
// Default until it asks (the host's own choice, terminal.cursor-style).
enum class TermCursorShape : uint8_t { Default, Block, Underline, Bar };

// The kitty keyboard protocol's progressive enhancement flags that the
// screen takes (others asked for are dropped): 1, disambiguate escape codes.
enum TermKeyboardFlag { TermKeyboardDisambiguate = 1 };

class TerminalScreen {
public:
    explicit TerminalScreen(int cols = 80, int rows = 24);

    void feed(const char* data, size_t length);
    void feed(const std::string& s) { feed(s.data(), s.size()); }

    // Change the size. The main screen and the scrollback are reflowed:
    // rows joined by soft wraps become one line again and are wrapped at
    // the new width (a wide character that no longer fits at the end of a
    // row moves to the next), and the cursor and the saved cursor stay on
    // their characters. When rows are removed, or reflowed lines take more
    // rows, lines go up into the scrollback to keep the cursor and the text
    // on screen; blank rows below them are cut first. Lines below the
    // cursor that no longer fit are set aside (the cursor must stay on
    // screen) and come back first when the screen grows, unless the main
    // screen scrolls or is erased in between. When rows are added, or lines
    // take fewer, lines come back down from the scrollback, as in
    // Terminal.app, so making a pane smaller and larger again loses
    // nothing. The alternate screen is cut or padded, not reflowed.
    // At least two columns (a wide character needs two cells).
    // `keep`, if given, holds places (a view's top line, a selection) that
    // are moved with their text; one whose line left the scrollback ends up
    // above -historySize().
    void resize(int cols, int rows, std::vector<TermPos>* keep = nullptr);

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    const TermCell& cell(int row, int col) const { return grid()[row][col]; }

    // What changed since the last takeDirty, so a host redraws only that.
    // `rows` has an entry per screen row, 1 for a row whose cells may have
    // changed (written, erased, inserted into, scrolled); a row is marked
    // when touched, even if it ends up as it was. `all` means everything:
    // a resize, a switch between the screens, a reset, a clear, or the
    // scrollback changing other than by lines going into it. `scrolled` is
    // how many lines went into the scrollback (historyBase() moved by as
    // much, and a view scrolled back moves with them). `marks`: the shell
    // integration's marks changed (a mark came, went, or moved), so
    // whatever is drawn from them may have to move. The cursor is not
    // counted: it is a host's to compare.
    struct Dirty {
        bool all = false;
        std::vector<uint8_t> rows;
        long long scrolled = 0;
        bool marks = false;
        bool any() const;
    };
    Dirty takeDirty();
    // The screen row's cells as stored, by reference (no copy).
    const std::vector<TermCell>& row(int r) const { return grid()[r]; }
    // A line's cells (lines as lineCell numbers them): a screen row by
    // reference, a scrollback line unpacked into `scratch` (its own width),
    // an empty vector outside the lines. A cell past the end is a blank.
    const std::vector<TermCell>& lineCells(int line, std::vector<TermCell>& scratch) const;

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
    // The line ran past the right edge and continues on the next one (a
    // soft wrap). The last screen row may say so while the cursor waits to
    // write there.
    bool lineWraps(int line) const;
    // Cells of a wrapped line that belong to its text: all but a blank
    // left at the end by a wide character that went to the next line.
    int lineTextWidth(int line) const;
    void clearHistory();
    // Bytes the scrollback takes (its lines' own storage, as allocated),
    // and what the same cells would take as TermCells, for the tests.
    size_t historyBytes() const;
    size_t historyCellBytes() const;

    // Every place `query` appears, in reading order: the scrollback first
    // (on the main screen; the alternate screen has none), then the screen.
    // Case is ignored unless `matchCase` (ASCII, Latin-1, Greek and Cyrillic
    // letters). A match runs over a soft wrap (a long line the terminal
    // wrapped) but never from one line into the next. At most `limit` are
    // returned.
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
    // selection copies it: each row's trailing blanks dropped, and no line
    // break where a row wrapped onto the next. Rows are lines as lineCell
    // numbers them, so negative ones are the scrollback.
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

    // The shell integration's marks count only with this nonce, as
    // TerminalStream::setMarkNonce says (empty: every mark counts).
    void setMarkNonce(const std::string& nonce) { nonce_ = nonce; }
    // Where the marks came: OSC 133 A, C and D on the main screen are kept
    // at the cursor's place, and stay on their text as it scrolls into the
    // scrollback, reflows, or is cleared (a mark on a line that goes, goes
    // with it). A prompt mark followed by zsh's PROMPT_SP wrap moves down
    // with the prompt. Commands in reading order, oldest first; the last
    // may still be at its prompt (not ran) or running (not ended).
    std::vector<TermCommand> commands() const;
    // What a command printed: from its output mark to its end mark (the
    // cursor while it runs), as textBetween copies it, without the line
    // break before the next prompt. Empty for a command that never ran.
    std::string commandOutput(const TermCommand& command) const;
    // The prompt line of the nearest command that ran above `line`, or below
    // it; INT_MIN when there is none.
    int previousCommandLine(int line) const;
    int nextCommandLine(int line) const;
    // Marks kept (for tests).
    size_t markCount() const { return marks_.size(); }

    // OSC 8 hyperlinks: "ESC ] 8 ; params ; URI ST" opens a link and
    // "ESC ] 8 ; ; ST" closes it; the text printed in between carries the
    // link's id in its style (TermStyle::link), into the scrollback and
    // through a reflow. Only http, https and file addresses are kept, up to
    // kMaxLinkBytes, with no spaces or control characters; anything else
    // prints as plain text. Links with the same address (and the same id=
    // parameter, when given) share an id. At most kMaxLinks are known at
    // once: when the table is full, ids no cell uses any more are given out
    // again, and with none free a new link prints as plain text.
    static constexpr size_t kMaxLinkBytes = 2048;
    static constexpr size_t kMaxLinks = 4096;
    // The address of a link id, "" for 0 or an id not in use.
    const std::string& linkURI(uint32_t id) const;
    // Links in use (for tests).
    size_t linkCount() const { return linkIds_.size(); }

    // Mouse reporting: what the program asked for. mouseTracking() is 0
    // (none), 9 (X10: presses only), 1000 (presses and releases), 1002 (and
    // motion with a button down) or 1003 (all motion); mouseEncoding() is 0
    // (xterm's bytes), 1005 (UTF-8), 1006 (SGR) or 1015 (urxvt).
    int mouseTracking() const { return mouseTracking_; }
    int mouseEncoding() const { return mouseEncoding_; }
    // DECSET 1004: the program wants to hear when the terminal gains and
    // loses the keyboard (encodeFocus).
    bool focusEvents() const { return focusEvents_; }
    // Turn mouse reporting and focus events off, as when the shell's prompt
    // comes back after a program that never turned them off.
    void resetMouseModes();
    // The bytes for a mouse event at a cell (0-based), or "" when the
    // program did not ask for it. `button` is 0 left, 1 middle, 2 right
    // (Press, Release, and Motion with a button down; -1 for motion with
    // none); mods is a TermMod mask (xterm sends Shift, Alt as Meta and
    // Ctrl). A release in xterm's and urxvt's bytes cannot say which button.
    std::string encodeMouse(TermMouse kind, int button, int col, int row, int mods) const;
    static std::string encodeFocus(bool focused) { return focused ? "\x1b[I" : "\x1b[O"; }

    // Bytes the program asked the terminal to send back (DSR, DA). The host
    // writes them to the pty; taking them clears the queue.
    std::string takeReplies();

    // What OSC 10, 11 and 12 queries answer (0xRRGGBB): the host's text,
    // background and cursor colors, so a program sees a light theme. The
    // defaults are the dark theme's.
    void setThemeColors(uint32_t foreground, uint32_t background, uint32_t cursor);
    // The version XTVERSION and DA2 report ("1.5.8"; empty: none known).
    void setVersion(const std::string& version) { version_ = version; }

    // The cursor's shape and whether it blinks (DECSCUSR, DECSET 12): the
    // program's choice, Default and steady until it makes one. RIS and
    // DECSTR put both back.
    TermCursorShape cursorShape() const { return cursorShape_; }
    bool cursorBlinks() const { return cursorBlink_; }

    // The underline color of a style (TermStyle::underlineColor); the
    // default color (the text's) for 0.
    TermColor underlineColor(uint16_t id) const { return underlineColors_.color(id); }
    size_t underlineColorCount() const { return underlineColors_.size(); }   // tests

    // The kitty keyboard protocol's flags in effect on the screen showing
    // (TermKeyboardFlag bits), for encodeKey and encodeChar. A program sets
    // them with "CSI > flags u" (push), "CSI < n u" (pop), "CSI = flags ;
    // mode u" (set), and reads them with "CSI ? u"; the main and alternate
    // screens keep a stack each, of at most kKeyboardStackMax entries.
    int keyboardFlags() const { return keyboardFlags_[onAlt_ ? 1 : 0]; }
    static constexpr size_t kKeyboardStackMax = 16;
    // Both stacks emptied and the flags off, as when the shell's prompt
    // comes back after a program that never popped them.
    void resetKeyboardModes();

    // Mode 2027 (see the top of this file): whether characters are counted
    // as grapheme clusters now (false: one code point at a time).
    bool clusterWidths() const;

    // The bytes for a key. `appCursor` is appCursorKeys(), `appKeypad` is
    // appKeypad(); mods is a TermMod mask.
    // keyboardFlags is keyboardFlags(): with the kitty protocol's
    // disambiguate flag, Escape is "CSI 27 u", Enter, Tab and Backspace
    // with a modifier are "CSI 13/9/127 ; m u" (without one, as before),
    // F1 to F4 are "CSI P", "CSI Q", "CSI 13 ~" and "CSI S" (with "1 ; m"
    // for modifiers), the cursor keys ignore DECCKM, and keypad Enter is
    // "CSI 57414 u"; the rest is as without it.
    static std::string encodeKey(TermKey key, int mods, bool appCursor,
                                 bool appKeypad = false, int keyboardFlags = 0);
    // The bytes for a typed character: Ctrl turns letters and a few symbols
    // into control codes, Alt (Option as Meta) prefixes ESC. With the kitty
    // protocol's disambiguate flag a character with Ctrl or Alt is "CSI
    // code ; m u" instead, the code its lowercase (unshifted) form and m
    // 1 + mods (Shift included); Shift alone types the character.
    static std::string encodeChar(uint32_t cp, int mods, int keyboardFlags = 0);
    // Pasted text, wrapped in the bracketed-paste markers when the program
    // asked for them. Newlines become carriage returns, as typed. Inside the
    // markers every ESC is shown as U+241B (as xterm.js does), so a paste
    // can neither end the bracket early (ESC [201~, however it is split or
    // nested) nor send any other escape sequence.
    static std::string encodePaste(const std::string& text, bool bracketed);

    // Whether a paste asks first: VS Code's
    // terminal.integrated.enableMultiLinePasteWarning, as
    // terminal.multiline-paste-warning ("auto", "always" or "never"; any
    // other value is "auto"). "auto" does not ask when the program turned
    // bracketed paste on (it takes the lines as text), and a command
    // followed by one line break and nothing else is pasted without that
    // break, so it does not run by itself. A lone CR counts as a line break
    // here: the program gets it as Return. Text of one line asks only when,
    // unbracketed, it holds a control character other than Tab (C0 or DEL),
    // which the program takes as a key (Ctrl+O runs the line in zsh,
    // ESC starts a sequence), in "auto" and "always"; inside the brackets
    // the program receives them as text.
    struct PasteCheck {
        bool ask = false;        // show the question before sending anything
        bool controls = false;   // unbracketed, with control characters (above)
        std::string text;        // what to paste when not asking
        int lines = 1;           // the lines the text holds
        // Its first three lines, each cut at 30 characters, control
        // characters shown as their symbols (U+2400 + c, U+2421 for DEL),
        // Tab as a space.
        std::string preview;
    };
    static PasteCheck checkPaste(const std::string& text, bool bracketed,
                                 const std::string& mode);
    // The text with its line breaks taken out (Paste as One Line).
    static std::string pasteAsOneLine(const std::string& text);

private:
    // A screen row: its cells, and whether it continues on the next row
    // (0 no; 1 it wrapped; 2 it wrapped and its last cell is a blank left
    // by a wide character that did not fit).
    struct Row : std::vector<TermCell> {
        using std::vector<TermCell>::vector;
        uint8_t wrap = 0;
    };
    using Grid = std::vector<Row>;
    // A scrollback line, packed: a TermCell is about 48 bytes, and a
    // terminal keeps thousands of lines. The cells' text is one string;
    // the style is a run per change. `cells` is empty when every cell is one
    // byte of width 1 (most lines), else a cell's offset into `text` << 2 |
    // its width (packCell in the .cpp; offsets stop short of 1 GiB).
    struct StyleRun {
        uint32_t col;             // the first cell in this style
        TermStyle style;
    };
    struct HistLine {
        std::string text;
        std::vector<uint32_t> cells;
        std::vector<StyleRun> runs;   // by col, the first at 0
        int width = 0;                // cells in the line
        uint8_t wrap = 0;             // as Row::wrap; a wrapped line keeps
                                      // its trailing blanks
    };
    static HistLine pack(const Row& row, size_t width);
    static Row unpack(const HistLine& line);
    static TermCell histCell(const HistLine& line, int col);
    static HistLine packRow(const Row& row);   // trailing blanks dropped unless wrapped
    // Reflow helpers on packed lines.
    static size_t cellStart(const HistLine& h, int col);
    static void appendCells(HistLine& dst, const HistLine& src, int from, int to);
    static void simplify(HistLine& h);
    struct Point { int abs; int col; bool pending; bool cursor; bool dropped; };
    void reflowMain(int cols, int rows, std::vector<Point>& points);
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
    std::vector<HistLine> below_;     // lines a shrink pushed off the bottom
                                      // of the main screen (see resize)
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
    int mouseTracking_ = 0, mouseEncoding_ = 0;
    bool focusEvents_ = false;
    // An OSC 133;A (prompt start) came and no carriage return since: an
    // auto-wrap now is zsh's PROMPT_SP moving the prompt below unfinished
    // output, and the output's row does not continue onto the prompt's.
    bool promptSp_ = false;
    int syncEnds_ = 0;
    TermCursorShape cursorShape_ = TermCursorShape::Default;
    bool cursorBlink_ = false;
    // Kitty keyboard flags per screen (0 main, 1 alternate), and the flags
    // pushed before them.
    int keyboardFlags_[2] = {0, 0};
    std::vector<int> keyboardStack_[2];
    void keyboardSequence(char lead, const std::string& params);
    // Mode 2027: -1 not chosen (clusters but at zsh's prompt), 0 code
    // points, 1 clusters. promptActive_: a 133;A came and no 133;C since.
    int graphemes_ = -1;
    bool promptActive_ = false;
    uint32_t themeFg_ = 0xD4D4D4, themeBg_ = 0x181818, themeCursor_ = 0xD4D4D4;
    std::string version_;
    TermColorTable underlineColors_;
    void collectUnderlineColors();
    void colorQuery(const std::string& body);   // OSC 4, 10, 11, 12
    int g0_ = 0, g1_ = 0, gl_ = 0;    // charsets: 0 ASCII, 1 DEC line drawing
    std::string lastChar_;            // for REP
    uint32_t lastCp_ = 0;
    std::string replies_;
    std::string title_;
    std::string directory_;
    int commandsEnded_ = 0;
    // Shell integration marks, in reading order: `id` is the line's number
    // counted from the first line ever (historyBase_ + line), which stays
    // the same as the line scrolls up; `kind` is 'A', 'C' or 'D'.
    struct Mark {
        long long id;
        int col;
        char kind;
        int status;
        long long seq;               // the order they came in
    };
    std::deque<Mark> marks_;
    long long markSeq_ = 0;
    long long promptSeq_ = -1;       // the last prompt mark (promptMovedDown)
    void insertMark(const Mark& m);
    long long marksVersion_ = 0;     // counts changes to marks_ (takeDirty)
    long long marksTaken_ = 0;
    // Dirty rows (takeDirty): one entry per screen row.
    std::vector<uint8_t> dirtyRows_;
    bool dirtyAll_ = true;
    long long dirtyBase_ = 0;        // historyBase_ at the last take
    void touch(int y) {
        if (y >= 0 && y < (int)dirtyRows_.size()) dirtyRows_[y] = 1;
    }
    void touchRows(int from, int to);   // [from, to]
    void touchAll() { dirtyAll_ = true; }
    std::string nonce_;
    void addMark(char kind, int status);
    void dropMarks(long long fromId, long long toId);   // [from, to)
    void shiftMarks(long long fromId, long long toId, long long by);
    void pruneMarks();               // those above the scrollback
    void promptMovedDown();          // PROMPT_SP wrapped: the prompt mark follows
    // OSC 8: the open link (0 none), and the table behind the ids (id - 1
    // indexes links_; linkKeys_ the key linkIds_ has it under; ids whose
    // links went, to give out again).
    uint32_t link_ = 0;
    std::vector<std::string> links_, linkKeys_;
    std::unordered_map<std::string, uint32_t> linkIds_;
    std::vector<uint32_t> freeLinks_;
    void hyperlink(const std::string& body);    // an OSC 8 body
    uint32_t internLink(const std::string& key, const std::string& uri);
    void collectLinks();                        // frees ids no cell uses

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
    void unwrapAbove(int y);          // the row above y no longer continues
    void switchScreen(bool alt, bool saveCursor, bool clearAlt);
    void saveCursor();
    void restoreCursor();
};
