// ShellIntegration.h — pure C++, header only. What minicode puts in the
// folder its terminals' zsh gets as ZDOTDIR: a copy of the integration
// script (resources/minicode-zsh-integration.zsh, in the bundle's Resources
// and beside the executable in build/) and a .zshenv that names the
// terminal's mode and sources that copy. The script's protocol (the marks,
// their nonce, the private keys below) is the app's, so the copy is the
// running app's own, never a newer bundle's script read later.
//
// tests/zsh_integration_tests.inc runs the script in a real zsh through this
// stub, so the test and the app start it the same way.
#pragma once
#include <string>

namespace ShellIntegration {

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
