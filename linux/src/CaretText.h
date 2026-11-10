// CaretText.h — a GtkTextBuffer as the core's caret and line commands
// (MultiCaret.h) read it, for Carets.h: one unit a character, as GtkTextIter
// counts and as BufferText.h reads it, so the core's offsets are the
// buffer's. BufferText gives a character outside the Basic Multilingual
// Plane (an emoji) as U+FFFD, which is all the rules need to know of it;
// but Move Line, Copy Line and Paste write back text they read, and an
// emoji moved a line down must stay one. Here each such character, and
// each of the Private Use Area that it could be mistaken for, is read as a
// Private Use Area unit kept in a table for the one command, and written
// back from it.
#pragma once

#include <gtk/gtk.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "MultiCaret.h"

class CaretText {
public:
    explicit CaretText(GtkTextBuffer* buffer) : buffer_(buffer) {}
    CaretText(const CaretText&) = delete;
    CaretText& operator=(const CaretText&) = delete;

    // The whole buffer, read in blocks where a command looks. It must not
    // outlive this, nor the buffer's text as it is now.
    MultiCaret::Text text();
    // The units of `n` characters from `from`, read now.
    std::u16string slice(size_t from, size_t to);
    // UTF-8 as units, and units back as UTF-8.
    std::u16string units(const std::string& utf8);
    std::string utf8(const std::u16string& units) const;
    // More than the table holds (6,400 such characters in one command):
    // the text written back would be wrong, so the command must not run.
    bool full() const { return full_; }

private:
    char16_t unitOf(uint32_t cp);

    GtkTextBuffer* buffer_;
    std::unordered_map<uint32_t, char16_t> slot_;
    std::vector<uint32_t> back_;
    bool full_ = false;
};
