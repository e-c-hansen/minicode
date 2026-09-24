// main.cpp — MiniCode GTK4 shell.
//
// Layout (mirrors the macOS window: file tree left, editor right, status bar
// at the bottom, collapsible terminal and browser panels):
//
//   GtkApplicationWindow
//     vbox
//       hpaned  (movable divider)
//         [start] FileTree sidebar
//         [end]   right vbox
//                   editor find-bar (GtkSearchBar)
//                   vpaned  (movable divider)
//                     [start] upper vbox
//                               Editor (GtkTextView, vexpand)
//                               browser revealer  (#ifdef MINICODE_ENABLE_BROWSER)
//                     [end]   Terminal panel      (#ifdef MINICODE_ENABLE_TERMINAL)
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
// several (Ctrl+N, or a second `minicode <path>` from a shell): each has its
// own tree, editor, terminal, browser, Find in Folder panel and language
// server session, as each macOS window has its own EditorController. What
// they share lives in `g` below: the settings file and stylesheet, the menu
// bar, the accelerators, and whether dotfiles are shown. The application
// quits when its last window closes, which is GtkApplication's own rule and
// the Mac's applicationShouldTerminateAfterLastWindowClosed. A closing
// window is taken apart in onWindowRemoved while its widgets still exist.
#include <gtk/gtk.h>

#include "AppSettings.h"
#include "Editor.h"
#include "FileTree.h"
#include "ThemeCss.h"
#include "Palette.h"
#include "Terminal.h"
#include "Browser.h"
#include "Search.h"
#include "Lsp.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>
#include <unistd.h>
#include <limits.h>

struct App {
    GtkWidget*      window = nullptr;
    GtkWidget*      hpaned = nullptr;
    GtkWidget*      statusLabel = nullptr;
    GtkWidget*      searchBar = nullptr;
    GtkWidget*      searchEntry = nullptr;
    GtkWidget*      settingsLabel = nullptr;   // first problem in the settings file

    int  sidebarWidth = 240;      // where the sidebar divider was before collapsing
    bool adjustingPaned = false;  // a snap is moving a divider; ignore the notify

    Editor*   editor = nullptr;
    FileTree* tree = nullptr;
    SearchPanel* search = nullptr;   // Find in Folder, created on first use
    LspSession*  lsp = nullptr;      // language servers for this window
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
    GtkWidget* hintsHint  = nullptr;

    std::string rootDir;
    bool sidebarVisible = true;

    bool prompting = false;   // a "Save changes?" alert is up
    bool closing = false;     // the user answered it for a window close

    // Files opened in this window, newest first, for Previous File (the
    // Mac's _recent in EditorController.mm).
    std::vector<std::string> recent;

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
    // The active window's terminal has the keyboard, so plain Ctrl keys are
    // the shell's (see "Keys in the terminal" below).
    bool shellKeys = false;
};
Shared g;
}  // namespace

static App* newWindow(const std::string& root, const std::string& file);

// Defined with the rest of the hints panel further down; the pane toggles call
// it so an open panel never reports stale state.
static void refreshHints(App* app);

// A divider dragged this close to the top (terminal) or the right edge
// (sidebar) snaps the rest of the way.
static const int kTopSnap = 40;
static const int kRightSnap = 60;

static const char* kHintsHintShow = "Ctrl+Shift+H  Shortcuts";
static const char* kHintsHintHide = "Ctrl+Shift+H  Hide shortcuts";

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
    return gtk_widget_get_visible(app->tree->widget()) && maxPos > 0 &&
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

static void updateTitle(void* userp) {
    App* app = static_cast<App*>(userp);
    std::string title = "MiniCode";
    const std::string& p = app->editor->currentPath();
    if (!p.empty()) {
        auto slash = p.find_last_of('/');
        std::string base = slash == std::string::npos ? p : p.substr(slash + 1);
        title = "MiniCode — " + base + (app->editor->dirty() ? " *" : "");
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
    gtk_label_set_text(GTK_LABEL(app->statusLabel), p.empty() ? "Ready" : p.c_str());
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
static bool saveCurrent(App* app) {
    std::string err;
    const bool ok = app->editor->save(&err);
    updateTitle(app);
    if (!ok)
        showError(app, "Could not save “" + baseName(app->editor->currentPath()) + "”",
                  err + "\n\nYour changes are still here, unsaved.");
    return ok;
}

namespace {
struct Pending {
    App* app;
    std::function<void()> proceed;
    std::function<void()> cancelled;
};
}  // namespace

// Runs `proceed` once the current buffer's edits are safe: at once when there
// are none, otherwise after the user picks Save (and the save worked) or Don't
// Save. Cancel, Escape, or a failed save run `cancelled` instead.
static void confirmUnsaved(App* app, std::function<void()> proceed,
                           std::function<void()> cancelled = nullptr) {
    if (!app->editor->dirty()) { proceed(); return; }
    if (app->prompting) { if (cancelled) cancelled(); return; }   // one at a time
    app->prompting = true;

    const std::string name = baseName(app->editor->currentPath());
    GtkAlertDialog* dlg = gtk_alert_dialog_new("Save changes to “%s”?",
                                               name.empty() ? "this file" : name.c_str());
    gtk_alert_dialog_set_detail(dlg, "Your changes will be lost if you don't save them.");
    const char* buttons[] = {"Save", "Don't Save", "Cancel", nullptr};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_default_button(dlg, 0);
    gtk_alert_dialog_set_cancel_button(dlg, 2);
    gtk_alert_dialog_set_modal(dlg, TRUE);

    auto* p = new Pending{app, std::move(proceed), std::move(cancelled)};
    gtk_alert_dialog_choose(dlg, GTK_WINDOW(app->window), nullptr,
        [](GObject* src, GAsyncResult* res, gpointer data) {
            Pending* pd = static_cast<Pending*>(data);
            App* a = pd->app;
            a->prompting = false;
            const int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src),
                                                              res, nullptr);
            if (a->dead) { delete pd; return; }
            bool go = choice == 1;                    // Don't Save
            if (choice == 0) go = saveCurrent(a);     // Save, unless it failed
            if (go) pd->proceed();
            else if (pd->cancelled) pd->cancelled();
            delete pd;
            // A change on disk that arrived while this was up could not be
            // asked about then.
            if (!a->dead) a->editor->checkExternalChange();
        }, p);
    g_object_unref(dlg);
}

