// CommandsLinux.cpp — the registry's keys on Linux (Commands.h, "platforms"),
// for the GTK port: one row a command the port has, in the registry's
// order, with its default keys in the registry's notation. The Mac's keys
// are the registry's own rows and do not change here.
//
// The rule (linux/PARITY.md, D1), in order:
//   - A feature minicode shares with VS Code takes VS Code's Linux key.
//   - minicode's own Shift+Cmd key is the same letter with Ctrl+Shift, and
//     does not move.
//   - minicode's own Cmd-only key is the same letter with Ctrl, unless VS
//     Code's Linux keys use it for something else; then minicode's yields.
//   - While the terminal has the keyboard, plain Ctrl keys are the
//     program's unless the command is Global (linuxTerminalTakes), so a
//     terminal command takes Ctrl+Shift.
//   - Nothing GNOME takes: Super, Ctrl+Alt+arrows, Ctrl+Alt+T,
//     Ctrl+Alt+Delete.
// A command the port does not have yet has no row, so its key is free until
// the item that brings it (Ctrl+Shift+P for the palette, Ctrl+L for Select
// Line, Ctrl+Shift+L for Select All Occurrences, Ctrl+H for Replace, Ctrl+I
// for the inline agent, Ctrl+G for Go to Line, Ctrl+W for Close Tab). The
// port's table of GActions (linux/src/Keys.cpp) has the same commands.
#include "Commands.h"


