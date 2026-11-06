// Notebook.h — pure C++. A Jupyter notebook (.ipynb, nbformat 4, and the
// cells of nbformat 3) read for showing, never run: no kernel. The
// notebook becomes Markdown that the Markdown preview renders as it is:
// Markdown cells as written (their attached pictures inlined), code cells
// as fenced blocks in the notebook's language (so they are highlighted),
// and outputs after them: text streams, results and errors (ANSI colors
// stripped) as plain blocks (a long one cut in the middle), pictures (SVG
// too) as data: images, Markdown results as Markdown, and HTML results as
// their plain text alternative, or the HTML's text when there is none.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace Notebook {

struct Output {
    enum class Kind { Stream, Text, Image, Markdown, Error };
    Kind kind = Kind::Text;
    std::string name;     // a stream's: "stdout" or "stderr"
    std::string text;     // the text (for an image, its plain alternative)
    std::string mime;     // an image's: "image/png", "image/jpeg", "image/gif", "image/svg+xml"
    std::string base64;   // an image's data, without whitespace
};

struct Cell {
    enum class Kind { Markdown, Code, Raw };
    Kind kind = Kind::Code;
    std::string source;
    long executionCount = -1;   // -1: never run
    std::vector<Output> outputs;
};

struct Document {
    std::vector<Cell> cells;
    std::string language;   // the kernel's ("python"), "" when not given
    int nbformat = 4;
};

// Reads a notebook. NO, with a reason, when it is not one.
bool parse(const std::string &json, Document &out, std::string *error = nullptr);

// The Markdown the preview shows for a notebook.
std::string toMarkdown(const Document &doc);
// The same from the file's text, saying so when it cannot be read.
std::string markdownForFile(const std::string &json);

// Helpers, tested on their own.
std::string stripAnsi(const std::string &s);
std::string htmlToText(const std::string &html);
// A text output cut for showing when it runs past kOutputMaxLines lines or
// kOutputMaxBytes: its first and last kOutputKeepLines lines (each end at
// most kOutputKeepBytes), with a line between saying how many were left
// out. Shorter text comes back as it is.
constexpr size_t kOutputMaxLines = 5000, kOutputKeepLines = 1000;
constexpr size_t kOutputMaxBytes = 512 * 1024, kOutputKeepBytes = 128 * 1024;
std::string shortenOutput(const std::string &text);
// A code fence longer than any run of backticks in `text` (at least ```).
std::string fenceFor(const std::string &text);

}  // namespace Notebook