// The tree moves its selection on the click, before the question is asked;
// after a Cancel it goes back to the file that is still open.
static void reselectCurrentFile(App* app) {
    const std::string& cur = app->editor->currentPath();
    if (isInside(cur, app->rootDir)) app->tree->revealPath(cur);
    else app->tree->clearSelection();
}

// Previous File's list: newest first, each file once. The Mac adds a file
// when it opened (text, image or PDF), not when it could not be shown.
static void noteRecent(App* app, const std::string& path) {
    auto& r = app->recent;
    r.erase(std::remove(r.begin(), r.end(), path), r.end());
    r.insert(r.begin(), path);
    if (r.size() > 50) r.resize(50);
}

static void openFileNow(App* app, const std::string& path) {
    showEditorArea(app);   // opening a file must not disappear into a hidden pane
    if (app->editor->openFile(path)) noteRecent(app, path);
    updateTitle(app);
    refreshHints(app);   // the Markdown line depends on the open file
}

// A file changed on disk while the buffer has edits of its own: the Mac's
// question, "Keep Mine" or "Reload" (EditorController.mm -checkExternalChange).
// Returns false when another question is already up; the editor asks again
// on its next check.
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

// Open a file, asking "Save changes?" first when the open one has edits. The
// answer may come later, so anything to do once the file is open (go to a
// line, focus the editor) goes in `then`, which runs after the open and not
// at all when the user cancels.
static void openFileThen(App* app, const std::string& path, std::function<void()> then) {
    // Clicking the file that is already open with edits in it must not reload
    // it from disk over them. A clean file does reload, which is how a change
    // made elsewhere gets picked up.
    if (path == app->editor->currentPath() && app->editor->dirty()) {
        showEditorArea(app);
        if (then) then();
        return;
    }
    confirmUnsaved(app, [app, path, then] {
        openFileNow(app, path);
        if (then) then();
    }, [app] { reselectCurrentFile(app); });
}

// Called by FileTree when a file is activated, and by everything else that
// opens a file with nothing to do afterwards.
static void openFileCb(const std::string& path, void* userp) {
    openFileThen(static_cast<App*>(userp), path, nullptr);
}

// Re-root on a new folder. The file from the old folder is closed, as on the
// Mac, so the editor never holds a file the tree no longer shows.
static void openFolder(App* app, const std::string& dir) {
    app->rootDir = dir;
    app->tree->setRoot(dir);
    app->editor->closeFile();
    // After closeFile, so the old file is not reopened under the new root.
    if (app->lsp) app->lsp->setRoot(dir);   // servers belong to a folder
#ifdef MINICODE_ENABLE_TERMINAL
    if (app->terminal) app->terminal->setProjectRoot(dir);   // for its links
#endif
    updateTitle(app);
    refreshHints(app);
}

// Closing the window with unsaved edits asks first. The answer arrives later,
// so the close is refused now and repeated once the edits are dealt with.
static gboolean onCloseRequest(GtkWindow*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (app->closing || !app->editor->dirty()) {
        // The window really is closing: stop its language servers
        // (shutdown, exit, then signals if they linger).
        if (app->lsp) app->lsp->shutdown();
        return FALSE;
    }
    confirmUnsaved(app, [app] {
        app->closing = true;
        gtk_window_close(GTK_WINDOW(app->window));
    });
    return TRUE;
}

// ---------------------------------------------------------------- actions

static void act_open(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    confirmUnsaved(app, [app] {
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
                    confirmUnsaved(a, [a, dir] { openFolder(a, dir); });
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
    gtk_widget_set_visible(app->tree->widget(), app->sidebarVisible);
    refreshHints(app);
}

// Ctrl+H, View > Show Hidden Files: the Mac's Shift+Cmd+. (toggleHiddenFiles:
// flips one global flag). An application action with a check-box state, so
// every window's tree and every window's menu agree.
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

// Ctrl+/ : only when the editor has the keyboard, so the shortcut never edits
// a file you are not looking at.
static void act_toggle_comment(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    GtkWidget* view = app->editor->textView();
    if (!gtk_widget_has_focus(view) || !app->editor->toggleComment())
        gtk_widget_error_bell(view);
}

// Ctrl+, : open the settings file, writing the commented defaults first.
static void act_settings(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (!g.settings->ensureFileExists()) {
        gtk_label_set_text(GTK_LABEL(app->settingsLabel),
                           ("Could not create " + g.settings->path()).c_str());
        return;
    }
    openFileThen(app, g.settings->path(),
                 [app] { gtk_widget_grab_focus(app->editor->textView()); });
}

static void act_find(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    gboolean on = gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(app->searchBar));
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(app->searchBar), !on);
    if (!on) gtk_widget_grab_focus(app->searchEntry);
}

// Defined with the find bar's search further down.
static bool findStep(App* app, bool forward);

// Ctrl+G and Ctrl+Shift+G: the next or previous match of what the find bar
// holds, from the selection, wrapping at either end, as the Mac's Find Next
// and Find Previous do (Command G, Shift Command G). They work with the bar
// closed too. With nothing to look for, they open the bar instead.
static void act_find_next(GSimpleAction*, GVariant*, gpointer userp) {
    findStep(static_cast<App*>(userp), true);
}

static void act_find_previous(GSimpleAction*, GVariant*, gpointer userp) {
    findStep(static_cast<App*>(userp), false);
}

