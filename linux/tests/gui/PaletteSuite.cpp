// PaletteSuite.cpp — the palette over the window (`./build/minicode-guitest
// palette`, linux/PARITY.md item 1.5), the counterpart of the Mac's
// tests/gui/PaletteSuite.mm, by real keys and clicks through XTest.
// Ctrl+Shift+P lists the port's commands with their keys, only those whose
// action is enabled; a fuzzy query ranks word starts first; Return runs the
// command on the next turn and puts the keyboard back; the command comes
// first next time, from the state file; Escape changes nothing and gives a
// text field back its selection; a click outside closes it and goes no
// further; the keyboard going elsewhere closes it. Ctrl+P lists the folder's
// files off the main thread by Find in Folder's rules and opens one, by
// Return or a click, and Quick Open's picks come first; Ctrl+G goes to a line
// and column; deleting the ">" turns commands into files. In the terminal
// Ctrl+P, Ctrl+G and F1 are the program's and Ctrl+Shift+P opens the palette.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "FileTree.h"
#include "Json.h"
#include "PalettePanel.h"

#ifdef MINICODE_ENABLE_TERMINAL
#include "Terminal.h"
#include <vte/vte.h>
#endif

#include <algorithm>
#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
PalettePanel* pal() { return MCGuiApp().palette; }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }
GtkWidget* focusWidget() { return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)); }

bool editorHasKeyboard() { return gtk_widget_has_focus(ed()->textView()); }
bool fieldHasKeyboard() {
    GtkWidget* f = focusWidget();
    return f && (f == pal()->entry() || gtk_widget_is_ancestor(f, pal()->entry()));
}
bool treeHasKeyboard() {
    GtkWidget* f = focusWidget();
    return f && gtk_widget_is_ancestor(f, MCGuiApp().tree->widget());
}

std::string selectedLabel() {
    const std::vector<std::string> l = pal()->labels();
    const int i = pal()->selected();
    return i >= 0 && i < (int)l.size() ? l[(size_t)i] : "";
}
bool listed(const std::string& label) {
    const std::vector<std::string> l = pal()->labels();
    return std::find(l.begin(), l.end(), label) != l.end();
}
int timesListed(const std::string& label) {
    const std::vector<std::string> l = pal()->labels();
    return (int)std::count(l.begin(), l.end(), label);
}
std::string keysOf(const std::string& label) {
    const std::vector<std::string> l = pal()->labels(), k = pal()->keys();
    for (size_t i = 0; i < l.size() && i < k.size(); i++)
        if (l[i] == label) return k[i];
    return "(not listed)";
}
std::string firstFew() {
    std::string s;
    const std::vector<std::string> l = pal()->labels();
    for (size_t i = 0; i < l.size() && i < 5; i++) s += (s.empty() ? "" : ", ") + l[i];
    return s;
}

// The caret's 1-based line and its offset in characters into it.
int caretLine() {
    GtkTextIter it;
    GtkTextBuffer* b = ed()->buffer();
    gtk_text_buffer_get_iter_at_mark(b, &it, gtk_text_buffer_get_insert(b));
    return gtk_text_iter_get_line(&it) + 1;
}
int caretColumn() {
    GtkTextIter it;
    GtkTextBuffer* b = ed()->buffer();
    gtk_text_buffer_get_iter_at_mark(b, &it, gtk_text_buffer_get_insert(b));
    return gtk_text_iter_get_line_offset(&it);
}
bool caretOnScreen() {
    GtkTextView* tv = GTK_TEXT_VIEW(ed()->textView());
    GtkTextIter it;
    GtkTextBuffer* b = ed()->buffer();
    gtk_text_buffer_get_iter_at_mark(b, &it, gtk_text_buffer_get_insert(b));
    GdkRectangle vis, at;
    gtk_text_view_get_visible_rect(tv, &vis);
    gtk_text_view_get_iter_location(tv, &it, &at);
    return vis.height > 0 && at.y >= vis.y && at.y + at.height <= vis.y + vis.height;
}

// The state file's list under `key`.
std::vector<std::string> stateList(const char* key) {
    Json j;
    std::vector<std::string> out;
    if (!Json::parse(MCGuiReadFile(PalettePanel::statePath()), j)) return out;
    for (const Json& e : j[key].elements()) out.push_back(e.asString());
    return out;
}

bool hiddenShown() {
    GVariant* v = g_action_group_get_action_state(G_ACTION_GROUP(MCGuiApplication()), "showhidden");
    const bool on = v && g_variant_get_boolean(v);
    if (v) g_variant_unref(v);
    return on;
}

