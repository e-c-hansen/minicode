// LaunchOptions.h — what `mutible` was asked on its command line: the
// options the port has, the help it prints, whether a run gives its terminal
// back, and the name of the systemd scope it then moves to. The plain C++
// half of leaving the terminal (PARITY.md R2); Detach.h is the half that
// forks. No GLib, so tests/run_tests.cpp covers all of it.
//
// The Mac's `mutible` is scripts/mutible-launcher, which hands the paths
// to the running app or to `open -a` and returns at once, as VS Code's
// `code` does. Here `mutible` is the app itself, so the same promise is
// kept by forking away from the terminal (Detach.cpp). The options follow
// the launcher's where the port has the behaviour: -v and -h, and
// --foreground, which the Mac has no need of. Not -n or -r, which the port
// has no behaviour for: a second `mutible <path>` opens a window of its
// own on a folder no window shows and brings forward the one that does,
// and nothing on the command line puts a folder in the window in front.
#pragma once

#include <string>
#include <vector>

namespace LaunchOptions {

struct Parsed {
    bool help = false;
    bool version = false;
    bool foreground = false;
    // --open-folder, which is also left in `args` for the running mutible.
    bool openFolder = false;
    // --new-window, left in `args` the same way.
    bool newWindow = false;
    // A usage error, ready to print ("mutible: unknown option -x" and how
    // to get the help). Exit status 2, as the Mac's launcher.
    std::string error;
    // What GApplication is given: argv[0], then the arguments with the
    // options above taken out. A relative path that starts with '-' gets
    // "./" before it, so neither GApplication's option parser nor the
    // running mutible reads it as an option.
    std::vector<std::string> args;
};

// Options and paths may come in any order; "--" ends the options.
// GApplication's own --gapplication-service is passed through to it.
Parsed parse(const std::vector<std::string>& argv);

// Whether to leave the terminal: started from one (`fromTerminal`: the
// process has a controlling terminal, or a standard stream is a terminal)
// and not asked to stay, by --foreground or by MUTIBLE_FOREGROUND set to
// anything but "" or "0" (`foregroundEnv`, null when unset). The desktop's
// launch has no terminal, so it never forks.
bool shouldDetach(const Parsed& parsed, bool fromTerminal, const char* foregroundEnv);

// The text `mutible --help` prints, and `mutible --version`'s line.
std::string helpText();
std::string versionText(const std::string& version);

// The transient scope a detached mutible asks systemd for, in the XDG
// naming systemd documents (app-<launcher>-<application id>-<pid>.scope):
// app-mutible-ai.ible.Mutible-1234.scope. The launcher is mutible
// itself, as `flatpak run` names its scopes app-flatpak-...; GNOME Shell's
// own launches are app-gnome-..., so the two stay told apart. The
// application id is escaped as systemd escapes unit names ('-' is \x2d).
std::string scopeName(const std::string& appId, long pid);

}  // namespace LaunchOptions
