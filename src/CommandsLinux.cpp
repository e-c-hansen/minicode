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
// the item that brings it (Ctrl+I for the inline agent, Ctrl+T for Go to
// Symbol in Workspace). The port's table of GActions (linux/src/Keys.cpp)
// has the same commands, but those the registry marks Fixed, which are a
// view's own keys.
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
        // Several carets and the line commands (PARITY.md 1.9), VS Code's
        // Linux keys: Ctrl+D, Ctrl+Shift+L, Ctrl+L and Ctrl+Shift+K. Add
        // Cursor is VS Code's Linux second key, Ctrl+Shift+Up and Down: its
        // first, Shift+Alt+Up and Down, is Copy Line's here, as on Windows
        // and the Mac, since VS Code's Linux Copy Line keys
        // (Ctrl+Shift+Alt+Up and Down) are GNOME's, moving a window to the
        // workspace above or below (and Ctrl+Alt+Up and Down switch to it).
        // Editor commands: the program in the terminal keeps Ctrl+D and
        // Ctrl+L.
        {"editor.action.addSelectionToNextFindMatch", "ctrl+d", 0, nullptr, nullptr},
        {"editor.action.selectHighlights", "ctrl+shift+l", 0, nullptr, nullptr},
        {"editor.action.insertCursorAbove", "ctrl+shift+up", 0, nullptr, nullptr},
        {"editor.action.insertCursorBelow", "ctrl+shift+down", 0, nullptr, nullptr},
        {"expandLineSelection", "ctrl+l", 0, nullptr, nullptr},
        {"editor.action.deleteLines", "ctrl+shift+k", 0, nullptr, nullptr},
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
        // The editor's zoom (PARITY.md 1.11), VS Code's Linux Ctrl+= and
        // Ctrl+- (its window's zoom; here the editor's and the Markdown
        // preview's). The reset is VS Code's Ctrl+keypad 0 alone, since
        // Ctrl+0 is Focus File Tree and Ctrl+Alt+0 is Fold All in the editor
        // (the Mac's reset is Option+Cmd+0; PARITY.md D1).
        // NotPageOrTerminal: in a page the same keys zoom it, and in the
        // terminal the terminals (below).
        {"editor.action.fontZoomIn", "ctrl+= ctrl+shift+=", 0, nullptr, nullptr},
        {"editor.action.fontZoomOut", "ctrl+-", 0, nullptr, nullptr},
        {"editor.action.fontZoomReset", "ctrl+numpad0", 0, nullptr, nullptr},
        // A PDF or the LaTeX preview on show, or the browser with the
        // keyboard. Ctrl+0 is Focus File Tree, as Cmd+0 is outside a page on
        // the Mac, so fit width is Ctrl+Alt+0, Fold All in the editor.
        {"minicode.page.zoomIn", "ctrl+= ctrl+shift+=", 0, nullptr, nullptr},
        {"minicode.page.zoomOut", "ctrl+-", 0, nullptr, nullptr},
        {"minicode.page.fitWidth", "ctrl+alt+0", 0, nullptr, nullptr},
        // VS Code's Linux key. An editor command, so in the terminal Alt+Z
        // is the program's (Meta), as Option+Z is on the Mac.
        {"editor.action.toggleWordWrap", "alt+z", 0, nullptr, nullptr},
        // Folding (PARITY.md 1.10). Fold and Unfold are VS Code's Linux
        // Ctrl+Shift+[ and ], which GTK matches as the braces Shift makes
        // (linuxAccelerators); the rest are Ctrl+K chords in VS Code, so
        // Ctrl+Alt with the chord's second key, as the Mac's are
        // Control+Command with it. Ctrl+Alt+0 is Fold All in the editor, the
        // family's key (VS Code's Ctrl+K Ctrl+0), and outside it the
        // terminals' zoom reset and a page's fit width, places that never
        // meet the editor; the editor's zoom reset keeps VS Code's own
        // Ctrl+keypad 0 (PARITY.md D1). Toggle Fold's would be Ctrl+Alt+L,
        // the task key's, so it has none. Editor commands: in the terminal
        // the program gets every one of these keys.
        {"editor.fold", "ctrl+shift+[", 0, nullptr, nullptr},
        {"editor.unfold", "ctrl+shift+]", 0, nullptr, nullptr},
        {"editor.toggleFold", "", 0, nullptr, nullptr},
        {"editor.foldRecursively", "ctrl+alt+[", 0, nullptr, nullptr},
        {"editor.unfoldRecursively", "ctrl+alt+]", 0, nullptr, nullptr},
        {"editor.foldAll", "ctrl+alt+0", 0, nullptr, nullptr},
        {"editor.unfoldAll", "ctrl+alt+j", 0, nullptr, nullptr},
        {"editor.foldAllBlockComments", "ctrl+alt+/", 0, nullptr, nullptr},
        {"editor.foldLevel1", "ctrl+alt+1", 0, nullptr, nullptr},
        {"editor.foldLevel2", "ctrl+alt+2", 0, nullptr, nullptr},
        {"editor.foldLevel3", "ctrl+alt+3", 0, nullptr, nullptr},
        {"editor.foldLevel4", "ctrl+alt+4", 0, nullptr, nullptr},
        {"editor.foldLevel5", "ctrl+alt+5", 0, nullptr, nullptr},
        {"editor.foldLevel6", "ctrl+alt+6", 0, nullptr, nullptr},
        {"editor.foldLevel7", "ctrl+alt+7", 0, nullptr, nullptr},
        {"workbench.action.toggleSidebarVisibility", "ctrl+b", 0, nullptr, nullptr},
        {"workbench.view.scm", "ctrl+shift+g", 0, nullptr, nullptr},
        {"minicode.toggleEditor", "ctrl+shift+e", 0, nullptr, nullptr},
        // The Mac's Ctrl+Cmd+M; VS Code has no key (its Toggle Maximize
        // Editor Group is a chord), so Ctrl+Alt with the letter, which GNOME
        // leaves alone. It has Alt, so it works from the terminal too.
        {"minicode.toggleMaximizedPane", "ctrl+alt+m", 0, nullptr, nullptr},
        {"minicode.toggleHiddenFiles", "ctrl+shift+.", 0, nullptr, nullptr},
        // The Mac's Cmd+J would be Ctrl+J here, which a Global command would
        // take from the shell (it ends a line), so Ctrl+` and Ctrl+Shift+T.
        {"workbench.action.terminal.toggleTerminal", "ctrl+shift+t ctrl+`", Global, nullptr,
         nullptr},
        {"minicode.toggleBrowser", "ctrl+shift+b", 0, nullptr, nullptr},
        {"minicode.togglePlayback", "ctrl+shift+space", 0, nullptr, nullptr},

        // ---- Terminal
        // The terminals' own zoom, with the editor's keys while the terminal
        // has the keyboard, as on the Mac (and GNOME Terminal's own Ctrl+=
        // and Ctrl+-). A Terminal command keeps its plain Ctrl keys there
        // (linuxTerminalTakes), and applies nowhere else.
        {"workbench.action.terminal.fontZoomIn", "ctrl+= ctrl+shift+=", 0, nullptr, nullptr},
        {"workbench.action.terminal.fontZoomOut", "ctrl+-", 0, nullptr, nullptr},
        {"workbench.action.terminal.fontZoomReset", "ctrl+alt+0 ctrl+numpad0", 0, nullptr, nullptr},

        // ---- Navigate
        // The palette's other ways in, VS Code's Linux keys. Ctrl+P and
        // Ctrl+G are plain Ctrl keys, so the program in the terminal keeps
        // them (readline's previous line, Emacs's quit), as the Mac's grid
        // keeps Ctrl+G. Go to Symbol in Workspace (VS Code's Ctrl+T) waits
        // for the language server work of Stage 2.
        {"workbench.action.quickOpen", "ctrl+p", 0, nullptr, nullptr},
        {"workbench.action.gotoLine", "ctrl+g", 0, nullptr, nullptr},
        {"workbench.action.gotoSymbol", "ctrl+shift+o", 0, nullptr, nullptr},
        // VS Code's Linux keys (PARITY.md 1.11). In the terminal Ctrl+Shift+-
        // is the program's (readline's undo reads it as Ctrl+_), as the
        // Mac's grid keeps Ctrl+-; Ctrl+Alt+- still goes back from there.
        {"workbench.action.navigateBack", "ctrl+alt+-", 0, nullptr, nullptr},
        {"workbench.action.navigateForward", "ctrl+shift+-", 0, nullptr, nullptr},
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

        // ---- the editor's own keys (PARITY.md 1.9). Move and Copy Line
        // are VS Code's (see Add Cursor above), in the Edit menu here, which
        // the Mac's are not. Ctrl+Home and Ctrl+End are GtkTextView's own
        // with one caret and Carets.cpp's with several; listed, not
        // actions, so they never leave an entry or the terminal.
        {"editor.action.moveLinesDownAction", "alt+down", 0, nullptr, "Edit"},
        {"editor.action.moveLinesUpAction", "alt+up", 0, nullptr, "Edit"},
        {"editor.action.copyLinesDownAction", "alt+shift+down", 0, nullptr, "Edit"},
        {"editor.action.copyLinesUpAction", "alt+shift+up", 0, nullptr, "Edit"},
        {"cursorTop", "ctrl+home", 0, nullptr, nullptr},
        {"cursorTopSelect", "ctrl+shift+home", 0, nullptr, nullptr},
        {"cursorBottom", "ctrl+end", 0, nullptr, nullptr},
        {"cursorBottomSelect", "ctrl+shift+end", 0, nullptr, nullptr},
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
    // A terminal sends these as Ctrl+G, Ctrl+Space and Ctrl+_, which Emacs
    // and readline use (keyboard-quit, set-mark, undo).
    if (isKey(k, Ctrl | Shift, "g") || isKey(k, Ctrl | Shift, "space") ||
        isKey(k, Ctrl | Shift, "-"))
        return true;
    // A command of the terminal's own (its zoom) keeps its keys there: it
    // applies nowhere else, so its plain Ctrl keys are no one else's.
    if (c.when == When::Terminal) return false;
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
        // With Num Lock off the same key is a cursor key, and GTK matches
        // the keyval it gives (the zoom's Ctrl+keypad 0 is Ctrl+KP_Insert).
        static const char* const kNoNumLock[10] = {"KP_Insert", "KP_End",  "KP_Down", "KP_Next",
                                                   "KP_Left",   "KP_Begin", "KP_Right", "KP_Home",
                                                   "KP_Up",     "KP_Prior"};
        const int digit = n[6] - '0';
        if (digit >= 0 && digit <= 9) out.push_back(mods + kNoNumLock[digit]);
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
