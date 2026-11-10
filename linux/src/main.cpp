// main.cpp — MiniCode GTK4 shell.
//
// Layout (mirrors the macOS window: file tree left, editor right, status bar
// at the bottom, collapsible terminal and browser panels):
//
//   GtkApplicationWindow
//     vbox
//       overlay  (the shortcut list floats over the panes)
//         hpaned  (movable divider)
//           [start] sidebar stack: FileTree, or the Source Control panel
//                   (GitPanel) in its place
//           [end]   right vbox
//                     vpaned  (movable divider)
//                       [start] upper vbox
//                                 Editor (GtkTextView, vexpand), the find
//                                   bar (FindBar) over its text
//                                 browser revealer  (#ifdef MINICODE_ENABLE_BROWSER)
//                       [end]   Terminal panel      (#ifdef MINICODE_ENABLE_TERMINAL)
//       status bar (GtkLabel, VS Code blue)
//
// The editor and the browser share the upper area — showing the browser hides
// the editor — and the terminal is docked below both. That mirrors the macOS
// -relayoutRightArea, which is why the browser is appended before the terminal.
//
// The terminal hangs off a GtkPaned rather than sitting in the box, so its top
// edge can be dragged the way the macOS build's can. Toggling it visible and
// invisible is what opens and closes the panel: GtkPaned draws no handle while
// one of its children is hidden, and the place the user dragged the divider to
// is remembered in App::termSplit so reopening restores it. Hiding the upper
// box the same way is how the terminal gets the whole window (win.toggleeditor);
// showEditorArea is the single place that brings it back.
//
// Both dividers can be dragged all the way: the terminal's to the top of the
// window, which snaps the editor shut, and the sidebar's to the right edge,
// which collapses the right side and leaves the file tree. Anything that needs
// to show something there opens it again (showEditorArea, showRightArea).
//
// Colors come from the settings file (AppSettings, shared Settings parser):
// ThemeCss turns them into the stylesheet, and the editor tags and VTE colors
// are set directly. The file is watched, so edits apply live.
//
// The always-built target is the tree + editor + markdown viewer. Terminal and
// Browser only exist when their libraries were found at configure time.
//
// Windows. Everything above belongs to one window, an App, and there can be
// several (New Window, or a second `minicode <path>` from a shell): each has its
// own tree, editor, terminal, browser, Find in Folder panel and language
// server session, as each macOS window has its own EditorController. What
// they share lives in `g` below: the settings file and stylesheet, the menu
// bar, the accelerators, and whether dotfiles are shown. The application
// quits when its last window closes, which is GtkApplication's own rule and
// the Mac's applicationShouldTerminateAfterLastWindowClosed. A closing
// window is taken apart in onWindowRemoved while its widgets still exist.
#include <gtk/gtk.h>

#include "AppSettings.h"
#include "Document.h"
#include "Editor.h"
#include "FileTree.h"
#include "ThemeCss.h"
#include "Palette.h"
#include "Terminal.h"
#include "Browser.h"
#include "Search.h"
#include "FindBar.h"
#include "Lsp.h"
#include "GitPanel.h"
#include "Keys.h"
#include "BackupStore.h"
#include "Persistence.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

// What the browser pane opens with. The GUI checks build this file with
// about:blank, so that a check never reaches the network.
#ifndef MINICODE_BROWSER_HOME
#define MINICODE_BROWSER_HOME "https://duckduckgo.com"
#endif

struct App {
    GtkWidget*      window = nullptr;
    GtkWidget*      hpaned = nullptr;
    GtkWidget*      statusLabel = nullptr;
    GtkWidget*      settingsLabel = nullptr;   // first problem in the settings file

    int  sidebarWidth = 240;      // where the sidebar divider was before collapsing
    bool adjustingPaned = false;  // a snap is moving a divider; ignore the notify

    Editor*   editor = nullptr;
    FindBar*  findBar = nullptr;     // the editor's find and replace bar
    FileTree* tree = nullptr;
    // The sidebar is a stack: the file tree, or the Source Control panel in
    // its place, made on first use.
    GtkWidget* sidebar = nullptr;
    GitPanel*  git = nullptr;
    bool       gitShown = false;
    SearchPanel* search = nullptr;   // Find in Folder, created on first use
    SearchPanel* todos = nullptr;    // the TODOs window, the same
    LspSession*  lsp = nullptr;      // language servers for this window
    // Its tabs between runs, the backups of its unsaved tabs, and auto-save.
    Persistence* persist = nullptr;
    GtkWidget*   lspLabel = nullptr; // their status, in the status bar

    GtkWidget* vpaned = nullptr;          // editor/browser above, terminal below
    GtkWidget* upperBox = nullptr;        // the editor/browser half of the vpaned
    GtkWidget* termPanel = nullptr;       // the terminal's own widget, or null
    GtkWidget* browserRevealer = nullptr;
#ifdef MINICODE_ENABLE_TERMINAL
    Terminal* terminal = nullptr;
    // Where the terminal divider sits, as a distance from the bottom of the
    // paned. Survives closing and reopening the panel. See placeTerminalDivider.
    int termSplit = Terminal::kDefaultHeight;
#endif
#ifdef MINICODE_ENABLE_BROWSER
    Browser* browser = nullptr;
#endif

    // The floating shortcut list and the status-bar label that advertises it.
    GtkWidget* hintsPanel = nullptr;
    GtkWidget* hintsLabel = nullptr;
    GtkWidget* hintsScroll = nullptr;
    GtkWidget* hintsHint  = nullptr;

    std::string rootDir;
    bool sidebarVisible = true;

    bool prompting = false;   // a "Save changes?" alert is up
    bool closing = false;     // the user answered it for a window close

    // The window is gone and everything in it deleted. The struct itself is
    // kept, a few hundred bytes, so an answer arriving later from a dialog
    // that was up can see that and do nothing.
    bool dead = false;
};

namespace {
// What every window shares.
struct Shared {
    GtkApplication* gapp = nullptr;
    AppSettings*    settings = nullptr;   // one file, watched once
    GtkCssProvider* css = nullptr;        // one stylesheet for the display
    GMenuModel*     treeMenu = nullptr;   // the file tree's right-click menu
    std::vector<App*> windows;            // open windows, oldest first
    // Dotfiles in the tree: the application's setting, as the Mac's
    // gShowHidden is one flag for every window.
    bool showHidden = false;
    // A quit is under way: every window's session was saved before the
    // windows began to close, so a window closing saves none of its own.
    bool quitting = false;
};
Shared g;
}  // namespace

static App* newWindow(const std::string& root, const std::string& file);

// Defined with the rest of the hints panel further down; the pane toggles call
// it so an open panel never reports stale state.
static void refreshHints(App* app);
// Tells Keys what has the keyboard (see "keys" below).
static void updateKeyPlace();

// A divider dragged this close to the top (terminal) or the right edge
// (sidebar) snaps the rest of the way.
static const int kTopSnap = 40;
static const int kRightSnap = 60;

// The status bar's note on the shortcut hints, with their key as keys.conf
// leaves it.
static std::string hintsHintText(bool showing) {
    const std::string key = Keys::label("minicode.toggleShortcutHints", "+");
    return (key.empty() ? std::string() : key + "  ") +
           (showing ? "Hide shortcuts" : "Shortcuts");
}

// ------------------------------------------------- the editor/terminal split
#ifdef MINICODE_ENABLE_TERMINAL
// Put the divider back where the user left it. gtk_paned_set_position measures
// from the TOP, so the saved distance has to be turned around against the
// paned's current height, which also keeps the panel proportionate when the
// window has been resized in the meantime.
static void placeTerminalDivider(App* app) {
    const int H = gtk_widget_get_height(app->vpaned);
    if (H <= 0) return;   // not laid out yet; GTK's own default is fine

    // The same clamp the macOS drag handler applies (EditorController.mm): at
    // least 80px of terminal, and never less than 120px of editor above it.
    const int split = MIN(MAX(app->termSplit, Terminal::kMinHeight),
                          MAX(120, H - 120));
    gtk_paned_set_position(GTK_PANED(app->vpaned), MAX(0, H - split));
}

// Save the divider as its distance from the BOTTOM of the paned, which is the
// quantity set_position round-trips exactly. Saving the panel's pixel height
// instead would drop the divider's own thickness on every close/reopen, and the
// terminal would creep smaller each time it was toggled.
static void rememberTerminalSplit(App* app) {
    const int H = gtk_widget_get_height(app->vpaned);
    const int pos = gtk_paned_get_position(GTK_PANED(app->vpaned));
    // Not while the editor is snapped shut: that is not a height to come back to.
    if (H > 0 && pos >= kTopSnap && H - pos >= Terminal::kMinHeight)
        app->termSplit = H - pos;
}
#endif

// The editor half is out of the way: hidden by Ctrl+Shift+E, or dragged shut.
static bool editorCollapsed(App* app) {
    if (!gtk_widget_get_visible(app->upperBox)) return true;
    return app->termPanel && gtk_widget_get_visible(app->termPanel) &&
           gtk_paned_get_position(GTK_PANED(app->vpaned)) < kTopSnap;
}

// Dragging the terminal's divider near the top closes the editor completely,
// instead of leaving a sliver of it. The divider stays at the top edge, so it
// can be dragged back down.
static void onVpanedPosition(GObject*, GParamSpec*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (app->adjustingPaned || !app->termPanel ||
        !gtk_widget_get_visible(app->termPanel))
        return;
    int pos = gtk_paned_get_position(GTK_PANED(app->vpaned));
    if (pos > 0 && pos < kTopSnap) {
        app->adjustingPaned = true;
        gtk_paned_set_position(GTK_PANED(app->vpaned), 0);
        app->adjustingPaned = false;
    }
    refreshHints(app);
}

// ------------------------------------------------- the sidebar/right split
static bool rightCollapsed(App* app) {
    int maxPos = 0;
    g_object_get(app->hpaned, "max-position", &maxPos, NULL);
    return gtk_widget_get_visible(app->sidebar) && maxPos > 0 &&
           gtk_paned_get_position(GTK_PANED(app->hpaned)) >= maxPos;
}

// Dragging the sidebar's divider to the right edge collapses the editor,
// terminal and browser, leaving the file tree.
static void onHpanedPosition(GObject*, GParamSpec*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (app->adjustingPaned) return;
    int maxPos = 0;
    g_object_get(app->hpaned, "max-position", &maxPos, NULL);
    int pos = gtk_paned_get_position(GTK_PANED(app->hpaned));
    if (maxPos <= 0) return;
    if (pos < maxPos && maxPos - pos < kRightSnap) {
        app->adjustingPaned = true;
        gtk_paned_set_position(GTK_PANED(app->hpaned), maxPos);
        app->adjustingPaned = false;
    }
}

// Remember the sidebar width when a drag starts, so collapsing and reopening
// comes back to the width it had, not somewhere along the way.
static void onHpanedPressed(GtkGestureClick*, int, double, double, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (!rightCollapsed(app))
        app->sidebarWidth = gtk_paned_get_position(GTK_PANED(app->hpaned));
}

// Bring the right side back if it was dragged shut.
static void showRightArea(App* app) {
    if (!rightCollapsed(app)) return;
    int maxPos = 0;
    g_object_get(app->hpaned, "max-position", &maxPos, NULL);
    int width = app->sidebarWidth;
    if (width <= 0 || width > maxPos - kRightSnap) width = MIN(240, maxPos / 3);
    app->adjustingPaned = true;
    gtk_paned_set_position(GTK_PANED(app->hpaned), width);
    app->adjustingPaned = false;
}

// Bring the editor/browser half back after it was collapsed, restoring the
// terminal to the height it had. Anything that needs to show something up there
// calls this first, so the editor cannot stay hidden behind new content.
static void showEditorArea(App* app) {
    showRightArea(app);
    if (!editorCollapsed(app)) return;
    gtk_widget_set_visible(app->upperBox, TRUE);
#ifdef MINICODE_ENABLE_TERMINAL
    app->adjustingPaned = true;
    placeTerminalDivider(app);
    app->adjustingPaned = false;
#endif
    refreshHints(app);
}

// ---------------------------------------------------------------- helpers

// Play/Pause (Ctrl+Shift+Space) acts only while video or audio that can play
// is shown, and never while the keyboard is typing somewhere: in the
// terminal, whose Ctrl+Shift+Space is the shell's, or in a text field such
// as the commit message. Disabled, the key goes on to whatever has the
// focus. Called when the title changes and when the focus moves.
static void updateMediaKey(App* app) {
    if (app->dead || !app->editor) return;
    GAction* a = g_action_map_lookup_action(G_ACTION_MAP(app->window), "playpause");
    if (!a) return;
    bool on = app->editor->canPlayMedia();
    if (on) {
        GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(app->window));
#ifdef MINICODE_ENABLE_TERMINAL
        if (app->terminal && app->terminal->owns(focus)) on = false;
#endif
        if (focus && GTK_IS_TEXT_VIEW(focus) && gtk_text_view_get_editable(GTK_TEXT_VIEW(focus)))
            on = false;
        if (focus && GTK_IS_EDITABLE(focus) && gtk_editable_get_editable(GTK_EDITABLE(focus)))
            on = false;
    }
    g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
}

