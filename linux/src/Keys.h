// Keys.h — the command registry (../src/Commands.h) in the GTK port
// (linux/PARITY.md, D1 and D12): the Linux keys in effect, which are
// CommandsLinux.cpp's table with keys.conf applied; the port's table of
// GActions by command id; and from those two, the menu bar, the
// application's accelerators, the file tree's own keys, and the key text
// the shortcut hints show. Nothing else names a key.
//
// A command the port has is a row in CommandsLinux.cpp and a row in
// actions() here, and the GUI checks hold the two to each other. A command
// with neither is shown nowhere. A new command adds both rows, its GAction
// in main.cpp (addAction), and its README row. A view's own keys (the
// registry's Fixed: Ctrl+Home in the editor) have a Linux row for the
// table, the hints and README, but no action and no accelerator.
//
// The accelerators follow what has the keyboard in the active window
// (setPlace), since GTK's are the application's and go to the key window:
//   - In the terminal, only commands that apply there, and of their keys
//     not the ones the program gets (Commands::linuxTerminalTakes: plain
//     Ctrl keys and unmodified keys unless Global, VTE's copy and paste,
//     Ctrl+Shift+G and Ctrl+Shift+Space). The Mac never needs this: its
//     keys are on Command.
//   - A page's commands (its zoom) only in a page (Place::page), where
//     their keys outrank the editor's zoom, which has the same ones.
//   - Elsewhere, every command but the terminal's. A Markdown-only key with
//     Ctrl that no other command has is always bound (its action rings the
//     bell where it cannot act, as on the Mac its menu item would be
//     disabled); its other keys (Alt+C) only while the editor has the
//     keyboard on a file of that language.
//   - The file tree's commands (F2, Delete) are never accelerators, which
//     would take Delete from the editor: a shortcut controller on the tree
//     runs them (addTreeKeys), and while the tree has the keyboard no
//     accelerator takes a key of theirs.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

#include "Commands.h"

namespace Keys {

// One of the port's actions, by the command it is.
struct Action {
    const char* id;       // the registry's id
    const char* action;   // the GAction, "win.save", "app.quit"
    bool section;         // a separator before it in its menu
};
// In the registry's order.
const std::vector<Action>& actions();
const Action* actionFor(const std::string& commandId);
// Rows of CommandsLinux.cpp without an action, and actions without a row,
// as messages; empty when the two tables agree.
std::vector<std::string> tableProblems();

// Reads keys.conf ($MINICODE_KEYS, else $XDG_CONFIG_HOME/minicode/keys.conf,
// the file the Mac reads), sets the accelerators, and watches the file.
// `changed` runs after it changed and was read again, the accelerators and
// the tree's keys already following it.
void init(GtkApplication* app, std::function<void()> changed);
const Commands::Keymap& keymap();
const std::string& path();
const std::vector<std::string>& errors();   // "line N: message"
// Writes the file, every command commented out, when there is none.
bool ensureFileExists();
// Reads the file again now.
void reload();

// What has the keyboard in the active window.
struct Place {
    bool terminal = false;   // the terminal
    bool editor = false;     // the editor's text view
    bool tree = false;       // the file tree's list (not a popover over it)
    // A page: the browser with the keyboard, or outside the terminal a PDF
    // or the LaTeX preview on show (which takes no keyboard of its own), so
    // Ctrl+= and Ctrl+- zoom it rather than the editor (PARITY.md 1.11).
    bool page = false;
    std::string language;    // Commands::languageOfPath of the editor's file
    bool operator==(const Place& o) const {
        return terminal == o.terminal && editor == o.editor && tree == o.tree &&
               page == o.page && language == o.language;
    }
};
// Sets the accelerators for it, when it differs from the last.
void setPlace(const Place& place);
// Is `focus` inside `area`, not counting a popover parented inside it?
bool within(GtkWidget* focus, GtkWidget* area);

// The menu bar: File, Edit, View, Terminal and Navigate as the registry
// orders them, with only the commands that have an action, and those Linux
// moves (Quit, Settings) at the end of their new menu. New reference.
GMenuModel* menuBar();
// A menu of these commands in this order, "" for a separator. New reference.
GMenuModel* menuOf(const std::vector<std::string>& ids);

// The tree's commands on a shortcut controller of `widget`'s, kept up with
// keys.conf for the widget's life.
void addTreeKeys(GtkWidget* widget);

// A command's first key (or the one at `index`) as the hints show it, "Ctrl
// Shift V" ("Ctrl+Shift+V" with "+"); "" when it has none.
std::string label(const std::string& id, const char* between = " ", size_t index = 0);
// All of them, "Ctrl Shift T, Ctrl `".
std::string labels(const std::string& id);

}  // namespace Keys
