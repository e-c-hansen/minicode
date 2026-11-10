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
// the item that brings it (Ctrl+L for Select Line, Ctrl+Shift+L for Select
// All Occurrences, Ctrl+I for the inline agent, Ctrl+T for Go to Symbol in
// Workspace). The port's table of GActions (linux/src/Keys.cpp) has the same
// commands.
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
        // VS Code's Linux Save All is a chord (Ctrl+K S), so the Mac's
        // Option+Cmd+S with Alt and Ctrl, as Show Hover's.
        {"workbench.action.files.saveAll", "ctrl+alt+s", 0, nullptr, nullptr},
        {"minicode.latex.exportPDF", "ctrl+shift+s", 0, nullptr, nullptr},
        {"explorer.newFolder", "ctrl+alt+n", 0, nullptr, nullptr},
        // The tree's own keys, as in VS Code's explorer on Linux. The port
        // gives them to the tree's shortcut controller, never to the menu
        // bar, so Delete in the editor still deletes text.
        {"renameFile", "f2", 0, nullptr, nullptr},
        {"moveFileToTrash", "delete", 0, nullptr, nullptr},
        {"revealFileInOS", "", 0, "Open Containing Folder", nullptr},
        {"copyFilePath", "", 0, nullptr, nullptr},
        // The tab on show (PARITY.md 1.1): VS Code's Linux keys where it has
        // them, the rest from the menu and the tab's right click menu, as on
        // the Mac. Ctrl+W and Ctrl+F4 are plain Ctrl keys, so the shell keeps
        // them while the terminal has the keyboard. Reopen Closed Tab is VS
        // Code's Ctrl+Shift+T, which is Toggle Terminal's here (minicode's
        // own Shift key does not move), so the Mac's Option+Shift+Cmd+T with
        // Alt and Ctrl, as the Mac moved it off Shift+Cmd+T.
        {"workbench.action.files.copyPathOfActiveFile", "", 0, nullptr, nullptr},
        {"workbench.action.files.revealActiveFileInWindows", "", 0,
         "Open Containing Folder of Active File", nullptr},
        {"workbench.files.action.showActiveFileInExplorer", "", 0, nullptr, nullptr},
        {"workbench.action.closeActiveEditor", "ctrl+w ctrl+f4", 0, nullptr, nullptr},
        {"workbench.action.reopenClosedEditor", "ctrl+alt+shift+t", 0, nullptr, nullptr},
        {"workbench.action.closeOtherEditors", "", 0, nullptr, nullptr},
        {"workbench.action.closeEditorsToTheRight", "", 0, nullptr, nullptr},
        {"workbench.action.closeAllEditors", "", 0, nullptr, nullptr},
        {"workbench.action.keepEditor", "", 0, nullptr, nullptr},
        // VS Code's Linux key.
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
        // The find bar (PARITY.md 1.6), VS Code's Linux keys: Ctrl+H shows
        // the replace row, F3 and Shift+F3 step (its Ctrl+G is Go to Line),
        // Ctrl+Shift+1 replaces one and Ctrl+Alt+Enter all, and Alt with the
        // option's letter flips it. Alt+L is Find in Selection's here, as in
        // VS Code: the task key, which took it on the Mac, is Ctrl+Alt+L.
        // In the terminal the Alt letters are the program's
        // (linuxTerminalTakes).
        {"editor.action.startFindReplaceAction", "ctrl+h", 0, nullptr, nullptr},
        {"editor.action.nextMatchFindAction", "f3", 0, nullptr, nullptr},
        {"editor.action.previousMatchFindAction", "shift+f3", 0, nullptr, nullptr},
        {"editor.action.replaceOne", "ctrl+shift+1", 0, nullptr, nullptr},
        {"editor.action.replaceAll", "ctrl+alt+enter", 0, nullptr, nullptr},
        {"toggleFindCaseSensitive", "alt+c", 0, nullptr, nullptr},
        {"toggleFindWholeWord", "alt+w", 0, nullptr, nullptr},
        {"toggleFindRegex", "alt+r", 0, nullptr, nullptr},
        {"togglePreserveCase", "alt+p", 0, nullptr, nullptr},
        {"toggleFindInSelection", "alt+l", 0, nullptr, nullptr},
        {"workbench.action.findInFiles", "ctrl+shift+f", 0, nullptr, nullptr},
        // VS Code's Ctrl+Shift+H is the shortcut hints' (minicode's own
        // Shift key, which does not move), so the Mac's Option+Shift+Cmd+F
        // with Ctrl+Alt for Option+Cmd, as Reopen Closed Tab's.
        {"workbench.action.replaceInFiles", "ctrl+alt+shift+f", 0, nullptr, nullptr},
        {"minicode.showTodos", "ctrl+shift+d", 0, nullptr, nullptr},

        // ---- View
        // VS Code's Linux keys (PARITY.md 1.5). F1 has no modifier, so the
        // program in the terminal gets it there, as it gets F3; Ctrl+Shift+P
        // still opens the palette from the terminal.
        {"workbench.action.showCommands", "ctrl+shift+p f1", 0, nullptr, nullptr},
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
        // The palette's other ways in, VS Code's Linux keys. Ctrl+P and
        // Ctrl+G are plain Ctrl keys, so the program in the terminal keeps
        // them (readline's previous line, Emacs's quit), as the Mac's grid
        // keeps Ctrl+G. Go to Symbol in Workspace (VS Code's Ctrl+T) waits
        // for the language server work of Stage 2.
        {"workbench.action.quickOpen", "ctrl+p", 0, nullptr, nullptr},
        {"workbench.action.gotoLine", "ctrl+g", 0, nullptr, nullptr},
        {"workbench.action.gotoSymbol", "ctrl+shift+o", 0, nullptr, nullptr},
        // VS Code's Linux key (PARITY.md 1.7). An editor command, so while
        // the terminal has the keyboard the program in it gets the key.
        {"editor.action.jumpToBracket", "ctrl+shift+\\", 0, nullptr, nullptr},
        // The keys out of the terminal without the mouse: Global, so the
        // shell never gets them.
        {"workbench.action.focusSideBar", "ctrl+0", Global, nullptr, nullptr},
        // One editor until the split editor (PARITY.md 4.6).
        {"workbench.action.focusFirstEditorGroup", "ctrl+1", Global, "Focus Editor", nullptr},
        // VS Code's Linux Ctrl+PageDown and Ctrl+PageUp, and the Mac's Ctrl+Tab
        // and Ctrl+Shift+Tab (VS Code's are its recently used list, which
        // minicode has not). Global, as Ctrl+Tab was before the tabs, so they
        // leave the terminal too.
        {"workbench.action.nextEditor", "ctrl+pagedown ctrl+tab", Global, nullptr, nullptr},
        {"workbench.action.previousEditor", "ctrl+pageup ctrl+shift+tab", Global, nullptr,
         nullptr},
        // VS Code's Alt+1 to Alt+8; the last tab is Alt+9, where the Mac's
        // Ctrl+9 is, and VS Code's Alt+0.
        {"workbench.action.openEditorAtIndex1", "alt+1", 0, nullptr, nullptr},
        {"workbench.action.openEditorAtIndex2", "alt+2", 0, nullptr, nullptr},
        {"workbench.action.openEditorAtIndex3", "alt+3", 0, nullptr, nullptr},
        {"workbench.action.openEditorAtIndex4", "alt+4", 0, nullptr, nullptr},
        {"workbench.action.openEditorAtIndex5", "alt+5", 0, nullptr, nullptr},
        {"workbench.action.openEditorAtIndex6", "alt+6", 0, nullptr, nullptr},
        {"workbench.action.openEditorAtIndex7", "alt+7", 0, nullptr, nullptr},
        {"workbench.action.openEditorAtIndex8", "alt+8", 0, nullptr, nullptr},
        {"workbench.action.lastEditorInGroup", "alt+9 alt+0", 0, nullptr, nullptr},
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
    // Alt with a letter alone is Meta to a terminal: readline's word moves
    // and case changes. The find bar's options have these keys (item 1.6).
    if (k.mods == Alt && k.name.size() == 1 && k.name[0] >= 'a' && k.name[0] <= 'z')
        return !(flagsOf(c, Platform::Linux) & Global);
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