static void updateTitle(void* userp) {
    App* app = static_cast<App*>(userp);
    std::string title = "MiniCode";
    const std::string& p = app->editor->currentPath();
    if (app->editor->isDiff()) {
        // "name (diff)" or "<short hash> <subject>", as the Mac titles it.
        title = "MiniCode — " + app->editor->diffTitle();
    } else if (!p.empty()) {
        auto slash = p.find_last_of('/');
        std::string base = slash == std::string::npos ? p : p.substr(slash + 1);
        title = "MiniCode — " + base + (app->editor->dirty() ? " *" : "");
        // A Markdown file with tasks: how many are done.
        if (app->editor->tasksTotal() > 0)
            title += " — " + std::to_string(app->editor->tasksDone()) + " of " +
                     std::to_string(app->editor->tasksTotal()) + " done";
        title += app->editor->titleSuffix();   // pixel size or page count
    }
    gtk_window_set_title(GTK_WINDOW(app->window), title.c_str());
    // Nothing to save while an image, a PDF or a binary file is shown.
    if (GAction* save = g_action_map_lookup_action(G_ACTION_MAP(app->window), "save"))
        g_simple_action_set_enabled(G_SIMPLE_ACTION(save), app->editor->canSave());
    if (GAction* exp = g_action_map_lookup_action(G_ACTION_MAP(app->window), "exportpdf"))
        g_simple_action_set_enabled(G_SIMPLE_ACTION(exp), app->editor->isLatex());
    // The zoom keys act only on a PDF or the LaTeX preview on screen; disabled,
    // their keys go on to whatever has the focus.
    for (const char* zoom : {"zoomin", "zoomout", "zoomfit"})
        if (GAction* z = g_action_map_lookup_action(G_ACTION_MAP(app->window), zoom))
            g_simple_action_set_enabled(G_SIMPLE_ACTION(z), app->editor->showingPdf());
    updateMediaKey(app);
    updateKeyPlace();    // a Markdown file's own keys
    refreshHints(app);   // Play/Pause is listed while video or audio can play
    gtk_label_set_text(GTK_LABEL(app->statusLabel),
                       app->editor->isDiff() ? app->editor->diffTitle().c_str()
                       : p.empty()           ? "Ready"
                                             : p.c_str());
}

static std::string baseName(const std::string& p) {
    auto slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// Is `path` the folder `dir` or somewhere inside it?
static bool isInside(const std::string& path, const std::string& dir) {
    return !dir.empty() &&
           (path == dir || path.compare(0, dir.size() + 1, dir + "/") == 0);
}

// ------------------------------------------------------------ data safety
//
// The macOS build asks "Save changes?" before anything replaces an edited
// buffer (EditorController.mm -confirmProceedPastUnsavedChanges): opening
// another file, opening a folder, closing the window. GtkAlertDialog is
// asynchronous, so here the question takes the rest of the operation as a
// continuation instead of returning an answer.

static void showError(App* app, const std::string& message, const std::string& detail) {
    GtkAlertDialog* dlg = gtk_alert_dialog_new("%s", message.c_str());
    if (!detail.empty()) gtk_alert_dialog_set_detail(dlg, detail.c_str());
    // An explicit button rather than gtk_alert_dialog_show's default one: that
    // builds a different kind of window, which did not close when its button
    // was activated in testing. With a button of our own it is the same window
    // as the "Save changes?" alert.
    const char* buttons[] = {"OK", nullptr};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_default_button(dlg, 0);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_choose(dlg, GTK_WINDOW(app->window), nullptr, nullptr, nullptr);
    g_object_unref(dlg);
}

// Save, and tell the user if that failed. The editor leaves the buffer marked
// unsaved on failure, so the title keeps its "*".
static void flashStatus(App* app, const std::string& message);

// One tab written to its file. editor.keep-sorted-on-save sorts the blocks of
// the tab on show first, as one undo step; a tab saved in the background is
// written as it is.
static bool saveDocument(App* app, Document* d) {
    if (!d) return true;
    std::string sortProblem;
    if (d == app->editor->active() && g.settings->settings().keepSortedOnSave())
        sortProblem = app->editor->sortBlocksBeforeSave();
    std::string err;
    const bool ok = app->editor->saveDocument(d, &err);
    updateTitle(app);
    if (ok && app->git && app->gitShown) app->git->refresh();
    if (!ok)
        showError(app, "Could not save “" + baseName(d->path()) + "”",
                  err + "\n\nYour changes are still here, unsaved.");
    else if (!sortProblem.empty())
        flashStatus(app, sortProblem);
    return ok;
}

static bool saveCurrent(App* app) { return saveDocument(app, app->editor->active()); }

// A message in the status bar for a few seconds, then what it said before.
static void flashStatus(App* app, const std::string& message) {
    if (message.empty()) return;
    struct Flash {
        GtkWidget* label;
        std::string message, before;
    };
    auto* f = new Flash{app->statusLabel, message, gtk_label_get_text(GTK_LABEL(app->statusLabel))};
    g_object_ref(f->label);
    gtk_label_set_text(GTK_LABEL(f->label), message.c_str());
    g_timeout_add_seconds(6, [](gpointer p) -> gboolean {
        auto* f = static_cast<Flash*>(p);
        // Unless something else has been written there since.
        if (f->message == gtk_label_get_text(GTK_LABEL(f->label)))
            gtk_label_set_text(GTK_LABEL(f->label), f->before.c_str());
        g_object_unref(f->label);
        delete f;
        return G_SOURCE_REMOVE;
    }, f);
}

// The question about unsaved changes, the Mac's: `answer` gets 0 for Save
// (or Save All), 1 for Don't Save, 2 for Cancel or Escape. One question at
// a time: with another up, the answer is Cancel at once. A change on disk
// that arrived while it was up is looked at afterwards.
static void askUnsaved(App* app, const std::string& message, const std::string& detail,
                       bool several, std::function<void(int)> answer) {
    if (app->prompting) { answer(2); return; }
    app->prompting = true;
    GtkAlertDialog* dlg = gtk_alert_dialog_new("%s", message.c_str());
    gtk_alert_dialog_set_detail(dlg, detail.c_str());
    const char* buttons[] = {several ? "Save All" : "Save", "Don't Save", "Cancel", nullptr};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_default_button(dlg, 0);
    gtk_alert_dialog_set_cancel_button(dlg, 2);
    gtk_alert_dialog_set_modal(dlg, TRUE);
    struct Pending {
        App* app;
        std::function<void(int)> answer;
    };
    gtk_alert_dialog_choose(dlg, GTK_WINDOW(app->window), nullptr,
        [](GObject* src, GAsyncResult* res, gpointer data) {
            Pending* pd = static_cast<Pending*>(data);
            App* a = pd->app;
            a->prompting = false;
            int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
            if (choice < 0 || choice > 2) choice = 2;
            if (!a->dead) pd->answer(choice);
            delete pd;
            if (!a->dead) a->editor->checkExternalChange();
        }, new Pending{app, std::move(answer)});
    g_object_unref(dlg);
}

static const char* kLostDetail = "Your changes will be lost if you don't save them.";

// Close one tab, asking Save, Don't Save or Cancel first when it has unsaved
// changes (the Mac's confirmCloseTab:). `done` hears whether it closed; a
// failed save keeps it, with the reason shown.
static void closeTabAsking(App* app, Document* d, std::function<void(bool)> done = nullptr) {
    auto finish = [done](bool closed) { if (done) done(closed); };
    if (app->dead || !d || app->editor->indexOf(d) < 0) { finish(false); return; }
    // With auto-save on, what it would write once the tab goes is written
    // first, so it is not asked about.
    if (d->dirty()) app->persist->autoSaveNow();
    if (!d->dirty()) {
        app->editor->closeTab(d);
        refreshHints(app);
        finish(true);
        return;
    }
    const std::string name = baseName(d->path());
    askUnsaved(app, "Save changes to “" + (name.empty() ? std::string("this file") : name) + "”?",
               kLostDetail, false, [app, d, finish](int choice) {
        // Closed meanwhile, or not to be closed.
        if (app->editor->indexOf(d) < 0 || choice == 2) { finish(false); return; }
        if (choice == 0 && !saveDocument(app, d)) { finish(false); return; }
        app->editor->closeTab(d);
        refreshHints(app);
        finish(true);
    });
}

// Close these tabs in turn; Cancel at any unsaved one stops there. They go
// by id, since each question is answered later: one closed meanwhile is
// passed over.
static void closeTabsAsking(App* app, std::vector<uint64_t> ids) {
    if (app->dead) return;
    Document* first = nullptr;
    while (!ids.empty() && !first) {
        first = app->editor->documentById(ids.front());
        ids.erase(ids.begin());
    }
    if (!first) return;
    closeTabAsking(app, first, [app, ids](bool closed) {
        if (closed) closeTabsAsking(app, ids);
    });
}

static std::vector<uint64_t> idsOf(const std::vector<Document*>& docs) {
    std::vector<uint64_t> out;
    for (Document* d : docs) out.push_back(d->id());
    return out;
}

// Before the window closes, the app quits or another folder takes over: one
// question for every tab with unsaved changes (the Mac's
// confirmCloseAllTabs). With one, it is put on show and asked about as a
// tab is; with several, Save All, Don't Save or Cancel for them all. Runs
// `proceed` once nothing unsaved is left to lose, else `cancelled`.
static void confirmAllUnsaved(App* app, std::function<void()> proceed,
                              std::function<void()> cancelled = nullptr) {
    app->persist->autoSaveNow();   // auto-save's writes first: only what it skips is asked about
    const std::vector<Document*> dirty = app->editor->dirtyDocuments();
    auto cancel = [cancelled] { if (cancelled) cancelled(); };
    if (dirty.empty()) { proceed(); return; }
    if (app->prompting) { cancel(); return; }
    if (dirty.size() == 1) {
        Document* d = dirty[0];
        app->editor->activate(d);   // so the file asked about is the one on show
        const std::string name = baseName(d->path());
        askUnsaved(app, "Save changes to “" + name + "”?", kLostDetail, false,
                   [app, d, proceed, cancel](int choice) {
            if (choice == 2) { cancel(); return; }
            if (choice == 0 && app->editor->indexOf(d) >= 0 && !saveDocument(app, d)) {
                cancel();
                return;
            }
            proceed();
        });
        return;
    }
    std::string names;
    for (Document* d : dirty) names += baseName(d->path()) + "\n";
    askUnsaved(app, "Save changes to " + std::to_string(dirty.size()) + " files?",
               names + "\n" + kLostDetail, true, [app, proceed, cancel](int choice) {
        if (choice == 2) { cancel(); return; }
        if (choice == 0)
            for (Document* d : app->editor->dirtyDocuments())
                if (!saveDocument(app, d)) { cancel(); return; }
        proceed();
    });
}

// The file tree's selection follows the tab on show, as on the Mac
// (reselectCurrentFileInTree).
static void revealActiveInTree(App* app) {
    const std::string& cur = app->editor->currentPath();
    if (isInside(cur, app->rootDir)) app->tree->revealPath(cur);
}

// A file changed on disk while its tab has edits of its own: the Mac's
// question, "Keep Mine" or "Reload" (EditorController.mm -checkExternalChange).
// Only ever about the tab on show. Returns false when another question is
// already up; the editor asks again on its next check.
static bool askExternalChange(void* userp) {
    App* app = static_cast<App*>(userp);
    if (app->dead || app->prompting) return false;
    app->prompting = true;
    const std::string name = baseName(app->editor->currentPath());
    GtkAlertDialog* dlg = gtk_alert_dialog_new("“%s” changed on disk.", name.c_str());
    gtk_alert_dialog_set_detail(dlg, "You have unsaved changes here. Keep your version, "
                                     "or reload the file from disk and lose them?");
    const char* buttons[] = {"Keep Mine", "Reload", nullptr};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_default_button(dlg, 0);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_modal(dlg, TRUE);
    gtk_alert_dialog_choose(dlg, GTK_WINDOW(app->window), nullptr,
        [](GObject* src, GAsyncResult* res, gpointer data) {
            App* a = static_cast<App*>(data);
            a->prompting = false;
            const int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src),
                                                              res, nullptr);
            if (a->dead) return;
            if (choice == 1) {
                if (!a->editor->reloadFromDisk())
                    showError(a, "Could not reload “" +
                                 baseName(a->editor->currentPath()) + "”",
                              "It can no longer be read as text. Your version is still here.");
                updateTitle(a);
            }
            a->editor->checkExternalChange();   // anything newer since
        }, app);
    g_object_unref(dlg);
    return true;
}

// Open a file in its tab, or a new one: in the preview tab when `from` opens
// there (a single click in the tree, a search result, a definition), else a
// kept tab. The tab on show keeps its unsaved changes, so nothing is asked.
// Anything to do once the file is on show goes in `then`.
static void openFileThen(App* app, const std::string& path, EditorTabs::OpenFrom from,
                         std::function<void()> then) {
    showEditorArea(app);   // opening a file must not disappear into a hidden pane
    app->editor->openFile(path, from);
    updateTitle(app);
    refreshHints(app);   // the Markdown line depends on the open file
    if (then) then();
}

// Called by everything that opens a file with nothing to do afterwards, in a
// tab of its own.
static void openFileCb(const std::string& path, void* userp) {
    openFileThen(static_cast<App*>(userp), path, EditorTabs::OpenFrom::Other, nullptr);
}

// A file opened from the tree: a single click (the preview tab), a
// double-click or Ctrl+click (kept), or Enter (kept), after which the
// keyboard goes to the editor, as on the Mac (EditorController.mm
// -activateRow:).
static void treeOpenCb(const std::string& path, EditorTabs::OpenFrom from, void* userp) {
    App* app = static_cast<App*>(userp);
    openFileThen(app, path, from, [app, from] {
        if (from == EditorTabs::OpenFrom::TreeReturn) app->editor->focusActiveDocument();
    });
}

