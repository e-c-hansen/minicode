// InlayHints.h — pure C++. Inlay hints (textDocument/inlayHint): a type
// after a name (`x: int`), a return type after a signature (`-> str`), a
// parameter's name before an argument (`count=`), drawn in the text without
// being part of it. LspClient sends the request; this reads the answer and
// keeps the hints on their text while it is edited, as VS Code's
// editor.inlayHints does, and the Mac's editor (LspInlayHints.mm) draws
// them in gaps it opens between characters.
#pragma once
#include "LspClient.h"
#include <cstddef>
#include <string>
#include <vector>

namespace Lsp {

enum class InlayHintKind { Other = 0, Type = 1, Parameter = 2 };

struct InlayHint {
    Position position;
    std::string label;   // a string label, or its parts' labels joined
    InlayHintKind kind = InlayHintKind::Other;
    // A little space before or after the label (VS Code's is a third of the
    // font size), which the server asks for when the hint would otherwise
    // touch the text: basedpyright pads `x: int`'s label on neither side
    // and a parameter's `count=` on the right.
    bool paddingLeft = false, paddingRight = false;
    std::string tooltip;   // a string or a MarkupContent's value; may be empty
};

// textDocument/inlayHint's answer (InlayHint[] or null), sorted by
// position. A hint without a position or with an empty label is left out.
std::vector<InlayHint> parseInlayHints(const Json& result);

// VS Code's editor.inlayHints.maximumLength (43 by default; 0 never cuts):
// on each line the labels are counted in order, and the one that takes the
// line's total past the limit ends in "…" where the limit falls; hints
// after it on that line are dropped. Counted in UTF-16 units, as VS Code
// counts, never cutting a character in two. `hints` is sorted by position.
void shortenInlayLabels(std::vector<InlayHint>& hints, size_t maximumLength = 43);

// A hint placed in the editor's text: at `offset` (UTF-16 units), between
// the characters before and after it. `followsBefore`: it belongs to the
// word before it (a name's type), so typing right at it pushes it along, as
// VS Code's word-attached hints do; else it belongs to what comes after it
// (an argument's parameter name), and typing there goes after it.
struct PlacedInlayHint {
    size_t offset = 0;
    bool followsBefore = false;
    size_t hint = 0;   // index into the hints placed
};

// Where hints fall in a window of the text: `window` is the text from
// `base` (an offset at the start of line `firstLine`) to the end of a line.
// Hints outside the window are left out, and so are hints at the start of
// a line, which have no character before them on their line to open a gap
// after. A position past its line's end is taken as the line's end.
std::vector<PlacedInlayHint> placeInlayHints(const std::vector<InlayHint>& hints,
                                             const std::u16string& window, size_t base,
                                             int firstLine);

// Whether a character is part of a word for the attaching rule above:
// letters, digits, the underscore, and anything outside ASCII.
bool inlayWordChar(char16_t c);

// Keeps a hint on its text through an edit: [start, start + oldLen)
// replaced by newLen units. Edits before it move it, an edit ending right at
// it leaves it at the end of the new text, an insertion right at it moves
// it past what was inserted only when it follows the text before it, and an
// edit after it leaves it alone. False when the edit took out the
// characters on both sides of it: the hint goes until the server says again.
bool inlayFollowEdit(size_t& offset, bool followsBefore, size_t start, size_t oldLen,
                     size_t newLen);

// Counts LSP line breaks ("\r\n", "\r", "\n") in text fed piece by piece,
// so the line of an offset in a long file is found without a copy of all
// that comes before it.
class LineBreakCounter {
public:
    void feed(const char16_t* s, size_t n);
    size_t lines() const { return lines_; }

private:
    size_t lines_ = 0;
    bool cr_ = false;   // the last unit fed was '\r'
};

}  // namespace Lsp
