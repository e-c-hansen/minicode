// Search.cpp — see Search.h. The search itself is FolderSearch::search in the
// shared core, and Replace in Folder its planReplace, replaceInText and
// replaceMatchInText with TextFind's edits for tabs; this file is the
// window, the worker threads, the list, and the tabs.
#include "Search.h"

#include "DocumentView.h"
#include "EditSpans.h"
#include "Editor.h"
#include "Keys.h"
#include "ListKeyboard.h"
#include "MultiEdit.h"
#include "Question.h"
#include "TextFind.h"
#include "Utf8Offsets.h"

#include <algorithm>
#include <memory>
#include <set>
#include <utility>

// What one search carries into its worker thread and back. The GTask owns it
// (task data), so it is freed once the result has been handled or dropped.
struct SearchPanel::Job {
    unsigned gen = 0;
    std::string root;
    std::string query;
    FolderSearchOptions options;
    bool todos = false;   // findTodos, not a query
    std::shared_ptr<std::atomic<bool>> cancel;
    std::weak_ptr<int> alive;   // the panel's life_
    FolderSearchResult result;
    std::string note;           // what a replace did, before the counts
    bool noteError = false;
};

// Replace All's count, on a worker.
struct SearchPanel::PlanJob {
    SearchPanel* self = nullptr;
    std::weak_ptr<int> alive;
    std::string root, query, replacement;
    FolderSearchOptions options;
    FolderReplaceExclusions dismissed;   // the rows dismissed when it was asked
    FolderReplacePlan plan;
};

// The files Replace in Folder writes, on a worker; the tabs were done on
// the main thread before it started.
struct SearchPanel::WriteJob {
    SearchPanel* self = nullptr;
    std::weak_ptr<int> alive;
    std::vector<std::string> paths;
    std::string query, replacement;
    FolderSearchOptions options;
    bool one = false;            // only a row's match
    int line = 0;
    std::size_t byteColumn = 0;
    std::string expected;        // that row's text as searched
    FolderReplaceExclusions dismissed;   // Replace All: the rows it leaves alone
    // Done on the main thread already:
    std::size_t tabFiles = 0, tabMatches = 0;
    std::vector<std::string> skipped, problems;
    // The worker's:
    std::size_t files = 0, matches = 0;
    std::vector<std::string> failed;
};

// The location half of a row, the same blue as the Markdown link color.
static const char* kLocationColor = "#4EA1F7";

namespace {

std::string baseName(const std::string& path) {
    gchar* b = g_path_get_basename(path.c_str());
    std::string s = b ? b : path;
    g_free(b);
    return s;
}

std::string joined(const std::vector<std::string>& v, const char* between = ", ") {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : between) + x;
    return s;
}

std::string plural(std::size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// A count with thousands separators, as the Mac's MCGrouped.
std::string grouped(std::size_t n) {
    std::string d = std::to_string(n), out;
    for (std::size_t i = 0; i < d.size(); i++) {
        if (i && (d.size() - i) % 3 == 0) out += ',';
        out += d[i];
    }
    return out;
}

// Replace in Folder leaves a file over FolderSearch's whole-read size
// alone (the search still finds its matches): "1 MB".
std::string replaceSizeLimit() {
    return std::to_string(FolderSearchOptions().maxFileSize / (1024 * 1024)) + " MB";
}

// Text that is not UTF-8 and holds no NUL: what the search reads, and the
// editor opens, as Windows-1252 (FolderSearch::readsAsWindows1252), and
// Replace in Folder, which writes UTF-8 or UTF-16 only, leaves alone. The
// Mac's MCReadsAsWindows1252.
bool readsAsWindows1252(const std::string& path) {
    std::string bytes;
    FolderSearch::FileStamp st;
    if (FolderSearch::readFileStable(path, FolderSearchOptions().maxFileSize, &bytes, &st) !=
        FolderSearch::ReadStatus::Ok)
        return false;
    return FolderSearch::readsAsWindows1252(bytes);
}

GtkWidget* optionToggle(const char* label, const std::string& tip) {
    GtkWidget* b = gtk_toggle_button_new_with_label(label);
    gtk_widget_set_focus_on_click(b, FALSE);
    gtk_widget_set_tooltip_text(b, tip.c_str());
    gtk_widget_add_css_class(b, "flat");
    return b;
}

std::string keyTip(const char* what, const char* id) {
    const std::string k = Keys::label(id, "+");
    return k.empty() ? std::string(what) : std::string(what) + " (" + k + ")";
}

bool active(GtkWidget* toggle) {
    return toggle && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle));
}

std::string textOf(GtkWidget* editable) {
    return editable ? gtk_editable_get_text(GTK_EDITABLE(editable)) : "";
}

// The commands that show a pane of the editor window or put the keyboard in
// one, whose keys this window hands to it (the Mac's currentForPane:
// callers in main.mm, #210). This window is no application window, so
// without this they did nothing here, and what was typed next stayed in
// its fields.
const char* const kPaneCommands[] = {
    "workbench.action.terminal.toggleTerminal",
    "workbench.action.terminal.new",
    "workbench.action.terminal.split",
    "mutible.terminal.splitDown",
    "mutible.toggleBrowser",
    "mutible.browser.newTab",
    "mutible.toggleShortcutHints",
    "workbench.action.toggleSidebarVisibility",
    "workbench.view.scm",
    "mutible.toggleAgents",
    "mutible.toggleProcesses",
    "mutible.toggleEditor",
    "workbench.actions.view.problems",
    "workbench.action.showCommands",
    "workbench.action.quickOpen",
    "workbench.action.gotoLine",
    "workbench.action.gotoSymbol",
    "workbench.action.showAllSymbols",
    "workbench.action.focusSideBar",
    "workbench.action.focusFirstEditorGroup",
    "workbench.action.focusSecondEditorGroup",
};

// A row's ×, shown while the pointer is over the row or it is selected (the
// Mac's MCSearchRowView); hidden, it still takes a click where it sits.
void installRowCss() {
    static bool done = false;
    if (done) return;
    done = true;
    GtkCssProvider* p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        p, ".minicode-search listview > row .minicode-dismiss { opacity: 0;"
           " min-height: 0; min-width: 0; padding: 0 6px; }"
           ".minicode-search listview > row:hover .minicode-dismiss,"
           ".minicode-search listview > row:selected .minicode-dismiss { opacity: 1; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

}  // namespace

SearchPanel::SearchPanel(GtkWindow* parent, Mode mode) : parent_(parent), mode_(mode) {
    build();
}

SearchPanel::~SearchPanel() {
    life_.reset();
    if (cancel_) cancel_->store(true);
    if (rerunTimer_) g_source_remove(rerunTimer_);
    if (window_) {
        g_object_remove_weak_pointer(G_OBJECT(window_), reinterpret_cast<gpointer*>(&window_));
        gtk_window_destroy(GTK_WINDOW(window_));
    }
}

