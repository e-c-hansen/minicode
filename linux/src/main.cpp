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
#include <gtk/gtk.h>

#include "AppSettings.h"
#include "Editor.h"
#include "FileTree.h"
#include "ThemeCss.h"
#include "Palette.h"
#include "Terminal.h"
#include "Browser.h"
#include "Search.h"

#include <functional>
#include <string>
#include <unistd.h>
#include <limits.h>

struct App {
    GtkApplication* gapp = nullptr;
    GtkWidget*      window = nullptr;
    GtkWidget*      hpaned = nullptr;
    GtkWidget*      statusLabel = nullptr;
    GtkWidget*      searchBar = nullptr;
    GtkWidget*      searchEntry = nullptr;
    GtkWidget*      settingsLabel = nullptr;   // first problem in the settings file

    AppSettings*    settings = nullptr;
    GtkCssProvider* css = nullptr;
    int  sidebarWidth = 240;      // where the sidebar divider was before collapsing
    bool adjustingPaned = false;  // a snap is moving a divider; ignore the notify

    Editor*   editor = nullptr;
    FileTree* tree = nullptr;
    SearchPanel* search = nullptr;   // Find in Folder, created on first use

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
    // Set when the command line named a file rather than a directory: the file
    // to show once the window exists. See resolveStartupPath.
    std::string startupFile;
    bool sidebarVisible = true;

    bool prompting = false;   // a "Save changes?" alert is up
    bool closing = false;     // the user answered it for a window close
};

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
            bool go = choice == 1;                    // Don't Save
            if (choice == 0) go = saveCurrent(a);     // Save, unless it failed
            if (go) pd->proceed();
            else if (pd->cancelled) pd->cancelled();
            delete pd;
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

static void openFileNow(App* app, const std::string& path) {
    showEditorArea(app);   // opening a file must not disappear into a hidden pane
    app->editor->openFile(path);
    updateTitle(app);
    refreshHints(app);   // the Markdown line depends on the open file
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
    updateTitle(app);
    refreshHints(app);
}

