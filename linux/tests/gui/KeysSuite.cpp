// KeysSuite.cpp — the registry's keys in the GTK port (`./build/minicode-guitest
// keys`, linux/PARITY.md item 0.3): the menu bar and the accelerators come from
// CommandsLinux.cpp's table and the port's actions; every key D1 moved is
// pressed through XTest, in the editor and with the terminal focused, and
// what it did is read back (and that its old key no longer does it); with
// the terminal focused, plain Ctrl keys reach the program in it, read as the
// bytes a raw `head` wrote; and keys.conf, a scratch $MINICODE_KEYS, moves
// the real accelerators, the tree's keys and the hints.
#include "Harness.h"

#include "Commands.h"
#include "Editor.h"
#include "FileTree.h"
#include "Keys.h"

#ifdef MINICODE_ENABLE_TERMINAL
#include "Terminal.h"
#include <vte/vte.h>
#endif

#include <map>
#include <set>

namespace {

GtkWidget* appWindow() { return GTK_WIDGET(MCGuiApp().window); }

GAction* lookup(const std::string& full) {
    const std::string name = full.substr(full.find('.') + 1);
    if (full.rfind("app.", 0) == 0)
        return g_action_map_lookup_action(G_ACTION_MAP(MCGuiApplication()), name.c_str());
    return g_action_map_lookup_action(G_ACTION_MAP(MCGuiApp().window), name.c_str());
}

std::vector<std::string> accels(const char* action) {
    std::vector<std::string> out;
    gchar** list = gtk_application_get_accels_for_action(MCGuiApplication(), action);
    for (gchar** a = list; a && *a; a++) out.push_back(*a);
    g_strfreev(list);
    return out;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : " ") + x;
    return s;
}

// Accelerators as GTK writes them back ("<Shift><Control>v" for
// "<Ctrl><Shift>v"), space separated, so that the two spellings compare.
std::string canon(const std::string& list) {
    std::vector<std::string> out;
    gchar** parts = g_strsplit(list.c_str(), " ", -1);
    for (gchar** p = parts; p && *p; p++) {
        if (!**p) continue;
        guint key = 0;
        GdkModifierType mods = (GdkModifierType)0;
        if (!gtk_accelerator_parse(*p, &key, &mods)) {
            out.push_back(std::string("(unparsable ") + *p + ")");
            continue;
        }
        gchar* name = gtk_accelerator_name(key, mods);
        out.push_back(name);
        g_free(name);
    }
    g_strfreev(parts);
    return joined(out);
}

std::string bound(const char* action) { return canon(joined(accels(action))); }

// How many times each watched action of the first window ran.
std::map<std::string, int> gRuns;
std::vector<std::pair<GAction*, gulong>> gWatching;

void watchActions() {
    for (const char* full : {"win.togglepreview", "win.toggletask", "win.todos", "win.newfile",
                             "win.newfolder", "win.newwindow", "win.close", "win.findnext",
                             "win.hoverinfo", "win.togglesidebar", "win.find", "win.open",
                             "app.quit"}) {
        GAction* a = lookup(full);
        GCHECK(a != nullptr);
        if (!a) continue;
        const gulong id = g_signal_connect(a, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*,
                                                                         gpointer name) {
                                               gRuns[static_cast<const char*>(name)]++;
                                           }),
                                           (gpointer)full);
        gWatching.push_back({a, id});
    }
}

void unwatchActions() {
    for (const auto& [a, id] : gWatching) g_signal_handler_disconnect(a, id);
    gWatching.clear();
}

bool hiddenShown() {
    GVariant* v = g_action_get_state(lookup("app.showhidden"));
    const bool on = v && g_variant_get_boolean(v);
    if (v) g_variant_unref(v);
    return on;
}

GtkWidget* focusWidget() { return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)); }

bool editorHasKeyboard() { return gtk_widget_has_focus(MCGuiApp().editor->textView()); }

bool labelShown(const char* text) { return MCGuiFindLabel(appWindow(), text) != nullptr; }

bool searchBarOpen() {
    return gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(MCGuiApp().searchBar));
}

