// Search.cpp — see Search.h. The search itself is FolderSearch::search in the
// shared core, and Replace in Folder its planReplace, replaceInText and
// replaceMatchInText with TextFind's edits for tabs; this file is the
// window, the worker threads, the list, and the tabs.
#include "Search.h"

#include "Document.h"
#include "EditSpans.h"
#include "Editor.h"
#include "Keys.h"
#include "MultiEdit.h"
#include "TextFind.h"
#include "Utf8Offsets.h"

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
        // Enter in the replacement is Replace All, as on the Mac.
        g_signal_connect_swapped(replaceEntry_, "activate", G_CALLBACK(+[](SearchPanel* self) {
            self->replaceAll();
        }), this);
        preserve_ = optionToggle("AB", keyTip("Preserve Case", "togglePreserveCase"));
        replaceAllButton_ = gtk_button_new_with_label("Replace All");
        gtk_widget_set_focus_on_click(replaceAllButton_, FALSE);
        gtk_widget_set_tooltip_text(replaceAllButton_,
                                    keyTip("Replace All (Enter in the replacement)",
                                           "editor.action.replaceAll").c_str());
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
    if (!todos)
        gtk_widget_set_tooltip_text(listView_,
                                    "Ctrl+Shift+1 replaces the selected row's match alone");
    // One click opens a match, as on the Mac, and so does Enter. The rows are
    // not activatable (onSetup), so a double-click cannot open it twice.
    // gtk_list_view_set_single_click_activate is not used because it also
    // selects every row the pointer passes over.
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(onRowPressed), this);
    g_signal_connect(click, "released", G_CALLBACK(onRowReleased), this);
    gtk_widget_add_controller(listView_, GTK_EVENT_CONTROLLER(click));
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
    // The options' keys, before a field takes them. This window is no
    // application window, so the editor's accelerators do not reach it.
    if (!todos) {
        GtkEventController* optionKeys = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(optionKeys, GTK_PHASE_CAPTURE);
        g_signal_connect(optionKeys, "key-pressed", G_CALLBACK(onWindowKey), this);
        gtk_widget_add_controller(window_, optionKeys);
    }
}

// The registry's keys for the options and Replace All, here too: Alt+C,
// Alt+W, Alt+R and Alt+P, and Ctrl+Alt+Enter, as keys.conf leaves them.
gboolean SearchPanel::onWindowKey(GtkEventControllerKey*, guint keyval, guint,
                                  GdkModifierType mods, gpointer selfp) {
    SearchPanel* self = static_cast<SearchPanel*>(selfp);
    Commands::Key k;
    const GdkModifierType m =
        (GdkModifierType)(mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK));
    if (m & GDK_CONTROL_MASK) k.mods |= Commands::Ctrl;
    if (m & GDK_ALT_MASK) k.mods |= Commands::Alt;
    if (m & GDK_SHIFT_MASK) k.mods |= Commands::Shift;
    const guint lower = gdk_keyval_to_lower(keyval);
    if (lower == GDK_KEY_Return || lower == GDK_KEY_KP_Enter) k.name = "enter";
    else if (lower < 0x80 && g_ascii_isalpha((gchar)lower)) k.name = std::string(1, (char)lower);
    else return FALSE;
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
    for (const Commands::Key& key : Keys::keymap().keysOf("editor.action.replaceAll"))
        if (key == k) {
            self->replaceAll();
            return TRUE;
        }
    return FALSE;
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
    GTask* task = g_task_new(nullptr, nullptr, [](GObject*, GAsyncResult* res, gpointer) {
        PlanJob* j = static_cast<PlanJob*>(g_task_get_task_data(G_TASK(res)));
        if (!j->alive.expired()) j->self->planned(j);
    }, nullptr);
    g_task_set_task_data(task, job, [](gpointer p) { delete static_cast<PlanJob*>(p); });
    g_task_run_in_thread(task, [](GTask* t, gpointer, gpointer data, GCancellable*) {
        PlanJob* j = static_cast<PlanJob*>(data);
        j->plan = FolderSearch::planReplace(j->root, j->query, nullptr, j->options);
        g_task_return_boolean(t, TRUE);
    });
    g_object_unref(task);
}

void SearchPanel::planned(PlanJob* job) {
    // Files whose tabs hold changes the disk lacks are left out of the
    // count, and named.
    std::vector<std::string> todo, held, away;
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
    GtkAlertDialog* dlg = gtk_alert_dialog_new("%s", question.c_str());
    gtk_alert_dialog_set_detail(dlg, detail.c_str());
    const char* buttons[] = {"Replace", "Cancel", nullptr};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_cancel_button(dlg, 1);
    gtk_alert_dialog_set_default_button(dlg, 0);
    struct Ask {
        SearchPanel* self;
        std::weak_ptr<int> alive;
        std::vector<std::string> todo, held;
    };
    gtk_alert_dialog_choose(dlg, GTK_WINDOW(window_), nullptr,
        [](GObject* src, GAsyncResult* res, gpointer data) {
            std::unique_ptr<Ask> ask(static_cast<Ask*>(data));
            const int answer = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
            if (ask->alive.expired()) return;
            if (answer != 0) {
                ask->self->replacing_ = false;
                ask->self->setStatus("Nothing replaced");
                return;
            }
            ask->self->replaceInPaths(ask->todo, nullptr, ask->held);
        }, new Ask{this, life_, std::move(todo), std::move(held)});
    g_object_unref(dlg);
}