namespace Commands {

const std::vector<LinuxRow>& linuxRows() {
    static const std::vector<LinuxRow> rows = {
        // ---- the Mac's minicode menu: on Linux Settings and Keyboard
        // Shortcuts are in the Edit menu and Quit in the File menu, as in
        // GNOME's applications.
        {"workbench.action.openSettings", "ctrl+,", 0, nullptr, "Edit"},
        {"workbench.action.openGlobalKeybindings", "", 0, nullptr, "Edit"},
        {"workbench.action.quit", "ctrl+q", 0, "Quit", "File"},

        // ---- File
        // VS Code's Linux keys: Ctrl+N a new file, Ctrl+Shift+N a new
        // window. New Folder, the Mac's Shift+Cmd+N, yields to New Window
        // and takes Ctrl+Alt+N, as on the Mac New Window took Option+Cmd+N.
        {"explorer.newFile", "ctrl+n", 0, nullptr, nullptr},
        {"workbench.action.newWindow", "ctrl+shift+n", 0, nullptr, nullptr},
        // VS Code's Linux Open Folder is a chord (Ctrl+K Ctrl+O), which
        // minicode has none of, and its Ctrl+O opens a file, a command
        // minicode does not have: Ctrl+O stays the folder's.
        {"workbench.action.files.openFolder", "ctrl+o", 0, nullptr, nullptr},
        {"workbench.action.files.save", "ctrl+s", 0, nullptr, nullptr},
        {"minicode.latex.exportPDF", "ctrl+shift+s", 0, nullptr, nullptr},
        {"explorer.newFolder", "ctrl+alt+n", 0, nullptr, nullptr},
        // The tree's own keys, as in VS Code's explorer on Linux. The port
        // gives them to the tree's shortcut controller, never to the menu
        // bar, so Delete in the editor still deletes text.
        {"renameFile", "f2", 0, nullptr, nullptr},
        {"moveFileToTrash", "delete", 0, nullptr, nullptr},
        {"revealFileInOS", "", 0, "Open Containing Folder", nullptr},
        {"copyFilePath", "", 0, nullptr, nullptr},
        // VS Code's Linux key. Its Ctrl+W, Close Editor, waits for tabs
        // (PARITY.md 1.1).
        {"workbench.action.closeWindow", "ctrl+shift+w", 0, nullptr, nullptr},

        // ---- Edit
        {"editor.action.commentLine", "ctrl+/", 0, nullptr, nullptr},
        // Ctrl+L is Select Line's (D1), so the task key is the Mac's Option+Cmd+L
        // with Alt and Ctrl; Alt+C is markdown-all-in-one's, in Markdown only.
        {"minicode.markdown.toggleTask", "ctrl+alt+l alt+c", 0, nullptr, nullptr},
        {"minicode.sortLines", "f9", 0, nullptr, nullptr},
        {"minicode.sortKeepSortedBlocks", "shift+f9", 0, nullptr, nullptr},
        {"editor.action.triggerSuggest", "ctrl+space", 0, nullptr, nullptr},
        // VS Code's is a chord (Ctrl+K Ctrl+I) and Ctrl+I is the inline
        // agent's (D1), so the Mac's Option+Cmd+I with Alt and Ctrl.
        {"editor.action.showHover", "ctrl+alt+i", 0, nullptr, nullptr},
        {"actions.find", "ctrl+f", 0, nullptr, nullptr},
        // VS Code's Linux keys; its Ctrl+G is Go to Line.
        {"editor.action.nextMatchFindAction", "f3", 0, nullptr, nullptr},
        {"editor.action.previousMatchFindAction", "shift+f3", 0, nullptr, nullptr},
        {"workbench.action.findInFiles", "ctrl+shift+f", 0, nullptr, nullptr},
        {"minicode.showTodos", "ctrl+shift+d", 0, nullptr, nullptr},

        // ---- View
        // In the terminal Ctrl+Shift+V pastes (linuxTerminalOwnKey).
        {"markdown.showPreview", "ctrl+shift+v", 0, nullptr, nullptr},
        {"minicode.toggleShortcutHints", "ctrl+shift+h", 0, nullptr, nullptr},
        // A PDF or the LaTeX preview. Ctrl+0 is Focus File Tree, as Cmd+0 is
        // outside a page on the Mac, so fit width is Ctrl+Alt+0.
        {"minicode.page.zoomIn", "ctrl+= ctrl+shift+=", 0, nullptr, nullptr},
        {"minicode.page.zoomOut", "ctrl+-", 0, nullptr, nullptr},
        {"minicode.page.fitWidth", "ctrl+alt+0", 0, nullptr, nullptr},
        {"workbench.action.toggleSidebarVisibility", "ctrl+b", 0, nullptr, nullptr},
        {"workbench.view.scm", "ctrl+shift+g", 0, nullptr, nullptr},
        {"minicode.toggleEditor", "ctrl+shift+e", 0, nullptr, nullptr},
        {"minicode.toggleHiddenFiles", "ctrl+shift+.", 0, nullptr, nullptr},
        // The Mac's Cmd+J would be Ctrl+J here, which a Global command would
        // take from the shell (it ends a line), so Ctrl+` and Ctrl+Shift+T.
        {"workbench.action.terminal.toggleTerminal", "ctrl+shift+t ctrl+`", Global, nullptr,
         nullptr},
        {"minicode.toggleBrowser", "ctrl+shift+b", 0, nullptr, nullptr},
        {"minicode.togglePlayback", "ctrl+shift+space", 0, nullptr, nullptr},

        // ---- Navigate
        // The keys out of the terminal without the mouse: Global, so the
        // shell never gets them.
        {"workbench.action.focusSideBar", "ctrl+0", Global, nullptr, nullptr},
        // One editor until the split editor (PARITY.md 4.6).
        {"workbench.action.focusFirstEditorGroup", "ctrl+1", Global, "Focus Editor", nullptr},
        // Until the port has tabs (PARITY.md 1.1), Ctrl+Tab goes back to the
        // file open before this one, as it did on the Mac before its tabs.
        {"workbench.action.nextEditor", "ctrl+tab", Global, "Previous File", nullptr},
        {"editor.action.revealDefinition", "f12", 0, nullptr, nullptr},
    };
    return rows;
}

const LinuxRow* linuxRow(const std::string& id) {
    for (const LinuxRow& r : linuxRows())
        if (id == r.id) return &r;
    return nullptr;
}

namespace {
bool isKey(const Key& k, unsigned mods, const char* name) {
    return k.mods == mods && k.name == name;
}
}  // namespace

bool linuxTerminalOwnKey(const Key& k) {
    return isKey(k, Ctrl | Shift, "c") || isKey(k, Ctrl | Shift, "v");
}

bool linuxTerminalTakes(const Command& c, const Key& k) {
    if (linuxTerminalOwnKey(k)) return true;
    // A terminal sends these as Ctrl+G and Ctrl+Space, which Emacs and
    // readline use (keyboard-quit, set-mark).
    if (isKey(k, Ctrl | Shift, "g") || isKey(k, Ctrl | Shift, "space")) return true;
    if (k.mods & (Shift | Alt | Cmd)) return false;
    return !(flagsOf(c, Platform::Linux) & Global);
}

namespace {
// GDK's names of the unshifted keys, and of the characters Shift makes of
// them on a US layout.
struct Shifted { char key; const char* plain; const char* shifted; };
const Shifted kPunctuation[] = {
    {'`', "grave", "asciitilde"},     {'-', "minus", "underscore"},
    {'=', "equal", "plus"},           {'[', "bracketleft", "braceleft"},
    {']', "bracketright", "braceright"}, {'\\', "backslash", "bar"},
    {';', "semicolon", "colon"},      {'\'', "apostrophe", "quotedbl"},
    {',', "comma", "less"},           {'.', "period", "greater"},
    {'/', "slash", "question"},       {'1', "1", "exclam"},
    {'2', "2", "at"},                 {'3', "3", "numbersign"},
    {'4', "4", "dollar"},             {'5', "5", "percent"},
    {'6', "6", "asciicircum"},        {'7', "7", "ampersand"},
    {'8', "8", "asterisk"},           {'9', "9", "parenleft"},
    {'0', "0", "parenright"},
};
const struct { const char* name; const char* gdk; const char* label; } kNamed[] = {
    {"up", "Up", "Up"},           {"down", "Down", "Down"},
    {"left", "Left", "Left"},     {"right", "Right", "Right"},
    {"home", "Home", "Home"},     {"end", "End", "End"},
    {"pageup", "Page_Up", "Page Up"}, {"pagedown", "Page_Down", "Page Down"},
    {"enter", "Return", "Enter"}, {"tab", "Tab", "Tab"},
    {"space", "space", "Space"},  {"escape", "Escape", "Escape"},
    {"backspace", "BackSpace", "Backspace"}, {"delete", "Delete", "Delete"},
    {"contextmenu", "Menu", "Menu"},
};
}  // namespace

std::vector<std::string> linuxAccelerators(const Key& k) {
    std::vector<std::string> out;
    if (k.empty() || (k.mods & Cmd)) return out;
    std::string mods;
    if (k.mods & Ctrl) mods += "<Ctrl>";
    if (k.mods & Alt) mods += "<Alt>";
    const bool shift = k.mods & Shift;
    const std::string& n = k.name;
    if (n.size() == 1) {
        for (const Shifted& p : kPunctuation)
            if (p.key == n[0]) {
                out.push_back(mods + (shift ? p.shifted : p.plain));
                // The keypad's own plus and minus, for the zoom keys.
                if (n == "=" && shift) out.push_back(mods + "KP_Add");
                if (n == "-" && !shift) out.push_back(mods + "KP_Subtract");
                return out;
            }
        // A letter: GTK matches it with Shift held whatever case it types.
        out.push_back(mods + (shift ? "<Shift>" : "") + n);
        return out;
    }
    if (shift) mods += "<Shift>";
    for (const auto& named : kNamed)
        if (n == named.name) {
            out.push_back(mods + named.gdk);
            return out;
        }
    if (n.size() >= 2 && n[0] == 'f') {
        out.push_back(mods + "F" + n.substr(1));
        return out;
    }
    if (n.size() == 7 && n.compare(0, 6, "numpad") == 0) {
        out.push_back(mods + "KP_" + n.substr(6));
        return out;
    }
    return out;
}

std::string linuxLabel(const Key& k, const char* between) {
    std::string s;
    auto add = [&](const std::string& word) {
        if (!s.empty()) s += between;
        s += word;
    };
    if (k.mods & Ctrl) add("Ctrl");
    if (k.mods & Alt) add("Alt");
    if (k.mods & Shift) add("Shift");
    if (k.mods & Cmd) add("Cmd");
    const std::string& n = k.name;
    for (const auto& named : kNamed)
        if (n == named.name) {
            add(named.label);
            return s;
        }
    if (n.size() == 7 && n.compare(0, 6, "numpad") == 0) {
        add("Keypad " + n.substr(6));
        return s;
    }
    if (n.size() >= 2 && n[0] == 'f') {
        add("F" + n.substr(1));
        return s;
    }
    std::string key = n;
    if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z') key[0] = (char)(key[0] - 'a' + 'A');
    add(key);
    return s;
}

}  // namespace Commands