// Re-root on a new folder. The old folder's tabs close (the caller asked
// about their unsaved changes), as on the Mac, so the editor never holds a
// file the tree no longer shows.
static void openFolder(App* app, const std::string& dir) {
    app->persist->saveSession();   // the old folder's tabs, for when it is back
    app->rootDir = dir;
    app->tree->setRoot(dir);
    if (app->git) app->git->setRoot(dir);   // the panel follows the folder
    app->editor->closeAllTabs();
    // After the tabs close, so no old file is reopened under the new root.
    if (app->lsp) app->lsp->setRoot(dir);   // servers belong to a folder
#ifdef MINICODE_ENABLE_TERMINAL
    if (app->terminal) app->terminal->setProjectRoot(dir);   // for its links
#endif
    app->persist->setRoot(dir);
    app->persist->restoreSession();   // the new folder's tabs from last time
    updateTitle(app);
    refreshHints(app);
}

// Closing the window with unsaved tabs asks once for them all. The answer
// arrives later, so the close is refused now and repeated once the edits are
// dealt with.
static gboolean onCloseRequest(GtkWindow*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (app->closing || !app->editor->anyDirty()) {
        // The window really is closing: stop its language servers
        // (shutdown, exit, then signals if they linger).
        if (app->lsp) app->lsp->shutdown();
        return FALSE;
    }
    confirmAllUnsaved(app, [app] {
        app->closing = true;
        gtk_window_close(GTK_WINDOW(app->window));
    }, [] { g.quitting = false; });   // a quit stops at a window kept open
    return TRUE;
}

// ---------------------------------------------------------------- actions

static void act_open(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    confirmAllUnsaved(app, [app] {
        GtkFileDialog* dlg = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dlg, "Open Folder");
        GFile* start = g_file_new_for_path(app->rootDir.c_str());
        gtk_file_dialog_set_initial_folder(dlg, start);
        g_object_unref(start);
        gtk_file_dialog_select_folder(dlg, GTK_WINDOW(app->window), nullptr,
            [](GObject* src, GAsyncResult* res, gpointer up) {
                App* a = static_cast<App*>(up);
                GFile* folder = gtk_file_dialog_select_folder_finish(
                    GTK_FILE_DIALOG(src), res, nullptr);
                if (!folder) return;   // cancelled
                if (a->dead) { g_object_unref(folder); return; }
                char* path = g_file_get_path(folder);
                // The edits were dealt with before the dialog opened, but the
                // user may have typed more while it was up.
                if (path) {
                    std::string dir = path;
                    confirmAllUnsaved(a, [a, dir] { openFolder(a, dir); });
                    g_free(path);
                }
                g_object_unref(folder);
            }, app);
        g_object_unref(dlg);
    });
}

static void act_save(GSimpleAction*, GVariant*, gpointer userp) {
    saveCurrent(static_cast<App*>(userp));
}

// Ctrl+Shift+S: the typeset PDF of a LaTeX file, as on the Mac (Shift+Cmd+S).
static void act_export_pdf(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    app->editor->exportPdf(GTK_WINDOW(app->window));
}

// Zoom for a PDF or the LaTeX preview (PdfView). The reset is Ctrl+Alt+0,
// not Ctrl+0, which is Focus the File Tree here as on the Mac.
static void act_play_pause(GSimpleAction*, GVariant*, gpointer userp) {
    static_cast<App*>(userp)->editor->togglePlayMedia();
}

static void act_zoom_in(GSimpleAction*, GVariant*, gpointer userp) {
    static_cast<App*>(userp)->editor->zoomPdf(1);
}
static void act_zoom_out(GSimpleAction*, GVariant*, gpointer userp) {
    static_cast<App*>(userp)->editor->zoomPdf(-1);
}
static void act_zoom_fit(GSimpleAction*, GVariant*, gpointer userp) {
    static_cast<App*>(userp)->editor->zoomPdf(0);
}

static void act_toggle_preview(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    app->editor->togglePreview();
    refreshHints(app);
}

static void act_toggle_sidebar(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    // Hiding the tree with the right side collapsed would leave nothing.
    if (app->sidebarVisible) showRightArea(app);
    app->sidebarVisible = !app->sidebarVisible;
    gtk_widget_set_visible(app->sidebar, app->sidebarVisible);
    refreshHints(app);
}

// View > Show Hidden Files, Ctrl+Shift+.: the Mac's Shift+Cmd+.
// (toggleHiddenFiles: flips one global flag). An application action with a
// check-box state, so every window's tree and every window's menu agree.
// GTK matches it as Ctrl+> (Commands::linuxAccelerators), so it needs a
// layout where Shift+. types ">", as the US and UK ones do. Ctrl+H, the
// file managers' key, is Replace's (PARITY.md, D1).
static void onShowHiddenChanged(GSimpleAction* action, GVariant* value, gpointer) {
    g_simple_action_set_state(action, value);
    g.showHidden = g_variant_get_boolean(value);
    for (App* a : g.windows) a->tree->setShowHidden(g.showHidden);
}

static void act_toggle_terminal(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (!app->termPanel) return;
#ifdef MINICODE_ENABLE_TERMINAL
    if (gtk_widget_get_visible(app->termPanel)) {
        rememberTerminalSplit(app);   // only readable while it is still up
        gtk_widget_set_visible(app->termPanel, FALSE);
        // Closing the terminal while the editor is collapsed would leave an
        // empty window, so the editor comes back with it.
        showEditorArea(app);
    } else {
        showRightArea(app);
        gtk_widget_set_visible(app->termPanel, TRUE);
        placeTerminalDivider(app);
        if (app->terminal) app->terminal->focus();
    }
    refreshHints(app);
#endif
}

// Collapse the editor/browser half entirely so the terminal owns the window.
// Dragging cannot do this — the divider stops at a 120px editor, deliberately,
// so a stray drag can't leave an unusable sliver — so hiding the whole widget
// is what takes the minimum out of the picture. This is the same trick the
// macOS build uses for Cmd+B on the sidebar, where a collapse flag lets the
// split's 160px minimum drop to zero.
static void act_toggle_editor(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (editorCollapsed(app) || rightCollapsed(app)) { showEditorArea(app); return; }

#ifdef MINICODE_ENABLE_TERMINAL
    if (!app->termPanel) return;   // nothing else could fill the window
    if (gtk_widget_get_visible(app->termPanel)) rememberTerminalSplit(app);
    else gtk_widget_set_visible(app->termPanel, TRUE);
    gtk_widget_set_visible(app->upperBox, FALSE);
    if (app->terminal) app->terminal->focus();
    refreshHints(app);
#endif
}

static void act_toggle_browser(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (!app->browserRevealer) return;
    showEditorArea(app);   // the browser lives up there too
    gboolean shown = gtk_revealer_get_reveal_child(GTK_REVEALER(app->browserRevealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->browserRevealer), !shown);
    // The macOS build docks the terminal at the bottom and lets the editor OR
    // the browser fill the area above it (EditorController.mm
    // -relayoutRightArea). Hide the editor while the browser is up; otherwise
    // the browser's vexpand squeezes the editor down to a single line.
    gtk_widget_set_visible(app->editor->widget(), shown);
    // The web view's vexpand propagates up to the revealer, so a collapsed
    // revealer would still claim half the leftover space and leave a dead gap
    // under the editor. Only let it expand while it is actually revealed.
    gtk_widget_set_vexpand(app->browserRevealer, !shown);
#ifdef MINICODE_ENABLE_BROWSER
    if (!shown && app->browser) app->browser->focusUrlBar();
#endif
    refreshHints(app);
}

// Toggle Comment: only when the editor has the keyboard, so the shortcut
// never edits a file you are not looking at.
static void act_toggle_comment(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    GtkWidget* view = app->editor->textView();
    if (!gtk_widget_has_focus(view) || !app->editor->toggleComment())
        gtk_widget_error_bell(view);
}

// The task key in a Markdown file's source, on the lines the selection
// touches. Elsewhere it rings the bell.
static void act_toggle_task(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    GtkWidget* view = app->editor->textView();
    if (!gtk_widget_has_focus(view) || !app->editor->toggleTask())
        gtk_widget_error_bell(view);
}

// Sort Lines: the selected lines, or the keep-sorted block around the
// caret. Sort keep-sorted Blocks: every block in the file.
static void sortCommand(App* app, bool all) {
    GtkWidget* view = app->editor->textView();
    std::string message;
    const bool ok = gtk_widget_has_focus(view) &&
                    (all ? app->editor->fixKeepSorted(&message) : app->editor->sortLines(&message));
    if (!ok) gtk_widget_error_bell(view);
    flashStatus(app, message);
}
static void act_sort_lines(GSimpleAction*, GVariant*, gpointer userp) {
    sortCommand(static_cast<App*>(userp), false);
}
static void act_fix_keep_sorted(GSimpleAction*, GVariant*, gpointer userp) {
    sortCommand(static_cast<App*>(userp), true);
}

// Settings: open the settings file, writing the commented defaults first.
static void act_settings(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (!g.settings->ensureFileExists()) {
        gtk_label_set_text(GTK_LABEL(app->settingsLabel),
                           ("Could not create " + g.settings->path()).c_str());
        return;
    }
    openFileThen(app, g.settings->path(), EditorTabs::OpenFrom::Other,
                 [app] { gtk_widget_grab_focus(app->editor->textView()); });
}

// Keyboard Shortcuts…: keys.conf, as Settings… opens settings.conf; the
// first time it is written with every command Linux has, commented out.
static void act_keyboard_shortcuts(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (!Keys::ensureFileExists()) {
        gtk_label_set_text(GTK_LABEL(app->settingsLabel),
                           ("Could not create " + Keys::path()).c_str());
        return;
    }
    openFileThen(app, Keys::path(), EditorTabs::OpenFrom::Other,
                 [app] { gtk_widget_grab_focus(app->editor->textView()); });
}

// The find bar (FindBar.h): Find and Replace seed the query from the
// selection or the word at the caret; Find Next and Find Previous (VS Code's
// F3 and Shift+F3 on Linux) step from the selection, wrapping, with the bar
// open or closed, and open it when there is nothing to look for; the
// options flip from the editor or the bar; Replace and Replace All act on
// the matches as the bar shows them.
static FindBar* findBarOf(gpointer userp) { return static_cast<App*>(userp)->findBar; }
static void act_find(GSimpleAction*, GVariant*, gpointer p) { findBarOf(p)->showFind(); }
static void act_replace(GSimpleAction*, GVariant*, gpointer p) { findBarOf(p)->showReplace(); }
static void act_find_next(GSimpleAction*, GVariant*, gpointer p) { findBarOf(p)->findNext(true); }
static void act_find_previous(GSimpleAction*, GVariant*, gpointer p) {
    findBarOf(p)->findNext(false);
}
static void act_replace_one(GSimpleAction*, GVariant*, gpointer p) { findBarOf(p)->replaceOne(); }
static void act_replace_all(GSimpleAction*, GVariant*, gpointer p) { findBarOf(p)->replaceAll(); }
static void act_find_case(GSimpleAction*, GVariant*, gpointer p) {
    findBarOf(p)->toggleMatchCase();
}
static void act_find_word(GSimpleAction*, GVariant*, gpointer p) {
    findBarOf(p)->toggleWholeWord();
}
static void act_find_regex(GSimpleAction*, GVariant*, gpointer p) { findBarOf(p)->toggleRegex(); }
static void act_find_preserve(GSimpleAction*, GVariant*, gpointer p) {
    findBarOf(p)->togglePreserveCase();
}
static void act_find_in_selection(GSimpleAction*, GVariant*, gpointer p) {
    findBarOf(p)->toggleFindInSelection();
}

static void act_toggle_git(GSimpleAction*, GVariant*, gpointer userp);

// New Window: one on this window's folder, as the Mac's opens one on the
// front window's root.
static void act_new_window(GSimpleAction*, GVariant*, gpointer userp) {
    newWindow(static_cast<App*>(userp)->rootDir, "");
}

// Close Window: this window only, asking "Save changes?" first like any
// close.
static void act_close_window(GSimpleAction*, GVariant*, gpointer userp) {
    gtk_window_close(GTK_WINDOW(static_cast<App*>(userp)->window));
}

// Quit: every window, each asking about its own unsaved edits. One that is
// cancelled stays open, and so does the application with it.
static void act_quit(GSimpleAction*, GVariant*, gpointer) {
    const std::vector<App*> open = g.windows;   // closing edits the list
    // Every window's tabs first, each with the others on its folder, so a
    // quit with two windows on one folder reopens the files of both, as on
    // the Mac (windows closing one by one would leave only the last one's).
    for (App* a : open)
        if (!a->dead) a->persist->saveSession();
    g.quitting = true;
    for (App* a : open)
        if (!a->dead) gtk_window_close(GTK_WINDOW(a->window));
}

// The editor shown in the upper half, with the browser put away if it was
// covering it.
static void showEditorOverBrowser(App* app) {
    showEditorArea(app);
#ifdef MINICODE_ENABLE_BROWSER
    // The browser shares the editor's half; the editor is what was asked for.
    if (app->browserRevealer &&
        gtk_revealer_get_reveal_child(GTK_REVEALER(app->browserRevealer))) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(app->browserRevealer), FALSE);
        gtk_widget_set_visible(app->editor->widget(), TRUE);
        gtk_widget_set_vexpand(app->browserRevealer, FALSE);
    }
#endif
}

// Focus Editor: the editor gets the keyboard, shown first if it was hidden
// (the Mac's Command 1): what the tab on show shows, the LaTeX pages too.
static void act_focus_editor(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    showEditorOverBrowser(app);
    app->editor->focusActiveDocument();
    refreshHints(app);
}

