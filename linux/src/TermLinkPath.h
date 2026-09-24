// TermLinkPath.h — pure C++ (POSIX only, no GTK). Turns a file reference that
// the shared TermLinks core found in terminal output into a real path, the
// way the macOS terminal does (Terminal.mm -resolvePath:isDirectory:).
//
// TermLinks only reads text; whether "src/foo.cpp" names a file depends on
// where the shell was. A relative path is tried against the shell's current
// directory first and then the project folder; "~/" is the home directory.
// Only something that exists resolves, so "e.g." or a word that happens to
// contain a dot never becomes a link.
#pragma once

#include <string>

namespace TermLinkPath {

struct Target {
    std::string path;          // absolute, and in the root's spelling when inside it
    bool isDir = false;
    bool insideRoot = false;   // the path is the project folder or below it
};

// `written` is the path as the terminal showed it. `cwd` and `root` may be
// empty. `home` is what "~" means ("" leaves a "~/" path unresolved).
bool resolve(const std::string& written, const std::string& cwd,
             const std::string& root, const std::string& home, Target* out);

}  // namespace TermLinkPath
