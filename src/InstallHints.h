// InstallHints.h — pure C++. How to install the language servers and
// formatters MiniCode talks to but never ships: one table, so the app's
// notes (the bar shown when a file's server is missing, the Language Servers
// popover, Format Document's note) and the README say the same thing.
// MiniCode never runs these itself; it shows them, copies them, or types
// them into a terminal for the user to run.
//
// Each tool lists its install commands best first. A command that needs a
// program of its own (uv, pipx, npm, brew, go, rustup, apt, cargo, snap)
// says so, and `suggest` picks the first one this computer can run, falling
// back to the best one plus how to get what it needs ("brew install uv").
// A way is for the Mac, for Linux, or for both: Homebrew and the command
// line tools are the Mac's, apt and the Linux packages Linux's, and the
// Mac's choices are what they were before Linux had any.
#pragma once
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace Install {

enum class Os { Mac, Linux };
// The system this was built for: what `suggest` and `prerequisite` assume
// unless told.
#ifdef __APPLE__
constexpr Os kThisOs = Os::Mac;
#else
constexpr Os kThisOs = Os::Linux;
#endif

// Which systems a way is for.
enum : unsigned { kOnMac = 1, kOnLinux = 2, kOnBoth = kOnMac | kOnLinux };

struct Way {
    std::string command;   // "uv tool install basedpyright"
    std::string needs;     // a program that must be installed first, or ""
    unsigned on = kOnBoth;
};

struct Tool {
    std::string name;       // "basedpyright"
    std::string gives;      // "completions, documentation on hover, ..."
    std::string homepage;   // https://...
    std::vector<Way> ways;  // best first
};

// The server MiniCode recommends for a language server key ("python",
// "cpp", "go", "rust", "typescript"), or for a second server's key
// ("python.also": Ruff's), or nullptr.
const Tool* server(const std::string& key);
// A formatter by program name or path ("pyink", "/x/bin/clang-format"),
// or nullptr for one MiniCode does not know.
const Tool* formatter(const std::string& program);
// Every tool, the servers first, then the formatters.
std::vector<const Tool*> all();
// The main server keys `server` knows, in the README's order.
const std::vector<std::string>& serverKeys();
// A tool's ways on `os`, best first.
std::vector<Way> waysOn(const Tool& tool, Os os);
// How the words shown name a computer that lacks a program ("this Mac",
// "this computer").
const char* thisComputer(Os os = kThisOs);

// How to get a program that a way needs. `command` is "" when there is no
// one-line way (rustup, Homebrew itself); `url` always says where to read
// more. `needs` is what `command` itself needs ("brew").
struct Prerequisite {
    std::string program;
    std::string command;
    std::string needs;
    std::string url;
};
Prerequisite prerequisite(const std::string& program, Os os = kThisOs);

struct Suggestion {
    std::string command;              // the one to run
    std::string before;               // run this first ("brew install uv"), or ""
    std::string beforeUrl;            // or get the prerequisite from here, or ""
    std::string missing;              // the program `before` gets ("uv"), or ""
    std::vector<std::string> alternatives;   // the other ways, best first
};

// What to show for `tool` on `os`. `have` answers whether a program is
// installed. The first way whose prerequisite is installed (or needs none)
// is the command; when none can run, the first way is, with how to get what
// it needs. Every other way is an alternative, in the table's order.
// `extra` (other tools that would do, such as yapf beside pyink) adds each
// one's own suggested command as an alternative too. Only `os`'s ways count.
Suggestion suggest(const Tool& tool, const std::function<bool(const std::string&)>& have,
                   const std::vector<const Tool*>& extra = {}, Os os = kThisOs);

// Don't Show Again for the install bar, remembered per language server key:
// the Linux port keeps the keys as JSON in its state folder (the Mac in its
// defaults). `decodeMuted` reads what `encodeMuted` wrote, and anything
// else as nothing muted.
std::string encodeMuted(const std::set<std::string>& keys);
std::set<std::string> decodeMuted(const std::string& text);

}  // namespace Install