// ---------------------------------------------------------------- tabs
//
// The tab on show, from the File and Navigate menus and the keys; a tab's
// right click menu sends the same commands for its own tab
// (Editor::onTabCommand), as the Mac's tab menu and File menu share them.
// Closing asks about unsaved changes first (closeTabAsking).

// The folder holding `path`, in the file manager, with the item selected
// where the file manager supports that.
static void openContainingFolder(App* app, const std::string& path) {
    GFile* f = g_file_new_for_path(path.c_str());
    GtkFileLauncher* launcher = gtk_file_launcher_new(f);
    g_object_unref(f);
    gtk_file_launcher_open_containing_folder(launcher, GTK_WINDOW(app->window), nullptr,
        [](GObject* src, GAsyncResult* res, gpointer up) {
            GError* err = nullptr;
            if (!gtk_file_launcher_open_containing_folder_finish(
                    GTK_FILE_LAUNCHER(src), res, &err) &&
                !g_error_matches(err, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
                !static_cast<App*>(up)->dead)
                showError(static_cast<App*>(up), "Could not open the containing folder",
                          err ? err->message : "");
            g_clear_error(&err);
        }, app);
    g_object_unref(launcher);
}

// A tab's file selected in the tree, the tree brought back first when the
// sidebar is hidden or shows Source Control (the Mac's revealTabInTree:).
static void revealInTree(App* app, const std::string& path) {
    if (!isInside(path, app->rootDir)) {
        gtk_widget_error_bell(app->window);
        return;
    }
    if (app->gitShown) act_toggle_git(nullptr, nullptr, app);
    if (!app->sidebarVisible) act_toggle_sidebar(nullptr, nullptr, app);
    app->tree->revealPath(path);
    app->tree->focus();
}

static void tabCommand(App* app, const std::string& name, Document* d) {
    if (app->dead || !d) return;
    Editor* e = app->editor;
    const std::vector<Document*> all = e->documents();
    if (name == "keep") {
        e->keepPreviewTab(d);
    } else if (name == "close") {
        closeTabAsking(app, d);
    } else if (name == "closeothers") {
        std::vector<Document*> others;
        for (Document* t : all)
            if (t != d) others.push_back(t);
        closeTabsAsking(app, idsOf(others));
    } else if (name == "closeright") {
        const int i = e->indexOf(d);
        if (i >= 0) closeTabsAsking(app, idsOf(std::vector<Document*>(all.begin() + i + 1, all.end())));
    } else if (name == "closeall") {
        closeTabsAsking(app, idsOf(all));
    } else if (name == "copypath") {
        if (!d->path().empty())
            gdk_clipboard_set_text(gtk_widget_get_clipboard(app->window), d->path().c_str());
    } else if (name == "reveal") {
        if (!d->path().empty()) openContainingFolder(app, d->path());
    } else if (name == "revealtree") {
        revealInTree(app, d->path());
    }
    updateTitle(app);
    refreshHints(app);
}

// The same for the tab on show; the bell with none.
static void activeTabCommand(App* app, const char* name) {
    Document* d = app->editor->active();
    if (!d) {
        gtk_widget_error_bell(app->window);
        return;
    }
    tabCommand(app, name, d);
}

static void act_close_other_tabs(GSimpleAction*, GVariant*, gpointer userp) {
    activeTabCommand(static_cast<App*>(userp), "closeothers");
}
static void act_close_tabs_right(GSimpleAction*, GVariant*, gpointer userp) {
    activeTabCommand(static_cast<App*>(userp), "closeright");
}
static void act_close_all_tabs(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    closeTabsAsking(app, idsOf(app->editor->documents()));
}
static void act_copy_active_path(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (app->editor->currentPath().empty()) gtk_widget_error_bell(app->window);
    else activeTabCommand(app, "copypath");
}
static void act_reveal_active(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (app->editor->currentPath().empty()) gtk_widget_error_bell(app->window);
    else activeTabCommand(app, "reveal");
}
static void act_reveal_active_in_tree(GSimpleAction*, GVariant*, gpointer userp) {
    activeTabCommand(static_cast<App*>(userp), "revealtree");
}

// Keep Open: the preview tab on show becomes an ordinary tab.
static void act_keep_tab(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    Document* d = app->editor->active();
    if (!d || !d->isPreviewTab()) gtk_widget_error_bell(app->window);
    else app->editor->keepPreviewTab(d);
}

// Close Tab, Ctrl+W (the Mac's closeFocused:): what has the keyboard closes.
// The browser panel hides while it has the keyboard, anywhere else the tab
// on show closes (asking about unsaved changes first), and with no tab
// left the window closes, so the key is never dead. In the terminal Ctrl+W
// is the shell's (it deletes a word).
static void act_close_tab(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
#ifdef MINICODE_ENABLE_BROWSER
    GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(app->window));
    if (app->browser && app->browserRevealer &&
        gtk_revealer_get_reveal_child(GTK_REVEALER(app->browserRevealer)) &&
        Keys::within(focus, app->browser->widget())) {
        act_toggle_browser(nullptr, nullptr, app);
        app->editor->focusActiveDocument();
        return;
    }
#endif
    Document* d = app->editor->active();
    if (!d) {
        gtk_window_close(GTK_WINDOW(app->window));
        return;
    }
    closeTabAsking(app, d);
}

// Reopen Closed Tab: the newest of the window's last 50 closed tabs, where
// it was, with its preview, selection and scroll.
static void act_reopen_tab(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    showEditorOverBrowser(app);
    if (!app->editor->reopenClosedTab()) gtk_widget_error_bell(app->window);
    updateTitle(app);
    refreshHints(app);
}

// Save All: every tab with unsaved changes, by Save's rules; the first that
// fails stops it, with the reason shown.
static void act_save_all(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    const std::vector<Document*> dirty = app->editor->dirtyDocuments();
    if (dirty.empty()) {
        gtk_widget_error_bell(app->window);
        return;
    }
    for (Document* d : dirty)
        if (!saveDocument(app, d)) break;
}

// Next and previous tab, wrapping, and tab 1 to 8 and the last: the tab
// comes on show with the keyboard, the editor brought back first.
static void act_next_tab(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    showEditorOverBrowser(app);
    app->editor->stepTab(1);
    refreshHints(app);
}
static void act_previous_tab(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    showEditorOverBrowser(app);
    app->editor->stepTab(-1);
    refreshHints(app);
}
template <int N>
static void act_go_to_tab(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    showEditorOverBrowser(app);
    app->editor->selectTabNumber(N);
    refreshHints(app);
}

// Opens a link from the terminal or a Markdown preview, the Mac's
// -openTerminalLink:. A URL opens in the browser panel; a file opens in the
// editor at the line and column the reference named; a folder is shown in
// the tree.
static void openLink(App* app, bool isUrl, const std::string& target, bool isDir,
                     bool insideRoot, int line, int column) {
    if (isUrl) {
#ifdef MINICODE_ENABLE_BROWSER
        if (app->browser && app->browserRevealer) {
            if (!gtk_revealer_get_reveal_child(GTK_REVEALER(app->browserRevealer)))
                act_toggle_browser(nullptr, nullptr, app);
            app->browser->load(target);
            return;
        }
#endif
        // Built without the browser panel: the desktop's own browser.
        GtkUriLauncher* launcher = gtk_uri_launcher_new(target.c_str());
        gtk_uri_launcher_launch(launcher, GTK_WINDOW(app->window), nullptr, nullptr, nullptr);
        g_object_unref(launcher);
        return;
    }

    const std::string path = target;
    if (isDir) {
        // Only the tree can show a folder, and only one inside it.
        if (!insideRoot) { gtk_widget_error_bell(app->window); return; }
        if (!app->sidebarVisible) act_toggle_sidebar(nullptr, nullptr, app);
        if (app->gitShown) act_toggle_git(nullptr, nullptr, app);   // the tree back
        app->tree->revealPath(path);
        return;
    }
    if (insideRoot) app->tree->revealPath(path);
    openFileThen(app, path, EditorTabs::OpenFrom::Other, [app, path, line, column] {
        // Opening can be refused (Cancel at "Save changes?"), and an image
        // or a PDF has no lines to go to.
        if (app->editor->currentPath() != path) return;
        showEditorOverBrowser(app);
        if (app->editor->isMedia()) return;
        if (line > 0) app->editor->revealLineColumn(line, column);
        else gtk_widget_grab_focus(app->editor->textView());
        refreshHints(app);   // a Markdown preview may have switched to source
    });
}

#ifdef MINICODE_ENABLE_TERMINAL
// Ctrl+click on a link in the terminal (Terminal::Link).
static void openTerminalLink(App* app, const Terminal::Link& link) {
    openLink(app, link.isUrl, link.target, link.isDir, link.insideRoot, link.line, link.column);
}
#endif

// A click on a link in a Markdown preview (Editor::Link). The editor has
// resolved the path; whether it is inside the open folder is decided here.
static void openEditorLink(App* app, const Editor::Link& link) {
    const bool inside = !link.isUrl && isInside(link.target, app->rootDir);
    openLink(app, link.isUrl, link.target, link.isDir, inside, link.line, 0);
}

// ------------------------------------------------------ file tree actions
//
// The same six as the Mac's tree menu and File menu (EditorController.mm,
// buildTreeContextMenu): each acts on the selected row, and a right-click
// selects the row it lands on. The directory monitors refresh the tree, and
// revealPath selects what an action made.

// Where New File and New Folder put things: the selected folder, the selected
// file's folder, or the root when nothing is selected (-targetDirectory).
static std::string targetDirectory(App* app) {
    const std::string folder = app->tree->selectedDir();
    if (!folder.empty()) return folder;
    const std::string sel = app->tree->selectedPath();   // a file, or nothing
    if (sel.empty()) return app->rootDir;
    char* dir = g_path_get_dirname(sel.c_str());
    std::string out = dir;
    g_free(dir);
    return out;
}

// A name typed into the popover must name one entry in one folder.
static bool validName(App* app, const std::string& name) {
    if (name == "." || name == "..") {
        showError(app, "“" + name + "” cannot be used as a name.", "");
        return false;
    }
    if (name.find('/') != std::string::npos) {
        showError(app, "A name cannot contain “/”.", "");
        return false;
    }
    return true;
}

// GIO's "file exists" message names nothing; say which name clashed.
static void showFileError(App* app, const std::string& what, const std::string& name,
                          GError* err) {
    if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS))
        showError(app, "“" + name + "” already exists.",
                  "Choose a different name.");
    else
        showError(app, what, err ? err->message : "");
}

static void act_new_file(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    const std::string dir = targetDirectory(app);
    app->tree->askName("New file name", "", [app, dir](const std::string& name) {
        if (!validName(app, name)) return;
        const std::string path = dir + "/" + name;
        GFile* f = g_file_new_for_path(path.c_str());
        GError* err = nullptr;
        // g_file_create fails rather than truncating an existing file.
        GFileOutputStream* out = g_file_create(f, G_FILE_CREATE_NONE, nullptr, &err);
        g_object_unref(f);
        if (!out) {
            showFileError(app, "Could not create “" + name + "”", name, err);
            g_clear_error(&err);
            return;
        }
        g_output_stream_close(G_OUTPUT_STREAM(out), nullptr, nullptr);
        g_object_unref(out);
        app->tree->revealPath(path);
        openFileCb(path, app);
    });
}

static void act_new_folder(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    const std::string dir = targetDirectory(app);
    app->tree->askName("New folder name", "", [app, dir](const std::string& name) {
        if (!validName(app, name)) return;
        const std::string path = dir + "/" + name;
        GFile* f = g_file_new_for_path(path.c_str());
        GError* err = nullptr;
        const bool ok = g_file_make_directory(f, nullptr, &err);
        g_object_unref(f);
        if (!ok) {
            showFileError(app, "Could not create “" + name + "”", name, err);
            g_clear_error(&err);
            return;
        }
        app->tree->revealPath(path);
    });
}

// A file or folder moved from `src` to `dst` (Rename, or a drag in the tree).
// Every tab of it keeps its buffer, edits included, under the new name, in
// every window that has it open: one left on the old name would save its
// edits back there and bring the old file back. That holds when a folder
// above it moved, too.
static void followMove(const std::string& src, const std::string& dst) {
    for (App* a : g.windows) {
        a->editor->pathMoved(src, dst);
        updateTitle(a);
        refreshHints(a);   // the preview toggle depends on the name
    }
}

static void act_rename(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    const std::string src = app->tree->selectedPath();
    if (src.empty()) { gtk_widget_error_bell(app->window); return; }
    const std::string old = baseName(src);
    app->tree->askName("Rename to", old, [app, src, old](const std::string& name) {
        if (name == old || !validName(app, name)) return;
        char* parent = g_path_get_dirname(src.c_str());
        const std::string dst = std::string(parent) + "/" + name;
        g_free(parent);
        GFile* from = g_file_new_for_path(src.c_str());
        GFile* to = g_file_new_for_path(dst.c_str());
        GError* err = nullptr;
        // Without OVERWRITE, GIO refuses when the name is taken, where a bare
        // rename(2) would silently replace that file.
        const bool ok = g_file_move(from, to, G_FILE_COPY_NOFOLLOW_SYMLINKS,
                                    nullptr, nullptr, nullptr, &err);
        g_object_unref(from);
        g_object_unref(to);
        if (!ok) {
            showFileError(app, "Could not rename “" + old + "”", name, err);
            g_clear_error(&err);
            return;
        }
        followMove(src, dst);
        app->tree->revealPath(dst);
    });
}