void SearchPanel::build() {
    window_ = gtk_window_new();
    const bool todos = mode_ == Mode::Todos;
    gtk_window_set_title(GTK_WINDOW(window_), todos ? "TODOs" : "Find in Folder");
    gtk_window_set_default_size(GTK_WINDOW(window_), 680, 460);
    gtk_window_set_transient_for(GTK_WINDOW(window_), parent_);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(window_), TRUE);
    // GTK may destroy it with its parent before this object goes.
    g_object_add_weak_pointer(G_OBJECT(window_), reinterpret_cast<gpointer*>(&window_));
    // Closing hides it, so the last query and results are still there when it
    // is opened again, as on the Mac.
    gtk_window_set_hide_on_close(GTK_WINDOW(window_), TRUE);
    gtk_widget_add_css_class(window_, "minicode-search");

    // --- scope row: "Folder:" [ path ] [ Choose… ] ---
    GtkWidget* scopeRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* label = gtk_label_new("Folder:");
    gtk_widget_add_css_class(label, "dim-label");
    scopeEntry_ = gtk_entry_new();
    gtk_widget_set_hexpand(scopeEntry_, TRUE);
    gtk_widget_set_tooltip_text(scopeEntry_,
        todos ? "The folder to list TODOs from. Type a path and press Enter; a "
                "relative path is taken from the open folder."
              : "The folder to search. Type a path and press Enter; a relative path is "
                "taken from the open folder.");
    g_signal_connect_swapped(scopeEntry_, "activate",
        G_CALLBACK(+[](SearchPanel* self) { self->scopeEntered(); }), this);
    GtkWidget* choose = gtk_button_new_with_label("Choose…");
    g_signal_connect_swapped(choose, "clicked",
        G_CALLBACK(+[](SearchPanel* self) { self->chooseScope(); }), this);
    gtk_box_append(GTK_BOX(scopeRow), label);
    gtk_box_append(GTK_BOX(scopeRow), scopeEntry_);
    gtk_box_append(GTK_BOX(scopeRow), choose);

    // --- the query, with the chevron and VS Code's three options ---
    GtkWidget* queryRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkSizeGroup* lead = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    if (!todos) {
        chevron_ = gtk_button_new_from_icon_name("pan-end-symbolic");
        gtk_widget_add_css_class(chevron_, "flat");
        gtk_widget_set_focus_on_click(chevron_, FALSE);
        gtk_widget_set_tooltip_text(
            chevron_, keyTip("Toggle Replace", "workbench.action.replaceInFiles").c_str());
        g_signal_connect_swapped(chevron_, "clicked", G_CALLBACK(+[](SearchPanel* self) {
            self->setReplaceShown(!self->replaceShown_);
            if (self->replaceShown_) gtk_widget_grab_focus(self->replaceEntry_);
        }), this);
        gtk_size_group_add_widget(lead, chevron_);
        gtk_box_append(GTK_BOX(queryRow), chevron_);
    }
    entry_ = gtk_search_entry_new();
    gtk_widget_set_hexpand(entry_, TRUE);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(entry_),
        todos ? "Filter the TODOs…" : "Search text in this folder…");
    // Search as the user types, after the same pause the Mac waits (0.35 s),
    // so a full folder scan does not start on every keystroke. Filtering the
    // TODOs found scans nothing, so it follows each key.
    gtk_search_entry_set_search_delay(GTK_SEARCH_ENTRY(entry_), todos ? 0 : 350);
    g_signal_connect(entry_, "search-changed", G_CALLBACK(onSearchChanged), this);
    g_signal_connect(entry_, "activate", G_CALLBACK(onQueryActivate), this);
    g_signal_connect(entry_, "stop-search", G_CALLBACK(onStopSearch), this);
    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(onQueryKey), this);
    gtk_widget_add_controller(entry_, keys);
    gtk_box_append(GTK_BOX(queryRow), entry_);
    if (!todos) {
        case_ = optionToggle("Aa", keyTip("Match Case", "toggleFindCaseSensitive"));
        word_ = optionToggle("ab", keyTip("Match Whole Word", "toggleFindWholeWord"));
        regex_ = optionToggle(".*", keyTip("Use Regular Expression", "toggleFindRegex"));
        for (GtkWidget* t : {case_, word_, regex_}) {
            g_signal_connect_swapped(t, "toggled", G_CALLBACK(+[](SearchPanel* self) {
                self->runSearch();
            }), this);
            gtk_box_append(GTK_BOX(queryRow), t);
        }
        details_ = optionToggle("…", "Files to include and exclude");
        g_signal_connect_swapped(details_, "toggled", G_CALLBACK(+[](SearchPanel* self) {
            gtk_widget_set_visible(self->detailsBox_, active(self->details_));
        }), this);
        gtk_box_append(GTK_BOX(queryRow), details_);
    }

    // --- the replace row: the replacement with Preserve Case, Replace All ---
    if (!todos) {
        replaceRow_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget* gap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_size_group_add_widget(lead, gap);
        gtk_box_append(GTK_BOX(replaceRow_), gap);
        replaceEntry_ = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(replaceEntry_), "Replace");
        gtk_widget_set_hexpand(replaceEntry_, TRUE);
        // Enter in the replacement never replaces: it does what Enter in
        // the query does, as VS Code's search view (the Mac since #210).
        // It was Replace All, and text typed here by mistake, then Enter,
        // rewrote files on the Mac.
        g_signal_connect_swapped(replaceEntry_, "activate", G_CALLBACK(+[](SearchPanel* self) {
            self->fieldReturn();
        }), this);
        preserve_ = optionToggle("AB", keyTip("Preserve Case", "togglePreserveCase"));
        replaceAllButton_ = gtk_button_new_with_label("Replace All");
        gtk_widget_set_focus_on_click(replaceAllButton_, FALSE);
        gtk_widget_set_tooltip_text(replaceAllButton_,
                                    keyTip("Replace All", "editor.action.replaceAll").c_str());
        g_signal_connect_swapped(replaceAllButton_, "clicked", G_CALLBACK(+[](SearchPanel* self) {
            self->replaceAll();
        }), this);
        gtk_box_append(GTK_BOX(replaceRow_), replaceEntry_);
        gtk_box_append(GTK_BOX(replaceRow_), preserve_);
        gtk_box_append(GTK_BOX(replaceRow_), replaceAllButton_);
        gtk_widget_set_visible(replaceRow_, FALSE);

        // --- files to include and exclude, behind "…" (VS Code's) ---
        detailsBox_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        includeEntry_ = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(includeEntry_),
                                       "files to include (e.g. *.ts, src/**/include)");
        excludeEntry_ = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(excludeEntry_),
                                       "files to exclude (e.g. *.ts, src/**/exclude)");
        for (GtkWidget* e : {includeEntry_, excludeEntry_}) {
            g_signal_connect_swapped(e, "activate", G_CALLBACK(+[](SearchPanel* self) {
                self->runSearch();
            }), this);
            g_signal_connect_swapped(e, "changed", G_CALLBACK(+[](SearchPanel* self) {
                if (self->rerunTimer_) g_source_remove(self->rerunTimer_);
                self->rerunTimer_ = g_timeout_add(350, [](gpointer p) -> gboolean {
                    SearchPanel* s = static_cast<SearchPanel*>(p);
                    s->rerunTimer_ = 0;
                    s->runSearch();
                    return G_SOURCE_REMOVE;
                }, self);
            }), this);
            gtk_box_append(GTK_BOX(detailsBox_), e);
        }
        gtk_widget_set_visible(detailsBox_, FALSE);
    }
    g_object_unref(lead);

    status_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(status_), 0.0);
    gtk_label_set_wrap(GTK_LABEL(status_), TRUE);
    gtk_widget_add_css_class(status_, "dim-label");

    // --- results ---
    rows_ = gtk_string_list_new(nullptr);
    // The selection model takes the list; keep a reference of our own, since
    // finish() splices into it directly.
    g_object_ref(rows_);
    GtkSingleSelection* sel = gtk_single_selection_new(G_LIST_MODEL(rows_));
    GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(onSetup), this);
    g_signal_connect(factory, "bind", G_CALLBACK(onBind), this);
    listView_ = gtk_list_view_new(GTK_SELECTION_MODEL(sel), factory);
    installRowCss();
    gtk_widget_set_tooltip_text(listView_,
                                todos ? "Delete dismisses the selected row"
                                      : "Ctrl+Shift+1 replaces the selected row's match alone; "
                                        "Delete dismisses it, so Replace All leaves it alone");
    // One click opens a match, as on the Mac, and so does Enter. The rows are
    // not activatable (onSetup), so a double-click cannot open it twice.
    // gtk_list_view_set_single_click_activate is not used because it also
    // selects every row the pointer passes over.
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(onRowPressed), this);
    g_signal_connect(click, "released", G_CALLBACK(onRowReleased), this);
    gtk_widget_add_controller(listView_, GTK_EVENT_CONTROLLER(click));
    // A right click: the row's menu (Dismiss, Dismiss File, and Replace This
    // Match in Find in Folder).
    GtkGesture* menuClick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(menuClick), GDK_BUTTON_SECONDARY);
    g_signal_connect(menuClick, "pressed", G_CALLBACK(onRowMenu), this);
    gtk_widget_add_controller(listView_, GTK_EVENT_CONTROLLER(menuClick));
    GtkEventController* listKeys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(listKeys, GTK_PHASE_CAPTURE);
    g_signal_connect(listKeys, "key-pressed", G_CALLBACK(onListKey), this);
    gtk_widget_add_controller(listView_, listKeys);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), listView_);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_widget_set_vexpand(scroller, TRUE);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_box_append(GTK_BOX(box), scopeRow);
    gtk_box_append(GTK_BOX(box), queryRow);
    if (replaceRow_) gtk_box_append(GTK_BOX(box), replaceRow_);
    if (detailsBox_) gtk_box_append(GTK_BOX(box), detailsBox_);
    gtk_box_append(GTK_BOX(box), status_);
    gtk_box_append(GTK_BOX(box), scroller);
    gtk_window_set_child(GTK_WINDOW(window_), box);

    // Escape anywhere in the window closes it (the query field has its own,
    // stop-search, because it consumes the key).
    GtkEventController* winKeys = gtk_event_controller_key_new();
    g_signal_connect(winKeys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*,
        guint keyval, guint, GdkModifierType, gpointer w) -> gboolean {
            if (keyval != GDK_KEY_Escape) return FALSE;
            gtk_window_close(GTK_WINDOW(w));
            return TRUE;
        }), window_);
    gtk_widget_add_controller(window_, winKeys);
    // The options' keys and the panes', before a field takes them. This
    // window is no application window, so the editor's accelerators do not
    // reach it.
    GtkEventController* windowKeys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(windowKeys, GTK_PHASE_CAPTURE);
    g_signal_connect(windowKeys, "key-pressed", G_CALLBACK(onWindowKey), this);
    gtk_widget_add_controller(window_, windowKeys);
}

