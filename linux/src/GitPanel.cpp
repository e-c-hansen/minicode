// GitPanel.cpp — see GitPanel.h. The Mac's src/GitPanel.mm, in GTK.
#include "GitPanel.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <mutex>

#define GIT_LOG_DOMAIN "minicode-git"

namespace {

// ------------------------------------------------------------ running git
struct GitResult {
    int status = -1;
    std::string out, err;
    bool ok() const { return status == 0; }
};

std::string bytesOf(GBytes* b) {
    if (!b) return std::string();
    gsize n = 0;
    const char* p = static_cast<const char*>(g_bytes_get_data(b, &n));
    return std::string(p ? p : "", n);
}

// Runs one git, blocking; only ever called on the panel's git thread. An
// argument vector, never a shell; stdin is /dev/null (GSubprocess's
// default), and stdout and stderr are read together, so a chatty stderr
// cannot fill its pipe and stall git.
GitResult runGit(const std::string& dir, const std::vector<std::string>& args) {
    GitResult r;
    const std::string& git = gitExecutable();
    if (git.empty()) {
        r.err = "Git was not found. Install it with your package manager, "
                "for example sudo apt install git.";
        return r;
    }
    GSubprocessLauncher* l = g_subprocess_launcher_new(
        (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE));
    for (const GitUi::EnvVar& e : GitUi::gitEnvironment())
        g_subprocess_launcher_setenv(l, e.name, e.value, TRUE);
    g_subprocess_launcher_set_cwd(l, dir.c_str());
    std::vector<const char*> argv;
    argv.push_back(git.c_str());
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    GError* err = nullptr;
    GSubprocess* p = g_subprocess_launcher_spawnv(l, argv.data(), &err);
    g_object_unref(l);
    if (!p) {
        r.err = err ? err->message : "Git could not be started.";
        g_clear_error(&err);
        return r;
    }
    GBytes* out = nullptr;
    GBytes* errb = nullptr;
    if (!g_subprocess_communicate(p, nullptr, nullptr, &out, &errb, &err)) {
        r.err = err ? err->message : "Git could not be read.";
        g_clear_error(&err);
    } else {
        r.out = bytesOf(out);
        r.err = bytesOf(errb);
        if (g_subprocess_get_if_exited(p)) r.status = g_subprocess_get_exit_status(p);
    }
    if (out) g_bytes_unref(out);
    if (errb) g_bytes_unref(errb);
    g_object_unref(p);
    return r;
}

std::string failure(const GitResult& r) {
    return GitUi::failureText(r.status, r.out, r.err);
}

// What the list shows about the folder, read off the main thread.
GitUi::Snapshot snapshotAt(const std::string& root) {
    GitUi::Snapshot s;
    if (gitExecutable().empty()) {
        s.notice = "Git was not found. Install it with your package manager, "
                   "for example sudo apt install git.";
        return s;
    }
    GitResult top = runGit(root, GitUi::topLevelArgs());
    if (!top.ok()) {
        if (top.err.find("not a git repository") != std::string::npos)
            s.notice = "This folder is not in a git repository.";
        else
            s.errorText = failure(top);
        return s;
    }
    if (!GitUi::parseTopLevel(top.out, s.topLevel, s.gitDir)) {
        s.errorText = failure(top);
        return s;
    }
    s.inRepository = true;
    GitResult st = runGit(s.topLevel, GitUi::statusArgs());
    if (!st.ok()) {
        s.errorText = failure(st);
        return s;
    }
    GitUi::applyStatus(s, Git::parseStatus(st.out));
    return s;
}

// The graph, or the previous one when nothing it depends on changed (most
// refreshes come from saving a file, and then this costs one for-each-ref).
std::shared_ptr<const GitUi::Graph> graphFor(const GitUi::Snapshot& s, int limit, bool all,
                                             std::shared_ptr<const GitUi::Graph> previous) {
    const bool compare = GitUi::compareWithUpstream(s);
    GitResult refs = runGit(s.topLevel, GitUi::refArgs());
    std::string key = GitUi::graphKey(s, limit, all, compare, refs.out);
    if (previous && previous->key == key) return previous;
    auto g = std::make_shared<GitUi::Graph>();
    g->key = key;
    g->limit = limit;
    GitResult log = runGit(s.topLevel, GitUi::logArgs(limit, all, compare));
    if (!log.ok()) {
        g->errorText = failure(log);
        return g;
    }
    GitUi::buildGraph(*g, s, log.out, refs.out, limit);
    if (compare && (s.ahead || s.behind)) {
        GitResult lr = runGit(s.topLevel, GitUi::leftRightArgs());
        if (lr.ok()) g->divergence = Git::parseLeftRight(lr.out);
    }
    return g;
}

std::string baseName(const std::string& p) {
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string dirName(const std::string& p) {
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

// ------------------------------------------------------------ drawing
void setColor(cairo_t* cr, unsigned rgb, double alpha = 1.0) {
    cairo_set_source_rgba(cr, ((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0,
                          (rgb & 0xFF) / 255.0, alpha);
}

void setColor(cairo_t* cr, const Rgba& c) {
    cairo_set_source_rgba(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a);
}

// A layout in the widget's own font at `px` pixels, `weight`, cut to
// `maxW` pixels with an ellipsis at `mode` (maxW <= 0 means no limit).
PangoLayout* makeLayout(GtkWidget* w, const std::string& text, double px, PangoWeight weight,
                        double maxW, PangoEllipsizeMode mode) {
    PangoLayout* l = gtk_widget_create_pango_layout(w, nullptr);
    PangoFontDescription* fd = pango_font_description_copy(
        pango_context_get_font_description(pango_layout_get_context(l)));
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    pango_font_description_set_weight(fd, weight);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l, text.c_str(), (int)text.size());
    pango_layout_set_single_paragraph_mode(l, TRUE);
    if (maxW > 0) {
        pango_layout_set_width(l, (int)(maxW * PANGO_SCALE));
        pango_layout_set_ellipsize(l, mode);
    }
    return l;
}

// Draws `l` with its middle at `midY`; returns its width.
double showCentered(cairo_t* cr, PangoLayout* l, double x, double midY) {
    int w = 0, h = 0;
    pango_layout_get_pixel_size(l, &w, &h);
    cairo_move_to(cr, x, midY - h / 2.0);
    pango_cairo_show_layout(cr, l);
    return w;
}

void roundedRect(cairo_t* cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

// A label: filled for HEAD's own branch (or a detached HEAD), outlined for
// the rest, in its kind's color. Returns its width, or 0 when it did not fit.
double drawPill(cairo_t* cr, GtkWidget* w, const Git::Ref& r, double x, double midY,
                double maxW) {
    const unsigned c = GitUi::pillColor(r.kind);
    std::string name = GitUi::validUtf8(r.name);
    if (r.kind == Git::RefKind::Tag) name = "tag " + name;
    PangoLayout* l = makeLayout(w, name, 10, PANGO_WEIGHT_SEMIBOLD, 0, PANGO_ELLIPSIZE_NONE);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(l, &tw, &th);
    double textW = std::min<double>(tw, std::max(0.0, maxW - 10));
    if (textW < 12) { g_object_unref(l); return 0; }
    if (textW < tw) {
        pango_layout_set_width(l, (int)(textW * PANGO_SCALE));
        pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_MIDDLE);
    }
    const double bw = textW + 10, bh = 14, by = midY - 7;
    roundedRect(cr, x + 0.5, by + 0.5, bw - 1, bh - 1, 6.5);
    if (r.current) {
        setColor(cr, c);
        cairo_fill(cr);
        setColor(cr, 0x1E1E1E);
    } else {
        setColor(cr, c, 0.14);
        cairo_fill_preserve(cr);
        setColor(cr, c, 0.7);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        setColor(cr, c);
    }
    showCentered(cr, l, x + 5, midY);
    g_object_unref(l);
    return bw;
}

// The row a point in a list view is on, through the drawing area that
// carries its GtkListItem; -1 when on none.
int rowAt(GtkWidget* list, double x, double y) {
    for (GtkWidget* w = gtk_widget_pick(list, x, y, GTK_PICK_DEFAULT); w && w != list;
         w = gtk_widget_get_parent(w)) {
        if (auto* li = static_cast<GtkListItem*>(
                g_object_get_data(G_OBJECT(w), "minicode-list-item")))
            return (int)gtk_list_item_get_position(li);
    }
    return -1;
}

int positionOf(GtkDrawingArea* da) {
    auto* li = static_cast<GtkListItem*>(g_object_get_data(G_OBJECT(da), "minicode-list-item"));
    if (!li) return -1;
    guint p = gtk_list_item_get_position(li);
    return p == GTK_INVALID_LIST_POSITION ? -1 : (int)p;
}

// A string list of `n` empty items: the lists' models. Rows are drawn from
// the panel's own vectors by position.
void resetModel(GtkStringList* model, guint n) {
    std::vector<const char*> items(n, "");
    items.push_back(nullptr);
    gtk_string_list_splice(model, 0, g_list_model_get_n_items(G_LIST_MODEL(model)),
                           items.data());
}

bool isEnter(guint key) {
    return key == GDK_KEY_Return || key == GDK_KEY_KP_Enter || key == GDK_KEY_ISO_Enter;
}

// Keys from the panel's own debug log (G_MESSAGES_DEBUG=minicode-git).
void debugLines(const char* what, const std::vector<std::string>& lines) {
    g_log(GIT_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "%s (%zu)", what, lines.size());
    const size_t n = std::min<size_t>(lines.size(), 40);
    for (size_t i = 0; i < n; ++i)
        g_log(GIT_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "  %s", lines[i].c_str());
}

}  // namespace

const std::string& gitExecutable() {
    static std::string git;
    static std::once_flag once;
    std::call_once(once, [] {
        if (char* p = g_find_program_in_path("git")) {
            git = p;
            g_free(p);
        }
    });
    return git;
}

// ================================================================ diff view
GitDiffView::GitDiffView() {
    buffer_ = gtk_text_buffer_new(nullptr);
    auto tag = [&](const char* name, const char* color, bool bold) {
        GtkTextTag* t = gtk_text_buffer_create_tag(buffer_, name, nullptr);
        if (color) g_object_set(t, "foreground", color, nullptr);
        if (bold) g_object_set(t, "weight", PANGO_WEIGHT_BOLD, nullptr);
    };
    tag("added", "#73C991", false);
    tag("removed", "#F14C4C", false);
    tag("muted", "#9CA3AF", false);
    tag("fileheader", "#9CA3AF", true);
    tag("hash", "#E2C08D", false);
    tag("bold", nullptr, true);
    view_ = gtk_text_view_new_with_buffer(buffer_);
    g_object_unref(buffer_);   // the view holds it
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view_), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view_), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view_), TRUE);
    // Long lines wrap, as on the Mac.
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view_), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view_), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view_), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view_), 8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view_), 8);
    // The editor's own colors: its background and text come from the
    // settings through the stylesheet, so a settings change recolors this too.
    gtk_widget_add_css_class(view_, "minicode-editor");
    scroller_ = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroller_, "minicode-scroller");
    gtk_widget_add_css_class(scroller_, "minicode-editor-scroller");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), view_);
    gtk_widget_set_hexpand(scroller_, TRUE);
    gtk_widget_set_vexpand(scroller_, TRUE);
}

