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
// The always-built target is the tree + editor + markdown viewer. Terminal and
// Browser only exist when their libraries were found at configure time.
#include <gtk/gtk.h>

#include "Editor.h"
#include "FileTree.h"
#include "Palette.h"
#include "Terminal.h"
#include "Browser.h"

#include <glib/gstdio.h>   // g_mkdir_with_parents

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

    Editor*   editor = nullptr;
    FileTree* tree = nullptr;

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
};

// Defined with the rest of the hints panel further down; the pane toggles call
// it so an open panel never reports stale state.
static void refreshHints(App* app);

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
    if (H > 0 && H - pos >= Terminal::kMinHeight) app->termSplit = H - pos;
}
#endif

// Bring the editor/browser half back after it was collapsed, restoring the
// terminal to the height it had. Anything that needs to show something up there
// calls this first, so the editor cannot stay hidden behind new content.
static void showEditorArea(App* app) {
    if (gtk_widget_get_visible(app->upperBox)) return;
    gtk_widget_set_visible(app->upperBox, TRUE);
#ifdef MINICODE_ENABLE_TERMINAL
    placeTerminalDivider(app);
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
    }
    gtk_window_set_title(GTK_WINDOW(app->window), title.c_str());
    gtk_label_set_text(GTK_LABEL(app->statusLabel), p.empty() ? "Ready" : p.c_str());
}

// Called by FileTree when a file is activated.
static void openFileCb(const std::string& path, void* userp) {
    App* app = static_cast<App*>(userp);
    showEditorArea(app);   // opening a file must not disappear into a hidden pane
    app->editor->openFile(path);
    updateTitle(app);
    refreshHints(app);   // the Markdown line depends on the open file
}

// ---------------------------------------------------------------- actions

static void act_open(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    GtkFileDialog* dlg = gtk_file_dialog_new();
    gtk_file_dialog_select_folder(dlg, GTK_WINDOW(app->window), nullptr,
        [](GObject* src, GAsyncResult* res, gpointer up) {
            App* a = static_cast<App*>(up);
            GFile* folder = gtk_file_dialog_select_folder_finish(
                GTK_FILE_DIALOG(src), res, nullptr);
            if (!folder) return;
            char* path = g_file_get_path(folder);
            if (path) { a->rootDir = path; a->tree->setRoot(path); g_free(path); }
            g_object_unref(folder);
        }, app);
    g_object_unref(dlg);
}

static void act_save(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    app->editor->save();
    updateTitle(app);
}

static void act_toggle_preview(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    app->editor->togglePreview();
    refreshHints(app);
}

static void act_toggle_sidebar(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
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
    if (!gtk_widget_get_visible(app->upperBox)) { showEditorArea(app); return; }

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

static void act_find(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    gboolean on = gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(app->searchBar));
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(app->searchBar), !on);
    if (!on) gtk_widget_grab_focus(app->searchEntry);
}

// Create a uniquely-named file/folder in the root; the file monitor refreshes
// the tree. A first draft: no inline rename UI yet.
static std::string uniqueChild(const std::string& root, const std::string& base) {
    for (int i = 0; ; ++i) {
        std::string name = i == 0 ? base : base + std::to_string(i);
        std::string full = root + "/" + name;
        if (access(full.c_str(), F_OK) != 0) return full;
    }
}

static void act_new_file(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    std::string p = uniqueChild(app->rootDir, "untitled.txt");
    g_file_set_contents(p.c_str(), "", 0, nullptr);
    app->editor->openFile(p);
    updateTitle(app);
}

static void act_new_folder(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    std::string p = uniqueChild(app->rootDir, "untitled-folder");
    g_mkdir_with_parents(p.c_str(), 0755);
}