bool searchBarOpen() {
    return gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(MCGuiApp().searchBar));
}
GtkWidget* findText() {   // the find bar's text, where its keyboard is
    GtkWidget* f = focusWidget();
    return f && GTK_IS_TEXT(f) && gtk_widget_is_ancestor(f, MCGuiApp().searchEntry) ? f : nullptr;
}
// The find bar all the way open: keys typed while it still slides in were
// lost now and then.
bool findBarRevealed() {
    GtkWidget* r = gtk_widget_get_ancestor(MCGuiApp().searchEntry, GTK_TYPE_REVEALER);
    return searchBarOpen() && findText() && gtk_widget_get_mapped(findText()) &&
           (!r || gtk_revealer_get_child_revealed(GTK_REVEALER(r)));
}
std::string findBarText() {
    return findText() ? gtk_editable_get_text(GTK_EDITABLE(findText())) : "(no keyboard)";
}

#ifdef MINICODE_ENABLE_TERMINAL
bool terminalShown() { return gtk_widget_get_visible(MCGuiApp().terminal->widget()); }
bool terminalHasKeyboard() { return MCGuiApp().terminal->owns(focusWidget()); }
VteTerminal* findVte(GtkWidget* root) {
    if (!root) return nullptr;
    if (VTE_IS_TERMINAL(root)) return VTE_TERMINAL(root);
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (VteTerminal* hit = findVte(c)) return hit;
    return nullptr;
}
std::string terminalText() {
    VteTerminal* vte = findVte(MCGuiApp().terminal->widget());
    if (!vte) return "";
    char* text = vte_terminal_get_text_format(vte, VTE_FORMAT_TEXT);
    std::string out = text ? text : "";
    g_free(text);
    return out;
}
bool gTerminalWas = false;
#endif

bool gHiddenWas = false;
size_t gWindows = 0;

}  // namespace