void GitDiffView::show(const std::string& bytes, bool commit) {
    const GitUi::StyledText st =
        commit ? GitUi::commitText(bytes, (long long)time(nullptr)) : GitUi::diffText(bytes);
    gtk_text_buffer_set_text(buffer_, st.text.c_str(), (int)st.text.size());
    for (const GitUi::StyledText::Run& r : st.runs) {
        const char* name = nullptr;
        switch (r.style) {
        case GitUi::Style::Added: name = "added"; break;
        case GitUi::Style::Removed: name = "removed"; break;
        case GitUi::Style::Muted: name = "muted"; break;
        case GitUi::Style::FileHeader: name = "fileheader"; break;
        case GitUi::Style::Hash: name = "hash"; break;
        case GitUi::Style::Bold: name = "bold"; break;
        case GitUi::Style::Plain: break;
        }
        if (!name) continue;
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)r.start);
        gtk_text_buffer_get_iter_at_offset(buffer_, &b, (int)(r.start + r.length));
        gtk_text_buffer_apply_tag_by_name(buffer_, name, &a, &b);
    }
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buffer_, &start);
    gtk_text_buffer_place_cursor(buffer_, &start);
    gtk_adjustment_set_value(
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_)), 0);
}

// ==================================================================== panel
namespace {
struct Delivery {
    std::shared_ptr<bool> alive;
    GitPanel* panel;
    std::function<void(GitPanel*)> fn;
};

// From the git thread to the main loop, and only if the panel still exists.
void deliver(std::shared_ptr<bool> alive, GitPanel* panel, std::function<void(GitPanel*)> fn) {
    auto* d = new Delivery{std::move(alive), panel, std::move(fn)};
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        auto* d = static_cast<Delivery*>(p);
        if (*d->alive) d->fn(d->panel);
        delete d;
        return G_SOURCE_REMOVE;
    }, d, nullptr);
}

void runJob(gpointer data, gpointer) {
    auto* job = static_cast<std::function<void()>*>(data);
    (*job)();
    delete job;
}

// The panel's children are placed by hand (GitPanel::layout), as the Mac's
// layoutPanel does: the change lists take what their rows need, up to about
// half of what is left, and the graph the rest.
void layoutAllocate(GtkWidget* w, int width, int height, int) {
    if (auto* p = static_cast<GitPanel*>(g_object_get_data(G_OBJECT(w), "minicode-git")))
        p->layout(width, height);
}

void layoutMeasure(GtkWidget*, GtkOrientation o, int, int* min, int* nat, int* minB, int* natB) {
    *min = o == GTK_ORIENTATION_HORIZONTAL ? 120 : 100;
    *nat = o == GTK_ORIENTATION_HORIZONTAL ? 240 : 400;
    *minB = *natB = -1;
}
}  // namespace

GitPanel::GitPanel(const std::string& root) : rootDir_(root) {
    alive_ = std::make_shared<bool>(true);
    ref_ = std::make_shared<GitPanel*>(this);
    // One thread: git runs one command at a time, in the order asked.
    pool_ = g_thread_pool_new(runJob, nullptr, 1, FALSE, nullptr);
    build();
}

GitPanel::~GitPanel() {
    *alive_ = false;
    *ref_ = nullptr;   // rows drawn after this draw nothing
    g_signal_handlers_disconnect_by_data(gtk_text_view_get_buffer(GTK_TEXT_VIEW(message_)), this);
    for (GtkWidget* v : {list_, graphList_})
        if (GtkListItemFactory* f = gtk_list_view_get_factory(GTK_LIST_VIEW(v)))
            g_signal_handlers_disconnect_by_data(f, this);
    if (refreshTimer_) g_source_remove(refreshTimer_);
    refreshTimer_ = 0;
    unwatch();
    g_object_set_data(G_OBJECT(root_), "minicode-git", nullptr);
    for (GtkWidget* w : {list_, graphList_, message_, commitButton_, allToggle_}) {
        g_signal_handlers_disconnect_by_data(w, this);
        GListModel* ctrls = gtk_widget_observe_controllers(w);
        for (guint i = 0; i < g_list_model_get_n_items(ctrls); ++i) {
            GObject* c = static_cast<GObject*>(g_list_model_get_item(ctrls, i));
            g_signal_handlers_disconnect_by_data(c, this);
            g_object_unref(c);
        }
        g_object_unref(ctrls);
    }
    // Git already running finishes on the pool's thread; what it delivers
    // finds alive_ false and is dropped. The pool frees itself after.
    g_thread_pool_free(pool_, FALSE, FALSE);
    g_object_unref(root_);
}