// Closing the window with unsaved edits asks first. The answer arrives later,
// so the close is refused now and repeated once the edits are dealt with.
static gboolean onCloseRequest(GtkWindow*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (app->closing || !app->editor->dirty()) return FALSE;
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

static void act_toggle_hidden(GSimpleAction*, GVariant*, gpointer userp) {
    static_cast<App*>(userp)->tree->toggleHidden();
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
    if (!app->settings->ensureFileExists()) {
        gtk_label_set_text(GTK_LABEL(app->settingsLabel),
                           ("Could not create " + app->settings->path()).c_str());
        return;
    }
    openFileThen(app, app->settings->path(),
                 [app] { gtk_widget_grab_focus(app->editor->textView()); });
}

static void act_find(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    gboolean on = gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(app->searchBar));
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(app->searchBar), !on);
    if (!on) gtk_widget_grab_focus(app->searchEntry);
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
            if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr) == 0)
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
                !g_error_matches(err, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
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
static void findFrom(App* app, const GtkTextIter& from) {
    const char* q = gtk_editable_get_text(GTK_EDITABLE(app->searchEntry));
    if (!q || !*q) return;

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
    if (!found) return;

    gtk_text_buffer_select_range(buf, &mstart, &mend);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(app->editor->textView()),
                                 &mstart, 0.1, FALSE, 0, 0);
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

// Enter means "next match": start one character past the current selection,
// otherwise forward_search finds the same match again and Enter does nothing.
static void onSearchNext(GtkSearchEntry*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    GtkTextBuffer* buf = app->editor->buffer();
    GtkTextIter selStart, selEnd;
    if (gtk_text_buffer_get_selection_bounds(buf, &selStart, &selEnd)) {
        gtk_text_iter_forward_char(&selStart);
        findFrom(app, selStart);
    } else {
        GtkTextIter insert;
        gtk_text_buffer_get_iter_at_mark(buf, &insert,
                                         gtk_text_buffer_get_insert(buf));
        findFrom(app, insert);
    }
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
    s += "Ctrl Shift F   Find in folder\n";
    s += "Ctrl /         Toggle comment\n";
    s += "Ctrl ,         Settings\n";
    s += "Ctrl 0         Focus the file tree\n";

    s += "\nFiles\n";
    s += "────────────────────────────────────────\n";
    s += "Ctrl Alt N     New file\n";
    s += "Ctrl Shift N   New folder\n";
    s += "F2             Rename (in the tree)\n";
    s += "Delete         Move to Trash (in the tree)\n";
    s += "Ctrl H         Show or hide dotfiles\n";

    s += "\nPanes\n";
    s += "────────────────────────────────────────\n";
    s += "Ctrl B         Sidebar\n";
    s += std::string("Ctrl Shift E   Editor      (") +
         (editorCollapsed(app) ? "collapsed" : "shown") + ")\n";
#ifdef MINICODE_ENABLE_TERMINAL
    s += std::string("Ctrl Shift T   Terminal    (") +
         (app->termPanel && gtk_widget_get_visible(app->termPanel) ? "open" : "hidden") +
         ")\n";
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

// Apply the settings file everywhere: the stylesheet, the editor's tags, the
// terminal's colors, and the status-bar note about any bad lines.
static void applySettings(App* app) {
    const Settings& st = app->settings->settings();
    if (!app->css) {
        app->css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(app->css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_string(app->css, theme::stylesheet(st).c_str());
    if (app->editor) app->editor->applySettings(st);
#ifdef MINICODE_ENABLE_TERMINAL
    if (app->terminal) app->terminal->applySettings(st);
#endif
    if (app->settingsLabel) {
        const auto& errors = app->settings->errors();
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
            errors.empty() ? nullptr : (app->settings->path() + "\n\n" + tip).c_str());
    }
}

static void onSettingsChanged(void* userp) { applySettings(static_cast<App*>(userp)); }

// ---------------------------------------------------------------- menu / accels

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

static void buildMenu(App* app) {
    GMenu* menuBar = g_menu_new();

    GMenu* fileMenu = g_menu_new();
    GMenu* top = g_menu_new();
    g_menu_append(top, "Open Folder…", "win.open");
    g_menu_append(top, "Save", "win.save");
    g_menu_append(top, "Export PDF…", "win.exportpdf");
    g_menu_append_section(fileMenu, nullptr, G_MENU_MODEL(top));
    g_object_unref(top);
    GMenuModel* treeItems = treeActionsMenu();
    g_menu_append_section(fileMenu, nullptr, treeItems);
    g_menu_append_submenu(menuBar, "File", G_MENU_MODEL(fileMenu));
    g_object_unref(fileMenu);   // menuBar holds it now

    // The same items on a right-click in the tree.
    app->tree->setContextMenu(treeItems);
    g_object_unref(treeItems);

    GMenu* editMenu = g_menu_new();
    g_menu_append(editMenu, "Find", "win.find");
    g_menu_append(editMenu, "Find in Folder…", "win.findinfolder");
    g_menu_append(editMenu, "Toggle Comment", "win.togglecomment");
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
    g_menu_append(viewMenu, "Show/Hide Dotfiles", "win.togglehidden");
    g_menu_append(viewMenu, "Focus File Tree", "win.focustree");
    g_menu_append_submenu(menuBar, "View", G_MENU_MODEL(viewMenu));
    g_object_unref(viewMenu);

    gtk_application_set_menubar(app->gapp, G_MENU_MODEL(menuBar));
    g_object_unref(menuBar);
}

static void addAction(App* app, const char* name, GCallback cb) {
    GSimpleAction* a = g_simple_action_new(name, nullptr);
    g_signal_connect(a, "activate", cb, app);
    g_action_map_add_action(G_ACTION_MAP(app->window), G_ACTION(a));
    g_object_unref(a);   // the action map holds its own ref now
}

static void setAccels(App* app) {
    struct { const char* action; const char* accel; } binds[] = {
        {"win.open",            "<Ctrl>o"},
        {"win.save",            "<Ctrl>s"},
        {"win.exportpdf",       "<Ctrl><Shift>s"},
        {"win.newfile",         "<Ctrl><Alt>n"},
        {"win.newfolder",       "<Ctrl><Shift>n"},
        {"win.find",            "<Ctrl>f"},
        {"win.findinfolder",    "<Ctrl><Shift>f"},
        {"win.togglepreview",   "<Ctrl><Shift>p"},
        {"win.togglehints",     "<Ctrl><Shift>h"},
        {"win.togglesidebar",   "<Ctrl>b"},
        {"win.toggleeditor",    "<Ctrl><Shift>e"},
        {"win.toggleterminal",  "<Ctrl><Shift>t"},
        {"win.togglecomment",   "<Ctrl>slash"},
        {"win.settings",        "<Ctrl>comma"},
        {"win.togglebrowser",   "<Ctrl><Shift>b"},
        {"win.togglehidden",    "<Ctrl>h"},
        {"win.focustree",       "<Ctrl>0"},
    };
    for (auto& b : binds) {
        const char* accels[] = { b.accel, nullptr };
        gtk_application_set_accels_for_action(app->gapp, b.action, accels);
    }
}

// ---------------------------------------------------------------- activate

static void onActivate(GtkApplication* gapp, gpointer userp) {
    App* app = static_cast<App*>(userp);
    app->gapp = gapp;

    app->settings = new AppSettings();
    app->settings->setChangeCallback(onSettingsChanged, app);
    applySettings(app);   // the stylesheet, before any widget is drawn

    app->window = gtk_application_window_new(gapp);
    gtk_widget_add_css_class(app->window, "minicode-window");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1100, 720);
    gtk_window_set_title(GTK_WINDOW(app->window), "MiniCode");
    gtk_application_window_set_show_menubar(
        GTK_APPLICATION_WINDOW(app->window), TRUE);

    // Core widgets.
    app->tree = new FileTree(app->rootDir);
    app->tree->setOpenCallback(openFileCb, app);
    app->editor = new Editor();
    app->editor->setTitleCallback(updateTitle, app);
    app->editor->setSettingsPath(app->settings->path());

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

    // Actions, menu, accelerators.
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
    addAction(app, "findinfolder",   G_CALLBACK(act_find_in_folder));
    addAction(app, "togglepreview",  G_CALLBACK(act_toggle_preview));
    addAction(app, "togglehints",    G_CALLBACK(act_toggle_hints));
    addAction(app, "togglesidebar",  G_CALLBACK(act_toggle_sidebar));
    addAction(app, "toggleeditor",   G_CALLBACK(act_toggle_editor));
    addAction(app, "toggleterminal", G_CALLBACK(act_toggle_terminal));
    addAction(app, "togglebrowser",  G_CALLBACK(act_toggle_browser));
    addAction(app, "togglehidden",   G_CALLBACK(act_toggle_hidden));
    addAction(app, "focustree",      G_CALLBACK(act_focus_tree));
    addAction(app, "togglecomment",  G_CALLBACK(act_toggle_comment));
    addAction(app, "settings",       G_CALLBACK(act_settings));
    buildMenu(app);
    setAccels(app);
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
    applySettings(app);   // now that the editor, terminal and status bar exist

    // A file named on the command line opens before the window is shown, so it
    // is already rendered when the window appears rather than flashing the
    // welcome text first. Markdown arrives rendered, the same as a click in the
    // sidebar would give (Editor::openFile decides that).
    if (!app->startupFile.empty()) openFileCb(app->startupFile, app);

    gtk_window_present(GTK_WINDOW(app->window));
}

// ---------------------------------------------------------------- main

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
// Returns a message to show the user if the path did not exist, else empty. It
// is returned rather than printed because the caller may be a remote invocation,
// whose output has to be sent back over the command line object to reach the
// terminal the user actually typed in.
static std::string resolveStartupPath(App& app, const char* arg,
                                      const char* cwdIn) {
    char cwdBuf[PATH_MAX];
    const std::string cwd = (cwdIn && *cwdIn) ? cwdIn
        : (getcwd(cwdBuf, sizeof(cwdBuf)) ? cwdBuf : ".");

    app.startupFile.clear();
    if (!arg || !*arg) { app.rootDir = cwd; return ""; }

    char* canon = g_canonicalize_filename(arg, cwd.c_str());
    const std::string path = canon ? canon : arg;
    g_free(canon);

    if (g_file_test(path.c_str(), G_FILE_TEST_IS_DIR)) {
        app.rootDir = path;
        return "";
    }

    if (g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) {
        char* parent = g_path_get_dirname(path.c_str());
        app.rootDir = parent ? parent : cwd;
        g_free(parent);
        app.startupFile = path;
        return "";
    }

    // Neither a directory nor an existing file. Root the tree at the parent if
    // that at least exists, so a typo in the filename still lands in the right
    // folder instead of dumping the user somewhere unrelated.
    char* parent = g_path_get_dirname(path.c_str());
    const bool parentOk = parent && g_file_test(parent, G_FILE_TEST_IS_DIR);
    app.rootDir = parentOk ? parent : cwd;
    g_free(parent);
    return std::string("minicode: ") + arg + ": no such file or directory";
}

// Handles both the first launch and every later `minicode <path>` typed while a
// window is already open. GApplication is single-instance, so without this the
// second invocation just raised the existing window and threw the argument away
// — the file never appeared, which is no use from a shell prompt.
//
// HANDLES_COMMAND_LINE (rather than HANDLES_OPEN) is what lets us keep parsing
// argv[1] ourselves: it can be a directory or a file, and GApplication must not
// guess which.
static int onCommandLine(GApplication* gapp, GApplicationCommandLine* cl,
                         gpointer userp) {
    App* app = static_cast<App*>(userp);

    int n = 0;
    char** args = g_application_command_line_get_arguments(cl, &n);
    const std::string err = resolveStartupPath(
        *app, n > 1 ? args[1] : nullptr,
        g_application_command_line_get_cwd(cl));
    g_strfreev(args);

    // printerr on the command line object, not g_printerr: for a second
    // `minicode <path>` this routes the message back to the shell that ran it
    // instead of the stderr of the process that happens to own the window.
    if (!err.empty()) g_application_command_line_printerr(cl, "%s\n", err.c_str());

    if (!app->window) {
        onActivate(GTK_APPLICATION(gapp), app);   // first run: build the window
        return 0;
    }

    // Already running: re-root the sidebar on what was asked for, show the file
    // if one was named, and raise the window so the command visibly did something.
    app->tree->setRoot(app->rootDir);
    if (!app->startupFile.empty()) openFileCb(app->startupFile, app);
    gtk_window_present(GTK_WINDOW(app->window));
    return 0;
}

int main(int argc, char** argv) {
    App app;

    GtkApplication* gapp = gtk_application_new("org.minicode.Editor",
                                              G_APPLICATION_HANDLES_COMMAND_LINE);
    // argv[1] = a directory to open, or a file to open; default = cwd. The real
    // argc/argv go to g_application_run so that a remote invocation forwards
    // them to the running instance; onCommandLine is where they are read.
    g_signal_connect(gapp, "command-line", G_CALLBACK(onCommandLine), &app);
    int status = g_application_run(G_APPLICATION(gapp), argc, argv);
    g_object_unref(gapp);
    return status;
}
