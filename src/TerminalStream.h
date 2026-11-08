// TerminalStream.h — pure C++. Turns the raw bytes a shell writes to its pty
// into styled lines plus shell-integration events.
//
// It is a line model, not a screen emulator: it tracks colors and attributes
// (SGR) and the cursor within the current line (\r, \b, tab, erase-in-line,
// cursor forward/back/column), which covers colored output and progress lines.
// Movement to other lines and scroll regions are recognized and ignored, and
// text drawn on the alternate screen is left out (TerminalScreen, the cell
// grid, shows that). The stream is parsed incrementally, so sequences and
// UTF-8 characters split across reads are handled.
//
// The escape-sequence parser (TermParser) and the SGR reader live here and are
// shared with TerminalScreen, so both models read the bytes the same way.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct TermColor {
    enum Kind : uint8_t { Default, Indexed, RGB };
    Kind kind = Default;
    uint8_t index = 0;           // Indexed: 0-255 (xterm palette)
    uint8_t r = 0, g = 0, b = 0; // RGB
    bool operator==(const TermColor& o) const {
        return kind == o.kind && (kind == Default ||
               (kind == Indexed ? index == o.index
                                : (r == o.r && g == o.g && b == o.b)));
    }
    bool operator!=(const TermColor& o) const { return !(*this == o); }

    // 0xRRGGBB for an Indexed or RGB color (not meaningful for Default).
    // Indexes 0-15 use MiniCode's dark-theme palette; 16-255 are the standard
    // xterm color cube and gray ramp.
    uint32_t rgb() const;
};

// Underline styles (SGR 4, 4:0 to 4:5, and 21 for double).
enum TermUnderline : uint8_t {
    TermUnderlineNone = 0, TermUnderlineSingle, TermUnderlineDouble,
    TermUnderlineCurly, TermUnderlineDotted, TermUnderlineDashed,
};

struct TermStyle {
    TermColor fg, bg;
    // Two bytes for the flags and the underline style, so the link and the
    // underline color below keep the style at 16 bytes (the scrollback
    // stores one per style run). C++17 has no default member initializers
    // for bit-fields, hence the constructor. `underline` is a
    // TermUnderline, so it still reads as true when the text is
    // underlined at all; `hidden` is SGR 8 (concealed), `overline` SGR 53.
    uint16_t bold : 1, dim : 1, italic : 1, underline : 3, inverse : 1, strike : 1,
             hidden : 1, overline : 1;
    // An OSC 8 hyperlink the text belongs to: an id in the TerminalScreen's
    // table (TerminalScreen::linkURI), 0 for none. Only the screen sets it,
    // on the cells it prints, keeping the open link apart from the SGR
    // state, so a reset (SGR 0) inside a link stays in the link.
    uint16_t link = 0;
    // The underline's color (SGR 58): an id in a TermColorTable (the
    // screen's, TerminalScreen::underlineColor), 0 for the text's color.
    uint16_t underlineColor = 0;
    TermStyle()
        : bold(0), dim(0), italic(0), underline(0), inverse(0), strike(0), hidden(0),
          overline(0) {}
    bool operator==(const TermStyle& o) const {
        return fg == o.fg && bg == o.bg && bold == o.bold && dim == o.dim &&
               italic == o.italic && underline == o.underline &&
               inverse == o.inverse && strike == o.strike && hidden == o.hidden &&
               overline == o.overline && link == o.link &&
               underlineColor == o.underlineColor;
    }
    bool operator!=(const TermStyle& o) const { return !(*this == o); }
};

// Colors given out as small ids, so a style can name one in two bytes (the
// underline color). Id 0 is "none"; at most kMax ids are in use at once,
// and with the table full intern() returns 0. Its owner frees ids nothing
// uses any more with release() (the screen does, scanning its cells, when
// full() and an SGR names a new color), as the OSC 8 link table does. Holds
// no pointers, so a screen that owns one stays copyable.
class TermColorTable {
public:
    static constexpr size_t kMax = 4095;
    uint16_t intern(const TermColor& c);
    // The color of an id; the default color for 0 or an unknown id.
    TermColor color(uint16_t id) const {
        return id && id <= colors_.size() ? colors_[id - 1] : TermColor();
    }
    size_t size() const { return ids_.size(); }
    bool full() const { return free_.empty() && colors_.size() >= kMax; }
    // Frees every id whose used[id] is false (used has kMax + 1 entries).
    void release(const std::vector<bool>& used);

private:
    std::vector<TermColor> colors_;
    std::vector<uint16_t> free_;
    std::unordered_map<uint32_t, uint16_t> ids_;   // by key(), the ids in use
    static uint32_t key(const TermColor& c);
};

// A stretch of text in one style.
struct TermRun {
    std::string text;            // UTF-8
    TermStyle style;
};