// The line of the buffer starting with `prefix`'s end, or "".
std::string lineWith(const std::string& needle) {
    const std::string text = MCGuiEditorText();
    const size_t at = text.find(needle);
    if (at == std::string::npos) return "";
    const size_t start = text.rfind('\n', at);
    const size_t end = text.find('\n', at);
    return text.substr(start == std::string::npos ? 0 : start + 1,
                       (end == std::string::npos ? text.size() : end) -
                           (start == std::string::npos ? 0 : start + 1));
}

void caretToLine(int line) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line(MCGuiApp().editor->buffer(), &it, line);
    gtk_text_buffer_place_cursor(MCGuiApp().editor->buffer(), &it);
}

// The menu bar's items, as (label, action), sections and submenus walked;
// top-level menus' names in `tops`.
void walkMenu(GMenuModel* model, std::vector<std::pair<std::string, std::string>>* items,
              std::vector<std::string>* tops) {
    for (int i = 0; i < g_menu_model_get_n_items(model); i++) {
        gchar* label = nullptr;
        gchar* action = nullptr;
        g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_LABEL, "s", &label);
        g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_ACTION, "s", &action);
        if (action) items->push_back({label ? label : "", action});
        for (const char* link : {G_MENU_LINK_SECTION, G_MENU_LINK_SUBMENU}) {
            GMenuModel* sub = g_menu_model_get_item_link(model, i, link);
            if (!sub) continue;
            if (tops && label && link == std::string(G_MENU_LINK_SUBMENU)) tops->push_back(label);
            walkMenu(sub, items, nullptr);
            g_object_unref(sub);
        }
        g_free(label);
        g_free(action);
    }
}

#ifdef MINICODE_ENABLE_TERMINAL
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

bool terminalHasKeyboard() { return MCGuiApp().terminal->owns(focusWidget()); }
#endif

std::string keysMd() { return MCGuiProjectFile("keys.md"); }
std::string keysTxt() { return MCGuiProjectFile("keys.txt"); }

size_t gWindows = 0;
bool gHidden = false;
std::map<std::string, int> gBefore;

}  // namespace