// Ctrl+N: a new window on this window's folder, as the Mac's New Window
// (Command N) opens one on the front window's root.
static void act_new_window(GSimpleAction*, GVariant*, gpointer userp) {
    newWindow(static_cast<App*>(userp)->rootDir, "");
}

// Ctrl+W: this window only, asking "Save changes?" first like any close.
static void act_close_window(GSimpleAction*, GVariant*, gpointer userp) {
    gtk_window_close(GTK_WINDOW(static_cast<App*>(userp)->window));
}

// Ctrl+Q: every window, each asking about its own unsaved edits. One that is
// cancelled stays open, and so does the application with it.
static void act_quit(GSimpleAction*, GVariant*, gpointer) {
    const std::vector<App*> open = g.windows;   // closing edits the list
    for (App* a : open)
        if (!a->dead) gtk_window_close(GTK_WINDOW(a->window));
}

// Ctrl+Tab: the file that was open before this one (the Mac's Previous File,
// Control Tab). Pressed again, it goes back.
static void act_previous_file(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (app->recent.size() < 2) { gtk_widget_error_bell(app->window); return; }
    openFileCb(app->recent[1], app);
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

// Ctrl+1: the editor gets the keyboard, shown first if it was hidden (the
// Mac's Focus Editor, Command 1).
static void act_focus_editor(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    showEditorOverBrowser(app);
    gtk_widget_grab_focus(app->editor->textView());
    refreshHints(app);
}

#ifdef MINICODE_ENABLE_TERMINAL
// Ctrl+click on a link in the terminal (Terminal::Link), the Mac's
// -openTerminalLink:. A URL opens in the browser panel; a file opens in the
// editor at the line and column the reference named; a folder is shown in
// the tree.
static void openTerminalLink(App* app, const Terminal::Link& link) {
    if (link.isUrl) {
#ifdef MINICODE_ENABLE_BROWSER
        if (app->browser && app->browserRevealer) {
            if (!gtk_revealer_get_reveal_child(GTK_REVEALER(app->browserRevealer)))
                act_toggle_browser(nullptr, nullptr, app);
            app->browser->load(link.target);
            return;
        }
#endif
        // Built without the browser panel: the desktop's own browser.
        GtkUriLauncher* launcher = gtk_uri_launcher_new(link.target.c_str());
        gtk_uri_launcher_launch(launcher, GTK_WINDOW(app->window), nullptr, nullptr, nullptr);
        g_object_unref(launcher);
        return;
    }

    const std::string path = link.target;
    if (link.isDir) {
        // Only the tree can show a folder, and only one inside it.
        if (!link.insideRoot) { gtk_widget_error_bell(app->window); return; }
        if (!app->sidebarVisible) act_toggle_sidebar(nullptr, nullptr, app);
        app->tree->revealPath(path);
        return;
    }
    if (link.insideRoot) app->tree->revealPath(path);
    const int line = link.line, column = link.column;
    openFileThen(app, path, [app, path, line, column] {
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
#endif

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
        openFileCb(path, app);   // asks about unsaved edits in the open file first
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
        // The open file keeps its buffer, edits included, under the new name.
        // That holds when a folder above it was renamed, too.
        const std::string cur = app->editor->currentPath();
        if (isInside(cur, src)) {
            app->editor->setPath(dst + cur.substr(src.size()));
            updateTitle(app);
        }
        // Previous File follows the rename, in every window.
        for (App* a : g.windows)
            for (std::string& r : a->recent)
                if (isInside(r, src)) r = dst + r.substr(src.size());
        app->tree->revealPath(dst);
    });
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
    // The open file went with it (or with its folder): back to the welcome
    // text, the way the Mac resets its editor.
    if (isInside(app->editor->currentPath(), path)) {
        app->editor->closeFile();
        updateTitle(app);
        refreshHints(app);
    }
    for (App* a : g.windows) {
        auto& r = a->recent;
        r.erase(std::remove_if(r.begin(), r.end(),
                               [&](const std::string& p) { return isInside(p, path); }),
                r.end());
    }
    app->tree->clearSelection();
}

static void act_trash(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    const std::string path = app->tree->selectedPath();
    if (path.empty()) { gtk_widget_error_bell(app->window); return; }

    GtkAlertDialog* dlg = gtk_alert_dialog_new("Move “%s” to the Trash?",
                                               baseName(path).c_str());
    // The Mac asks the same question. Unsaved edits in the open file go with
    // it, so say so rather than asking "Save changes?" about a file that is
    // on its way out.
    if (app->editor->dirty() && isInside(app->editor->currentPath(), path)) {
        const std::string detail = "The open file “" +
            baseName(app->editor->currentPath()) +
            "” has unsaved changes, which will be lost.";
        gtk_alert_dialog_set_detail(dlg, detail.c_str());
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

// The selected item's absolute path, else the open folder's, as plain text.
static void act_copy_path(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    std::string path = app->tree->selectedPath();
    if (path.empty()) path = app->rootDir;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(app->window), path.c_str());
}

static void act_focus_tree(GSimpleAction*, GVariant*, gpointer userp) {
    static_cast<App*>(userp)->tree->focus();
}

// A Find in Folder match was activated: open the file the way a click in the
// tree does, then go to the match. If the file did not end up open (it could
// not be read, or opening was refused), there is nowhere to go.
// Opening may wait on a "Save changes?" answer, so going to the match is the
// open's continuation, and the match is copied for it.
static void openSearchMatch(const FolderSearchMatch& m, void* userp) {
    App* app = static_cast<App*>(userp);
    const std::string path = m.path;
    const int line = m.line;
    const std::size_t column = m.byteColumn, length = m.byteLength;
    openFileThen(app, path, [app, path, line, column, length] {
        if (app->editor->currentPath() == path)
            app->editor->revealLine(line, column, length);
        refreshHints(app);   // a Markdown preview may have switched to source
    });
    gtk_window_present(GTK_WINDOW(app->window));
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

// Ctrl+Shift+F: search the folder selected in the tree, or else the open
// folder, which is what the Mac's Shift+Cmd+F does.
static void act_find_in_folder(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (!app->search) {
        app->search = new SearchPanel(GTK_WINDOW(app->window));
        app->search->setOpenCallback(openSearchMatch, app);
    }
    app->search->setRoot(app->rootDir);   // resets the scope after Open Folder
    const std::string dir = app->tree->selectedDir();
    if (!dir.empty()) app->search->setScope(dir);
    app->search->show();
}

// ---------------------------------------------------------------- find impl

// Find the next match at or after `from`, wrapping to the top of the buffer.
static bool findFrom(App* app, const GtkTextIter& from) {
    const char* q = gtk_editable_get_text(GTK_EDITABLE(app->searchEntry));
    if (!q || !*q) return false;

    GtkTextBuffer* buf = app->editor->buffer();
    GtkTextIter mstart, mend;
    gboolean found = gtk_text_iter_forward_search(
        &from, q, GTK_TEXT_SEARCH_CASE_INSENSITIVE, &mstart, &mend, nullptr);
    if (!found) {
        GtkTextIter top;
        gtk_text_buffer_get_start_iter(buf, &top);
        found = gtk_text_iter_forward_search(
            &top, q, GTK_TEXT_SEARCH_CASE_INSENSITIVE, &mstart, &mend, nullptr);
    }
    if (!found) return false;

    gtk_text_buffer_select_range(buf, &mstart, &mend);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(app->editor->textView()),
                                 &mstart, 0.1, FALSE, 0, 0);
    return true;
}

// The last match that ends before `before`, wrapping to the bottom.
static bool findBackFrom(App* app, const GtkTextIter& before) {
    const char* q = gtk_editable_get_text(GTK_EDITABLE(app->searchEntry));
    if (!q || !*q) return false;

    GtkTextBuffer* buf = app->editor->buffer();
    GtkTextIter mstart, mend;
    gboolean found = gtk_text_iter_backward_search(
        &before, q, GTK_TEXT_SEARCH_CASE_INSENSITIVE, &mstart, &mend, nullptr);
    if (!found) {
        GtkTextIter bottom;
        gtk_text_buffer_get_end_iter(buf, &bottom);
        found = gtk_text_iter_backward_search(
            &bottom, q, GTK_TEXT_SEARCH_CASE_INSENSITIVE, &mstart, &mend, nullptr);
    }
    if (!found) return false;

    gtk_text_buffer_select_range(buf, &mstart, &mend);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(app->editor->textView()),
                                 &mstart, 0.1, FALSE, 0, 0);
    return true;
}

// Typing in the find bar re-searches from the start of the current selection,
// so the match under the cursor keeps growing with the query instead of the
// search jumping ahead on every keystroke.
static void onSearchChanged(GtkSearchEntry*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    GtkTextBuffer* buf = app->editor->buffer();
    GtkTextIter start;
    gtk_text_buffer_get_iter_at_mark(buf, &start,
                                     gtk_text_buffer_get_selection_bound(buf));
    GtkTextIter insert;
    gtk_text_buffer_get_iter_at_mark(buf, &insert,
                                     gtk_text_buffer_get_insert(buf));
    if (gtk_text_iter_compare(&insert, &start) < 0) start = insert;
    findFrom(app, start);
}

// One step through the matches. Forward starts one character past the start
// of the selection, otherwise forward_search finds the same match again and
// Enter does nothing; backward ends before the selection's start.
static bool findStep(App* app, bool forward) {
    const char* q = gtk_editable_get_text(GTK_EDITABLE(app->searchEntry));
    if (!q || !*q) {
        // Nothing to look for yet: open the bar to type it in.
        gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(app->searchBar), TRUE);
        gtk_widget_grab_focus(app->searchEntry);
        return false;
    }
    GtkTextBuffer* buf = app->editor->buffer();
    GtkTextIter selStart, selEnd;
    const bool sel = gtk_text_buffer_get_selection_bounds(buf, &selStart, &selEnd);
    if (!sel)
        gtk_text_buffer_get_iter_at_mark(buf, &selStart, gtk_text_buffer_get_insert(buf));
    bool found;
    if (forward) {
        if (sel) gtk_text_iter_forward_char(&selStart);
        found = findFrom(app, selStart);
    } else {
        found = findBackFrom(app, selStart);
    }
    if (!found) gtk_widget_error_bell(app->editor->textView());
    return found;
}

