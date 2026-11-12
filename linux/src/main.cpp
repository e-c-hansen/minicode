// main.cpp — MiniCode GTK4 shell.
//
// Layout (mirrors the macOS window: file tree left, editor right, status bar
// at the bottom, collapsible terminal and browser panels):
//
//   GtkApplicationWindow
//     vbox
//       overlay  (the shortcut list floats over the panes)
//         WindowLayout (WindowLayout.h): a GtkPaned
//           [start] sidebar stack: FileTree, or the Source Control panel
//                   (GitPanel) in its place
//           [end]   the right side, laid out as the Mac's relayoutRightArea:
//                     the editor's side (the Editor: its tab strip and slot,
//                     the find bar (FindBar) over its text) at the top left
//                     the browser at the top right, a bar between them
//                                         (#ifdef MINICODE_ENABLE_BROWSER)
//                     the terminal below both, full width, a bar above it
//                                         (#ifdef MINICODE_ENABLE_TERMINAL)
//       status bar (GtkLabel, VS Code blue)
//
// Toggle Editor, Toggle Browser, Toggle Terminal and Toggle Sidebar each hide
// or show their own pane; with the editor, the browser and the terminal all
// hidden the file tree has the window, and nothing comes back until asked
// (WindowLayout::collapseRightAreaIfEmpty). Opening a file and the other ways
// of putting one in front of the user call WindowLayout::revealEditor.
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
#include "Appearance.h"
#include "BracketView.h"
#include "Carets.h"
#include "ConsentBar.h"
#include "Detach.h"
#include "Document.h"
#include "Editor.h"
#include "FileTree.h"
#include "Folds.h"
#include "ThemeCss.h"
#include "Palette.h"
#include "Terminal.h"
#include "TerminalDock.h"
#include "TerminalAttention.h"
#include "ProcessMonitor.h"
#include "AgentsPanel.h"
#include "EvidencePage.h"
#include "AgentWorktrees.h"
#include "WorktreeChangesPage.h"
#include "RunFile.h"
#include "ProcessesPanel.h"
#include "IdeLinkServer.h"
#include "Browser.h"
#include "Search.h"
#include "FindBar.h"
#include "FolderConsent.h"
#include "Formatting.h"
#include "TreeGitColors.h"
#include "Lsp.h"
#include "AgentNotes.h"
#include "GitGutter.h"
#include "GitPanel.h"
#include "InlineAgentBar.h"
#include "Keys.h"
#include "MenuBarFocus.h"
#include "TabStrip.h"
#include "WindowLayout.h"
#include "BackupStore.h"
#include "Persistence.h"
#include "PalettePanel.h"
#include "ProblemsPanel.h"
#include "NavHistoryUi.h"
#include "Zoom.h"
#include "ControlCommands.h"
#include "ControlServer.h"
#include "StartupLog.h"

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
    int             controlId = 0;             // what the control channel calls this window
    GtkWidget*      window = nullptr;
    GtkWidget*      statusLabel = nullptr;
    GtkWidget*      settingsLabel = nullptr;   // first problem in the settings file
    GtkWidget*      statusBar = nullptr;       // the bar holding both

    // The panes: the sidebar beside the editor, browser and terminal, each
    // shown or hidden there (WindowLayout.h).
    WindowLayout* layout = nullptr;

    Editor*   editor = nullptr;
    FindBar*  findBar = nullptr;     // the editor's find and replace bar
    FileTree* tree = nullptr;
    // The sidebar is a stack: the file tree, or the Source Control panel in
    // its place, made on first use.
    GtkWidget* sidebar = nullptr;
    GitPanel*  git = nullptr;
    bool       gitShown = false;
    // The Processes panel takes the same place (View > Toggle Processes).
    ProcessesPanel* procs = nullptr;
    IdeLinkServer*  ide = nullptr;   // Claude Code's /ide (agent.ide-link), PARITY.md 3.9
    // The Agents panel takes the same place (View > Toggle Agents).
    AgentsPanel* agents = nullptr;
    AgentWorktrees* worktrees = nullptr;   // the file tree's Worktrees node, Compare with Main
    GMenu*       worktreeMenu = nullptr;
    bool       agentsShown = false;
    bool       procsShown = false;
    // The editor's change bars and the tree's git colors (PARITY.md 2.10).
    GitGutter*     gitGutter = nullptr;
    TreeGitColors* treeColors = nullptr;
    SearchPanel* search = nullptr;   // Find in Folder, created on first use
    SearchPanel* todos = nullptr;    // the TODOs window, the same
    LspSession*  lsp = nullptr;      // language servers for this window
    // Its tabs between runs, the backups of its unsaved tabs, and auto-save.
    Persistence* persist = nullptr;
    GtkWidget*   lspLabel = nullptr; // their status, in the status bar
    PalettePanel* palette = nullptr; // the palette, over the panes (PalettePanel.h)
    NavHistoryUi* nav = nullptr;     // Go Back and Go Forward (NavHistoryUi.h)
    int zoomListener = 0;            // the zoom and word wrap's (Zoom.h)
    // Format Document (Formatting.h), and the bar under the tabs that asks
    // about the folder's own tools for the servers (ConsentBar.h), with the
    // folder's answer's listener (Ask Again's enabled state).
    Formatting* format = nullptr;
    ConsentBar* consentBar = nullptr;
    int consentListener = 0;
    // The Problems list above the terminal and its counts in the status bar
    // (ProblemsPanel.h, PARITY.md 2.2).
    ProblemsPanel* problems = nullptr;

#ifdef MINICODE_ENABLE_TERMINAL
    // The terminals' tabs (PARITY.md 4.7); `terminal` is always the active
    // one, as the Mac's `terminal` getter, so everything that means "the
    // terminal" keeps working.
    TerminalDock* termDock = nullptr;
    Terminal* terminal = nullptr;
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

// The status bar's note on the shortcut hints, with their key as keys.conf
// leaves it.
static std::string hintsHintText(bool showing) {
    const std::string key = Keys::label("minicode.toggleShortcutHints", "+");
    return (key.empty() ? std::string() : key + "  ") +
           (showing ? "Hide shortcuts" : "Shortcuts");
}

// The editor shown, and the right side opened if it was collapsed: anything
// that puts a file in front of the user calls this first (the Mac's
// revealEditor), so opening one never seems to do nothing. The browser and
// the terminal stay as they are.
static void showEditorArea(App* app) {
    app->layout->revealEditor();
}

// ---------------------------------------------------------------- helpers

// The browser's page or its address bar has the keyboard: Page Zoom's keys
// are the page's then (PARITY.md 1.11).
static bool browserHasKeyboard(App* app) {
#ifdef MINICODE_ENABLE_BROWSER
    if (!app->dead && app->browser && app->layout && app->layout->browserShown())
        return Keys::within(gtk_root_get_focus(GTK_ROOT(app->window)), app->browser->widget());
#else
    (void)app;
#endif
    return false;
}

