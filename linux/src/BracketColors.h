// BracketColors.h — bracket-pair colors for one document, linux/PARITY.md
// item 1.7: VS Code's editor.bracketPairColorization, as the Mac's
// EditorController+Highlighting.mm paints them. Every bracket in code,
// ( ) [ ] { }, takes one of three colors by how deeply it is nested, and a
// closer with nothing open before it takes syntax.bracket-unexpected;
// brackets in strings, escapes and comments keep the lexer's colors. The
// rules and the bookkeeping are the core's BracketPairs.h; this file keeps
// them for a GtkTextBuffer and paints them as tags.
//
// - The depths: each line's BracketPairs::LineSum in a BracketPairs::Depths
//   (blocks of about 512 lines), set from every line's tokens when the
//   document's highlighter lexes it whole (reset), and after that from the
//   tokens the highlighter re-lexes for each edit (edited), so an edit
//   never reads the file again. Lines are the highlighter's, which are the
//   buffer's BufferLines (both end a line at the same characters).
// - The colors are four tags in the shared tag table, made after the
//   highlighter's, so they win over its colors. Only the lines on screen
//   are painted (paintVisible): when the document is shown, scrolled,
//   resized, or edited, as on the Mac, where painting every bracket at
//   open doubled the CPU of an open. Which lines are current is a set of
//   line ranges that follows the edits: an edit's own lines are no longer
//   current, and when it changed the depth the lines below it start at,
//   nothing from it on is, so the lines on screen are painted again at once
//   and the rest as they scroll into view. A line that is not current keeps
//   the tags it had, which move with its text, until it is seen.
// - Off (editor.bracket-pair-colorization = off, the settings file, a file
//   without a grammar, and so a large file once large-file mode drops its
//   highlighter) takes every bracket tag off the buffer.
//
// Document owns one per buffer and calls it from its highlighting
// (Document.cpp); the editor's BracketView asks it to paint on scrolls.
#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "BracketPairs.h"
#include "SyntaxHighlighter.h"

class Settings;

class BracketColors {
public:
    explicit BracketColors(GtkTextBuffer* buffer);
    BracketColors(const BracketColors&) = delete;
    BracketColors& operator=(const BracketColors&) = delete;

    // The four tags' colors (syntax.bracket-1 to 3, syntax.bracket-unexpected),
    // in the table every document shares; they are made there on first use.
    static void applySettings(GtkTextTagTable* table, const Settings& s);
    // The tag for color index i (BracketPairs::colorIndex), for the checks.
    static GtkTextTag* tag(GtkTextTagTable* table, int i);

    bool on() const { return on_; }
    // Start over. With a highlighter (lexed whole, `tokens` every line's in
    // order, `text` its UTF-8 source): on, nothing painted yet. Null: off.
    // Either way every bracket tag comes off the buffer.
    void reset(IncrementalHighlighter<char>* hl, const std::string& text,
               const std::vector<Token>& tokens);
    // The highlighter re-lexed lines [r.firstLine, r.endLine) for an edit
    // (their tokens `relexed`, in order); it had `linesBefore` lines before.
    // `text` is the source as it is after the edit (a batch's edits come
    // one at a time, each over the text as it then is: EditBatch.h).
    void edited(const IncrementalHighlighter<char>& hl, const TextSource<char>& text,
                const IncrementalHighlighter<char>::Range& r, size_t linesBefore,
                const std::vector<Token>& relexed);
    // The lines `view` has on screen that are not current, painted.
    void paintVisible(GtkTextView* view, IncrementalHighlighter<char>& hl,
                      const std::string& text);

    // For the checks: the lines painted since the document opened, whether
    // a line is current, and the CPU time painting and keeping the depths
    // took, on the main thread (ms).
    size_t linesPainted() const { return linesPainted_; }
    bool current(size_t line) const;
    static double cpuMs();

private:
    void takeAllOff();
    void paintLine(size_t line, int32_t depth, IncrementalHighlighter<char>& hl,
                   const std::string& text);
    void markCurrent(size_t first, size_t end);

    GtkTextBuffer* buffer_;
    bool on_ = false;
    BracketPairs::Depths depths_;
    // The lines whose bracket tags are current: ascending, disjoint [first, end).
    std::vector<std::pair<size_t, size_t>> current_;
    size_t linesPainted_ = 0;
    bool painting_ = false;
};
