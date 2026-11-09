// Commands.cpp — see Commands.h.
#include "Commands.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

namespace Commands {

const char* whenName(When w) {
    switch (w) {
        case When::Editor: return "the editor";
        case When::Terminal: return "the terminal";
        case When::Tree: return "the file tree";
        case When::Page: return "a web page or a PDF";
        case When::NotPage: return "anywhere but a web page or a PDF";
        case When::NotPageOrTerminal: return "anywhere but a web page, a PDF, or the terminal";
        case When::Any: break;
    }
    return "anywhere";
}

namespace {
// The places a When stands for, as bits: the named places, and "other" for
// what has the keyboard when it is none of them (the source control panel,
// a viewer, a dialog's field).
unsigned placesOf(When w) {
    enum : unsigned { kOther = 1, kEditor = 2, kTerminal = 4, kTree = 8, kPage = 16 };
    switch (w) {
        case When::Editor: return kEditor;
        case When::Terminal: return kTerminal;
        case When::Tree: return kTree;
        case When::Page: return kPage;
        case When::NotPage: return kOther | kEditor | kTerminal | kTree;
        case When::NotPageOrTerminal: return kOther | kEditor | kTree;
        case When::Any: break;
    }
    return kOther | kEditor | kTerminal | kTree | kPage;
}

// How many places a When stands for: 1 for a named one.
int placeCount(When w) {
    int n = 0;
    for (unsigned b = placesOf(w); b; b &= b - 1) n++;
    return n;
}
}  // namespace

bool overlaps(When a, When b) { return (placesOf(a) & placesOf(b)) != 0; }

bool appliesIn(When w, When where) {
    if (w == When::Any || w == where) return true;
    // `where` is what has the keyboard; Any there is "none of the others".
    if (where == When::Any) return w == When::NotPage || w == When::NotPageOrTerminal;
    return (placesOf(w) & placesOf(where)) == placesOf(where);
}

std::string languageOfPath(const std::string& path) {
    const size_t slash = path.rfind('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return "";
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext == "md" || ext == "markdown" || ext == "mdown" || ext == "mkd" || ext == "mkdn")
        return "markdown";
    if (ext == "py" || ext == "pyi" || ext == "pyw") return "python";
    return "";
}

std::string languageName(const std::string& language) {
    if (language == "markdown") return "Markdown files";
    if (language == "python") return "Python files";
    return language + " files";
}

std::string placeName(const Command& c) {
    if (c.language) return std::string("the editor, in ") + languageName(c.language);
    return whenName(c.when);
}

namespace {
// Two commands' language scopes: the same (or both none).
bool sameLanguage(const Command& a, const Command& b) {
    if (!a.language || !b.language) return !a.language && !b.language;
    return std::strcmp(a.language, b.language) == 0;
}
}  // namespace

// ------------------------------------------------------------------ keys
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// The keys with names, and the other names they answer to.
const char* const kNamed[] = {
    "up", "down", "left", "right", "home", "end", "pageup", "pagedown", "enter",
    "tab", "space", "escape", "backspace", "delete", "contextmenu",
};
const struct { const char* alias; const char* name; } kAliases[] = {
    {"return", "enter"}, {"esc", "escape"}, {"pgup", "pageup"}, {"pgdn", "pagedown"},
    {"page_up", "pageup"}, {"page_down", "pagedown"}, {"del", "delete"},
    {"forwarddelete", "delete"}, {"menu", "contextmenu"}, {"backtick", "`"},
    {"backquote", "`"}, {"grave", "`"}, {"minus", "-"}, {"equal", "="}, {"equals", "="},
    {"bracketleft", "["}, {"bracketright", "]"}, {"backslash", "\\"}, {"semicolon", ";"},
    {"quote", "'"}, {"comma", ","}, {"period", "."}, {"slash", "/"},
    {"uparrow", "up"}, {"downarrow", "down"}, {"leftarrow", "left"}, {"rightarrow", "right"},
};
const char kPunctuation[] = "`-=[]\\;',./";
// A US keyboard's shifted characters, for the error that names the key.
const char kShifted[] = "~!@#$%^&*()_+{}|:\"<>?";
const char kUnshifted[] = "`1234567890-=[]\\;',./";

bool knownName(const std::string& n) {
    if (n.size() == 1) {
        unsigned char c = (unsigned char)n[0];
        return std::isdigit(c) || std::islower(c) || std::strchr(kPunctuation, c);
    }
    for (const char* k : kNamed)
        if (n == k) return true;
    if (n.size() >= 2 && n[0] == 'f') {
        int v = 0;
        for (size_t i = 1; i < n.size(); i++) {
            if (!std::isdigit((unsigned char)n[i])) return false;
            v = v * 10 + (n[i] - '0');
        }
        return v >= 1 && v <= 20 && n[1] != '0';
    }
    if (n.size() == 7 && n.compare(0, 6, "numpad") == 0) return std::isdigit((unsigned char)n[6]);
    return false;
}

}  // namespace

