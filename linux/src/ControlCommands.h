// ControlCommands.h — the control channel's handlers for the GTK port
// (PARITY.md 3.1; the Mac's are src/ControlCommands.mm): ping, window, open,
// state and selection. Every other command in ControlProtocol's table is
// registered too and answers "not on Linux yet", so the table never makes a
// crash and an agent reads what is missing.
//
// The handlers see the app only through ControlHost, which main.cpp fills in
// (it keeps the windows to itself).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ControlProtocol.h"
#include "ShellIntegration.h"

class Editor;
class Browser;
typedef struct _GtkWindow GtkWindow;

struct ControlWindow {
    int id = 0;               // what `state` calls the window, and --window names
    std::string folder;       // the tree's root, as the window spells it
    Editor* editor = nullptr;
    GtkWindow* window = nullptr;
    Browser* browser = nullptr;   // the window's browser pane (null without WebKit)
    int shellPid = 0;         // the terminal's shell (VTE's child), 0 while it has none
    // What the shell integration's newest mark said (PARITY.md 3.1, D10);
    // not `known` for a shell that never marks.
    ShellIntegration::ShellState shell;
};

struct ControlHost {
    // Open windows, oldest first.
    std::function<std::vector<ControlWindow>()> windows;
    // The one the user last used (or the newest).
    std::function<int()> frontId;
    // A window on `folder` (a new one); false when it could not be made.
    std::function<bool(const std::string& folder, ControlWindow* out)> openWindow;
    // Brings the window forward.
    std::function<void(const ControlWindow&)> present;
    // Shows a file in a tab of its own, as a double-click in the tree does,
    // with the editor's area revealed.
    std::function<void(const ControlWindow&, const std::string& path)> showFile;
    // Shows the browser pane beside the editor, the keyboard where it was.
    std::function<void(const ControlWindow&)> showBrowser;
    // control.page-any-origin (PageCheck.h).
    std::function<bool()> pageAnyOrigin;
    // A web address in the user's page, as a link from the terminal opens it.
    std::function<void(const ControlWindow&, const std::string& url)> openUrl;
    // The window's terminal (PARITY.md 3.1): shown, then `line` typed and
    // entered at its prompt; `done` gets "" or why nothing was typed.
    std::function<void(const ControlWindow&, const std::string& line,
                       std::function<void(const std::string&)> done)> terminalRun;
    // A new terminal tab in the window (PARITY.md 4.7), shown and active,
    // with `title` as its name when not empty; false for none.
    std::function<bool(const ControlWindow&, const std::string& title)> newTerminal;
    // Splits the active terminal beside itself (PARITY.md 4.7, `split --right
    // term`), with `title` as its name when not empty; the keyboard stays
    // where it was. False when no pane fits.
    std::function<bool(const ControlWindow&, const std::string& title)> splitTerminal;
    // Shows the window's terminal, the keyboard where it was.
    std::function<void(const ControlWindow&)> showTerminal;
    // The window's terminal's last `lines` lines; false when it has none.
    std::function<bool(int id, int lines, std::string* text)> terminalOutput;
};

void registerControlCommands(ControlChannel::Dispatcher& dispatcher, ControlHost host);

namespace ControlCommands {
// The character offset of the 1-based line and column in `text` (UTF-8),
// the column no further than the line's end; the text's length when the
// line is past its end.
long offsetOfLineColumn(const std::string& text, int line, int column);
}  // namespace ControlCommands
