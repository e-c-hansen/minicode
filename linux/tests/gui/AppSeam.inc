// AppSeam.inc — see AppSeam.h. Included at the end of src/main.cpp when it is
// compiled for mutible-guitest, so it can see App and the shared state `g`.
#include "AppSeam.h"

GtkApplication* MCGuiApplication() { return g.gapp; }
GMenu* MCGuiBookmarksMenu() { return g.bookmarksMenu; }

static MCGuiAppWindow appWindowParts(App* a) {
    MCGuiAppWindow w;
    w.window = GTK_WINDOW(a->window);
    w.editor = a->editor;
    w.tree = a->tree;
    w.git = a->git;
    w.procs = a->procs;
    w.procsShown = a->procsShown;
    w.ide = a->ide;
    w.agents = a->agents;
    w.agentsShown = a->agentsShown;
    w.worktrees = a->worktrees;
    w.lsp = a->lsp;
    w.layout = a->layout;
    w.statusLabel = a->statusLabel;
    w.settingsLabel = a->settingsLabel;
    w.statusBar = a->statusBar;
    w.findBar = a->findBar;
    w.search = a->search;
    w.hintsPanel = a->hintsPanel;
    w.hintsColumns = a->hintsColumns;
    w.hintsHint = a->hintsHint;
    w.hintsSearch = a->hintsSearch ? a->hintsSearch->widget() : nullptr;
    w.palette = a->palette;
    w.nav = a->nav;
    w.consentBar = a->consentBar;
    w.format = a->format;
    w.formatOffer = a->formatOffer;
    w.problems = a->problems;
    w.changeBars = a->gitGutter;
    w.tests = a->testGutter;
    w.treeColors = a->treeColors;
    w.sidebarIcons = a->sidebarIcons;
    w.sidebarIconsRow = a->sidebarIconsRow;
    w.sidebarStack = a->sidebar;
    w.gitShown = a->gitShown;
    w.sessionBrowser = a->sessionBrowser;
    w.rootDir = a->rootDir;
#ifdef MUTIBLE_ENABLE_TERMINAL
    w.terminal = a->terminal;
    w.termDock = a->termDock;
    w.controlId = a->controlId;
#endif
#ifdef MUTIBLE_ENABLE_BROWSER
    w.browser = a->browser;
#endif
    return w;
}

std::vector<MCGuiAppWindow> MCGuiAppWindows() {
    std::vector<MCGuiAppWindow> out;
    for (App* a : g.windows) out.push_back(appWindowParts(a));
    return out;
}

MCGuiAppWindow MCGuiApp() {
    return g.windows.empty() ? MCGuiAppWindow() : appWindowParts(g.windows.back());
}

void MCGuiOpenFile(const std::string& path) {
    if (!g.windows.empty()) treeOpenCb(path, EditorTabs::OpenFrom::TreeClick, g.windows.back());
}

void MCGuiOpenFileKept(const std::string& path) {
    if (!g.windows.empty()) openFileCb(path, g.windows.back());
}

void MCGuiReloadSettings() {
    if (g.settings) g.settings->reload();
}

void MCGuiReloadKeys() { Keys::reload(); }

void MCGuiCloseAllWindows() {
    const std::vector<App*> open = g.windows;   // closing edits the list
    for (App* a : open) {
        a->closing = true;   // no "Save changes?"
        gtk_window_close(GTK_WINDOW(a->window));
    }
}

void MCGuiOpenWindow(const std::string& folder) { newWindow(folder, ""); }

void MCGuiCommandLine(const std::vector<std::string>& args, const std::string& cwd,
                      std::string* errors) {
    std::vector<std::string> all = {"mutible"};
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char*> ptrs;
    for (std::string& a : all) ptrs.push_back(&a[0]);
    handleCommandLine(ptrs.data(), (int)ptrs.size(), cwd.c_str(), [errors](const std::string& m) {
        if (errors) *errors += m + "\n";
    });
}

GMenu* MCGuiRecentMenu() { return g.recentMenu; }

std::string MCGuiWindowRecord(GtkWindow* window) {
    for (App* a : g.windows)
        if (!a->dead && GTK_WINDOW(a->window) == window && a->persist)
            return TabSessions::encodeWindows({a->persist->windowRecord()});
    return "";
}

void MCGuiOpenWindowRestoring(const std::string& folder, const std::string& record) {
    const std::vector<TabSessions::Window> windows = TabSessions::decodeWindows(record);
    if (!windows.empty()) newWindow(folder, "", &windows[0]);
}

void MCGuiAnswerFolderChooser(std::function<std::string(const std::string& start)> answer) {
    if (!answer) {
        gFolderChooserHook = nullptr;
        return;
    }
    gFolderChooserHook = [answer](GtkWindow*, const std::string& start,
                                  std::function<void(const std::string&)> done) {
        const std::string chosen = answer(start);
        // Later, as the portal's answer is.
        g_idle_add_once([](gpointer p) {
            auto* f = static_cast<std::function<void()>*>(p);
            (*f)();
            delete f;
        }, new std::function<void()>([done, chosen] { done(chosen); }));
    };
}
