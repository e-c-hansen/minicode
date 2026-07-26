// Utf8Offsets.h — byte offset -> character offset, for driving GtkTextBuffer.
//
// SyntaxHighlighter reports {start, length} as BYTE offsets into the UTF-8
// source. GtkTextBuffer iterators are indexed by CHARACTER. Converting between
// them is the seam where the Linux port is most likely to put colors on the
// wrong spans, so the conversion lives here, in plain C++ with no GTK, and is
// unit-tested by linux/tests/run_tests.cpp.
//
// The lexer is a single left-to-right scan, so its tokens arrive in ascending,
// non-overlapping order. That lets one forward-only cursor convert the entire
// token stream in O(n) total. Counting from the start of the string for every
// token instead (what g_utf8_pointer_to_offset would do) is O(n) per token and
// O(n^2) over the file, which stalls the UI on large sources.
#pragma once

#include <string>

class Utf8OffsetCursor {
public:
    explicit Utf8OffsetCursor(const std::string& text) : text_(text) {}

    // Character offset corresponding to `byte`. Requests are expected to
    // ascend; a backward request is still answered correctly, just by
    // restarting the scan. Offsets past the end clamp to the end.
    long charOffset(long byte) {
        const long n = static_cast<long>(text_.size());
        if (byte < 0) byte = 0;
        if (byte > n) byte = n;
        if (byte < bytePos_) { bytePos_ = 0; charPos_ = 0; }  // defensive rescan
        while (bytePos_ < byte) {
            // Every byte that is not a UTF-8 continuation byte (10xxxxxx)
            // starts a new character.
            if (!isContinuation(text_[bytePos_])) ++charPos_;
            ++bytePos_;
        }
        return charPos_;
    }

    // Total character count of the string. Leaves the cursor at the end.
    long totalChars() { return charOffset(static_cast<long>(text_.size())); }

private:
    static bool isContinuation(char c) {
        return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
    }

    const std::string& text_;
    long bytePos_ = 0;
    long charPos_ = 0;
};