// "“name” already exists in “folder”." for a drop that would have replaced
// something; GIO's own message names neither.
static void showTaken(App* app, const std::vector<std::string>& names,
                      const std::string& dir, const std::string& detail) {
    const std::string folder = "“" + baseName(dir) + "”";
    if (names.size() == 1) {
        showError(app, "“" + names[0] + "” already exists in " + folder + ".", detail);
        return;
    }
    std::string list;
    for (const std::string& n : names) list += (list.empty() ? "" : ", ") + n;
    showError(app, std::to_string(names.size()) + " items already exist in " + folder + ".",
              list + ". " + detail);
}

namespace {
// Files dragged in from another program, copied into the tree on a worker
// thread, since a folder may be large.
struct CopyJob {
    App* app = nullptr;
    std::vector<std::string> paths;
    std::string dir;
    std::vector<std::string> copied;   // where each copy went
    std::vector<std::string> taken;    // names the folder already had
    std::string failed, why;           // the first other failure
};

// `src` to `dst`, a folder with everything in it, never over anything that is
// there; a symbolic link is copied as a link. Stops at the first error.
bool copyTree(GFile* src, GFile* dst, GError** err) {
    if (g_file_query_file_type(src, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr) !=
        G_FILE_TYPE_DIRECTORY)
        return g_file_copy(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS, nullptr, nullptr,
                           nullptr, err);
    if (!g_file_make_directory(dst, nullptr, err)) return false;
    GFileEnumerator* en = g_file_enumerate_children(src, "standard::name",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, err);
    if (!en) return false;
    bool ok = true;
    while (ok) {
        GFileInfo* info = nullptr;   // both owned by the enumerator
        GFile* child = nullptr;
        if (!g_file_enumerator_iterate(en, &info, &child, nullptr, err)) { ok = false; break; }
        if (!info) break;
        GFile* to = g_file_get_child(dst, g_file_info_get_name(info));
        ok = copyTree(child, to, err);
        g_object_unref(to);
    }
    g_object_unref(en);
    return ok;
}
}  // namespace

// Something dropped on the tree (FileTree::setDropCallback). A row dragged
// within MiniCode moves, as Rename moves it, and the open file follows it;
// files from another program are copied. Neither ever replaces an entry of
// the same name. The folder monitors bring the rows in, and revealPath
// selects what arrived.
static void dropOnTree(App* app, const std::vector<std::string>& paths,
                       const std::string& dir, bool move) {
    if (move) {
        for (const std::string& src : paths) {
            const std::string name = baseName(src);
            const std::string dst = dir + "/" + name;
            GFile* from = g_file_new_for_path(src.c_str());
            GFile* to = g_file_new_for_path(dst.c_str());
            GError* err = nullptr;
            // Without OVERWRITE GIO refuses a name that is taken. A folder
            // only moves within one file system (a rename); across two GIO
            // says so, and nothing has been touched.
            const bool ok = g_file_move(from, to, G_FILE_COPY_NOFOLLOW_SYMLINKS,
                                        nullptr, nullptr, nullptr, &err);
            g_object_unref(from);
            g_object_unref(to);
            if (!ok) {
                if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS))
                    showTaken(app, {name}, dir, "Nothing was moved.");
                else
                    showError(app, "Could not move “" + name + "”", err ? err->message : "");
                g_clear_error(&err);
                return;
            }
            followMove(src, dst);
            app->tree->revealPath(dst);
        }
        return;
    }
    auto* job = new CopyJob;
    job->app = app;
    job->paths = paths;
    job->dir = dir;
    GTask* task = g_task_new(nullptr, nullptr, [](GObject*, GAsyncResult* res, gpointer) {
        auto* j = static_cast<CopyJob*>(g_task_get_task_data(G_TASK(res)));
        App* a = j->app;
        if (!a->dead) {
            if (!j->copied.empty()) a->tree->revealPath(j->copied.back());
            if (!j->taken.empty()) showTaken(a, j->taken, j->dir, "Nothing was copied over it.");
            if (!j->failed.empty()) showError(a, "Could not copy “" + j->failed + "”", j->why);
        }
    }, nullptr);
    g_task_set_task_data(task, job, [](gpointer p) { delete static_cast<CopyJob*>(p); });
    g_task_run_in_thread(task, [](GTask*, gpointer, gpointer data, GCancellable*) {
        auto* j = static_cast<CopyJob*>(data);
        for (const std::string& src : j->paths) {
            const std::string name = baseName(src);
            const std::string dst = j->dir + "/" + name;
            GFile* from = g_file_new_for_path(src.c_str());
            GFile* to = g_file_new_for_path(dst.c_str());
            GError* err = nullptr;
            struct stat st;
            if (lstat(dst.c_str(), &st) == 0) {
                j->taken.push_back(name);
            } else if (copyTree(from, to, &err)) {
                j->copied.push_back(dst);
            } else if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
                j->taken.push_back(name);
            } else if (j->failed.empty()) {
                j->failed = name;
                j->why = err ? err->message : "";
            }
            g_clear_error(&err);
            g_object_unref(from);
            g_object_unref(to);
        }
    });
    g_object_unref(task);
}

static void trashNow(App* app, const std::string& path) {
    GFile* f = g_file_new_for_path(path.c_str());
    GError* err = nullptr;
    const bool ok = g_file_trash(f, nullptr, &err);
    g_object_unref(f);
    if (!ok) {
        showError(app, "Could not move “" + baseName(path) + "” to the Trash",
                  err ? err->message : "");
        g_clear_error(&err);
        return;
    }
    // The tabs of what went close, in every window, unless they hold unsaved
    // changes: those stay open, and saving writes the file again (the Mac's
    // rule).
    for (App* a : g.windows) {
        a->editor->pathTrashed(path);
        updateTitle(a);
        refreshHints(a);
    }
    app->tree->clearSelection();
}

static void act_trash(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    const std::string path = app->tree->selectedPath();
    if (path.empty()) { gtk_widget_error_bell(app->window); return; }

    GtkAlertDialog* dlg = gtk_alert_dialog_new("Move “%s” to the Trash?",
                                               baseName(path).c_str());
    // The Mac asks the same question. A tab of it with unsaved changes stays
    // open with them, so say so.
    for (Document* d : app->editor->dirtyDocuments())
        if (isInside(d->path(), path)) {
            const std::string detail = "“" + baseName(d->path()) +
                "” has unsaved changes. Its tab stays open with them, and saving it "
                "writes the file again.";
            gtk_alert_dialog_set_detail(dlg, detail.c_str());
            break;
        }
    const char* buttons[] = {"Move to Trash", "Cancel", nullptr};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_cancel_button(dlg, 1);
    gtk_alert_dialog_set_default_button(dlg, 0);
    auto* target = new std::pair<App*, std::string>(app, path);
    gtk_alert_dialog_choose(dlg, GTK_WINDOW(app->window), nullptr,
        [](GObject* src, GAsyncResult* res, gpointer data) {
            auto* t = static_cast<std::pair<App*, std::string>*>(data);
            if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr) == 0 &&
                !t->first->dead)
                trashNow(t->first, t->second);
            delete t;
        }, target);
    g_object_unref(dlg);
}

// "Reveal in Finder" becomes opening the containing folder in the file
// manager, with the item selected where the file manager supports that.
static void act_reveal(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    std::string path = app->tree->selectedPath();
    if (path.empty()) path = app->rootDir;
    openContainingFolder(app, path);
}

// The selected item's absolute path, else the open folder's, as plain text.
static void act_copy_path(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    std::string path = app->tree->selectedPath();
    if (path.empty()) path = app->rootDir;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(app->window), path.c_str());
}

// Focus File Tree: the tree, or the Source Control panel's list while it is
// in the tree's place, as the Mac's Command 0.
static void act_focus_tree(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (app->gitShown) {
        if (!app->sidebarVisible) act_toggle_sidebar(nullptr, nullptr, app);
        app->git->focus();
        return;
    }
    app->tree->focus();
}

// A file's diff or a commit, in the tab diffs and commits share.
static void showDiffInEditor(App* app, const std::string& title, const std::string& bytes,
                             bool commit) {
    showEditorOverBrowser(app);
    app->editor->showDiff(title, bytes, commit);
    updateTitle(app);
    refreshHints(app);
}

// Ctrl+Shift+G: the Source Control panel takes the file tree's place in the
// sidebar and gives it back when pressed again, the Mac's
// toggleSourceControl:. The sidebar keeps its width, and a hidden one opens.
static void act_toggle_git(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (!app->git) {
        app->git = new GitPanel(app->rootDir);
        app->git->applySettings(g.settings->settings());
        app->git->onShowDiff = [app](const std::string& name, const std::string&,
                                     const std::string& diff) {
            if (!app->dead) showDiffInEditor(app, name + " (diff)", diff, false);
        };
        app->git->onShowCommit = [app](const std::string& title, const std::string& show) {
            if (!app->dead) showDiffInEditor(app, title, show, true);
        };
        gtk_stack_add_named(GTK_STACK(app->sidebar), app->git->widget(), "git");
    }
    app->gitShown = !app->gitShown;
    if (app->gitShown) {
        if (!app->sidebarVisible) act_toggle_sidebar(nullptr, nullptr, app);
        gtk_stack_set_visible_child(GTK_STACK(app->sidebar), app->git->widget());
        app->git->setActive(true);
        app->git->focus();
    } else {
        gtk_stack_set_visible_child(GTK_STACK(app->sidebar), app->tree->widget());
        app->git->setActive(false);
        app->tree->focus();
    }
    refreshHints(app);
}

// A Find in Folder match, a TODO or a definition: open the file (a single
// click on a result in the preview tab, a double-click or Enter kept, a
// definition's file in preview), then go to the match. If the file did not
// end up open (it could not be read), there is nowhere to go.
static void openMatch(App* app, const FolderSearchMatch& m, EditorTabs::OpenFrom from) {
    const std::string path = m.path;
    const int line = m.line;
    const std::size_t column = m.byteColumn, length = m.byteLength;
    openFileThen(app, path, from, [app, path, line, column, length] {
        if (app->editor->currentPath() == path)
            app->editor->revealLine(line, column, length);
        refreshHints(app);   // a Markdown preview may have switched to source
    });
    gtk_window_present(GTK_WINDOW(app->window));
}

static void openSearchMatch(const FolderSearchMatch& m, bool kept, void* userp) {
    openMatch(static_cast<App*>(userp), m,
              kept ? EditorTabs::OpenFrom::SearchOpen : EditorTabs::OpenFrom::SearchClick);
}

// Language servers (Lsp.h). Each acts on the editor, so only when it has the
// keyboard, like Ctrl+/.
static bool editorFocused(App* app) {
    GtkWidget* view = app->editor->textView();
    if (gtk_widget_has_focus(view)) return true;
    gtk_widget_error_bell(view);
    return false;
}

static void act_complete(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (editorFocused(app)) app->lsp->triggerCompletion();
}

static void act_definition(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (editorFocused(app)) app->lsp->goToDefinition();
}

static void act_hover(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (editorFocused(app)) app->lsp->showHoverInfo();
}

// Find in Folder: search the folder selected in the tree, or else the open
// folder, which is what the Mac's Shift+Cmd+F does.
static SearchPanel* findInFolderPanel(App* app) {
    if (!app->search) {
        app->search = new SearchPanel(GTK_WINDOW(app->window));
        app->search->setOpenCallback(openSearchMatch, app);
        // Replace in Folder looks at every window's tabs, and edits this
        // window's (Search.h).
        app->search->setEditors([] {
            std::vector<Editor*> all;
            for (App* a : g.windows)
                if (!a->dead && a->editor) all.push_back(a->editor);
            return all;
        }, app->editor);
    }
    app->search->setRoot(app->rootDir);   // resets the scope after Open Folder
    const std::string dir = app->tree->selectedDir();
    if (!dir.empty()) app->search->setScope(dir);
    return app->search;
}

static void act_find_in_folder(GSimpleAction*, GVariant*, gpointer userp) {
    findInFolderPanel(static_cast<App*>(userp))->show();
}

// Replace in Folder: the same window with its replace row, the keyboard in
// the replacement (the Mac's Option+Shift+Cmd+F).
static void act_replace_in_folder(GSimpleAction*, GVariant*, gpointer userp) {
    findInFolderPanel(static_cast<App*>(userp))->showReplace();
}

// TODOs in Folder: the window, scoped as Find in Folder is, and scanned
// again each time it comes up. A row opens like a Find in Folder match.
static void act_todos(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (!app->todos) {
        app->todos = new SearchPanel(GTK_WINDOW(app->window), SearchPanel::Mode::Todos);
        app->todos->setOpenCallback(openSearchMatch, app);
    }
    app->todos->setRoot(app->rootDir);
    const std::string dir = app->tree->selectedDir();
    if (!dir.empty()) app->todos->setScope(dir);
    app->todos->show();
}

// ---------------------------------------------------------- shortcut hints

// The macOS build floats a shortcut list over the top-right of the window
// (EditorController.mm -buildHintsPanelInContainer) and advertises it from the
// status bar, because nothing else in the UI tells you the bindings exist. This
// is that panel. As on the Mac, what each row does is written here and its key
// comes from the registry (Keys::label), so keys.conf moves it too; a command
// keys.conf took every key from shows "—". The panel is monospace, so the key
// column is as wide as its widest key.
namespace {
struct HintRow {
    enum Kind { Heading, Row, Text } kind;
    std::string key, text;
};
}  // namespace