// Enter and Ctrl+G in the find bar: the next match. Shift+Enter and
// Ctrl+Shift+G: the previous one.
static void onSearchNext(GtkSearchEntry*, gpointer userp) {
    findStep(static_cast<App*>(userp), true);
}

static void onSearchPrevious(GtkSearchEntry*, gpointer userp) {
    findStep(static_cast<App*>(userp), false);
}

// ---------------------------------------------------------- shortcut hints

// The macOS build floats a shortcut list over the top-right of the window
// (EditorController.mm -buildHintsPanelInContainer) and advertises it from the
// status bar, because nothing else in the UI tells you the bindings exist. This
// is that panel, with the Linux accelerators and Ctrl+Shift+H instead of ⇧⌘H —
// Ctrl+H is taken here, and rightly so, since it is the usual Linux binding for
// showing hidden files.
static std::string hintsText(App* app) {
    std::string s;
    s += "Keyboard Shortcuts\n";
    s += "────────────────────────────────────────\n";
    s += "Ctrl O         Open folder\n";
    s += "Ctrl S         Save\n";
    s += "Ctrl F         Find in file\n";
    s += "Ctrl G         Find next (Shift: previous)\n";
    s += "Ctrl Shift F   Find in folder\n";
    s += "Ctrl /         Toggle comment\n";
    s += "Ctrl ,         Settings\n";
    s += "Ctrl 0         Focus the file tree\n";
    s += "Ctrl 1         Focus the editor\n";
    s += "Ctrl Tab       Previous file\n";

    s += "\nWindows\n";
    s += "────────────────────────────────────────\n";
    s += "Ctrl N         New window\n";
    s += "Ctrl W         Close window\n";
    s += "Ctrl Q         Quit\n";

    s += "\nFiles\n";
    s += "────────────────────────────────────────\n";
    s += "Ctrl Alt N     New file\n";
    s += "Ctrl Shift N   New folder\n";
    s += "F2             Rename (in the tree)\n";
    s += "Delete         Move to Trash (in the tree)\n";
    s += std::string("Ctrl H         Hidden files (") +
         (g.showHidden ? "shown" : "hidden") + ")\n";

    s += "\nLanguage servers\n";
    s += "────────────────────────────────────────\n";
    s += "Ctrl Space     Complete\n";
    s += "F12            Go to definition (or Ctrl click)\n";
    s += "Ctrl I         Hover info\n";

    s += "\nPanes\n";
    s += "────────────────────────────────────────\n";
    s += "Ctrl B         Sidebar\n";
    s += std::string("Ctrl Shift E   Editor      (") +
         (editorCollapsed(app) ? "collapsed" : "shown") + ")\n";
#ifdef MINICODE_ENABLE_TERMINAL
    s += std::string("Ctrl Shift T   Terminal    (") +
         (app->termPanel && gtk_widget_get_visible(app->termPanel) ? "open" : "hidden") +
         ")\n";
    s += "Ctrl `         Terminal, too\n";
#endif
#ifdef MINICODE_ENABLE_BROWSER
    s += std::string("Ctrl Shift B   Browser     (") +
         (app->browserRevealer &&
          gtk_revealer_get_reveal_child(GTK_REVEALER(app->browserRevealer))
              ? "open" : "hidden") + ")\n";
#endif
    if (app->editor->isMarkdown()) {
        s += std::string("Ctrl Shift P   Markdown    (") +
             (app->editor->inPreview() ? "rendered" : "source") + ")\n";
    }
    if (app->editor->isLatex()) {
        s += std::string("Ctrl Shift P   LaTeX       (") +
             (app->editor->inPreview() ? "preview" : "source") + ")\n";
        s += "Ctrl Shift S   Export PDF\n";
    }
    if (app->editor->showingPdf()) {
        s += "Ctrl + / -     Zoom the PDF (or Ctrl wheel)\n";
        s += "Ctrl Alt 0     Fit width\n";
    }

#ifdef MINICODE_ENABLE_TERMINAL
    s += "\nIn the terminal\n";
    s += "────────────────────────────────────────\n";
    s += "Plain Ctrl keys go to the shell (Ctrl C,\n";
    s += "Ctrl W, Ctrl H...). These still work:\n";
    s += "Ctrl Shift C   Copy\n";
    s += "Ctrl Shift V   Paste\n";
    s += "Ctrl click     Open a file:line or URL\n";
    s += "Ctrl Shift …   Every Ctrl Shift shortcut\n";
    s += "Ctrl ` 0 1     Panes: terminal, tree, editor\n";
    s += "Ctrl Tab       Previous file\n";
    s += "Ctrl Alt N     New file\n";
#endif

    s += "\nCtrl Shift H   Hide these hints";
    return s;
}

