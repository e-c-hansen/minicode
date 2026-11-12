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
//
// The Linux port's terminal is VTE, which reads no OSC 133 or 633
// (linux/PARITY.md D10): there the zsh script (with __minicode_term=vte in
// the stub) and the bash one (linux/resources/minicode-bash-integration.bash)
// print one string termprop the app installs, vte.ext.minicode.mark, whose
// value parseVteMark reads and ShellState keeps (linux/src/Terminal.cpp).
#pragma once
#include <cstddef>
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
// The Linux port's bash script (bash --rcfile), beside the zsh one.
inline const char *const kBashScriptName = "minicode-bash-integration.bash";

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
// own folder, which is where the script's copy is. `vte`: the Linux port's
// terminal, where the marks are the termprop below, not OSC 133.
inline std::string zshenvStub(bool grid, bool vte = false) {
    std::string s = "# Written by minicode for its terminal panel. Safe to delete.\n";
    s += "typeset -g __minicode_mode=";
    s += grid ? "grid" : "line";
    if (vte) s += "\ntypeset -g __minicode_term=vte";
    s += "\nbuiltin source \"$ZDOTDIR/";
    s += kZshScriptName;
    s += "\"\n";
    return s;
}

// A shell's nonce as the scripts get it in MINICODE_NONCE: the bytes (16,
// so 128 random bits) as lower-case hex.
inline std::string nonceHex(const unsigned char* raw, size_t n) {
    static const char hex[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += hex[raw[i] >> 4];
        s += hex[raw[i] & 15];
    }
    return s;
}

// ---------------------------------------------------------------- VTE marks
// The termprop the Linux port installs (vte_install_termprop, before any
// terminal exists) and the scripts set with
//   ESC ] 666 ; vte.ext.minicode.mark=p=<n>\sx=<n>\sd=<n>\ss=<status>\sc=<command>\smc=<nonce> ESC \ (ST)
// VTE splits OSC 666 at ';' and unescapes \s (;), \\ and \n in a string
// value, so the value the app reads is "p=..;x=..;d=..;s=..;c=..;mc=..": p
// counts prompts, x commands started, d commands finished (d is set to x
// when a command ends, so a line of three commands counts once), s is the
// last finished command's status (empty before the first), c the command's
// text (control characters blanked, at most 500 characters; empty when the
// shell does not know it). VTE keeps only the newest value of a read's
// parse, and a value equal to the one before fires nothing, so the state is
// read from the newest value alone and the counters make each one new.
// VTE refuses a value over 1,024 characters (the termprop goes unset) and
// a bad escape the same way.
inline const char *const kVteMarkTermprop = "vte.ext.minicode.mark";

struct VteMark {
    long long prompts = 0, started = 0, done = 0;
    int status = -1;   // -1: no command has finished yet
    std::string command;
};

namespace detail {
inline bool readCount(const std::string& v, size_t* at, const char* key, long long* out, bool allowEmpty) {
    const std::string k = key;
    if (v.compare(*at, k.size(), k) != 0) return false;
    size_t i = *at + k.size(), start = i;
    long long n = 0;
    while (i < v.size() && v[i] >= '0' && v[i] <= '9') {
        if (i - start >= 15) return false;
        n = n * 10 + (v[i] - '0');
        i++;
    }
    if (i == start && !allowEmpty) return false;
    if (i >= v.size() || v[i] != ';') return false;
    *out = i == start ? -1 : n;
    *at = i + 1;
    return true;
}
}  // namespace detail

// The mark in a termprop's value, only when it ends in ";mc=<nonce>" with
// this shell's nonce: anything a program prints without it (a file shown
// with cat, a remote shell, a forged printf) is false and changes nothing.
inline bool parseVteMark(const std::string& value, const std::string& nonce, VteMark* out) {
    if (nonce.empty()) return false;
    const std::string tail = ";mc=" + nonce;
    if (value.size() < tail.size() ||
        value.compare(value.size() - tail.size(), tail.size(), tail) != 0)
        return false;
    VteMark m;
    size_t at = 0;
    long long status = -1;
    if (!detail::readCount(value, &at, "p=", &m.prompts, false) ||
        !detail::readCount(value, &at, "x=", &m.started, false) ||
        !detail::readCount(value, &at, "d=", &m.done, false) ||
        !detail::readCount(value, &at, "s=", &status, true))
        return false;
    if (status > 255) return false;
    m.status = (int)status;
    const size_t end = value.size() - tail.size();
    if (at > end || value.compare(at, 2, "c=") != 0 || at + 2 > end) return false;
    m.command = value.substr(at + 2, end - at - 2);
    *out = m;
    return true;
}

// What a terminal knows of its shell from the newest mark. `known` is false
// until a mark with the nonce arrives (another shell, or one whose
// integration did not load, never marks: unknown, never idle).
struct ShellState {
    bool known = false;
    bool busy = false;        // a command started and has not finished
    bool atPrompt = false;    // the shell has drawn a prompt and runs nothing
    std::string command;      // the running command's text, "" when idle or not known
    int status = -1;          // the last finished command's status, -1 for none
    long long prompts = 0, started = 0, done = 0;

    void apply(const VteMark& m) {
        known = true;
        prompts = m.prompts;
        started = m.started;
        done = m.done;
        busy = m.started > m.done;
        atPrompt = !busy && m.prompts > 0;
        command = busy ? m.command : std::string();
        status = m.status;
    }
};

}  // namespace ShellIntegration
