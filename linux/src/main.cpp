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
//                   Editor (GtkTextView, vexpand)
//                   browser  revealer   (#ifdef MINICODE_ENABLE_BROWSER)
//                   terminal revealer   (#ifdef MINICODE_ENABLE_TERMINAL)
//       status bar (GtkLabel, VS Code blue)
//
// The editor and the browser share the upper area — showing the browser hides
// the editor — and the terminal is docked below both. That mirrors the macOS
// -relayoutRightArea, which is why the browser is appended before the terminal.
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

    GtkWidget* termRevealer = nullptr;
    GtkWidget* browserRevealer = nullptr;
#ifdef MINICODE_ENABLE_TERMINAL
    Terminal* terminal = nullptr;
#endif
#ifdef MINICODE_ENABLE_BROWSER
    Browser* browser = nullptr;
#endif

    std::string rootDir;
    bool sidebarVisible = true;
};

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
    app->editor->openFile(path);
    updateTitle(app);
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
}

static void act_toggle_sidebar(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    app->sidebarVisible = !app->sidebarVisible;
    gtk_widget_set_visible(app->tree->widget(), app->sidebarVisible);
}

static void act_toggle_hidden(GSimpleAction*, GVariant*, gpointer userp) {
    static_cast<App*>(userp)->tree->toggleHidden();
}

static void act_toggle_terminal(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (!app->termRevealer) return;
    gboolean shown = gtk_revealer_get_reveal_child(GTK_REVEALER(app->termRevealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->termRevealer), !shown);
#ifdef MINICODE_ENABLE_TERMINAL
    if (!shown && app->terminal) app->terminal->focus();
#endif
}

static void act_toggle_browser(GSimpleAction*, GVariant*, gpointer userp) {
    App* app = static_cast<App*>(userp);
    if (!app->browserRevealer) return;
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
        "  color: #FFFFFF; padding: 2px 8px; font-size: 12px; }";
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
    g_menu_append(viewMenu, "Toggle Sidebar", "win.togglesidebar");
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
        {"win.togglesidebar",   "<Ctrl>b"},
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

    gtk_box_append(GTK_BOX(rightBox), app->editor->widget());

    // Order matters: the browser sits with the editor in the upper area and the
    // terminal is docked below both, the same arrangement the macOS build lays
    // out by hand in -relayoutRightArea.
#ifdef MINICODE_ENABLE_BROWSER
    app->browser = new Browser("https://duckduckgo.com");
    app->browserRevealer = gtk_revealer_new();
    gtk_revealer_set_child(GTK_REVEALER(app->browserRevealer),
                           app->browser->widget());
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->browserRevealer), FALSE);
    gtk_widget_set_vexpand(app->browserRevealer, FALSE);  // see act_toggle_browser
    gtk_box_append(GTK_BOX(rightBox), app->browserRevealer);
#endif
#ifdef MINICODE_ENABLE_TERMINAL
    app->terminal = new Terminal(app->rootDir);
    app->termRevealer = gtk_revealer_new();
    gtk_revealer_set_child(GTK_REVEALER(app->termRevealer),
                           app->terminal->widget());
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->termRevealer), FALSE);
    gtk_box_append(GTK_BOX(rightBox), app->termRevealer);
#endif

    // Sidebar | editor split.
    app->hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(app->hpaned), app->tree->widget());
    gtk_paned_set_end_child(GTK_PANED(app->hpaned), rightBox);
    gtk_paned_set_position(GTK_PANED(app->hpaned), 240);
    gtk_paned_set_resize_start_child(GTK_PANED(app->hpaned), FALSE);
    gtk_widget_set_vexpand(app->hpaned, TRUE);

    // Status bar.
    app->statusLabel = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(app->statusLabel), 0.0);
    gtk_widget_add_css_class(app->statusLabel, "minicode-status");

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(vbox), app->hpaned);
    gtk_box_append(GTK_BOX(vbox), app->statusLabel);
    gtk_window_set_child(GTK_WINDOW(app->window), vbox);

    // Actions, menu, accelerators.
    addAction(app, "open",           G_CALLBACK(act_open));
    addAction(app, "save",           G_CALLBACK(act_save));
    addAction(app, "newfile",        G_CALLBACK(act_new_file));
    addAction(app, "newfolder",      G_CALLBACK(act_new_folder));
    addAction(app, "find",           G_CALLBACK(act_find));
    addAction(app, "togglepreview",  G_CALLBACK(act_toggle_preview));
    addAction(app, "togglesidebar",  G_CALLBACK(act_toggle_sidebar));
    addAction(app, "toggleterminal", G_CALLBACK(act_toggle_terminal));
    addAction(app, "togglebrowser",  G_CALLBACK(act_toggle_browser));
    addAction(app, "togglehidden",   G_CALLBACK(act_toggle_hidden));
    addAction(app, "focustree",      G_CALLBACK(act_focus_tree));
    buildMenu(app);
    setAccels(app);

    gtk_window_present(GTK_WINDOW(app->window));
}

// ---------------------------------------------------------------- main

int main(int argc, char** argv) {
    App app;

    // argv[1] = directory to open; default = current working directory.
    if (argc > 1) {
        app.rootDir = argv[1];
    } else {
        char cwd[PATH_MAX];
        app.rootDir = getcwd(cwd, sizeof(cwd)) ? cwd : ".";
    }

    GtkApplication* gapp =
        gtk_application_new("org.minicode.Editor", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gapp, "activate", G_CALLBACK(onActivate), &app);
    // We consume argv[1] ourselves (the directory to open) and do NOT register
    // G_APPLICATION_HANDLES_OPEN, so hand GApplication only the program name to
    // avoid it treating the directory arg as a file to open.
    int status = g_application_run(G_APPLICATION(gapp), 1, argv);
    g_object_unref(gapp);
    return status;
}