// A pane command's key (kPaneCommands, as keys.conf leaves them): the
// editor window runs it, taking the keyboard first, so what is typed next
// goes to that pane and never on into this window's fields.
bool SearchPanel::runPaneKey(GdkEvent* key) {
    if (!paneCommand_ || !key) return false;
    // Typing never runs one (Commands::typesText): only Ctrl, Alt or F keys.
    const guint keyval = gdk_key_event_get_keyval(key);
    if (!(gdk_event_get_modifier_state(key) & (GDK_CONTROL_MASK | GDK_ALT_MASK)) &&
        !(keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F35))
        return false;
    for (const char* id : kPaneCommands)
        for (const Commands::Key& k : Keys::keymap().keysOf(id))
            for (const std::string& accel : Commands::linuxAccelerators(k)) {
                GtkShortcutTrigger* t = gtk_shortcut_trigger_parse_string(accel.c_str());
                if (!t) continue;
                const bool hit = gtk_shortcut_trigger_trigger(t, key, FALSE) == GDK_KEY_MATCH_EXACT;
                g_object_unref(t);
                if (!hit) continue;
                // A copy: the editor window may come back into this panel.
                const std::function<void(const std::string&)> run = paneCommand_;
                run(id);
                return true;
            }
    return false;
}

// The registry's keys for the options and Replace All, here too: Alt+C,
// Alt+W, Alt+R and Alt+P, and Ctrl+Alt+Enter, as keys.conf leaves them;
// then the panes' (runPaneKey).
gboolean SearchPanel::onWindowKey(GtkEventControllerKey* c, guint keyval, guint,
                                  GdkModifierType mods, gpointer selfp) {
    SearchPanel* self = static_cast<SearchPanel*>(selfp);
    GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(c));
    if (self->mode_ != Mode::Find) return self->runPaneKey(event);
    Commands::Key k;
    const GdkModifierType m =
        (GdkModifierType)(mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK));
    if (m & GDK_CONTROL_MASK) k.mods |= Commands::Ctrl;
    if (m & GDK_ALT_MASK) k.mods |= Commands::Alt;
    if (m & GDK_SHIFT_MASK) k.mods |= Commands::Shift;
    const guint lower = gdk_keyval_to_lower(keyval);
    if (lower == GDK_KEY_Return || lower == GDK_KEY_KP_Enter) k.name = "enter";
    else if (lower < 0x80 && g_ascii_isalpha((gchar)lower)) k.name = std::string(1, (char)lower);
    else return self->runPaneKey(event);
    const struct { const char* id; GtkWidget* toggle; } options[] = {
        {"toggleFindCaseSensitive", self->case_},
        {"toggleFindWholeWord", self->word_},
        {"toggleFindRegex", self->regex_},
        {"togglePreserveCase", self->preserve_},
    };
    for (const auto& o : options)
        for (const Commands::Key& key : Keys::keymap().keysOf(o.id))
            if (key == k) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(o.toggle), !active(o.toggle));
                return TRUE;
            }
    // Replace All only while the replace row shows (its field is what
    // replaces; hidden, it is no text shown to write), as the Mac's
    // replaceAllFromKey.
    for (const Commands::Key& key : Keys::keymap().keysOf("editor.action.replaceAll"))
        if (key == k) {
            if (self->replaceShown_) self->replaceAll();
            else gtk_widget_error_bell(self->window_);
            return TRUE;
        }
    return self->runPaneKey(event);
}

// ------------------------------------------------------------------ scope

void SearchPanel::setRoot(const std::string& root) {
    if (root.empty() || root == root_) return;
    root_ = root;
    setScope(root);
}

void SearchPanel::setScope(const std::string& dir) {
    if (dir.empty()) return;
    if (dir == scope_) { updateScopeField(); return; }
    scope_ = dir;
    updateScopeField();
    // A hidden TODOs window scans when show() brings it up.
    if (mode_ == Mode::Todos && !gtk_widget_get_visible(window_)) return;
    runSearch();
}

// The scope as the user would write it: the home folder shown as "~".
void SearchPanel::updateScopeField() {
    std::string shown = scope_;
    const char* home = g_get_home_dir();
    if (home && *home) {
        const std::string h = home;
        if (shown == h) shown = "~";
        else if (shown.compare(0, h.size() + 1, h + "/") == 0)
            shown = "~" + shown.substr(h.size());
    }
    gtk_editable_set_text(GTK_EDITABLE(scopeEntry_), shown.c_str());
}

// A path typed into the scope field: "~/..." is the home folder, a relative
// path is taken from the open folder, and anything that is not a folder is
// refused with the scope left as it was.
void SearchPanel::scopeEntered() {
    std::string typed = gtk_editable_get_text(GTK_EDITABLE(scopeEntry_));
    typed = FolderSearch::normalizeQuery(typed);   // trims the same way
    std::string path;
    if (typed == "~" || typed.compare(0, 2, "~/") == 0)
        path = std::string(g_get_home_dir()) + typed.substr(1);
    else if (!typed.empty() && typed[0] == '/')
        path = typed;
    else
        path = root_ + "/" + typed;
    char* canon = g_canonicalize_filename(path.c_str(), nullptr);
    path = canon ? canon : path;
    g_free(canon);
    if (g_file_test(path.c_str(), G_FILE_TEST_IS_DIR)) {
        setScope(path);
    } else {
        updateScopeField();   // back to the folder still in use
        setStatus("Not a folder");
    }
}

void SearchPanel::chooseScope() {
    GtkFileDialog* dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Search in Folder");
    GFile* initial = g_file_new_for_path(scope_.c_str());
    gtk_file_dialog_set_initial_folder(dlg, initial);
    g_object_unref(initial);
    // The picker can outlive the panel: the portal's dialog is modal only
    // to the search window, so the editor window (and this panel with it)
    // can close while it is up. The callback gets the panel together with
    // a weak reference to life_, and touches the panel only if that is
    // still alive.
    struct Pick {
        SearchPanel* self;
        std::weak_ptr<int> alive;
    };
    gtk_file_dialog_select_folder(dlg, GTK_WINDOW(window_), nullptr,
        [](GObject* src, GAsyncResult* res, gpointer data) {
            std::unique_ptr<Pick> pick(static_cast<Pick*>(data));
            GFile* folder = gtk_file_dialog_select_folder_finish(
                GTK_FILE_DIALOG(src), res, nullptr);
            if (!folder) return;   // cancelled
            char* path = g_file_get_path(folder);
            if (path && !pick->alive.expired()) pick->self->setScope(path);
            g_free(path);
            g_object_unref(folder);
        }, new Pick{this, life_});
    g_object_unref(dlg);
}

