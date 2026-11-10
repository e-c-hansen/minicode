// Keys.cpp — see Keys.h.
#include "Keys.h"

#include <glib/gstdio.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

using Commands::Command;
using Commands::Key;
using Commands::Platform;
using Commands::When;

namespace Keys {

// The port's actions, in the registry's order. main.cpp's addAction names
// the win. ones, initShared the app. ones.
const std::vector<Action>& actions() {
    static const std::vector<Action> table = {
        {"workbench.action.openSettings", "win.settings", true},
        {"workbench.action.openGlobalKeybindings", "win.keyboardshortcuts", false},
        {"minicode.forgetFolderToolsAnswer", "win.forgetfoldertools", false},
        {"workbench.action.quit", "app.quit", false},
        {"explorer.newFile", "win.newfile", false},
        {"workbench.action.newWindow", "win.newwindow", false},
        {"workbench.action.files.openFolder", "win.open", false},
        {"workbench.action.files.save", "win.save", true},
        {"workbench.action.files.saveAll", "win.saveall", false},
        {"minicode.latex.exportPDF", "win.exportpdf", false},
        {"explorer.newFolder", "win.newfolder", true},
        {"renameFile", "win.rename", false},
        {"moveFileToTrash", "win.trash", false},
        {"revealFileInOS", "win.reveal", true},
        {"copyFilePath", "win.copypath", false},
        {"workbench.action.files.copyPathOfActiveFile", "win.copyactivepath", true},
        {"workbench.action.files.revealActiveFileInWindows", "win.revealactive", false},
        {"workbench.files.action.showActiveFileInExplorer", "win.revealactiveintree", false},
        {"workbench.action.closeActiveEditor", "win.closetab", true},
        {"workbench.action.reopenClosedEditor", "win.reopentab", false},
        {"workbench.action.closeOtherEditors", "win.closeothertabs", false},
        {"workbench.action.closeEditorsToTheRight", "win.closetabsright", false},
        {"workbench.action.closeAllEditors", "win.closealltabs", false},
        {"workbench.action.keepEditor", "win.keeptab", false},
        {"workbench.action.closeWindow", "win.close", true},
        {"editor.action.addSelectionToNextFindMatch", "win.addnextoccurrence", false},
        {"editor.action.selectHighlights", "win.selectalloccurrences", false},
        {"editor.action.insertCursorAbove", "win.addcursorabove", false},
        {"editor.action.insertCursorBelow", "win.addcursorbelow", false},
        {"expandLineSelection", "win.selectline", false},
        {"editor.action.deleteLines", "win.deleteline", false},
        {"editor.action.commentLine", "win.togglecomment", true},
        {"minicode.markdown.toggleTask", "win.toggletask", false},
        {"editor.action.formatDocument", "win.formatdocument", false},
        {"minicode.sortLines", "win.sortlines", false},
        {"minicode.sortKeepSortedBlocks", "win.fixkeepsorted", false},
        {"editor.action.triggerSuggest", "win.complete", true},
        {"editor.action.showHover", "win.hoverinfo", false},
        {"editor.action.rename", "win.renamesymbol", false},
        {"editor.action.quickFix", "win.quickfix", false},
        {"actions.find", "win.find", true},
        {"editor.action.startFindReplaceAction", "win.replace", false},
        {"editor.action.nextMatchFindAction", "win.findnext", false},
        {"editor.action.previousMatchFindAction", "win.findprevious", false},
        {"editor.action.replaceOne", "win.replaceone", false},
        {"editor.action.replaceAll", "win.replaceall", false},
        {"toggleFindCaseSensitive", "win.findcase", false},
        {"toggleFindWholeWord", "win.findword", false},
        {"toggleFindRegex", "win.findregex", false},
        {"togglePreserveCase", "win.findpreservecase", false},
        {"toggleFindInSelection", "win.findinselection", false},
        {"workbench.action.findInFiles", "win.findinfolder", true},
        {"workbench.action.replaceInFiles", "win.replaceinfolder", false},
        {"minicode.showTodos", "win.todos", false},
        {"workbench.action.showCommands", "win.showcommands", false},
        {"markdown.showPreview", "win.togglepreview", true},
        {"minicode.toggleShortcutHints", "win.togglehints", false},
        {"editor.action.fontZoomIn", "win.editorzoomin", true},
        {"editor.action.fontZoomOut", "win.editorzoomout", false},
        {"editor.action.fontZoomReset", "win.editorzoomreset", false},
        {"minicode.page.zoomIn", "win.zoomin", true},
        {"minicode.page.zoomOut", "win.zoomout", false},
        {"minicode.page.fitWidth", "win.zoomfit", false},
        {"editor.action.toggleWordWrap", "win.togglewordwrap", false},
        // View > Folding (Folds.h adds the actions).
        {"editor.fold", "win.fold", true},
        {"editor.unfold", "win.unfold", false},
        {"editor.toggleFold", "win.togglefold", false},
        {"editor.foldRecursively", "win.foldrecursively", false},
        {"editor.unfoldRecursively", "win.unfoldrecursively", false},
        {"editor.foldAll", "win.foldall", false},
        {"editor.unfoldAll", "win.unfoldall", false},
        {"editor.foldAllBlockComments", "win.foldcomments", false},
        {"editor.foldLevel1", "win.foldlevel1", false},
        {"editor.foldLevel2", "win.foldlevel2", false},
        {"editor.foldLevel3", "win.foldlevel3", false},
        {"editor.foldLevel4", "win.foldlevel4", false},
        {"editor.foldLevel5", "win.foldlevel5", false},
        {"editor.foldLevel6", "win.foldlevel6", false},
        {"editor.foldLevel7", "win.foldlevel7", false},
        {"workbench.action.toggleSidebarVisibility", "win.togglesidebar", true},
        {"workbench.view.scm", "win.togglegit", false},
        {"minicode.toggleEditor", "win.toggleeditor", false},
        {"minicode.toggleMaximizedPane", "win.togglemaximize", false},
        {"minicode.toggleHiddenFiles", "app.showhidden", false},
        {"workbench.action.terminal.toggleTerminal", "win.toggleterminal", false},
        {"minicode.toggleBrowser", "win.togglebrowser", false},
        {"minicode.togglePlayback", "win.playpause", true},
        {"workbench.action.terminal.fontZoomIn", "win.terminalzoomin", false},
        {"workbench.action.terminal.fontZoomOut", "win.terminalzoomout", false},
        {"workbench.action.terminal.fontZoomReset", "win.terminalzoomreset", false},
        {"workbench.action.quickOpen", "win.quickopen", false},
        {"workbench.action.gotoLine", "win.gotoline", false},
        {"workbench.action.gotoSymbol", "win.gotosymbol", false},
        {"workbench.action.navigateBack", "win.goback", true},
        {"workbench.action.navigateForward", "win.goforward", false},
        {"editor.action.jumpToBracket", "win.gotobracket", false},
        {"workbench.action.focusSideBar", "win.focustree", true},
        {"workbench.action.focusFirstEditorGroup", "win.focuseditor", false},
        {"workbench.action.nextEditor", "win.nexttab", true},
        {"workbench.action.previousEditor", "win.previoustab", false},
        {"workbench.action.openEditorAtIndex1", "win.tab1", false},
        {"workbench.action.openEditorAtIndex2", "win.tab2", false},
        {"workbench.action.openEditorAtIndex3", "win.tab3", false},
        {"workbench.action.openEditorAtIndex4", "win.tab4", false},
        {"workbench.action.openEditorAtIndex5", "win.tab5", false},
        {"workbench.action.openEditorAtIndex6", "win.tab6", false},
        {"workbench.action.openEditorAtIndex7", "win.tab7", false},
        {"workbench.action.openEditorAtIndex8", "win.tab8", false},
        {"workbench.action.lastEditorInGroup", "win.lasttab", false},
        {"editor.action.revealDefinition", "win.definition", true},
        {"editor.action.goToReferences", "win.references", false},
        {"editor.action.moveLinesDownAction", "win.movelinesdown", true},
        {"editor.action.moveLinesUpAction", "win.movelinesup", false},
        {"editor.action.copyLinesDownAction", "win.copylinesdown", false},
        {"editor.action.copyLinesUpAction", "win.copylinesup", false},
    };
    return table;
}

const Action* actionFor(const std::string& commandId) {
    for (const Action& a : actions())
        if (commandId == a.id) return &a;
    return nullptr;
}

std::vector<std::string> tableProblems() {
    std::vector<std::string> out;
    for (const Commands::LinuxRow& r : Commands::linuxRows()) {
        // A view's own keys (the registry's Fixed: Ctrl+Home in the editor)
        // need none, and are no accelerator.
        const Command* c = Commands::find(r.id);
        if (c && (c->flags & Commands::Fixed) && !actionFor(r.id)) continue;
        if (!actionFor(r.id))
            out.push_back(std::string(r.id) + " has Linux keys but no action in Keys.cpp");
    }
    for (const Action& a : actions()) {
        if (!Commands::find(a.id)) out.push_back(std::string(a.id) + " is no command");
        else if (!Commands::linuxRow(a.id))
            out.push_back(std::string(a.id) + " (" + a.action + ") has no row in CommandsLinux.cpp");
    }
    return out;
}

// ------------------------------------------------------------ the file
namespace {

struct State {
    GtkApplication* app = nullptr;
    std::function<void()> changed;
    Commands::Keymap map = Commands::defaults(Platform::Linux);
    std::string path, text;
    bool loaded = false;
    std::vector<std::string> errors;
    GFileMonitor* monitor = nullptr;
    Place place;
    bool applied = false;
    std::vector<GtkShortcutController*> trees;   // addTreeKeys's, while they live
};
State& S() {
    static State s;
    return s;
}

bool load() {
    State& s = S();
    std::string text;
    gchar* contents = nullptr;
    gsize len = 0;
    if (g_file_get_contents(s.path.c_str(), &contents, &len, nullptr)) {
        text.assign(contents, len);
        g_free(contents);
    }
    if (s.loaded && text == s.text) return false;
    s.loaded = true;
    s.text = text;
    std::vector<Commands::Error> errs;
    s.map = Commands::resolve(text, &errs, Platform::Linux);
    s.errors.clear();
    for (const Commands::Error& e : errs) {
        s.errors.push_back("line " + std::to_string(e.line) + ": " + e.message);
        std::fprintf(stderr, "MiniCode keys.conf %s\n", s.errors.back().c_str());
    }
    return true;
}

void applyAccels();
void fillTree(GtkShortcutController* ctrl);

void onChanged(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent ev, gpointer) {
    switch (ev) {
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_RENAMED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
    case G_FILE_MONITOR_EVENT_CHANGED:
        reload();
        break;
    default:
        break;
    }
}

// As AppSettings watches settings.conf: GLib watches the folder underneath,
// so an editor's save by rename, and the file appearing, are seen too.
void watch() {
    State& s = S();
    if (s.monitor) return;
    GFile* file = g_file_new_for_path(s.path.c_str());
    s.monitor = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    g_object_unref(file);
    if (!s.monitor) return;
    g_file_monitor_set_rate_limit(s.monitor, 100);
    g_signal_connect(s.monitor, "changed", G_CALLBACK(onChanged), nullptr);
}

}  // namespace

void init(GtkApplication* app, std::function<void()> changed) {
    State& s = S();
    s.app = app;
    s.changed = std::move(changed);
    const char* env = g_getenv("MINICODE_KEYS");
    if (env && *env) {
        s.path = env;
    } else {
        gchar* p = g_build_filename(g_get_user_config_dir(), "minicode", "keys.conf", nullptr);
        s.path = p;
        g_free(p);
    }
    for (const std::string& problem : tableProblems())
        g_warning("minicode keys: %s", problem.c_str());
    load();
    watch();
    applyAccels();
}

const Commands::Keymap& keymap() { return S().map; }
const std::string& path() { return S().path; }
const std::vector<std::string>& errors() { return S().errors; }

void reload() {
    State& s = S();
    if (!load()) return;
    applyAccels();
    for (GtkShortcutController* c : s.trees) fillTree(c);
    if (s.changed) s.changed();
}

bool ensureFileExists() {
    State& s = S();
    if (g_file_test(s.path.c_str(), G_FILE_TEST_EXISTS)) return true;
    gchar* dir = g_path_get_dirname(s.path.c_str());
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    const std::string body = Commands::defaultFileText(Platform::Linux);
    if (!g_file_set_contents(s.path.c_str(), body.c_str(), (gssize)body.size(), nullptr))
        return false;
    // Watching a path in a folder that did not exist yet may have failed.
    if (!s.monitor) watch();
    reload();
    return true;
}

// ------------------------------------------------------------ accelerators
namespace {

// Does `k`, a key of `c`'s, run it where the keyboard is now?
bool bound(const Command& c, const Key& k) {
    const State& s = S();
    if (s.place.terminal) {
        if (!Commands::appliesIn(c.when, When::Terminal)) return false;
        return !Commands::linuxTerminalTakes(c, k);
    }
    if (c.when == When::Terminal) return false;
    // A page's zoom only in a page, and the editor's zoom (anywhere but a
    // page or the terminal) only outside one: they share their keys. Focus
    // File Tree, the Mac's NotPage, keeps Ctrl+0 in a page, where Linux has
    // no Actual Size to give it.
    if (c.when == When::Page && !s.place.page) return false;
    if (c.when == When::NotPageOrTerminal && s.place.page) return false;
    // While the tree has the keyboard its own keys are its controller's.
    if (s.place.tree) {
        const Command* own = s.map.commandFor(k, When::Tree);
        if (own && own != &c && own->when == When::Tree && actionFor(own->id)) return false;
    }
    if (c.language) {
        const bool alone = s.map.commandsFor(k).size() == 1;
        if ((k.mods & Commands::Ctrl) && alone) return true;
        return s.place.editor && s.place.language == c.language;
    }
    return true;
}

// Which of two commands wanting one accelerator gets it: the one for the
// language on show, then the one for the place, then the earlier.
int rank(const Command& c) {
    const State& s = S();
    if (c.language && s.place.editor && s.place.language == c.language) return 0;
    if ((s.place.editor && c.when == When::Editor) ||
        (s.place.terminal && c.when == When::Terminal) || (s.place.page && c.when == When::Page))
        return 1;
    return 2;
}

void applyAccels() {
    State& s = S();
    if (!s.app) return;
    s.applied = true;
    std::vector<std::pair<const Action*, const Command*>> order;
    for (const Action& a : actions())
        if (const Command* c = Commands::find(a.id)) order.push_back({&a, c});
    std::stable_sort(order.begin(), order.end(),
                     [](const auto& x, const auto& y) { return rank(*x.second) < rank(*y.second); });
    std::set<std::string> taken;
    for (const auto& [a, c] : order) {
        std::vector<std::string> accels;
        if (c->when != When::Tree)
            for (const Key& k : s.map.keysOf(c->id)) {
                if (!bound(*c, k)) continue;
                for (const std::string& accel : Commands::linuxAccelerators(k))
                    if (taken.insert(accel).second) accels.push_back(accel);
            }
        std::vector<const char*> list;
        for (const std::string& accel : accels) list.push_back(accel.c_str());
        list.push_back(nullptr);
        gtk_application_set_accels_for_action(s.app, a->action, list.data());
    }
}

void fillTree(GtkShortcutController* ctrl) {
    // Take the old ones off, last first.
    GListModel* model = G_LIST_MODEL(ctrl);
    for (guint i = g_list_model_get_n_items(model); i > 0; i--) {
        GtkShortcut* old = GTK_SHORTCUT(g_list_model_get_item(model, i - 1));
        gtk_shortcut_controller_remove_shortcut(ctrl, old);
        g_object_unref(old);
    }
    for (const Action& a : actions()) {
        const Command* c = Commands::find(a.id);
        if (!c || c->when != When::Tree) continue;
        for (const Key& k : S().map.keysOf(c->id))
            for (const std::string& accel : Commands::linuxAccelerators(k)) {
                GtkShortcutTrigger* trigger = gtk_shortcut_trigger_parse_string(accel.c_str());
                if (!trigger) continue;
                gtk_shortcut_controller_add_shortcut(
                    ctrl, gtk_shortcut_new(trigger, gtk_named_action_new(a.action)));
            }
    }
}

void forgetTree(gpointer, GObject* gone) {
    auto& trees = S().trees;
    trees.erase(std::remove(trees.begin(), trees.end(), (GtkShortcutController*)gone), trees.end());
}

}  // namespace

void setPlace(const Place& place) {
    State& s = S();
    if (s.applied && place == s.place) return;
    s.place = place;
    applyAccels();
}

bool within(GtkWidget* focus, GtkWidget* area) {
    for (GtkWidget* w = focus; w; w = gtk_widget_get_parent(w)) {
        if (w == area) return true;
        if (GTK_IS_POPOVER(w)) return false;
    }
    return false;
}

void addTreeKeys(GtkWidget* widget) {
    GtkEventController* ctrl = gtk_shortcut_controller_new();
    fillTree(GTK_SHORTCUT_CONTROLLER(ctrl));
    S().trees.push_back(GTK_SHORTCUT_CONTROLLER(ctrl));
    g_object_weak_ref(G_OBJECT(ctrl), forgetTree, nullptr);
    gtk_widget_add_controller(widget, ctrl);
}

// ------------------------------------------------------------ menus
namespace {

struct Item {
    const Command* command;
    const Action* action;
};

GMenuItem* menuItem(const Item& it) {
    return g_menu_item_new(Commands::titleOf(*it.command, Platform::Linux), it.action->action);
}

std::string topOf(const std::string& menu) {
    const size_t gt = menu.find(" > ");
    return gt == std::string::npos ? menu : menu.substr(0, gt);
}

std::string subOf(const std::string& menu) {
    const size_t gt = menu.find(" > ");
    return gt == std::string::npos ? std::string() : menu.substr(gt + 3);
}

// One top-level menu: its own commands in the registry's order, then those
// Linux moves into it; sections where the table says; a submenu ("View >
// Page Zoom") where its first command falls.
GMenu* buildMenu(const std::string& top) {
    std::vector<Item> own, moved;
    for (const Command& c : Commands::all()) {
        const Action* a = actionFor(c.id);
        if (!a || !Commands::available(c, Platform::Linux)) continue;
        if (topOf(Commands::menuOf(c, Platform::Linux)) != top) continue;
        const Commands::LinuxRow* row = Commands::linuxRow(c.id);
        (row && row->menu ? moved : own).push_back({&c, a});
    }
    std::vector<Item> items = own;
    items.insert(items.end(), moved.begin(), moved.end());
    std::vector<std::vector<Item>> sections;
    for (const Item& it : items) {
        if (sections.empty() || (it.action->section && !sections.back().empty()))
            sections.emplace_back();
        sections.back().push_back(it);
    }
    GMenu* menu = g_menu_new();
    for (const std::vector<Item>& part : sections) {
        GMenu* section = g_menu_new();
        std::set<std::string> subsDone;
        for (const Item& it : part) {
            const std::string sub = subOf(Commands::menuOf(*it.command, Platform::Linux));
            if (sub.empty()) {
                GMenuItem* mi = menuItem(it);
                g_menu_append_item(section, mi);
                g_object_unref(mi);
                continue;
            }
            if (!subsDone.insert(sub).second) continue;
            GMenu* submenu = g_menu_new();
            for (const Item& other : part)
                if (subOf(Commands::menuOf(*other.command, Platform::Linux)) == sub) {
                    GMenuItem* mi = menuItem(other);
                    g_menu_append_item(submenu, mi);
                    g_object_unref(mi);
                }
            g_menu_append_submenu(section, sub.c_str(), G_MENU_MODEL(submenu));
            g_object_unref(submenu);
        }
        g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
        g_object_unref(section);
    }
    return menu;
}

}  // namespace

GMenuModel* menuBar() {
    GMenu* bar = g_menu_new();
    for (const char* top : {"File", "Edit", "View", "Terminal", "Navigate"}) {
        GMenu* menu = buildMenu(top);
        if (g_menu_model_get_n_items(G_MENU_MODEL(menu)) > 0)
            g_menu_append_submenu(bar, top, G_MENU_MODEL(menu));
        g_object_unref(menu);
    }
    return G_MENU_MODEL(bar);
}

GMenuModel* menuOf(const std::vector<std::string>& ids) {
    GMenu* menu = g_menu_new();
    GMenu* section = nullptr;
    auto close = [&] {
        if (!section) return;
        g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
        g_object_unref(section);
        section = nullptr;
    };
    for (const std::string& id : ids) {
        if (id.empty()) {
            close();
            continue;
        }
        const Command* c = Commands::find(id);
        const Action* a = actionFor(id);
        if (!c || !a) continue;
        if (!section) section = g_menu_new();
        GMenuItem* mi = menuItem({c, a});
        g_menu_append_item(section, mi);
        g_object_unref(mi);
    }
    close();
    return G_MENU_MODEL(menu);
}

// ------------------------------------------------------------ labels
std::string label(const std::string& id, const char* between, size_t index) {
    const std::vector<Key>& keys = S().map.keysOf(id);
    return index < keys.size() ? Commands::linuxLabel(keys[index], between) : std::string();
}

std::string labels(const std::string& id) {
    std::string out;
    for (const Key& k : S().map.keysOf(id)) {
        if (!out.empty()) out += ", ";
        out += Commands::linuxLabel(k);
    }
    return out;
}

}  // namespace Keys
