// LargeFile.h — pure C++, header only. When a file is too large for the
// editor's per-character features, with VS Code's numbers
// (src/vs/editor/common/model/textModel.ts):
//
//   - A text over 20 MB (20 * 1024 * 1024 UTF-16 units, as VS Code counts a
//     JavaScript string) or over 300,000 lines opens in large-file mode
//     (editor.large-file-optimizations, on by default, as VS Code's
//     editor.largeFileOptimizations): no syntax colors, folding or bracket
//     boxes, which VS Code also turns off there ("tokenization, wrapping,
//     folding, codelens, word highlighting and sticky scroll"), and no
//     language server (VS Code still syncs up to 50 MB, but its
//     servers get incremental changes; here each didChange is the whole
//     text). Word wrap is the one feature left as set: VS Code turns it off
//     for its own view's sake, while a TextKit view that does not wrap must
//     lay out all of the text to know its width (a 25 MB file took 5.5 s to
//     open that way, nine tenths of it in sizeToFit; 0.4 s wrapped).
//   - Past 50 MB no language server gets the document even with the mode
//     turned off: VS Code's _MODEL_SYNC_LIMIT, past which extensions never
//     see a model.
//   - Opening a file over 50 MB by hand asks first (Open Anyway, Cancel).
//     VS Code asks only past 1 GB locally (workbench.editorLargeFileConfirmation),
//     but its piece tree is not one NSString in a TextKit view: here a
//     60 MB file takes 1.3 s to read and show even in large-file mode, and
//     the memory grows with it.
//   - Lines over 20,000 units are never lexed (kMaxTokenizationLineLength in
//     SyntaxHighlighter.h, VS Code's editor.maxTokenizationLineLength).
//
// The GTK port (linux/src/LargeFile.cpp) reads files as UTF-8 and its buffer
// counts characters, so the same tests come in UTF-8 below, with two things
// of its own:
//   - A file is refused past a size of the port's choosing (openingFor): its
//     windows share one process, and a GtkTextBuffer costs several times a
//     file's size.
//   - A line over 10,000 characters is shown 10,000 characters at a time
//     (shownPart), VS Code's editor.stopRenderingLineAfter. GtkTextView lays
//     out a line as one Pango layout: a 5 MB minified line held the main loop
//     for seconds on open and on every keystroke, and past about 260,000
//     characters (2^31 Pango units of width) it is not even drawn right. The
//     rest of the line stays in the buffer, hidden, and the part shown
//     follows the caret.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace LargeFile {

constexpr size_t kSizeThreshold = 20 * 1024 * 1024;     // UTF-16 units
constexpr size_t kLineCountThreshold = 300 * 1000;
constexpr size_t kSyncLimit = 50 * 1024 * 1024;         // UTF-16 units
constexpr uint64_t kAskBeforeOpeningBytes = 50ull * 1024 * 1024;

// The lines of text[0, n), counted as LineIndex counts them (a line ends
// after '\n', a '\r' not before '\n', or U+2029; a text ending in a break
// has an empty last line), but no further than `stopAt` lines: the caller
// only wants to know whether there are more than a threshold.
inline size_t countLines(const char16_t* text, size_t n, size_t stopAt) {
    size_t lines = 1;
    for (size_t i = 0; i < n && lines <= stopAt; i++) {
        const char16_t c = text[i];
        if (c == u'\n' || c == 0x2029 || (c == u'\r' && (i + 1 >= n || text[i + 1] != u'\n')))
            lines++;
    }
    return lines;
}

// Whether a text of `length` UTF-16 units and `lines` lines is a large file
// (VS Code's _isTooLargeForTokenization).
inline bool isLarge(size_t length, size_t lines) {
    return length > kSizeThreshold || lines > kLineCountThreshold;
}

// Whether text[0, n) is a large file. Counts lines only when it could hold
// more than the threshold, and stops counting there.
inline bool isLargeText(const char16_t* text, size_t n) {
    if (n > kSizeThreshold) return true;
    if (n < kLineCountThreshold) return false;   // n units hold at most n + 1 lines
    return countLines(text, n, kLineCountThreshold) > kLineCountThreshold;
}

// What a file opened with `optimizations` (the setting) gets, and whether
// the user turned the features on for its tab (`forced`).
struct Mode {
    bool large = false;          // over the thresholds
    bool featuresOff = false;    // large, with the setting on, not forced
    bool languageServer = true;  // a server may have the document
};
inline Mode modeFor(size_t length, bool large, bool optimizations, bool forced) {
    Mode m;
    m.large = large;
    m.featuresOff = large && optimizations && !forced;
    m.languageServer = !m.featuresOff && length <= kSyncLimit;
    return m;
}

// ---- UTF-8 (the GTK port)

// UTF-16 units of the valid UTF-8 text[0, n): one a character, two for a
// character past U+FFFF (a four-byte sequence), as VS Code counts.
inline size_t utf16Length(const char* text, size_t n) {
    size_t units = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)text[i];
        if ((c & 0xC0) != 0x80) units += c >= 0xF0 ? 2 : 1;
    }
    return units;
}

// Whether a UTF-8 line break ends at text[i]: '\n', a '\r' not before '\n',
// or U+2029 (E2 80 A9, ending at its last byte). Returns the break's bytes,
// counted back from i, or 0.
inline size_t breakEndingAt(const char* text, size_t n, size_t i) {
    const unsigned char c = (unsigned char)text[i];
    if (c == '\n') return i > 0 && text[i - 1] == '\r' ? 2 : 1;
    if (c == '\r') return i + 1 < n && text[i + 1] == '\n' ? 0 : 1;
    if (c == 0xA9 && i >= 2 && (unsigned char)text[i - 1] == 0x80 &&
        (unsigned char)text[i - 2] == 0xE2)
        return 3;
    return 0;
}

