// SpellCheck.h — pure C++, no dependencies. Which parts of a text are
// prose a spell checker may read (editor.spell-check). The ports hand the
// words found here, and nothing else, to the system's checker (the Mac's
// NSSpellChecker), so code, keywords, identifiers, paths and addresses are
// never marked.
//
// What counts as prose, by the kind of file:
//   Markdown  paragraphs, headings, list items, quotes and table cells, but
//             not code spans, fenced or indented code, $...$ and $$ math,
//             links' addresses, autolinks, HTML tags or hidden lines
//             (reference definitions, comments), as MarkdownParser reads
//             the blocks.
//   LaTeX     text and comments, but not commands, the option arguments
//             after a command, the arguments of commands that name things
//             (\label, \ref, \cite, \usepackage, \includegraphics, ...),
//             math, verbatim bodies or \url, as the highlighter's tokens say.
//   Code      comments and docstrings, and a string's text only when it
//             reads as a sentence (two words with a space between, nothing
//             that looks like a format, a regex or markup).
//   Plain     every line (.txt and files with no extension).
//   Commit    a commit message: every line.
// Within prose, a word is checked only when it is made of letters (with an
// apostrophe or hyphen inside): anything holding a digit, an underscore, a
// slash, a dot between letters, an @, brackets or other symbols, camelCase
// or two capitals (identifiers, paths, addresses, acronyms) is left out.
#pragma once
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

struct Token;   // SyntaxHighlighter.h

namespace Spell {

// editor.spell-check: off, prose-only (the default: Markdown, LaTeX, and
// commit messages), or on (comments, docstrings, and plain text too).
enum class Mode { Off, ProseOnly, On };
enum class Kind { None, Markdown, Latex, Code, Plain, Commit };

// The kind of a file by its lowercase extension without the dot ("md",
// "tex", "py"); `hasGrammar` says whether the highlighter knows the
// extension (SyntaxHighlighter::supports), which makes it code.
Kind kindForExtension(const std::string& ext, bool hasGrammar);
// Whether `mode` checks text of `kind` at all.
bool checks(Mode mode, Kind kind);

// [start, end) in UTF-16 code units.
using Span = std::pair<size_t, size_t>;

// The words worth checking in the prose spans of `text`, ascending.
std::vector<Span> words(const std::u16string& text, const std::vector<Span>& prose);

// The prose of text from `from` on, for each kind. `tokens` (offsets into
// `text`) are the highlighter's for the whole lines of [from, text.size())
// and matter for Latex and Code only. Markdown reads text from its start,
// since a fence or a math block opened above `from` decides what follows.
std::vector<Span> prose(Kind kind, const std::u16string& text, size_t from,
                        const std::vector<Token>& tokens);

// Both: the words to check in [from, text.size()).
std::vector<Span> checkable(Kind kind, const std::u16string& text, size_t from,
                            const std::vector<Token>& tokens);

}  // namespace Spell