void SearchPanel::replaceMatchAt(guint row) {
    if (row >= hits_.size() || !readyToReplace()) return;
    replacing_ = true;
    const FolderSearchMatch hit = hits_[row];
    replaceInPaths({hit.path}, &hit, {});
}

// Replace every match in these files, or only `hit`'s. Tabs first, on the
// main thread through MultiEdit, then the other files on disk, off it.
// `held` names files already left out for their unsaved tabs.
void SearchPanel::replaceInPaths(const std::vector<std::string>& paths,
                                 const FolderSearchMatch* hit, std::vector<std::string> held) {
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
    job->skipped = std::move(held);
    // One file under two names (a symlink, a hard link) is replaced once.
    std::set<std::pair<std::uint64_t, std::uint64_t>> seen;
    for (const std::string& path : paths) {
        const FolderSearch::FileStamp st = FolderSearch::stampOf(path);
        if (st.exists && !seen.insert({st.device, st.inode}).second) continue;
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
            edits = TextFind::replaceAll(u, query, with16);
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
        if (MultiEdit::apply(here->buffer(), changes, true)) {
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
                j->failed.push_back(baseName(p) + " could not be read as text");
                continue;
            }
            std::size_t n = 0;
            if (j->one) {
                if (FolderSearch::replaceMatchInText(content, j->query, j->replacement, j->options,
                                                     j->line, j->byteColumn, &out, &j->expected))
                    n = 1;
            } else {
                out = FolderSearch::replaceInText(content, j->query, j->replacement, j->options, &n);
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

// ------------------------------------------------------------------ signals

void SearchPanel::onSearchChanged(GtkSearchEntry*, gpointer selfp) {
    SearchPanel* self = static_cast<SearchPanel*>(selfp);
    if (self->mode_ == Mode::Todos) self->applyFilter();
    else self->runSearch();
}

// Enter searches at once, unless the list already answers this query, in
// which case it moves into the list (Enter again opens the match). In the
// TODOs window the list always answers the field.
void SearchPanel::onQueryActivate(GtkSearchEntry* e, gpointer selfp) {
    SearchPanel* self = static_cast<SearchPanel*>(selfp);
    if (self->mode_ == Mode::Todos) {
        if (!self->hits_.empty()) self->focusFirstResult();
        return;
    }
    const std::string q = gtk_editable_get_text(GTK_EDITABLE(e));
    if (q != self->lastQuery_ || (self->hits_.empty() && !self->searching_)) {
        self->runSearch();
    } else if (!self->hits_.empty()) {
        self->focusFirstResult();
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

void SearchPanel::onRowPressed(GtkGestureClick*, int, double x, double y, gpointer selfp) {
    SearchPanel* self = static_cast<SearchPanel*>(selfp);
    self->pressRow_ = self->rowAt(x, y);
}

// The first click of a double-click opens the match in the preview tab; the
// second keeps it in a tab of its own, as on the Mac. A press released over
// another row does nothing.
void SearchPanel::onRowReleased(GtkGestureClick*, int n, double x, double y, gpointer selfp) {
    SearchPanel* self = static_cast<SearchPanel*>(selfp);
    const guint pressed = self->pressRow_;
    self->pressRow_ = GTK_INVALID_LIST_POSITION;
    if ((n == 1 || n == 2) && pressed != GTK_INVALID_LIST_POSITION &&
        self->rowAt(x, y) == pressed)
        self->openRow(pressed, n == 2);
}

// Enter on a result opens it; Ctrl+Shift+1 replaces its match alone (VS
// Code's key for a match in its search view).
gboolean SearchPanel::onListKey(GtkEventControllerKey*, guint keyval, guint,
                                GdkModifierType mods, gpointer selfp) {
    SearchPanel* self = static_cast<SearchPanel*>(selfp);
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

void SearchPanel::onSetup(GtkSignalListItemFactory*, GObject* obj, gpointer) {
    GtkWidget* label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_use_markup(GTK_LABEL(label), TRUE);
    gtk_widget_add_css_class(label, "monospace");
    gtk_widget_set_margin_top(label, 2);
    gtk_widget_set_margin_bottom(label, 2);
    gtk_list_item_set_child(GTK_LIST_ITEM(obj), label);
    gtk_list_item_set_activatable(GTK_LIST_ITEM(obj), FALSE);   // clicks are ours
    g_object_set_data(G_OBJECT(label), "minicode-list-item", obj);
}

void SearchPanel::onBind(GtkSignalListItemFactory*, GObject* obj, gpointer) {
    GtkListItem* li = GTK_LIST_ITEM(obj);
    GtkStringObject* s = GTK_STRING_OBJECT(gtk_list_item_get_item(li));
    GtkWidget* label = gtk_list_item_get_child(li);
    gtk_label_set_markup(GTK_LABEL(label), s ? gtk_string_object_get_string(s) : "");
}