static GtkWidget* buildHintsPanel(App* app) {
    app->hintsLabel = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->hintsLabel), 0.0);

    app->hintsPanel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(app->hintsPanel, "minicode-hints");
    gtk_box_append(GTK_BOX(app->hintsPanel), app->hintsLabel);

    // Float it in the top-right corner, the same corner macOS puts it in, and
    // keep it from stretching to fill the overlay.
    gtk_widget_set_halign(app->hintsPanel, GTK_ALIGN_END);
    gtk_widget_set_valign(app->hintsPanel, GTK_ALIGN_START);
    gtk_widget_set_margin_top(app->hintsPanel, 18);
    gtk_widget_set_margin_end(app->hintsPanel, 18);
    // The panel is a reference card, not a control: clicks belong to whatever is
    // underneath it.
    gtk_widget_set_can_target(app->hintsPanel, FALSE);
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
    if (showing) gtk_label_set_text(GTK_LABEL(app->hintsLabel), hintsText(app).c_str());
    gtk_widget_set_visible(app->hintsPanel, showing);
    gtk_label_set_text(GTK_LABEL(app->hintsHint),
                       showing ? kHintsHintHide : kHintsHintShow);
}

// ---------------------------------------------------------------- settings

// One window's share of the settings: the editor's tags, the language
// servers, the terminal's colors, and the status-bar note about bad lines.
static void applyWindowSettings(App* app) {
    const Settings& st = g.settings->settings();
    app->editor->applySettings(st);
    if (app->lsp) app->lsp->applySettings(st);   // restarts servers if lsp.* changed
#ifdef MINICODE_ENABLE_TERMINAL
    if (app->terminal) app->terminal->applySettings(st);
#endif
    const auto& errors = g.settings->errors();
    std::string note;
    if (!errors.empty()) {
        note = "Settings " + errors.front();
        if (errors.size() > 1)
            note += " (and " + std::to_string(errors.size() - 1) + " more)";
    }
    gtk_label_set_text(GTK_LABEL(app->settingsLabel), note.c_str());
    std::string tip;
    for (const auto& e : errors) tip += (tip.empty() ? "" : "\n") + e;
    gtk_widget_set_tooltip_text(app->settingsLabel,
        errors.empty() ? nullptr : (g.settings->path() + "\n\n" + tip).c_str());
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

// ------------------------------------------------------------ keys in the terminal
//
// Application accelerators are handled by the window in the capture phase,
// before the focused widget sees the key, so while the terminal had the
// keyboard Ctrl+B, Ctrl+F, Ctrl+H, Ctrl+W and the rest were taken from bash,
// readline, vim and emacs (Ctrl+H is backspace in many setups, Ctrl+W deletes
// a word). The Mac never has this problem: its shortcuts are on Command, and
// Control goes to the terminal.
//
// So while the active window's terminal has the focus, every accelerator that
// is a plain Ctrl key, or a bare function key, is taken off, and they come
// back when the focus leaves it. What stays: anything with Shift or Alt in it
// (the pane toggles, Find in Folder, New Folder and New File, Export PDF, the
// hints), and the pane keys Ctrl+`, Ctrl+0, Ctrl+1 and Ctrl+Tab, which a shell
// has no use for and which are how you get out of the terminal without the
// mouse. Accelerators belong to the application, not a window, but only the
// active window gets keys, so its focus decides.

struct Bind {
    const char* action;
    const char* accels[3];
};

static const Bind kBinds[] = {
    {"win.newwindow",      {"<Ctrl>n"}},
    {"win.close",          {"<Ctrl>w"}},
    {"app.quit",           {"<Ctrl>q"}},
    {"win.open",           {"<Ctrl>o"}},
    {"win.save",           {"<Ctrl>s"}},
    {"win.exportpdf",      {"<Ctrl><Shift>s"}},
    {"win.newfile",        {"<Ctrl><Alt>n"}},
    {"win.newfolder",      {"<Ctrl><Shift>n"}},
    {"win.find",           {"<Ctrl>f"}},
    {"win.findnext",       {"<Ctrl>g"}},
    {"win.findprevious",   {"<Ctrl><Shift>g"}},
    {"win.findinfolder",   {"<Ctrl><Shift>f"}},
    {"win.togglepreview",  {"<Ctrl><Shift>p"}},
    {"win.togglehints",    {"<Ctrl><Shift>h"}},
    {"win.togglesidebar",  {"<Ctrl>b"}},
    {"win.toggleeditor",   {"<Ctrl><Shift>e"}},
    {"win.toggleterminal", {"<Ctrl><Shift>t", "<Ctrl>grave"}},
    {"win.togglecomment",  {"<Ctrl>slash"}},
    {"win.settings",       {"<Ctrl>comma"}},
    {"win.togglebrowser",  {"<Ctrl><Shift>b"}},
    {"app.showhidden",     {"<Ctrl>h"}},
    {"win.focustree",      {"<Ctrl>0"}},
    {"win.focuseditor",    {"<Ctrl>1"}},
    {"win.previousfile",   {"<Ctrl>Tab"}},
    {"win.complete",       {"<Ctrl>space"}},
    {"win.definition",     {"F12"}},
    {"win.hoverinfo",      {"<Ctrl>i"}},
    {"win.zoomin",         {"<Ctrl>plus", "<Ctrl>equal", "<Ctrl>KP_Add"}},
    {"win.zoomout",        {"<Ctrl>minus", "<Ctrl>KP_Subtract"}},
    {"win.zoomfit",        {"<Ctrl><Alt>0"}},
};

// Is this accelerator the shell's while the terminal has the keyboard?
static bool shellOwns(const char* accel) {
    guint key = 0;
    GdkModifierType mods = (GdkModifierType)0;
    if (!gtk_accelerator_parse(accel, &key, &mods)) return false;
    if (mods & (GDK_SHIFT_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK)) return false;
    switch (key) {
        case GDK_KEY_grave: case GDK_KEY_0: case GDK_KEY_1: case GDK_KEY_Tab:
            return false;   // the pane keys
        default:
            return true;
    }
}

static void applyAccels(bool shell) {
    for (const Bind& b : kBinds) {
        const char* accels[4] = {nullptr, nullptr, nullptr, nullptr};
        int n = 0;
        for (const char* a : b.accels)
            if (a && !(shell && shellOwns(a))) accels[n++] = a;
        gtk_application_set_accels_for_action(g.gapp, b.action, accels);
    }
}

static App* appForWindow(GtkWindow* w) {
    for (App* a : g.windows)
        if (GTK_WINDOW(a->window) == w) return a;
    return nullptr;
}

static void updateShellKeys() {
    bool shell = false;
#ifdef MINICODE_ENABLE_TERMINAL
    if (App* a = appForWindow(gtk_application_get_active_window(g.gapp)))
        shell = a->terminal && a->terminal->owns(gtk_root_get_focus(GTK_ROOT(a->window)));
#endif
    if (shell == g.shellKeys) return;
    g.shellKeys = shell;
    applyAccels(shell);
}

static void onFocusWidget(GObject*, GParamSpec*, gpointer) { updateShellKeys(); }

// Coming back to a window is when the Mac looks for changes made elsewhere
// (windowDidBecomeKey); the file monitor catches most of them sooner.
static void onIsActive(GObject* w, GParamSpec*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    updateShellKeys();
    if (!app->dead && gtk_window_is_active(GTK_WINDOW(w))) app->editor->checkExternalChange();
}

// ---------------------------------------------------------------- menu

// New File, New Folder, Rename, Move to Trash, Reveal and Copy Path, in the
// Mac's order and grouping. Shared by the File menu and the tree's
// right-click menu.
static GMenuModel* treeActionsMenu() {
    GMenu* menu = g_menu_new();
    GMenu* create = g_menu_new();
    g_menu_append(create, "New File…", "win.newfile");
    g_menu_append(create, "New Folder…", "win.newfolder");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(create));
    g_object_unref(create);
    GMenu* change = g_menu_new();
    g_menu_append(change, "Rename…", "win.rename");
    g_menu_append(change, "Move to Trash", "win.trash");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(change));
    g_object_unref(change);
    GMenu* where = g_menu_new();
    g_menu_append(where, "Open Containing Folder", "win.reveal");
    g_menu_append(where, "Copy Path", "win.copypath");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(where));
    g_object_unref(where);
    return G_MENU_MODEL(menu);
}

