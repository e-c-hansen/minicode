// SemanticColors.h — a language server's semantic tokens painted over one
// document's lexer colors, linux/PARITY.md item 2.6: VS Code's
// editor.semanticHighlighting, as the Mac's EditorController+Highlighting.mm
// paints them. The server says what each name is (a class, a parameter, a
// property...), which the lexer cannot know; LspSessionSemantic.cpp asks for
// the tokens and hands them here, decoded (Lsp::decodeSemanticTokens) and in
// UTF-8 bytes of the text (Lsp::semanticSpansInUtf8).
//
// - The tokens are kept only while the characters are unchanged: any
//   character edit ends them (edited). Their tags stay where they are and
//   move with their text, until the next answer; the lines an edit touches
//   are retagged by the lexer (Document::retagLines), which takes the
//   semantic tags off those lines (lexed), so they show the lexer's colors
//   at once.
// - The colors are six tags in the shared tag table (class, function,
//   variable, parameter, property, constant: Lsp::SemanticStyle less None),
//   placed just above the lexer's tags, so they win over its colors and lose
//   to the bracket colors (BracketColors.h) and everything else. Function is
//   the lexer's own syntax.function; the others are syntax.class, .variable,
//   .parameter, .property and .constant (Theme::semantic).
// - Only the lines on screen are painted (paintVisible): when the tokens
//   arrive and as lines scroll into view (BracketView's scroll hook), each
//   line once while the tokens are current. A line's tags are taken off and
//   its spans put on, cut where the lexer has a string, an escape or a
//   comment (Lsp::semanticSpansOverCode), never over those.
// - None in large-file mode (no highlighter), without a grammar, or with
//   editor.semantic-highlighting off (clear takes every tag off).
//
// Document owns one per buffer.
#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "LspClient.h"
#include "SyntaxHighlighter.h"

class Settings;

class SemanticColors {
public:
    explicit SemanticColors(GtkTextBuffer* buffer);
    SemanticColors(const SemanticColors&) = delete;
    SemanticColors& operator=(const SemanticColors&) = delete;

    // The tag for a style (null for None), made on first use in `table`,
    // which every document shares.
    static GtkTextTag* tag(GtkTextTagTable* table, Lsp::SemanticStyle style);
    // Whether `t` is one of the six.
    static bool isTag(GtkTextTagTable* table, GtkTextTag* t);
    // The tags' colors from the settings (the theme's, or the file's own).
    static void applySettings(GtkTextTagTable* table, const Settings& s);

    // The server's tokens for the text as it is now, in UTF-8 bytes of it
    // (ascending and apart): kept, nothing painted yet.
    void set(std::vector<Lsp::SemanticSpan> spans);
    // A character edit: the tokens end; the tags stay.
    void edited();
    // Off: the tokens and every tag gone.
    void clear();
    // The lexer retagged [a, b), its lines [firstLine, endLine): their
    // semantic tags come off, and they are painted again while the tokens
    // are current.
    void lexed(const GtkTextIter* a, const GtkTextIter* b, size_t firstLine, size_t endLine);
    // The lines `view` has on screen that are not painted yet, painted.
    void paintVisible(GtkTextView* view, IncrementalHighlighter<char>& hl,
                      const std::string& text);

    bool current() const { return current_; }
    // For the checks: the tokens kept, the lines painted since the document
    // opened, whether a line is painted, and the CPU time painting took on
    // the main thread (ms).
    size_t spanCount() const { return spans_.size(); }
    size_t linesPainted() const { return linesPainted_; }
    bool painted(size_t line) const { return current_ && line < painted_.size() && painted_[line]; }
    static double cpuMs();

private:
    void takeOff(const GtkTextIter* a, const GtkTextIter* b);
    void paintLine(size_t line, IncrementalHighlighter<char>& hl, const std::string& text);

    GtkTextBuffer* buffer_;
    bool current_ = false;
    bool any_ = false;   // a tag may be on the buffer
    bool painting_ = false;
    std::vector<Lsp::SemanticSpan> spans_;
    std::vector<bool> painted_;   // by line, while current
    size_t linesPainted_ = 0;
};
