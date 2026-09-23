// TermLinks.h — pure C++. Finds what is clickable in a line of terminal
// output: URLs, and file references such as `src/foo.cpp:42:7` from a
// compiler, `File "app.py", line 12` from Python, or `foo.ts(42,7)` from
// TypeScript and MSVC.
//
// It only reads text. Whether a path names a real file depends on where the
// shell was, so each port resolves a FileLink against the shell's directory
// and the project root and drops the ones that do not exist.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace TermLinks {

struct Link {
    enum Kind { Url, File } kind;
    size_t start, length;   // the clickable span, in bytes of the line
    std::string target;     // the URL, or the path as written
    int line = 0;           // 1-based, 0 when the reference has none
    int column = 0;         // 1-based, 0 when the reference has none
};

// Every link in `line` (UTF-8, no newline), left to right, not overlapping.
std::vector<Link> find(const std::string& line);

// The link covering byte `offset` of `line`, if any.
const Link* at(const std::vector<Link>& links, size_t offset);

}  // namespace TermLinks
