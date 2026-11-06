// SymbolOutline.h — pure C++. Go to Symbol in File (Shift+Cmd+O) for a file
// no language server answers for: an outline read from the text alone.
//
//   Markdown   its headings (ATX and setext; none inside code), nested by level
//   LaTeX      \part, \chapter, \section ... \subparagraph, nested by level
//              (comments and verbatim environments left out)
//   Python     class and def, nested by indentation (a def in a class is a
//              method); strings, comments and lines inside brackets skipped
//   C family   functions and the types and namespaces around them, read from
//              the text before each opening brace that is not inside a
//              function: C, C++, Objective-C, Java, C#, JavaScript,
//              TypeScript, Go, Rust, Swift, Kotlin, Scala, Dart, PHP
//
// It is a reader, not a parser: it finds what is plain to find and misses
// the rest (a Python function made by assignment, a JavaScript arrow
// function, a C function behind a macro), and never fails. Symbols come out
// as a language server's would (Lsp::Symbol: parent first, the file's
// order, 0-based lines and UTF-16 columns), so the palette treats both alike.
// Headings are kind String, as VS Code's Markdown outline has them.
#pragma once
#include "LspClient.h"

#include <string>
#include <vector>

namespace Outline {

enum class Language { None, Markdown, Latex, Python, CLike };
// By the file name's extension (lowercase or not); None for anything else.
Language languageFor(const std::string& fileName);

// The outline of a file's text, by its name; empty for a language with none.
std::vector<Lsp::Symbol> outline(const std::string& fileName, const std::u16string& text);

std::vector<Lsp::Symbol> markdown(const std::u16string& text);
std::vector<Lsp::Symbol> latex(const std::u16string& text);
std::vector<Lsp::Symbol> python(const std::u16string& text);
// Go reads `type Name struct` and method receivers; in Rust a quote that
// starts no character literal is a lifetime ('a); elsewhere single quotes
// make strings.
enum class Dialect { Plain, Go, Rust };
std::vector<Lsp::Symbol> clike(const std::u16string& text, Dialect dialect = Dialect::Plain);

}  // namespace Outline
