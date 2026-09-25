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

#include <cstddef>
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

// ------------------------------------------------ UTF-16 <-> characters
// The shared LineComments and Settings editing helpers work in UTF-16 code
// units (they line up with NSString on macOS). GtkTextBuffer counts
// characters, which differ only where a character outside the Basic
// Multilingual Plane (most emoji) takes two UTF-16 units.

namespace utf16 {

inline bool isHigh(char16_t c) { return c >= 0xD800 && c <= 0xDBFF; }
inline bool isLow(char16_t c)  { return c >= 0xDC00 && c <= 0xDFFF; }

// Character offset of UTF-16 offset `u16`. An offset between the halves of a
// pair counts as the character's start.
inline long toCharOffset(const std::u16string& s, size_t u16) {
    long chars = 0;
    size_t i = 0;
    while (i < u16 && i < s.size()) {
        size_t step = (isHigh(s[i]) && i + 1 < s.size() && isLow(s[i + 1])) ? 2 : 1;
        if (i + step > u16) break;
        i += step;
        ++chars;
    }
    return chars;
}

// UTF-16 offset of character offset `chars` (clamped to the end).
inline size_t fromCharOffset(const std::u16string& s, long chars) {
    size_t i = 0;
    for (long n = 0; n < chars && i < s.size(); ++n)
        i += (isHigh(s[i]) && i + 1 < s.size() && isLow(s[i + 1])) ? 2 : 1;
    return i;
}

// Decode UTF-8. Invalid bytes become U+FFFD, one per byte, so offsets stay
// predictable (GtkTextBuffer only ever holds valid UTF-8 anyway).
inline std::u16string fromUtf8(const std::string& in) {
    std::u16string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = (unsigned char)in[i];
        char32_t cp;
        size_t len;
        if (c < 0x80)                { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { out += u'\uFFFD'; ++i; continue; }
        bool ok = i + len <= in.size();
        for (size_t k = 1; ok && k < len; ++k) {
            unsigned char cc = (unsigned char)in[i + k];
            if ((cc & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { out += u'\uFFFD'; ++i; continue; }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += (char16_t)(0xD800 + (cp >> 10));
            out += (char16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out += (char16_t)cp;
        }
        i += len;
    }
    return out;
}

// Byte offset in `in` of UTF-16 offset `u16`, by the same decoding rules as
// fromUtf8 (an invalid byte is one unit), clamped to the end. An offset
// between the halves of a surrogate pair counts as the character's start.
// Walking the bytes directly keeps it inside the string: counting characters
// and then stepping with g_utf8_offset_to_pointer, which trusts each lead
// byte's length, ran past the end of a line holding invalid UTF-8.
inline size_t byteOffsetOfUtf16(const std::string& in, size_t u16) {
    size_t i = 0, units = 0;
    while (i < in.size()) {
        unsigned char c = (unsigned char)in[i];
        size_t len;
        if (c < 0x80)                len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else                         len = 0;   // invalid lead byte
        bool ok = len > 0 && i + len <= in.size();
        for (size_t k = 1; ok && k < len; ++k)
            if (((unsigned char)in[i + k] & 0xC0) != 0x80) ok = false;
        size_t width = 1;   // UTF-16 units this character takes
        if (!ok) len = 1;
        else if (len == 4) width = 2;
        if (units + width > u16) break;
        units += width;
        i += len;
    }
    return i;
}

inline std::string toUtf8(const std::u16string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char32_t cp = in[i];
        if (isHigh(in[i]) && i + 1 < in.size() && isLow(in[i + 1])) {
            cp = 0x10000 + ((in[i] - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            ++i;
        } else if (isHigh(in[i]) || isLow(in[i])) {
            cp = 0xFFFD;   // an unpaired surrogate
        }
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

}  // namespace utf16