void GitPanel::post(std::function<void()> job) {
    g_thread_pool_push(pool_, new std::function<void()>(std::move(job)), nullptr);
}

// ---------------------------------------------------------------- building
void GitPanel::build() {
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_ref_sink(root_);   // kept until the destructor, whoever shows it
    gtk_widget_add_css_class(root_, "minicode-git");
    gtk_widget_set_layout_manager(root_, gtk_custom_layout_new(
        [](GtkWidget*) { return GTK_SIZE_REQUEST_CONSTANT_SIZE; }, layoutMeasure,
        layoutAllocate));
    g_object_set_data(G_OBJECT(root_), "minicode-git", this);
    gtk_widget_set_hexpand(root_, TRUE);
    gtk_widget_set_vexpand(root_, TRUE);

    branchLabel_ = gtk_label_new("Source Control");
    gtk_label_set_xalign(GTK_LABEL(branchLabel_), 0);
    gtk_label_set_ellipsize(GTK_LABEL(branchLabel_), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(branchLabel_, "minicode-git-branch");
    gtk_box_append(GTK_BOX(root_), branchLabel_);

    // The message box: Ctrl+Return commits, Tab goes back to the list, and a
    // muted hint shows while it is empty.
    message_ = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(message_), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(message_), 5);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(message_), 5);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(message_), 4);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(message_), 4);
    gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(message_), FALSE);
    GtkWidget* msgScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(msgScroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(msgScroll), message_);
    gtk_widget_add_css_class(msgScroll, "minicode-scroller");
    placeholder_ = gtk_label_new("Message (Ctrl+Return to commit)");
    gtk_label_set_xalign(GTK_LABEL(placeholder_), 0);
    gtk_label_set_ellipsize(GTK_LABEL(placeholder_), PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(placeholder_, GTK_ALIGN_FILL);
    gtk_widget_set_valign(placeholder_, GTK_ALIGN_START);
    gtk_widget_set_margin_start(placeholder_, 6);
    gtk_widget_set_margin_top(placeholder_, 4);
    gtk_widget_set_can_target(placeholder_, FALSE);
    gtk_widget_add_css_class(placeholder_, "minicode-git-placeholder");
    messageFrame_ = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(messageFrame_), msgScroll);
    gtk_overlay_add_overlay(GTK_OVERLAY(messageFrame_), placeholder_);
    gtk_widget_set_overflow(messageFrame_, GTK_OVERFLOW_HIDDEN);
    gtk_widget_add_css_class(messageFrame_, "minicode-git-message");
    gtk_box_append(GTK_BOX(root_), messageFrame_);
    g_signal_connect_swapped(gtk_text_view_get_buffer(GTK_TEXT_VIEW(message_)), "changed",
        G_CALLBACK(+[](GitPanel* self) {
            GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->message_));
            gtk_widget_set_visible(self->placeholder_, gtk_text_buffer_get_char_count(b) == 0);
        }), this);
    GtkEventController* mkeys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(mkeys, GTK_PHASE_CAPTURE);
    g_signal_connect(mkeys, "key-pressed", G_CALLBACK(onMessageKey), this);
    gtk_widget_add_controller(message_, mkeys);

    commitButton_ = gtk_button_new_with_label("Commit");
    gtk_widget_add_css_class(commitButton_, "minicode-git-commit");
    g_signal_connect_swapped(commitButton_, "clicked", G_CALLBACK(+[](GitPanel* self) {
        self->commit();
    }), this);
    gtk_box_append(GTK_BOX(root_), commitButton_);

    errorLabel_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(errorLabel_), 0);
    gtk_label_set_wrap(GTK_LABEL(errorLabel_), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(errorLabel_), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(errorLabel_), 6);   // the whole text in the tooltip
    gtk_label_set_ellipsize(GTK_LABEL(errorLabel_), PANGO_ELLIPSIZE_END);
    gtk_label_set_selectable(GTK_LABEL(errorLabel_), TRUE);
    gtk_widget_set_focusable(errorLabel_, FALSE);
    gtk_widget_add_css_class(errorLabel_, "minicode-git-error");
    gtk_widget_set_visible(errorLabel_, FALSE);
    gtk_box_append(GTK_BOX(root_), errorLabel_);

    notice_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(notice_), 0);
    gtk_label_set_wrap(GTK_LABEL(notice_), TRUE);
    gtk_widget_add_css_class(notice_, "minicode-git-notice");
    gtk_widget_set_visible(notice_, FALSE);

    // The change lists and the graph: list views of hand-drawn rows, the way
    // the Mac's are table views of hand-drawn cells. The models hold nothing
    // but a count; the rows are drawn from rows_ and graph_ by position.
    auto makeList = [&](GtkStringList** model, GtkSingleSelection** sel,
                        GCallback setup, GCallback bind) {
        *model = gtk_string_list_new(nullptr);
        *sel = gtk_single_selection_new(G_LIST_MODEL(*model));
        gtk_single_selection_set_autoselect(*sel, FALSE);
        gtk_single_selection_set_can_unselect(*sel, TRUE);
        GtkListItemFactory* f = gtk_signal_list_item_factory_new();
        g_signal_connect(f, "setup", setup, this);
        g_signal_connect(f, "bind", bind, this);
        GtkWidget* view = gtk_list_view_new(GTK_SELECTION_MODEL(*sel), f);
        gtk_widget_add_css_class(view, "minicode-git-list");
        return view;
    };

    list_ = makeList(&listModel_, &listSel_,
        G_CALLBACK(+[](GtkSignalListItemFactory*, GObject* obj, gpointer selfp) {
            GtkListItem* item = GTK_LIST_ITEM(obj);
            GtkWidget* da = gtk_drawing_area_new();
            gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(da), 22);
            gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), drawRow,
                new std::shared_ptr<GitPanel*>(static_cast<GitPanel*>(selfp)->ref_),
                [](gpointer h) { delete static_cast<std::shared_ptr<GitPanel*>*>(h); });
            g_object_set_data(G_OBJECT(da), "minicode-list-item", item);
            gtk_list_item_set_activatable(item, FALSE);
            gtk_list_item_set_child(item, da);
        }),
        G_CALLBACK(+[](GtkSignalListItemFactory*, GObject* obj, gpointer selfp) {
            GitPanel* self = static_cast<GitPanel*>(selfp);
            GtkListItem* item = GTK_LIST_ITEM(obj);
            GtkWidget* da = gtk_list_item_get_child(item);
            const guint pos = gtk_list_item_get_position(item);
            const bool header = pos < self->rows_.size() && self->rows_[pos].header;
            gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(da), header ? 26 : 22);
            gtk_list_item_set_selectable(item, !header);
            gtk_list_item_set_focusable(item, !header);
            const std::string tip = pos < self->rows_.size()
                                        ? GitUi::validUtf8(GitUi::rowToolTip(self->rows_[pos]))
                                        : std::string();
            gtk_widget_set_tooltip_text(da, tip.empty() ? nullptr : tip.c_str());
            gtk_widget_queue_draw(da);
        }));
    listScroll_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(listScroll_), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_add_css_class(listScroll_, "minicode-scroller");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(listScroll_), list_);
    gtk_box_append(GTK_BOX(root_), listScroll_);
    gtk_box_append(GTK_BOX(root_), notice_);   // over the list's empty space

    // The graph: a rule, a heading with the "All branches" switch, one line
    // on what is pushed, then the commits.
    graphLine_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(graphLine_, "minicode-git-rule");
    gtk_box_append(GTK_BOX(root_), graphLine_);
    graphHeading_ = gtk_label_new("GRAPH");
    gtk_label_set_xalign(GTK_LABEL(graphHeading_), 0);
    gtk_widget_add_css_class(graphHeading_, "minicode-git-heading");
    gtk_box_append(GTK_BOX(root_), graphHeading_);
    allToggle_ = gtk_check_button_new_with_label("All branches");
    gtk_widget_set_tooltip_text(allToggle_, "Show every local and remote branch, not only "
                                            "this branch and its upstream");
    gtk_widget_add_css_class(allToggle_, "minicode-git-toggle");
    g_signal_connect_swapped(allToggle_, "toggled", G_CALLBACK(+[](GitPanel* self) {
        const bool all = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->allToggle_));
        if (all == self->allBranches_) return;
        self->allBranches_ = all;
        self->graphLimit_ = GitUi::kGraphBatch;
        self->runRefresh();
    }), this);
    gtk_box_append(GTK_BOX(root_), allToggle_);
    summaryLabel_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(summaryLabel_), 0);
    gtk_label_set_ellipsize(GTK_LABEL(summaryLabel_), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(summaryLabel_, "minicode-git-summary");
    gtk_box_append(GTK_BOX(root_), summaryLabel_);

    graphList_ = makeList(&graphModel_, &graphSel_,
        G_CALLBACK(+[](GtkSignalListItemFactory*, GObject* obj, gpointer selfp) {
            GtkListItem* item = GTK_LIST_ITEM(obj);
            GtkWidget* da = gtk_drawing_area_new();
            gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(da), 22);
            gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), drawGraphRow,
                new std::shared_ptr<GitPanel*>(static_cast<GitPanel*>(selfp)->ref_),
                [](gpointer h) { delete static_cast<std::shared_ptr<GitPanel*>*>(h); });
            g_object_set_data(G_OBJECT(da), "minicode-list-item", item);
            gtk_list_item_set_activatable(item, FALSE);
            gtk_list_item_set_child(item, da);
        }),
        G_CALLBACK(+[](GtkSignalListItemFactory*, GObject* obj, gpointer selfp) {
            GitPanel* self = static_cast<GitPanel*>(selfp);
            GtkListItem* item = GTK_LIST_ITEM(obj);
            GtkWidget* da = gtk_list_item_get_child(item);
            const guint pos = gtk_list_item_get_position(item);
            std::string tip;
            if (self->graph_) tip = GitUi::graphToolTip(*self->graph_, pos, (long long)time(nullptr));
            gtk_widget_set_tooltip_text(da, tip.empty() ? nullptr : tip.c_str());
            gtk_widget_queue_draw(da);
        }));
    graphScroll_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(graphScroll_), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_add_css_class(graphScroll_, "minicode-scroller");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(graphScroll_), graphList_);
    gtk_box_append(GTK_BOX(root_), graphScroll_);

    // Keys: capture phase, so they arrive before the lists' own bindings.
    GtkEventController* lkeys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(lkeys, GTK_PHASE_CAPTURE);
    g_signal_connect(lkeys, "key-pressed", G_CALLBACK(onListKey), this);
    gtk_widget_add_controller(list_, lkeys);
    GtkEventController* gkeys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(gkeys, GTK_PHASE_CAPTURE);
    g_signal_connect(gkeys, "key-pressed", G_CALLBACK(onGraphKey), this);
    gtk_widget_add_controller(graphList_, gkeys);

    // A click shows the diff or the commit, as the Mac's mouseDown does; the
    // rows' own handling (bubble phase, so first) has selected it by then.
    // Only a single click, released on the row it was pressed on.
    for (GtkWidget* view : {list_, graphList_}) {
        GtkGesture* click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
        g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick* g, int, double x,
                                                          double y, gpointer selfp) {
            GtkWidget* v = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
            static_cast<GitPanel*>(selfp)->pressRow_ = rowAt(v, x, y);
        }), this);
        g_signal_connect(click, "released", G_CALLBACK(+[](GtkGestureClick* g, int n, double x,
                                                           double y, gpointer selfp) {
            GitPanel* self = static_cast<GitPanel*>(selfp);
            GtkWidget* v = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
            const int pressed = self->pressRow_;
            self->pressRow_ = -1;
            if (n != 1 || pressed < 0 || rowAt(v, x, y) != pressed) return;
            if (v == self->list_) {
                if (pressed >= (int)self->rows_.size() || self->rows_[pressed].header) return;
                self->selectRow(pressed, true);
                self->showDiff(pressed);
            } else {
                self->selectGraphRow(pressed, true);
                self->showCommit(pressed);
            }
        }), this);
        gtk_widget_add_controller(view, GTK_EVENT_CONTROLLER(click));
    }

    showGraph(false);
    showMessageArea(false);
}