// countLines over UTF-8, lines ending as breakEndingAt says.
inline size_t countLinesUtf8(const char* text, size_t n, size_t stopAt) {
    size_t lines = 1;
    for (size_t i = 0; i < n && lines <= stopAt; i++)
        if (breakEndingAt(text, n, i)) lines++;
    return lines;
}

// isLargeText over UTF-8. Lines are counted only when there could be more
// than the threshold of them (n bytes hold at most n + 1 lines).
inline bool isLargeUtf8(const char* text, size_t n) {
    if (n > kSizeThreshold && utf16Length(text, n) > kSizeThreshold) return true;
    if (n < kLineCountThreshold) return false;
    return countLinesUtf8(text, n, kLineCountThreshold) > kLineCountThreshold;
}

// Opening a file of `bytes` by hand: at once, after asking (over 50 MB), or
// not at all (over `maxBytes`, the port's limit).
enum class Opening { Open, Ask, TooLarge };
inline Opening openingFor(uint64_t bytes, uint64_t maxBytes) {
    if (bytes > maxBytes) return Opening::TooLarge;
    if (bytes > kAskBeforeOpeningBytes) return Opening::Ask;
    return Opening::Open;
}

// Where the piece of the UTF-8 text[0, n) that starts at `at` ends, when a
// big file is put in its buffer a piece at a time (the GTK port's
// Document::putPieces): about `piece` bytes, cut after the last '\n' in
// them, so "\r\n" is never parted (GTK takes a '\n' put in after a lone
// '\r' for a break of its own); with none in them, inside a long line, up
// to the line's end or `linePiece` bytes, whichever is first, at the start
// of a character and never between "\r\n". GTK joins a piece put in at a
// line's end to the text before it, copying all of it, so pieces of a long
// line are bigger. Always past `at` while at < n.
inline size_t pieceEnd(const char* text, size_t n, size_t at, size_t piece, size_t linePiece) {
    size_t end = at + piece < n ? at + piece : n;
    if (end >= n) return n;
    size_t cut = end;
    while (cut > at && text[cut - 1] != '\n') cut--;
    if (cut > at) return cut;
    const size_t far = at + linePiece < n ? at + linePiece : n;
    end = far;
    for (size_t i = at + piece; i < far; i++)
        if (text[i] == '\n') {
            end = i + 1;
            break;
        }
    auto parts = [&](size_t e) {   // a cut at e would part a character or "\r\n"
        return e < n && (((unsigned char)text[e] & 0xC0) == 0x80 ||
                         (text[e - 1] == '\r' && text[e] == '\n'));
    };
    while (end > at + 1 && parts(end)) end--;
    while (parts(end)) end++;   // nothing earlier will do: the next place that does
    return end;
}

// ---- Long lines (the GTK port)

constexpr size_t kLongLineShown = 10000;   // characters, VS Code's stopRenderingLineAfter
constexpr size_t kNoCaret = (size_t)-1;

// The part [start, end) of a line of `length` characters that is shown: all
// of a line up to kLongLineShown, else kLongLineShown characters from
// `start`, moved only when `caret` (a position in the line, 0 to length, or
// kNoCaret) is outside it, to put the caret in its middle; a part that would
// run past the line's end (the line got shorter) moves back.
struct Shown {
    size_t start = 0, end = 0;
};
inline Shown shownPart(size_t length, size_t start, size_t caret = kNoCaret) {
    Shown s;
    if (length <= kLongLineShown) {
        s.end = length;
        return s;
    }
    if (caret != kNoCaret && (caret < start || caret > start + kLongLineShown))
        start = caret > kLongLineShown / 2 ? caret - kLongLineShown / 2 : 0;
    if (start > length - kLongLineShown) start = length - kLongLineShown;
    s.start = start;
    s.end = start + kLongLineShown;
    return s;
}

// The lines of the UTF-8 text[0, n) with more than `minChars` characters, as
// (line number in the text from 0, characters without its break), lines
// ending as breakEndingAt says. A line never holds more characters than
// bytes, so only lines longer than minChars bytes are counted through.
inline std::vector<std::pair<size_t, size_t>> longLinesUtf8(const char* text, size_t n,
                                                             size_t minChars) {
    std::vector<std::pair<size_t, size_t>> out;
    size_t line = 0, start = 0;
    auto close = [&](size_t end) {
        if (end - start <= minChars) return;
        size_t chars = 0;
        for (size_t i = start; i < end; i++)
            if (((unsigned char)text[i] & 0xC0) != 0x80) chars++;
        if (chars > minChars) out.push_back({line, chars});
    };
    for (size_t i = 0; i < n; i++) {
        const size_t brk = breakEndingAt(text, n, i);
        if (!brk) continue;
        close(i + 1 - brk);
        start = i + 1;
        line++;
    }
    close(n);
    return out;
}

// What the editor writes after the part of a long line it shows, for the
// characters left out before and after it: "4,990,000 more characters", or
// with some before, "2,490,000 more characters, 2,500,000 before".
inline std::string hiddenNote(size_t before, size_t after) {
    auto grouped = [](size_t v) {
        const std::string d = std::to_string(v);
        std::string out;
        for (size_t i = 0; i < d.size(); i++) {
            if (i && (d.size() - i) % 3 == 0) out += ',';
            out += d[i];
        }
        return out;
    };
    const std::string more = grouped(after) + (after == 1 ? " more character" : " more characters");
    if (!before) return more;
    if (!after) return grouped(before) + (before == 1 ? " character before" : " characters before");
    return more + ", " + grouped(before) + " before";
}

}  // namespace LargeFile