// ------------------------------------------------------------------ show

void SearchPanel::show() {
    gtk_window_present(GTK_WINDOW(window_));
    if (mode_ == Mode::Todos) runSearch();   // files change between looks
    gtk_widget_grab_focus(entry_);
    gtk_editable_select_region(GTK_EDITABLE(entry_), 0, -1);
}

void SearchPanel::showReplace() {
    if (mode_ != Mode::Find) {
        show();
        return;
    }
    gtk_window_present(GTK_WINDOW(window_));
    setReplaceShown(true);
    gtk_widget_grab_focus(replaceEntry_);
    gtk_editable_select_region(GTK_EDITABLE(replaceEntry_), 0, -1);
}

void SearchPanel::setReplaceShown(bool on) {
    if (!replaceRow_) return;
    replaceShown_ = on;
    gtk_widget_set_visible(replaceRow_, on);
    gtk_button_set_icon_name(GTK_BUTTON(chevron_), on ? "pan-down-symbolic" : "pan-end-symbolic");
}

std::string SearchPanel::statusText() const {
    return gtk_label_get_text(GTK_LABEL(status_));
}

void SearchPanel::setStatus(const std::string& s) {
    gtk_label_set_text(GTK_LABEL(status_), s.c_str());
}

void SearchPanel::clearResults() {
    hits_.clear();
    gtk_string_list_splice(rows_, 0,
                           g_list_model_get_n_items(G_LIST_MODEL(rows_)), nullptr);
}

FolderSearchOptions SearchPanel::options() const {
    FolderSearchOptions o;
    if (mode_ == Mode::Find) {
        o.caseSensitive = active(case_);
        o.wholeWord = active(word_);
        o.regex = active(regex_);
        o.preserveCase = active(preserve_);
        o.include = textOf(includeEntry_);
        o.exclude = textOf(excludeEntry_);
    }
    // The window's folder's ignore files apply to a scope inside it.
    o.ignoreTop = root_;
    return o;
}

std::string SearchPanel::replacement() const { return textOf(replaceEntry_); }

// ------------------------------------------------------------------ search

void SearchPanel::runSearch() {
    // Cancel whatever is in flight first: its result will carry an old
    // generation and be dropped, and its flag stops it scanning.
    ++generation_;
    if (cancel_) cancel_->store(true);
    cancel_.reset();
    // A replace's note goes with the results it asked for, and no others.
    const std::string note = afterNote_;
    const bool noteError = afterNoteError_;
    afterNote_.clear();
    afterNoteError_ = false;

    const bool todos = mode_ == Mode::Todos;
    const FolderSearchOptions opts = options();
    const std::string raw = gtk_editable_get_text(GTK_EDITABLE(entry_));
    const std::string query = todos ? std::string() : raw;
    lastQuery_ = query;
    clearResults();
    found_.clear();
    dismissed_.clear();   // a new search shows every row again, as VS Code's does
    if (todos && scope_.empty()) {
        searching_ = false;
        setStatus("");
        return;
    }
    if (!todos && (!FolderSearch::isSearchable(query, opts) || scope_.empty())) {
        searching_ = false;
        setStatus(!note.empty() ? note
                  : FolderSearch::normalizeQuery(query).empty() ? ""
                                                                : "Type at least 2 characters");
        return;
    }

    searching_ = true;
    setStatus(todos ? "Looking for TODOs…" : "Searching…");
    auto* job = new Job;
    job->gen = generation_;
    job->root = scope_;
    job->query = query;
    job->options = opts;
    job->todos = todos;
    job->cancel = std::make_shared<std::atomic<bool>>(false);
    job->alive = life_;
    job->note = note;
    job->noteError = noteError;
    cancel_ = job->cancel;

    GTask* task = g_task_new(nullptr, nullptr, onDone, this);
    g_task_set_task_data(task, job, [](gpointer p) { delete static_cast<Job*>(p); });
    g_task_run_in_thread(task, worker);
    g_object_unref(task);   // the running task holds its own reference
}

// Runs on a GLib worker thread. Touches nothing but the job.
void SearchPanel::worker(GTask* task, gpointer, gpointer data, GCancellable*) {
    Job* job = static_cast<Job*>(data);
    job->result = job->todos ? FolderSearch::findTodos(job->root, job->cancel.get())
                             : FolderSearch::search(job->root, job->query, job->cancel.get(),
                                                    job->options);
    g_task_return_boolean(task, TRUE);
}

// Back on the main thread, in the context the task was created in.
void SearchPanel::onDone(GObject*, GAsyncResult* res, gpointer selfp) {
    Job* job = static_cast<Job*>(g_task_get_task_data(G_TASK(res)));
    if (job->alive.expired()) return;   // the panel went with its window
    static_cast<SearchPanel*>(selfp)->finish(job);
}

void SearchPanel::finish(Job* job) {
    if (job->gen != generation_) return;   // superseded: drop it
    searching_ = false;
    cancel_.reset();

    if (mode_ == Mode::Todos) {
        found_ = std::move(job->result.matches);
        foundFiles_ = job->result.filesMatched;
        foundTruncated_ = job->result.truncated;
        applyFilter();
        return;
    }
    hits_ = std::move(job->result.matches);
    showRows();

    const std::string prefix = job->note.empty() ? std::string() : job->note + ". Now ";
    if (!job->result.error.empty()) {
        setStatus(prefix + job->result.error);
        return;
    }
    // Every match (a row is a line, which may hold several).
    const std::size_t n = job->result.totalMatches, files = job->result.filesMatched;
    std::string status = std::to_string(n) + (n == 1 ? " match in " : " matches in ") +
                         std::to_string(files) + (files == 1 ? " file" : " files");
    if (job->result.tooSlow) status += ". This pattern takes too long; the search stopped";
    else if (job->result.truncated)
        status += " (stopped at the first " + std::to_string(hits_.size()) + " lines)";
    if (!job->note.empty() && n == 0) status = "no matches";
    setStatus(prefix + status);
}

// The TODOs whose "path:line  text" holds what is in the field, in any case.
void SearchPanel::applyFilter() {
    const std::string q =
        FolderSearch::normalizeQuery(gtk_editable_get_text(GTK_EDITABLE(entry_)));
    hits_.clear();
    for (const FolderSearchMatch& m : found_) {
        std::size_t col = 0, len = 0;
        if (q.empty() ||
            FolderSearch::findInLine(m.relativePath + ":" + std::to_string(m.line) + "  " + m.text,
                                     q, false, &col, &len))
            hits_.push_back(m);
    }
    showRows();
    setTodoStatus();
}

void SearchPanel::setTodoStatus() {
    if (searching_) return;
    const std::size_t n = found_.size(), files = foundFiles_;
    if (n == 0) {
        setStatus("No TODOs");
        return;
    }
    std::string status = std::to_string(n) + (n == 1 ? " TODO in " : " TODOs in ") +
                         std::to_string(files) + (files == 1 ? " file" : " files");
    if (foundTruncated_) status += " (stopped at the first " + std::to_string(n) + ")";
    if (hits_.size() != n) status += ", " + std::to_string(hits_.size()) + " shown";
    setStatus(status);
}

void SearchPanel::showRows() {
    std::vector<std::string> markup;
    markup.reserve(hits_.size());
    for (const FolderSearchMatch& m : hits_) {
        const std::string loc = m.relativePath + ":" + std::to_string(m.line);
        char* l = g_markup_escape_text(loc.c_str(), -1);
        char* t = g_markup_escape_text(m.text.c_str(), -1);
        markup.push_back(std::string("<span foreground=\"") + kLocationColor + "\">" +
                         l + "</span>  " + t);
        g_free(l);
        g_free(t);
    }
    std::vector<const char*> ptrs;
    ptrs.reserve(markup.size() + 1);
    for (const std::string& s : markup) ptrs.push_back(s.c_str());
    ptrs.push_back(nullptr);
    gtk_string_list_splice(rows_, 0,
                           g_list_model_get_n_items(G_LIST_MODEL(rows_)), ptrs.data());
}

