// ShellIntegration.h — pure C++, header only. What minicode puts in the
// folder its terminals' zsh gets as ZDOTDIR (one per process and mode,
// folderName): a copy of the integration script
// (resources/minicode-zsh-integration.zsh, in the bundle's Resources and
// beside the executable in build/) and a .zshenv that names the
// terminal's mode and sources that copy. The script's protocol (the marks,
// their nonce, the private keys below) is the app's, so the copy is the
// running app's own, never a newer bundle's script read later.
//
// tests/zsh_integration_tests.inc runs the script in a real zsh through this
// stub, so the test and the app start it the same way.
#pragma once
#include <cstdlib>
#include <string>

namespace ShellIntegration {

// The folder, in the user's temporary folder, that a process's terminals of
// one mode get as ZDOTDIR: "MiniCode-zsh-grid-<pid>-<start>" (grid mode) or
// "MiniCode-zsh-<pid>-<start>" (line mode), with the time the process
// started in whole seconds, against pid reuse. One per process, so two
// minicodes running at once (the Homebrew copy and a build, or the old and
// new process of a restart) never source each other's script (older
// minicodes all wrote their copy into one shared folder).
inline std::string folderName(bool grid, long long pid, long long start) {
    return std::string(grid ? "MiniCode-zsh-grid-" : "MiniCode-zsh-") + std::to_string(pid) +
           "-" + std::to_string(start);
}

// The process a folder's name gives; false for any other name (the shared
// folders older minicodes write, "MiniCode-zsh-grid" and "MiniCode-zsh",
// included: an older process may still be using them).
inline bool parseFolderName(const std::string& name, long long* pid, long long* start) {
    std::string rest;
    for (const char* prefix : {"MiniCode-zsh-grid-", "MiniCode-zsh-"}) {
        const std::string p = prefix;
        if (name.compare(0, p.size(), p) == 0) { rest = name.substr(p.size()); break; }
    }
    const size_t dash = rest.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= rest.size()) return false;
    const std::string a = rest.substr(0, dash), b = rest.substr(dash + 1);
    for (const std::string* part : {&a, &b}) {
        if (part->size() > 18) return false;
        for (char c : *part)
            if (c < '0' || c > '9') return false;
    }
    *pid = std::strtoll(a.c_str(), nullptr, 10);
    *start = std::strtoll(b.c_str(), nullptr, 10);
    return *pid > 0;
}

// Whether the process a folder names has gone: `startNow` is when the
// process now holding that pid started (Unix seconds, 0 for none), so a
// pid a later process reuses counts as gone too.
inline bool ownerGone(long long start, double startNow) {
    if (startNow <= 0) return true;
    const double d = startNow - (double)start;
    return d < -1.0 || d > 1.0;
}

// The script's file name, in Resources (or build/) and in the ZDOTDIR folder.
inline const char *const kZshScriptName = "minicode-zsh-integration.zsh";

// Grid mode's private keys, sent only while ZLE reads input; the script
// binds each in emacs, viins and vicmd (zsh_integration checks it).
//   push-input (the user's half-typed line comes back at the next prompt,
//   so a command the app types never runs together with it)
inline const char *const kZlePushKey = "\x1b[9999~";
//   Cmd+Left, Cmd+Right and Cmd+Delete: the line's start and end, delete
//   back to its start (in vi's modes too, where Ctrl+A, Ctrl+E and Ctrl+U
//   do something else), and Cmd+K: clear-screen, so a prompt of several
//   lines is drawn again whole.
inline const char *const kZleLineStartKey = "\x1b[9998~";
inline const char *const kZleLineEndKey = "\x1b[9997~";
inline const char *const kZleKillToStartKey = "\x1b[9996~";
inline const char *const kZleClearKey = "\x1b[9995~";

// The .zshenv zsh reads first from ZDOTDIR. It is read with ZDOTDIR at its
// own folder, which is where the script's copy is.
inline std::string zshenvStub(bool grid) {
    std::string s = "# Written by minicode for its terminal panel. Safe to delete.\n";
    s += "typeset -g __minicode_mode=";
    s += grid ? "grid" : "line";
    s += "\nbuiltin source \"$ZDOTDIR/";
    s += kZshScriptName;
    s += "\"\n";
    return s;
}

}  // namespace ShellIntegration