void GitPanel::applySettings(const Settings& s) {
    text_ = s.text(Surface::Sidebar);
    panelBg_ = s.background(Surface::Sidebar);
    gtk_widget_queue_draw(list_);
    gtk_widget_queue_draw(graphList_);
    for (GtkWidget* c = gtk_widget_get_first_child(list_); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* d = gtk_widget_get_first_child(c)) gtk_widget_queue_draw(d);
    for (GtkWidget* c = gtk_widget_get_first_child(graphList_); c;
         c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* d = gtk_widget_get_first_child(c)) gtk_widget_queue_draw(d);
}

void GitPanel::showGraph(bool show) {
    graphShown_ = show;
    for (GtkWidget* w : {graphLine_, graphHeading_, allToggle_, summaryLabel_, graphScroll_})
        gtk_widget_set_visible(w, show);
}

void GitPanel::showMessageArea(bool show) {
    gtk_widget_set_visible(messageFrame_, show);
    gtk_widget_set_visible(commitButton_, show);
}

// ------------------------------------------------------------------ layout
void GitPanel::layout(int W, int H) {
    const int pad = 10, inner = std::max(0, W - 2 * pad);
    // Measure, then give the child at least its minimum (GTK requires both).
    auto place = [&](GtkWidget* c, int x, int y, int w, int h) {
        int minW = 0, natW = 0, minH = 0, natH = 0;
        gtk_widget_measure(c, GTK_ORIENTATION_HORIZONTAL, -1, &minW, &natW, nullptr, nullptr);
        w = std::max(w, minW);
        gtk_widget_measure(c, GTK_ORIENTATION_VERTICAL, w, &minH, &natH, nullptr, nullptr);
        h = std::max(h, minH);
        GtkAllocation a = {x, y, w, h};
        gtk_widget_size_allocate(c, &a, -1);
        return h;
    };
    auto natHeight = [&](GtkWidget* c, int w) {
        int minH = 0, natH = 0;
        gtk_widget_measure(c, GTK_ORIENTATION_VERTICAL, w, &minH, &natH, nullptr, nullptr);
        return natH;
    };
    int y = 8;
    y += std::max(18, place(branchLabel_, pad, y, inner, natHeight(branchLabel_, inner))) + 6;
    if (gtk_widget_get_visible(messageFrame_)) {
        place(messageFrame_, pad, y, inner, 60);
        y += 66;
        y += place(commitButton_, pad, y, inner, 28) + 4;
    }
    if (gtk_widget_get_visible(errorLabel_)) {
        const int h = std::min(natHeight(errorLabel_, inner), 120);
        y += place(errorLabel_, pad, y, inner, h) + 6;
    }
    int noticeH = 0;
    if (gtk_widget_get_visible(notice_)) {
        noticeH = natHeight(notice_, inner);
        place(notice_, pad, y + 6, inner, noticeH);
    }
    if (!graphShown_) {
        place(listScroll_, 0, y, W, std::max(0, H - y));
        return;
    }
    int content = 0;
    for (const GitUi::Row& r : rows_) content += r.header ? 26 : 22;
    if (noticeH > 0) content = std::max(content, noticeH + 12);
    const int headH = 46;
    const int listH = std::min(content + 4, (int)std::max(70.0, (H - y - headH) * 0.5));
    place(listScroll_, 0, y, W, std::max(0, listH));
    y += listH + 4;
    place(graphLine_, 0, y, W, 1);
    y += 6;
    int togMin = 0, togNat = 0;
    gtk_widget_measure(allToggle_, GTK_ORIENTATION_HORIZONTAL, -1, &togMin, &togNat, nullptr,
                       nullptr);
    const int togH = natHeight(allToggle_, togNat);
    place(graphHeading_, pad, y + std::max(0, (togH - 14) / 2), std::max(0, inner - togNat - 4),
          14);
    place(allToggle_, W - pad - togNat, y, togNat, togH);
    y += std::max(20, togH);
    y += place(summaryLabel_, pad, y, inner, 15) + 4;
    place(graphScroll_, 0, y, W, std::max(0, H - y));
}

