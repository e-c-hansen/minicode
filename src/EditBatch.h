// EditBatch.h — the edits of one action at several places (typing at many
// carets, Replace All), given to an IncrementalHighlighter as what they are,
// many small edits, rather than as one edit from the first to the last.
//
// The GTK port's Document used to take such a batch as one edit spanning
// it, read back from the buffer whole: 1,000 carets spread over a
// 100,000-line file re-lexed and retagged every line between, about a
// second of CPU for a keystroke, and copying the buffer out took 55 ms of it
// (linux/PARITY.md, D7). Here the changes, taken in bytes before the editor
// applied them, make the new text from the old in one pass, and the
// highlighter takes them together (IncrementalHighlighter::editMany): each
// re-lexes only its own lines, and the line table moves once.
//
// Bytes (UTF-8), as the GTK port's highlighter mirror counts.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "SyntaxHighlighter.h"

namespace EditBatch {

// One replacement, in the text as it was before the batch.
struct Change {
    size_t pos = 0;      // bytes from the start
    size_t oldLen = 0;   // bytes replaced
    std::string text;    // what replaces them
};

// Ascending, apart (one may end where the next starts), and inside a text
// of `length` bytes.
bool valid(const std::vector<Change>& changes, size_t length);

// The text with the changes made.
std::string applied(const std::string& base, const std::vector<Change>& changes);

// What the highlighter re-lexed, one range at a time in the text's order,
// as edit() reports one edit, so a caller keeping more than the
// highlighter (the bracket depths) can take each as such: the text after
// the batch, the range, a line count before it, and its tokens. The count
// is the one edit()'s caller works from: the range's lines replaced
// linesBefore + (its lines) - hl.lineCount() old ones, hl.lineCount() being
// the count after the whole batch.
using Step = std::function<void(const TextSource<char>& now,
                                const IncrementalHighlighter<char>::Range& range,
                                size_t linesBefore, const std::vector<Token>& relexed)>;

// The batch given to `hl`, which must describe `base`; `step`, if set, for
// each range re-lexed. Returns the text after the batch. The changes must
// be valid().
std::string apply(IncrementalHighlighter<char>& hl, const std::string& base,
                  const std::vector<Change>& changes, const Step& step = Step());

}  // namespace EditBatch
