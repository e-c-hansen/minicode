// MermaidDiagram.h — pure C++. The decisions behind Mermaid diagrams in the
// Markdown preview, with no I/O: which runs of a page are a ```mermaid
// block, a diagram's cache key, the mmdc command line, and what of mmdc's
// complaint to show.
//
// minicode draws no diagram itself and bundles no Mermaid code (it has no
// third-party code at all): a page's diagrams are drawn by mermaid-cli's
// `mmdc` when the user has it installed, else by one pinned mermaid.js the
// user downloads from the page (checked against kMermaidJsSha256, as
// tectonic is), run in a WebKit view that loads nothing else, and show as
// their source in the code style otherwise (MarkdownMermaid.mm, macOS;
// Linux and Android show the source).
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
// What drew a diagram, which its key holds: mermaid-cli's mmdc, or the
// pinned mermaid.js.
extern const char *const kDrawerMmdc;
extern const char *const kDrawerMermaidJs;   // "mermaid.js <kMermaidJsVersion>"

// A hex key for the diagram: kVersion, the drawer, the theme and the source.
std::string key(const std::string &source, const std::string &theme,
                const std::string &drawer = kDrawerMmdc);

// The mermaid.js minicode downloads on request: its version, where from
// (the npm package's file, through jsDelivr's mirror of npm), and the
// SHA-256 of mermaid.min.js in the npm package mermaid@12.3.0, which the
// download must match (5,537,838 bytes, checked against the registry's
// tarball on 2026-11-15).
extern const char *const kMermaidJsVersion;
extern const char *const kMermaidJsUrl;
extern const char *const kMermaidJsSha256;
extern const long kMermaidJsBytes;

// The body of the async function the WebKit view runs for one diagram,
// given `source` and `theme`: mermaid.render with plain SVG text labels
// (htmlLabels off, so NSImage can draw the result; HTML labels are
// foreignObject, which it leaves out), strict security, and the SVG's
// width and height set from its viewBox, its style (a max-width, a
// background) dropped. It returns the SVG as text.
std::string renderScript();

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