// The page zoom keys act on a PDF or the LaTeX preview on screen, or on the
// browser's page while it has the keyboard; disabled otherwise, their keys
// go on to whatever has the focus (and the editor's zoom has them).
static void updatePageZoom(App* app) {
    if (app->dead || !app->editor) return;
    const bool on = app->editor->showingPdf() || browserHasKeyboard(app);
    for (const char* zoom : {"zoomin", "zoomout", "zoomfit"})
        if (GAction* z = g_action_map_lookup_action(G_ACTION_MAP(app->window), zoom))
            g_simple_action_set_enabled(G_SIMPLE_ACTION(z), on);
}

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
    // Claude Code sessions waiting for the user, in any window (PARITY.md 3.6):
    // the count leads the title, which the task switcher shows (GTK4 has no
    // Dock badge or urgency hint to set).
    if (int waiting = TerminalAttention::waitingCount())
        title = "(" + std::to_string(waiting) + " waiting) " + title;
    gtk_window_set_title(GTK_WINDOW(app->window), title.c_str());
    // Nothing to save while an image, a PDF or a binary file is shown.
    if (GAction* save = g_action_map_lookup_action(G_ACTION_MAP(app->window), "save"))
        g_simple_action_set_enabled(G_SIMPLE_ACTION(save), app->editor->canSave());
    if (GAction* exp = g_action_map_lookup_action(G_ACTION_MAP(app->window), "exportpdf"))
        g_simple_action_set_enabled(G_SIMPLE_ACTION(exp), app->editor->isLatex());
    if (GAction* exh = g_action_map_lookup_action(G_ACTION_MAP(app->window), "exporthtml"))
        g_simple_action_set_enabled(G_SIMPLE_ACTION(exh), app->editor->isMarkdown());
    if (GAction* syn = g_action_map_lookup_action(G_ACTION_MAP(app->window), "latexsynctex"))
        g_simple_action_set_enabled(G_SIMPLE_ACTION(syn), app->editor->isLatex());
    // Format Document acts on a file's editable source only.
    if (GAction* fmt = g_action_map_lookup_action(G_ACTION_MAP(app->window), "formatdocument"))
        g_simple_action_set_enabled(G_SIMPLE_ACTION(fmt), app->format && app->format->canFormat());
    updatePageZoom(app);
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
#ifdef MINICODE_ENABLE_TERMINAL
static std::vector<std::string> sessionRootsOf(App* app);
static void openTerminalLink(App* app, const Terminal::Link& link);
#endif

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
    // The tree watches only the folders it listed (TreeGitColors.h).
    if (ok && app->treeColors) app->treeColors->changedOnDisk(d->path());
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
    if (app->dead || !d || !app->editor->isOpen(d)) { finish(false); return; }
    // With auto-save on, what it would write once the tab goes is written
    // first, so it is not asked about.
    if (d->dirty()) app->persist->autoSaveNow();
    // Still open in the other editor group, nothing is lost: no question.
    if (!d->dirty() || app->editor->inBothGroups(d)) {
        app->editor->closeTab(d);
        refreshHints(app);
        finish(true);
        return;
    }
    const std::string name = baseName(d->path());
    askUnsaved(app, "Save changes to “" + (name.empty() ? std::string("this file") : name) + "”?",
               kLostDetail, false, [app, d, finish](int choice) {
        // Closed meanwhile, or not to be closed.
        if (!app->editor->isOpen(d) || choice == 2) { finish(false); return; }
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
            if (choice == 0 && app->editor->isOpen(d) && !saveDocument(app, d)) {
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
            // Auto-save may write the tab again: Reload's text is the disk's,
            // and Keep Mine's goes over the version it was asked about.
            a->editor->diskQuestionAnswered();
            a->persist->scheduleAutoSave();
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
    if (app->treeColors) app->treeColors->setRoot(dir);
    if (app->git) app->git->setRoot(dir);   // the panel follows the folder
    app->editor->closeAllTabs();
    app->nav->clear();   // Go Back's places were the old folder's
    // After the tabs close, so no old file is reopened under the new root.
    if (app->lsp) app->lsp->setRoot(dir);   // servers belong to a folder
#ifdef MINICODE_ENABLE_TERMINAL
    if (app->termDock)
        for (Terminal* t : app->termDock->all()) t->setProjectRoot(dir);   // for their links
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
    App* app = static_cast<App*>(userp);
    // editor.organize-imports-on-save, then format.on-save: Ctrl+S organizes
    // and formats first and saves when that is done (Save All, auto-save and
    // the close question's Save do neither).
    const Settings& st = g.settings->settings();
    if (app->format && app->format->beforeSaving(st.organizeImportsOnSave(), st.formatOnSave()))
        return;
    saveCurrent(app);
}

// Format Document (PARITY.md 2.4), and with lines selected those lines.
static void act_format_document(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (app->format) app->format->formatDocument();
}

// Ask Again About This Folder's Tools (PARITY.md 2.3): on only while the
// folder has an answer to take back.
static void updateFolderToolsAction(App* app) {
    if (app->dead) return;
    if (GAction* a = g_action_map_lookup_action(G_ACTION_MAP(app->window), "forgetfoldertools"))
        g_simple_action_set_enabled(G_SIMPLE_ACTION(a),
                                    !FolderConsent::answer(app->rootDir).empty());
}

// The answer the bar or Format Document stored goes: the servers stop using
// the folder's environment (they start again), the bar asks again over a
// Python file (also when its question was put off with ×), and so does the
// next format with a formatter from the folder.
static void act_forget_folder_tools(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (FolderConsent::answer(app->rootDir).empty()) {
        gtk_widget_error_bell(app->window);
        return;
    }
    FolderConsent::forget(app->rootDir);
    flashStatus(app, "minicode will ask before running this folder's own tools again");
}

// Ctrl+Shift+S: the typeset PDF of a LaTeX file, as on the Mac (Shift+Cmd+S).
static void act_export_pdf(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    app->editor->exportPdf(GTK_WINDOW(app->window));
}

// File > Export as HTML…: a Markdown file as a standalone page.
static void act_export_html(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    app->editor->exportHtml(GTK_WINDOW(app->window));
}

// Ctrl+Alt+Shift+J: the caret's line in the PDF, as the Mac's Option+Cmd+J.
static void act_latex_synctex(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    app->editor->showLineInPdf();
}

// Zoom for a PDF or the LaTeX preview (PdfView). The reset is Ctrl+Alt+0,
// not Ctrl+0, which is Focus the File Tree here as on the Mac.
static void act_play_pause(GSimpleAction*, GVariant*, gpointer userp) {
    static_cast<App*>(userp)->editor->togglePlayMedia();
}

// Page Zoom: the browser's page while it has the keyboard (Safari's steps,
// and 100% for Ctrl+Alt+0), else the PDF on screen.
static void pageZoom(App* app, int step) {
#ifdef MINICODE_ENABLE_BROWSER
    if (browserHasKeyboard(app)) {
        app->browser->zoom(step);
        return;
    }
#endif
    app->editor->zoomPdf(step);
}
static void act_zoom_in(GSimpleAction*, GVariant*, gpointer userp) {
    pageZoom(static_cast<App*>(userp), 1);
}
static void act_zoom_out(GSimpleAction*, GVariant*, gpointer userp) {
    pageZoom(static_cast<App*>(userp), -1);
}
static void act_zoom_fit(GSimpleAction*, GVariant*, gpointer userp) {
    pageZoom(static_cast<App*>(userp), 0);
}

// The editor's zoom and the terminals', for every window (Zoom.h, PARITY.md
// 1.11): a point at a time, or back to none; the bell at a limit.
static void act_editor_zoom_in(GSimpleAction*, GVariant*, gpointer userp) {
    if (!Zoom::zoomEditor(1)) gtk_widget_error_bell(static_cast<App*>(userp)->window);
}
static void act_editor_zoom_out(GSimpleAction*, GVariant*, gpointer userp) {
    if (!Zoom::zoomEditor(-1)) gtk_widget_error_bell(static_cast<App*>(userp)->window);
}
static void act_editor_zoom_reset(GSimpleAction*, GVariant*, gpointer) { Zoom::zoomEditor(0); }
static void act_terminal_zoom_in(GSimpleAction*, GVariant*, gpointer userp) {
    if (!Zoom::zoomTerminal(1)) gtk_widget_error_bell(static_cast<App*>(userp)->window);
}
static void act_terminal_zoom_out(GSimpleAction*, GVariant*, gpointer userp) {
    if (!Zoom::zoomTerminal(-1)) gtk_widget_error_bell(static_cast<App*>(userp)->window);
}
static void act_terminal_zoom_reset(GSimpleAction*, GVariant*, gpointer) { Zoom::zoomTerminal(0); }

// Word wrap (Alt+Z): turned over for the session, in every window.
static void act_toggle_word_wrap(GSimpleAction*, GVariant*, gpointer) { Zoom::toggleWordWrap(); }

// Maximize Pane (Ctrl+Alt+M): the pane with the keyboard takes the right
// side, or gives it back (WindowLayout::toggleMaximized).
static void act_toggle_maximize(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (!app->layout->toggleMaximized()) gtk_widget_error_bell(app->window);
    refreshHints(app);
}

// Go Back and Go Forward (NavHistoryUi.h); the bell when there is nowhere
// to go.
static void act_go_back(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    app->nav->goBack();
    refreshHints(app);
}
static void act_go_forward(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    app->nav->goForward();
    refreshHints(app);
}

static void act_toggle_preview(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    showEditorArea(app);   // the preview shows in the editor's slot
    app->editor->togglePreview();
    updateTitle(app);      // Format Document only on the source
    refreshHints(app);
}

static void act_toggle_side_preview(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    showEditorArea(app);
    app->editor->toggleSidePreview();
    refreshHints(app);
}

// Toggle Sidebar: the tree, or the panel in its place, hides or shows at the
// width it had (WindowLayout::toggleSidebar).
static void act_toggle_sidebar(GSimpleAction*, GVariant*, gpointer userp) {
    static_cast<App*>(userp)->layout->toggleSidebar();
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

// Toggle Terminal, Toggle Editor and Toggle Browser each hide or show their
// own pane and nothing else, as the Mac's toggleTerminal:, toggleEditor: and
// toggleBrowser:. A pane shown again comes back at the size it had; with the
// editor, the browser and the terminal all hidden the file tree has the
// window, and none of them comes back until asked (the owner's report of
// 2026-11-09: the port used to reopen the editor when the last of them
// closed). WindowLayout.cpp has the rules and where the keyboard goes.
#ifdef MINICODE_ENABLE_TERMINAL
// One terminal of the window's dock, with the window's hooks on it (the
// dock's factory): its folder and control id (MINICODE_TERMINAL_ID, before
// the shell starts), links, the IDE link's variables, and the settings and
// zoom as they are now.
static Terminal* makeTerminal(App* app) {
    Terminal* t = new Terminal(app->rootDir);
    t->setProjectRoot(app->rootDir);
    t->setControlId(app->controlId);
    t->onCommandFinished = [app] {
        if (!app->dead && app->lsp) app->lsp->lookAgainForMissingServer();
    };
    t->setSessionRoots([app] { return sessionRootsOf(app); });
    t->setLinkHandler([app](const Terminal::Link& link) {
        if (!app->dead) openTerminalLink(app, link);
    });
    t->extraEnvironment = [app] {
        return app->ide ? app->ide->terminalEnvironment() : std::vector<std::string>();
    };
    if (g.settings) {
        t->applySettings(g.settings->settings());
        const double base = Zoom::terminalBasePoints();
        t->setFontSize(base, Zoom::terminalPoints() / base);
    }
    return t;
}

// The question before a terminal with a program running closes.
static void askCloseBusyTerminal(App* app, Terminal* t, std::function<void()> proceed) {
    GtkAlertDialog* dlg = gtk_alert_dialog_new("Close this terminal?");
    gtk_alert_dialog_set_detail(dlg, "A program is still running in it, and closing the terminal ends it.");
    const char* buttons[] = {"Close Terminal", "Cancel", nullptr};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_cancel_button(dlg, 1);
    gtk_alert_dialog_set_default_button(dlg, 1);
    struct Ask { App* app; std::function<void()> proceed; };
    gtk_alert_dialog_choose(dlg, GTK_WINDOW(app->window), nullptr,
        [](GObject* src, GAsyncResult* res, gpointer up) {
            Ask* a = static_cast<Ask*>(up);
            GError* err = nullptr;
            const int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, &err);
            if (err) g_error_free(err);
            else if (choice == 0 && !a->app->dead) a->proceed();
            delete a;
        }, new Ask{app, std::move(proceed)});
    g_object_unref(dlg);
    (void)t;
}

// New Terminal: a tab right of the active one, shown, with the keyboard.
static Terminal* newTerminalTab(App* app, const std::string& title, bool focus) {
    if (app->dead || !app->termDock) return nullptr;
    Terminal* t = app->termDock->newTerminal(title);
    if (!app->layout->terminalShown()) {
        GtkWidget* keep = gtk_window_get_focus(GTK_WINDOW(app->window));
        app->layout->toggleTerminal();
        if (!focus && keep && gtk_widget_get_root(keep) && gtk_window_get_focus(GTK_WINDOW(app->window)) != keep)
            gtk_widget_grab_focus(keep);
    } else if (focus) {
        t->focus();
    }
    return t;
}
#endif

// Run File and `minicode term --run` (PARITY.md 3.1): the window's one
// terminal shown with the keyboard left where it was, then a line typed and
// entered at its prompt, or `done` told why not (a running command, a shell
// that does not report). Nothing is ever typed into a busy terminal.
static void showTerminalQuietly(App* app) {
#ifdef MINICODE_ENABLE_TERMINAL
    if (app->dead || !app->terminal || app->layout->terminalShown()) return;
    GtkWidget* keep = gtk_window_get_focus(GTK_WINDOW(app->window));
    app->layout->toggleTerminal();
    if (keep && gtk_widget_get_root(keep) && gtk_window_get_focus(GTK_WINDOW(app->window)) != keep)
        gtk_widget_grab_focus(keep);
#else
    (void)app;
#endif
}

static void runInTerminal(App* app, const std::string& line, std::function<void(const std::string&)> done,
                          bool newTabIfBusy = false) {
#ifdef MINICODE_ENABLE_TERMINAL
    if (app->dead || !app->terminal) {
        done("this window has no terminal");
        return;
    }
    if (newTabIfBusy) {   // Run File never types into a busy terminal: a new tab, as the Mac's
        bool wait = false;
        if (!app->terminal->runBlocker(&wait).empty() && !wait) newTerminalTab(app, "", false);
    }
    showTerminalQuietly(app);
    app->terminal->runLine(line, std::move(done));
#else
    (void)app; (void)line;
    done("this build has no terminal");
#endif
}

// Run File (Shift+Cmd+Return on the Mac, Ctrl+Shift+Return here): the open
// file saved, then run.<language>'s command typed into the terminal as if
// the user had, so it is in the shell's history. A busy terminal is never
// typed into: it goes to a new terminal tab instead.
static void act_run_file(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    const std::string path = app->editor->currentPath();
    if (path.empty() || !app->editor->canSave()) {
        flashStatus(app, "Open a file to run it");
        return;
    }
    std::string ext;
    const size_t dot = baseName(path).rfind('.');
    if (dot != std::string::npos) ext = baseName(path).substr(dot + 1);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    const std::string tmpl = g.settings->settings().runTemplate(ext);
    if (tmpl.empty()) {
        flashStatus(app, ext.empty() ? "No run command for files without an extension"
                                     : "No run command for ." + ext + " files (add run." + ext + " to the settings)");
        return;
    }
    const std::string dir = path.substr(0, path.rfind('/'));
    std::map<std::string, std::string> vars = {
        {"file", path}, {"dir", dir}, {"root", app->rootDir.empty() ? dir : app->rootDir},
        {"name", baseName(path)}};
    if (tmpl.find("{python}") != std::string::npos) {
        std::string python = app->lsp ? app->lsp->serverPythonInterpreter() : std::string();
        if (python.empty()) python = RunFile::findVirtualenvPython(path, app->rootDir);
        vars["python"] = python.empty() ? "python3" : python;
    }
    const std::string line = RunFile::expand(tmpl, vars);
    if (app->editor->dirty()) {
        saveCurrent(app);
        if (app->editor->dirty()) return;   // the save failed and said why
    }
    App* a = app;
    runInTerminal(app, line, [a, name = baseName(path)](const std::string& why) {
        if (why.empty() || a->dead) return;
        flashStatus(a, "Not run: " + why + " (" + name + ")");
    }, true);
}

#ifdef MINICODE_ENABLE_TERMINAL
// The Terminal menu (PARITY.md 4.7).
static void act_new_terminal(GSimpleAction*, GVariant*, gpointer userp) {
    newTerminalTab(static_cast<App*>(userp), "", true);
}
static void act_next_terminal(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (app->termDock) app->termDock->next();
}
static void act_previous_terminal(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (app->termDock) app->termDock->previous();
}
// Close Terminal closes the active pane; the tab goes with its last.
static void act_close_terminal(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (app->termDock) app->termDock->closePane(app->termDock->active());
}
// Split Terminal and Split Terminal Down (PARITY.md 4.7): the dock is shown
// first, and a refusal (a pane would be under 20 columns or 5 rows) beeps.
static Terminal* splitTerminalPane(App* app, bool down, bool focus) {
    if (app->dead || !app->termDock) return nullptr;
    if (!app->layout->terminalShown()) {
        GtkWidget* keep = gtk_window_get_focus(GTK_WINDOW(app->window));
        app->layout->toggleTerminal();
        if (!focus && keep && gtk_widget_get_root(keep) && gtk_window_get_focus(GTK_WINDOW(app->window)) != keep)
            gtk_widget_grab_focus(keep);
    }
    return app->termDock->split(down, focus);
}
static void act_split_terminal(GSimpleAction*, GVariant*, gpointer userp) {
    splitTerminalPane(static_cast<App*>(userp), false, true);
}
static void act_split_terminal_down(GSimpleAction*, GVariant*, gpointer userp) {
    splitTerminalPane(static_cast<App*>(userp), true, true);
}
static void resizeTerminalPane(gpointer userp, TerminalGroup::Dir d) {
    App* app = static_cast<App*>(userp);
    if (app->termDock && !app->termDock->resizeActive(d)) {
        if (GdkDisplay* disp = gdk_display_get_default()) gdk_display_beep(disp);
    }
}
static void act_resize_terminal_left(GSimpleAction*, GVariant*, gpointer p) { resizeTerminalPane(p, TerminalGroup::Dir::Left); }
static void act_resize_terminal_right(GSimpleAction*, GVariant*, gpointer p) { resizeTerminalPane(p, TerminalGroup::Dir::Right); }
static void act_resize_terminal_up(GSimpleAction*, GVariant*, gpointer p) { resizeTerminalPane(p, TerminalGroup::Dir::Up); }
static void act_resize_terminal_down(GSimpleAction*, GVariant*, gpointer p) { resizeTerminalPane(p, TerminalGroup::Dir::Down); }
static void act_rename_terminal(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (!app->termDock) return;
    if (!app->layout->terminalShown()) app->layout->toggleTerminal();
    app->termDock->askRename(app->termDock->activeIndex());
}
static void act_move_terminal_left(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (app->termDock) app->termDock->moveActive(-1);
}
static void act_move_terminal_right(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (app->termDock) app->termDock->moveActive(1);
}
#endif

static void act_toggle_terminal(GSimpleAction*, GVariant*, gpointer userp) {
    static_cast<App*>(userp)->layout->toggleTerminal();
}

// Shows a window's terminal and gives it the keyboard (Next Waiting Terminal,
// the Agents panel's Show Terminal).
static void showTerminalOf(App* target) {
    gtk_window_present(GTK_WINDOW(target->window));
    if (!target->layout->terminalShown()) target->layout->toggleTerminal();
    target->terminal->focus();
}

// The scratchpad and worktree of the Claude Code session this window's
// terminal last named to `minicode notify` (the Mac's sessionLinkRoots),
// read from the Agents model; none when no session is known.
static void ensureAgents(App* app);
static std::vector<std::string> sessionRootsOf(App* app) {
    std::vector<std::string> roots;
    std::string id, transcript;
    double when = 0;
    if (app->dead || !TerminalAttention::sessionOf(app->controlId, &id, &transcript, &when)) return roots;
    ensureAgents(app);
    if (!app->agents) return roots;
    std::function<void(const AgentModelData::Work&)> walk = [&](const AgentModelData::Work& w) {
        if ((!id.empty() && w.sessionId == id) || (!transcript.empty() && w.transcript == transcript)) {
            if (!w.scratchpad.empty()) roots.push_back(w.scratchpad);
            if (!w.worktreePath.empty()) roots.push_back(w.worktreePath);
        }
        for (const auto& sub : w.subagents) walk(sub);
    };
    if (auto snap = app->agents->model().snapshot())
        for (const auto& w : snap->sessions) walk(w);
    return roots;
}

// Next Waiting Terminal (the Mac's Ctrl+Cmd+W, PARITY.md 3.6): the window
// whose terminal has a Claude Code session waiting, after this one's in the
// windows' order, wrapping; its terminal is shown and takes the keyboard.
static void act_next_waiting(GSimpleAction*, GVariant*, gpointer userp) {
    App* here = static_cast<App*>(userp);
    std::vector<int> order;
    for (App* a : g.windows)
        if (!a->dead && a->terminal) order.push_back(a->controlId);
    const int want = TerminalAttention::nextWaiting(order, here->controlId);
    App* target = nullptr;
    for (App* a : g.windows)
        if (!a->dead && a->terminal && a->controlId == want) target = a;
    if (!target) {
        gtk_widget_error_bell(here->window);
        return;
    }
    showTerminalOf(target);
}

static void act_toggle_editor(GSimpleAction*, GVariant*, gpointer userp) {
    static_cast<App*>(userp)->layout->toggleEditor();
}

// New Browser Tab: Ctrl+T in the browser's page (Safari's Cmd+T on the Mac),
// View > New Browser Tab from anywhere. The pane is shown, the keyboard in
// the new tab's address bar.
static void act_new_browser_tab(GSimpleAction*, GVariant*, gpointer userp) {
#ifdef MINICODE_ENABLE_BROWSER
    App* app = static_cast<App*>(userp);
    if (!app->browser) return;
    app->layout->showBrowser();
    app->browser->newTab(MINICODE_BROWSER_HOME);
    app->browser->focusUrlBar();
#else
    (void)userp;
#endif
}

static void act_toggle_browser(GSimpleAction*, GVariant*, gpointer userp) {
    static_cast<App*>(userp)->layout->toggleBrowser();
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

// Adjust Settings… from a part's right click menu or View > Adjust Settings
// (the target is the part, Settings::appearanceSurfaces): the settings file
// opened at that part's keys (Appearance.h, the Mac's showSettingsForSurface:).
static void act_adjust_settings(GSimpleAction*, GVariant* param, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (app->dead || !param) return;
    const std::string surface = g_variant_get_string(param, nullptr);
    if (!Settings::appearanceSurface(surface)) return;
    if (!g.settings->ensureFileExists()) {
        gtk_label_set_text(GTK_LABEL(app->settingsLabel),
                           ("Could not create " + g.settings->path()).c_str());
        return;
    }
    openFileThen(app, g.settings->path(), EditorTabs::OpenFrom::Other, [app, surface] {
        Appearance::showSurfaceKeys(app->editor, surface, Appearance::FileHost{
            [app](Document* d) { return !app->dead && saveDocument(app, d); },
            [app](const std::string& note) {
                if (!app->dead) flashStatus(app, note);
            }});
    });
}

// The find bar (FindBar.h): Find and Replace seed the query from the
// selection or the word at the caret; Find Next and Find Previous (VS Code's
// F3 and Shift+F3 on Linux) step from the selection, wrapping, with the bar
// open or closed, and open it when there is nothing to look for; the
// options flip from the editor or the bar; Replace and Replace All act on
// the matches as the bar shows them.
static FindBar* findBarOf(gpointer userp) { return static_cast<App*>(userp)->findBar; }
// The bar and the matches are on the editor's side, so a hidden editor comes
// back first (WindowLayout::revealEditor).
static FindBar* revealFindBar(gpointer p) {
    showEditorArea(static_cast<App*>(p));
    return findBarOf(p);
}
static void act_find(GSimpleAction*, GVariant*, gpointer p) {
#ifdef MINICODE_ENABLE_BROWSER
    // With the keyboard in the browser pane (page or address bar) Find is the page's.
    if (browserHasKeyboard(static_cast<App*>(p)) && static_cast<App*>(p)->browser->webView()) {
        static_cast<App*>(p)->browser->showFind();
        return;
    }
#endif
    revealFindBar(p)->showFind();
}
static void act_replace(GSimpleAction*, GVariant*, gpointer p) { revealFindBar(p)->showReplace(); }
static void act_find_next(GSimpleAction*, GVariant*, gpointer p) {
#ifdef MINICODE_ENABLE_BROWSER
    if (browserHasKeyboard(static_cast<App*>(p)) && static_cast<App*>(p)->browser->webView()) {
        static_cast<App*>(p)->browser->findNext(true);
        return;
    }
#endif
    revealFindBar(p)->findNext(true);
}
static void act_find_previous(GSimpleAction*, GVariant*, gpointer p) {
#ifdef MINICODE_ENABLE_BROWSER
    if (browserHasKeyboard(static_cast<App*>(p)) && static_cast<App*>(p)->browser->webView()) {
        static_cast<App*>(p)->browser->findNext(false);
        return;
    }
#endif
    revealFindBar(p)->findNext(false);
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
static void act_toggle_processes(GSimpleAction*, GVariant*, gpointer userp);
static void act_toggle_agents(GSimpleAction*, GVariant*, gpointer userp);
static void showEvidence(App* app, const std::string& transcript, const std::string& name);
static void setupWorktrees(App* app);
static void addAction(App* app, const char* name, GCallback cb);

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

// Focus Editor: the editor gets the keyboard, shown first if it was hidden
// (the Mac's Command 1): what the tab on show shows, the LaTeX pages too.
static void act_focus_editor(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    showEditorArea(app);
    app->editor->focusGroup(0);   // the left group when split (PARITY.md 4.6)
    refreshHints(app);
}

// The split editor (PARITY.md 4.6, Groups.cpp): Focus Second Editor Group
// (which splits the editor when it is not split), Split Editor, and Move
// Editor into Next and Previous Group.
static void act_focus_group2(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    showEditorArea(app);
    app->editor->focusGroup(1);
    refreshHints(app);
}
static void act_split_editor(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    showEditorArea(app);
    app->editor->splitEditor();
    refreshHints(app);
}
static void act_move_editor_next(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    showEditorArea(app);
    app->editor->moveToGroup(1);
    refreshHints(app);
}
static void act_move_editor_previous(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    showEditorArea(app);
    app->editor->moveToGroup(-1);
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

// A folder in the file manager (the Mac's Reveal in Finder for a scratchpad).
// $MINICODE_FILE_MANAGER names a program that gets the folder as its one
// argument instead, which is how the checks record it.
static void revealFolder(App* app, const std::string& folder) {
    if (const char* fm = g_getenv("MINICODE_FILE_MANAGER"); fm && *fm) {
        const char* argv[] = {fm, folder.c_str(), nullptr};
        GError* err = nullptr;
        if (!g_spawn_async(nullptr, const_cast<char**>(argv), nullptr, G_SPAWN_SEARCH_PATH,
                           nullptr, nullptr, nullptr, &err))
            showError(app, "Could not open the folder", err ? err->message : "");
        g_clear_error(&err);
        return;
    }
    GFile* f = g_file_new_for_path(folder.c_str());
    GtkFileLauncher* launcher = gtk_file_launcher_new(f);
    g_object_unref(f);
    gtk_file_launcher_launch(launcher, GTK_WINDOW(app->window), nullptr,
        [](GObject* src, GAsyncResult* res, gpointer up) {
            GError* err = nullptr;
            if (!gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(src), res, &err) &&
                !g_error_matches(err, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
                !static_cast<App*>(up)->dead)
                showError(static_cast<App*>(up), "Could not open the folder",
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
    if (app->procsShown) act_toggle_processes(nullptr, nullptr, app);
    if (app->agentsShown) act_toggle_agents(nullptr, nullptr, app);
    if (!app->layout->sidebarShown()) act_toggle_sidebar(nullptr, nullptr, app);
    app->tree->revealPath(path);
    app->tree->focus();
}

static void tabCommand(App* app, const std::string& name, Document* d) {
    if (app->dead || !d) return;
    Editor* e = app->editor;
    // The tab's group's tabs (the strip with the menu is the focused one's).
    const std::vector<Document*> all = e->groupDocuments(e->focusedGroup());
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
    closeTabsAsking(app, idsOf(app->editor->groupDocuments(app->editor->focusedGroup())));
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
    // The Problems list with the keyboard closes, as the Mac's Cmd+W.
    if (app->problems && app->problems->hasFocus()) {
        app->problems->hide();
        app->layout->focusAfterClosingPanel();
        return;
    }
#ifdef MINICODE_ENABLE_BROWSER
    // The agent page when it shows, else the browser's tab; the last tab
    // hides the pane (Browser::onEmpty), the keyboard to what is left.
    if (browserHasKeyboard(app)) {
        app->browser->closeActive();
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
    showEditorArea(app);
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
#ifdef MINICODE_ENABLE_BROWSER
    if (browserHasKeyboard(app) && app->browser->tabCount() > 1) {   // with one tab the editor's
        app->browser->stepTab(1);
        return;
    }
#endif
    showEditorArea(app);
    app->editor->stepTab(1);
    refreshHints(app);
}
static void act_previous_tab(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
#ifdef MINICODE_ENABLE_BROWSER
    if (browserHasKeyboard(app) && app->browser->tabCount() > 1) {
        app->browser->stepTab(-1);
        return;
    }
#endif
    showEditorArea(app);
    app->editor->stepTab(-1);
    refreshHints(app);
}
template <int N>
static void act_go_to_tab(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
#ifdef MINICODE_ENABLE_BROWSER
    if (browserHasKeyboard(app) && app->browser->tabCount() > 1) {
        app->browser->selectTabNumber(N);
        return;
    }
#endif
    showEditorArea(app);
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
        if (app->browser) {
            // Shown beside the editor, the keyboard kept where it was, as the
            // Mac's openWebAddressFromTerminal: does. Loaded first, so a
            // browser not shown before loads this page, not its home page.
            app->browser->load(target);
            app->layout->showBrowser();
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
        if (!app->layout->sidebarShown()) act_toggle_sidebar(nullptr, nullptr, app);
        if (app->gitShown) act_toggle_git(nullptr, nullptr, app);   // the tree back
        if (app->procsShown) act_toggle_processes(nullptr, nullptr, app);
        if (app->agentsShown) act_toggle_agents(nullptr, nullptr, app);
        app->tree->revealPath(path);
        return;
    }
    if (insideRoot) app->tree->revealPath(path);
    // A jump, for Go Back (NavHistoryUi.h).
    app->nav->jump([app, path, line, column] {
        openFileThen(app, path, EditorTabs::OpenFrom::Other, [app, path, line, column] {
            // Opening can be refused (Cancel at "Save changes?"), and an
            // image or a PDF has no lines to go to.
            if (app->editor->currentPath() != path) return;
            showEditorArea(app);
            if (app->editor->isMedia()) return;
            if (line > 0) app->editor->revealLineColumn(line, column);
            else gtk_widget_grab_focus(app->editor->textView());
            refreshHints(app);   // a Markdown preview may have switched to source
        });
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
        a->nav->moved(src, dst);   // Go Back's places follow it
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
        a->nav->removed(path);   // no place to go back to
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
        if (!app->layout->sidebarShown()) act_toggle_sidebar(nullptr, nullptr, app);
        app->git->focus();
        return;
    }
    if (app->procsShown) {
        if (!app->layout->sidebarShown()) act_toggle_sidebar(nullptr, nullptr, app);
        app->procs->focus();
        return;
    }
    if (app->agentsShown) {
        if (!app->layout->sidebarShown()) act_toggle_sidebar(nullptr, nullptr, app);
        app->agents->focus();
        return;
    }
    app->tree->focus();
}

// A file's diff or a commit, in the tab diffs and commits share.
static void showDiffInEditor(App* app, const std::string& title, const std::string& bytes,
                             bool commit) {
    showEditorArea(app);
    app->editor->showDiff(title, bytes, commit);
    updateTitle(app);
    refreshHints(app);
}

// The Source Control panel's object, made with the window as the Mac's is
// (EditorController+Git.mm's ensureGitPanel), so autofetch and View > Source
// Control's commands (GitPanel.h) work before it is first shown; its widget
// joins the sidebar on the first Ctrl+Shift+G.
static void makeGitPanel(App* app) {
    app->git = new GitPanel(app->rootDir);
    app->git->applySettings(g.settings->settings());
    app->git->hostWindow = GTK_WINDOW(app->window);
    app->git->onShowDiff = [app](const std::string& name, const std::string&,
                                 const std::string& diff) {
        if (!app->dead) showDiffInEditor(app, name + " (diff)", diff, false);
    };
    app->git->onShowCommit = [app](const std::string& title, const std::string& show) {
        if (!app->dead) showDiffInEditor(app, title, show, true);
    };
    app->git->onNote = [app](const std::string& text) {
        if (!app->dead) flashStatus(app, text);
    };
    app->git->onOpenFile = [app](const std::string& path) {
        if (!app->dead) openFileThen(app, path, EditorTabs::OpenFrom::Other, nullptr);
    };
    app->git->onRevealFile = [app](const std::string& path) {
        if (!app->dead) revealInTree(app, path);
    };
    app->git->showPick = [app](const PalettePanel::Pick& pick) {
        if (!app->dead && app->palette) app->palette->showPick(pick);
    };
    app->git->isActive = [app] {
        return !app->dead && !app->closing && gtk_window_is_active(GTK_WINDOW(app->window));
    };
    app->git->fileOnShow = [app] {
        return app->dead || app->editor->isDiff() ? std::string() : app->editor->currentPath();
    };
    app->git->installActions(G_ACTION_MAP(app->window));
    app->editor->onDiffViewMade = [app](GitDiffView* view) {
        if (!app->dead && app->git) app->git->attachDiffView(view);
    };
    // The editor's change bars (GitGutter.h), made with the panel as on the
    // Mac; its Stage Change has the panel look again, and the panel's
    // refresh has the bars look again.
    app->gitGutter = new GitGutter(app->editor);
    app->gitGutter->onNote = [app](const std::string& text) {
        if (!app->dead) flashStatus(app, text);
    };
    app->gitGutter->onIndexChanged = [app] {
        if (!app->dead && app->git) app->git->refresh();
    };
    app->gitGutter->installActions(G_ACTION_MAP(app->window));
    app->editor->notes()->installActions(G_ACTION_MAP(app->window));   // the tour's keys
    // The inline agent's bar (PARITY.md 3.3).
    app->editor->inlineAgent()->command = [] { return g.settings->settings().agentCommand(); };
    app->editor->inlineAgent()->folder = [app] { return app->rootDir; };
    app->editor->inlineAgent()->note = [app](const std::string& text) {
        if (!app->dead) flashStatus(app, text);
    };
    app->editor->inlineAgent()->typeLine = [app](const std::string& line) {
#ifdef MINICODE_ENABLE_TERMINAL
        if (app->dead || !app->terminal) return false;
        if (!app->layout->terminalShown()) app->layout->toggleTerminal();
        if (!app->terminal->typeText(line)) return false;   // never Return: the user reads it first
        app->terminal->focus();
        return true;
#else
        (void)app; (void)line;
        return false;
#endif
    };
    app->git->onRefreshed = [app] {
        if (!app->dead && app->gitGutter) app->gitGutter->refreshSoon();
    };
}

// Ctrl+Shift+G: the Source Control panel takes the file tree's place in the
// sidebar and gives it back when pressed again, the Mac's
// toggleSourceControl:. The sidebar keeps its width, and a hidden one opens.
static void act_toggle_git(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (app->procsShown) act_toggle_processes(nullptr, nullptr, app);
    if (app->agentsShown) act_toggle_agents(nullptr, nullptr, app);
    if (!gtk_widget_get_parent(app->git->widget()))
        gtk_stack_add_named(GTK_STACK(app->sidebar), app->git->widget(), "git");
    app->gitShown = !app->gitShown;
    if (app->gitShown) {
        if (!app->layout->sidebarShown()) act_toggle_sidebar(nullptr, nullptr, app);
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

// View > Toggle Processes: the Processes panel in the tree's place, as the
// Source Control panel's is (the Mac's toggleProcesses:). Sampling comes
// every 2 s only while it shows.
static void act_toggle_processes(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (!app->procs) {
        app->procs = new ProcessesPanel();
        app->procs->hostWindow = GTK_WINDOW(app->window);
        app->procs->windowId = [app] { return app->controlId; };
        app->procs->folder = [app] {
            char buf[PATH_MAX];
            return realpath(app->rootDir.c_str(), buf) ? std::string(buf) : app->rootDir;
        };
        app->procs->onNote = [app](const std::string& text) {
            if (!app->dead) flashStatus(app, text);
        };
        app->procs->onOpenUrl = [app](const std::string& url) {
            if (!app->dead) openLink(app, true, url, false, false, 0, 0);
        };
        app->procs->onRevealTerminal = [](int terminalId) {
            for (App* a : g.windows)
                if (!a->dead && a->terminal && a->controlId == terminalId) {
                    showTerminalOf(a);
                    return true;
                }
            return false;
        };
    }
    if (app->gitShown) act_toggle_git(nullptr, nullptr, app);
    if (app->agentsShown) act_toggle_agents(nullptr, nullptr, app);
    if (!gtk_widget_get_parent(app->procs->widget()))
        gtk_stack_add_named(GTK_STACK(app->sidebar), app->procs->widget(), "processes");
    app->procsShown = !app->procsShown;
    if (app->procsShown) {
        if (!app->layout->sidebarShown()) act_toggle_sidebar(nullptr, nullptr, app);
        gtk_stack_set_visible_child(GTK_STACK(app->sidebar), app->procs->widget());
        app->procs->setActive(true);
        app->procs->focus();
    } else {
        gtk_stack_set_visible_child(GTK_STACK(app->sidebar), app->tree->widget());
        app->procs->setActive(false);
        app->tree->focus();
    }
    refreshHints(app);
}

// View > Toggle Agents (Ctrl+Shift+A): the Agents panel in the tree's place
// (the Mac's toggleAgents:). Opening it closes Source Control and Processes.
// The Agents panel and the worktrees the file tree shows, made when first
// wanted (the panel's toggle, or the tree finding .claude/worktrees).
static void ensureAgents(App* app) {
    if (app->agents) return;
        app->agents = new AgentsPanel(app->rootDir);
        app->agents->onOpenFile = [app](const std::string& path) {
            openFileThen(app, path, EditorTabs::OpenFrom::Other, [app] { refreshHints(app); });
            gtk_window_present(GTK_WINDOW(app->window));
        };
        app->agents->onShowEvidence = [app](const std::string& transcript, const std::string& name) {
            showEvidence(app, transcript, name);
        };
        app->agents->terminalFor = [](const std::string& id, const std::string& transcript) {
            return TerminalAttention::terminalFor(id, transcript);
        };
        app->agents->onShowTerminal = [app](int terminalId) {
            for (App* a : g.windows)
                if (!a->dead && a->terminal && a->controlId == terminalId) {
                    showTerminalOf(a);
                    return;
                }
            if (!app->dead) flashStatus(app, "That terminal has closed");
        };
        app->agents->onOpenFolderInNewWindow = [app](const std::string& folder) {
            if (!app->dead) newWindow(folder, "");
        };
        app->agents->onRevealFolder = [app](const std::string& folder) {
            if (!app->dead) revealFolder(app, folder);
        };
        app->agents->onNote = [app](const std::string& text) {
            if (!app->dead) flashStatus(app, text);
        };
    app->worktrees = new AgentWorktrees(app->rootDir, app->agents->model());
    app->worktrees->onChange = [app] {
        if (!app->dead && app->tree) app->tree->redecorate();
    };
    app->agents->onModelChange = [app] {
        if (!app->dead && app->worktrees) app->worktrees->refreshSoon();
    };
    app->worktrees->refresh();
}

// The tree's Worktrees node (PARITY.md 3.5): each worktree named for its
// agent with its ahead count and changes; a right click offers Compare with
// Main (a page of the changed files, each opening side by side with main at
// the branch point) and Open in New Window.
static void compareWorktree(App* app) {
    if (app->dead || !app->worktrees) return;
    const WorktreeInfo* w = app->worktrees->find(app->tree->selectedPath());
    if (!w) {
        flashStatus(app, "git has not listed that folder as a worktree yet");
        app->worktrees->refresh();
        return;
    }
    auto page = std::make_shared<WorktreeChangesPage>(app->worktrees, *w);
    page->onShowComparison = [app](const CompareSpec& spec) {
        if (!app->dead) app->editor->showComparison(spec);
    };
    app->editor->showPage(page);
}

static void setupWorktrees(App* app) {
    app->tree->setWorktreeText([app](const std::string& path, std::string* title, std::string* tip) {
        if (app->dead || !app->worktrees) return false;
        const WorktreeInfo* w = app->worktrees->find(path);
        if (!w) return false;
        *title = w->label;
        if (!w->badge().empty()) *title += "  " + w->badge();
        *tip = w->label + "\n" + path;
        if (!w->branch.empty()) *tip += "\nBranch " + w->branch;
        if (w->running) *tip += "\nIts agent is running";
        return true;
    });
    app->worktreeMenu = g_menu_new();
    g_menu_append(app->worktreeMenu, "Compare with Main", "win.compareworktree");
    g_menu_append(app->worktreeMenu, "Open in New Window", "win.worktreewindow");
    app->tree->setWorktreeMenu(G_MENU_MODEL(app->worktreeMenu));
    addAction(app, "compareworktree", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer p) {
        compareWorktree(static_cast<App*>(p));
    }));
    addAction(app, "worktreewindow", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer p) {
        App* a = static_cast<App*>(p);
        const std::string path = a->tree->selectedPath();
        if (!a->dead && a->tree->isWorktreeRow(path)) newWindow(path, "");
    }));
    struct stat st;
    if (stat((app->rootDir + "/.claude/worktrees").c_str(), &st) == 0) ensureAgents(app);
}

// The Evidence page of a session in a tab (PARITY.md 3.5); Open in Terminal
// types the command at the session's folder and never presses Return.
static std::string shQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

static void showEvidence(App* app, const std::string& transcript, const std::string& name) {
    auto page = std::make_shared<EvidencePage>(transcript, name);
    page->onOpenInTerminal = [app](const std::string& command, const std::string& cwd) {
#ifdef MINICODE_ENABLE_TERMINAL
        if (app->dead || !app->terminal) return;
        std::string line = command;
        struct stat st;
        if (!cwd.empty() && cwd != app->rootDir && stat(cwd.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            line = "cd " + shQuote(cwd) + " && " + command;
        else if (!cwd.empty() && cwd != app->rootDir)
            flashStatus(app, cwd + " is gone; the command is typed in the folder");
        if (!app->layout->terminalShown()) app->layout->toggleTerminal();
        app->terminal->typeText(line);   // never Return: the user reads it first
#else
        (void)app; (void)command; (void)cwd;
#endif
    };
    app->editor->showPage(page);
}

static void act_toggle_agents(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    ensureAgents(app);
    if (app->gitShown) act_toggle_git(nullptr, nullptr, app);
    if (app->procsShown) act_toggle_processes(nullptr, nullptr, app);
    if (!gtk_widget_get_parent(app->agents->widget()))
        gtk_stack_add_named(GTK_STACK(app->sidebar), app->agents->widget(), "agents");
    app->agentsShown = !app->agentsShown;
    if (app->agentsShown) {
        if (!app->layout->sidebarShown()) act_toggle_sidebar(nullptr, nullptr, app);
        gtk_stack_set_visible_child(GTK_STACK(app->sidebar), app->agents->widget());
        app->agents->setActive(true);
        app->agents->focus();
    } else {
        gtk_stack_set_visible_child(GTK_STACK(app->sidebar), app->tree->widget());
        app->agents->setActive(false);
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
    // A jump, for Go Back (NavHistoryUi.h).
    app->nav->jump([app, path, from, line, column, length] {
        openFileThen(app, path, from, [app, path, line, column, length] {
            if (app->editor->currentPath() == path)
                app->editor->revealLine(line, column, length);
            refreshHints(app);   // a Markdown preview may have switched to source
        });
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

// The inline agent's bar (InlineAgentBar.h, PARITY.md 3.3): what has the
// keyboard decides the request, as the Mac's. A text file in the editor or a
// Markdown preview is an edit; an idle terminal a command, typed and never
// run; anything else (the tree, a panel, the browser, a picture, the LaTeX
// preview, a busy terminal) a task for Claude Code.
static void act_inline_agent(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    using Kind = InlineAgentBar::Kind;
    InlineAgentBar* bar = app->editor->inlineAgent();
    GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(app->window));
    // In the bar already: take the keyboard again, whatever its kind.
    if (bar->mode() != InlineAgentBar::Mode::Closed && focus &&
        (focus == bar->entry() || gtk_widget_is_ancestor(focus, bar->widget()))) {
        bar->open(bar->kind());
        return;
    }
    std::string where = "a panel";
    bool inTerminal = false;
#ifdef MINICODE_ENABLE_TERMINAL
    inTerminal = focus && app->terminal && app->terminal->owns(focus);
    if (inTerminal && !app->terminal->busy()) {
        if (!bar->open(Kind::Command)) gtk_widget_error_bell(app->window);
        return;
    }
#endif
    if (inTerminal) {
        where = "a terminal with a program running";
    } else if (focus && gtk_widget_is_ancestor(focus, app->editor->widget())) {
        if (bar->open(Kind::Edit)) return;
        const std::string& p = app->editor->currentPath();
        where = "the editor, showing " + (p.empty() ? std::string("no text file") : p);
    } else if (focus && app->tree && Keys::within(focus, app->tree->widget())) {
        where = "the file tree";
    }
#ifdef MINICODE_ENABLE_BROWSER
    if (where == "a panel" && focus && app->browser && Keys::within(focus, app->browser->widget()))
        where = "the browser";
#endif
    if (!bar->open(Kind::Task, where)) gtk_widget_error_bell(app->window);
}

static void act_hover(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (editorFocused(app)) app->lsp->showHoverInfo();
}

// Rename, references and quick fixes (PARITY.md 2.1, LspSessionActions.cpp).
static void act_rename_symbol(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (editorFocused(app)) app->lsp->renameSymbol();
}

static void act_references(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (editorFocused(app)) app->lsp->findReferences();
}

static void act_quick_fix(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (editorFocused(app)) app->lsp->showQuickFixes();
}

// Organize Imports, Shift+Alt+O (PARITY.md 2.2, LspSessionImports.cpp).
static void act_organize_imports(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (editorFocused(app)) app->lsp->organizeImports();
}

// The Problems list, Ctrl+Shift+M, and F8 and Shift+F8 through the file's
// problems (PARITY.md 2.2, ProblemsPanel.h, LspSessionProblems.cpp).
static void act_toggle_problems(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (app->problems) app->problems->toggle(true);
}

static void act_next_problem(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (editorFocused(app)) app->lsp->nextProblem();
}

static void act_previous_problem(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (editorFocused(app)) app->lsp->previousProblem();
}

// The palette (PalettePanel.h): the commands (Ctrl+Shift+P, F1), the folder's
// files (Ctrl+P), Go to Line (Ctrl+G), the file's symbols (Ctrl+Shift+O) and
// the workspace's (Ctrl+T) are one box, chosen by its first character. A
// file from Quick Open opens as on the Mac (OpenFrom::QuickOpen, a kept tab
// unless the settings say), and the tree follows it. The symbols come from
// the window's language servers (LspSessionSymbols.cpp, PARITY.md 2.12).
static void showPalette(App* app, const char* text) {
    PalettePanel::Context ctx;
    ctx.root = app->rootDir;
    ctx.useIgnoreFiles = g.settings->settings().searchUseIgnoreFiles();
    ctx.openFile = [app](const std::string& path) {
        if (!app->dead) openFileThen(app, path, EditorTabs::OpenFrom::QuickOpen, nullptr);
    };
    ctx.documentSymbols = [app](PalettePanel::Context::SymbolsDone done) {
        return !app->dead && app->lsp && app->lsp->documentSymbols(std::move(done));
    };
    ctx.workspaceSymbols = [app](const std::string& query,
                                 PalettePanel::Context::WorkspaceDone done) {
        if (!app->dead && app->lsp) app->lsp->workspaceSymbols(query, std::move(done));
    };
    ctx.cancelWorkspaceSymbols = [app] {
        if (!app->dead && app->lsp) app->lsp->cancelWorkspaceSymbols();
    };
    ctx.openWorkspaceSymbol = [app](const Lsp::Symbol& symbol) {
        if (!app->dead && app->lsp) app->lsp->revealSymbol(symbol);
    };
    app->palette->show(text, ctx);
}

static void act_show_commands(GSimpleAction*, GVariant*, gpointer userp) {
    showPalette(static_cast<App*>(userp), ">");
}
static void act_quick_open(GSimpleAction*, GVariant*, gpointer userp) {
    showPalette(static_cast<App*>(userp), "");
}
static void act_go_to_line(GSimpleAction*, GVariant*, gpointer userp) {
    showPalette(static_cast<App*>(userp), ":");
}
static void act_go_to_symbol(GSimpleAction*, GVariant*, gpointer userp) {
    showPalette(static_cast<App*>(userp), "@");
}
static void act_workspace_symbol(GSimpleAction*, GVariant*, gpointer userp) {
    showPalette(static_cast<App*>(userp), "#");
}

// Go to Bracket (BracketView.h): the bell when there is none to go to.
static void act_go_to_bracket(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (editorFocused(app) && !app->editor->brackets()->goToBracket())
        gtk_widget_error_bell(app->editor->textView());
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
    row("inlineChat.start", "Ask the inline agent");
    row("editor.action.commentLine", "Toggle comment");
    row("editor.action.jumpToBracket", "Go to bracket");
    row("editor.fold", "Fold the region at the cursor");
    row("editor.unfold", "Unfold it");
    row("editor.foldAll", "Fold all");
    row("editor.unfoldAll", "Unfold all");
    plain("Tab, Shift Tab", "Indent, outdent lines");
    row("editor.action.formatDocument", "Format document (or the selected lines)");
    row("minicode.sortLines", "Sort lines");
    row("minicode.sortKeepSortedBlocks", "Sort keep-sorted blocks");
    row("workbench.action.openSettings", "Settings");
    row("workbench.action.focusSideBar", "Focus the file tree");
    row("workbench.action.focusFirstEditorGroup", "Focus the editor (its left group)");
    row("workbench.action.focusSecondEditorGroup", "Focus the editor's right group");
    row("workbench.action.splitEditor", "Split the editor");
    row("workbench.action.moveEditorToNextGroup", "Move the tab to the next group");
    row("workbench.action.moveEditorToPreviousGroup", "Move the tab to the previous group");

    heading("Several carets and lines");
    row("editor.action.addSelectionToNextFindMatch", "Add the next occurrence");
    row("editor.action.selectHighlights", "Select all occurrences");
    row("editor.action.insertCursorBelow", "Add a cursor below");
    row("editor.action.insertCursorAbove", "Add a cursor above");
    plain("Alt click", "Add or remove a cursor");
    plain("Shift Alt drag", "A column (or a middle-button drag)");
    plain("Escape", "Back to one cursor");
    row("expandLineSelection", "Select line");
    row("editor.action.deleteLines", "Delete line");
    row("editor.action.moveLinesDownAction", "Move line down");
    row("editor.action.moveLinesUpAction", "Move line up");
    row("editor.action.copyLinesDownAction", "Copy line down");
    row("editor.action.copyLinesUpAction", "Copy line up");
    row("cursorTop", "Start of the file");
    row("cursorBottom", "End of the file");

    heading("Navigation");
    row("workbench.action.showCommands", "Command palette");
    row("workbench.action.quickOpen", "Go to file");
    row("workbench.action.gotoLine", "Go to line");
    row("workbench.action.gotoSymbol", "Go to symbol in the file");
    row("workbench.action.showAllSymbols", "Go to symbol in the workspace");
    row("workbench.action.navigateBack", "Go back");
    row("workbench.action.navigateForward", "Go forward");
    row("workbench.action.editor.nextChange", "Next change (git)");
    row("workbench.action.editor.previousChange", "Previous change");

    heading("View");
    row("editor.action.fontZoomIn", "Zoom in");
    row("editor.action.fontZoomOut", "Zoom out");
    row("editor.action.fontZoomReset", "Reset zoom");
    row("editor.action.toggleWordWrap",
        std::string("Word wrap   (") + (Zoom::wordWrap() ? "on" : "off") + ")");

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
    row("editor.action.rename", "Rename symbol");
    row("editor.action.goToReferences", "Find all references");
    row("editor.action.quickFix", "Quick fix");
    row("editor.action.organizeImports", "Organize imports");
    row("workbench.actions.view.problems",
        std::string("Problems    (") + (app->problems && app->problems->shown() ? "shown" : "hidden") +
            ")");
    row("editor.action.marker.next", "Next problem");
    row("editor.action.marker.prev", "Previous problem");

    heading("Panes");
    row("workbench.action.toggleSidebarVisibility", "Sidebar");
    row("minicode.toggleEditor",
        std::string("Editor      (") + (app->layout->editorShown() ? "shown" : "collapsed") + ")");
    row("minicode.toggleMaximizedPane",
        std::string("Maximize    (") + (app->layout->maximized() ? "maximized" : "off") + ")");
#ifdef MINICODE_ENABLE_TERMINAL
    row("workbench.action.terminal.toggleTerminal",
        std::string("Terminal    (") +
            (app->layout->terminalShown() ? "open" : "hidden") + ")");
    row("workbench.action.terminal.toggleTerminal", "Terminal, too", 1);
#endif
#ifdef MINICODE_ENABLE_BROWSER
    row("minicode.toggleBrowser",
        std::string("Browser     (") +
            (app->layout->browserShown() ? "open" : "hidden") + ")");
#endif
    row("workbench.view.scm",
        std::string("Git panel   (") + (app->gitShown ? "open" : "hidden") + ")");
    if (app->gitShown) {
        plain("Enter", "Show the diff or commit");
        plain("Space", "Stage or unstage");
        // Ctrl+Enter to commit is written in the message box itself.
        plain("Tab", "Changes, graph, message");
        plain("Ctrl or Shift click", "Choose several files");
        plain("Right click", "Push, pull, stage all, discard, stash");
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
        row("latex-workshop.synctex", "Show the caret's line in the PDF");
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
    if (app->dead || !app->layout) return;
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

// A terminal's font: terminal.font-size or the desktop's size, scaled for
// the terminals' zoom (Zoom.h).
static void applyTerminalZoom(App* app) {
#ifdef MINICODE_ENABLE_TERMINAL
    if (app->dead || !app->termDock) return;
    const double base = Zoom::terminalBasePoints();
    for (Terminal* t : app->termDock->all()) t->setFontSize(base, Zoom::terminalPoints() / base);
#else
    (void)app;
#endif
}

// One window's share of the settings: the editor's tags, the language
// servers, the terminal's colors, and the status-bar note about bad lines.
static void applyWindowSettings(App* app) {
    const Settings& st = g.settings->settings();
    Zoom::applySettings(st);   // every window's; each follows through its listener
    app->editor->applySettings(st);
    app->findBar->applySettings(st);
    if (app->lsp) app->lsp->applySettings(st);   // restarts servers if lsp.* changed
    if (app->ide) app->ide->setEnabled(st.ideLink());
    if (app->persist) app->persist->scheduleAutoSave();   // editor.auto-save may have changed
    if (app->git) app->git->applySettings(st);
    if (app->gitGutter) app->gitGutter->applySettings(st);
#ifdef MINICODE_ENABLE_TERMINAL
    if (app->termDock)
        for (Terminal* t : app->termDock->all()) t->applySettings(st);
#endif
    applyTerminalZoom(app);
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
    PalettePanel::applySettings(g.settings->settings());
    // View > Theme's check, and GTK's own widgets dark or light with it.
    Appearance::settingsChanged(g.gapp, g.settings->settings());
}

// The settings file changed on disk: every window follows.
static GSimpleAction* gIdeLinkAction = nullptr;

static void onSettingsChanged(void*) {
    if (gIdeLinkAction)
        g_simple_action_set_state(gIdeLinkAction,
                                  g_variant_new_boolean(g.settings->settings().ideLink()));
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
            place.page = browserHasKeyboard(a) || (!place.terminal && a->editor->showingPdf());
            place.language = Commands::languageOfPath(a->editor->currentPath());
        }
    }
    Keys::setPlace(place);
}

static void onFocusWidget(GObject*, GParamSpec*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    updateKeyPlace();
    updateMediaKey(app);
    if (!app->dead && app->editor)   // focus outside the editor closes the inline agent's bar
        app->editor->inlineAgent()->focusMoved(gtk_root_get_focus(GTK_ROOT(app->window)));
    updatePageZoom(app);   // the browser's page took or left the keyboard
    if (!app->dead && app->persist)   // focus-change auto-save
        app->persist->focusMoved(gtk_root_get_focus(GTK_ROOT(app->window)));
    if (!app->dead && app->git) app->git->updateActions();   // the panel's list has the keyboard
}

// Coming back to a window is when the Mac looks for changes made elsewhere
// (windowDidBecomeKey); the file monitor catches most of them sooner.
static void onIsActive(GObject* w, GParamSpec*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    updateKeyPlace();
    if (!app->dead && gtk_window_is_active(GTK_WINDOW(w))) {
        app->editor->checkExternalChange();
        if (app->lsp) app->lsp->lookAgainForMissingServer();   // installed elsewhere
        if (app->git && app->gitShown) app->git->refresh();   // commits made elsewhere
        if (app->gitGutter) app->gitGutter->refreshSoon();     // the index, staged elsewhere
        if (app->treeColors) app->treeColors->lookSoon();      // files the tree has not listed
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
    StartupLog::mark("settings read");
    // The dots and the titles' count follow the board (PARITY.md 3.6).
    TerminalAttention::setChangedHandler([] {
        for (App* a : g.windows) {
            if (a->dead) continue;
#ifdef MINICODE_ENABLE_TERMINAL
            if (a->termDock)
                for (Terminal* t : a->termDock->all())
                    t->setAttention(TerminalAttention::markName(a->controlId),
                                    TerminalAttention::tooltip(a->controlId));
#endif
            if (a->editor) updateTitle(a);
        }
    });
    applyStylesheet();   // before any widget is drawn
    StartupLog::mark("stylesheet");

    GSimpleAction* quit = g_simple_action_new("quit", nullptr);
    g_signal_connect(quit, "activate", G_CALLBACK(act_quit), nullptr);
    g_action_map_add_action(G_ACTION_MAP(g.gapp), G_ACTION(quit));
    g_object_unref(quit);
    GSimpleAction* hidden = g_simple_action_new_stateful(
        "showhidden", nullptr, g_variant_new_boolean(g.showHidden));
    g_signal_connect(hidden, "change-state", G_CALLBACK(onShowHiddenChanged), nullptr);
    g_action_map_add_action(G_ACTION_MAP(g.gapp), G_ACTION(hidden));
    g_object_unref(hidden);

    // View > Claude Code IDE Link: writes agent.ide-link, which every window
    // follows (the Mac's MCIdeLinkMenuItem). The check mark follows the file.
    gIdeLinkAction = g_simple_action_new_stateful(
        "idelink", nullptr, g_variant_new_boolean(g.settings->settings().ideLink()));
    g_signal_connect(gIdeLinkAction, "change-state",
                     G_CALLBACK(+[](GSimpleAction*, GVariant* value, gpointer) {
                         std::string why;
                         if (!g.settings->writeValue("agent.ide-link",
                                                     g_variant_get_boolean(value) ? "on" : "off", &why) &&
                             !g.windows.empty())
                             flashStatus(g.windows.back(), why);
                     }),
                     nullptr);
    g_action_map_add_action(G_ACTION_MAP(g.gapp), G_ACTION(gIdeLinkAction));
    g_object_unref(gIdeLinkAction);

    Zoom::applySettings(g.settings->settings());   // the zoom from last time, before any view
    Keys::init(g.gapp, onKeysChanged);
    // minicode's own writes to the settings file (a theme) wait while any
    // window's tab holds it with unsaved changes; the reason goes to the
    // front window's status bar (PARITY.md 2.11, the Mac's unsavedTextCheck).
    AppSettings::unsavedTextCheck = [](const std::string& path) {
        char want[PATH_MAX];
        const std::string resolved = realpath(path.c_str(), want) ? want : path;
        for (App* a : g.windows) {
            if (a->dead || !a->editor) continue;
            for (Document* d : a->editor->documents()) {
                if (!d->dirty()) continue;
                char got[PATH_MAX];
                if (d->path() == path ||
                    (realpath(d->path().c_str(), got) && resolved == got))
                    return true;
            }
        }
        return false;
    };
    Appearance::installThemeAction(g.gapp, Appearance::ThemeHost{
        g.settings,
        [] {
            App* a = appForWindow(gtk_application_get_active_window(g.gapp));
            if ((!a || a->dead) && !g.windows.empty()) a = g.windows.back();
            return a && !a->dead ? GTK_WINDOW(a->window) : nullptr;
        },
        [](const std::string& note) {
            App* a = appForWindow(gtk_application_get_active_window(g.gapp));
            if ((!a || a->dead) && !g.windows.empty()) a = g.windows.back();
            if (a && !a->dead) flashStatus(a, note);
        }});
    // The menu bar, with View > Adjust Settings and View > Theme after the
    // registry's View commands.
    GMenuModel* bar = Keys::menuBar(Appearance::extendMenu);
    gtk_application_set_menubar(g.gapp, bar);
    g_object_unref(bar);
    // The tree's right-click menu, in the Mac's order and grouping, ending
    // with Adjust Settings… for the tree's keys.
    g.treeMenu = Keys::menuOf({"explorer.newFile", "explorer.newFolder", "", "renameFile",
                               "moveFileToTrash", "", "revealFileInOS", "copyFilePath"});
    Appearance::addItem(G_MENU(g.treeMenu), "tree");
    StartupLog::mark("keys and the menu bar");
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

    std::vector<gpointer> owners = {app,         app->lsp,     app->tree,   app->git,
                                    app->layout, app->palette, app->findBar};
    for (void* o : app->editor->handlerOwners()) owners.push_back(o);
#ifdef MINICODE_ENABLE_TERMINAL
    owners.push_back(app->termDock);
    if (app->termDock)
        for (Terminal* t : app->termDock->all()) owners.push_back(t);
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
    app->editor->onActiveWillChange = nullptr;
    app->editor->onDocumentsChanged = nullptr;
    app->editor->onDiffViewMade = nullptr;
    Zoom::unlisten(app->zoomListener);
    app->editor->setLinkHandler(nullptr);
    app->editor->setExternalChangeCallback(nullptr, nullptr);
    delete app->search;
    app->search = nullptr;
    delete app->todos;
    app->todos = nullptr;
    delete app->palette;   // before the editor it previews symbols in
    app->palette = nullptr;
    delete app->nav;       // before the editor whose buffers it watches
    app->nav = nullptr;
    FolderConsent::unlisten(app->consentListener);
    if (app->git) app->git->onRefreshed = nullptr;
    delete app->gitGutter;   // before the editor whose view and buffers it watches
    app->gitGutter = nullptr;
    delete app->treeColors;  // before the tree it colors
    app->treeColors = nullptr;
    if (app->tree) app->tree->setDecorator(nullptr);
    delete app->ide;       // before the editor whose tabs and selection it serves
    app->ide = nullptr;
    delete app->format;    // before the session and the editor it asks
    app->format = nullptr;
    delete app->problems;  // before the session it lists
    app->problems = nullptr;
    delete app->consentBar;
    app->consentBar = nullptr;
    delete app->lsp;
    app->lsp = nullptr;
#ifdef MINICODE_ENABLE_TERMINAL
    delete app->termDock;
    app->termDock = nullptr;
    app->terminal = nullptr;
#endif
#ifdef MINICODE_ENABLE_BROWSER
    delete app->browser;
    app->browser = nullptr;
#endif
    if (app->procsShown) ProcessMonitor::shared().setPanelShown(false);
    delete app->procs;
    app->procs = nullptr;
    delete app->worktrees;   // before the model it reads
    app->worktrees = nullptr;
    if (app->worktreeMenu) g_object_unref(app->worktreeMenu);
    app->worktreeMenu = nullptr;
    delete app->agents;
    app->agents = nullptr;
    delete app->git;
    app->git = nullptr;
    delete app->layout;
    app->layout = nullptr;
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
    static int nextControlId = 1;
    app->controlId = nextControlId++;
    app->rootDir = root;
    g.windows.push_back(app);

    app->window = gtk_application_window_new(g.gapp);
    gtk_widget_add_css_class(app->window, "minicode-window");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1100, 720);
    gtk_window_set_title(GTK_WINDOW(app->window), "MiniCode");
    gtk_application_window_set_show_menubar(
        GTK_APPLICATION_WINDOW(app->window), TRUE);
    // The keyboard back where it was after a choice from the menu bar.
    MenuBarFocus::attach(GTK_WINDOW(app->window));

    // Core widgets.
    app->tree = new FileTree(app->rootDir, g.showHidden);
    StartupLog::mark("window: file tree");
    app->tree->setOpenCallback(treeOpenCb, app);
    app->tree->setDropCallback([app](const std::vector<std::string>& paths,
                                     const std::string& dir, bool move) {
        if (!app->dead) dropOnTree(app, paths, dir, move);
    });
    // The tree's folder monitors see changes in the work tree; the Source
    // Control panel looks again when it is showing.
    app->tree->setActivityCallback([app](const std::string& path) {
        if (app->dead) return;
        if (app->git && app->gitShown) app->git->refresh();
        if (app->treeColors) app->treeColors->changedOnDisk(path);
    });
    // The rows' git colors (TreeGitColors.h).
    app->treeColors = new TreeGitColors();
    app->tree->setDecorator([app](const std::string& path, bool isDir, std::string* tip) {
        if (app->dead || !app->treeColors) return (const char*)nullptr;
        const Git::Decoration d = app->treeColors->decorationFor(path, isDir);
        *tip = TreeGitColors::tooltip(d);
        return TreeGitColors::cssClass(d);
    });
    app->treeColors->onChange = [app] {
        if (!app->dead && app->tree) app->tree->redecorate();
    };
    app->treeColors->setRoot(app->rootDir);
    setupWorktrees(app);
    app->editor = new Editor();
    StartupLog::mark("window: editor");
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
    // Go Back and Go Forward: every tab switch is a jump (NavHistoryUi.h).
    app->nav = new NavHistoryUi(app->editor, NavHistoryUi::Host{
        [app](const std::string& path) {
            if (app->dead) return false;
            openFileThen(app, path, EditorTabs::OpenFrom::Other, nullptr);
            return app->editor->currentPath() == path;
        },
        [app] {
            if (!app->dead) gtk_widget_error_bell(app->window);
        }});
    app->editor->onActiveWillChange = [app] {
        if (app->dead) return;
        if (app->nav) app->nav->tabWillSwitch();
        // A Ctrl+S waiting for its formatter saves the tab as it leaves.
        if (app->format) app->format->tabLeaving();
        app->editor->inlineAgent()->closeFromElsewhere();
    };
    app->editor->onActiveChanged = [app] {
        if (app->dead) return;
        if (app->nav) app->nav->tabDidSwitch();
        revealActiveInTree(app);
        refreshHints(app);
        if (app->persist) app->persist->tabShown();
        if (app->git) app->git->updateActions();   // Discard Changes acts on the file on show
    };
    // The zoom and word wrap are every window's (Zoom.h): the editor's text
    // follows the stylesheet by itself, its preview's tags and the
    // terminal's font here.
    Zoom::attachEditor(app->editor->textView());
    Zoom::scaleTags(gtk_text_buffer_get_tag_table(app->editor->buffer()));
    app->zoomListener = Zoom::listen([app](const std::string& what) {
        if (app->dead) return;
        if (what == "editor") Zoom::scaleTags(gtk_text_buffer_get_tag_table(app->editor->buffer()));
        else if (what == "terminal") applyTerminalZoom(app);
        else if (what == "wrap") app->editor->applyWordWrap();
        refreshHints(app);
    });
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
        },
        [app](const Document* d) { return !app->dead && app->format && app->format->saveWaits(d); },
        [app](int* active) {
            std::vector<TabSessions::BrowserTab> tabs;
#ifdef MINICODE_ENABLE_BROWSER
            if (!app->dead && app->browser) tabs = app->browser->records(active);
#else
            (void)app;
            (void)active;
#endif
            return tabs;
        }});
    app->editor->onDocumentsChanged = [app](Document* d) {
        if (!app->dead && app->persist) app->persist->changed(d);
    };
    // View > Folding's actions, and its notes (Folds.h).
    app->editor->folds()->addActions(G_ACTION_MAP(app->window));
    app->editor->folds()->onNote = [app](const std::string& note) {
        if (!app->dead) flashStatus(app, note);
    };

    // The editor's side: the editor (its tab strip and slot), with the find
    // and replace bar hanging over its text (FindBar.h); it stays with the
    // editor when the browser is beside it.
    GtkWidget* editorPane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    app->findBar = new FindBar(app->editor);
    app->findBar->onNote = [app](const std::string& note) {
        if (!app->dead) flashStatus(app, note);
    };
    app->editor->carets()->onNote = [app](const std::string& note) {
        if (!app->dead) flashStatus(app, note);
    };
    gtk_box_append(GTK_BOX(editorPane), app->editor->widget());
    StartupLog::mark("window: sessions and the find bar");

    GtkWidget* browserPane = nullptr;
#ifdef MINICODE_ENABLE_BROWSER
    app->browser = new Browser(MINICODE_BROWSER_HOME);
    // The last tab closed hides the pane; any change to the tabs is for the
    // session (a second later, once it has settled).
    app->browser->onEmpty = [app] {
        if (!app->dead && app->layout) app->layout->hideBrowser();
    };
    app->browser->onChanged = [app] {
        if (!app->dead && app->persist) app->persist->changed(nullptr);
    };
    browserPane = app->browser->widget();
    StartupLog::mark("window: browser");
#endif
    GtkWidget* terminalPane = nullptr;
#ifdef MINICODE_ENABLE_TERMINAL
    app->termDock = new TerminalDock([app] { return makeTerminal(app); });
    app->terminal = app->termDock->active();
    app->termDock->onActiveChanged = [app](Terminal* t) { app->terminal = t; updateKeyPlace(); };
    app->termDock->onEmpty = [app] {
        if (!app->dead && app->layout) app->layout->hideTerminal();
    };
    app->termDock->confirmClose = [app](Terminal* t, std::function<void()> proceed) {
        askCloseBusyTerminal(app, t, std::move(proceed));
    };
    terminalPane = app->termDock->widget();
    StartupLog::mark("window: terminal");
#endif

    // The sidebar beside the editor, the browser to the editor's right and
    // the terminal below both, as on the Mac (WindowLayout.h). The terminal
    // and the browser start hidden.
    app->sidebar = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(app->sidebar), app->tree->widget(), "tree");
    app->layout = new WindowLayout(app->sidebar, editorPane, app->editor->strip()->widget(),
                                   browserPane, terminalPane);
    app->layout->focusEditor = [app] {
        if (!app->dead) app->editor->focusActiveDocument();
    };
    app->layout->focusBrowser = [app] {
#ifdef MINICODE_ENABLE_BROWSER
        if (!app->dead && app->browser) app->browser->focusUrlBar();
#endif
    };
    app->layout->focusTerminal = [app] {
#ifdef MINICODE_ENABLE_TERMINAL
        if (!app->dead && app->terminal) app->terminal->focus();
#endif
    };
    app->layout->focusTree = [app] {
        if (app->dead) return;
        if (app->gitShown && app->git) app->git->focus();
        else app->tree->focus();
    };
    app->layout->onChanged = [app] {
        if (!app->dead) refreshHints(app);
    };
    StartupLog::mark("window: layout");

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
    gtk_box_append(GTK_BOX(statusBar), app->editor->largeFileNote());   // LargeFileView.h
    gtk_box_append(GTK_BOX(statusBar), app->lspLabel);
    gtk_box_append(GTK_BOX(statusBar), app->settingsLabel);
    gtk_box_append(GTK_BOX(statusBar), app->hintsHint);
    app->statusBar = statusBar;

    // Adjust Settings… at the end of each part's right click menu
    // (Appearance.h): the editor's (the Markdown page's while one shows),
    // the tab strip's empty part (a tab's own menu adds it in Tabs.cpp), and
    // the status bar's. The tree's is in g.treeMenu, the Source Control
    // panel's is added when it is made, the terminal's and the browser's in
    // their own files.
    Appearance::attachEditorMenu(app->editor->textView(), [app]() -> std::string {
        if (app->dead || !app->editor) return "editor";
        return app->editor->isMarkdown() && app->editor->inPreview() ? "markdown" : "editor";
    });
    Appearance::attachMenu(app->editor->strip()->widget(), "tabs");
    Appearance::attachMenu(statusBar, "statusbar");

    // The hints panel floats over the panes rather than displacing them, so it
    // needs a GtkOverlay. The status bar stays outside it: a long list then
    // stops above the bar, and the bar's "Hide shortcuts" stays in sight.
    GtkWidget* overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), app->layout->widget());
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), buildHintsPanel(app));
    // The palette over the panes and the hints, hidden until opened.
    app->palette = new PalettePanel(GTK_WINDOW(app->window), GTK_OVERLAY(overlay), app->editor);
    // What it previews is no jump; Go to Line and a symbol are, from where
    // the editor was when it opened.
    app->palette->onOpenChanged = [app](bool open) {
        if (app->dead || !app->nav) return;
        if (open) app->nav->paletteOpened();
        else app->nav->paletteClosed();
    };
    app->palette->aroundMove = [app](const std::function<void()>& body) {
        if (app->dead || !app->nav) body();
        else app->nav->paletteJump(body);
    };
    gtk_widget_set_vexpand(overlay, TRUE);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(vbox), overlay);
    gtk_box_append(GTK_BOX(vbox), statusBar);
    gtk_window_set_child(GTK_WINDOW(app->window), vbox);
    StartupLog::mark("window: status bar, hints and palette");

    // Language servers: one session for the window, told about every file the
    // editor opens, saves and edits through the editor's observer hook.
    app->lsp = new LspSession(app->editor, app->rootDir, g.settings->settings());
    app->lsp->onStatus = [app](const std::string& text) {
        gtk_label_set_text(GTK_LABEL(app->lspLabel), text.c_str());
        if (!app->dead && app->consentBar) app->consentBar->refresh();
        if (!app->dead && app->problems) app->problems->update();
    };
    // The Problems list above the terminal, and its counts in the status bar
    // left of the servers' line (PARITY.md 2.2).
    app->problems = new ProblemsPanel(app->lsp, ProblemsPanel::Host{
        [app] { return app->rootDir; },
        [app](bool shown) {
            if (!app->dead) app->layout->setProblemsShown(shown);
        },
        [app] {
            if (!app->dead) app->editor->focusActiveDocument();
        },
        [app] {
            if (!app->dead) app->layout->focusAfterClosingPanel();
        }});
    app->layout->setProblemsPane(app->problems->widget());
    gtk_box_insert_child_after(GTK_BOX(statusBar), app->problems->statusLabel(),
                               app->editor->largeFileNote());
    app->lsp->onProblemsChanged = [app] {
        if (!app->dead && app->problems) app->problems->update();
    };
    // The folder's own tools (PARITY.md 2.3): the bar under the tabs asks
    // about them for the servers, and Format Document asks for its own.
    app->consentBar = new ConsentBar(ConsentBar::Host{
        [app] { return app->rootDir; },
        [app] { return app->dead ? std::string() : app->lsp->folderEnvironmentAwaitingConsent(); },
        [app] { return app->dead ? std::string() : app->lsp->folderCodeAwaitingConsent(); },
        [app] { return app->dead ? std::string() : app->lsp->folderCodeQuestion(); },
        [app] { return app->dead ? std::string() : app->lsp->missingServer(); },
        [app] { return app->dead ? std::string() : app->lsp->currentServerKey(); },
        [](const std::string& key) {
            return g.settings ? g.settings->settings().lspCommand(key) : std::string();
        },
        [app](const std::string& command) {
#ifdef MINICODE_ENABLE_TERMINAL
            if (app->dead || !app->terminal) return;
            if (!app->layout->terminalShown()) app->layout->toggleTerminal();
            app->terminal->typeText(command);   // never Return: the user reads it first
#else
            (void)app; (void)command;
#endif
        }});
    app->editor->addBar(app->consentBar->widget());
    app->format = new Formatting(GTK_WINDOW(app->window), app->editor, app->lsp, Formatting::Host{
        [app] { return app->rootDir; },
        [app](Document* d) {
            if (!app->dead) saveDocument(app, d);
        },
        [app](const std::string& command) {
#ifdef MINICODE_ENABLE_TERMINAL
            if (app->dead || !app->terminal) return;
            if (!app->layout->terminalShown()) app->layout->toggleTerminal();
            app->terminal->typeText(command);   // never Return: the user reads it first
#else
            (void)app; (void)command;
#endif
        }});
    app->consentListener = FolderConsent::listen([app](const std::string&) {
        updateFolderToolsAction(app);
    });
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
    StartupLog::mark("window: language server session");

    // Actions. The menu bar and the accelerators are the application's
    // (initShared); these are what they name in this window.
    addAction(app, "newwindow",      G_CALLBACK(act_new_window));
    addAction(app, "close",          G_CALLBACK(act_close_window));
    addAction(app, "complete",       G_CALLBACK(act_complete));
    addAction(app, "definition",     G_CALLBACK(act_definition));
    addAction(app, "hoverinfo",      G_CALLBACK(act_hover));
    addAction(app, "inlineagent",    G_CALLBACK(act_inline_agent));
    addAction(app, "renamesymbol",   G_CALLBACK(act_rename_symbol));
    addAction(app, "references",     G_CALLBACK(act_references));
    addAction(app, "quickfix",       G_CALLBACK(act_quick_fix));
    addAction(app, "organizeimports", G_CALLBACK(act_organize_imports));
    addAction(app, "problems",       G_CALLBACK(act_toggle_problems));
    addAction(app, "nextproblem",    G_CALLBACK(act_next_problem));
    addAction(app, "previousproblem", G_CALLBACK(act_previous_problem));
    addAction(app, "gotobracket",    G_CALLBACK(act_go_to_bracket));
    addAction(app, "open",           G_CALLBACK(act_open));
    addAction(app, "save",           G_CALLBACK(act_save));
    addAction(app, "exportpdf",      G_CALLBACK(act_export_pdf));
    addAction(app, "exporthtml",     G_CALLBACK(act_export_html));
    addAction(app, "latexsynctex",   G_CALLBACK(act_latex_synctex));
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
    addAction(app, "togglesidepreview", G_CALLBACK(act_toggle_side_preview));
    addAction(app, "togglehints",    G_CALLBACK(act_toggle_hints));
    addAction(app, "togglesidebar",  G_CALLBACK(act_toggle_sidebar));
    addAction(app, "toggleeditor",   G_CALLBACK(act_toggle_editor));
    addAction(app, "toggleterminal", G_CALLBACK(act_toggle_terminal));
    addAction(app, "runfile",        G_CALLBACK(act_run_file));
    addAction(app, "togglebrowser",  G_CALLBACK(act_toggle_browser));
    addAction(app, "newbrowsertab",  G_CALLBACK(act_new_browser_tab));
    addAction(app, "togglegit",      G_CALLBACK(act_toggle_git));
    addAction(app, "toggleprocesses", G_CALLBACK(act_toggle_processes));
    addAction(app, "toggleagents", G_CALLBACK(act_toggle_agents));
    addAction(app, "focustree",      G_CALLBACK(act_focus_tree));
    addAction(app, "focuseditor",    G_CALLBACK(act_focus_editor));
    addAction(app, "focusgroup2",    G_CALLBACK(act_focus_group2));
    addAction(app, "spliteditor",    G_CALLBACK(act_split_editor));
    addAction(app, "moveeditornext", G_CALLBACK(act_move_editor_next));
    addAction(app, "moveeditorprevious", G_CALLBACK(act_move_editor_previous));
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
    addAction(app, "editorzoomin",   G_CALLBACK(act_editor_zoom_in));
    addAction(app, "editorzoomout",  G_CALLBACK(act_editor_zoom_out));
    addAction(app, "editorzoomreset", G_CALLBACK(act_editor_zoom_reset));
    addAction(app, "terminalzoomin", G_CALLBACK(act_terminal_zoom_in));
    addAction(app, "terminalzoomout", G_CALLBACK(act_terminal_zoom_out));
    addAction(app, "terminalzoomreset", G_CALLBACK(act_terminal_zoom_reset));
    addAction(app, "togglewordwrap", G_CALLBACK(act_toggle_word_wrap));
    addAction(app, "togglemaximize", G_CALLBACK(act_toggle_maximize));
#ifdef MINICODE_ENABLE_TERMINAL
    addAction(app, "nextwaiting",    G_CALLBACK(act_next_waiting));
    addAction(app, "newterminal",    G_CALLBACK(act_new_terminal));
    addAction(app, "nextterminal",   G_CALLBACK(act_next_terminal));
    addAction(app, "previousterminal", G_CALLBACK(act_previous_terminal));
    addAction(app, "closeterminal",  G_CALLBACK(act_close_terminal));
    addAction(app, "splitterminal",  G_CALLBACK(act_split_terminal));
    addAction(app, "splitterminaldown", G_CALLBACK(act_split_terminal_down));
    addAction(app, "resizeterminalleft", G_CALLBACK(act_resize_terminal_left));
    addAction(app, "resizeterminalright", G_CALLBACK(act_resize_terminal_right));
    addAction(app, "resizeterminalup", G_CALLBACK(act_resize_terminal_up));
    addAction(app, "resizeterminaldown", G_CALLBACK(act_resize_terminal_down));
    addAction(app, "renameterminal", G_CALLBACK(act_rename_terminal));
    addAction(app, "moveterminalleft", G_CALLBACK(act_move_terminal_left));
    addAction(app, "moveterminalright", G_CALLBACK(act_move_terminal_right));
#endif
    addAction(app, "goback",         G_CALLBACK(act_go_back));
    addAction(app, "goforward",      G_CALLBACK(act_go_forward));
    addAction(app, "playpause",      G_CALLBACK(act_play_pause));
    addAction(app, "showcommands",   G_CALLBACK(act_show_commands));
    addAction(app, "quickopen",      G_CALLBACK(act_quick_open));
    addAction(app, "gotoline",       G_CALLBACK(act_go_to_line));
    addAction(app, "gotosymbol",     G_CALLBACK(act_go_to_symbol));
    addAction(app, "workspacesymbol", G_CALLBACK(act_workspace_symbol));
    {   // Adjust Settings…, its target the part of the window (Appearance.h)
        GSimpleAction* a = g_simple_action_new(Appearance::kAction, G_VARIANT_TYPE_STRING);
        g_signal_connect(a, "activate", G_CALLBACK(act_adjust_settings), app);
        g_action_map_add_action(G_ACTION_MAP(app->window), G_ACTION(a));
        g_object_unref(a);
    }
    // Several carets and the line commands (Carets.h).
    Carets::addActions(G_ACTION_MAP(app->window), app->editor);
    addAction(app, "formatdocument", G_CALLBACK(act_format_document));
    addAction(app, "forgetfoldertools", G_CALLBACK(act_forget_folder_tools));
    updateFolderToolsAction(app);
    makeGitPanel(app);   // and View > Source Control's actions
    for (const char* off : {"zoomin", "zoomout", "zoomfit", "playpause"})   // until shown
        g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(
                                        G_ACTION_MAP(app->window), off)), FALSE);
#ifndef MINICODE_ENABLE_TERMINAL
    for (const char* off : {"terminalzoomin", "terminalzoomout", "terminalzoomreset"})   // no terminal
        g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(
                                        G_ACTION_MAP(app->window), off)), FALSE);
#endif
    app->tree->setContextMenu(g.treeMenu);
    // Rename and Move to Trash (F2 and Delete) only while the tree has the
    // keyboard. As window accelerators they would take Delete from the editor.
    Keys::addTreeKeys(app->tree->widget());
    g_signal_connect(app->window, "close-request", G_CALLBACK(onCloseRequest), app);
    g_signal_connect(app->window, "notify::focus-widget", G_CALLBACK(onFocusWidget), app);
    g_signal_connect(app->window, "notify::is-active", G_CALLBACK(onIsActive), app);
    {
        IdeLinkHost ih;
        ih.editor = app->editor;
        ih.window = GTK_WINDOW(app->window);
        ih.root = app->rootDir;
        ih.problems = [app] { return app->lsp ? app->lsp->problemRows() : std::vector<LspProblem>(); };
        ih.note = [app](const std::string& text) { if (!app->dead) flashStatus(app, text); };
        app->ide = new IdeLinkServer(ih);
    }
    applyWindowSettings(app);   // now that the editor, terminal and status bar exist
    StartupLog::mark("window: actions and settings applied");
    app->persist->restoreSession();   // the files open here last time
#ifdef MINICODE_ENABLE_BROWSER
    if (app->browser) {   // the browser's tabs, suspended; the one on show loads when the pane is
        const TabSessions::Session saved = app->persist->savedSession();
        app->browser->restore(saved.browser, saved.browserActive);
    }
#endif
    StartupLog::markf("window: session restored (%d tabs)", app->editor->tabCount());

    // A file named on the command line opens before the window is shown, so it
    // is already rendered when the window appears rather than flashing the
    // welcome text first. Markdown arrives rendered, the same as a click in the
    // sidebar would give (Editor::openFile decides that).
    if (!file.empty()) openFileCb(file, app);

    StartupLog::watchFrames(app->window);
    gtk_window_present(GTK_WINDOW(app->window));
    StartupLog::mark("window: presented");
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

// ------------------------------------------------------ the control channel
// PARITY.md 3.1: the socket (ControlServer.cpp) and the handlers
// (ControlCommands.cpp), which see the windows through this.
static ControlServer* gControl = nullptr;

static ControlWindow controlWindowOf(App* a) {
    ControlWindow w;
    w.id = a->controlId;
    w.folder = a->rootDir;
    w.editor = a->editor;
    w.window = GTK_WINDOW(a->window);
#ifdef MINICODE_ENABLE_BROWSER
    w.browser = a->browser;
#endif
    w.shellPid = a->terminal ? a->terminal->shellPid() : 0;
    if (a->terminal) w.shell = a->terminal->shellState();
    return w;
}

static App* controlApp(int id) {
    for (App* a : g.windows)
        if (!a->dead && a->controlId == id) return a;
    return nullptr;
}

// Starts the channel once, with the first window.
static void startControlChannel() {
    if (gControl) return;
    gControl = new ControlServer;
    ControlHost host;
    host.windows = [] {
        std::vector<ControlWindow> out;
        for (App* a : g.windows)
            if (!a->dead && a->editor) out.push_back(controlWindowOf(a));
        return out;
    };
    host.frontId = [] {
        App* a = appForWindow(gtk_application_get_active_window(g.gapp));
        if ((!a || a->dead) && !g.windows.empty()) a = g.windows.back();
        return a && !a->dead ? a->controlId : 0;
    };
    host.openWindow = [](const std::string& folder, ControlWindow* out) {
        App* a = newWindow(folder, "");
        if (!a) return false;
        *out = controlWindowOf(a);
        return true;
    };
    host.present = [](const ControlWindow& w) {
        if (App* a = controlApp(w.id)) gtk_window_present(GTK_WINDOW(a->window));
    };
    host.showFile = [](const ControlWindow& w, const std::string& path) {
        if (App* a = controlApp(w.id))
            openFileThen(a, path, EditorTabs::OpenFrom::Other, nullptr);
    };
    host.showBrowser = [](const ControlWindow& w) {
        if (App* a = controlApp(w.id)) a->layout->showBrowser();
    };
    host.pageAnyOrigin = [] { return g.settings->settings().pageAnyOrigin(); };
    host.openUrl = [](const ControlWindow& w, const std::string& url) {
        if (App* a = controlApp(w.id)) openLink(a, true, url, false, false, 0, 0);
    };
    host.showTerminal = [](const ControlWindow& w) {
        if (App* a = controlApp(w.id)) showTerminalQuietly(a);
    };
    host.newTerminal = [](const ControlWindow& w, const std::string& title) {
#ifdef MINICODE_ENABLE_TERMINAL
        App* a = controlApp(w.id);
        return a && newTerminalTab(a, title, false) != nullptr;
#else
        (void)w; (void)title;
        return false;
#endif
    };
    host.splitTerminal = [](const ControlWindow& w, const std::string& title) {
#ifdef MINICODE_ENABLE_TERMINAL
        App* a = controlApp(w.id);
        Terminal* t = a ? splitTerminalPane(a, false, false) : nullptr;
        if (t && !title.empty()) t->setUserTitle(title);
        return t != nullptr;
#else
        (void)w; (void)title;
        return false;
#endif
    };
    host.terminalRun = [](const ControlWindow& w, const std::string& line,
                          std::function<void(const std::string&)> done) {
        App* a = controlApp(w.id);
        if (!a) {
            done("the window was closed");
            return;
        }
        runInTerminal(a, line, std::move(done));
    };
    host.terminalOutput = [](int id, int lines, std::string* text) {
#ifdef MINICODE_ENABLE_TERMINAL
        App* a = controlApp(id);
        if (!a || !a->terminal) return false;
        *text = a->terminal->made() ? a->terminal->recentText(lines) : std::string();
        return true;
#else
        (void)id; (void)lines; (void)text;
        return false;
#endif
    };
    registerControlCommands(gControl->dispatcher(), host);
    // The process sampler (PARITY.md 3.8): each window's folder as realpath
    // spells it, and its terminal by the shell's pid. Window ids stand for
    // terminal ids (a window has one terminal).
    ProcessMonitor::shared().start([](std::vector<Procs::Terminal>& terms,
                                      std::vector<std::string>& folders) {
        for (App* a : g.windows) {
            if (a->dead || !a->editor) continue;
            char buf[PATH_MAX];
            folders.push_back(realpath(a->rootDir.c_str(), buf) ? std::string(buf) : a->rootDir);
            Procs::Terminal t;
            t.id = a->controlId;
            t.title = "terminal";
            t.shellPid = a->terminal ? a->terminal->shellPid() : 0;
            t.window = a->controlId;
            // R7.4: the session `minicode notify` last named here, and when
            // (as process start times count: microseconds since 1970).
            std::string sid, transcript;
            double when = 0;
            if (TerminalAttention::sessionOf(a->controlId, &sid, &transcript, &when) && !sid.empty()) {
                t.session = sid;
                t.sessionNamedAt = (int64_t)(when * 1e6);
            }
#ifdef MINICODE_ENABLE_TERMINAL
            if (a->termDock && a->termDock->all().size() > 1) {
                // Every tab's shell is tracked (PARITY.md 4.7). Ids stand for
                // terminals only on the window's first; the others are offset
                // from it and reveal nothing by themselves.
                t.shellPid = 0;
                const std::vector<Terminal*> panes = a->termDock->all();
                for (size_t i = 0; i < panes.size(); i++) {
                    Procs::Terminal u = t;
                    u.id = a->controlId + (int)i * 100000;
                    u.title = panes[i]->title();
                    u.shellPid = panes[i]->shellPid();
                    if (i > 0) { u.session.clear(); u.sessionNamedAt = 0; }
                    terms.push_back(u);
                }
                continue;
            }
#endif
            terms.push_back(t);
        }
    });
    std::string error;
    if (!gControl->start(&error)) g_printerr("minicode: %s\n", error.c_str());
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
    StartupLog::mark("command line");
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
    startControlChannel();        // once, with the first window
    return 0;
}

// The client half of the control channel (tools/minicode-cli.cpp): `minicode
// open|window|state|selection ...` talks to the running minicode over its
// socket and exits, with no GTK started (PARITY.md 3.1).
int minicodeCliMain(int argc, char** argv);
int minicodeInstallSkill(int argc, char** argv);

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "install-skill") return minicodeInstallSkill(argc, argv);
    if (argc > 1 && ControlChannel::isSubcommand(argv[1])) return minicodeCliMain(argc, argv);