// The menu bar is the application's, shown in every window.
static void buildMenu() {
    GMenu* menuBar = g_menu_new();

    GMenu* fileMenu = g_menu_new();
    GMenu* top = g_menu_new();
    g_menu_append(top, "New Window", "win.newwindow");
    g_menu_append(top, "Open Folder…", "win.open");
    g_menu_append(top, "Save", "win.save");
    g_menu_append(top, "Export PDF…", "win.exportpdf");
    g_menu_append_section(fileMenu, nullptr, G_MENU_MODEL(top));
    g_object_unref(top);
    g.treeMenu = treeActionsMenu();
    g_menu_append_section(fileMenu, nullptr, g.treeMenu);
    GMenu* bottom = g_menu_new();
    g_menu_append(bottom, "Close Window", "win.close");
    g_menu_append(bottom, "Quit", "app.quit");
    g_menu_append_section(fileMenu, nullptr, G_MENU_MODEL(bottom));
    g_object_unref(bottom);
    g_menu_append_submenu(menuBar, "File", G_MENU_MODEL(fileMenu));
    g_object_unref(fileMenu);   // menuBar holds it now

    GMenu* editMenu = g_menu_new();
    GMenu* find = g_menu_new();
    g_menu_append(find, "Find", "win.find");
    g_menu_append(find, "Find Next", "win.findnext");
    g_menu_append(find, "Find Previous", "win.findprevious");
    g_menu_append(find, "Find in Folder…", "win.findinfolder");
    g_menu_append_section(editMenu, nullptr, G_MENU_MODEL(find));
    g_object_unref(find);
    GMenu* code = g_menu_new();
    g_menu_append(code, "Toggle Comment", "win.togglecomment");
    g_menu_append(code, "Complete", "win.complete");
    g_menu_append(code, "Go to Definition", "win.definition");
    g_menu_append(code, "Show Hover Info", "win.hoverinfo");
    g_menu_append_section(editMenu, nullptr, G_MENU_MODEL(code));
    g_object_unref(code);
    g_menu_append(editMenu, "Settings…", "win.settings");
    g_menu_append_submenu(menuBar, "Edit", G_MENU_MODEL(editMenu));
    g_object_unref(editMenu);

    GMenu* viewMenu = g_menu_new();
    g_menu_append(viewMenu, "Toggle Markdown Preview", "win.togglepreview");
    g_menu_append(viewMenu, "Toggle Shortcut Hints", "win.togglehints");
    g_menu_append(viewMenu, "Toggle Sidebar", "win.togglesidebar");
    g_menu_append(viewMenu, "Toggle Editor", "win.toggleeditor");
    g_menu_append(viewMenu, "Toggle Terminal", "win.toggleterminal");
    g_menu_append(viewMenu, "Toggle Browser", "win.togglebrowser");
    g_menu_append(viewMenu, "Show Hidden Files", "app.showhidden");
    GMenu* zoom = g_menu_new();
    g_menu_append(zoom, "Zoom In", "win.zoomin");
    g_menu_append(zoom, "Zoom Out", "win.zoomout");
    g_menu_append(zoom, "Fit Width", "win.zoomfit");
    g_menu_append_section(viewMenu, nullptr, G_MENU_MODEL(zoom));
    g_object_unref(zoom);
    g_menu_append_submenu(menuBar, "View", G_MENU_MODEL(viewMenu));
    g_object_unref(viewMenu);

    // Keyboard-driven movement, the Mac's Navigate menu.
    GMenu* navMenu = g_menu_new();
    g_menu_append(navMenu, "Focus File Tree", "win.focustree");
    g_menu_append(navMenu, "Focus Editor", "win.focuseditor");
    g_menu_append(navMenu, "Previous File", "win.previousfile");
    g_menu_append(navMenu, "Go to Definition", "win.definition");
    g_menu_append_submenu(menuBar, "Navigate", G_MENU_MODEL(navMenu));
    g_object_unref(navMenu);

    gtk_application_set_menubar(g.gapp, G_MENU_MODEL(menuBar));
    g_object_unref(menuBar);
}

