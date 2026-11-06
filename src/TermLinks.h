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

// What an OSC 8 hyperlink's address opens (TerminalScreen keeps the
// address): an http or https address is a Url with the address as its
// target; a file address on this computer (no host, "localhost", or
// `localHost`, the name gethostname gives, compared without case and with
// or without ".local") is a File with the path percent-decoded. Anything
// else, a file on another host included, gives false.
bool hyperlink(const std::string& uri, const std::string& localHost, Link& out);

// The address to load for a web address from the terminal. A server that
// listens on every interface prints 0.0.0.0 or [::] (Python's http.server
// does), which a browser will not load, so those become localhost, as VS
// Code does; anything else is returned as it is.
std::string browserAddress(const std::string& url);

}  // namespace TermLinks