// Columns a code point takes on a terminal, from Unicode's data (the tables
// in TerminalStream.cpp are written by tools/gen-term-width.py): 0 for
// combining and enclosing marks, format characters (the zero width joiner,
// the bidi controls), the Hangul medial and final jamo, and the emoji skin
// tone modifiers; 2 for East Asian wide and fullwidth characters and the
// emoji shown wide; 1 for the rest. With grapheme clusters (termJoinsCluster,
// VS16) this is how programs that follow Unicode (Claude Code's Ink) count.
int termCharWidth(uint32_t cp);
// Columns zsh's line editor gives a code point, one code point at a time:
// macOS's /bin/zsh is built with zsh's own Unicode 9 table (wcwidth9.h,
// ENABLE_UNICODE9; it never calls the C library's wcwidth), which gives
// regional indicators 2 columns each, spacing marks such as U+093F none,
// and U+2764 one. The ranges are written by tools/gen-zsh-width.py from
// that header. Where zsh's table has no width (unassigned in Unicode 9,
// non-printing: zsh shows those as <XXXX> itself) this is termCharWidth.
int termCodepointWidth(uint32_t cp);
// Extended_Pictographic: an emoji, or a symbol that may show as one. After a
// zero width joiner such a character joins the emoji before it (a family,
// a profession) instead of taking cells of its own.
bool termIsPictographic(uint32_t cp);
inline bool termIsRegionalIndicator(uint32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }
// The first code point of a cell's UTF-8 (0 for none), and its length.
uint32_t termFirstCodepoint(const std::string& utf8, size_t* length = nullptr);
// Whether `cp`, which takes columns of its own, belongs to the character
// already in `cell` instead (grapheme clusters, as terminals that follow
// Unicode draw them): the second regional indicator of a flag, or a
// pictograph after an emoji that ended in a zero width joiner. Combining
// marks (width 0) always join; this is the rest.
bool termJoinsCluster(const std::string& cell, uint32_t cp);

// The shell integration's marks (OSC 133, OSC 633 and OSC 7 bodies) count
// only when they end in ";mc=<nonce>", once a nonce is set (see
// TerminalStream::setMarkNonce). False for a mark without it, which is
// someone else's and dropped; otherwise true, with the body less the suffix
// in `body` (other OSC bodies pass unchanged).
bool termMarkAccepted(std::string& body, const std::string& nonce);

// The directory an OSC 7 body names ("7;file://host/path", or a bare
// "7;/path"), percent-decoded; empty when the body is not a usable OSC 7.
std::string termOsc7Path(const std::string& body);

// Apply an SGR parameter string (the body of "CSI ... m") to a style.
// Underline colors (SGR 58) are interned in `underlineColors`; without a
// table they are ignored.
void applySgr(TermStyle& style, const std::string& params,
              TermColorTable* underlineColors = nullptr);

// The most bytes a cell keeps: its character plus combining marks. Marks
// past it are dropped, as xterm keeps only a few, since a program printing
// megabytes of U+0301 after one letter otherwise grew a single cell that
// every frame copied and laid out (and, past 16 MiB in one line, overflowed
// the scrollback's packed offsets). 64 bytes hold a flag's tag sequence,
// the skin-toned family emoji (four people, four modifiers and three
// joiners, 41 bytes), and any real script's marks.
constexpr size_t kTermMaxCellBytes = 64;

// A window title as a program sent it (UTF-8), made safe to show in a tab:
// C0 and C1 controls, DEL, and the bidirectional and zero-width format
// characters dropped (U+202E would reorder the label), at most `maxBytes`,
// cut at a character's start.
std::string termCleanTitle(const std::string& text, size_t maxBytes = 256);

// The DEC/ANSI escape-sequence parser (the state machine Paul Williams
// documented on vt100.net), trimmed to what MiniCode needs. It decodes UTF-8,
// recognizes every sequence so none leaks into the text, and hands each piece
// to a Handler. Input may be split anywhere across feed() calls.
class TermParser {
public:
    struct Handler {
        virtual ~Handler() = default;
        // A printable character (U+FFFD for invalid UTF-8).
        virtual void print(uint32_t cp, const std::string& utf8) = 0;
        // A C0 control byte other than ESC, or DEL.
        virtual void control(unsigned char c) = 0;
        // CSI: `params` is everything between "ESC [" and the final byte,
        // including a private marker ("?25") and intermediates (" q").
        virtual void csi(const std::string& params, unsigned char final) = 0;
        // Other escapes: "ESC 7" has no intermediates, "ESC ( B" has "(".
        virtual void esc(const std::string& intermediates, unsigned char final) = 0;
        // An OSC body, terminated by BEL or ST.
        virtual void osc(const std::string& body) = 0;
    };

    void feed(const char* data, size_t length, Handler& h);

    // Limits on a CSI sequence: at most kMaxCsiParams parameters and
    // sub-parameters (xterm keeps 30), and kMaxCsiBytes in all. Past either
    // the sequence is read to its end and dropped. A count, not a byte
    // limit, decides, so a long truecolor SGR (foreground, background and
    // underline color in the colon form) still applies.
    static constexpr size_t kMaxCsiParams = 32;
    static constexpr size_t kMaxCsiBytes = 512;