// ----------------------------------------------------------------- refresh
void GitPanel::setRoot(const std::string& root) {
    rootDir_ = root;
    rows_.clear();
    topLevel_.clear();
    inRepository_ = false;
    graph_.reset();
    graphLimit_ = GitUi::kGraphBatch;
    showGraph(false);
    reloadList();
    reloadGraph();
    setError("", false);
    unwatch();
    refresh();
}

void GitPanel::setActive(bool active) {
    active_ = active;
    if (active) refresh();
}

void GitPanel::refresh() {
    if (!active_ || refreshTimer_) return;
    // A short wait gathers a burst of requests (file monitors, focus, an
    // action) into one status.
    refreshTimer_ = g_timeout_add(50, [](gpointer p) -> gboolean {
        GitPanel* self = static_cast<GitPanel*>(p);
        self->refreshTimer_ = 0;
        self->runRefresh();
        return G_SOURCE_REMOVE;
    }, this);
}

// The status first, shown as soon as it is read; then the graph, which on a
// long history takes longer, and which is rebuilt only when HEAD, the refs,
// the limit or the "All branches" switch changed.
void GitPanel::runRefresh() {
    const std::string root = rootDir_;
    if (root.empty()) return;
    const unsigned gen = ++graphGeneration_;
    const int limit = graphLimit_;
    const bool all = allBranches_;
    std::shared_ptr<const GitUi::Graph> previous = graph_;
    std::shared_ptr<bool> alive = alive_;
    GitPanel* self = this;
    post([=] {
        auto snap = std::make_shared<GitUi::Snapshot>(snapshotAt(root));
        deliver(alive, self, [root, snap](GitPanel* p) {
            if (root != p->rootDir_) return;   // the folder changed
            p->applySnapshot(*snap);
        });
        if (!GitUi::graphWanted(*snap)) return;
        std::shared_ptr<const GitUi::Graph> g = graphFor(*snap, limit, all, previous);
        deliver(alive, self, [root, gen, g](GitPanel* p) {
            if (root != p->rootDir_ || gen != p->graphGeneration_) return;
            p->applyGraph(g);
        });
    });
}

void GitPanel::applySnapshot(const GitUi::Snapshot& s) {
    // Keep the selection on the same file, or at the same place in the same
    // list, so Space can be pressed down a list of files.
    const int was = selectedRow();
    const std::vector<GitUi::Row> oldRows = rows_;
    const bool listHadFocus = gtk_widget_has_focus(list_) ||
                              gtk_widget_get_focus_child(list_) != nullptr;

    inRepository_ = s.inRepository;
    topLevel_ = s.topLevel;
    initial_ = s.initial;
    rows_ = s.rows;
    const std::string branch = s.inRepository ? GitUi::validUtf8(s.branchText)
                                              : std::string("Source Control");
    gtk_label_set_text(GTK_LABEL(branchLabel_), branch.c_str());
    showMessageArea(s.inRepository);
    const std::string notice = GitUi::validUtf8(s.notice);
    gtk_label_set_text(GTK_LABEL(notice_), notice.c_str());
    gtk_widget_set_visible(notice_, !notice.empty());
    // A failing status says why until one succeeds; a failed action's
    // message stays until the next action.
    if (!s.errorText.empty()) {
        setError(s.errorText, false);
        errorFromStatus_ = true;
    } else if (errorFromStatus_) {
        setError("", false);
    }
    reloadList();
    const bool graph = GitUi::graphWanted(s);
    showGraph(graph);
    const std::string summary = graph ? GitUi::validUtf8(GitUi::summaryText(s)) : std::string();
    gtk_label_set_text(GTK_LABEL(summaryLabel_), summary.c_str());
    gtk_widget_set_tooltip_text(summaryLabel_, summary.empty() ? nullptr : summary.c_str());
    if (!graph && graph_) {
        graph_.reset();
        reloadGraph();
    }
    const int pick = GitUi::pickAfterRefresh(oldRows, was, rows_);
    selectRow(pick, listHadFocus && pick >= 0);
    if (focusPending_ && GitUi::nextFileRow(rows_, -1, 1) >= 0) {
        focusPending_ = false;
        if (panelHasFocus()) focusList();
    } else if (focusPending_ && !GitUi::graphWanted(s)) {
        focusPending_ = false;   // no graph is coming
        if (panelHasFocus()) focusList();
    }
    gtk_widget_queue_allocate(root_);
    if (s.inRepository) watch(s);

    g_log(GIT_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "status: %s | %s | error: %s | notice: %s",
          branch.c_str(), summary.c_str(), errorText_.c_str(), notice.c_str());
    debugLines("rows", rowDescriptions());
}

void GitPanel::applyGraph(std::shared_ptr<const GitUi::Graph> g) {
    if (g == graph_) return;
    // Keep the selection on the same commit.
    std::string was;
    const int sel = selectedGraphRow();
    if (graph_ && sel >= 0 && sel < (int)graph_->commits.size()) was = graph_->commits[sel].hash;
    const bool wasMoreRow = graph_ && sel == (int)graph_->commits.size();
    const bool hadFocus = gtk_widget_get_focus_child(graphList_) != nullptr ||
                          gtk_widget_has_focus(graphList_);
    graph_ = g;
    reloadGraph();
    int pick = -1;
    for (size_t i = 0; !was.empty() && i < g->commits.size(); ++i)
        if (g->commits[i].hash == was) { pick = (int)i; break; }
    // After "Show more", the first of the new commits.
    if (pick < 0 && wasMoreRow && sel < (int)g->commits.size()) pick = sel;
    selectGraphRow(pick, hadFocus && pick >= 0);
    if (focusPending_) {
        focusPending_ = false;
        if (panelHasFocus()) focusList();   // no changes: the graph
    }
    if (!g->errorText.empty()) setError(g->errorText, false);
    debugLines("graph", graphDescriptions());
}

// Run one git that changes the index or makes a commit, then refresh.
// `done` gets the result on the main loop first.
void GitPanel::runAction(std::vector<std::string> args,
                         std::function<void(int status, const std::string& out)> done) {
    const std::string dir = topLevel_, root = rootDir_;
    if (dir.empty()) return;
    std::shared_ptr<bool> alive = alive_;
    GitPanel* self = this;
    std::string what;
    for (const std::string& a : args) what += (what.empty() ? "" : " ") + a;
    g_log(GIT_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "action: %s", what.c_str());
    post([=] {
        auto r = std::make_shared<GitResult>(runGit(dir, args));
        deliver(alive, self, [root, r, done](GitPanel* p) {
            if (root != p->rootDir_) return;
            p->setError(r->ok() ? std::string() : failure(*r), false);
            if (done) done(r->status, r->out);
            p->runRefresh();
        });
    });
}