static void act_focus_tree(GSimpleAction*, GVariant*, gpointer userp) {
    static_cast<App*>(userp)->tree->focus();
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
    s += "Ctrl 0         Focus the file tree\n";

    s += "\nFiles\n";
    s += "────────────────────────────────────────\n";
    s += "Ctrl Alt N     New file\n";
    s += "Ctrl Shift N   New folder\n";
    s += "Ctrl H         Show or hide dotfiles\n";

    s += "\nPanes\n";
    s += "────────────────────────────────────────\n";
    s += "Ctrl B         Sidebar\n";
    s += std::string("Ctrl Shift E   Editor      (") +
         (gtk_widget_get_visible(app->upperBox) ? "shown" : "collapsed") + ")\n";
#ifdef MINICODE_ENABLE_TERMINAL
    s += std::string("Ctrl T         Terminal    (") +
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

// ---------------------------------------------------------------- css

static void loadCss() {
    GtkCssProvider* css = gtk_css_provider_new();
    const char* style =
        ".minicode-editor text { background-color: " "#1E1E1E" "; color: " "#D4D4D4" "; }"
        ".minicode-editor text selection { background-color: #264F78; }"
        // Markdown preview and plain messages: proportional body text at the
        // macOS body size (15pt). Code and table runs re-apply monospace via
        // their GtkTextTags.
        ".minicode-prose text { font-family: sans-serif; font-size: 15pt; }"
        ".minicode-sidebar { background-color: " "#252526" "; }"
        ".minicode-tree { background-color: " "#252526" "; }"
        ".minicode-tree-label { color: " "#CCCCCC" "; }"
        ".minicode-dir-icon  { color: " "#C09553" "; }"
        ".minicode-file-icon { color: " "#8A99A8" "; }"
        ".minicode-status {"
        "  background-color: " "#007ACC" ";"
        "  color: #FFFFFF; padding: 2px 8px; font-size: 12px; }"

        // Terminal panel. A shade darker than the editor so it reads as its own
        // surface, with the same 6px inset the macOS terminal uses
        // (Terminal.mm, textContainerInset). VTE takes its inner border from
        // the CSS padding.
        ".minicode-terminal { background-color: " "#181818" "; }"
        ".minicode-terminal vte-terminal { padding: 6px; }"

        // Pane dividers. GTK's default is a hairline in the desktop theme's
        // color: invisible against dark chrome, and a poor drag target. 4px in
        // the divider gray, lighting up blue under the pointer, says "drag me".
        ".minicode-split > separator {"
        "  min-width: 4px; min-height: 4px;"
        "  background-color: " "#333333" "; }"
        ".minicode-split > separator:hover {"
        "  background-color: " "#007ACC" "; }"

        // Scrollbars over the dark panels. The desktop theme's are tuned for a
        // light background and all but disappear on #1E1E1E.
        ".minicode-scroller scrollbar { background-color: transparent; border: none; }"
        ".minicode-scroller scrollbar slider {"
        "  background-color: rgba(255,255,255,0.22);"
        "  border: none; min-width: 8px; min-height: 8px; }"
        ".minicode-scroller scrollbar slider:hover {"
        "  background-color: rgba(255,255,255,0.38); }"

        // The floating shortcut list. Nearly opaque rather than fully so, to
        // read as an overlay on top of the editor rather than a pane of it.
        ".minicode-hints {"
        "  background-color: " "#252526" ";"
        "  border: 1px solid " "#333333" ";"
        "  border-radius: 8px;"
        "  padding: 14px 18px;"
        "  color: " "#D4D4D4" ";"
        "  font-family: monospace;"
        "  box-shadow: 0 6px 20px rgba(0,0,0,0.55); }";
    gtk_css_provider_load_from_string(css, style);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

// ---------------------------------------------------------------- menu / accels

static void buildMenu(App* app) {
    GMenu* menuBar = g_menu_new();

    GMenu* fileMenu = g_menu_new();
    g_menu_append(fileMenu, "Open Folder…", "win.open");
    g_menu_append(fileMenu, "New File", "win.newfile");
    g_menu_append(fileMenu, "New Folder", "win.newfolder");
    g_menu_append(fileMenu, "Save", "win.save");
    g_menu_append_submenu(menuBar, "File", G_MENU_MODEL(fileMenu));
    g_object_unref(fileMenu);   // menuBar holds it now

    GMenu* editMenu = g_menu_new();
    g_menu_append(editMenu, "Find", "win.find");
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
        {"win.newfile",         "<Ctrl><Alt>n"},
        {"win.newfolder",       "<Ctrl><Shift>n"},
        {"win.find",            "<Ctrl>f"},
        {"win.togglepreview",   "<Ctrl><Shift>p"},
        {"win.togglehints",     "<Ctrl><Shift>h"},
        {"win.togglesidebar",   "<Ctrl>b"},
        {"win.toggleeditor",    "<Ctrl><Shift>e"},
        {"win.toggleterminal",  "<Ctrl>t"},
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

    loadCss();

    app->window = gtk_application_window_new(gapp);
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1100, 720);
    gtk_window_set_title(GTK_WINDOW(app->window), "MiniCode");
    gtk_application_window_set_show_menubar(
        GTK_APPLICATION_WINDOW(app->window), TRUE);

    // Core widgets.
    app->tree = new FileTree(app->rootDir);
    app->tree->setOpenCallback(openFileCb, app);
    app->editor = new Editor();
    app->editor->setTitleCallback(updateTitle, app);

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
    // A GtkTextView will happily shrink to nothing, so without a floor here the
    // divider could be dragged to the top of the window and leave a 12px sliver
    // of editor. 120px is the floor the macOS drag handler keeps.
    gtk_widget_set_size_request(upperBox, -1, 120);
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
    gtk_paned_set_shrink_start_child(GTK_PANED(app->vpaned), FALSE);
    gtk_widget_set_vexpand(app->vpaned, TRUE);
#ifdef MINICODE_ENABLE_TERMINAL
    app->terminal = new Terminal(app->rootDir);
    app->termPanel = app->terminal->widget();
    gtk_paned_set_end_child(GTK_PANED(app->vpaned), app->termPanel);
    gtk_paned_set_resize_end_child(GTK_PANED(app->vpaned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(app->vpaned), FALSE);
    gtk_widget_set_visible(app->termPanel, FALSE);   // opens on Ctrl+T
#endif
    gtk_box_append(GTK_BOX(rightBox), app->vpaned);

    // Sidebar | editor split.
    app->hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(app->hpaned, "minicode-split");
    gtk_paned_set_start_child(GTK_PANED(app->hpaned), app->tree->widget());
    gtk_paned_set_end_child(GTK_PANED(app->hpaned), rightBox);
    gtk_paned_set_position(GTK_PANED(app->hpaned), 240);
    gtk_paned_set_resize_start_child(GTK_PANED(app->hpaned), FALSE);
    gtk_widget_set_vexpand(app->hpaned, TRUE);

    // Status bar: the current path on the left, and the hint that advertises the
    // shortcuts panel on the right. The macOS build carries the same pair, and
    // that right-hand label is the only thing that makes the panel discoverable.
    app->statusLabel = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(app->statusLabel), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(app->statusLabel), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(app->statusLabel, TRUE);

    app->hintsHint = gtk_label_new(kHintsHintShow);
    gtk_label_set_xalign(GTK_LABEL(app->hintsHint), 1.0);

    GtkWidget* statusBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(statusBar, "minicode-status");
    gtk_box_append(GTK_BOX(statusBar), app->statusLabel);
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
    addAction(app, "newfile",        G_CALLBACK(act_new_file));
    addAction(app, "newfolder",      G_CALLBACK(act_new_folder));
    addAction(app, "find",           G_CALLBACK(act_find));
    addAction(app, "togglepreview",  G_CALLBACK(act_toggle_preview));
    addAction(app, "togglehints",    G_CALLBACK(act_toggle_hints));
    addAction(app, "togglesidebar",  G_CALLBACK(act_toggle_sidebar));
    addAction(app, "toggleeditor",   G_CALLBACK(act_toggle_editor));
    addAction(app, "toggleterminal", G_CALLBACK(act_toggle_terminal));
    addAction(app, "togglebrowser",  G_CALLBACK(act_toggle_browser));
    addAction(app, "togglehidden",   G_CALLBACK(act_toggle_hidden));
    addAction(app, "focustree",      G_CALLBACK(act_focus_tree));
    buildMenu(app);
    setAccels(app);

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