MC_GUI_SUITE(palette) {
    MCGuiStep("files to find, big.txt open, the keyboard in the editor", [] {
        ed()->closeAllTabs();
        MCGuiWriteFile(F("pal/deeper/quick-open-target.md"), "# Target\n");
        MCGuiWriteFile(F("pal/.env.sample"), "KEY=value\n");
        MCGuiWriteFile(F("pal/node_modules/left-out-dep.js"), "module.exports = 1;\n");
        MCGuiOpenFileKept(F("big.txt"));
    });
    MCGuiWait("it shows", 10, [] { return ed()->currentPath() == F("big.txt"); });
    MCGuiStep("a click in the text", [] { MCGuiClick(ed()->textView()); });
    MCGuiWait("the editor has the keyboard", 10, editorHasKeyboard);

    // ---------------------------------------------------------- commands
    MCGuiStep("Ctrl+Shift+P: the port's commands, with \">\"", [] {
        GCHECK(!pal()->isOpen());
        MCGuiKey("ctrl+shift+p");
    });
    MCGuiWait("it opens with the keyboard", 10, [] { return pal()->isOpen() && fieldHasKeyboard(); });
    MCGuiStep("each command once, \"Menu > Title\", with its key in effect", [] {
        GCHECK_EQ_STR(pal()->text(), ">");
        GCHECK(pal()->labels().size() > 40);
        GCHECK(timesListed("View > Toggle Sidebar") == 1);
        GCHECK_EQ_STR(keysOf("View > Toggle Sidebar"), "Ctrl+B");
        GCHECK_EQ_STR(keysOf("View > Show Hidden Files"), "Ctrl+Shift+.");
        GCHECK_EQ_STR(keysOf("Navigate > Go to Line…"), "Ctrl+G");
        GCHECK_EQ_STR(keysOf("Edit > Settings…"), "Ctrl+,");
        GCHECK(listed("File > Save"));
        GCHECK(!listed("View > Command Palette…"));   // not itself
        // Only the enabled: Export PDF wants a LaTeX file on show, the zoom
        // keys a PDF.
        GCHECK(!listed("File > Export PDF…"));
        GCHECK(!listed("View > Page Zoom > Zoom In"));
        MCGuiSnapshot(pal()->box(), "palette-commands");
    });
#ifdef MINICODE_ENABLE_TERMINAL
    MCGuiStep("\"tog term\" ranks Toggle Terminal first", [] {
        GCHECK_EQ_STR(keysOf("View > Toggle Terminal"), "Ctrl+Shift+T");
        gTerminalWas = terminalShown();
        MCGuiType("tog term");
    });
    MCGuiWait("ranked", 10, [] { return pal()->text() == ">tog term"; });
    MCGuiStep("Return runs it", [] {
        GCHECK_EQ_STR(selectedLabel(), "View > Toggle Terminal");
        MCGuiKey("enter");
    });
    MCGuiWait("the palette goes and the terminal toggles", 10, [] {
        return !pal()->isOpen() && terminalShown() != gTerminalWas;
    });
    MCGuiStep("it is the first recent command in the state file; the terminal shut again", [] {
        const std::vector<std::string> recent = stateList("commands");
        GCHECK(!recent.empty() && recent[0] == "workbench.action.terminal.toggleTerminal");
        MCGuiKey("ctrl+shift+t");
    });
    MCGuiWait("shut", 10, [] { return terminalShown() == gTerminalWas; });
    MCGuiStep("the keyboard in the editor", [] { MCGuiClick(ed()->textView()); });
    MCGuiWait("it is", 10, editorHasKeyboard);
#endif
    MCGuiStep("an application action: \"hidden\"", [] {
        if (!pal()->isOpen()) MCGuiKey("ctrl+shift+p");
        gHiddenWas = hiddenShown();
    });
    MCGuiWait("open", 10, [] { return pal()->isOpen() && fieldHasKeyboard(); });
    MCGuiStep("typed", [] { MCGuiType("hidden"); });
    MCGuiWait("ranked", 10, [] { return selectedLabel() == "View > Show Hidden Files"; });
    MCGuiStep("Return", [] { MCGuiKey("enter"); });
    MCGuiWait("the dotfiles flip, the keyboard back in the editor", 10, [] {
        return !pal()->isOpen() && hiddenShown() != gHiddenWas && editorHasKeyboard();
    });
    MCGuiStep("the command comes first next time", [] {
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApplication()), "showhidden", nullptr);
        MCGuiKey("ctrl+shift+p");
    });
    MCGuiWait("open", 10, [] { return pal()->isOpen() && hiddenShown() == gHiddenWas; });
    MCGuiStep("first and chosen", [] {
        GCHECK_EQ_STR(pal()->labels().empty() ? "" : pal()->labels()[0], "View > Show Hidden Files");
        GCHECK_EQ_STR(selectedLabel(), "View > Show Hidden Files");
        const std::vector<std::string> recent = stateList("commands");
        GCHECK(!recent.empty() && recent[0] == "minicode.toggleHiddenFiles");
        MCGuiKey("down");
    });
    MCGuiWait("Down chooses the next", 10, [] { return pal()->selected() == 1; });
    MCGuiStep("Up twice wraps to the last", [] {
        MCGuiKey("up");
        MCGuiKey("up");
    });
    MCGuiWait("the last", 10, [] { return pal()->selected() == (int)pal()->labels().size() - 1; });
    MCGuiStep("Escape closes it", [] { MCGuiKey("escape"); });
    MCGuiWait("closed, the keyboard in the editor", 10, [] {
        return !pal()->isOpen() && editorHasKeyboard();
    });
    MCGuiStep("nothing ran; F1 opens it too", [] {
        GCHECK(hiddenShown() == gHiddenWas);
        MCGuiKey("f1");
    });
    MCGuiWait("open with \">\"", 10, [] { return pal()->isOpen() && pal()->text() == ">"; });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("closed", 10, [] { return !pal()->isOpen() && editorHasKeyboard(); });

    // --------------------------------- the keyboard and a field's selection
    MCGuiStep("the find bar, \"abcdef\" in it, \"cd\" selected", [] {
        GCHECK(!searchBarOpen());
        MCGuiKey("ctrl+f");
    });
    MCGuiWait("it is open with the keyboard", 10, findBarRevealed);
    MCGuiStep("typed", [] { MCGuiType("abcdef"); });
    MCGuiWait("the text arrives", 10, [] { return findBarText() == "abcdef"; });
    MCGuiStep("selected", [] {
        MCGuiKey("home");
        MCGuiKey("right");
        MCGuiKey("right");
        MCGuiKey("shift+right");
        MCGuiKey("shift+right");
    });
    MCGuiWait("\"cd\" is selected", 10, [] {
        int a = 0, b = 0;
        return findText() && gtk_editable_get_selection_bounds(GTK_EDITABLE(findText()), &a, &b) &&
               a == 2 && b == 4;
    });
    MCGuiStep("Ctrl+Shift+P from the find bar", [] { MCGuiKey("ctrl+shift+p"); });
    MCGuiWait("open", 10, [] { return pal()->isOpen() && fieldHasKeyboard(); });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("the find bar has the keyboard again, \"cd\" still selected", 10, [] {
        int a = 0, b = 0;
        return !pal()->isOpen() && findText() &&
               gtk_editable_get_selection_bounds(GTK_EDITABLE(findText()), &a, &b) && a == 2 &&
               b == 4;
    });
    MCGuiStep("the find bar shut", [] { MCGuiKey("ctrl+f"); });
    MCGuiWait("shut", 10, [] { return !searchBarOpen(); });
    MCGuiStep("the keyboard in the editor", [] { MCGuiClick(ed()->textView()); });
    MCGuiWait("it is", 10, editorHasKeyboard);

    // ------------------------------------------------- a click outside
    MCGuiStep("a click outside the box, on a file in the tree", [] { MCGuiKey("ctrl+shift+p"); });
    MCGuiWait("open", 10, [] { return pal()->isOpen() && fieldHasKeyboard(); });
    MCGuiStep("clicked", [] {
        GtkWidget* row = MCGuiFindLabel(MCGuiApp().tree->widget(), "README.md");
        GCHECK(row != nullptr);
        if (row) MCGuiClick(row);
    });
    MCGuiWait("it closes and the keyboard goes back", 10, [] {
        return !pal()->isOpen() && editorHasKeyboard();
    });
    MCGuiPause(0.4);   // a negative check: the click must go no further
    MCGuiStep("the click opened nothing", [] { GCHECK_EQ_STR(ed()->currentPath(), F("big.txt")); });

    // ------------------------------------------ the keyboard going elsewhere
    MCGuiStep("Ctrl+0 with the palette open", [] { MCGuiKey("ctrl+shift+p"); });
    MCGuiWait("open", 10, [] { return pal()->isOpen() && fieldHasKeyboard(); });
    MCGuiStep("Ctrl+0", [] { MCGuiKey("ctrl+0"); });
    MCGuiWait("it closes, the keyboard staying in the tree", 10, [] {
        return !pal()->isOpen() && treeHasKeyboard();
    });
    MCGuiStep("the keyboard in the editor", [] { MCGuiClick(ed()->textView()); });
    MCGuiWait("it is", 10, editorHasKeyboard);

    // -------------------------------------------------------- Quick Open
    MCGuiStep("Ctrl+P: the folder's files", [] { MCGuiKey("ctrl+p"); });
    MCGuiWait("open, empty", 10, [] { return pal()->isOpen() && pal()->text().empty(); });
    MCGuiWait("listed", 10, [] { return pal()->filesListed(); });
    MCGuiStep("off the main thread, by Find in Folder's rules", [] {
        GCHECK(pal()->filesListedOffMainThread());
        GCHECK(listed("pal/.env.sample"));                       // dotfiles listed
        GCHECK(!listed("pal/node_modules/left-out-dep.js"));     // skipped folders not
        GCHECK(listed("big.txt") && listed("README.md"));
        // The tab on show first, as a recent file.
        if (!GCHECK_EQ_STR(pal()->labels().empty() ? "" : pal()->labels()[0], "big.txt"))
            MCGuiNote("listed first: %s", firstFew().c_str());
        GCHECK_EQ_STR(keysOf("big.txt"), "recently opened");
        MCGuiType("qot");
    });
    MCGuiWait("\"qot\" finds it by its name's word starts", 10, [] {
        return selectedLabel() == "pal/deeper/quick-open-target.md";
    });
    MCGuiStep("Return opens it", [] {
        MCGuiSnapshot(pal()->box(), "palette-files");
        MCGuiKey("enter");
    });
    MCGuiWait("opened", 10, [] {
        return !pal()->isOpen() && ed()->currentPath() == F("pal/deeper/quick-open-target.md");
    });
    MCGuiStep("kept in a tab, the keyboard back, the pick in the state file", [] {
        GCHECK(ed()->active() && !ed()->active()->isPreviewTab());
        GCHECK(editorHasKeyboard());
        const std::vector<std::string> files = stateList("files");
        GCHECK(!files.empty() && files[0] == F("pal/deeper/quick-open-target.md"));
    });
    MCGuiStep("\">\" deleted from the commands lists files", [] { MCGuiKey("ctrl+shift+p"); });
    MCGuiWait("open", 10, [] { return pal()->isOpen() && pal()->text() == ">"; });
    MCGuiStep("Backspace", [] { MCGuiKey("backspace"); });
    MCGuiWait("files, the one just opened first", 10, [] {
        return pal()->text().empty() && pal()->filesListed() && !pal()->labels().empty();
    });
    MCGuiStep("recently opened first", [] {
        GCHECK_EQ_STR(pal()->labels()[0], "pal/deeper/quick-open-target.md");
        MCGuiType("left-out");
    });
    MCGuiWait("a skipped file matches nothing", 10, [] {
        return pal()->text() == "left-out" && pal()->labels().empty();
    });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("closed", 10, [] { return !pal()->isOpen(); });

    MCGuiStep("with no tab open, Quick Open's own picks come first", [] {
        ed()->closeAllTabs();
        MCGuiKey("ctrl+p");
    });
    MCGuiWait("listed", 10, [] {
        return pal()->isOpen() && pal()->filesListed() && !pal()->labels().empty();
    });
    MCGuiStep("the earlier pick first, as recently opened", [] {
        GCHECK_EQ_STR(pal()->labels()[0], "pal/deeper/quick-open-target.md");
        GCHECK_EQ_STR(keysOf("pal/deeper/quick-open-target.md"), "recently opened");
        MCGuiType("big");
    });
    MCGuiWait("big.txt", 10, [] { return selectedLabel() == "big.txt"; });
    MCGuiStep("a click on its row opens it", [] {
        GtkWidget* row = MCGuiFindLabel(pal()->box(), "big.txt");
        GCHECK(row != nullptr);
        if (row) MCGuiClick(row);
    });
    MCGuiWait("opened", 10, [] { return !pal()->isOpen() && ed()->currentPath() == F("big.txt"); });

    // -------------------------------------------------------- Go to Line
    MCGuiStep("the keyboard in big.txt", [] { MCGuiClick(ed()->textView()); });
    MCGuiWait("it is", 10, editorHasKeyboard);
    MCGuiStep("Ctrl+G: \":\"", [] { MCGuiKey("ctrl+g"); });
    MCGuiWait("open", 10, [] { return pal()->isOpen() && pal()->text() == ":"; });
    MCGuiStep("asks for a number; \"12x\" is none, and Return does nothing", [] {
        GCHECK_EQ_STR(pal()->labels().empty() ? "" : pal()->labels()[0], "Type a line number to go to");
        MCGuiType("12x");
    });
    MCGuiWait("refused", 10, [] { return pal()->text() == ":12x"; });
    MCGuiStep("Return", [] {
        GCHECK_EQ_STR(pal()->labels()[0], "Type a line number, and a column after a colon if you like");
        MCGuiKey("enter");
    });
    MCGuiPause(0.3);   // a negative check: the palette stays and nothing moves
    MCGuiStep("still open; \"120:6\" instead", [] {
        GCHECK(pal()->isOpen());
        MCGuiKey("backspace");
        MCGuiKey("backspace");
        MCGuiKey("backspace");
        MCGuiType("120:6");
    });
    MCGuiWait("the row", 10, [] { return selectedLabel() == "Go to line 120, column 6"; });
    MCGuiStep("Return", [] { MCGuiKey("enter"); });
    MCGuiWait("the caret on line 120, column 6, in view, the keyboard in the editor", 10, [] {
        return !pal()->isOpen() && caretLine() == 120 && caretColumn() == 5 && caretOnScreen() &&
               editorHasKeyboard();
    });
    MCGuiStep("Ctrl+G \"42,7\"", [] { MCGuiKey("ctrl+g"); });
    MCGuiWait("open", 10, [] { return pal()->isOpen() && pal()->text() == ":"; });
    MCGuiStep("typed", [] { MCGuiType("42,7"); });
    MCGuiWait("the row", 10, [] { return selectedLabel() == "Go to line 42, column 7"; });
    MCGuiStep("Return", [] { MCGuiKey("enter"); });
    MCGuiWait("line 42, column 7", 10, [] {
        return !pal()->isOpen() && caretLine() == 42 && caretColumn() == 6 && caretOnScreen() &&
               editorHasKeyboard();
    });
    MCGuiStep("\"#\": not on Linux yet, said so", [] { MCGuiKey("ctrl+p"); });
    MCGuiWait("open", 10, [] { return pal()->isOpen(); });
    MCGuiStep("typed", [] { MCGuiType("#"); });
    MCGuiWait("nothing listed", 10, [] {
        return pal()->text() == "#" && pal()->labels().empty() &&
               pal()->emptyText().rfind("Go to Symbol in Workspace is not on Linux yet", 0) == 0;
    });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("closed", 10, [] { return !pal()->isOpen() && editorHasKeyboard(); });