// ------------------------------------------------------------------ replace

// The query, replacement and options as they are now, or false (with the
// bell and a word in the status line) when there is nothing to replace.
bool SearchPanel::readyToReplace() {
    if (mode_ != Mode::Find || replacing_ || scope_.empty()) {
        gtk_widget_error_bell(window_);
        return false;
    }
    const FolderSearchOptions o = options();
    const std::string raw = gtk_editable_get_text(GTK_EDITABLE(entry_));
    if (!FolderSearch::isSearchable(raw, o)) {
        gtk_widget_error_bell(window_);
        setStatus("Type what to find first");
        return false;
    }
    const TextFind::Query q = FolderSearch::makeQuery(raw, o);
    if (!q.usable()) {
        gtk_widget_error_bell(window_);
        setStatus(q.error());
        return false;
    }
    return true;
}

// How the tabs hold a file, across every window, as the Mac's
// replaceStateOfPath: a tab whose text the disk lacks anywhere leaves it
// alone; a clean tab of this window holding it as text is changed there;
// otherwise the file is written (and other windows' clean tabs follow the
// disk). A tab showing its Markdown or LaTeX preview takes the disk's text
// too. Files are the same by device and inode, so a link and its file are.
SearchPanel::Hold SearchPanel::holdOf(const std::string& path, Document** here) const {
    if (here) *here = nullptr;
    if (!editors_) return Hold::None;
    const FolderSearch::FileStamp target = FolderSearch::stampOf(path);
    Hold hold = Hold::None;
    for (Editor* ed : editors_()) {
        for (Document* d : ed->documents()) {
            if (d->kind() != Document::Kind::Text || d->path().empty()) continue;
            bool same = d->path() == path;
            if (!same && target.exists) {
                const FolderSearch::FileStamp st = FolderSearch::stampOf(d->path());
                same = st.exists && st.device == target.device && st.inode == target.inode;
            }
            if (!same) continue;
            if (d->dirty()) return Hold::Conflict;
            if (ed == mine_ && !d->showsPreview()) {
                hold = Hold::Here;
                if (here) *here = d;
            }
        }
    }
    return hold;
}

void SearchPanel::replaceAll() {
    if (!readyToReplace()) return;
    replacing_ = true;
    setStatus("Counting matches…");
    auto* job = new PlanJob;
    job->self = this;
    job->alive = life_;
    job->root = scope_;
    job->query = gtk_editable_get_text(GTK_EDITABLE(entry_));
    job->replacement = replacement();
    job->options = options();
    // As they are now: a search while the matches are counted clears the
    // panel's, never what this replace leaves alone.
    job->dismissed = dismissed_;
    GTask* task = g_task_new(nullptr, nullptr, [](GObject*, GAsyncResult* res, gpointer) {
        PlanJob* j = static_cast<PlanJob*>(g_task_get_task_data(G_TASK(res)));
        if (!j->alive.expired()) j->self->planned(j);
    }, nullptr);
    g_task_set_task_data(task, job, [](gpointer p) { delete static_cast<PlanJob*>(p); });
    g_task_run_in_thread(task, [](GTask* t, gpointer, gpointer data, GCancellable*) {
        PlanJob* j = static_cast<PlanJob*>(data);
        j->plan = FolderSearch::planReplace(j->root, j->query, nullptr, j->options);
        // The rows dismissed from the results stay as they are.
        FolderSearch::excludeDismissed(j->plan, j->query, j->options, j->dismissed);
        g_task_return_boolean(t, TRUE);
    });
    g_object_unref(task);
}

void SearchPanel::planned(PlanJob* job) {
    // Files whose tabs hold changes the disk lacks are left out of the
    // count, and named.
    std::vector<std::string> todo, held, away, large = job->plan.tooLarge;
    if (job->plan.tooLargeCount > large.size())
        large.push_back("and " + grouped(job->plan.tooLargeCount - large.size()) + " more");
    // Files read as Windows-1252: searched, but written as UTF-8 or UTF-16
    // only, so left alone and named.
    std::vector<std::string> legacy = job->plan.otherEncoding;
    if (job->plan.otherEncodingCount > legacy.size())
        legacy.push_back("and " + grouped(job->plan.otherEncodingCount - legacy.size()) + " more");
    // Files with a dismissed row whose line changed since the search: left
    // alone whole (FolderSearch::excludeDismissed).
    const std::vector<std::string>& changed = job->plan.changedSinceSearch;
    std::size_t matches = 0;
    for (const FolderReplaceFile& f : job->plan.files) {
        if (holdOf(f.path, nullptr) == Hold::Conflict) {
            held.push_back(baseName(f.path));
            continue;
        }
        todo.push_back(f.path);
        matches += f.matches;
        if (f.outside) away.push_back(f.relativePath);
    }
    if (!job->plan.error.empty() || todo.empty()) {
        replacing_ = false;
        setStatus(!job->plan.error.empty() ? job->plan.error
                  : !held.empty() ? "Nothing replaced: the matches are in tabs with unsaved "
                                    "changes (" + joined(held) + ")"
                  : !large.empty() ? "Nothing replaced: the matches are in files larger than " +
                                         replaceSizeLimit() + ", which Replace in Folder leaves "
                                         "alone (" + joined(large) + ")"
                  : !legacy.empty() ? "Nothing replaced: the matches are in files that are not "
                                      "UTF-8 (read as Windows-1252), which Replace in Folder "
                                      "leaves alone (" + joined(legacy) + ")"
                  : !changed.empty() ? "Nothing replaced: files with dismissed rows changed "
                                       "after the search (" + joined(changed) + "); search again"
                  : job->plan.dismissed ? "Nothing replaced: every match left is in a dismissed row"
                                        : "Nothing to replace");
        return;
    }
    const std::string question = "Replace " + plural(matches, "match", "matches") + " in " +
                                 plural(todo.size(), "file", "files") + "?";
    std::string detail = "Each becomes “" + job->replacement + "”" +
                         (job->options.preserveCase ? ", in the case of what it replaces" : "") +
                         ". Files open in a tab are changed there, unsaved, and can be undone "
                         "there; the others are written to disk.";
    if (!away.empty())
        detail += " Through symbolic links, these are files outside the folder, and they are "
                  "changed too: " + joined(away) + ".";
    if (!held.empty())
        detail += " Left alone, since their tabs have unsaved changes: " + joined(held) + ".";
    if (!large.empty())
        detail += " Left alone, since they are larger than " + replaceSizeLimit() + ": " +
                  joined(large) + ".";
    if (!legacy.empty())
        detail += " Left alone, since they are not UTF-8 (read as Windows-1252): " +
                  joined(legacy) + ".";
    if (const std::size_t d = job->plan.dismissed)
        detail += " " + plural(d, "match", "matches") + " in dismissed rows " +
                  (d == 1 ? "is" : "are") + " left alone.";
    if (!changed.empty())
        detail += " Left alone, since they changed after the search and have dismissed rows: " +
                  joined(changed) + ".";
    // The detail can list files by their paths: Question wraps inside them
    // and scrolls a long list.
    Question::Spec spec;
    spec.message = question;
    spec.detail = detail;
    spec.buttons = {"Replace", "Cancel"};
    spec.defaultButton = 0;
    spec.cancelButton = 1;
    struct Ask {
        SearchPanel* self;
        std::weak_ptr<int> alive;
        std::vector<std::string> todo, held;
        FolderReplaceExclusions dismissed;
    };
    auto ask = std::make_shared<Ask>(
        Ask{this, life_, std::move(todo), std::move(held), job->dismissed});
    Question::ask(GTK_WINDOW(window_), spec, [ask](int answer) {
        if (ask->alive.expired()) return;
        if (answer != 0) {
            ask->self->replacing_ = false;
            ask->self->setStatus("Nothing replaced");
            return;
        }
        ask->self->replaceInPaths(ask->todo, nullptr, ask->held, ask->dismissed);
    });
}

