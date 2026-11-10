// BufferText.h — a GtkTextBuffer as the core's editing rules read it,
// linux/PARITY.md items 1.7 and 1.8: one unit a character, as GtkTextIter
// counts, so the core's offsets are the buffer's (U+FFFD stands for a
// character outside the BMP, which the rules only ever ask whether it is a
// bracket, a quote, a blank or a break; AutoClose.h). And the document's
// strings and comments, from its highlighter, in the same units.
#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <memory>
#include <string>

#include "AutoClose.h"
#include "MultiCaret.h"

class Document;

namespace BufferText {

// UTF-8 text as one unit a code point.
std::u16string units(const char* utf8, size_t bytes);
// Characters [from, to) of the buffer, the same way.
std::u16string slice(GtkTextBuffer* buffer, size_t from, size_t to);
// The whole buffer for MultiCaret and AutoClose, read in blocks where they
// look. It must not outlive the buffer, nor be used after the buffer changes.
MultiCaret::Text text(GtkTextBuffer* buffer);
// What `d`'s highlighter knows of strings and comments in `buffer` (its
// source), in characters; the plain Context (everything code) when it has
// no grammar or is not in step with the buffer just now. It reads the
// document's highlighter and must not outlive the next edit.
std::unique_ptr<AutoClose::Context> context(Document* d, GtkTextBuffer* buffer);

}  // namespace BufferText
