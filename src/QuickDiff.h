// QuickDiff.h — pure C++, part of the portable core. The change bars in the
// editor's gutter, as VS Code's quick diff ("dirty diff") draws them: which
// lines of the buffer were added or changed against the version git has
// for the file, and where lines were deleted; moving them as the buffer is
// edited until the next diff; and the text that puts one change back
// (Revert Change) or takes it into the index (Stage Change).
//
// The original is the file's index version (`git show :path`), as VS
// Code's git extension gives it (its quickDiffProvider's ref '' is the
// index), so a staged change shows no bar and Stage Change makes one go.
//
// Lines are counted as the editor counts them: split at "\n", the text after
// the last "\n" (empty after a final newline) being a line too. A "\r"
// before a "\n" is ignored when comparing, so a work tree written with
// CRLF against an index holding LF shows no bars. Offsets are UTF-16 code
// units, as NSString's. Tested in tests/quick_diff_tests.inc (`quickdiff:*`).
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace QuickDiff {

enum class Kind { Added, Modified, Deleted };

// One run of changed lines. Buffer lines [line, line + lines) were added or
// changed; for Deleted, lines is 0 and the original's lines went from just
// above buffer line `line` (drawn at the bottom of line - 1, or at the top
// of line 0). The same run in the original is [oldLine, oldLine + oldLines).
struct Bar {
    Kind kind = Kind::Modified;
    size_t line = 0, lines = 0;
    size_t oldLine = 0, oldLines = 0;
    bool operator==(const Bar& o) const {
        return kind == o.kind && line == o.line && lines == o.lines && oldLine == o.oldLine &&
               oldLines == o.oldLines;
    }
};

// The bars of `buffer` against `original`, in order. Empty when they are
// the same (line endings aside).
std::vector<Bar> bars(const std::u16string& original, const std::u16string& buffer);

// The line a bar is drawn from and Next Change goes to: its first line, or
// for a deletion the line above it (line 0 for one at the start).
size_t anchorLine(const Bar& b);

// The bar drawn on `line`, or -1.
int barAt(const std::vector<Bar>& bars, size_t line);

// Next Change and Previous Change (VS Code's workbench.action.editor.
// nextChange and previousChange): the first bar whose anchor is below the
// caret's line, or above it; wrapping at either end. -1 with no bars.
int nextBar(const std::vector<Bar>& bars, size_t caretLine);
int previousBar(const std::vector<Bar>& bars, size_t caretLine);

// An edit to the buffer between diffs: it began on line `line` and took
// away `removed` line breaks and put in `added`. Bars below it move by the
// difference; a bar the edit is in grows or shrinks with it. The next diff
// (0.3 s after typing stops) puts everything right.
void shift(std::vector<Bar>& bars, size_t line, size_t removed, size_t added);

// Replace [start, start + length) of a text with `text`.
struct Edit {
    size_t start = 0, length = 0;
    std::u16string text;
};

// Revert Change: the buffer edit that puts the bar's lines back as the
// original has them.
Edit revertEdit(const Bar& b, const std::u16string& original, const std::u16string& buffer);

// Stage Change: the original (the index) with this one change of the buffer
// in it, whole, for `git hash-object`. Lines taken from the buffer get the
// original's line endings when the original has only one kind.
std::u16string stagedText(const Bar& b, const std::u16string& original,
                          const std::u16string& buffer);

// "3 lines added", "1 line changed", "2 lines deleted": the popover's title.
std::string describe(const Bar& b);

// The original's lines of a bar, as they were (the popover shows them),
// without their line breaks.
std::vector<std::u16string> oldLines(const Bar& b, const std::u16string& original);

// The byte order mark. A tab keeps its file's UTF-8 BOM apart from the
// text it edits (FolderSearch::stripUtf8Bom, written back on save), while
// the index's text has it as a first U+FEFF; compared as it is, every line
// 0 read as changed and Revert Change put a second BOM in the file.
// stripBom takes the original's U+FEFF away when the buffer's file kept
// one apart (`bufferBom`), and says whether it did; restoreBom puts it in
// front of stagedText's result again, so the index keeps its BOM. A BOM
// in the index that the file on disk lost is a change, and stays one.
bool stripBom(std::u16string& original, bool bufferBom);
void restoreBom(std::u16string& staged, bool stripped);

// UTF-8 to UTF-16 (invalid bytes become U+FFFD), for the original git prints.
std::u16string fromUtf8(const std::string& s);
// UTF-16 to UTF-8 (a lone surrogate becomes U+FFFD), for what is staged.
std::string toUtf8(const std::u16string& s);

}  // namespace QuickDiff