void SearchPanel::replaceMatchAt(guint row) {
    if (row >= hits_.size() || !readyToReplace()) return;
    replacing_ = true;
    const FolderSearchMatch hit = hits_[row];
    replaceInPaths({hit.path}, &hit, {}, FolderReplaceExclusions());
}

// Replace every match in these files, or only `hit`'s. Tabs first, on the
// main thread through MultiEdit, then the other files on disk, off it.
// `held` names files already left out for their unsaved tabs. `dismissed`
// (Replace All's) holds the rows taken out of the results: their files, and
// their lines' matches, are left alone (FolderSearch::dismissedLines), and a
// file whose dismissed line no longer reads as its row showed is left alone
// whole and named.
void SearchPanel::replaceInPaths(const std::vector<std::string>& paths,
                                 const FolderSearchMatch* hit, std::vector<std::string> held,
                                 const FolderReplaceExclusions& dismissed) {
    const FolderSearchOptions o = options();
    const std::string raw = gtk_editable_get_text(GTK_EDITABLE(entry_));
    const std::string with = replacement();
    const TextFind::Query query = FolderSearch::makeQuery(raw, o);
    const std::u16string with16 = utf16::fromUtf8(with);

    auto* job = new WriteJob;
    job->self = this;
    job->alive = life_;
    job->query = raw;
    job->replacement = with;
    job->options = o;
    job->one = hit != nullptr;
    if (hit) {
        job->line = hit->line;
        job->byteColumn = hit->byteColumn;
        job->expected = hit->text;   // the row's match: only while its line reads as searched
    }
    if (!hit) job->dismissed = dismissed;
    job->skipped = std::move(held);
    // One file under two names (a symlink, a hard link) is replaced once.
    std::set<std::pair<std::uint64_t, std::uint64_t>> seen;
    for (const std::string& path : paths) {
        const FolderSearch::FileStamp st = FolderSearch::stampOf(path);
        if (st.exists && !seen.insert({st.device, st.inode}).second) continue;
        if (job->dismissed.excludesFile(path)) continue;
        Document* here = nullptr;
        const Hold hold = holdOf(path, &here);
        if (hold == Hold::Conflict) {
            job->skipped.push_back(baseName(path));
            continue;
        }
        if (hold != Hold::Here || !here) {
            job->paths.push_back(path);
            continue;
        }
        const std::string text = here->text();
        const std::u16string u = utf16::fromUtf8(text);
        std::vector<TextFind::Edit16> edits;
        if (hit) {
            TextFind::Edit16 e;
            std::string now;
            if (FolderSearch::lineAt(text, hit->line, &now) &&
                FolderSearch::displayText(now, o.maxLineChars) == job->expected &&
                TextFind::replaceOneAt(u, query, with16, hit->line, hit->byteColumn, &e))
                edits.push_back(e);
        } else {
            std::set<int> keep;
            if (!FolderSearch::dismissedLines(text, path, job->dismissed, o.maxLineChars, &keep)) {
                job->problems.push_back(baseName(path) +
                                        " changed after the search and has dismissed rows: "
                                        "left alone");
                continue;
            }
            edits = TextFind::replaceAll(u, query, with16);
            TextFind::dropEditsOnLines(u, &edits, keep);
        }
        if (edits.empty()) continue;
        std::vector<std::size_t> at;
        for (const TextFind::Edit16& e : edits) {
            at.push_back(e.start);
            at.push_back(e.start + e.length);
        }
        const std::vector<std::size_t> cp = EditSpans::codePoints(u, at);
        std::vector<MultiEdit::Change> changes;
        changes.reserve(edits.size());
        for (std::size_t i = 0; i < edits.size(); i++)
            changes.push_back({cp[2 * i], cp[2 * i + 1] - cp[2 * i], utf16::toUtf8(edits[i].text)});
        if (MultiEdit::apply(here->buffer(), changes, true, "Replace in Folder")) {
            job->tabFiles++;
            job->tabMatches += edits.size();
        } else {
            job->problems.push_back(baseName(path) + " could not be changed in its tab");
        }
    }

    GTask* task = g_task_new(nullptr, nullptr, [](GObject*, GAsyncResult* res, gpointer) {
        WriteJob* j = static_cast<WriteJob*>(g_task_get_task_data(G_TASK(res)));
        if (!j->alive.expired()) j->self->written(j);
    }, nullptr);
    g_task_set_task_data(task, job, [](gpointer p) { delete static_cast<WriteJob*>(p); });
    g_task_run_in_thread(task, [](GTask* t, gpointer, gpointer data, GCancellable*) {
        WriteJob* j = static_cast<WriteJob*>(data);
        for (const std::string& p : j->paths) {
            std::string content, out, error;
            // A UTF-16 file is replaced in its decoded text and written back
            // as UTF-16 with its byte order mark.
            FolderSearch::TextEncoding encoding = FolderSearch::TextEncoding::Utf8;
            if (!FolderSearch::readTextFile(p, FolderSearchOptions().maxFileSize, &content,
                                            &encoding)) {
                const FolderSearch::FileStamp st = FolderSearch::stampOf(p);
                if (st.exists && st.size > FolderSearchOptions().maxFileSize)
                    j->failed.push_back(baseName(p) + " is larger than " + replaceSizeLimit() +
                                        ", which Replace in Folder leaves alone");
                else if (readsAsWindows1252(p))
                    j->failed.push_back(baseName(p) + " is not UTF-8 (read as Windows-1252), "
                                                      "which Replace in Folder leaves alone");
                else
                    j->failed.push_back(baseName(p) + " could not be read as text");
                continue;
            }
            std::size_t n = 0;
            if (j->one) {
                if (FolderSearch::replaceMatchInText(content, j->query, j->replacement, j->options,
                                                     j->line, j->byteColumn, &out, &j->expected))
                    n = 1;
            } else {
                std::set<int> keep;
                if (!FolderSearch::dismissedLines(content, p, j->dismissed, j->options.maxLineChars,
                                                  &keep)) {
                    j->failed.push_back(baseName(p) +
                                        " changed after the search and has dismissed rows: "
                                        "left alone");
                    continue;
                }
                out = FolderSearch::replaceInText(content, j->query, j->replacement, j->options, &n,
                                                  &keep);
            }
            if (n == 0 || out == content) continue;
            if (!FolderSearch::writeFileAtomically(p, FolderSearch::encodeText(out, encoding),
                                                   &error)) {
                j->failed.push_back(baseName(p) + ": " + error);
                continue;
            }
            j->files++;
            j->matches += n;
        }
        g_task_return_boolean(t, TRUE);
    });
    g_object_unref(task);
}

void SearchPanel::written(WriteJob* job) {
    replacing_ = false;
    std::vector<std::string> problems = job->problems;
    problems.insert(problems.end(), job->failed.begin(), job->failed.end());
    const std::size_t total = job->matches + job->tabMatches, files = job->files + job->tabFiles;
    std::string msg;
    if (total == 0 && job->skipped.empty() && problems.empty())
        msg = job->one ? "That match is no longer there" : "Nothing to replace";
    else
        msg = "Replaced " + plural(total, "match", "matches") + " in " +
              plural(files, "file", "files");
    if (job->tabFiles) msg += " (" + std::to_string(job->tabFiles) + " open in tabs, unsaved)";
    if (!job->skipped.empty()) msg += ". Left alone, with unsaved changes: " + joined(job->skipped);
    if (!problems.empty()) msg += ". " + joined(problems, "; ");
    // The results as they are now, after this note.
    afterNote_ = msg;
    afterNoteError_ = !problems.empty();
    runSearch();
    if (!searching_) setStatus(msg);
}

// ------------------------------------------------------------------ dismiss

// VS Code's Dismiss (the Mac's dismissItem:): the row leaves the results,
// and Replace All leaves what it held alone until the next search.
void SearchPanel::dismissRow(guint row) {
    if (row >= hits_.size() || replacing_) return;
    const FolderSearchMatch m = hits_[row];
    if (mode_ == Mode::Find) dismissed_.dismissLine(m.path, m.line, m.text);
    removeRows([&m](const FolderSearchMatch& h) { return h.path == m.path && h.line == m.line; });
}

