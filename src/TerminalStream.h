// TerminalStream.h — pure C++. Turns the raw bytes a shell writes to its pty
// into styled lines plus shell-integration events.
//
// It is a line model, not a screen emulator: it tracks colors and attributes
// (SGR) and the cursor within the current line (\r, \b, tab, erase-in-line,
// cursor forward/back/column), which covers colored output and progress lines.
// Movement to other lines, scroll regions and the alternate screen are
// recognized and ignored. The stream is parsed incrementally, so sequences
// and UTF-8 characters split across reads are handled.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
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

struct TermStyle {
    TermColor fg, bg;
    bool bold = false, dim = false, italic = false, underline = false,
         inverse = false, strike = false;
    bool operator==(const TermStyle& o) const {
        return fg == o.fg && bg == o.bg && bold == o.bold && dim == o.dim &&
               italic == o.italic && underline == o.underline &&
               inverse == o.inverse && strike == o.strike;
    }
    bool operator!=(const TermStyle& o) const { return !(*this == o); }
};

// A stretch of text in one style.
struct TermRun {
    std::string text;            // UTF-8
    TermStyle style;
};

struct TermEvent {
    enum Kind {
        Line,            // the current line's full content (see `ended`)
        PromptStart,     // OSC 133;A — the shell is ready for a command
        CommandStart,    // OSC 133;C — a command began running
        CommandEnd,      // OSC 133;D[;status] — it finished
        Directory,       // OSC 7 — the shell's working directory changed
    };
    Kind kind;
    // Line: the whole current line as styled runs. When `ended` is false the
    // line is still live and a later Line event replaces it; when true it was
    // finished by a newline and the next Line event is a new line.
    std::vector<TermRun> runs;
    bool ended = false;
    std::string text;    // Directory: the decoded path
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

    // Longest a line may grow before it is ended automatically, so one huge
    // unbroken line doesn't get re-rendered on every chunk.
    static constexpr size_t kMaxLineCells = 8192;

private:
    enum class State { Ground, Escape, EscapeIntermediate, Csi, Osc, OscEscape,
                       String, StringEscape };
    struct Cell {
        std::string ch;          // one code point, plus any combining marks
        TermStyle style;
    };

    State state_ = State::Ground;
    std::string osc_;            // body of the OSC sequence being read
    std::string csi_;            // parameters/intermediates of the CSI
    bool csiOverflow_ = false;
    std::string utf8_;           // bytes of a UTF-8 character in progress
    int utf8Need_ = 0;           // total bytes that character needs

    TermStyle style_;
    std::vector<Cell> line_;
    size_t col_ = 0;
    bool dirty_ = false;         // the live line changed since last emitted

    void printCodepoint(uint32_t cp, const std::string& utf8);
    void finishUtf8(bool abandon);
    void newline(std::vector<TermEvent>& out);
    void flushLine(std::vector<TermEvent>& out);
    void moveTo(size_t col);
    void finishCsi(unsigned char final);
    void applySgr();
    void finishOsc(std::vector<TermEvent>& out);
};
