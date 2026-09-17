// LineComments.h — pure C++. Cmd+/ : comment or uncomment the lines a
// selection touches, the way most editors do it.
//
// Works in UTF-16 code units so offsets line up with NSString/NSRange; the
// markers and indentation are ASCII, so nothing here needs to decode text.
#pragma once
#include <cstddef>
#include <string>

namespace LineComments {

// The line-comment marker for a file ("#", "//", "--", ";"), chosen by
// extension or, for files like Makefile, by name. Empty when unknown or the
// format has no line comments (JSON, Markdown).
std::string markerFor(const std::string& filename);

struct Result {
    std::u16string text;         // the whole text, edited
    size_t selStart, selEnd;     // the selection, moved to follow the edit
    bool changed;
    // The same edit as one replacement: the touched lines in the original
    // text, [replaceStart, replaceStart + replaceLength), become `replacement`.
    size_t replaceStart, replaceLength;
    std::u16string replacement;
};

// Toggle comments on every line touched by [selStart, selEnd). If every
// non-blank line is already commented they are uncommented (the marker and
// one following space removed); otherwise "marker " is inserted at the
// smallest indentation among them, so the block stays aligned. Blank lines
// are left alone unless every touched line is blank. A selection ending at
// the very start of a line doesn't include that line.
Result toggle(const std::u16string& text, size_t selStart, size_t selEnd,
              const std::string& marker);

}  // namespace LineComments
