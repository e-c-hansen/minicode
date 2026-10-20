// AutoIndent.h — pure C++. What Return, Tab, Shift+Tab and Backspace do to
// indentation, and the dedent a few typed characters cause, decided from the
// text around the caret and the file's language.
//
// Works in UTF-16 code units, like LineComments and MarkdownTasks, so results
// map straight onto NSRange and Java strings (the GTK port converts). Every
// result is one replacement, so an editor applies it as one undo step.
//
// The rules, by language (languageFor):
//   Python   indent after a line ending in ':' (outside strings, comments and
//            brackets); inside an open bracket align with the first thing
//            after it, or indent one level when the bracket ends its line;
//            dedent after return, pass, break, continue and raise; a
//            backslash continuation indents once; inside a triple-quoted
//            string only the line's indentation is kept. The next line's
//            indentation comes from the first line of the statement, so a
//            call spread over several lines does not leave the next statement
//            aligned with its arguments.
//   CLike    (C, C++, Java, JS, TS, Go, Rust, Swift, JSON, CSS, ...) indent
//            after a line ending in an opening bracket; after closing
//            brackets opened on earlier lines, back to the line that opened
//            them; inside /* */ continue the comment with " * ".
//   Yaml     indent after a line ending in ':' (or a block scalar's | or >).
//   Generic  any other language with line comments (shell, Ruby, Lua, SQL,
//            TOML, ...): the bracket rules only.
//   Plain    keep the line's indentation, as before.
// Everywhere but Plain, Return between a bracket pair, (|), opens it with the
// closer on its own line, a moved line starting with a closer goes to its
// opener's indentation, trailing spaces before the caret are dropped, and the
// moved text loses its leading spaces.
#pragma once
#include <cstddef>
#include <string>

namespace AutoIndent {

enum class Lang { Plain, Python, CLike, Yaml, Generic };

// The rules a file gets, by its name (a path is fine).
Lang languageFor(const std::string& filename);

// One level of indentation.
struct Unit {
    bool tabs = false;
    int width = 4;          // spaces per level; for tabs, the columns a tab takes
    bool fromFile = false;  // the file showed it; false when it is the default
    std::u16string text() const;   // "\t" or `width` spaces
};

// The file's indentation, guessed the way editors do: tabs or spaces by the
// lines that start with each, and the width by the most common step between
// one line's indentation and the next deeper one. Defaults when the file has
// none: 4 spaces (2 for YAML), tabs for Go and Makefiles.
Unit detect(const std::u16string& text, const std::string& filename);

// One replacement: [replaceStart, replaceStart + replaceLength) becomes
// `replacement`, and the selection moves to [selStart, selEnd) in the new
// text. changed == false: let the editor do its usual thing.
struct Edit {
    bool changed = false;
    size_t replaceStart = 0, replaceLength = 0;
    std::u16string replacement;
    size_t selStart = 0, selEnd = 0;
};

// Return with [selStart, selEnd) selected (the selection is replaced).
Edit newline(const std::u16string& text, size_t selStart, size_t selEnd,
             const std::string& filename);
Edit newline(const std::u16string& text, size_t selStart, size_t selEnd,
             const std::string& filename, const Unit& unit);

// After a character was typed, with the caret just after it: a closer
// ( ) ] } ) typed on an otherwise blank line moves to its opener's
// indentation, and in Python a ':' ending "else", "elif ...", "except ...",
// "finally" or "case ..." moves the line back to the indentation of the
// if/for/while/try/case it belongs to, when it sits deeper than that.
Edit typed(const std::u16string& text, size_t caret, const std::string& filename);
Edit typed(const std::u16string& text, size_t caret, const std::string& filename,
           const Unit& unit);

// Tab: with a selection over more than one line, indent those lines one
// level; otherwise, when the file indents with spaces, insert spaces to the
// next tab stop. changed == false means a plain tab character.
Edit tab(const std::u16string& text, size_t selStart, size_t selEnd,
         const std::string& filename);
Edit tab(const std::u16string& text, size_t selStart, size_t selEnd,
         const std::string& filename, const Unit& unit);

// Shift+Tab: outdent the lines the selection (or the caret) touches by one
// level.
Edit backtab(const std::u16string& text, size_t selStart, size_t selEnd,
             const std::string& filename);
Edit backtab(const std::u16string& text, size_t selStart, size_t selEnd,
             const std::string& filename, const Unit& unit);

// Backspace with no selection, in a line's leading spaces, in a file that
// indents with spaces: delete back to the previous tab stop.
Edit backspace(const std::u16string& text, size_t selStart, size_t selEnd,
               const std::string& filename);
Edit backspace(const std::u16string& text, size_t selStart, size_t selEnd,
               const std::string& filename, const Unit& unit);

}  // namespace AutoIndent