void GitPanel::setError(const std::string& text, bool info) {
    errorIsInfo_ = info;
    errorFromStatus_ = false;
    errorText_ = GitUi::validUtf8(text);
    gtk_label_set_text(GTK_LABEL(errorLabel_), errorText_.c_str());
    gtk_widget_set_tooltip_text(errorLabel_, errorText_.empty() ? nullptr : errorText_.c_str());
    gtk_widget_set_visible(errorLabel_, !errorText_.empty());
    if (info) {
        gtk_widget_remove_css_class(errorLabel_, "minicode-git-error");
        gtk_widget_add_css_class(errorLabel_, "minicode-git-info");
    } else {
        gtk_widget_remove_css_class(errorLabel_, "minicode-git-info");
        gtk_widget_add_css_class(errorLabel_, "minicode-git-error");
    }
    gtk_widget_queue_allocate(root_);
    if (!errorText_.empty())
        g_log(GIT_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "message (%s): %s", info ? "info" : "error",
              errorText_.c_str());
}

// ---------------------------------------------------------------- watching
// The repository's top level and its .git directory, while the panel shows:
// staging, commits and checkouts made in the terminal all write in .git.
// Files changed in folders below are reported by the file tree's own
// monitors (main.cpp forwards them), and the window coming to the front
// refreshes too. None of the panel's own git commands write anything: status
// runs with GIT_OPTIONAL_LOCKS=0, so a refresh cannot set off another.
void GitPanel::watch(const GitUi::Snapshot& s) {
    if (s.topLevel == watchedTop_ && s.gitDir == watchedGitDir_) return;
    unwatch();
    watchedTop_ = s.topLevel;
    watchedGitDir_ = s.gitDir;
    std::vector<std::string> dirs = {s.topLevel};
    if (!s.gitDir.empty()) {
        // A branch made or moved writes only under refs/.
        dirs.push_back(s.gitDir);
        dirs.push_back(s.gitDir + "/refs/heads");
        dirs.push_back(s.gitDir + "/refs/tags");
    }
    for (const std::string& dir : dirs) {
        if (dir.empty()) continue;
        GFile* f = g_file_new_for_path(dir.c_str());
        GFileMonitor* m = g_file_monitor_directory(f, G_FILE_MONITOR_WATCH_MOVES, nullptr,
                                                   nullptr);
        g_object_unref(f);
        if (!m) continue;
        g_signal_connect_swapped(m, "changed", G_CALLBACK(+[](GitPanel* self) {
            self->refresh();
        }), this);
        monitors_.push_back(m);
    }
}

void GitPanel::unwatch() {
    for (GFileMonitor* m : monitors_) {
        g_signal_handlers_disconnect_by_data(m, this);
        g_file_monitor_cancel(m);
        g_object_unref(m);
    }
    monitors_.clear();
    watchedTop_.clear();
    watchedGitDir_.clear();
}

// -------------------------------------------------------------- the lists
void GitPanel::reloadList() {
    resetModel(listModel_, (guint)rows_.size());
}

int GitPanel::graphRowCount() const {
    if (!graph_) return 0;
    return (int)graph_->commits.size() + (graph_->hasMore ? 1 : 0);
}

void GitPanel::reloadGraph() {
    resetModel(graphModel_, (guint)graphRowCount());
}

int GitPanel::selectedRow() const {
    guint p = gtk_single_selection_get_selected(listSel_);
    return p == GTK_INVALID_LIST_POSITION ? -1 : (int)p;
}

int GitPanel::selectedGraphRow() const {
    guint p = gtk_single_selection_get_selected(graphSel_);
    return p == GTK_INVALID_LIST_POSITION ? -1 : (int)p;
}

// Selecting without `focus` leaves the keyboard where it is: a refresh must
// never pull it out of the message box.
static void selectIn(GtkWidget* view, GtkSingleSelection* sel, int row, bool focus) {
    if (row < 0) {
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(sel));
        return;
    }
    if (focus) {
        // FOCUS makes the row the list's cursor; the keyboard only moves
        // into the list with the grab.
        gtk_list_view_scroll_to(GTK_LIST_VIEW(view), (guint)row,
            (GtkListScrollFlags)(GTK_LIST_SCROLL_FOCUS | GTK_LIST_SCROLL_SELECT), nullptr);
        gtk_widget_grab_focus(view);
    } else {
        gtk_selection_model_select_item(GTK_SELECTION_MODEL(sel), (guint)row, TRUE);
        gtk_list_view_scroll_to(GTK_LIST_VIEW(view), (guint)row, GTK_LIST_SCROLL_NONE, nullptr);
    }
}

void GitPanel::selectRow(int row, bool focus) { selectIn(list_, listSel_, row, focus); }
void GitPanel::selectGraphRow(int row, bool focus) { selectIn(graphList_, graphSel_, row, focus); }

bool GitPanel::graphHasRows() const { return graphShown_ && graphRowCount() > 0; }

// Focus order: the change lists, the graph, the message box (Tab), and back
// (Shift+Tab). With no changes the list is skipped for the graph.
void GitPanel::focusList() {
    int sel = selectedRow();
    if (sel < 0) sel = GitUi::nextFileRow(rows_, -1, 1);
    if (sel < 0) {
        if (graphHasRows()) focusGraph();
        else if (gtk_widget_get_visible(messageFrame_)) focusMessage();
        else gtk_widget_grab_focus(list_);
        return;
    }
    selectRow(sel, true);
}

// Opening the panel asks for the keyboard before git has answered; it goes
// to the first file, or the graph, once they arrive, if it is still in the
// panel then.
void GitPanel::focus() {
    focusPending_ = rows_.empty() && !graph_;
    focusList();
}

bool GitPanel::panelHasFocus() const {
    GtkRoot* r = gtk_widget_get_root(root_);
    GtkWidget* f = r ? gtk_root_get_focus(r) : nullptr;
    return f && (f == root_ || gtk_widget_is_ancestor(f, root_));
}

void GitPanel::focusMessage() {
    if (!gtk_widget_get_visible(messageFrame_)) return;
    gtk_widget_grab_focus(message_);
}

void GitPanel::focusGraph() {
    if (!graphHasRows()) { focusMessage(); return; }
    int sel = selectedGraphRow();
    selectGraphRow(sel < 0 ? 0 : sel, true);
}

// Up from the graph's first row, or Shift+Tab: the last file in the lists.
// With no changes, Shift+Tab (`wrap`) goes round to the message and Up stays.
void GitPanel::focusListFromGraph(bool wrap) {
    int sel = selectedRow();
    if (sel < 0) sel = GitUi::lastFileRow(rows_);
    if (sel < 0) {
        if (wrap) focusMessage();
        return;
    }
    selectRow(sel, true);
}

void GitPanel::focusMessageOrList() {
    if (gtk_widget_get_visible(messageFrame_)) focusMessage();
    else focusList();
}

gboolean GitPanel::onListKey(GtkEventControllerKey*, guint key, guint, GdkModifierType mods,
                             gpointer selfp) {
    GitPanel* self = static_cast<GitPanel*>(selfp);
    if (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK)) return FALSE;
    const bool shift = mods & GDK_SHIFT_MASK;
    const int sel = self->selectedRow();
    if (isEnter(key) && !shift) { self->showDiff(sel); return TRUE; }
    if (key == GDK_KEY_space && !shift) { self->toggleStage(sel); return TRUE; }
    if (key == GDK_KEY_ISO_Left_Tab || (key == GDK_KEY_Tab && shift)) {
        self->focusMessage();
        return TRUE;
    }
    if (key == GDK_KEY_Tab || key == GDK_KEY_KP_Tab) { self->focusGraph(); return TRUE; }
    const bool up = key == GDK_KEY_Up || key == GDK_KEY_KP_Up;
    const bool down = key == GDK_KEY_Down || key == GDK_KEY_KP_Down;
    if (!up && !down) return FALSE;
    const int step = up ? -1 : 1;
    const int next = GitUi::nextFileRow(self->rows_, sel, step);
    if (next >= 0) self->selectRow(next, true);
    else if (step > 0) self->focusGraph();
    return TRUE;
}