static std::string hintsText(App* app) {
    std::vector<HintRow> rows;
    auto heading = [&](const std::string& title) { rows.push_back({HintRow::Heading, "", title}); };
    auto text = [&](const std::string& line) { rows.push_back({HintRow::Text, "", line}); };
    auto plain = [&](const std::string& key, const std::string& what) {
        rows.push_back({HintRow::Row, key, what});
    };
    // The command's key at `index` (the first by default); "—" for none, and
    // nothing at all for a second key it does not have.
    auto row = [&](const char* id, const std::string& what, size_t index = 0) {
        const std::string key = Keys::label(id, " ", index);
        if (key.empty() && index > 0) return;
        plain(key.empty() ? "—" : key, what);
    };

    heading("Keyboard Shortcuts");
    row("workbench.action.files.openFolder", "Open folder");
    row("workbench.action.files.save", "Save");
    row("actions.find", "Find in file");
    row("editor.action.startFindReplaceAction", "Replace in file");
    row("editor.action.nextMatchFindAction", "Find next");
    row("editor.action.previousMatchFindAction", "Find previous");
    row("editor.action.replaceAll", "Replace all");
    row("toggleFindCaseSensitive", "Match case");
    row("toggleFindWholeWord", "Match whole word");
    row("toggleFindRegex", "Regular expression");
    row("togglePreserveCase", "Preserve case");
    row("toggleFindInSelection", "Find in selection");
    row("workbench.action.findInFiles", "Find in folder");
    row("workbench.action.replaceInFiles", "Replace in folder");
    row("minicode.showTodos", "TODOs in folder");
    row("editor.action.commentLine", "Toggle comment");
    plain("Tab, Shift Tab", "Indent, outdent lines");
    row("minicode.sortLines", "Sort lines");
    row("minicode.sortKeepSortedBlocks", "Sort keep-sorted blocks");
    row("workbench.action.openSettings", "Settings");
    row("workbench.action.focusSideBar", "Focus the file tree");
    row("workbench.action.focusFirstEditorGroup", "Focus the editor");

    heading("Tabs");
    row("workbench.action.nextEditor", "Next tab");
    row("workbench.action.nextEditor", "Next tab, too", 1);
    row("workbench.action.previousEditor", "Previous tab");
    row("workbench.action.previousEditor", "Previous tab, too", 1);
    row("workbench.action.openEditorAtIndex1", "Tab 1 (and so on to 8)");
    row("workbench.action.lastEditorInGroup", "The last tab");
    row("workbench.action.closeActiveEditor", "Close tab");
    row("workbench.action.reopenClosedEditor", "Reopen closed tab");
    row("workbench.action.files.saveAll", "Save all");

    heading("Windows");
    row("workbench.action.newWindow", "New window");
    row("workbench.action.closeWindow", "Close window");
    row("workbench.action.quit", "Quit");

    heading("Files");
    row("explorer.newFile", "New file");
    row("explorer.newFolder", "New folder");
    row("renameFile", "Rename (in the tree)");
    row("moveFileToTrash", "Move to Trash (in the tree)");
    row("minicode.toggleHiddenFiles",
        std::string("Hidden files (") + (g.showHidden ? "shown" : "hidden") + ")");

    heading("Language servers");
    row("editor.action.triggerSuggest", "Complete");
    row("editor.action.revealDefinition", "Go to definition (or Ctrl click)");
    row("editor.action.showHover", "Hover info");

    heading("Panes");
    row("workbench.action.toggleSidebarVisibility", "Sidebar");
    row("minicode.toggleEditor",
        std::string("Editor      (") + (editorCollapsed(app) ? "collapsed" : "shown") + ")");
#ifdef MINICODE_ENABLE_TERMINAL
    row("workbench.action.terminal.toggleTerminal",
        std::string("Terminal    (") +
            (app->termPanel && gtk_widget_get_visible(app->termPanel) ? "open" : "hidden") + ")");
    row("workbench.action.terminal.toggleTerminal", "Terminal, too", 1);
#endif
#ifdef MINICODE_ENABLE_BROWSER
    row("minicode.toggleBrowser",
        std::string("Browser     (") +
            (app->browserRevealer &&
                     gtk_revealer_get_reveal_child(GTK_REVEALER(app->browserRevealer))
                 ? "open" : "hidden") + ")");
#endif
    row("workbench.view.scm",
        std::string("Git panel   (") + (app->gitShown ? "open" : "hidden") + ")");
    if (app->gitShown) {
        plain("Enter", "Show the diff or commit");
        plain("Space", "Stage or unstage");
        // Ctrl+Enter to commit is written in the message box itself.
        plain("Tab", "Changes, graph, message");
    }
    if (app->editor->isMarkdown()) {
        row("markdown.showPreview", std::string("Markdown    (") +
                                        (app->editor->inPreview() ? "rendered" : "source") + ")");
        if (app->editor->inPreview()) {
            plain("Click a box", "Check or clear a task");
        } else {
            row("minicode.markdown.toggleTask", "Toggle task");
            row("minicode.markdown.toggleTask", "Toggle task, too", 1);
        }
    }
    if (app->editor->isLatex()) {
        row("markdown.showPreview", std::string("LaTeX       (") +
                                        (app->editor->inPreview() ? "preview" : "source") + ")");
        row("minicode.latex.exportPDF", "Export PDF");
    }
    if (app->editor->showingPdf()) {
        row("minicode.page.zoomIn", "Zoom the PDF in (or Ctrl wheel)");
        row("minicode.page.zoomOut", "Zoom it out");
        row("minicode.page.fitWidth", "Fit width");
    }
    if (app->editor->canPlayMedia()) {
        heading("Video and audio");
        row("minicode.togglePlayback", "Play or pause");
    }

#ifdef MINICODE_ENABLE_TERMINAL
    heading("In the terminal");
    text("Plain Ctrl keys go to the program (Ctrl C,");
    text("Ctrl W, Ctrl H...). These still work:");
    plain("Ctrl Shift C", "Copy");
    plain("Ctrl Shift V", "Paste");
    plain("Ctrl click", "Open a file:line or URL");
    plain("Ctrl Shift …", "Ctrl Shift shortcuts, but");
    plain("", "G and Space (the shell's), and:");
    // The commands that keep a plain Ctrl key there (Global on Linux).
    for (const Keys::Action& a : Keys::actions()) {
        const Commands::Command* c = Commands::find(a.id);
        if (!c || !Commands::appliesIn(c->when, Commands::When::Terminal)) continue;
        for (const Commands::Key& k : Keys::keymap().keysOf(c->id))
            if (!(k.mods & (Commands::Shift | Commands::Alt)) &&
                !Commands::linuxTerminalTakes(*c, k))
                plain(Commands::linuxLabel(k), Commands::titleOf(*c, Commands::Platform::Linux));
    }
#endif

    rows.push_back({HintRow::Text, "", ""});
    row("minicode.toggleShortcutHints", "Hide these hints");

    size_t width = 0;
    for (const HintRow& r : rows)
        if (r.kind == HintRow::Row) width = std::max(width, (size_t)g_utf8_strlen(r.key.c_str(), -1));
    std::string s;
    for (const HintRow& r : rows) {
        switch (r.kind) {
        case HintRow::Heading:
            if (!s.empty()) s += "\n";
            s += r.text + "\n────────────────────────────────────────\n";
            break;
        case HintRow::Text:
            s += r.text + "\n";
            break;
        case HintRow::Row:
            s += r.key + std::string(width + 2 - (size_t)g_utf8_strlen(r.key.c_str(), -1), ' ') +
                 r.text + "\n";
            break;
        }
    }
    if (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
}
static GtkWidget* buildHintsPanel(App* app) {
    app->hintsLabel = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->hintsLabel), 0.0);
    // Room for the scrollbar, which is drawn over the content.
    gtk_widget_set_margin_end(app->hintsLabel, 10);

    // The list is taller than a laptop's window once the terminal's keys and a
    // file's own are in it, so it scrolls. The scrolled window asks for the
    // label's whole size, GtkOverlay never gives an overlay child more than the
    // overlay has, and what does not fit is a wheel turn away.
    app->hintsScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(app->hintsScroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(
        GTK_SCROLLED_WINDOW(app->hintsScroll), TRUE);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(app->hintsScroll), TRUE);
    gtk_widget_add_css_class(app->hintsScroll, "minicode-scroller");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(app->hintsScroll), app->hintsLabel);

    app->hintsPanel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(app->hintsPanel, "minicode-hints");
    gtk_box_append(GTK_BOX(app->hintsPanel), app->hintsScroll);

    // Float it in the top-right corner, the same corner macOS puts it in, and
    // keep it from stretching to fill the overlay. The bottom margin matters
    // only when the list is cut to fit.
    gtk_widget_set_halign(app->hintsPanel, GTK_ALIGN_END);
    gtk_widget_set_valign(app->hintsPanel, GTK_ALIGN_START);
    gtk_widget_set_margin_top(app->hintsPanel, 18);
    gtk_widget_set_margin_end(app->hintsPanel, 18);
    gtk_widget_set_margin_bottom(app->hintsPanel, 18);
    // It takes the pointer, which costs only clicks on text it already hides,
    // so that the wheel scrolls the list.
    gtk_widget_set_visible(app->hintsPanel, FALSE);
    return app->hintsPanel;
}

// Half the panel's value is reporting which panes are currently up, so a toggle
// while it is open has to redraw it. The macOS build refreshes from the same
// four places (EditorController.mm).
static void refreshHints(App* app) {
    if (app->hintsPanel && gtk_widget_get_visible(app->hintsPanel))
        gtk_label_set_text(GTK_LABEL(app->hintsLabel), hintsText(app).c_str());
}

static void act_toggle_hints(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    const bool showing = !gtk_widget_get_visible(app->hintsPanel);
    // Rebuilt on every open, not once at startup: half its value is reporting
    // which panels are currently up.
    if (showing) {
        gtk_label_set_text(GTK_LABEL(app->hintsLabel), hintsText(app).c_str());
        gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(
                                     GTK_SCROLLED_WINDOW(app->hintsScroll)), 0);
    }
    gtk_widget_set_visible(app->hintsPanel, showing);
    gtk_label_set_text(GTK_LABEL(app->hintsHint), hintsHintText(showing).c_str());
}

// ---------------------------------------------------------------- settings

// Problems in settings.conf, then in keys.conf, on the status bar: the first,
// with a count of the rest, and all of them in the tooltip, as on the Mac.
static void showConfigErrors(App* app) {
    std::vector<std::string> errors;
    for (const std::string& e : g.settings->errors()) errors.push_back("Settings " + e);
    for (const std::string& e : Keys::errors()) errors.push_back("Keys " + e);
    std::string note;
    if (!errors.empty()) {
        note = errors.front();
        if (errors.size() > 1)
            note += " (and " + std::to_string(errors.size() - 1) + " more)";
    }
    gtk_label_set_text(GTK_LABEL(app->settingsLabel), note.c_str());
    std::string tip;
    auto add = [&tip](const std::string& path, const std::vector<std::string>& list) {
        if (list.empty()) return;
        if (!tip.empty()) tip += "\n\n";
        tip += path;
        for (const std::string& e : list) tip += "\n" + e;
    };
    add(g.settings->path(), g.settings->errors());
    add(Keys::path(), Keys::errors());
    gtk_widget_set_tooltip_text(app->settingsLabel, tip.empty() ? nullptr : tip.c_str());
}

// One window's share of the settings: the editor's tags, the language
// servers, the terminal's colors, and the status-bar note about bad lines.
static void applyWindowSettings(App* app) {
    const Settings& st = g.settings->settings();
    app->editor->applySettings(st);
    app->findBar->applySettings(st);
    if (app->lsp) app->lsp->applySettings(st);   // restarts servers if lsp.* changed
    if (app->persist) app->persist->scheduleAutoSave();   // editor.auto-save may have changed
    if (app->git) app->git->applySettings(st);
#ifdef MINICODE_ENABLE_TERMINAL
    if (app->terminal) app->terminal->applySettings(st);
#endif
    showConfigErrors(app);
}

