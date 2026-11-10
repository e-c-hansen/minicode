// BufferText.cpp — see BufferText.h.
#include "BufferText.h"

#include "BufferLines.h"
#include "Document.h"
#include "SyntaxHighlighter.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace BufferText {

std::u16string units(const char* s, size_t n) {
    std::u16string out;
    out.reserve(n);
    for (size_t i = 0; i < n;) {
        const unsigned char c = (unsigned char)s[i];
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else if (c >= 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if (c >= 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if (c >= 0x80) {
            len = 1;   // a stray continuation byte: a unit of its own, as LineIndex counts
            cp = 0xFFFD;
        }
        for (size_t k = 1; k < len; k++) {
            if (i + k >= n || ((unsigned char)s[i + k] & 0xC0) != 0x80) {
                len = k;
                cp = 0xFFFD;
                break;
            }
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        }
        out += cp > 0xFFFF ? char16_t(0xFFFD) : char16_t(cp);
        i += len;
    }
    return out;
}

std::u16string slice(GtkTextBuffer* buffer, size_t from, size_t to) {
    if (to <= from) return std::u16string();
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(buffer, &a, (int)from);
    gtk_text_buffer_get_iter_at_offset(buffer, &b, (int)to);
    // A slice, not the text: an embedded object is a character (U+FFFC)
    // there, so the count stays the buffer's.
    char* raw = gtk_text_buffer_get_slice(buffer, &a, &b, TRUE);
    std::u16string out = units(raw ? raw : "", raw ? strlen(raw) : 0);
    g_free(raw);
    out.resize(to - from, u' ');
    return out;
}

MultiCaret::Text text(GtkTextBuffer* buffer) {
    const size_t n = (size_t)gtk_text_buffer_get_char_count(buffer);
    return MultiCaret::Text(n, [buffer](size_t start, size_t length, char16_t* out) {
        const std::u16string s = slice(buffer, start, start + length);
        std::copy(s.begin(), s.end(), out);
    });
}

std::unique_ptr<AutoClose::Context> context(Document* d, GtkTextBuffer* buffer) {
    IncrementalHighlighter<char>* hl = d ? d->highlighter() : nullptr;
    BufferLines* lines = BufferLines::of(buffer);
    if (!hl || !lines || d->buffer() != buffer || hl->lineCount() != lines->lineCount() ||
        hl->length() != d->highlighterText().size() ||
        lines->index().length() != (size_t)gtk_text_buffer_get_char_count(buffer))
        return std::make_unique<AutoClose::Context>();
    return std::make_unique<AutoClose::Utf8HighlighterContext>(
        *hl, std::make_unique<StringSource<char>>(d->highlighterText()), lines->index());
}

}  // namespace BufferText
