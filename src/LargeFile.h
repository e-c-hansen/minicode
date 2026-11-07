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
#pragma once
#include <cstddef>
#include <cstdint>

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

}  // namespace LargeFile
