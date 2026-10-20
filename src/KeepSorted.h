// KeepSorted.h — pure C++. Sorting the lines between "keep-sorted start" and
// "keep-sorted end" markers, the way Google's keep-sorted tool does
// (https://github.com/google/keep-sorted), so a file sorted here passes its
// check, and plain line sorting for a selection.
//
// The markers are found as substrings of a line, so they work inside any
// comment syntax ("// keep-sorted start", "# keep-sorted start case=no",
// "<!-- keep-sorted start -->"). Options follow "start" on the same line as
// key=value: case, numeric, skip_lines, remove_duplicates (yes, no,
// keep_first_comment, merge_comments), block, group, group_prefixes,
// sticky_comments, sticky_prefixes, newline_separated (yes, no or a count),
// prefix_order, ignore_prefixes, order (asc, desc) and allow_yaml_lists.
// Lists are comma-separated or a YAML flow sequence (["* ", "* ["]). The
// regular-expression options (by_regex, group_start_regex, group_end_regex)
// are not supported: a block using one is reported and left as it is, since
// sorting it without them would disagree with keep-sorted. An option keep-sorted
// itself does not know is reported and ignored, as keep-sorted does.
//
// Blocks nest, as in keep-sorted: inner blocks are sorted first. A marker
// without its partner is an error, and then nothing in the file is sorted.
// A block whose sorting would split a nested block apart is left alone.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace KeepSorted {

struct Message {
    int line;            // 0-based line of the marker it concerns
    std::string text;
};

struct Result {
    std::string text;            // the file, with what could be sorted sorted
    bool changed = false;
    int blocks = 0;              // blocks found, nested ones included
    std::vector<Message> errors;    // something was left as it is, and why
    std::vector<Message> warnings;  // an option was not understood
    // For each line of `text`, the line of the input it came from, or -1
    // for a blank line added by newline_separated.
    std::vector<int> origin;
};

// Every block in the file, as `keep-sorted file` would sort them. `id` is the
// marker's name ("keep-sorted" makes "keep-sorted start" and "keep-sorted end").
Result fixAll(const std::string& text, const std::string& id = "keep-sorted");

// The innermost block holding 0-based `line` (its markers included), and the
// blocks nested in it. An error when the line is in no block.
Result fixAt(const std::string& text, int line, const std::string& id = "keep-sorted");

// Lines [first, last] (0-based) sorted on their own: by the text after their
// leading spaces, byte by byte, then by the whole line; nothing grouped or
// removed.
Result sortLines(const std::string& text, int first, int last);

// The editor's commands, in UTF-16 code units like LineComments, so results
// map straight onto NSRange and Java strings.
struct Edit {
    bool changed = false;
    size_t replaceStart = 0, replaceLength = 0;
    std::u16string replacement;
    size_t selStart = 0, selEnd = 0;  // in the new text; the caret stays on its line
    bool error = false;               // something was left alone
    std::string message;              // what to tell the user, or ""
};

// Sort Lines: a selection over two or more lines sorts those lines plainly;
// otherwise the keep-sorted block the caret is in is sorted.
Edit sortCommand(const std::u16string& text, size_t selStart, size_t selEnd);

// Every keep-sorted block in the file.
Edit fixCommand(const std::u16string& text, size_t selStart, size_t selEnd);

// Whether the text holds a "keep-sorted start" at all (a quick test before
// sorting on save).
bool hasBlocks(const std::u16string& text);

}  // namespace KeepSorted