MC_GUI_SUITE(keys) {
    // An earlier suite may leave an edit unsaved (harness does), and every
    // open below would then ask about it.
    MCGuiStep("an editor with nothing unsaved", [] {
        if (MCGuiApp().editor->dirty()) MCGuiKey("ctrl+s");
    });
    MCGuiWait("saved", 10, [] { return !MCGuiApp().editor->dirty(); });

    // ---- the tables and what they build
    MCGuiStep("CommandsLinux.cpp's rows and the port's actions agree", [] {
        for (const std::string& p : Keys::tableProblems()) MCGuiNote("%s", p.c_str());
        GCHECK(Keys::tableProblems().empty());
        for (const Keys::Action& a : Keys::actions()) {
            const bool there = lookup(a.action) != nullptr;
            if (!there) MCGuiNote("no action %s for %s", a.action, a.id);
            GCHECK(there);
        }
    });
    MCGuiStep("the menu bar holds each command with an action once, in Linux's words", [] {
        std::vector<std::pair<std::string, std::string>> items;
        std::vector<std::string> tops;
        walkMenu(gtk_application_get_menubar(MCGuiApplication()), &items, &tops);
        GCHECK_EQ_STR(joined(tops), "File Edit View Navigate");
        std::map<std::string, std::string> byAction;
        for (const auto& [label, action] : items)
            GCHECK(byAction.insert({action, label}).second);   // once each
        GCHECK(items.size() == Keys::actions().size());
        for (const Keys::Action& a : Keys::actions()) {
            const Commands::Command* c = Commands::find(a.id);
            GCHECK(c && byAction.count(a.action));
            if (c && byAction.count(a.action))
                GCHECK_EQ_STR(byAction[a.action], Commands::titleOf(*c, Commands::Platform::Linux));
        }
        // Linux's words and places: Quit in File, Settings in Edit, and no
        // command without an action (the palette) anywhere.
        GCHECK_EQ_STR(byAction["win.reveal"], "Open Containing Folder");
        GCHECK_EQ_STR(byAction["app.quit"], "Quit");
        GCHECK_EQ_STR(byAction["win.togglepreview"], "Toggle Preview");
        GCHECK(byAction.count("win.showcommands") == 0);
    });
    MCGuiStep("each action's accelerators are its Linux keys", [] {
        GCHECK_EQ_STR(bound("win.togglepreview"), canon("<Ctrl><Shift>v"));
        GCHECK_EQ_STR(bound("win.todos"), canon("<Ctrl><Shift>d"));
        GCHECK_EQ_STR(bound("win.newfile"), canon("<Ctrl>n"));
        GCHECK_EQ_STR(bound("win.newwindow"), canon("<Ctrl><Shift>n"));
        GCHECK_EQ_STR(bound("win.newfolder"), canon("<Ctrl><Alt>n"));
        GCHECK_EQ_STR(bound("win.close"), canon("<Ctrl><Shift>w"));
        GCHECK_EQ_STR(bound("win.hoverinfo"), canon("<Ctrl><Alt>i"));
        GCHECK_EQ_STR(bound("win.findnext"), canon("F3"));
        GCHECK_EQ_STR(bound("app.showhidden"), canon("<Ctrl>greater"));
        GCHECK_EQ_STR(bound("win.zoomin"), canon("<Ctrl>equal <Ctrl>plus <Ctrl>KP_Add"));
        // The tree's keys are its own controller's, never the window's.
        GCHECK(accels("win.rename").empty() && accels("win.trash").empty());
        // Every other command's are its keys, as GTK writes them.
        for (const Keys::Action& a : Keys::actions()) {
            const Commands::Command* c = Commands::find(a.id);
            if (!c || c->language || c->when == Commands::When::Tree) continue;
            std::vector<std::string> want;
            for (const Commands::Key& k : Keys::keymap().keysOf(a.id))
                for (const std::string& s : Commands::linuxAccelerators(k)) want.push_back(s);
            GCHECK_EQ_STR(bound(a.action), canon(joined(want)));
        }
    });

    // ---- Keyboard Shortcuts…, before any keys.conf: it writes one
    MCGuiStep("Keyboard Shortcuts… writes keys.conf with Linux's commands and opens it", [] {
        GCHECK(!MCGuiFileExists(Keys::path()));
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApp().window), "keyboardshortcuts",
                                       nullptr);
    });
    MCGuiWait("the editor shows it", 10, [] {
        return MCGuiApp().editor->currentPath() == Keys::path() &&
               MCGuiEditorText().rfind("# minicode keyboard shortcuts", 0) == 0;
    });
    MCGuiStep("every line is commented out, so the keys are the defaults", [] {
        const std::string text = MCGuiReadFile(Keys::path());
        GCHECK(text.find("# ctrl+shift+v = markdown.showPreview") != std::string::npos);
        GCHECK(text.find("cmd+") == std::string::npos);
        GCHECK(text.find("workbench.action.showCommands") == std::string::npos);
        GCHECK(Keys::errors().empty());
        GCHECK_EQ_STR(bound("win.togglepreview"), canon("<Ctrl><Shift>v"));
    });

    // ---- in the editor
    MCGuiStep("a Markdown file and a text file", [] {
        MCGuiWriteFile(keysMd(), "# Keys\n\nfirst item\nsecond item\n");
        MCGuiWriteFile(keysTxt(), "plain text\n");
        MCGuiOpenFile(keysMd());
        watchActions();
        gWindows = MCGuiAppWindows().size();
    });
    MCGuiWait("the Markdown file shows, rendered", 10, [] {
        return MCGuiApp().editor->currentPath() == keysMd() && MCGuiApp().editor->inPreview();
    });
    MCGuiStep("a click in it gives it the keyboard", [] { MCGuiClick(MCGuiApp().editor->textView()); });
    MCGuiWait("the editor has the keyboard", 5, editorHasKeyboard);
    MCGuiStep("Ctrl+Shift+P, the preview's old key, is free (the palette's)", [] {
        MCGuiKey("ctrl+shift+p");
    });
    MCGuiPause(0.3);   // a negative check: nothing may happen in this time
    MCGuiStep("the preview stays", [] {
        GCHECK(MCGuiApp().editor->inPreview());
        GCHECK(gRuns["win.togglepreview"] == 0);
    });
    MCGuiStep("Ctrl+Shift+V toggles it", [] { MCGuiKey("ctrl+shift+v"); });
    MCGuiWait("the source shows", 10, [] { return !MCGuiApp().editor->inPreview(); });
    MCGuiStep("the caret on a line, the keyboard in the source", [] {
        MCGuiClick(MCGuiApp().editor->textView());
    });
    MCGuiWait("the editor has the keyboard", 5, editorHasKeyboard);
    MCGuiStep("Ctrl+L, the task's old key, is free (Select Line's)", [] {
        caretToLine(2);
        MCGuiKey("ctrl+l");
    });
    MCGuiPause(0.3);   // a negative check
    MCGuiStep("the line is as it was", [] {
        GCHECK_EQ_STR(lineWith("first item"), "first item");
        GCHECK(gRuns["win.toggletask"] == 0);
    });
    MCGuiStep("Ctrl+Alt+L makes it a task", [] { MCGuiKey("ctrl+alt+l"); });
    MCGuiWait("it is one", 10, [] { return lineWith("first item") == "- [ ] first item"; });
    MCGuiStep("in a Markdown file Alt+C is the task key too", [] {
        GCHECK_EQ_STR(bound("win.toggletask"), canon("<Ctrl><Alt>l <Alt>c"));
        MCGuiKey("alt+c");
    });
    MCGuiWait("it is ticked", 10, [] { return lineWith("first item") == "- [x] first item"; });
    MCGuiStep("saved", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("the file has it", 10, [] {
        return !MCGuiApp().editor->dirty() &&
               MCGuiReadFile(keysMd()).find("- [x] first item") != std::string::npos;
    });

    MCGuiStep("the text file", [] { MCGuiOpenFile(keysTxt()); });
    MCGuiWait("it shows", 10, [] { return MCGuiApp().editor->currentPath() == keysTxt(); });
    MCGuiStep("the keyboard in it", [] { MCGuiClick(MCGuiApp().editor->textView()); });
    MCGuiWait("the editor has the keyboard", 5, editorHasKeyboard);
    MCGuiStep("outside Markdown Alt+C is no key of the task's", [] {
        GCHECK_EQ_STR(bound("win.toggletask"), canon("<Ctrl><Alt>l"));
        gBefore = gRuns;
        MCGuiKey("alt+c");
    });
    MCGuiPause(0.3);   // a negative check
    MCGuiStep("nothing ran and nothing changed", [] {
        GCHECK(gRuns["win.toggletask"] == gBefore["win.toggletask"]);
        GCHECK_EQ_STR(MCGuiEditorText(), "plain text\n");
    });

    MCGuiStep("Ctrl+Shift+L, the TODOs' old key, opens nothing", [] { MCGuiKey("ctrl+shift+l"); });
    MCGuiPause(0.3);   // a negative check
    MCGuiStep("no window came up", [] {
        GCHECK(MCGuiTopWindow() == MCGuiApp().window);
        GCHECK(gRuns["win.todos"] == 0);
    });
    MCGuiStep("Ctrl+Shift+D opens the TODOs window", [] { MCGuiKey("ctrl+shift+d"); });
    MCGuiWait("it is up", 10, [] { return MCGuiTopWindow() != MCGuiApp().window; });
    MCGuiStep("Escape closes it", [] {
        GCHECK(gRuns["win.todos"] == 1);
        MCGuiSetKeyWindow(MCGuiTopWindow());
        MCGuiKey("escape");
    });
    MCGuiWait("it is gone", 10, [] { return MCGuiTopWindow() == MCGuiApp().window; });
    MCGuiStep("the keyboard back in the editor", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();   // a window manager would, as the TODOs window went
        MCGuiClick(MCGuiApp().editor->textView());
    });
    MCGuiWait("the editor has the keyboard", 5, editorHasKeyboard);

    MCGuiStep("Ctrl+N is New File", [] { MCGuiKey("ctrl+n"); });
    MCGuiWait("it asks for the name", 10, [] { return labelShown("New file name"); });
    MCGuiStep("no new window", [] {
        GCHECK(MCGuiAppWindows().size() == gWindows);
        GCHECK(gRuns["win.newfile"] == 1 && gRuns["win.newwindow"] == 0);
        MCGuiKey("escape");
    });
    MCGuiWait("the question goes", 10, [] { return !labelShown("New file name"); });
    MCGuiStep("Ctrl+Alt+N, New File's old key, is New Folder", [] { MCGuiKey("ctrl+alt+n"); });
    MCGuiWait("it asks for the folder's name", 10, [] { return labelShown("New folder name"); });
    MCGuiStep("put away", [] {
        GCHECK(gRuns["win.newfolder"] == 1 && gRuns["win.newfile"] == 1);
        MCGuiKey("escape");
    });
    MCGuiWait("the question goes", 10, [] { return !labelShown("New folder name"); });

    MCGuiStep("Ctrl+Shift+N, New Folder's old key, is New Window", [] {
        MCGuiClick(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+shift+n");
    });
    MCGuiWait("a second window", 10, [] { return MCGuiAppWindows().size() == gWindows + 1; });
    MCGuiStep("Ctrl+W, Close Window's old key, is Close Tab's: in a window with no tab open it "
              "closes the window, as on the Mac", [] {
        // A new window on the folder has the folder's tabs (its session, as
        // on the Mac); they go first, so it has none.
        MCGuiApp().editor->closeAllTabs();
        GCHECK(MCGuiApp().editor->tabCount() == 0);
        MCGuiKey("ctrl+w");   // to the new window, the newest
    });
    MCGuiWait("the new window closes", 10, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("the first window still has its tabs; another new window", [] {
        GCHECK(MCGuiApp().editor->tabCount() > 0);
        MCGuiClick(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+shift+n");
    });
    MCGuiWait("a second window", 10, [] { return MCGuiAppWindows().size() == gWindows + 1; });
    MCGuiStep("Ctrl+Shift+W closes it", [] { MCGuiKey("ctrl+shift+w"); });
    MCGuiWait("one again", 10, [] { return MCGuiAppWindows().size() == gWindows; });

    MCGuiStep("Ctrl+H, the dotfiles' old key, is free (Replace's)", [] {
        MCGuiClick(MCGuiApp().editor->textView());
        gHidden = hiddenShown();
        MCGuiKey("ctrl+h");
    });
    MCGuiPause(0.3);   // a negative check
    MCGuiStep("the dotfiles are as they were; Ctrl+Shift+. shows them", [] {
        GCHECK(hiddenShown() == gHidden);
        MCGuiKey("ctrl+shift+.");
    });
    MCGuiWait("they flip", 10, [] { return hiddenShown() != gHidden; });
    MCGuiStep("and back", [] { MCGuiKey("ctrl+shift+."); });
    MCGuiWait("as they were", 10, [] { return hiddenShown() == gHidden; });

    MCGuiStep("Ctrl+G, Find Next's old key, is free (Go to Line's)", [] {
        GCHECK(!searchBarOpen());
        MCGuiKey("ctrl+g");
    });
    MCGuiPause(0.3);   // a negative check
    MCGuiStep("the find bar stays shut; F3, with nothing to find, opens it", [] {
        GCHECK(!searchBarOpen());
        GCHECK(gRuns["win.findnext"] == 0);
        MCGuiKey("f3");
    });
    MCGuiWait("it is open", 10, searchBarOpen);
    MCGuiStep("Ctrl+F shuts it", [] {
        GCHECK(gRuns["win.findnext"] == 1);
        MCGuiKey("ctrl+f");
    });
    MCGuiWait("shut", 10, [] { return !searchBarOpen(); });

    MCGuiStep("Ctrl+I, the hover's old key, is free (the inline agent's)", [] {
        MCGuiClick(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+i");
    });
    MCGuiPause(0.3);   // a negative check
    MCGuiStep("the hover did not run; Ctrl+Alt+I runs it", [] {
        GCHECK(gRuns["win.hoverinfo"] == 0);
        MCGuiKey("ctrl+alt+i");
    });
    MCGuiWait("it ran", 10, [] { return gRuns["win.hoverinfo"] == 1; });

#ifdef MINICODE_ENABLE_TERMINAL
    // ---- with the terminal focused
    MCGuiStep("Ctrl+` opens the terminal with the keyboard", [] {
        MCGuiClick(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+`");
    });
    MCGuiWait("the terminal has it", 10, terminalHasKeyboard);
    MCGuiWait("the shell is up", 20, [] { return terminalText().find_first_not_of(" \n") != std::string::npos; });
    MCGuiStep("there the plain Ctrl keys and F3 are no accelerators, Shift and Alt ones are", [] {
        GCHECK(accels("win.newfile").empty());
        GCHECK(accels("win.findnext").empty());
        GCHECK(accels("win.togglesidebar").empty());
        GCHECK(accels("app.quit").empty());
        GCHECK(accels("win.togglepreview").empty());   // VTE's paste
        GCHECK(accels("win.toggletask").empty());      // the editor's
        GCHECK(accels("win.hoverinfo").empty());
        GCHECK_EQ_STR(bound("win.newwindow"), canon("<Ctrl><Shift>n"));
        GCHECK_EQ_STR(bound("win.newfolder"), canon("<Ctrl><Alt>n"));
        GCHECK_EQ_STR(bound("win.todos"), canon("<Ctrl><Shift>d"));
        GCHECK_EQ_STR(bound("win.toggleterminal"), canon("<Ctrl><Shift>t <Ctrl>grave"));
        GCHECK_EQ_STR(bound("win.focustree"), canon("<Ctrl>0"));
        GCHECK(accels("win.togglegit").empty());   // the shell reads it as Ctrl+G
        // The tabs: their Ctrl keys leave the terminal (Global), Alt and
        // Shift ones are the app's anyway, and Close Tab's are the shell's
        // (Ctrl+W deletes a word).
        GCHECK_EQ_STR(bound("win.nexttab"), canon("<Ctrl>Page_Down <Ctrl>Tab"));
        GCHECK_EQ_STR(bound("win.previoustab"), canon("<Ctrl>Page_Up <Ctrl><Shift>Tab"));
        GCHECK_EQ_STR(bound("win.tab1"), canon("<Alt>1"));
        GCHECK_EQ_STR(bound("win.lasttab"), canon("<Alt>9 <Alt>0"));
        GCHECK_EQ_STR(bound("win.reopentab"), canon("<Ctrl><Alt><Shift>t"));
        GCHECK_EQ_STR(bound("win.saveall"), canon("<Ctrl><Alt>s"));
        GCHECK(accels("win.closetab").empty());
    });
    MCGuiStep("a program that writes the bytes it reads", [] {
        MCGuiType("stty raw -echo; head -c 17 > keys.bin; stty sane\n");
    });
    MCGuiWait("it is reading", 20, [] { return MCGuiFileExists(MCGuiProjectFile("keys.bin")); });
    MCGuiStep("the plain Ctrl keys, old and new, and the editor's keys", [] {
        gBefore = gRuns;
        for (const char* k : {"ctrl+n", "ctrl+l", "ctrl+h", "ctrl+g", "ctrl+w", "ctrl+i", "ctrl+b",
                              "ctrl+o", "ctrl+q", "ctrl+f", "ctrl+alt+l", "ctrl+alt+i", "f3"})
            MCGuiKey(k);
    });
    MCGuiWait("the program got them all", 20, [] {
        return MCGuiReadFile(MCGuiProjectFile("keys.bin")).size() == 17;
    });
    MCGuiStep("as bytes, in order, and no command ran", [] {
        GCHECK(MCGuiReadFile(MCGuiProjectFile("keys.bin")) ==
               std::string("\x0e\x0c\x08\x07\x17\x09\x02\x0f\x11\x06\x1b\x0c\x1b\x09\x1bOR"));
        for (const auto& [name, n] : gRuns)
            if (n != gBefore[name]) MCGuiNote("%s ran in the terminal", name.c_str());
        GCHECK(gRuns == gBefore);
        GCHECK(MCGuiAppWindows().size() == gWindows);
        GCHECK(!searchBarOpen());
        GCHECK(!labelShown("New file name"));
        GCHECK(terminalHasKeyboard());
    });
    MCGuiStep("Ctrl+Shift+V pastes there, not the preview", [] {
        gdk_clipboard_set_text(gtk_widget_get_clipboard(appWindow()), "Zq");
        MCGuiType("printf %s '");
        MCGuiKey("ctrl+shift+v");
        MCGuiType("' > paste.txt\n");
    });
    MCGuiWait("the shell got the paste", 20, [] {
        return MCGuiReadFile(MCGuiProjectFile("paste.txt")) == "Zq";
    });
    MCGuiStep("and the preview's action did not run", [] {
        GCHECK(gRuns["win.togglepreview"] == gBefore["win.togglepreview"]);
    });
    MCGuiStep("Ctrl+Shift+D from the terminal opens the TODOs window", [] { MCGuiKey("ctrl+shift+d"); });
    MCGuiWait("it is up", 10, [] { return MCGuiTopWindow() != MCGuiApp().window; });
    MCGuiStep("Escape closes it", [] {
        MCGuiSetKeyWindow(MCGuiTopWindow());
        MCGuiKey("escape");
    });
    MCGuiWait("it is gone", 10, [] { return MCGuiTopWindow() == MCGuiApp().window; });
    MCGuiStep("back in the main window, the terminal still has the keyboard", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
    });
    MCGuiWait("it has", 10, [] { return gtk_window_is_active(MCGuiApp().window) && terminalHasKeyboard(); });
    MCGuiStep("Ctrl+Shift+N from the terminal opens a window", [] { MCGuiKey("ctrl+shift+n"); });
    MCGuiWait("a second window", 10, [] { return MCGuiAppWindows().size() == gWindows + 1; });
    MCGuiStep("Ctrl+Shift+W closes it", [] { MCGuiKey("ctrl+shift+w"); });
    MCGuiWait("one again", 10, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("the first window's terminal still has the keyboard", [] { MCGuiFocusKeyWindow(); });
    MCGuiWait("it has", 10, [] { return gtk_window_is_active(MCGuiApp().window) && terminalHasKeyboard(); });
    MCGuiStep("Ctrl+Alt+N from the terminal asks for a folder's name", [] { MCGuiKey("ctrl+alt+n"); });
    MCGuiWait("it asks", 10, [] { return labelShown("New folder name"); });
    MCGuiStep("put away", [] { MCGuiKey("escape"); });
    MCGuiWait("the question goes", 10, [] { return !labelShown("New folder name"); });
    MCGuiStep("a click gives the terminal the keyboard back", [] {
        MCGuiClick(MCGuiApp().terminal->widget());
    });
    MCGuiWait("it has", 10, terminalHasKeyboard);
    MCGuiStep("Ctrl+0, Global, goes to the tree from there", [] { MCGuiKey("ctrl+0"); });
    MCGuiWait("the tree has the keyboard", 10, [] {
        return Keys::within(focusWidget(), MCGuiApp().tree->widget());
    });
#endif

    // ---- keys.conf
    MCGuiStep("keys.conf moves the preview, Rename and nothing else, and refuses a Mac line", [] {
        MCGuiSetKeys("ctrl+alt+j = markdown.showPreview\n"
                     "ctrl+shift+v = -\n"
                     "f2 = -renameFile\n"
                     "ctrl+alt+r = renameFile\n"
                     "cmd+k = minicode.toggleBrowser\n");
    });
    MCGuiWait("the accelerators follow", 10, [] {
        return bound("win.togglepreview") == canon("<Ctrl><Alt>j");
    });
    MCGuiStep("the status bar names the refused line", [] {
        const std::string note = gtk_label_get_text(GTK_LABEL(MCGuiApp().settingsLabel));
        GCHECK(note.rfind("Keys line 5:", 0) == 0);
        GCHECK_EQ_STR(bound("win.newfile"), canon("<Ctrl>n"));   // the rest as they were
    });
    MCGuiStep("the Markdown file's tab again, rendered", [] {
        MCGuiOpenFile(keysMd());
        // Its tab kept the source it was left showing.
        if (!MCGuiApp().editor->inPreview()) MCGuiApp().editor->togglePreview();
    });
    MCGuiWait("it shows, rendered", 10, [] {
        return MCGuiApp().editor->currentPath() == keysMd() && MCGuiApp().editor->inPreview();
    });
    MCGuiStep("the keyboard in it", [] { MCGuiClick(MCGuiApp().editor->textView()); });
    MCGuiWait("the editor has the keyboard", 5, editorHasKeyboard);
    MCGuiStep("Ctrl+Shift+V does nothing now", [] { MCGuiKey("ctrl+shift+v"); });
    MCGuiPause(0.3);   // a negative check
    MCGuiStep("still rendered; Ctrl+Alt+J toggles it", [] {
        GCHECK(MCGuiApp().editor->inPreview());
        MCGuiKey("ctrl+alt+j");
    });
    MCGuiWait("the source shows", 10, [] { return !MCGuiApp().editor->inPreview(); });
    MCGuiStep("the hints show the new key", [] { MCGuiKey("ctrl+shift+h"); });
    MCGuiWait("they are up", 10, [] { return gtk_widget_get_visible(MCGuiApp().hintsPanel); });
    MCGuiStep("with it, and the moved keys", [] {
        const std::string hints = gtk_label_get_text(GTK_LABEL(MCGuiApp().hintsLabel));
        // The row naming a thing, as its key column reads.
        auto keyOf = [&hints](const std::string& what) {
            const size_t at = hints.find(what);
            if (at == std::string::npos) return std::string("(no row for " + what + ")");
            const size_t start = hints.rfind('\n', at) + 1;
            std::string key = hints.substr(start, at - start);
            while (!key.empty() && key.back() == ' ') key.pop_back();
            return key;
        };
        GCHECK_EQ_STR(keyOf("Markdown    (source)"), "Ctrl Alt J");
        GCHECK_EQ_STR(keyOf("TODOs in folder"), "Ctrl Shift D");
        GCHECK_EQ_STR(keyOf("Toggle task\n"), "Ctrl Alt L");
        GCHECK_EQ_STR(keyOf("Toggle task, too"), "Alt C");
        GCHECK_EQ_STR(keyOf("New file"), "Ctrl N");
        GCHECK_EQ_STR(keyOf("New window"), "Ctrl Shift N");
        GCHECK_EQ_STR(keyOf("New folder"), "Ctrl Alt N");
        GCHECK_EQ_STR(keyOf("Close window"), "Ctrl Shift W");
        GCHECK_EQ_STR(keyOf("Hover info"), "Ctrl Alt I");
        GCHECK_EQ_STR(keyOf("Find next"), "F3");
        GCHECK_EQ_STR(keyOf("Hidden files ("), "Ctrl Shift .");
        GCHECK_EQ_STR(keyOf("Rename (in the tree)"), "Ctrl Alt R");
        GCHECK(hints.find("Ctrl Shift L") == std::string::npos);
        GCHECK(hints.find("Ctrl Shift P") == std::string::npos);
        MCGuiKey("ctrl+shift+h");
    });
    MCGuiWait("they go", 10, [] { return !gtk_widget_get_visible(MCGuiApp().hintsPanel); });
    MCGuiStep("a row of the tree, with the keyboard", [] {
        MCGuiClick(MCGuiFindLabel(appWindow(), "keys.txt"));
    });
    MCGuiWait("the tree has the keyboard", 10, [] {
        return Keys::within(focusWidget(), MCGuiApp().tree->widget()) &&
               MCGuiApp().editor->currentPath() == keysTxt();
    });
    MCGuiStep("F2 renames nothing now", [] { MCGuiKey("f2"); });
    MCGuiPause(0.3);   // a negative check
    MCGuiStep("no question; Ctrl+Alt+R asks", [] {
        GCHECK(!labelShown("Rename to"));
        MCGuiKey("ctrl+alt+r");
    });
    MCGuiWait("it asks", 10, [] { return labelShown("Rename to"); });
    MCGuiStep("put away", [] { MCGuiKey("escape"); });
    MCGuiWait("the question goes", 10, [] { return !labelShown("Rename to"); });

    MCGuiStep("an empty keys.conf puts the defaults back", [] { MCGuiSetKeys(""); });
    MCGuiWait("the accelerators follow", 10, [] {
        return bound("win.togglepreview") == canon("<Ctrl><Shift>v");
    });
    MCGuiStep("the note goes", [] {
        GCHECK_EQ_STR(gtk_label_get_text(GTK_LABEL(MCGuiApp().settingsLabel)), "");
        MCGuiClick(MCGuiFindLabel(appWindow(), "keys.txt"));
    });
    MCGuiWait("the tree has the keyboard", 10, [] {
        return Keys::within(focusWidget(), MCGuiApp().tree->widget());
    });
    MCGuiStep("F2 renames again", [] { MCGuiKey("f2"); });
    MCGuiWait("it asks", 10, [] { return labelShown("Rename to"); });
    MCGuiStep("put away", [] {
        MCGuiKey("escape");
        unwatchActions();
    });
    MCGuiWait("the question goes", 10, [] { return !labelShown("Rename to"); });
}