// The stylesheet, which is the display's and so shared by every window.
static void applyStylesheet() {
    if (!g.css) {
        g.css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(g.css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_string(g.css, theme::stylesheet(g.settings->settings()).c_str());
}

// The settings file changed on disk: every window follows.
static void onSettingsChanged(void*) {
    applyStylesheet();
    for (App* a : g.windows) applyWindowSettings(a);
}

// ---------------------------------------------------------------- keys
//
// The accelerators, the menu bar, the tree's own keys and the hints' key text
// all come from the command registry through Keys.h, with keys.conf applied.
// What stays here is telling Keys what has the keyboard, since GTK's
// accelerators are the application's and only the active window gets keys:
// while its terminal has the keyboard the plain Ctrl keys are the program's
// (Ctrl+W deletes a word, Ctrl+H is backspace), as the Mac's terminal gets
// Control because its shortcuts are on Command.

static App* appForWindow(GtkWindow* w) {
    for (App* a : g.windows)
        if (GTK_WINDOW(a->window) == w) return a;
    return nullptr;
}

static void updateKeyPlace() {
    Keys::Place place;
    if (App* a = appForWindow(gtk_application_get_active_window(g.gapp))) {
        if (!a->dead) {
            GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(a->window));
#ifdef MINICODE_ENABLE_TERMINAL
            place.terminal = a->terminal && a->terminal->owns(focus);
#endif
            place.editor = focus && focus == a->editor->textView();
            place.tree = Keys::within(focus, a->tree->widget());
            place.language = Commands::languageOfPath(a->editor->currentPath());
        }
    }
    Keys::setPlace(place);
}

static void onFocusWidget(GObject*, GParamSpec*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    updateKeyPlace();
    updateMediaKey(app);
    if (!app->dead && app->persist)   // focus-change auto-save
        app->persist->focusMoved(gtk_root_get_focus(GTK_ROOT(app->window)));
}

// Coming back to a window is when the Mac looks for changes made elsewhere
// (windowDidBecomeKey); the file monitor catches most of them sooner.
static void onIsActive(GObject* w, GParamSpec*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    updateKeyPlace();
    if (!app->dead && gtk_window_is_active(GTK_WINDOW(w))) {
        app->editor->checkExternalChange();
        if (app->git && app->gitShown) app->git->refresh();   // commits made elsewhere
    } else if (!app->dead && app->persist) {
        app->persist->windowActive(false);   // focus-change auto-save
    }
}

// keys.conf changed: the accelerators and the tree already follow it; the
// status bar's note on its errors and the hints follow here.
static void onKeysChanged() {
    for (App* a : g.windows) {
        showConfigErrors(a);
        gtk_label_set_text(GTK_LABEL(a->hintsHint),
                           hintsHintText(gtk_widget_get_visible(a->hintsPanel)).c_str());
        refreshHints(a);
    }
}

static void addAction(App* app, const char* name, GCallback cb) {
    GSimpleAction* a = g_simple_action_new(name, nullptr);
    g_signal_connect(a, "activate", cb, app);
    g_action_map_add_action(G_ACTION_MAP(app->window), G_ACTION(a));
    g_object_unref(a);   // the action map holds its own ref now
}

// What the application needs once, before its first window: the settings,
// the stylesheet, its own actions, the keys and the menu bar.
static void initShared() {
    if (g.settings) return;
    g.settings = new AppSettings();
    g.settings->setChangeCallback(onSettingsChanged, nullptr);
    applyStylesheet();   // before any widget is drawn

    GSimpleAction* quit = g_simple_action_new("quit", nullptr);
    g_signal_connect(quit, "activate", G_CALLBACK(act_quit), nullptr);
    g_action_map_add_action(G_ACTION_MAP(g.gapp), G_ACTION(quit));
    g_object_unref(quit);
    GSimpleAction* hidden = g_simple_action_new_stateful(
        "showhidden", nullptr, g_variant_new_boolean(g.showHidden));
    g_signal_connect(hidden, "change-state", G_CALLBACK(onShowHiddenChanged), nullptr);
    g_action_map_add_action(G_ACTION_MAP(g.gapp), G_ACTION(hidden));
    g_object_unref(hidden);

    Keys::init(g.gapp, onKeysChanged);
    GMenuModel* bar = Keys::menuBar();
    gtk_application_set_menubar(g.gapp, bar);
    g_object_unref(bar);
    // The tree's right-click menu, in the Mac's order and grouping.
    g.treeMenu = Keys::menuOf({"explorer.newFile", "explorer.newFolder", "", "renameFile",
                               "moveFileToTrash", "", "revealFileInOS", "copyFilePath"});
}

// ---------------------------------------------------------------- windows

// Take every signal handler that names one of `owners` off `w`, its event
// controllers and everything inside it.
static void disconnectOwners(GtkWidget* w, const std::vector<gpointer>& owners) {
    for (gpointer o : owners) g_signal_handlers_disconnect_by_data(w, o);
    GListModel* ctrls = gtk_widget_observe_controllers(w);
    for (guint i = 0; i < g_list_model_get_n_items(ctrls); ++i) {
        GObject* c = static_cast<GObject*>(g_list_model_get_item(ctrls, i));
        for (gpointer o : owners) g_signal_handlers_disconnect_by_data(c, o);
        g_object_unref(c);
    }
    g_object_unref(ctrls);
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        disconnectOwners(c, owners);
}

// GtkApplication lets go of a closing window before it disposes a single
// widget, so this is where the window is taken apart: its language servers
// are already stopping (onCloseRequest), tectonic and the file monitors stop
// here, the shell is not restarted, and every handler that names one of the
// window's objects comes off before GTK disposes the widgets, whose last
// signals would otherwise land in deleted objects.
static void onWindowRemoved(GtkApplication*, GtkWindow* window, gpointer) {
    App* app = appForWindow(window);
    if (!app) return;   // the Find in Folder window, an alert
    g.windows.erase(std::find(g.windows.begin(), g.windows.end(), app));
    app->dead = true;
    // Its tabs for the folder's next window, unless a quit saved every
    // window's already. Then nothing more is written for it.
    if (!g.quitting) app->persist->saveSessionClosing();
    delete app->persist;
    app->persist = nullptr;

    std::vector<gpointer> owners = {app, app->lsp, app->tree, app->git, app->findBar};
    for (void* o : app->editor->handlerOwners()) owners.push_back(o);
#ifdef MINICODE_ENABLE_TERMINAL
    owners.push_back(app->terminal);
#endif
#ifdef MINICODE_ENABLE_BROWSER
    owners.push_back(app->browser);
#endif
    // A part the window never made (the LaTeX preview before any .tex was
    // opened) is null, and disconnecting by null data would strip every
    // handler GTK itself connected without data, skipping its own cleanup.
    owners.erase(std::remove(owners.begin(), owners.end(), nullptr), owners.end());
    disconnectOwners(app->window, owners);

    app->editor->setObserver(nullptr);
    app->editor->setTitleCallback(nullptr, nullptr);
    app->editor->onCloseRequest = nullptr;
    app->editor->onTabCommand = nullptr;
    app->editor->onActiveChanged = nullptr;
    app->editor->onDocumentsChanged = nullptr;
    app->editor->setLinkHandler(nullptr);
    app->editor->setExternalChangeCallback(nullptr, nullptr);
    delete app->search;
    app->search = nullptr;
    delete app->todos;
    app->todos = nullptr;
    delete app->lsp;
    app->lsp = nullptr;
#ifdef MINICODE_ENABLE_TERMINAL
    delete app->terminal;
    app->terminal = nullptr;
#endif
#ifdef MINICODE_ENABLE_BROWSER
    delete app->browser;
    app->browser = nullptr;
#endif
    delete app->git;
    app->git = nullptr;
    delete app->tree;
    app->tree = nullptr;
    delete app->findBar;   // before the editor, whose view it follows
    app->findBar = nullptr;
    delete app->editor;   // its tabs' backups go with them (Document::~Document)
    app->editor = nullptr;
    BackupStore::shared().flush();   // gone from disk before anything can end the process
    // The App itself is kept (see App::dead).
    updateKeyPlace();
}

// Build a window rooted at `root`, showing `file` if one is named.
static App* newWindow(const std::string& root, const std::string& file) {
    initShared();
    App* app = new App;
    app->rootDir = root;
    g.windows.push_back(app);

    app->window = gtk_application_window_new(g.gapp);
    gtk_widget_add_css_class(app->window, "minicode-window");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1100, 720);
    gtk_window_set_title(GTK_WINDOW(app->window), "MiniCode");
    gtk_application_window_set_show_menubar(
        GTK_APPLICATION_WINDOW(app->window), TRUE);

    // Core widgets.
    app->tree = new FileTree(app->rootDir, g.showHidden);
    app->tree->setOpenCallback(treeOpenCb, app);
    app->tree->setDropCallback([app](const std::vector<std::string>& paths,
                                     const std::string& dir, bool move) {
        if (!app->dead) dropOnTree(app, paths, dir, move);
    });
    // The tree's folder monitors see changes in the work tree; the Source
    // Control panel looks again when it is showing.
    app->tree->setActivityCallback([app] {
        if (!app->dead && app->git && app->gitShown) app->git->refresh();
    });
    app->editor = new Editor();
    app->editor->setTitleCallback(updateTitle, app);
    app->editor->setLinkHandler([app](const Editor::Link& link) {
        if (!app->dead) openEditorLink(app, link);
    });
    app->editor->setExternalChangeCallback(askExternalChange, app);
    app->editor->setSettingsPath(g.settings->path());
    app->editor->onCloseRequest = [app](Document* d) {
        if (!app->dead) closeTabAsking(app, d);
    };
    app->editor->onTabCommand = [app](const std::string& name, Document* d) {
        if (!app->dead) tabCommand(app, name, d);
    };
    app->editor->onActiveChanged = [app] {
        if (app->dead) return;
        revealActiveInTree(app);
        refreshHints(app);
        if (app->persist) app->persist->tabShown();
    };
    // The window's tabs between runs, their backups, and auto-save.
    app->persist = new Persistence(app->editor, app->rootDir, Persistence::Host{
        [app](const std::string& note) {
            if (!app->dead) flashStatus(app, note);
        },
        [app]() {
            std::vector<Persistence*> out;
            for (App* a : g.windows)
                if (a != app && !a->dead && a->persist && a->persist->key() == app->persist->key())
                    out.push_back(a->persist);
            return out;
        }});
    app->editor->onDocumentsChanged = [app](Document* d) {
        if (!app->dead && app->persist) app->persist->changed(d);
    };

    // The find and replace bar hangs over the editor's text (FindBar.h).
    app->findBar = new FindBar(app->editor);
    app->findBar->onNote = [app](const std::string& note) {
        if (!app->dead) flashStatus(app, note);
    };

    // Right side: editor + collapsible panels.
    GtkWidget* rightBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    // Order matters: the browser sits with the editor in the upper area and the
    // terminal is docked below both, the same arrangement the macOS build lays
    // out by hand in -relayoutRightArea.
    GtkWidget* upperBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    app->upperBox = upperBox;
    gtk_box_append(GTK_BOX(upperBox), app->editor->widget());
#ifdef MINICODE_ENABLE_BROWSER
    app->browser = new Browser(MINICODE_BROWSER_HOME);
    app->browserRevealer = gtk_revealer_new();
    gtk_revealer_set_child(GTK_REVEALER(app->browserRevealer),
                           app->browser->widget());
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->browserRevealer), FALSE);
    gtk_widget_set_vexpand(app->browserRevealer, FALSE);  // see act_toggle_browser
    gtk_box_append(GTK_BOX(upperBox), app->browserRevealer);
#endif

    app->vpaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class(app->vpaned, "minicode-split");
    gtk_paned_set_start_child(GTK_PANED(app->vpaned), upperBox);
    // The editor absorbs window resizes; the terminal keeps the height it was
    // dragged to. Neither may be shrunk past its minimum, which is what stops a
    // drag from collapsing the terminal into an unusable sliver.
    gtk_paned_set_resize_start_child(GTK_PANED(app->vpaned), TRUE);
    // The editor may be dragged all the way shut; onVpanedPosition snaps a
    // near-miss the rest of the way so no sliver is left.
    gtk_paned_set_shrink_start_child(GTK_PANED(app->vpaned), TRUE);
    g_signal_connect(app->vpaned, "notify::position",
                     G_CALLBACK(onVpanedPosition), app);
    gtk_widget_set_vexpand(app->vpaned, TRUE);
