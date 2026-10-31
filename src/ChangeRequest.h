// ChangeRequest.h — the parts of "Request a Change to minicode" that need no
// GUI: the prompt Claude Code is given, the commands typed into the terminal
// to start and to resume it, and how minicode's own source folder and its
// fresh build are recognised. The Mac's ChangePanel.mm drives it.
//
// The flow: the user describes a change in a bar over the editor; a terminal
// tab opens in minicode's source folder running Claude Code with that
// request; Claude edits the source and runs `make`; minicode sees the new
// MiniCode.app there and offers to restart into it, reopening its windows
// and resuming the conversation (`--continue`) in a terminal.
#pragma once

#include <functional>
#include <string>

namespace ChangeRequest {

struct Context {
    std::string request;   // what the user typed
    std::string version;   // the running minicode's version, "1.5.2"
    std::string appPath;   // where the running app is, "/Applications/MiniCode.app"
};

// The request, trimmed, then a paragraph telling Claude where it is and
// what minicode expects: change this repository, rebuild with `make`, and
// leave committing, pushing, and releasing to the user. "" for a request
// that is only whitespace.
std::string prompt(const Context &c);

// `cd <source> && <command> "$(cat <promptFile>)"`, paths shell-quoted.
// The prompt is read from a file so the line typed into the terminal, its
// tab's title and the shell's history stay one short line. `command` is the
// develop.command setting, a whole command line ("claude" by default)
// typed as written.
std::string startCommand(const std::string &command, const std::string &source,
                         const std::string &promptFile);
// `cd <source> && <command> --continue`: the last conversation in that
// folder, after a restart.
std::string resumeCommand(const std::string &command, const std::string &source);

// A folder holding minicode's source: a Makefile and src/EditorController.mm.
bool looksLikeSource(const std::string &dir,
                     const std::function<bool(const std::string &)> &exists);
// The folder an app bundle was built in ("/x/minicode" for
// "/x/minicode/MiniCode.app"), where `make` puts it; "" for a bundle at
// the root.
std::string checkoutOfBundle(const std::string &bundlePath);
// The bundle `make` builds in `source`, and the file codesign writes last,
// whose change says a build has finished.
std::string builtApp(const std::string &source);
std::string buildStamp(const std::string &source);

}  // namespace ChangeRequest