    // Split CSI parameters on ';'. Each parameter keeps its ':' sub-parameters.
    // Empty values are -1 (meaning "default"). Other bytes are skipped.
    static std::vector<std::vector<int>> params(const std::string& s);

    // Whether the OSC being handed to Handler::osc ended with BEL (else ST),
    // so a reply can end the way the query did, as xterm's do.
    bool oscEndedWithBel() const { return oscBel_; }

private:
    enum class State { Ground, Escape, EscapeIntermediate, Csi, Osc, OscEscape,
                       String, StringEscape };
    State state_ = State::Ground;
    std::string osc_;            // body of the OSC sequence being read
    std::string seq_;            // CSI parameters, or ESC intermediates
    bool csiOverflow_ = false;
    bool oscBel_ = false;
    size_t csiParams_ = 0;       // separators in seq_ so far, plus one
    std::string utf8_;           // bytes of a UTF-8 character in progress
    int utf8Need_ = 0;           // total bytes that character needs

    void finishUtf8(bool abandon, Handler& h);
};

struct TermEvent {
    enum Kind {
        Line,            // the current line's full content (see `ended`)
        PromptStart,     // OSC 133;A — the shell is ready for a command
        CommandStart,    // OSC 133;C — a command began running
        CommandEnd,      // OSC 133;D[;status] — it finished
        Directory,       // OSC 7 — the shell's working directory changed
        InputStart,      // OSC 133;B — the prompt is drawn and the line
                         // editor reads input (sent by the grid mode's zsh
                         // integration once ZLE has started)
        Title,           // OSC 0 or 2 — a program named the window (text;
                         // empty to clear it)
    };
    Kind kind;
    // Line: the whole current line as styled runs. When `ended` is false the
    // line is still live and a later Line event replaces it; when true it was
    // finished by a newline and the next Line event is a new line.
    std::vector<TermRun> runs;
    bool ended = false;
    std::string text;    // Directory: the decoded path. CommandStart: the
                         // command line, when an OSC 633;E;<line> came
                         // before it (the grid mode's zsh integration sends
                         // one from preexec), else empty
    int status = 0;      // CommandEnd: exit status (0 if not reported)
};

class TerminalStream {
public:
    // Feed the next chunk of output. Anything incomplete at the end of the
    // chunk (an escape sequence, half a UTF-8 character) is held back and
    // finished by a later call. A Line event for the live line is emitted at
    // the end of the chunk if it changed.
    std::vector<TermEvent> feed(const char* data, size_t length);
    std::vector<TermEvent> feed(const std::string& s) {
        return feed(s.data(), s.size());
    }

    // The host wrote its own text below the live line: start a fresh, empty
    // line without emitting anything. The current style is kept.
    void breakLine();

    // True while the program is drawing on the alternate screen, whose text
    // is kept out of the log.
    bool altScreen() const { return alt_; }

    // The shell integration's marks (OSC 133, OSC 633;E and OSC 7) count
    // only when they end in ";mc=<nonce>", once a nonce is set: any program
    // can print a mark (a remote shell with iTerm2's integration, a server
    // logging a request path, cat of a file), and the host types commands
    // into the terminal on the strength of them. The host makes a nonce per
    // shell and hands it to the integration, which never exports it. Marks
    // without it are dropped whole. Empty (the default): every mark counts.
    void setMarkNonce(const std::string& nonce) { nonce_ = nonce; }

    // Events only: no Line events and no line kept, for a host that shows
    // the grid (TerminalScreen) and wants only where the shell is. Grid mode
    // threw away a third of the parse time building lines here.
    void setEventsOnly(bool on) { eventsOnly_ = on; if (on) breakLine(); }

    // Longest a line may grow before it is ended automatically, so one huge
    // unbroken line doesn't get re-rendered on every chunk.
    static constexpr size_t kMaxLineCells = 8192;

private:
    struct Cell {
        std::string ch;          // one code point, plus any combining marks
        TermStyle style;
    };
    struct Sink;                 // routes the parser's callbacks here

    TermParser parser_;
    TermStyle style_;
    std::vector<Cell> line_;
    size_t col_ = 0;
    bool dirty_ = false;         // the live line changed since last emitted
    bool alt_ = false;           // on the alternate screen
    std::string commandLine_;    // from OSC 633;E, for the next CommandStart
    std::string nonce_;          // marks must carry it (setMarkNonce)
    bool eventsOnly_ = false;

    void printCodepoint(uint32_t cp, const std::string& utf8);
    void newline(std::vector<TermEvent>& out);
    void flushLine(std::vector<TermEvent>& out);
    void moveTo(size_t col);
    void control(unsigned char c, std::vector<TermEvent>& out);
    void finishCsi(const std::string& params, unsigned char final);
    void finishOsc(const std::string& body, std::vector<TermEvent>& out);
};
