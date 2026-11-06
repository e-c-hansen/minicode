// MermaidDiagram.h — pure C++. The decisions behind Mermaid diagrams in the
// Markdown preview, with no I/O: which runs of a page are a ```mermaid
// block, a diagram's cache key, the mmdc command line, and what of mmdc's
// complaint to show.
//
// minicode draws no diagram itself and bundles no Mermaid code (it has no
// third-party code at all): a page's diagrams are drawn by mermaid-cli's
// `mmdc` when the user has it installed, as its math is by tectonic, and
// show as their source in the code style otherwise (MarkdownMermaid.mm,
// macOS; Linux and Android show the source).
#pragma once
#include "MarkdownParser.h"
#include <string>
#include <vector>

namespace MermaidDiagram {

// Bumped whenever the command line or what is drawn changes: it is in
// every cache key.
extern const int kVersion;

// A ```mermaid block of a page: runs [first, end) of the parser's output,
// its source (the lines between the fences), and its first source line.
struct Block {
    size_t first = 0, end = 0;
    std::string source;
    int line = -1;
};
std::vector<Block> blocks(const std::vector<MdRun> &runs);
// Whether a run is a line of a ```mermaid block.
bool isMermaidRun(const MdRun &r);

// The theme mmdc draws in, for a dark or a light preview.
std::string theme(bool dark);
// A hex key for the diagram: kVersion, the theme and the source.
std::string key(const std::string &source, const std::string &theme);

// The pixels per point a diagram is drawn at (sharp on a Retina screen).
extern const int kScale;
// mmdc's arguments (after the program): the source file in, a transparent
// PNG out at kScale, the theme, quiet.
std::vector<std::string> arguments(const std::string &input, const std::string &output,
                                   const std::string &theme);

// What mmdc printed when it failed, made short for a tooltip: its
// "Error: " message without the stack trace under it (at most 6 lines),
// else the last line that says anything, else `fallback`.
std::string errorSummary(const std::string &stderrText, const std::string &fallback);

}  // namespace MermaidDiagram