bool parseKey(const std::string& text, Key* out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    std::string t = trim(text);
    if (t.empty()) return fail("no key");
    // Split on "+", keeping a final "+" as part of the last piece's place.
    std::vector<std::string> parts;
    std::string cur;
    for (char c : t) {
        if (c == '+') {
            parts.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(trim(cur));
    Key k;
    for (size_t i = 0; i < parts.size(); i++) {
        const std::string raw = parts[i];
        const bool last = i + 1 == parts.size();
        if (raw.empty())
            return fail(last ? "\"+\" is not a key here; write shift+=" : "a \"+\" with nothing before it");
        if (raw.find_first_of(" \t") != std::string::npos)
            return fail("\"" + raw + "\": chords (two keys one after the other) are not supported");
        std::string p = raw.size() == 1 ? raw : lower(raw);
        if (!last) {
            unsigned m = 0;
            if (p == "cmd" || p == "command" || p == "meta") m = Cmd;
            else if (p == "ctrl" || p == "control") m = Ctrl;
            else if (p == "alt" || p == "opt" || p == "option") m = Alt;
            else if (p == "shift") m = Shift;
            else return fail("\"" + raw + "\" is not a modifier (cmd, ctrl, alt, shift)");
            k.mods |= m;
            continue;
        }
        if (p == "cmd" || p == "command" || p == "ctrl" || p == "control" || p == "alt" ||
            p == "opt" || p == "option" || p == "shift" || p == "meta")
            return fail("\"" + t + "\" has modifiers but no key");
        if (p.size() == 1 && std::isupper((unsigned char)p[0])) p = lower(p);
        for (const auto& a : kAliases)
            if (p == a.alias) p = a.name;
        if (p == "keypad0" || (p.size() == 7 && p.compare(0, 6, "keypad") == 0))
            p = "numpad" + p.substr(6);
        if (p.size() == 4 && p.compare(0, 3, "num") == 0 && std::isdigit((unsigned char)p[3]))
            p = "numpad" + p.substr(3);
        if (p.size() == 1) {
            const char* s = std::strchr(kShifted, p[0]);
            if (s && p[0]) {
                std::string base(1, kUnshifted[s - kShifted]);
                return fail("\"" + p + "\" is a shifted key; write shift+" + base);
            }
        }
        if (!knownName(p)) return fail("\"" + raw + "\" is not a key minicode knows");
        k.name = p;
    }
    if (out) *out = k;
    return true;
}

std::string format(const Key& k) {
    std::string s;
    if (k.mods & Ctrl) s += "ctrl+";
    if (k.mods & Alt) s += "alt+";
    if (k.mods & Shift) s += "shift+";
    if (k.mods & Cmd) s += "cmd+";
    return s + k.name;
}

std::string glyphs(const Key& k) {
    std::string s;
    if (k.mods & Ctrl) s += "⌃";
    if (k.mods & Alt) s += "⌥";
    if (k.mods & Shift) s += "⇧";
    if (k.mods & Cmd) s += "⌘";
    const std::string& n = k.name;
    static const struct { const char* name; const char* glyph; } kGlyphs[] = {
        {"up", "↑"}, {"down", "↓"}, {"left", "←"}, {"right", "→"}, {"home", "↖"},
        {"end", "↘"}, {"pageup", "⇞"}, {"pagedown", "⇟"}, {"enter", "⏎"}, {"tab", "⇥"},
        {"space", "Space"}, {"escape", "⎋"}, {"backspace", "⌫"}, {"delete", "⌦"},
        {"contextmenu", "Menu"},
    };
    for (const auto& g : kGlyphs)
        if (n == g.name) return s + g.glyph;
    if (n.size() == 7 && n.compare(0, 6, "numpad") == 0) return s + "Keypad " + n.substr(6);
    if (n.size() >= 2 && n[0] == 'f') return s + "F" + n.substr(1);
    if (n.size() == 1) return s + std::string(1, (char)std::toupper((unsigned char)n[0]));
    return s + n;
}

bool isFunctionKey(const Key& k) {
    const std::string& n = k.name;
    if (n == "contextmenu") return true;
    return n.size() >= 2 && n[0] == 'f' && std::isdigit((unsigned char)n[1]);
}

bool typesText(const Key& k) {
    return !(k.mods & (Cmd | Ctrl | Alt)) && !isFunctionKey(k);
}

// ------------------------------------------------------------- the table
// NSTextFinderAction values, the tags of the find items (AppKit's enum).
enum {
    kFindShow = 1, kFindNext = 2, kFindPrevious = 3, kFindReplaceAll = 4,
    kFindReplaceAndFind = 6, kFindSetSearchString = 7, kFindShowReplace = 12,
};
// MCTreeColumns (TreeDetails.h).
enum { kColumnSize = 1, kColumnSaved = 2, kColumnCreated = 4 };

const When A = When::Any, E = When::Editor, T = When::Terminal, R = When::Tree,
           P = When::Page, NP = When::NotPage, NPT = When::NotPageOrTerminal;

const std::vector<Command>& all() {
    static const std::vector<Command> table = {
        // ---- the app menu
        {"minicode.about", "About MiniCode", "minicode", "", A,
         "orderFrontStandardAboutPanel:", 0, 0, nullptr},
        {"workbench.action.openSettings", "Settings…", "minicode", "cmd+,", A, "openSettings:",
         0, 0, nullptr},
        {"workbench.action.openGlobalKeybindings", "Keyboard Shortcuts…", "minicode", "", A,
         "openKeyboardShortcuts:", 0, 0, nullptr},
        {"minicode.showLanguageServers", "Language Servers…", "minicode", "", A,
         "showLanguageServers:", 0, 0, nullptr},
        {"minicode.restartLanguageServer", "Restart Language Server", "minicode", "", A,
         "restartLanguageServer:", 0, 0, nullptr},
        {"python.setInterpreter", "Select Python Interpreter…", "minicode", "", A,
         "selectPythonInterpreter:", 0, 0, nullptr},
        {"minicode.forgetFolderToolsAnswer", "Ask Again About This Folder's Tools", "minicode",
         "", A, "forgetFolderToolsAnswer:", 0, 0, nullptr},
        {"minicode.requestChange", "Request a Change to minicode…", "minicode", "shift+cmd+k", A,
         "requestChange:", 0, 0, nullptr},
        {"minicode.restart", "Restart minicode", "minicode", "", A, "restartMinicode:", 0, 0,
         nullptr},
        {"workbench.action.quit", "Quit MiniCode", "minicode", "cmd+q", A, "terminate:", 0, 0,
         nullptr},

        // ---- File
        {"explorer.newFile", "New File…", "File", "cmd+n", A, "newFile:", 0, 0, nullptr},
        {"workbench.action.newWindow", "New Window", "File", "alt+cmd+n", A, "newWindow:", 0, 0,
         nullptr},
        {"workbench.action.files.openFolder", "Open Folder…", "File", "cmd+o", A, "openFolder:",
         0, 0, nullptr},
        {"workbench.action.files.save", "Save", "File", "cmd+s", A, "saveCurrentFile:", 0, 0,
         nullptr},
        // VS Code's Save As key, Shift+Cmd+S, is Export PDF here.
        {"workbench.action.files.saveAs", "Save As…", "File", "", A, "saveFileAs:", 0, 0,
         nullptr},
        {"workbench.action.files.saveAll", "Save All", "File", "alt+cmd+s", A, "saveAllFiles:", 0,
         0, nullptr},
        // The recent folders, in the palette (File > Open Recent lists them).
        // In the terminal's grid Ctrl+R stays the shell's history search.
        {"workbench.action.openRecent", "Open Recent…", "File", "ctrl+r", A, "openRecent:", 0, 0,
         nullptr},
        {"minicode.latex.exportPDF", "Export PDF…", "File", "shift+cmd+s", A, "exportPDF:", 0, 0,
         nullptr},
        // A Markdown file as a standalone page (VS Code has no export).
        {"minicode.markdown.exportHtml", "Export as HTML…", "File", "", A, "exportMarkdownHtml:",
         0, 0, nullptr},
        {"explorer.newFolder", "New Folder…", "File", "shift+cmd+n", A, "newFolder:", 0, 0,
         nullptr},
        // Keys only while the tree has the keyboard, as in Finder and VS Code:
        // a tree command's key is never put on its menu item, which would run
        // it from the editor too (Cmd+Delete trashed the tree's selection
        // from the editor once). The key monitor runs them (Keymap.mm).
        {"renameFile", "Rename…", "File", "f2", R, "renameSelected:", 0, 0, nullptr},
        {"moveFileToTrash", "Move to Trash", "File", "cmd+backspace cmd+delete", R,
         "deleteSelected:", 0, 0, nullptr},
        {"revealFileInOS", "Reveal in Finder", "File", "", A, "revealInFinder:", 0, 0, nullptr},
        {"copyFilePath", "Copy Path", "File", "", A, "copyPath:", 0, 0, nullptr},
        {"copyRelativeFilePath", "Copy Relative Path", "File", "", A, "copyRelativePath:", 0, 0,
         nullptr},
        {"workbench.files.action.refreshFilesExplorer", "Refresh File Tree", "File", "cmd+r", A,
         "refreshTree:", 0, 0, nullptr},
        {"workbench.action.files.copyPathOfActiveFile", "Copy Path of Active File", "File", "", A,
         "copyActiveFilePath:", 0, 0, nullptr},
        {"workbench.action.files.copyRelativePathOfActiveFile",
         "Copy Relative Path of Active File", "File", "", A, "copyActiveFileRelativePath:", 0, 0,
         nullptr},
        {"workbench.action.files.revealActiveFileInWindows", "Reveal Active File in Finder",
         "File", "", A, "revealActiveFileInFinder:", 0, 0, nullptr},
        {"workbench.files.action.showActiveFileInExplorer", "Reveal Active File in File Tree",
         "File", "", A, "revealActiveFileInTree:", 0, 0, nullptr},
        {"workbench.action.closeActiveEditor", "Close Tab", "File", "cmd+w", A, "closeFocused:", 0,
         0, nullptr},
        {"workbench.action.reopenClosedEditor", "Reopen Closed Tab", "File", "alt+shift+cmd+t", A,
         "reopenClosedTab:", 0, 0, nullptr},
        {"workbench.action.closeOtherEditors", "Close Other Tabs", "File", "alt+cmd+t", A,
         "closeOtherTabs:", 0, 0, nullptr},
        {"workbench.action.closeEditorsToTheRight", "Close Tabs to the Right", "File", "", A,
         "closeTabsToTheRight:", 0, 0, nullptr},
        {"workbench.action.closeAllEditors", "Close All Tabs", "File", "", A, "closeAllTabs:", 0,
         0, nullptr},
        {"workbench.action.keepEditor", "Keep Open", "File", "", A, "keepActiveTabOpen:", 0, 0,
         nullptr},
        {"workbench.action.closeWindow", "Close Window", "File", "shift+cmd+w", A,
         "performClose:", 0, 0, nullptr},

        // ---- Edit
        {"undo", "Undo", "Edit", "cmd+z", A, "undo:", 0, 0, nullptr},
        {"redo", "Redo", "Edit", "shift+cmd+z", A, "redo:", 0, 0, nullptr},
        {"editor.action.clipboardCutAction", "Cut", "Edit", "cmd+x", A, "cut:", 0, 0, nullptr},
        {"editor.action.clipboardCopyAction", "Copy", "Edit", "cmd+c", A, "copy:", 0, 0, nullptr},
        {"editor.action.clipboardPasteAction", "Paste", "Edit", "cmd+v", A, "paste:", 0, 0,
         nullptr},
        {"editor.action.selectAll", "Select All", "Edit", "cmd+a", A, "selectAll:", 0, 0, nullptr},
        {"editor.action.addSelectionToNextFindMatch", "Add Next Occurrence", "Edit", "cmd+d", E,
         "addNextOccurrence:", 0, 0, nullptr},
        {"editor.action.selectHighlights", "Select All Occurrences", "Edit", "shift+cmd+l", E,
         "selectAllOccurrences:", 0, 0, nullptr},
        {"editor.action.insertCursorAbove", "Add Cursor Above", "Edit", "alt+cmd+up", E,
         "addCursorAbove:", 0, 0, nullptr},
        {"editor.action.insertCursorBelow", "Add Cursor Below", "Edit", "alt+cmd+down", E,
         "addCursorBelow:", 0, 0, nullptr},
        {"expandLineSelection", "Select Line", "Edit", "cmd+l", E, "expandLineSelection:", 0, 0,
         nullptr},
        {"editor.action.deleteLines", "Delete Line", "Edit", "ctrl+shift+k", E, "deleteLine:", 0,
         0, nullptr},
        {"editor.action.indentLines", "Indent Lines", "Edit", "cmd+]", E, "indentLines:", 0, 0,
         nullptr},
        {"editor.action.outdentLines", "Outdent Lines", "Edit", "cmd+[", E, "outdentLines:", 0, 0,
         nullptr},
        {"editor.action.insertLineAfter", "Insert Line Below", "Edit", "cmd+enter", E,
         "insertLineBelow:", 0, 0, nullptr},
        // VS Code's Shift+Cmd+Return is Run File here.
        {"editor.action.insertLineBefore", "Insert Line Above", "Edit", "", E, "insertLineAbove:",
         0, 0, nullptr},
        {"inlineChat.start", "Inline Agent…", "Edit", "cmd+i", A, "inlineAgent:", 0, 0, nullptr},
        {"editor.action.commentLine", "Toggle Comment", "Edit", "cmd+/", E, "toggleComment:", 0,
         0, nullptr},
        // Option+C is markdown-all-in-one's key (in Markdown only, so it
        // types its character everywhere else).
        {"minicode.markdown.toggleTask", "Toggle Task", "Edit", "alt+cmd+l alt+c", E,
         "toggleTask:", 0, 0, nullptr, "markdown"},
        // markdown-all-in-one's Cmd+B and Cmd+I are the sidebar and the
        // inline agent here, as in VS Code itself, so no key by default.
        {"markdown.extension.editing.toggleBold", "Toggle Bold", "Edit", "", E, "toggleBold:", 0,
         0, nullptr, "markdown"},
        {"markdown.extension.editing.toggleItalic", "Toggle Italic", "Edit", "", E,
         "toggleItalic:", 0, 0, nullptr, "markdown"},
        {"editor.action.formatDocument", "Format Document", "Edit", "alt+shift+f", E,
         "formatDocument:", 0, 0, nullptr},
        {"minicode.sortLines", "Sort Lines", "Edit", "f9", E, "sortLines:", 0, 0, nullptr},
        {"minicode.sortKeepSortedBlocks", "Sort keep-sorted Blocks", "Edit", "shift+f9", E,
         "fixKeepSorted:", 0, 0, nullptr},
        {"editor.action.triggerSuggest", "Complete", "Edit", "ctrl+space", E,
         "triggerCompletion:", 0, 0, nullptr},
        {"editor.action.showHover", "Show Hover Info", "Edit", "alt+cmd+i", E, "showHoverInfo:",
         0, 0, nullptr},
        // F2 is renameFile's in the tree (VS Code's key for both).
        {"editor.action.rename", "Rename Symbol…", "Edit", "f2", E, "renameSymbol:", 0, 0,
         nullptr},
        {"editor.action.quickFix", "Quick Fix…", "Edit", "cmd+.", E, "showQuickFixes:", 0, 0,
         nullptr},
        {"editor.action.organizeImports", "Organize Imports", "Edit", "alt+shift+o", E,
         "organizeImports:", 0, 0, nullptr},
        {"actions.find", "Find…", "Edit", "cmd+f", A, "performTextFinderAction:", kFindShow, 0,
         nullptr},
        {"editor.action.startFindReplaceAction", "Replace…", "Edit", "alt+cmd+f", A,
         "performTextFinderAction:", kFindShowReplace, 0, nullptr},
        {"editor.action.nextMatchFindAction", "Find Next", "Edit", "cmd+g", A,
         "performTextFinderAction:", kFindNext, 0, nullptr},
        {"editor.action.previousMatchFindAction", "Find Previous", "Edit", "shift+cmd+g", A,
         "performTextFinderAction:", kFindPrevious, 0, nullptr},
        {"actions.findWithSelection", "Use Selection for Find", "Edit", "cmd+e", A,
         "performTextFinderAction:", kFindSetSearchString, 0, nullptr},
        {"editor.action.replaceOne", "Replace", "Edit", "", A, "performTextFinderAction:",
         kFindReplaceAndFind, 0, nullptr},
        {"editor.action.replaceAll", "Replace All", "Edit", "alt+cmd+enter", A,
         "performTextFinderAction:", kFindReplaceAll, 0, nullptr},
        {"toggleFindCaseSensitive", "Match Case", "Edit > Find Options", "alt+cmd+c", A,
         "toggleFindMatchCase:", 0, 0, nullptr},
        {"toggleFindWholeWord", "Match Whole Word", "Edit > Find Options", "alt+cmd+w", A,
         "toggleFindWholeWord:", 0, 0, nullptr},
        {"toggleFindRegex", "Use Regular Expression", "Edit > Find Options", "alt+cmd+r", A,
         "toggleFindRegex:", 0, 0, nullptr},
        {"togglePreserveCase", "Preserve Case", "Edit > Find Options", "alt+cmd+p", A,
         "toggleFindPreserveCase:", 0, 0, nullptr},
        // VS Code's alt+cmd+l is Toggle Task's here; the find bar takes it
        // while it has the keyboard (EditorFind.h).
        {"toggleFindInSelection", "Find in Selection", "Edit > Find Options", "", A,
         "toggleFindInSelection:", 0, 0, nullptr},
        {"workbench.action.findInFiles", "Find in Folder…", "Edit", "shift+cmd+f", A,
         "openSearch:", 0, 0, nullptr},
        {"workbench.action.replaceInFiles", "Replace in Folder…", "Edit", "alt+shift+cmd+f", A,
         "openReplaceInFolder:", 0, 0, nullptr},
        {"minicode.showTodos", "TODOs in Folder…", "Edit", "shift+cmd+d", A, "showTodos:", 0, 0,
         nullptr},

        // ---- View
        {"workbench.action.showCommands", "Command Palette…", "View", "shift+cmd+p f1", A,
         "showCommandPalette:", 0, 0, nullptr},
        {"markdown.showPreview", "Toggle Preview", "View", "shift+cmd+v", A, "togglePreview:", 0,
         0, nullptr},
        // VS Code's Cmd+K V. minicode has no chords: a Cmd+K chord is
        // Control+Command and its second key, as the folding keys are.
        {"markdown.showPreviewToSide", "Open Preview to the Side", "View", "ctrl+cmd+v", E,
         "toggleSidePreview:", 0, 0, nullptr, "markdown"},
        // LaTeX Workshop's latex-workshop.synctex (SyncTeX from Cursor).
        {"latex-workshop.synctex", "Show Line in PDF", "View", "alt+cmd+j", A, "showLineInPDF:", 0,
         0, nullptr},
        {"minicode.toggleShortcutHints", "Toggle Shortcut Hints", "View", "shift+cmd+h", A,
         "toggleHints:", 0, 0, nullptr},
        // In a web page or a PDF, Cmd+= and Cmd+- zoom that instead (Page
        // Zoom below), and in a terminal the terminals (Terminal > Zoom In).
        {"editor.action.fontZoomIn", "Zoom In", "View", "cmd+= shift+cmd+=", NPT, "editorZoomIn:",
         0, 0, nullptr},
        {"editor.action.fontZoomOut", "Zoom Out", "View", "cmd+-", NPT, "editorZoomOut:", 0, 0,
         nullptr},
        {"editor.action.fontZoomReset", "Reset Zoom", "View", "alt+cmd+0 cmd+numpad0", NPT,
         "editorZoomReset:", 0, Global, nullptr},
        // The browser's page or a PDF, while it has the keyboard (or the
        // pointer was last there, scrolling or clicking): Safari's keys. As
        // with the tree's, the keys are not put on the menu items, which
        // would run them from every pane; the key monitor runs them.
        {"minicode.page.zoomIn", "Zoom In", "View > Page Zoom", "cmd+= shift+cmd+=", P,
         "zoomPageIn:", 0, 0, nullptr},
        {"minicode.page.zoomOut", "Zoom Out", "View > Page Zoom", "cmd+-", P, "zoomPageOut:", 0, 0,
         nullptr},
        {"minicode.page.actualSize", "Actual Size", "View > Page Zoom", "cmd+0", P,
         "zoomPageToActualSize:", 0, 0, nullptr},
        {"minicode.page.fitWidth", "Zoom to Fit Width", "View > Page Zoom", "", P,
         "zoomPageToFitWidth:", 0, 0, nullptr},
        {"minicode.page.fitPage", "Zoom to Fit Page", "View > Page Zoom", "", P, "zoomPageToFit:",
         0, 0, nullptr},
        {"editor.action.toggleWordWrap", "Word Wrap", "View", "alt+z", E, "toggleWordWrap:", 0, 0,
         nullptr},
        {"editor.fold", "Fold", "View > Folding", "ctrl+cmd+[", E, "foldRegion:", 0, 0, nullptr},
        {"editor.unfold", "Unfold", "View > Folding", "ctrl+cmd+]", E, "unfoldRegion:", 0, 0,
         nullptr},
        {"editor.toggleFold", "Toggle Fold", "View > Folding", "ctrl+cmd+l", E, "toggleFold:", 0,
         0, nullptr},
        {"editor.foldRecursively", "Fold Recursively", "View > Folding", "", E,
         "foldRecursively:", 0, 0, nullptr},
        {"editor.unfoldRecursively", "Unfold Recursively", "View > Folding", "", E,
         "unfoldRecursively:", 0, 0, nullptr},
        {"editor.foldAll", "Fold All", "View > Folding", "ctrl+cmd+0", E, "foldAll:", 0, 0,
         nullptr},
        {"editor.unfoldAll", "Unfold All", "View > Folding", "ctrl+cmd+j", E, "unfoldAll:", 0, 0,
         nullptr},
        {"editor.foldAllBlockComments", "Fold All Comments", "View > Folding", "ctrl+cmd+/", E,
         "foldAllComments:", 0, 0, nullptr},
        {"editor.foldLevel1", "Fold Level 1", "View > Folding", "ctrl+cmd+1", E, "foldLevel:", 1,
         0, nullptr},
        {"editor.foldLevel2", "Fold Level 2", "View > Folding", "ctrl+cmd+2", E, "foldLevel:", 2,
         0, nullptr},
        {"editor.foldLevel3", "Fold Level 3", "View > Folding", "ctrl+cmd+3", E, "foldLevel:", 3,
         0, nullptr},
        {"editor.foldLevel4", "Fold Level 4", "View > Folding", "ctrl+cmd+4", E, "foldLevel:", 4,
         0, nullptr},
        {"editor.foldLevel5", "Fold Level 5", "View > Folding", "ctrl+cmd+5", E, "foldLevel:", 5,
         0, nullptr},
        {"editor.foldLevel6", "Fold Level 6", "View > Folding", "ctrl+cmd+6", E, "foldLevel:", 6,
         0, nullptr},
        {"editor.foldLevel7", "Fold Level 7", "View > Folding", "ctrl+cmd+7", E, "foldLevel:", 7,
         0, nullptr},
        {"workbench.action.toggleSidebarVisibility", "Toggle Sidebar", "View", "cmd+b", A,
         "toggleSidebarVisibility:", 0, 0, nullptr},
        {"workbench.view.scm", "Toggle Source Control", "View", "ctrl+shift+g", A,
         "toggleSourceControl:", 0, 0, nullptr},
        // VS Code has these without keys.
        {"git.fetch", "Fetch", "View > Source Control", "", A, "gitFetch:", 0, 0, nullptr},
        {"git.pull", "Pull", "View > Source Control", "", A, "gitPull:", 0, 0, nullptr},
        {"git.push", "Push", "View > Source Control", "", A, "gitPush:", 0, 0, nullptr},
        {"git.sync", "Sync", "View > Source Control", "", A, "gitSync:", 0, 0, nullptr},
        {"git.stageAll", "Stage All Changes", "View > Source Control", "", A, "gitStageAll:", 0,
         0, nullptr},
        {"git.unstageAll", "Unstage All Changes", "View > Source Control", "", A,
         "gitUnstageAll:", 0, 0, nullptr},
        {"git.clean", "Discard Changes…", "View > Source Control", "", A, "gitClean:", 0, 0,
         nullptr},
        {"minicode.git.undoDiscard", "Undo Discard", "View > Source Control", "", A,
         "gitUndoDiscard:", 0, 0, nullptr},
        {"git.checkout", "Switch Branch…", "View > Source Control", "", A, "gitCheckout:", 0, 0,
         nullptr},
        {"git.commitAmend", "Commit and Amend", "View > Source Control", "", A,
         "gitCommitAmend:", 0, 0, nullptr},
        {"git.undoCommit", "Undo Last Commit", "View > Source Control", "", A, "gitUndoCommit:",
         0, 0, nullptr},
        {"git.stashIncludeUntracked", "Stash", "View > Source Control", "", A, "gitStash:", 0, 0,
         nullptr},
        {"git.stashPop", "Pop Stash", "View > Source Control", "", A, "gitStashPop:", 0, 0,
         nullptr},
        // The change bar on the caret's line (VS Code's from its change widget).
        {"git.revertChange", "Revert Change", "View > Source Control", "", E, "revertChange:", 0,
         0, nullptr},
        {"git.stageChange", "Stage Change", "View > Source Control", "", E, "stageChange:", 0, 0,
         nullptr},
        {"minicode.toggleAgents", "Toggle Agents", "View", "shift+cmd+a", A, "toggleAgents:", 0,
         0, nullptr},
        {"minicode.toggleProcesses", "Toggle Processes", "View", "", A, "toggleProcesses:", 0, 0,
         nullptr},
        {"minicode.toggleIdeLink", "Claude Code IDE Link", "View", "", A, "toggleIdeLink:", 0,
         0, nullptr},
        {"workbench.actions.view.problems", "Problems", "View", "shift+cmd+m", A,
         "toggleProblems:", 0, 0, nullptr},
        {"minicode.toggleEditor", "Toggle Editor", "View", "shift+cmd+e", A, "toggleEditor:", 0,
         0, nullptr},
        // VS Code's Toggle Maximize Editor Group is a Cmd+K chord, which
        // minicode has none of.
        {"minicode.toggleMaximizedPane", "Maximize Pane", "View", "ctrl+cmd+m", A,
         "toggleMaximizedPane:", 0, 0, nullptr},
        {"minicode.toggleHiddenFiles", "Show Hidden Files", "View", "shift+cmd+.", A,
         "toggleHiddenFiles:", 0, 0, nullptr},
        {"minicode.tree.toggleSizes", "Show Sizes", "View", "shift+cmd+i", A, "toggleTreeColumn:",
         kColumnSize, 0, nullptr},
        {"minicode.tree.toggleSaved", "Show When Last Saved", "View", "alt+shift+cmd+i", A,
         "toggleTreeColumn:", kColumnSaved, 0, nullptr},
        {"minicode.tree.toggleCreated", "Show When Created", "View", "ctrl+shift+cmd+i", A,
         "toggleTreeColumn:", kColumnCreated, 0, nullptr},
        // Ctrl+` from anywhere, the terminal's grid included.
        {"workbench.action.terminal.toggleTerminal", "Toggle Terminal", "View",
         "shift+cmd+t cmd+j ctrl+`", A, "toggleTerminal:", 0, Global, nullptr},
        {"minicode.toggleBrowser", "Toggle Browser", "View", "shift+cmd+b", A, "toggleBrowser:",
         0, 0, nullptr},
        {"minicode.togglePlayback", "Play/Pause", "View", "shift+cmd+space", A,
         "togglePlayback:", 0, 0, nullptr},
        {"editor.action.showContextMenu", "Show Context Menu", "View",
         "shift+f10 ctrl+enter contextmenu", A, "showContextMenu:", 0, Global, nullptr},

        // ---- Terminal
        {"workbench.action.terminal.new", "New Terminal", "Terminal", "ctrl+shift+`", A,
         "newTerminal:", 0, Global, nullptr},
        {"workbench.action.terminal.split", "Split Terminal", "Terminal", "cmd+\\", A,
         "splitTerminal:", 0, 0, nullptr},
        // VS Code's terminal groups take one direction from the panel's
        // place; here a column stacks. No key, as VS Code has none.
        {"minicode.terminal.splitDown", "Split Terminal Down", "Terminal", "", A,
         "splitTerminalDown:", 0, 0, nullptr},
        {"workbench.action.terminal.focusNext", "Next Terminal", "Terminal", "alt+cmd+]", A,
         "nextTerminal:", 0, 0, nullptr},
        {"workbench.action.terminal.focusPrevious", "Previous Terminal", "Terminal", "alt+cmd+[",
         A, "previousTerminal:", 0, 0, nullptr},
        {"workbench.action.terminal.kill", "Close Terminal", "Terminal", "ctrl+shift+w", A,
         "closeTerminal:", 0, 0, nullptr},
        // VS Code has no such command; the key is free in both.
        {"minicode.terminal.nextWaiting", "Next Waiting Terminal", "Terminal", "ctrl+cmd+w", A,
         "nextWaitingTerminal:", 0, 0, nullptr},
        {"workbench.action.terminal.clear", "Clear Terminal", "Terminal", "cmd+k", T,
         "clearTerminal:", 0, 0, nullptr},
        // VS Code has no key for it either.
        {"workbench.action.terminal.copyLastCommandOutput", "Copy Last Command Output",
         "Terminal", "", T, "copyLastCommandOutput:", 0, 0, nullptr},
        // The terminals' own zoom, in every window, with the editor's keys
        // while a terminal has the keyboard (VS Code has these without keys,
        // its window zoom scaling both). The keys stay off the menu items,
        // which would take them from the editor's; the key monitor runs them.
        {"workbench.action.terminal.fontZoomIn", "Zoom In", "Terminal", "cmd+= shift+cmd+=", T,
         "terminalZoomIn:", 0, 0, nullptr},
        {"workbench.action.terminal.fontZoomOut", "Zoom Out", "Terminal", "cmd+-", T,
         "terminalZoomOut:", 0, 0, nullptr},
        {"workbench.action.terminal.fontZoomReset", "Reset Zoom", "Terminal",
         "alt+cmd+0 cmd+numpad0", T, "terminalZoomReset:", 0, Global, nullptr},
        // VS Code has no Mac key for Rename; moving a tab is a drag there.
        {"workbench.action.terminal.rename", "Rename Terminal…", "Terminal", "", A,
         "renameTerminal:", 0, 0, nullptr},
        {"minicode.terminal.moveLeft", "Move Terminal Left", "Terminal", "", A,
         "moveTerminalLeft:", 0, 0, nullptr},
        {"minicode.terminal.moveRight", "Move Terminal Right", "Terminal", "", A,
         "moveTerminalRight:", 0, 0, nullptr},
        // VS Code's Mac keys; the tag is the direction (MCPaneDirection).
        {"workbench.action.terminal.resizePaneLeft", "Resize Terminal Pane Left", "Terminal",
         "ctrl+cmd+left", T, "resizeTerminalPane:", 1, 0, nullptr},
        {"workbench.action.terminal.resizePaneRight", "Resize Terminal Pane Right", "Terminal",
         "ctrl+cmd+right", T, "resizeTerminalPane:", 2, 0, nullptr},
        {"workbench.action.terminal.resizePaneUp", "Resize Terminal Pane Up", "Terminal",
         "ctrl+cmd+up", T, "resizeTerminalPane:", 3, 0, nullptr},
        {"workbench.action.terminal.resizePaneDown", "Resize Terminal Pane Down", "Terminal",
         "ctrl+cmd+down", T, "resizeTerminalPane:", 4, 0, nullptr},
        {"minicode.runFile", "Run File", "Terminal", "shift+cmd+enter", A, "runCurrentFile:", 0, 0,
         nullptr},
        // The Python extension's Shift+Return: the selection, or the line,
        // to a Python REPL in a terminal tab of its own.
        {"python.execSelectionInTerminal", "Run Selection or Line in Python REPL", "Terminal",
         "shift+enter", E, "runSelectionInTerminal:", 0, 0, nullptr, "python"},

        // ---- Navigate
        {"workbench.action.quickOpen", "Go to File…", "Navigate", "cmd+p", A, "quickOpen:", 0, 0,
         nullptr},
        {"workbench.action.gotoLine", "Go to Line…", "Navigate", "ctrl+g", A, "goToLinePrompt:", 0,
         0, nullptr},
        {"workbench.action.gotoSymbol", "Go to Symbol in File…", "Navigate", "shift+cmd+o", A,
         "goToSymbolInFile:", 0, 0, nullptr},
        {"workbench.action.showAllSymbols", "Go to Symbol in Workspace…", "Navigate", "cmd+t", A,
         "goToSymbolInWorkspace:", 0, 0, nullptr},
        {"workbench.action.navigateBack", "Go Back", "Navigate", "ctrl+-", A, "navigateBack:", 0,
         0, nullptr},
        {"workbench.action.navigateForward", "Go Forward", "Navigate", "ctrl+shift+-", A,
         "navigateForward:", 0, 0, nullptr},
        {"editor.action.jumpToBracket", "Go to Bracket", "Navigate", "shift+cmd+\\", E,
         "goToBracket:", 0, 0, nullptr},
        // In a web page or a PDF Cmd+0 is Actual Size, as in Safari.
        {"workbench.action.focusSideBar", "Focus File Tree", "Navigate", "cmd+0", NP, "focusTree:",
         0, 0, nullptr},
        {"workbench.action.focusFirstEditorGroup", "Focus Editor", "Navigate", "cmd+1", A,
         "focusEditor:", 0, 0, nullptr},
        {"workbench.action.terminal.focus", "Focus Terminal", "Navigate", "cmd+2", A,
         "focusTerminal:", 0, 0, nullptr},
        {"minicode.focusBrowser", "Focus Browser", "Navigate", "cmd+3", A, "focusBrowser:", 0, 0,
         nullptr},
        {"workbench.action.nextEditor", "Next Tab", "Navigate",
         "ctrl+tab shift+cmd+] alt+cmd+right", A, "selectNextTab:", 0, 0, nullptr},
        {"workbench.action.previousEditor", "Previous Tab", "Navigate",
         "ctrl+shift+tab shift+cmd+[ alt+cmd+left", A, "selectPreviousTab:", 0, 0, nullptr},
        {"workbench.action.openEditorAtIndex1", "Tab 1", "Navigate > Go to Tab",
         "shift+cmd+1 ctrl+1", A, "selectTabNumber:", 1, 0, nullptr},
        {"workbench.action.openEditorAtIndex2", "Tab 2", "Navigate > Go to Tab",
         "shift+cmd+2 ctrl+2", A, "selectTabNumber:", 2, 0, nullptr},
        {"workbench.action.openEditorAtIndex3", "Tab 3", "Navigate > Go to Tab",
         "shift+cmd+3 ctrl+3", A, "selectTabNumber:", 3, 0, nullptr},
        {"workbench.action.openEditorAtIndex4", "Tab 4", "Navigate > Go to Tab",
         "shift+cmd+4 ctrl+4", A, "selectTabNumber:", 4, 0, nullptr},
        {"workbench.action.openEditorAtIndex5", "Tab 5", "Navigate > Go to Tab",
         "shift+cmd+5 ctrl+5", A, "selectTabNumber:", 5, 0, nullptr},
        {"workbench.action.openEditorAtIndex6", "Tab 6", "Navigate > Go to Tab",
         "shift+cmd+6 ctrl+6", A, "selectTabNumber:", 6, 0, nullptr},
        {"workbench.action.openEditorAtIndex7", "Tab 7", "Navigate > Go to Tab",
         "shift+cmd+7 ctrl+7", A, "selectTabNumber:", 7, 0, nullptr},
        {"workbench.action.openEditorAtIndex8", "Tab 8", "Navigate > Go to Tab",
         "shift+cmd+8 ctrl+8", A, "selectTabNumber:", 8, 0, nullptr},
        {"workbench.action.lastEditorInGroup", "Last Tab", "Navigate > Go to Tab",
         "shift+cmd+9 ctrl+9", A, "selectTabNumber:", 9, 0, nullptr},
        {"editor.action.revealDefinition", "Go to Definition", "Navigate", "f12", E,
         "goToDefinition:", 0, 0, nullptr},
        {"editor.action.goToReferences", "Find All References", "Navigate", "shift+f12", E,
         "findReferences:", 0, 0, nullptr},
        {"editor.action.marker.next", "Next Problem", "Navigate", "f8", E, "nextProblem:", 0, 0,
         nullptr},
        {"editor.action.marker.prev", "Previous Problem", "Navigate", "shift+f8", E,
         "previousProblem:", 0, 0, nullptr},
        // The compare view's runs of changes, as in VS Code's diff editor.
        {"editor.action.accessibleDiffViewer.next", "Next Difference", "Navigate", "f7", A,
         "goToNextDifference:", 0, 0, nullptr},
        {"editor.action.accessibleDiffViewer.prev", "Previous Difference", "Navigate", "shift+f7",
         A, "goToPreviousDifference:", 0, 0, nullptr},
        // The editor's change bars (quickDiffWidget.ts: Alt+F5, Shift+Alt+F5).
        {"workbench.action.editor.nextChange", "Go to Next Change", "Navigate", "alt+f5", E,
         "nextChange:", 0, 0, nullptr},
        {"workbench.action.editor.previousChange", "Go to Previous Change", "Navigate",
         "alt+shift+f5", E, "previousChange:", 0, 0, nullptr},
        // Only while a tour is on; otherwise Option+] types its character.
        {"minicode.tour.next", "Next Tour Stop", "Navigate", "alt+]", A, "nextTourStop:", 0,
         Global, nullptr},
        {"minicode.tour.previous", "Previous Tour Stop", "Navigate", "alt+[", A,
         "previousTourStop:", 0, Global, nullptr},

        // ---- keys the views handle themselves (not remappable yet)
        // VS Code's Mac keys: Option+Up/Down move lines, Shift+Option+Up/Down
        // copy them, Cmd+Up/Down go to the file's start and end (Cmd+Home
        // and Cmd+End as well), with Shift selecting.
        {"editor.action.moveLinesDownAction", "Move Line Down", "Editor", "alt+down", E, nullptr,
         0, Fixed, "the editor's text view"},
        {"editor.action.moveLinesUpAction", "Move Line Up", "Editor", "alt+up", E, nullptr, 0,
         Fixed, "the editor's text view"},
        {"editor.action.copyLinesDownAction", "Copy Line Down", "Editor", "alt+shift+down", E,
         nullptr, 0, Fixed, "the editor's text view"},
        {"editor.action.copyLinesUpAction", "Copy Line Up", "Editor", "alt+shift+up", E, nullptr,
         0, Fixed, "the editor's text view"},
        {"cursorTop", "Go to the Start of the File", "Editor", "cmd+up cmd+home", E, nullptr, 0,
         Fixed, "the editor's text view"},
        {"cursorTopSelect", "Select to the Start of the File", "Editor",
         "shift+cmd+up shift+cmd+home", E, nullptr, 0, Fixed, "the editor's text view"},
        {"cursorBottom", "Go to the End of the File", "Editor", "cmd+down cmd+end", E, nullptr, 0,
         Fixed, "the editor's text view"},
        {"cursorBottomSelect", "Select to the End of the File", "Editor",
         "shift+cmd+down shift+cmd+end", E, nullptr, 0, Fixed, "the editor's text view"},
        {"deleteAllLeft", "Delete to the Start of the Line", "Editor", "cmd+backspace", E, nullptr,
         0, Fixed, "the editor's text view"},
        {"workbench.action.terminal.scrollUpPage", "Scroll Up a Page", "Terminal",
         "shift+pageup", T, nullptr, 0, Fixed, "the terminal"},
        {"workbench.action.terminal.scrollDownPage", "Scroll Down a Page", "Terminal",
         "shift+pagedown", T, nullptr, 0, Fixed, "the terminal"},
        {"workbench.action.terminal.scrollToTop", "Scroll to the Top", "Terminal", "cmd+home", T,
         nullptr, 0, Fixed, "the terminal"},
        {"workbench.action.terminal.scrollToBottom", "Scroll to the Bottom", "Terminal",
         "cmd+end", T, nullptr, 0, Fixed, "the terminal"},
        // VS Code's keys, between the commands the shell integration marked;
        // in the editor they go to the file's start and end (cursorTop).
        {"workbench.action.terminal.scrollToPreviousCommand", "Scroll to the Previous Command",
         "Terminal", "cmd+up", T, nullptr, 0, Fixed, "the terminal"},
        {"workbench.action.terminal.scrollToNextCommand", "Scroll to the Next Command",
         "Terminal", "cmd+down", T, nullptr, 0, Fixed, "the terminal"},
        {"workbench.action.terminal.selectToPreviousCommand", "Select to the Previous Command",
         "Terminal", "shift+cmd+up", T, nullptr, 0, Fixed, "the terminal"},
        {"workbench.action.terminal.selectToNextCommand", "Select to the Next Command",
         "Terminal", "shift+cmd+down", T, nullptr, 0, Fixed, "the terminal"},
        // At the shell's prompt (or a REPL's); a full-screen program gets them.
        {"minicode.terminal.lineStart", "Go to the Start of the Prompt's Line", "Terminal",
         "cmd+left", T, nullptr, 0, Fixed, "the terminal"},
        {"minicode.terminal.lineEnd", "Go to the End of the Prompt's Line", "Terminal",
         "cmd+right", T, nullptr, 0, Fixed, "the terminal"},
        {"minicode.terminal.deleteToLineStart", "Delete to the Start of the Prompt's Line",
         "Terminal", "cmd+backspace", T, nullptr, 0, Fixed, "the terminal"},
        {"minicode.terminal.wordLeft", "Move a Word Left at the Prompt", "Terminal", "alt+left",
         T, nullptr, 0, Fixed, "the terminal"},
        {"minicode.terminal.wordRight", "Move a Word Right at the Prompt", "Terminal",
         "alt+right", T, nullptr, 0, Fixed, "the terminal"},
        // Finder's keys, typing keys, so never a command's of keys.conf.
        {"minicode.tree.quickLook", "Quick Look", "File Tree", "space", R, nullptr, 0, Fixed,
         "the file tree"},
        {"minicode.tree.closeQuickLook", "Close Quick Look", "File Tree", "escape", R, nullptr, 0,
         Fixed, "the file tree"},
    };
    return table;
}

const Command* find(const std::string& id) {
    for (const Command& c : all())
        if (id == c.id) return &c;
    return nullptr;
}

const Command* findBySelector(const std::string& selector, int tag) {
    for (const Command& c : all())
        if (c.selector && selector == c.selector && c.tag == tag) return &c;
    return nullptr;
}

std::vector<Key> defaultKeys(const Command& c) {
    std::vector<Key> out;
    std::string word;
    auto take = [&]() {
        if (word.empty()) return;
        Key k;
        if (parseKey(word, &k)) out.push_back(k);
        word.clear();
    };
    for (const char* p = c.keys; *p; p++) {
        if (*p == ' ') take();
        else word += *p;
    }
    take();
    return out;
}

// ------------------------------------------------------------ the keymap
const std::vector<Key>& Keymap::keysOf(const std::string& id) const {
    static const std::vector<Key> none;
    auto it = keys.find(id);
    return it == keys.end() ? none : it->second;
}

void Keymap::index() {
    byKey_.clear();
    for (const Command& c : all())
        for (const Key& k : keysOf(c.id)) byKey_[format(k)].push_back(&c);
}

std::vector<const Command*> Keymap::commandsFor(const Key& k) const {
    auto it = byKey_.find(format(k));
    return it == byKey_.end() ? std::vector<const Command*>() : it->second;
}

const Command* Keymap::commandFor(const Key& k, When where, const std::string& language) const {
    const Command *any = nullptr, *place = nullptr;
    for (const Command* c : commandsFor(k)) {
        if (c->language) {
            if (where != When::Any && c->when == where && language == c->language) return c;
            continue;
        }
        if (c->when == where && where != When::Any && where != When::NotPage && !place) place = c;
        if (!any && appliesIn(c->when, where)) any = c;
    }
    return place ? place : any;
}

std::vector<std::string> Keymap::conflicts() const {
    std::vector<std::string> out;
    for (const auto& [key, cmds] : byKey_)
        for (size_t i = 0; i < cmds.size(); i++)
            for (size_t j = i + 1; j < cmds.size(); j++)
                if (overlaps(cmds[i]->when, cmds[j]->when) && sameLanguage(*cmds[i], *cmds[j]))
                    out.push_back(key + ": " + cmds[i]->title + " and " + cmds[j]->title);
    return out;
}

Keymap defaults() {
    Keymap m;
    for (const Command& c : all()) m.keys[c.id] = defaultKeys(c);
    m.index();
    return m;
}

namespace {

std::string describe(const Command& c) {
    std::string s = std::string(c.title) + " (" + c.id + ")";
    return s;
}

void removeKey(std::vector<Key>& v, const Key& k) {
    v.erase(std::remove(v.begin(), v.end(), k), v.end());
}

bool hasKey(const std::vector<Key>& v, const Key& k) {
    return std::find(v.begin(), v.end(), k) != v.end();
}

}  // namespace

Keymap resolve(const std::string& text, std::vector<Error>* errors) {
    Keymap m = defaults();
    auto err = [&](int line, const std::string& msg) {
        if (errors) errors->push_back({line, msg});
    };
    struct Given { const Command* cmd; int line; };
    std::map<std::string, std::vector<Given>> given;   // format(key) -> earlier lines
    std::map<std::string, size_t> userKeys;            // command id -> keys the file gave it
    int lineNo = 0;
    size_t at = 0;
    while (at <= text.size()) {
        size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? text.size() + 1 : nl + 1;
        lineNo++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        // A comment after the command: " #" or a tab before it.
        for (size_t i = 1; i < t.size(); i++)
            if (t[i] == '#' && std::isspace((unsigned char)t[i - 1])) {
                t = trim(t.substr(0, i));
                break;
            }
        // The last "=" splits it, since a key may be "=" (cmd+=).
        size_t eq = t.rfind('=');
        if (eq == std::string::npos) {
            err(lineNo, "expected \"key = command\"");
            continue;
        }
        std::string left = trim(t.substr(0, eq)), right = trim(t.substr(eq + 1));
        if (right.empty()) {
            err(lineNo, "no command after \"=\" (a command's id, or - to take the key away)");
            continue;
        }
        Key k;
        std::string why;
        if (!parseKey(left, &k, &why)) {
            err(lineNo, why);
            continue;
        }
        const std::string ks = format(k);
        if (right == "-") {
            for (const Command& c : all()) {
                std::vector<Key>& v = m.keys[c.id];
                if (!hasKey(v, k)) continue;
                if (c.flags & Fixed) {
                    err(lineNo, ks + " is " + c.title + " in " + placeName(c) + ", which " +
                                    c.note + " handles; keys.conf cannot take it away yet");
                    continue;
                }
                removeKey(v, k);
            }
            given.erase(ks);
            continue;
        }
        const bool remove = right[0] == '-';
        const std::string id = remove ? trim(right.substr(1)) : right;
        const Command* cmd = find(id);
        if (!cmd) {
            err(lineNo, "no command \"" + id + "\" (keys.conf lists them all when Keyboard "
                        "Shortcuts writes it)");
            continue;
        }
        if (cmd->flags & Fixed) {
            err(lineNo, std::string(cmd->title) + " is handled by " + cmd->note +
                            ", so keys.conf cannot change its keys yet");
            continue;
        }
        std::vector<Key>& mine = m.keys[cmd->id];
        if (remove) {
            if (!hasKey(mine, k)) {
                err(lineNo, ks + " is not a key of " + describe(*cmd));
                continue;
            }
            removeKey(mine, k);
            auto& g = given[ks];
            g.erase(std::remove_if(g.begin(), g.end(),
                                   [&](const Given& x) { return x.cmd == cmd; }),
                    g.end());
            continue;
        }
        if (typesText(k) && !cmd->language) {
            err(lineNo, ks + " types or moves in a text field; give it cmd, ctrl or alt "
                             "(only f1 to f20 work alone)");
            continue;
        }
        bool refused = false, again = false;
        for (const Given& g : given[ks]) {
            if (g.cmd == cmd) { again = true; break; }
            if (overlaps(g.cmd->when, cmd->when) && sameLanguage(*g.cmd, *cmd)) {
                // The narrower of the two: Any, then NotPage, then
                // NotPageOrTerminal, then a place.
                const bool wider = placeCount(cmd->when) > placeCount(g.cmd->when);
                const Command& narrower = wider ? *g.cmd : *cmd;
                const bool everywhere = placeCount(narrower.when) > 1;
                err(lineNo, ks + " is already " + describe(*g.cmd) + ", from line " +
                                std::to_string(g.line) + "; a key can run only one command" +
                                (everywhere ? std::string()
                                            : std::string(" in ") + placeName(narrower)));
                refused = true;
                break;
            }
        }
        if (again || refused) continue;
        for (const Command& c : all()) {
            if (&c == cmd || !(c.flags & Fixed) || !overlaps(c.when, cmd->when)) continue;
            if (hasKey(m.keys[c.id], k)) {
                err(lineNo, ks + " is " + c.title + " in " + placeName(c) + ", which " +
                                c.note + " handles; keys.conf cannot change that yet");
                refused = true;
                break;
            }
        }
        if (refused) continue;
        // The key leaves the other commands that had it where this one
        // applies too (by default: an earlier line would have been refused).
        // A command for one language takes it only from that language's
        // others: elsewhere the key stays what it was.
        for (const Command& c : all())
            if (&c != cmd && overlaps(c.when, cmd->when) &&
                (!cmd->language || sameLanguage(c, *cmd)))
                removeKey(m.keys[c.id], k);
        removeKey(mine, k);
        size_t& n = userKeys[cmd->id];
        mine.insert(mine.begin() + (long)std::min(n, mine.size()), k);
        n++;
        given[ks].push_back({cmd, lineNo});
    }
    m.index();
    return m;
}

// ---------------------------------------------------------- the file text
std::string defaultFileText() {
    std::string s =
        "# minicode keyboard shortcuts\n"
        "#\n"
        "# Each line gives a key to a command, beside the keys it has already:\n"
        "#\n"
        "#   ctrl+alt+cmd+k = editor.action.deleteLines\n"
        "#\n"
        "# A minus takes a key away, from every command or from one:\n"
        "#\n"
        "#   ctrl+shift+k = -\n"
        "#   cmd+k = -workbench.action.terminal.clear\n"
        "#\n"
        "# Modifiers are ctrl, alt (or option), shift and cmd, joined with + before\n"
        "# the key: a letter, a digit, ` - = [ ] \\ ; ' , . /, f1 to f20, up, down,\n"
        "# left, right, home, end, pageup, pagedown, enter, tab, space, escape,\n"
        "# backspace, delete, numpad0 to numpad9, or contextmenu (the Menu key).\n"
        "# A key given here is taken from any other command that had it by default,\n"
        "# unless one works only in the editor and the other only in the terminal\n"
        "# (or the file tree, or a web page or a PDF), or one works only in files\n"
        "# of one language (Markdown, Python), where it goes first. Two lines giving\n"
        "# one key to two commands are an error, shown in the status bar. The menus\n"
        "# follow this file as it is saved.\n"
        "#\n"
        "# Every command, with its keys by default, follows. Uncomment a line and\n"
        "# change its key to add one.\n";
    std::string menu;
    for (const Command& c : all()) {
        if (c.flags & Fixed) continue;
        std::string top = c.menu;
        size_t gt = top.find(" >");
        if (gt != std::string::npos) top = top.substr(0, gt);
        if (top != menu) {
            menu = top;
            s += "\n# ---- " + menu + "\n";
        }
        std::string where = c.when == When::Any ? ""
                            : c.when == When::NotPage ? ", but not in a web page or a PDF"
                            : c.when == When::NotPageOrTerminal
                                ? ", but not in a web page, a PDF, or the terminal"
                                : std::string(", in ") + placeName(c);
        std::string comment = std::string(c.menu) + " > " + c.title + where;
        std::vector<Key> keys = defaultKeys(c);
        if (keys.empty()) {
            std::string l = "# (no key) = " + std::string(c.id);
            if (l.size() < 56) l.append(56 - l.size(), ' ');
            s += l + "  # " + comment + "\n";
        }
        for (const Key& k : keys) {
            std::string l = "# " + format(k) + " = " + c.id;
            if (l.size() < 56) l.append(56 - l.size(), ' ');
            s += l + "  # " + comment + "\n";
        }
    }
    s += "\n# ---- Keys the editor and the terminal handle themselves, which this file\n"
         "# cannot change yet, nor give to another command where they apply:\n";
    for (const Command& c : all()) {
        if (!(c.flags & Fixed)) continue;
        std::string l = "#   ";
        for (const Key& k : defaultKeys(c)) l += format(k) + " ";
        if (l.size() < 40) l.append(40 - l.size(), ' ');
        s += l + c.title + ", in " + placeName(c) + "\n";
    }
    return s;
}

}  // namespace Commands
