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

class Editor;
typedef struct _GtkWindow GtkWindow;

struct ControlWindow {
    int id = 0;               // what `state` calls the window, and --window names
    std::string folder;       // the tree's root, as the window spells it
    Editor* editor = nullptr;
    GtkWindow* window = nullptr;
    int shellPid = 0;         // the terminal's shell (VTE's child), 0 while it has none
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
};

void registerControlCommands(ControlChannel::Dispatcher& dispatcher, ControlHost host);

namespace ControlCommands {
// The character offset of the 1-based line and column in `text` (UTF-8),
// the column no further than the line's end; the text's length when the
// line is past its end.
long offsetOfLineColumn(const std::string& text, int line, int column);
}  // namespace ControlCommands