// A file's rows, all of them (the Mac's Dismiss on a file's row).
void SearchPanel::dismissFileOf(guint row) {
    if (row >= hits_.size() || replacing_) return;
    const std::string path = hits_[row].path;
    if (mode_ == Mode::Find) dismissed_.dismissFile(path);
    removeRows([&path](const FolderSearchMatch& h) { return h.path == path; });
}

// The rows `gone` picks out of hits_ and the list, a run at a time, so the
// rest keep their places and the list its scroll. The keyboard, when it was
// in the list, goes to the row now where the first one was: a removed
// row's widget keeps the window's focus until GTK's next frame, every key
// until then lost (ListKeyboard.h), so the window's box holds it meanwhile
// (a box takes the focus itself; the scroller would pass it to the list).
void SearchPanel::removeRows(const std::function<bool(const FolderSearchMatch&)>& gone) {
    GtkWidget* holder = gtk_widget_get_parent(gtk_widget_get_parent(listView_));
    const bool had = ListKeyboard::hold(listView_, holder);
    guint first = GTK_INVALID_LIST_POSITION;
    for (guint end = (guint)hits_.size(); end > 0;) {
        if (!gone(hits_[end - 1])) {
            --end;
            continue;
        }
        guint start = end - 1;
        while (start > 0 && gone(hits_[start - 1])) --start;
        hits_.erase(hits_.begin() + start, hits_.begin() + end);
        gtk_string_list_splice(rows_, start, end - start, nullptr);
        first = start;
        end = start;
    }
    if (mode_ == Mode::Todos) {
        found_.erase(std::remove_if(found_.begin(), found_.end(), gone), found_.end());
        std::set<std::string> files;
        for (const FolderSearchMatch& m : found_) files.insert(m.path);
        foundFiles_ = files.size();
    }
    if (first != GTK_INVALID_LIST_POSITION && !hits_.empty()) {
        const guint at = std::min<guint>(first, (guint)hits_.size() - 1);
        GtkListScrollFlags flags = GTK_LIST_SCROLL_SELECT;
        if (had) flags = (GtkListScrollFlags)(flags | GTK_LIST_SCROLL_FOCUS);
        gtk_list_view_scroll_to(GTK_LIST_VIEW(listView_), at, flags, nullptr);
        if (had) gtk_widget_grab_focus(listView_);
    } else if (had) {
        gtk_widget_grab_focus(entry_);   // nothing left to hold it
    }
    if (had) gtk_widget_set_focusable(holder, FALSE);
    if (first == GTK_INVALID_LIST_POSITION) return;
    if (mode_ == Mode::Todos) setTodoStatus();
    else showDismissedStatus();
}

// The status line after a dismiss: what is left to replace.
void SearchPanel::showDismissedStatus() {
    std::size_t matches = 0;
    std::set<std::string> files;
    for (const FolderSearchMatch& m : hits_) {
        matches += m.count;
        files.insert(m.path);
    }
    setStatus(grouped(matches) + (matches == 1 ? " match in " : " matches in ") +
              grouped(files.size()) + (files.size() == 1 ? " file" : " files") +
              " (dismissed rows are left alone)");
}

// A row's right click menu: Open, Replace This Match (Find in Folder),
// Dismiss and Dismiss File. Its items find their row again by its file and
// line, since a search may have changed the list while it was up.
void SearchPanel::showRowMenu(guint row, double x, double y) {
    if (row >= hits_.size()) return;
    if (rowMenu_) gtk_popover_popdown(GTK_POPOVER(rowMenu_));
    gtk_list_view_scroll_to(GTK_LIST_VIEW(listView_), row,
        (GtkListScrollFlags)(GTK_LIST_SCROLL_FOCUS | GTK_LIST_SCROLL_SELECT), nullptr);
    gtk_widget_grab_focus(listView_);

    GtkWidget* pop = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);
    gtk_widget_add_css_class(pop, "menu");
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    struct Item {
        SearchPanel* self;
        std::weak_ptr<int> alive;
        std::string path;
        int line;
        int what;   // 0 open, 1 replace, 2 dismiss, 3 dismiss the file
    };
    const FolderSearchMatch& m = hits_[row];
    auto add = [&](const char* label, int what) {
        GtkWidget* b = gtk_button_new_with_label(label);
        gtk_widget_add_css_class(b, "flat");
        gtk_label_set_xalign(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(b))), 0.0);
        g_object_set_data_full(G_OBJECT(b), "minicode-row-item",
                               new Item{this, life_, m.path, m.line, what},
                               [](gpointer p) { delete static_cast<Item*>(p); });
        g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer popp) {
            const Item it = *static_cast<Item*>(g_object_get_data(G_OBJECT(button),
                                                                  "minicode-row-item"));
            gtk_popover_popdown(GTK_POPOVER(popp));
            if (it.alive.expired()) return;
            SearchPanel* self = it.self;
            // The keyboard back in the results, where the menu came from.
            gtk_widget_grab_focus(self->listView_);
            guint at = GTK_INVALID_LIST_POSITION;
            for (guint i = 0; i < self->hits_.size(); i++)
                if (self->hits_[i].path == it.path && self->hits_[i].line == it.line) {
                    at = i;
                    break;
                }
            if (at == GTK_INVALID_LIST_POSITION) return;
            switch (it.what) {
            case 0: self->openRow(at, true); break;
            case 1: self->replaceMatchAt(at); break;
            case 2: self->dismissRow(at); break;
            default: self->dismissFileOf(at); break;
            }
        }), pop);
        gtk_box_append(GTK_BOX(box), b);
    };
    add("Open", 0);
    if (mode_ == Mode::Find) add("Replace This Match", 1);
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    add("Dismiss", 2);
    add("Dismiss File", 3);
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    gtk_widget_set_parent(pop, listView_);
    const GdkRectangle at = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &at);
    // Taken off the list from an idle, since it is still emitting "closed".
    g_signal_connect(pop, "closed", G_CALLBACK(+[](GtkPopover* p, gpointer selfp) {
        SearchPanel* self = static_cast<SearchPanel*>(selfp);
        if (self->rowMenu_ == GTK_WIDGET(p)) self->rowMenu_ = nullptr;
        g_idle_add([](gpointer w) -> gboolean {
            GtkWidget* widget = GTK_WIDGET(w);
            if (gtk_widget_get_parent(widget)) gtk_widget_unparent(widget);
            g_object_unref(widget);
            return G_SOURCE_REMOVE;
        }, g_object_ref(p));
    }), this);
    rowMenu_ = pop;
    gtk_popover_popup(GTK_POPOVER(pop));
}

void SearchPanel::onRowMenu(GtkGestureClick* g, int, double x, double y, gpointer selfp) {
    SearchPanel* self = static_cast<SearchPanel*>(selfp);
    const guint row = self->rowAt(x, y);
    if (row == GTK_INVALID_LIST_POSITION) return;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    self->showRowMenu(row, x, y);
}

// ------------------------------------------------------------------ signals

void SearchPanel::onSearchChanged(GtkSearchEntry*, gpointer selfp) {
    SearchPanel* self = static_cast<SearchPanel*>(selfp);
    if (self->mode_ == Mode::Todos) self->applyFilter();
    else self->runSearch();
}

void SearchPanel::onQueryActivate(GtkSearchEntry*, gpointer selfp) {
    static_cast<SearchPanel*>(selfp)->fieldReturn();
}

// Enter in the query, or in the replacement, which never replaces (#210):
// it searches at once, unless the list already answers this query, in which
// case it moves into the list (Enter again opens the match). In the TODOs
// window the list always answers the field. (A search shows the dismissed
// rows again, so while some are left Enter keeps the dismissals.)
void SearchPanel::fieldReturn() {
    if (mode_ == Mode::Todos) {
        if (!hits_.empty()) focusFirstResult();
        return;
    }
    const std::string q = gtk_editable_get_text(GTK_EDITABLE(entry_));
    if (q != lastQuery_ || (hits_.empty() && !searching_)) {
        runSearch();
    } else if (!hits_.empty()) {
        focusFirstResult();
    }
}