static void addAction(App* app, const char* name, GCallback cb) {
    GSimpleAction* a = g_simple_action_new(name, nullptr);
    g_signal_connect(a, "activate", cb, app);
    g_action_map_add_action(G_ACTION_MAP(app->window), G_ACTION(a));
    g_object_unref(a);   // the action map holds its own ref now
}

// What the application needs once, before its first window: the settings,
// the stylesheet, the menu bar, its own actions and the accelerators.
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

    buildMenu();
    applyAccels(false);
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

    std::vector<gpointer> owners = {app, app->editor, app->editor->media(), app->lsp,
                                    app->tree};
#ifdef MINICODE_ENABLE_PDF
    owners.push_back(app->editor->latex());
#endif
#ifdef MINICODE_ENABLE_TERMINAL
    owners.push_back(app->terminal);
#endif
#ifdef MINICODE_ENABLE_BROWSER
    owners.push_back(app->browser);
#endif
    disconnectOwners(app->window, owners);

    app->editor->setObserver(nullptr);
    app->editor->setTitleCallback(nullptr, nullptr);
    app->editor->setExternalChangeCallback(nullptr, nullptr);
    delete app->search;
    app->search = nullptr;
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
    delete app->tree;
    app->tree = nullptr;
    delete app->editor;
    app->editor = nullptr;
    // The App itself is kept (see App::dead).
    updateShellKeys();
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
    app->tree->setOpenCallback(openFileCb, app);
    app->editor = new Editor();
    app->editor->setTitleCallback(updateTitle, app);
    app->editor->setExternalChangeCallback(askExternalChange, app);
    app->editor->setSettingsPath(g.settings->path());

    // Right side: find bar + editor + collapsible panels.
    GtkWidget* rightBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    app->searchBar = gtk_search_bar_new();
    app->searchEntry = gtk_search_entry_new();
    gtk_search_bar_set_child(GTK_SEARCH_BAR(app->searchBar), app->searchEntry);
    gtk_search_bar_connect_entry(GTK_SEARCH_BAR(app->searchBar),
                                 GTK_EDITABLE(app->searchEntry));
    g_signal_connect(app->searchEntry, "search-changed",
                     G_CALLBACK(onSearchChanged), app);
    g_signal_connect(app->searchEntry, "activate",
                     G_CALLBACK(onSearchNext), app);
    g_signal_connect(app->searchEntry, "next-match",
                     G_CALLBACK(onSearchNext), app);
    g_signal_connect(app->searchEntry, "previous-match",
                     G_CALLBACK(onSearchPrevious), app);
    // Shift+Enter in the find bar goes back, as in most editors' find bars.
    GtkEventController* findKeys = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(findKeys),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Return, GDK_SHIFT_MASK),
                         gtk_named_action_new("win.findprevious")));
    gtk_widget_add_controller(app->searchEntry, findKeys);
    // Deliberately no gtk_search_bar_set_key_capture_widget: in an editor that
    // would auto-reveal the find bar on any keystroke and swallow typing.
    gtk_box_append(GTK_BOX(rightBox), app->searchBar);

    // Order matters: the browser sits with the editor in the upper area and the
    // terminal is docked below both, the same arrangement the macOS build lays
    // out by hand in -relayoutRightArea.
    GtkWidget* upperBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    app->upperBox = upperBox;
    gtk_box_append(GTK_BOX(upperBox), app->editor->widget());
