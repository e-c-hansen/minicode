// Search.cpp — see Search.h. The search itself is FolderSearch::search in the
// shared core; this file is the window, the worker thread and the list.
#include "Search.h"

#include <utility>

// What one search carries into its worker thread and back. The GTask owns it
// (task data), so it is freed once the result has been handled or dropped.
struct SearchPanel::Job {
    unsigned gen = 0;
    std::string root;
    std::string query;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::weak_ptr<int> alive;   // the panel's life_
    FolderSearchResult result;
};

// The location half of a row, the same blue as the Markdown link color.
static const char* kLocationColor = "#4EA1F7";

SearchPanel::SearchPanel(GtkWindow* parent) : parent_(parent) { build(); }

SearchPanel::~SearchPanel() {
    life_.reset();
    if (cancel_) cancel_->store(true);
    if (window_) {
        g_object_remove_weak_pointer(G_OBJECT(window_), reinterpret_cast<gpointer*>(&window_));
        gtk_window_destroy(GTK_WINDOW(window_));
    }
}

void SearchPanel::build() {
    window_ = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window_), "Find in Folder");
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
        "The folder to search. Type a path and press Enter; a relative path is "
        "taken from the open folder.");
    g_signal_connect_swapped(scopeEntry_, "activate",
        G_CALLBACK(+[](SearchPanel* self) { self->scopeEntered(); }), this);
    GtkWidget* choose = gtk_button_new_with_label("Choose…");
    g_signal_connect_swapped(choose, "clicked",
        G_CALLBACK(+[](SearchPanel* self) { self->chooseScope(); }), this);
    gtk_box_append(GTK_BOX(scopeRow), label);
    gtk_box_append(GTK_BOX(scopeRow), scopeEntry_);
    gtk_box_append(GTK_BOX(scopeRow), choose);

    // --- the query ---
    entry_ = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(entry_),
                                          "Search text in this folder…");
    // Search as the user types, after the same pause the Mac waits (0.35 s),
    // so a full folder scan does not start on every keystroke.
    gtk_search_entry_set_search_delay(GTK_SEARCH_ENTRY(entry_), 350);
    g_signal_connect(entry_, "search-changed", G_CALLBACK(onSearchChanged), this);
    g_signal_connect(entry_, "activate", G_CALLBACK(onQueryActivate), this);
    g_signal_connect(entry_, "stop-search", G_CALLBACK(onStopSearch), this);
    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(onQueryKey), this);
    gtk_widget_add_controller(entry_, keys);

    status_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(status_), 0.0);
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
    gtk_box_append(GTK_BOX(box), entry_);
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
    gtk_file_dialog_select_folder(dlg, GTK_WINDOW(window_), nullptr,
        [](GObject* src, GAsyncResult* res, gpointer selfp) {
            GFile* folder = gtk_file_dialog_select_folder_finish(
                GTK_FILE_DIALOG(src), res, nullptr);
            if (!folder) return;   // cancelled
            char* path = g_file_get_path(folder);
            if (path) static_cast<SearchPanel*>(selfp)->setScope(path);
            g_free(path);
            g_object_unref(folder);
        }, this);
    g_object_unref(dlg);
}

// ------------------------------------------------------------------ show