gboolean GitPanel::onGraphKey(GtkEventControllerKey*, guint key, guint, GdkModifierType mods,
                              gpointer selfp) {
    GitPanel* self = static_cast<GitPanel*>(selfp);
    if (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK)) return FALSE;
    const bool shift = mods & GDK_SHIFT_MASK;
    const int sel = self->selectedGraphRow();
    if (isEnter(key) && !shift) { self->showCommit(sel); return TRUE; }
    if (key == GDK_KEY_space) return TRUE;
    if (key == GDK_KEY_ISO_Left_Tab || (key == GDK_KEY_Tab && shift)) {
        self->focusListFromGraph(true);
        return TRUE;
    }
    if (key == GDK_KEY_Tab || key == GDK_KEY_KP_Tab) { self->focusMessageOrList(); return TRUE; }
    const bool up = key == GDK_KEY_Up || key == GDK_KEY_KP_Up;
    const bool down = key == GDK_KEY_Down || key == GDK_KEY_KP_Down;
    if (!up && !down) return FALSE;
    const int n = self->graphRowCount();
    int r = sel < 0 ? (down ? 0 : n - 1) : sel + (down ? 1 : -1);
    if (r >= 0 && r < n) self->selectGraphRow(r, true);
    else if (up) self->focusListFromGraph(false);
    return TRUE;
}

gboolean GitPanel::onMessageKey(GtkEventControllerKey*, guint key, guint, GdkModifierType mods,
                                gpointer selfp) {
    GitPanel* self = static_cast<GitPanel*>(selfp);
    const bool ctrl = mods & GDK_CONTROL_MASK;
    const bool other = mods & (GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK);
    if (isEnter(key) && ctrl && !other) { self->commit(); return TRUE; }
    if (ctrl || other) return FALSE;
    if (key == GDK_KEY_ISO_Left_Tab || (key == GDK_KEY_Tab && (mods & GDK_SHIFT_MASK))) {
        self->focusGraph();
        return TRUE;
    }
    if (key == GDK_KEY_Tab || key == GDK_KEY_KP_Tab) { self->focusList(); return TRUE; }
    return FALSE;
}

// ---------------------------------------------------------------- actions
void GitPanel::toggleStage(int row) {
    if (row < 0 || row >= (int)rows_.size() || rows_[row].header) {
        gtk_widget_error_bell(list_);
        return;
    }
    runAction(GitUi::stageArgs(rows_[row], initial_), nullptr);
}

void GitPanel::commit() {
    if (!inRepository_) return;
    GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(message_));
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* raw = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    const std::string msg = raw ? raw : "";
    g_free(raw);
    runAction(GitUi::commitArgs(msg), [this](int status, const std::string& out) {
        if (status != 0) return;   // the message is kept when a commit fails
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(message_)), "", 0);
        setError(GitUi::firstLine(out), true);   // "[main 1a2b3c4] Subject"
    });
}

void GitPanel::showDiff(int row) {
    if (row < 0 || row >= (int)rows_.size() || rows_[row].header || topLevel_.empty()) return;
    const GitUi::Row r = rows_[row];
    const unsigned gen = ++diffGeneration_;
    const std::string dir = topLevel_, root = rootDir_;
    const std::string path = dir + "/" + r.path;
    const std::string name = GitUi::validUtf8(baseName(r.path));
    const std::vector<std::string> args = GitUi::diffArgs(r);
    std::shared_ptr<bool> alive = alive_;
    GitPanel* self = this;
    post([=] {
        auto res = std::make_shared<GitResult>(runGit(dir, args));
        deliver(alive, self, [=](GitPanel* p) {
            if (gen != p->diffGeneration_ || root != p->rootDir_) return;
            // Exit status 1 from --no-index means "differs".
            if (!(res->ok() || (r.untracked && res->status == 1))) {
                p->setError(failure(*res), false);
                return;
            }
            g_log(GIT_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "diff: %s, %zu bytes", r.path.c_str(),
                  res->out.size());
            if (p->onShowDiff) p->onShowDiff(name, path, res->out);
        });
    });
}

// A commit's header, message and diff (against its first parent for a
// merge, which is what the merge brought in; git's default for a merge is a
// combined diff that is usually empty).
void GitPanel::showCommit(int row) {
    if (!graph_ || row < 0 || topLevel_.empty()) return;
    if (row >= (int)graph_->commits.size()) {
        if (graph_->hasMore) showMore();
        return;
    }
    const Git::Commit& c = graph_->commits[row];
    const std::string title = GitUi::commitTitle(c);
    const std::vector<std::string> args = GitUi::showArgs(c.hash);
    const unsigned gen = ++diffGeneration_;
    const std::string dir = topLevel_, root = rootDir_;
    std::shared_ptr<bool> alive = alive_;
    GitPanel* self = this;
    post([=] {
        auto res = std::make_shared<GitResult>(runGit(dir, args));
        deliver(alive, self, [=](GitPanel* p) {
            if (gen != p->diffGeneration_ || root != p->rootDir_) return;
            if (!res->ok()) {
                p->setError(failure(*res), false);
                return;
            }
            g_log(GIT_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "commit view: %s, %zu bytes",
                  title.c_str(), res->out.size());
            if (p->onShowCommit) p->onShowCommit(title, res->out);
        });
    });
}

// "Show more" raises the limit and loads the whole window again: topological
// order has to be worked out from the top anyway.
void GitPanel::showMore() {
    graphLimit_ += GitUi::kGraphBatch;
    runRefresh();
}

// ----------------------------------------------------------------- drawing
// A file: its name, its folder muted (and "from old" for a rename), and the
// status letter at the right. A heading: small muted capitals.
void GitPanel::drawRow(GtkDrawingArea* da, cairo_t* cr, int W, int H, gpointer handle) {
    GitPanel* self = *static_cast<std::shared_ptr<GitPanel*>*>(handle)->get();
    if (!self) return;   // the window is closing
    const int pos = positionOf(da);
    if (pos < 0 || pos >= (int)self->rows_.size()) return;
    const GitUi::Row& r = self->rows_[pos];
    GtkWidget* w = GTK_WIDGET(da);
    if (r.header) {
        std::string title = GitUi::validUtf8(r.title);
        char* up = g_utf8_strup(title.c_str(), -1);
        PangoLayout* l = makeLayout(w, up, 10, PANGO_WEIGHT_SEMIBOLD, W - 20,
                                    PANGO_ELLIPSIZE_END);
        g_free(up);
        PangoAttrList* attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_letter_spacing_new((int)(0.6 * PANGO_SCALE)));
        pango_layout_set_attributes(l, attrs);
        pango_attr_list_unref(attrs);
        setColor(cr, 0x9CA3AF);
        int tw = 0, th = 0;
        pango_layout_get_pixel_size(l, &tw, &th);
        cairo_move_to(cr, 10, 5 + (16 - th) / 2.0);
        pango_cairo_show_layout(cr, l);
        g_object_unref(l);
        return;
    }
    const double letterW = 22, mid = H / 2.0;
    std::string name = GitUi::validUtf8(baseName(r.path));
    std::string dir = GitUi::validUtf8(dirName(r.path));
    if (!r.origPath.empty())
        dir += (dir.empty() ? "" : "  ") + std::string("from ") + GitUi::validUtf8(r.origPath);
    std::string text = name;
    if (!dir.empty()) text += "  " + dir;
    PangoLayout* l = makeLayout(w, text, 12.5, PANGO_WEIGHT_NORMAL,
                                std::max(1.0, W - 18 - letterW - 4), PANGO_ELLIPSIZE_END);
    PangoAttrList* attrs = pango_attr_list_new();
    if (!dir.empty()) {
        PangoAttribute* muted = pango_attr_foreground_new(0x9C * 257, 0xA3 * 257, 0xAF * 257);
        muted->start_index = (guint)name.size();
        muted->end_index = (guint)text.size();
        pango_attr_list_insert(attrs, muted);
        PangoAttribute* small = pango_attr_size_new_absolute(11 * PANGO_SCALE);
        small->start_index = (guint)name.size();
        small->end_index = (guint)text.size();
        pango_attr_list_insert(attrs, small);
    }
    pango_layout_set_attributes(l, attrs);
    pango_attr_list_unref(attrs);
    setColor(cr, self->text_);   // the folder's muted color is an attribute
    showCentered(cr, l, 18, mid);
    g_object_unref(l);
    char letter[2] = {r.letter, 0};
    PangoLayout* ll = gtk_widget_create_pango_layout(w, letter);
    PangoFontDescription* fd = pango_font_description_from_string("Monospace Semi-Bold");
    pango_font_description_set_absolute_size(fd, 12 * PANGO_SCALE);
    pango_layout_set_font_description(ll, fd);
    pango_font_description_free(fd);
    setColor(cr, GitUi::letterColor(r.letter, r.unmerged));
    showCentered(cr, ll, W - letterW, mid);
    g_object_unref(ll);
}