// Select the first result and give the list the keyboard. GTK_LIST_SCROLL_FOCUS
// alone moves the list's own focus item but not the window's focus when the
// list does not have it, so the keyboard stayed in the query field: found
// with real key presses in September 2026.
void SearchPanel::focusFirstResult() {
    gtk_list_view_scroll_to(GTK_LIST_VIEW(listView_), 0,
        (GtkListScrollFlags)(GTK_LIST_SCROLL_FOCUS | GTK_LIST_SCROLL_SELECT), nullptr);
    gtk_widget_grab_focus(listView_);
}

void SearchPanel::onStopSearch(GtkSearchEntry*, gpointer selfp) {
    gtk_window_close(GTK_WINDOW(static_cast<SearchPanel*>(selfp)->window_));
}

// Down arrow from the query field goes to the first result.
gboolean SearchPanel::onQueryKey(GtkEventControllerKey*, guint keyval, guint,
                                 GdkModifierType, gpointer selfp) {
    SearchPanel* self = static_cast<SearchPanel*>(selfp);
    if (keyval != GDK_KEY_Down || self->hits_.empty()) return FALSE;
    self->focusFirstResult();
    return TRUE;
}

void SearchPanel::openRow(guint pos, bool kept) {
    if (pos >= hits_.size() || !openCb_) return;
    // A copy, because the callback may come back into the panel (a new
    // search replaces hits_).
    const FolderSearchMatch m = hits_[pos];
    openCb_(m, kept, openUser_);
}

// The result row under a point in the list's coordinates.
guint SearchPanel::rowAt(double x, double y) const {
    for (GtkWidget* w = gtk_widget_pick(listView_, x, y, GTK_PICK_DEFAULT);
         w && w != listView_; w = gtk_widget_get_parent(w))
        if (auto* li = static_cast<GtkListItem*>(
                g_object_get_data(G_OBJECT(w), "minicode-list-item")))
            return gtk_list_item_get_position(li);
    return GTK_INVALID_LIST_POSITION;
}

guint SearchPanel::selectedRow() const {
    GtkSelectionModel* model = gtk_list_view_get_model(GTK_LIST_VIEW(listView_));
    return gtk_single_selection_get_selected(GTK_SINGLE_SELECTION(model));
}

void SearchPanel::onRowPressed(GtkGestureClick* g, int, double x, double y, gpointer selfp) {
    SearchPanel* self = static_cast<SearchPanel*>(selfp);
    self->pressTime_ = gtk_event_controller_get_current_event_time(GTK_EVENT_CONTROLLER(g));
    self->pressRow_ = self->rowAt(x, y);
    // A row's × dismisses it (its own "clicked"); it never opens the row.
    for (GtkWidget* w = gtk_widget_pick(self->listView_, x, y, GTK_PICK_DEFAULT);
         w && w != self->listView_; w = gtk_widget_get_parent(w))
        if (gtk_widget_has_css_class(w, "minicode-dismiss")) {
            self->pressRow_ = GTK_INVALID_LIST_POSITION;
            break;
        }
}

// The first click of a double-click opens the match in the preview tab; the
// second keeps it in a tab of its own, as on the Mac. A press released over
// another row does nothing.
// The count is by the presses' event times (ClickCount): a slow first open
// made GTK's own timer reset the second press to n = 1.
void SearchPanel::onRowReleased(GtkGestureClick*, int gtkN, double x, double y, gpointer selfp) {
    SearchPanel* self = static_cast<SearchPanel*>(selfp);
    const guint pressed = self->pressRow_;
    self->pressRow_ = GTK_INVALID_LIST_POSITION;
    const int n = pressed == GTK_INVALID_LIST_POSITION
                      ? gtkN
                      : self->clicks_.press(self->listView_, self->pressTime_,
                                            GUINT_TO_POINTER(pressed + 1), gtkN, x, y);
    if ((n == 1 || n == 2) && pressed != GTK_INVALID_LIST_POSITION &&
        self->rowAt(x, y) == pressed) {
        // The first click shows the match in the preview tab with the
        // keyboard in the results (VS Code's search view, the Mac's
        // previewHandler), so the arrows go on from this row; the second
        // keeps it, and the editor window comes up (main.cpp).
        if (n == 1) {
            gtk_list_view_scroll_to(GTK_LIST_VIEW(self->listView_), pressed,
                (GtkListScrollFlags)(GTK_LIST_SCROLL_FOCUS | GTK_LIST_SCROLL_SELECT), nullptr);
            gtk_widget_grab_focus(self->listView_);
        }
        self->openRow(pressed, n == 2);
    }
}

// Enter on a result opens it; Ctrl+Shift+1 replaces its match alone (VS
// Code's key for a match in its search view); Delete dismisses it (VS
// Code's Dismiss, search.action.remove, on Linux).
gboolean SearchPanel::onListKey(GtkEventControllerKey*, guint keyval, guint,
                                GdkModifierType mods, gpointer selfp) {
    SearchPanel* self = static_cast<SearchPanel*>(selfp);
    if ((keyval == GDK_KEY_Delete || keyval == GDK_KEY_KP_Delete) &&
        !(mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK))) {
        const guint pos = self->selectedRow();
        if (pos == GTK_INVALID_LIST_POSITION) return FALSE;
        self->dismissRow(pos);
        return TRUE;
    }
    if (self->mode_ == Mode::Find && (mods & GDK_CONTROL_MASK) && (mods & GDK_SHIFT_MASK) &&
        !(mods & GDK_ALT_MASK) && (keyval == GDK_KEY_exclam || keyval == GDK_KEY_1)) {
        const guint pos = self->selectedRow();
        if (pos == GTK_INVALID_LIST_POSITION) return FALSE;
        self->replaceMatchAt(pos);
        return TRUE;
    }
    if (keyval != GDK_KEY_Return && keyval != GDK_KEY_KP_Enter && keyval != GDK_KEY_ISO_Enter)
        return FALSE;
    if (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK)) return FALSE;
    const guint pos = self->selectedRow();
    if (pos == GTK_INVALID_LIST_POSITION) return FALSE;
    self->openRow(pos, true);
    return TRUE;
}

// A row: its text, and the × that dismisses it (shown while the pointer is
// over the row or it is selected: installRowCss).
void SearchPanel::onSetup(GtkSignalListItemFactory*, GObject* obj, gpointer selfp) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget* label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_use_markup(GTK_LABEL(label), TRUE);
    gtk_widget_add_css_class(label, "monospace");
    gtk_widget_set_margin_top(label, 2);
    gtk_widget_set_margin_bottom(label, 2);
    gtk_widget_set_hexpand(label, TRUE);
    GtkWidget* dismiss = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(dismiss, "flat");
    gtk_widget_add_css_class(dismiss, "minicode-dismiss");
    gtk_widget_set_focusable(dismiss, FALSE);
    gtk_widget_set_focus_on_click(dismiss, FALSE);
    gtk_widget_set_valign(dismiss, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(dismiss, "Dismiss (Delete)");
    g_object_set_data(G_OBJECT(dismiss), "minicode-list-item", obj);
    g_signal_connect(dismiss, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer p) {
        auto* li = static_cast<GtkListItem*>(g_object_get_data(G_OBJECT(b), "minicode-list-item"));
        if (li) static_cast<SearchPanel*>(p)->dismissRow(gtk_list_item_get_position(li));
    }), selfp);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), dismiss);
    gtk_list_item_set_child(GTK_LIST_ITEM(obj), box);
    gtk_list_item_set_activatable(GTK_LIST_ITEM(obj), FALSE);   // clicks are ours
    g_object_set_data(G_OBJECT(box), "minicode-list-item", obj);
}

void SearchPanel::onBind(GtkSignalListItemFactory*, GObject* obj, gpointer) {
    GtkListItem* li = GTK_LIST_ITEM(obj);
    GtkStringObject* s = GTK_STRING_OBJECT(gtk_list_item_get_item(li));
    GtkWidget* label = gtk_widget_get_first_child(gtk_list_item_get_child(li));
    gtk_label_set_markup(GTK_LABEL(label), s ? gtk_string_object_get_string(s) : "");
}