#ifndef MINICODE_GUITEST
    // A run with the startup log stays in the terminal it was asked from: a
    // detached app gives that terminal up, and the log with it (Detach.h).
    const bool logStays = StartupLog::wanted() && !g_getenv("MINICODE_FOREGROUND");
    if (logStays) g_setenv("MINICODE_FOREGROUND", "1", TRUE);
    // The options, and a terminal given back at once (Detach.h, PARITY.md R2).
    if (const int status = Detach::start(&argc, &argv); status >= 0) return status;
    if (logStays) g_unsetenv("MINICODE_FOREGROUND");   // not for the shells it starts
#endif
    // MINICODE_STARTUP_LOG=1: each phase's time on stderr. After the fork,
    // so its clock is this process's main thread's.
    StartupLog::begin();
    GtkApplication* gapp = gtk_application_new("org.minicode.Editor",
                                              G_APPLICATION_HANDLES_COMMAND_LINE);
    g.gapp = gapp;
    // argv[1] = a directory to open, or a file to open; default = cwd. The real
    // argc/argv go to g_application_run so that a remote invocation forwards
    // them to the running instance; onCommandLine is where they are read.
    g_signal_connect(gapp, "command-line", G_CALLBACK(onCommandLine), nullptr);
    // After GtkApplication's own startup, which opens the display and loads
    // the theme.
    g_signal_connect(gapp, "startup", G_CALLBACK(+[](GApplication*, gpointer) {
        StartupLog::mark("gtk: started (the display, the theme, the session bus name)");
    }), nullptr);
    g_signal_connect(gapp, "window-removed", G_CALLBACK(onWindowRemoved), nullptr);
    // Quitting: no language server may outlive the app. The main loop has
    // stopped by now, so this waits on the processes directly.
    g_signal_connect(gapp, "shutdown", G_CALLBACK(+[](GApplication*, gpointer) {
        LspTerminateAllServers();
        BackupStore::shared().flush();   // what the last windows let go is gone from disk
        if (gControl) gControl->stop();   // the socket goes with the app
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
