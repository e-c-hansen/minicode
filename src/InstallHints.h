// InstallHints.h — pure C++. How to install the language servers and
// formatters MiniCode talks to but never ships: one table, so the app's
// notes (the bar shown when a file's server is missing, the Language Servers
// popover, Format Document's note) and the README say the same thing.
// MiniCode never runs these itself; it shows them, copies them, or types
// them into a terminal for the user to run.
//
// Each tool lists its install commands best first. A command that needs a
// program of its own (uv, pipx, npm, brew, go, rustup) says so, and
// `suggest` picks the first one this Mac can run, falling back to the best
// one plus how to get what it needs ("brew install uv").
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace Install {

struct Way {
    std::string command;   // "uv tool install basedpyright"
    std::string needs;     // a program that must be installed first, or ""
};

struct Tool {
    std::string name;       // "basedpyright"
    std::string gives;      // "completions, documentation on hover, ..."
    std::string homepage;   // https://...
    std::vector<Way> ways;  // best first
};

// The server MiniCode recommends for a language server key ("python",
// "cpp", "go", "rust", "typescript"), or nullptr.
const Tool* server(const std::string& key);
// A formatter by program name or path ("pyink", "/x/bin/clang-format"),
// or nullptr for one MiniCode does not know.
const Tool* formatter(const std::string& program);
// Every tool, the servers first, then the formatters.
std::vector<const Tool*> all();
// The server keys `server` knows, in the README's order.
const std::vector<std::string>& serverKeys();

// How to get a program that a way needs. `command` is "" when there is no
// one-line way (rustup, Homebrew itself); `url` always says where to read
// more. `needs` is what `command` itself needs ("brew").
struct Prerequisite {
    std::string program;
    std::string command;
    std::string needs;
    std::string url;
};
Prerequisite prerequisite(const std::string& program);

struct Suggestion {
    std::string command;              // the one to run
    std::string before;               // run this first ("brew install uv"), or ""
    std::string beforeUrl;            // or get the prerequisite from here, or ""
    std::string missing;              // the program `before` gets ("uv"), or ""
    std::vector<std::string> alternatives;   // the other ways, best first
};

// What to show for `tool`. `have` answers whether a program is installed.
// The first way whose prerequisite is installed (or needs none) is the
// command; when none can run, the first way is, with how to get what it
// needs. Every other way is an alternative, in the table's order.
// `extra` (other tools that would do, such as yapf beside pyink) adds each
// one's own suggested command as an alternative too.
Suggestion suggest(const Tool& tool, const std::function<bool(const std::string&)>& have,
                   const std::vector<const Tool*>& extra = {});

}  // namespace Install