#ifdef MINICODE_ENABLE_PDF
    // ------------------------------------------- an action enabled by the file
    MCGuiStep("a LaTeX file: Export PDF is offered", [] {
        MCGuiWriteFile(F("pal/paper.tex"),
                       "\\documentclass{article}\n\\begin{document}\nHello.\n\\end{document}\n");
        MCGuiOpenFileKept(F("pal/paper.tex"));
    });
    MCGuiWait("it shows", 10, [] { return ed()->currentPath() == F("pal/paper.tex"); });
    MCGuiStep("the palette", [] { MCGuiKey("ctrl+shift+p"); });
    MCGuiWait("open", 10, [] { return pal()->isOpen(); });
    MCGuiStep("Export PDF is there now", [] {
        GCHECK(listed("File > Export PDF…"));
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return !pal()->isOpen(); });
    MCGuiStep("back to big.txt", [] { MCGuiOpenFileKept(F("big.txt")); });
    MCGuiWait("it shows", 10, [] { return ed()->currentPath() == F("big.txt"); });
#endif

    // ----------------------------- a window closed with its palette open
    MCGuiStep("a second window", [] {
        gWindows = MCGuiAppWindows().size();
        MCGuiKey("ctrl+shift+n");
    });
    MCGuiWait("it opens", 10, [] { return MCGuiAppWindows().size() == gWindows + 1; });
    MCGuiStep("Quick Open there, and Ctrl+Shift+W while it lists", [] {
        MCGuiKey("ctrl+p");
        MCGuiKey("ctrl+shift+w");
    });
    MCGuiWait("the window goes", 10, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiPause(0.5);   // a negative check: a listing coming back to it finds nothing to fill
    MCGuiStep("the first window's palette still works", [] {
        MCGuiFocusKeyWindow();
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+shift+p");
    });
    MCGuiWait("open", 10, [] { return pal()->isOpen() && fieldHasKeyboard(); });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("closed", 10, [] { return !pal()->isOpen() && editorHasKeyboard(); });

#ifdef MINICODE_ENABLE_TERMINAL
    // ------------------------------------------- the terminal keeps its keys
    MCGuiStep("the terminal with the keyboard", [] {
        // An earlier suite may have left it open.
        gTerminalWas = terminalShown();
        if (gTerminalWas) {
            MCGuiClick(GTK_WIDGET(findVte(MCGuiApp().terminal->widget())));
        } else {
            MCGuiClick(ed()->textView());
            MCGuiKey("ctrl+`");
        }
    });
    MCGuiWait("it has it", 10, terminalHasKeyboard);
    MCGuiWait("the shell is up", 20, [] {
        return terminalText().find_first_not_of(" \n") != std::string::npos;
    });
    MCGuiStep("a program that writes the bytes it reads", [] {
        MCGuiType("stty raw -echo; head -c 5 > pal-keys.bin; stty sane\n");
    });
    MCGuiWait("it is reading", 20, [] { return MCGuiFileExists(F("pal-keys.bin")); });
    MCGuiStep("Ctrl+P, Ctrl+G and F1", [] {
        MCGuiKey("ctrl+p");
        MCGuiKey("ctrl+g");
        MCGuiKey("f1");
    });
    MCGuiWait("the program got them", 20, [] { return MCGuiReadFile(F("pal-keys.bin")).size() == 5; });
    MCGuiStep("as their bytes, and no palette", [] {
        GCHECK(MCGuiReadFile(F("pal-keys.bin")) == std::string("\x10\x07\x1bOP"));
        GCHECK(!pal()->isOpen());
        GCHECK(terminalHasKeyboard());
        MCGuiKey("ctrl+shift+p");
    });
    MCGuiWait("Ctrl+Shift+P opens it from the terminal", 10, [] {
        return pal()->isOpen() && pal()->text() == ">" && fieldHasKeyboard();
    });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("the terminal has the keyboard again", 10, [] {
        return !pal()->isOpen() && terminalHasKeyboard();
    });
    MCGuiStep("the terminal as it was", [] {
        if (!gTerminalWas) MCGuiKey("ctrl+shift+t");
    });
    MCGuiWait("as it was", 10, [] { return terminalShown() == gTerminalWas; });
#endif
}