#ifdef MINICODE_ENABLE_BROWSER
    app->browser = new Browser("https://duckduckgo.com");
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
    gtk_paned_set_start_child(GTK_PANED(app->hpaned), app->tree->widget());
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

    app->hintsHint = gtk_label_new(kHintsHintShow);
    gtk_label_set_xalign(GTK_LABEL(app->hintsHint), 1.0);

    GtkWidget* statusBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(statusBar, "minicode-status");
    gtk_box_append(GTK_BOX(statusBar), app->statusLabel);
    app->lspLabel = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(app->lspLabel), PANGO_ELLIPSIZE_START);
    gtk_box_append(GTK_BOX(statusBar), app->lspLabel);
    gtk_box_append(GTK_BOX(statusBar), app->settingsLabel);
    gtk_box_append(GTK_BOX(statusBar), app->hintsHint);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(vbox), app->hpaned);
    gtk_box_append(GTK_BOX(vbox), statusBar);

    // The hints panel floats over the whole window rather than displacing it,
    // so it needs a GtkOverlay between the window and the layout.
    GtkWidget* overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), vbox);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), buildHintsPanel(app));
    gtk_window_set_child(GTK_WINDOW(app->window), overlay);

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
        openSearchMatch(m, app);
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
    addAction(app, "findnext",       G_CALLBACK(act_find_next));
    addAction(app, "findprevious",   G_CALLBACK(act_find_previous));
    addAction(app, "findinfolder",   G_CALLBACK(act_find_in_folder));
    addAction(app, "togglepreview",  G_CALLBACK(act_toggle_preview));
    addAction(app, "togglehints",    G_CALLBACK(act_toggle_hints));
    addAction(app, "togglesidebar",  G_CALLBACK(act_toggle_sidebar));
    addAction(app, "toggleeditor",   G_CALLBACK(act_toggle_editor));
    addAction(app, "toggleterminal", G_CALLBACK(act_toggle_terminal));
    addAction(app, "togglebrowser",  G_CALLBACK(act_toggle_browser));
    addAction(app, "focustree",      G_CALLBACK(act_focus_tree));
    addAction(app, "focuseditor",    G_CALLBACK(act_focus_editor));
    addAction(app, "previousfile",   G_CALLBACK(act_previous_file));
    addAction(app, "togglecomment",  G_CALLBACK(act_toggle_comment));
    addAction(app, "settings",       G_CALLBACK(act_settings));
    addAction(app, "zoomin",         G_CALLBACK(act_zoom_in));
    addAction(app, "zoomout",        G_CALLBACK(act_zoom_out));
    addAction(app, "zoomfit",        G_CALLBACK(act_zoom_fit));
    for (const char* zoom : {"zoomin", "zoomout", "zoomfit"})   // until a PDF shows
        g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(
                                        G_ACTION_MAP(app->window), zoom)), FALSE);
    app->tree->setContextMenu(g.treeMenu);
    // F2 and Delete rename and trash only while the tree has the keyboard. As
    // window accelerators they would take Delete away from the editor.
    GtkEventController* treeKeys = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(treeKeys),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_F2, (GdkModifierType)0),
                         gtk_named_action_new("win.rename")));
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(treeKeys),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Delete, (GdkModifierType)0),
                         gtk_named_action_new("win.trash")));
    gtk_widget_add_controller(app->tree->widget(), treeKeys);
    g_signal_connect(app->window, "close-request", G_CALLBACK(onCloseRequest), app);
    g_signal_connect(app->window, "notify::focus-widget", G_CALLBACK(onFocusWidget), app);
    g_signal_connect(app->window, "notify::is-active", G_CALLBACK(onIsActive), app);
    applyWindowSettings(app);   // now that the editor, terminal and status bar exist

    // A file named on the command line opens before the window is shown, so it
    // is already rendered when the window appears rather than flashing the
    // welcome text first. Markdown arrives rendered, the same as a click in the
    // sidebar would give (Editor::openFile decides that).
    if (!file.empty()) openFileCb(file, app);

    gtk_window_present(GTK_WINDOW(app->window));
    return app;
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
    }), nullptr);
    int status = g_application_run(G_APPLICATION(gapp), argc, argv);
    g_object_unref(gapp);
    return status;
}
