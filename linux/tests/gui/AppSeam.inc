// AppSeam.inc — see AppSeam.h. Included at the end of src/main.cpp when it is
// compiled for minicode-guitest, so it can see App and the shared state `g`.
#include "AppSeam.h"

GtkApplication* MCGuiApplication() { return g.gapp; }

static MCGuiAppWindow appWindowParts(App* a) {
    MCGuiAppWindow w;
    w.window = GTK_WINDOW(a->window);
    w.editor = a->editor;
    w.tree = a->tree;
    w.git = a->git;
    w.lsp = a->lsp;
    w.layout = a->layout;
    w.statusLabel = a->statusLabel;
    w.settingsLabel = a->settingsLabel;
    w.statusBar = a->statusBar;
    w.findBar = a->findBar;
    w.search = a->search;
    w.hintsPanel = a->hintsPanel;
    w.hintsLabel = a->hintsLabel;
    w.hintsHint = a->hintsHint;
    w.palette = a->palette;
    w.nav = a->nav;
    w.consentBar = a->consentBar;
    w.format = a->format;
    w.problems = a->problems;
    w.rootDir = a->rootDir;
#ifdef MINICODE_ENABLE_TERMINAL
    w.terminal = a->terminal;
#endif
#ifdef MINICODE_ENABLE_BROWSER
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