// One commit: its lines and dot, the ref labels as pills, the subject, and
// an arrow at the right for a commit to push or pull.
void GitPanel::drawGraphRow(GtkDrawingArea* da, cairo_t* cr, int W, int H, gpointer handle) {
    GitPanel* self = *static_cast<std::shared_ptr<GitPanel*>*>(handle)->get();
    if (!self) return;
    const GitUi::Graph* g = self->graph_.get();
    const int pos = positionOf(da);
    if (!g || pos < 0) return;
    GtkWidget* w = GTK_WIDGET(da);
    const double h = H, mid = std::floor(h / 2);
    if (pos >= (int)g->commits.size()) {
        PangoLayout* l = makeLayout(w, "Show " + std::to_string(GitUi::kGraphBatch) + " more…",
                                    12, PANGO_WEIGHT_NORMAL, 0, PANGO_ELLIPSIZE_NONE);
        setColor(cr, 0x4EA1F7);
        showCentered(cr, l, 18, mid);
        g_object_unref(l);
        return;
    }
    const Git::Commit& c = g->commits[pos];
    const Git::GraphRow& row = g->rows[pos];
    const bool outgoing = g->divergence.outgoing.count(c.hash) > 0;
    const bool incoming = g->divergence.incoming.count(c.hash) > 0;
    const double lw = GitUi::laneWidth(W, g->maxWidth);
    auto X = [&](int lane) { return 8 + lane * lw + lw / 2; };

    if (outgoing || incoming) {
        setColor(cr, outgoing ? 0x4EA1F7 : 0x73C991, 0.09);
        cairo_rectangle(cr, 0, 0, W, H);
        cairo_fill(cr);
    }

    // Lines: straight down within a lane, curves between lanes, meeting the
    // dot sideways the way VS Code's graph draws a merge or a branch-off.
    cairo_set_line_width(cr, 1.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (const Git::GraphEdge& e : row.edges) {
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        switch (e.kind) {
        case Git::GraphEdge::Pass: x0 = X(e.from); y0 = 0; x1 = X(e.to); y1 = h; break;
        case Git::GraphEdge::In: x0 = X(e.from); y0 = 0; x1 = X(row.lane); y1 = mid; break;
        case Git::GraphEdge::Out: x0 = X(row.lane); y0 = mid; x1 = X(e.to); y1 = h; break;
        }
        cairo_move_to(cr, x0, y0);
        if (x0 == x1) cairo_line_to(cr, x1, y1);
        else if (e.kind == Git::GraphEdge::Pass) cairo_curve_to(cr, x0, mid, x1, mid, x1, y1);
        else if (e.kind == Git::GraphEdge::In) cairo_curve_to(cr, x0, y1, x0, y1, x1, y1);
        else cairo_curve_to(cr, x1, y0, x1, y0, x1, y1);
        setColor(cr, GitUi::laneColor(e.color));
        cairo_stroke(cr);
    }

    // The dot: filled; hollow when not pushed or not pulled yet; a merge has
    // a hole in the middle; HEAD gets a ring around it.
    const unsigned dc = GitUi::laneColor(row.color);
    const double cx = X(row.lane);
    auto circle = [&](double r) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, mid, r, 0, 2 * M_PI);
    };
    const bool merge = c.parents.size() > 1;
    if (c.hash == g->head) {
        setColor(cr, self->panelBg_);
        circle(6.5);
        cairo_fill(cr);
        circle(6);
        cairo_set_line_width(cr, 1.3);
        setColor(cr, dc);
        cairo_stroke(cr);
    }
    setColor(cr, self->panelBg_);
    circle(5);
    cairo_fill(cr);
    if (outgoing || incoming) {
        circle(3.4);
        cairo_set_line_width(cr, 1.6);
        setColor(cr, dc);
        cairo_stroke(cr);
        if (merge) { circle(1.4); cairo_fill(cr); }
    } else {
        setColor(cr, dc);
        circle(4);
        cairo_fill(cr);
        if (merge) { setColor(cr, self->panelBg_); circle(1.6); cairo_fill(cr); }
    }

    // Labels, then the subject.
    double x = 8 + row.width * lw + 6;
    const double right = W - ((outgoing || incoming) ? 20 : 6);
    auto it = g->labels.find(c.hash);
    if (it != g->labels.end()) {
        size_t shown = 0;
        for (const Git::Ref& r : it->second) {
            const double room = std::min(120.0, right - x - 40);
            if (room < 30) break;
            const double pw = drawPill(cr, w, r, x, mid, room);
            if (pw <= 0) break;
            x += pw + 4;
            ++shown;
        }
        if (shown < it->second.size() && right - x > 20) {
            PangoLayout* l = makeLayout(w, "+" + std::to_string(it->second.size() - shown), 10,
                                        PANGO_WEIGHT_NORMAL, 0, PANGO_ELLIPSIZE_NONE);
            setColor(cr, 0x9CA3AF);
            x += showCentered(cr, l, x, mid) + 4;
            g_object_unref(l);
        }
    }
    if (right - x > 8) {
        PangoLayout* l = makeLayout(w, GitUi::validUtf8(c.subject), 12.5, PANGO_WEIGHT_NORMAL,
                                    right - x, PANGO_ELLIPSIZE_END);
        if (incoming) setColor(cr, 0x9CA3AF);
        else setColor(cr, self->text_);
        showCentered(cr, l, x, mid);
        g_object_unref(l);
    }
    if (outgoing || incoming) {
        PangoLayout* l = makeLayout(w, outgoing ? "↑" : "↓", 12, PANGO_WEIGHT_SEMIBOLD,
                                    0, PANGO_ELLIPSIZE_NONE);
        setColor(cr, outgoing ? 0x4EA1F7 : 0x73C991);
        showCentered(cr, l, W - 16, mid);
        g_object_unref(l);
    }
}

// ------------------------------------------------------------ inspection
std::vector<std::string> GitPanel::rowDescriptions() const {
    std::vector<std::string> out;
    for (const GitUi::Row& r : rows_) {
        if (r.header) out.push_back("# " + r.title);
        else out.push_back(std::string(r.staged ? "S " : "W ") + r.letter + " " + r.path +
                           (r.origPath.empty() ? "" : " <- " + r.origPath));
    }
    return out;
}

std::vector<std::string> GitPanel::graphDescriptions() const {
    return graph_ ? GitUi::graphDescriptions(*graph_) : std::vector<std::string>();
}

std::string GitPanel::errorLine() const { return errorText_; }