void SearchPanel::show() {
    gtk_window_present(GTK_WINDOW(window_));
    gtk_widget_grab_focus(entry_);
    gtk_editable_select_region(GTK_EDITABLE(entry_), 0, -1);
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

// ------------------------------------------------------------------ search

void SearchPanel::runSearch() {
    // Cancel whatever is in flight first: its result will carry an old
    // generation and be dropped, and its flag stops it scanning.
    ++generation_;
    if (cancel_) cancel_->store(true);
    cancel_.reset();

    const std::string query =
        FolderSearch::normalizeQuery(gtk_editable_get_text(GTK_EDITABLE(entry_)));
    lastQuery_ = query;
    clearResults();
    if (!FolderSearch::isSearchable(query) || scope_.empty()) {
        searching_ = false;
        setStatus(query.empty() ? "" : "Type at least 2 characters");
        return;
    }

    searching_ = true;
    setStatus("Searching…");
    auto* job = new Job;
    job->gen = generation_;
    job->root = scope_;
    job->query = query;
    job->cancel = std::make_shared<std::atomic<bool>>(false);
    job->alive = life_;
    cancel_ = job->cancel;

    GTask* task = g_task_new(nullptr, nullptr, onDone, this);
    g_task_set_task_data(task, job, [](gpointer p) { delete static_cast<Job*>(p); });
    g_task_run_in_thread(task, worker);
    g_object_unref(task);   // the running task holds its own reference
}

// Runs on a GLib worker thread. Touches nothing but the job.
void SearchPanel::worker(GTask* task, gpointer, gpointer data, GCancellable*) {
    Job* job = static_cast<Job*>(data);
    job->result = FolderSearch::search(job->root, job->query, job->cancel.get());
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

    hits_ = std::move(job->result.matches);
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

    const std::size_t n = hits_.size(), files = job->result.filesMatched;
    std::string status = std::to_string(n) + (n == 1 ? " match in " : " matches in ") +
                         std::to_string(files) + (files == 1 ? " file" : " files");
    if (job->result.truncated) status += " (stopped at the first " + std::to_string(n) + ")";
    setStatus(status);
}

// ------------------------------------------------------------------ signals

void SearchPanel::onSearchChanged(GtkSearchEntry*, gpointer selfp) {
    static_cast<SearchPanel*>(selfp)->runSearch();
}

// Enter searches at once, unless the list already answers this query, in
// which case it moves into the list (Enter again opens the match).
void SearchPanel::onQueryActivate(GtkSearchEntry* e, gpointer selfp) {
    SearchPanel* self = static_cast<SearchPanel*>(selfp);
    const std::string q =
        FolderSearch::normalizeQuery(gtk_editable_get_text(GTK_EDITABLE(e)));
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

void SearchPanel::openRow(guint pos) {
    if (pos >= hits_.size() || !openCb_) return;
    // A copy, because the callback may come back into the panel (a new
    // search replaces hits_).
    const FolderSearchMatch m = hits_[pos];
    openCb_(m, openUser_);
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

void SearchPanel::onRowPressed(GtkGestureClick*, int, double x, double y, gpointer selfp) {
    SearchPanel* self = static_cast<SearchPanel*>(selfp);
    self->pressRow_ = self->rowAt(x, y);
}

// The first click of a double-click opens the match; the second does
// nothing. A press released over another row does nothing either.
void SearchPanel::onRowReleased(GtkGestureClick*, int n, double x, double y, gpointer selfp) {
    SearchPanel* self = static_cast<SearchPanel*>(selfp);
    const guint pressed = self->pressRow_;
    self->pressRow_ = GTK_INVALID_LIST_POSITION;
    if (n == 1 && pressed != GTK_INVALID_LIST_POSITION && self->rowAt(x, y) == pressed)
        self->openRow(pressed);
}

// Enter on a result opens it.
gboolean SearchPanel::onListKey(GtkEventControllerKey*, guint keyval, guint,
                                GdkModifierType mods, gpointer selfp) {
    SearchPanel* self = static_cast<SearchPanel*>(selfp);
    if (keyval != GDK_KEY_Return && keyval != GDK_KEY_KP_Enter && keyval != GDK_KEY_ISO_Enter)
        return FALSE;
    if (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK)) return FALSE;
    GtkSelectionModel* model = gtk_list_view_get_model(GTK_LIST_VIEW(self->listView_));
    const guint pos = gtk_single_selection_get_selected(GTK_SINGLE_SELECTION(model));
    if (pos == GTK_INVALID_LIST_POSITION) return FALSE;
    self->openRow(pos);
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