#ifdef MINICODE_ENABLE_TERMINAL
    app->terminal = new Terminal(app->rootDir);
    app->termPanel = app->terminal->widget();
    app->terminal->setProjectRoot(app->rootDir);
    app->terminal->setLinkHandler([app](const Terminal::Link& link) {
        if (!app->dead) openTerminalLink(app, link);
    });
    gtk_paned_set_end_child(GTK_PANED(app->vpaned), app->termPanel);
    gtk_paned_set_resize_end_child(GTK_PANED(app->vpaned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(app->vpaned), FALSE);
    gtk_widget_set_visible(app->termPanel, FALSE);   // opens on Ctrl+Shift+T
#endif
    gtk_box_append(GTK_BOX(rightBox), app->vpaned);

    // Sidebar | editor split.
    app->hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(app->hpaned, "minicode-split");
    app->sidebar = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(app->sidebar), app->tree->widget(), "tree");
    gtk_paned_set_start_child(GTK_PANED(app->hpaned), app->sidebar);
    gtk_paned_set_end_child(GTK_PANED(app->hpaned), rightBox);
    gtk_paned_set_position(GTK_PANED(app->hpaned), 240);
    gtk_paned_set_resize_start_child(GTK_PANED(app->hpaned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(app->hpaned), TRUE);
    g_signal_connect(app->hpaned, "notify::position",
                     G_CALLBACK(onHpanedPosition), app);
    GtkGesture* hpanedPress = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(hpanedPress),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(hpanedPress, "pressed", G_CALLBACK(onHpanedPressed), app);
    gtk_widget_add_controller(app->hpaned, GTK_EVENT_CONTROLLER(hpanedPress));
    gtk_widget_set_vexpand(app->hpaned, TRUE);

    // Status bar: the current path on the left, and the hint that advertises the
    // shortcuts panel on the right. The macOS build carries the same pair, and
    // that right-hand label is the only thing that makes the panel discoverable.
    app->statusLabel = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(app->statusLabel), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(app->statusLabel), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(app->statusLabel, TRUE);

    app->settingsLabel = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(app->settingsLabel), PANGO_ELLIPSIZE_END);

    app->hintsHint = gtk_label_new(hintsHintText(false).c_str());
    gtk_label_set_xalign(GTK_LABEL(app->hintsHint), 1.0);

    GtkWidget* statusBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(statusBar, "minicode-status");
    gtk_box_append(GTK_BOX(statusBar), app->statusLabel);
    app->lspLabel = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(app->lspLabel), PANGO_ELLIPSIZE_START);
    gtk_box_append(GTK_BOX(statusBar), app->lspLabel);
    gtk_box_append(GTK_BOX(statusBar), app->settingsLabel);
    gtk_box_append(GTK_BOX(statusBar), app->hintsHint);

    // The hints panel floats over the panes rather than displacing them, so it
    // needs a GtkOverlay. The status bar stays outside it: a long list then
    // stops above the bar, and the bar's "Hide shortcuts" stays in sight.
    GtkWidget* overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), app->hpaned);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), buildHintsPanel(app));
    gtk_widget_set_vexpand(overlay, TRUE);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(vbox), overlay);
    gtk_box_append(GTK_BOX(vbox), statusBar);
    gtk_window_set_child(GTK_WINDOW(app->window), vbox);

    // Language servers: one session for the window, told about every file the
    // editor opens, saves and edits through the editor's observer hook.
    app->lsp = new LspSession(app->editor, app->rootDir, g.settings->settings());
    app->lsp->onStatus = [app](const std::string& text) {
        gtk_label_set_text(GTK_LABEL(app->lspLabel), text.c_str());
    };
    app->lsp->onReveal = [app](const std::string& path, int line, std::size_t col,
                               std::size_t len) {
        FolderSearchMatch m;
        m.path = path;
        m.line = line;
        m.byteColumn = col;
        m.byteLength = len;
        openMatch(app, m, EditorTabs::OpenFrom::CodeNavigation);
    };
    app->editor->setObserver(app->lsp);   // onCloseRequest shuts it down

    // Actions. The menu bar and the accelerators are the application's
    // (initShared); these are what they name in this window.
    addAction(app, "newwindow",      G_CALLBACK(act_new_window));
    addAction(app, "close",          G_CALLBACK(act_close_window));
    addAction(app, "complete",       G_CALLBACK(act_complete));
    addAction(app, "definition",     G_CALLBACK(act_definition));
    addAction(app, "hoverinfo",      G_CALLBACK(act_hover));
    addAction(app, "open",           G_CALLBACK(act_open));
    addAction(app, "save",           G_CALLBACK(act_save));
    addAction(app, "exportpdf",      G_CALLBACK(act_export_pdf));
    addAction(app, "newfile",        G_CALLBACK(act_new_file));
    addAction(app, "newfolder",      G_CALLBACK(act_new_folder));
    addAction(app, "rename",         G_CALLBACK(act_rename));
    addAction(app, "trash",          G_CALLBACK(act_trash));
    addAction(app, "reveal",         G_CALLBACK(act_reveal));
    addAction(app, "copypath",       G_CALLBACK(act_copy_path));
    addAction(app, "find",           G_CALLBACK(act_find));
    addAction(app, "replace",        G_CALLBACK(act_replace));
    addAction(app, "findnext",       G_CALLBACK(act_find_next));
    addAction(app, "findprevious",   G_CALLBACK(act_find_previous));
    addAction(app, "replaceone",     G_CALLBACK(act_replace_one));
    addAction(app, "replaceall",     G_CALLBACK(act_replace_all));
    addAction(app, "findcase",       G_CALLBACK(act_find_case));
    addAction(app, "findword",       G_CALLBACK(act_find_word));
    addAction(app, "findregex",      G_CALLBACK(act_find_regex));
    addAction(app, "findpreservecase", G_CALLBACK(act_find_preserve));
    addAction(app, "findinselection", G_CALLBACK(act_find_in_selection));
    addAction(app, "findinfolder",   G_CALLBACK(act_find_in_folder));
    addAction(app, "replaceinfolder", G_CALLBACK(act_replace_in_folder));
    addAction(app, "togglepreview",  G_CALLBACK(act_toggle_preview));
    addAction(app, "togglehints",    G_CALLBACK(act_toggle_hints));
    addAction(app, "togglesidebar",  G_CALLBACK(act_toggle_sidebar));
    addAction(app, "toggleeditor",   G_CALLBACK(act_toggle_editor));
    addAction(app, "toggleterminal", G_CALLBACK(act_toggle_terminal));
    addAction(app, "togglebrowser",  G_CALLBACK(act_toggle_browser));
    addAction(app, "togglegit",      G_CALLBACK(act_toggle_git));
    addAction(app, "focustree",      G_CALLBACK(act_focus_tree));
    addAction(app, "focuseditor",    G_CALLBACK(act_focus_editor));
    addAction(app, "saveall",        G_CALLBACK(act_save_all));
    addAction(app, "copyactivepath", G_CALLBACK(act_copy_active_path));
    addAction(app, "revealactive",   G_CALLBACK(act_reveal_active));
    addAction(app, "revealactiveintree", G_CALLBACK(act_reveal_active_in_tree));
    addAction(app, "closetab",       G_CALLBACK(act_close_tab));
    addAction(app, "reopentab",      G_CALLBACK(act_reopen_tab));
    addAction(app, "closeothertabs", G_CALLBACK(act_close_other_tabs));
    addAction(app, "closetabsright", G_CALLBACK(act_close_tabs_right));
    addAction(app, "closealltabs",   G_CALLBACK(act_close_all_tabs));
    addAction(app, "keeptab",        G_CALLBACK(act_keep_tab));
    addAction(app, "nexttab",        G_CALLBACK(act_next_tab));
    addAction(app, "previoustab",    G_CALLBACK(act_previous_tab));
    addAction(app, "tab1",           G_CALLBACK(&act_go_to_tab<1>));
    addAction(app, "tab2",           G_CALLBACK(&act_go_to_tab<2>));
    addAction(app, "tab3",           G_CALLBACK(&act_go_to_tab<3>));
    addAction(app, "tab4",           G_CALLBACK(&act_go_to_tab<4>));
    addAction(app, "tab5",           G_CALLBACK(&act_go_to_tab<5>));
    addAction(app, "tab6",           G_CALLBACK(&act_go_to_tab<6>));
    addAction(app, "tab7",           G_CALLBACK(&act_go_to_tab<7>));
    addAction(app, "tab8",           G_CALLBACK(&act_go_to_tab<8>));
    addAction(app, "lasttab",        G_CALLBACK(&act_go_to_tab<9>));
    addAction(app, "togglecomment",  G_CALLBACK(act_toggle_comment));
    addAction(app, "toggletask",     G_CALLBACK(act_toggle_task));
    addAction(app, "sortlines",      G_CALLBACK(act_sort_lines));
    addAction(app, "fixkeepsorted",  G_CALLBACK(act_fix_keep_sorted));
    addAction(app, "todos",          G_CALLBACK(act_todos));
    addAction(app, "settings",       G_CALLBACK(act_settings));
    addAction(app, "keyboardshortcuts", G_CALLBACK(act_keyboard_shortcuts));
    addAction(app, "zoomin",         G_CALLBACK(act_zoom_in));
    addAction(app, "zoomout",        G_CALLBACK(act_zoom_out));
    addAction(app, "zoomfit",        G_CALLBACK(act_zoom_fit));
    addAction(app, "playpause",      G_CALLBACK(act_play_pause));
    for (const char* off : {"zoomin", "zoomout", "zoomfit", "playpause"})   // until shown
        g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(
                                        G_ACTION_MAP(app->window), off)), FALSE);
    app->tree->setContextMenu(g.treeMenu);
    // Rename and Move to Trash (F2 and Delete) only while the tree has the
    // keyboard. As window accelerators they would take Delete from the editor.
    Keys::addTreeKeys(app->tree->widget());
    g_signal_connect(app->window, "close-request", G_CALLBACK(onCloseRequest), app);
    g_signal_connect(app->window, "notify::focus-widget", G_CALLBACK(onFocusWidget), app);
    g_signal_connect(app->window, "notify::is-active", G_CALLBACK(onIsActive), app);
    applyWindowSettings(app);   // now that the editor, terminal and status bar exist
    app->persist->restoreSession();   // the files open here last time

    // A file named on the command line opens before the window is shown, so it
    // is already rendered when the window appears rather than flashing the
    // welcome text first. Markdown arrives rendered, the same as a click in the
    // sidebar would give (Editor::openFile decides that).
    if (!file.empty()) openFileCb(file, app);

    gtk_window_present(GTK_WINDOW(app->window));
    return app;
}

// After the windows the first launch opens: a window for every folder whose
// unsaved buffers outlived a crash, a SIGKILL or a power loss, with no
// running window holding them (the Mac's reopenFoldersWithBackups), and the
// launch's own window brought back to the front.
static void reopenFoldersWithBackups() {
    static bool done = false;
    if (done) return;
    done = true;
    std::vector<std::string> open;
    for (App* a : g.windows)
        if (!a->dead) open.push_back(a->persist->key());
    App* front = g.windows.empty() ? nullptr : g.windows.back();
    bool opened = false;
    for (const std::string& root : BackupStore::shared().foldersToReopenExcept(open)) {
        // A folder no longer there: its backups wait, and go after 30 days.
        if (!g_file_test(root.c_str(), G_FILE_TEST_IS_DIR)) continue;
        newWindow(root, "");
        opened = true;
    }
    if (opened && front && !front->dead) gtk_window_present(GTK_WINDOW(front->window));
}

// ---------------------------------------------------------------- main

namespace {
// What the command line asked for.
struct Startup {
    std::string root;    // the folder for the tree
    std::string file;    // a file to show, or ""
    std::string error;   // for the user's terminal, or ""
};
}  // namespace

// Work out what to open from argv[1]. It may be a directory, or a single file:
// `minicode notes.md` should show that file, not make the user name its folder.
// A file roots the tree at its parent, so the sidebar still lists the files
// alongside it and New File / New Folder land somewhere sensible.
//
// The path is canonicalized because the tree, the terminal's working directory
// and the editor all keep it, and a relative "." or "../thing" would otherwise
// be re-resolved against whatever the process's cwd happened to be later on.
// `cwd` is passed in rather than read here because a second `minicode foo.md`
// is answered by the already-running process, and the path has to be resolved
// against the directory the user typed it in, not that process's cwd.
// The error is returned rather than printed because the caller may be a
// remote invocation, whose output has to be sent back over the command line
// object to reach the terminal the user actually typed in.
static Startup resolveStartupPath(const char* arg, const char* cwdIn) {
    char cwdBuf[PATH_MAX];
    const std::string cwd = (cwdIn && *cwdIn) ? cwdIn
        : (getcwd(cwdBuf, sizeof(cwdBuf)) ? cwdBuf : ".");

    Startup out;
    if (!arg || !*arg) { out.root = cwd; return out; }

    char* canon = g_canonicalize_filename(arg, cwd.c_str());
    const std::string path = canon ? canon : arg;
    g_free(canon);

    if (g_file_test(path.c_str(), G_FILE_TEST_IS_DIR)) {
        out.root = path;
        return out;
    }

    if (g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) {
        char* parent = g_path_get_dirname(path.c_str());
        out.root = parent ? parent : cwd;
        g_free(parent);
        out.file = path;
        return out;
    }

    // Neither a directory nor an existing file. Root the tree at the parent if
    // that at least exists, so a typo in the filename still lands in the right
    // folder instead of dumping the user somewhere unrelated.
    char* parent = g_path_get_dirname(path.c_str());
    const bool parentOk = parent && g_file_test(parent, G_FILE_TEST_IS_DIR);
    out.root = parentOk ? parent : cwd;
    g_free(parent);
    out.error = std::string("minicode: ") + arg + ": no such file or directory";
    return out;
}

// Handles both the first launch and every later `minicode <path>` run while
// MiniCode is already open. GApplication is single-instance: the second
// process hands its arguments to this one over D-Bus and exits, so this is
// where every window a command line asks for is made.
//
// A second invocation opens a new window on what it names, which is what the
// Mac's command-line launcher gives (it starts a separate process there), and
// what GNOME's "New Window" in the dock does, since that runs the desktop
// entry again. If a window already has that folder open, it is raised
// instead, and a named file opens in it. Before windows, this re-rooted the
// only window on the new folder, open file and all.
//
// HANDLES_COMMAND_LINE (rather than HANDLES_OPEN) is what lets us keep parsing
// argv[1] ourselves: it can be a directory or a file, and GApplication must not
// guess which.
static int onCommandLine(GApplication*, GApplicationCommandLine* cl, gpointer) {
    int n = 0;
    char** args = g_application_command_line_get_arguments(cl, &n);
    const Startup st = resolveStartupPath(n > 1 ? args[1] : nullptr,
                                          g_application_command_line_get_cwd(cl));
    g_strfreev(args);

    // printerr on the command line object, not g_printerr: for a second
    // `minicode <path>` this routes the message back to the shell that ran it
    // instead of the stderr of the process that happens to own the window.
    if (!st.error.empty()) g_application_command_line_printerr(cl, "%s\n", st.error.c_str());

    for (App* a : g.windows) {
        if (a->rootDir != st.root) continue;
        if (!st.file.empty()) openFileCb(st.file, a);
        gtk_window_present(GTK_WINDOW(a->window));
        return 0;
    }
    newWindow(st.root, st.file);
    reopenFoldersWithBackups();   // once, at the first launch
    return 0;
}

int main(int argc, char** argv) {
    GtkApplication* gapp = gtk_application_new("org.minicode.Editor",
                                              G_APPLICATION_HANDLES_COMMAND_LINE);
    g.gapp = gapp;
    // argv[1] = a directory to open, or a file to open; default = cwd. The real
    // argc/argv go to g_application_run so that a remote invocation forwards
    // them to the running instance; onCommandLine is where they are read.
    g_signal_connect(gapp, "command-line", G_CALLBACK(onCommandLine), nullptr);
    g_signal_connect(gapp, "window-removed", G_CALLBACK(onWindowRemoved), nullptr);
    // Quitting: no language server may outlive the app. The main loop has
    // stopped by now, so this waits on the processes directly.
    g_signal_connect(gapp, "shutdown", G_CALLBACK(+[](GApplication*, gpointer) {
        LspTerminateAllServers();
        BackupStore::shared().flush();   // what the last windows let go is gone from disk
    }), nullptr);
    int status = g_application_run(G_APPLICATION(gapp), argc, argv);
    g_object_unref(gapp);
    return status;
}

// What the GUI checks may reach of this file (linux/tests/gui/AppSeam.h),
// compiled only into minicode-guitest.
#ifdef MINICODE_GUITEST
#include "../tests/gui/AppSeam.inc"
#endif
