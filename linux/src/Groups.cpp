// Groups.cpp — the split editor (PARITY.md 4.6), VS Code's two editor
// groups as the Mac's EditorController+Groups.mm has them: two at most,
// side by side, each with its own row of tabs, a bar between them that
// drags (EditorTabs::leftGroupWidth), and one document per file however
// many groups show it.
//
// The view and everything on it (the gutter, carets, folds, the find bar,
// the viewers) is always in the focused group: tabs_ and active_ are that
// group's, so the strip, the tab keys and closing act on it unchanged. The
// other group's tabs are other_ and otherActive_; it shows its tab on show
// in a plain read-only GtkTextView on the same GtkTextBuffer (D2 makes that
// a second view, where the Mac needed MCEditorMirror's copy of the layout),
// so what is typed in one group shows in the other as it is typed, with one
// undo history and one dirty state. A buffer holds a single selection, so
// the other group's caret and first line are kept as marks of their own and
// traded with the view's when the focus moves (swapGroups); the mirror's
// scroll is its own view's. A viewer (a picture, a PDF, a table, a preview)
// shows in the focused group only: the other group names the file until it
// is clicked, since a second MediaView, LaTeX preview or rendered page
// would cost as much again. A press in the other group, on its text or its
// strip, focuses that group, the press going on to the character it hit.
#include "Editor.h"

#include <algorithm>
#include <cmath>

#include "AgentNotes.h"
#include "CodeView.h"
#include "TabStrip.h"
#include "Zoom.h"

namespace {

constexpr int kBar = 4;    // the bar's line, as WindowLayout's
constexpr int kGrab = 4;   // its grab area past each side (.minicode-bar's padding)
constexpr double kMinGroupWidth = 150;   // the Mac's kMinSideWidth
const char kEditorKey[] = "minicode-editor";

std::string baseName(const std::string& p) {
    const size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

int offsetOf(GtkTextMark* m) {
    if (!m || gtk_text_mark_get_deleted(m)) return -1;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(gtk_text_mark_get_buffer(m), &it, m);
    return gtk_text_iter_get_offset(&it);
}

// A mark of our own at `offset` (clamped), which outlives its buffer. Left
// gravity: what the focused group types at the other group's caret goes in
// after it, leaving that caret where it was.
GtkTextMark* newMark(GtkTextBuffer* b, int offset) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(b, &it,
                                       std::min(offset, gtk_text_buffer_get_char_count(b)));
    GtkTextMark* m = gtk_text_mark_new(nullptr, TRUE);
    gtk_text_buffer_add_mark(b, m, &it);
    return m;
}

void dropMark(GtkTextMark*& m) {
    if (!m) return;
    if (!gtk_text_mark_get_deleted(m))
        gtk_text_buffer_delete_mark(gtk_text_mark_get_buffer(m), m);
    g_object_unref(m);
    m = nullptr;
}

int minWidth(GtkWidget* w) {
    int min = 0, nat = 0;
    gtk_widget_measure(w, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, nullptr, nullptr);
    return min;
}

void place(GtkWidget* w, int x, int width, int height) {
    minWidth(w);   // GTK asks for a measure before every allocation
    int min = 0, nat = 0;
    gtk_widget_measure(w, GTK_ORIENTATION_VERTICAL, std::max(width, 0), &min, &nat, nullptr,
                       nullptr);
    GtkAllocation a = {x, 0, std::max(width, 0), height};
    gtk_widget_size_allocate(w, &a, -1);
}

bool showsText(Document* d) {
    if (!d || d->loading() || d->unread()) return false;
    if (d->kind() == Document::Kind::Message) return true;
    if (d->kind() != Document::Kind::Text || d->showsTable()) return false;
    return !((d->isMarkdown() || d->isNotebook() || d->isLatex()) && d->showsPreview());
}

}  // namespace

// ---------------------------------------------------------------- the widgets

void Editor::buildGroups() {
    mirrorEmpty_ = gtk_text_buffer_new(nullptr);

    otherStrip_ = new TabStrip();
    otherStrip_->onSelect = [this](int i) {
        if (!split_ || i < 0 || i >= (int)other_.size()) return;
        Document* d = other_[(size_t)i];
        if (d != otherActive_) {
            otherActive_ = d;
            showInMirror(d);
        }
        focusGroup(1 - focused_);
    };
    otherStrip_->onClose = [this](int i) {
        if (!split_ || i < 0 || i >= (int)other_.size()) return;
        Document* d = other_[(size_t)i];
        // Still open in the focused group: nothing is lost, nothing asked.
        if (inBothGroups(d) || !onCloseRequest) dropOther(d, true);
        else onCloseRequest(d);
    };
    otherStrip_->onDoubleClick = [this](int i) {
        if (i >= 0 && i < (int)other_.size()) keepPreviewTab(other_[(size_t)i]);
    };

    mirrorView_ = gtk_text_view_new_with_buffer(mirrorEmpty_);
    GtkTextView* tv = GTK_TEXT_VIEW(mirrorView_);
    gtk_text_view_set_editable(tv, FALSE);
    gtk_text_view_set_cursor_visible(tv, FALSE);
    gtk_text_view_set_monospace(tv, TRUE);
    gtk_text_view_set_left_margin(tv, 8);
    gtk_text_view_set_top_margin(tv, 6);
    gtk_widget_set_focusable(mirrorView_, FALSE);
    gtk_widget_add_css_class(mirrorView_, "minicode-editor");
    gtk_widget_add_css_class(mirrorView_, "minicode-mirror");
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroller, "minicode-scroller");
    gtk_widget_add_css_class(scroller, "minicode-editor-scroller");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), mirrorView_);
    mirrorNote_ = gtk_label_new("");
    gtk_label_set_justify(GTK_LABEL(mirrorNote_), GTK_JUSTIFY_CENTER);
    gtk_label_set_wrap(GTK_LABEL(mirrorNote_), TRUE);
    gtk_widget_add_css_class(mirrorNote_, "minicode-mirror-note");
    mirrorStack_ = gtk_stack_new();
    gtk_widget_set_hexpand(mirrorStack_, TRUE);
    gtk_widget_set_vexpand(mirrorStack_, TRUE);
    gtk_stack_add_named(GTK_STACK(mirrorStack_), scroller, "text");
    gtk_stack_add_named(GTK_STACK(mirrorStack_), mirrorNote_, "note");
    // A press anywhere in it is the group's: taken before the view sees it
    // (which would move the shared buffer's caret), and the group focused
    // with the caret where it landed, once the press is over.
    GtkGesture* press = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(press), 0);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(press), GTK_PHASE_CAPTURE);
    g_signal_connect(press, "pressed", G_CALLBACK((+[](GtkGestureClick* g, int, double x, double y,
                                                       gpointer p) {
        Editor* self = static_cast<Editor*>(p);
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
        if (!self->split_) return;
        int at = -1;
        GtkTextView* v = GTK_TEXT_VIEW(self->mirrorView_);
        graphene_point_t in = GRAPHENE_POINT_INIT((float)x, (float)y), out;
        if (self->otherActive_ && gtk_text_view_get_buffer(v) == self->otherActive_->buffer() &&
            gtk_widget_compute_point(self->mirrorStack_, self->mirrorView_, &in, &out)) {
            int bx = 0, by = 0;
            gtk_text_view_window_to_buffer_coords(v, GTK_TEXT_WINDOW_WIDGET, (int)out.x,
                                                  (int)out.y, &bx, &by);
            GtkTextIter it;
            gtk_text_view_get_iter_at_location(v, &it, bx, by);
            at = gtk_text_iter_get_offset(&it);
        }
        struct Job {
            std::weak_ptr<int> alive;
            Editor* editor;
            int at;
            Document* doc;
        };
        auto* job = new Job{self->alive_, self, at, self->otherActive_};
        g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer q) -> gboolean {
            Job* j = static_cast<Job*>(q);
            if (j->alive.expired()) return G_SOURCE_REMOVE;
            Editor* e = j->editor;
            if (!e->split_ || e->otherActive_ != j->doc) return G_SOURCE_REMOVE;
            e->focusGroup(1 - e->focused_);
            if (j->at >= 0 && e->isSource()) {
                GtkTextIter it;
                gtk_text_buffer_get_iter_at_offset(e->buffer_, &it, j->at);
                gtk_text_buffer_place_cursor(e->buffer_, &it);
            }
            return G_SOURCE_REMOVE;
        }, job, [](gpointer q) { delete static_cast<Job*>(q); });
    })), this);
    gtk_widget_add_controller(mirrorStack_, GTK_EVENT_CONTROLLER(press));

    otherBox_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(otherBox_), otherStrip_->widget());
    gtk_box_append(GTK_BOX(otherBox_), mirrorStack_);

    groupBar_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(groupBar_, "minicode-bar");
    gtk_widget_add_css_class(groupBar_, "vertical");
    gtk_widget_set_cursor_from_name(groupBar_, "col-resize");
    // The drag is the container's, in the capture phase as WindowLayout's:
    // on the bar itself the offsets would shrink as the bar moves under the
    // pointer. A press anywhere else is the groups' own.
    GtkGesture* drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drag), GTK_PHASE_CAPTURE);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(+[](GtkGestureDrag* g, double x, double, gpointer p) {
        Editor* self = static_cast<Editor*>(p);
        const double total = gtk_widget_get_width(self->groups_) - kBar;
        const double left = EditorTabs::leftGroupWidth(total, self->groupFraction_, kMinGroupWidth);
        if (!self->split_ || x < left - kGrab || x >= left + kBar + kGrab) {
            gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);
            self->dragLeft_ = -1;
            return;
        }
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
        self->dragLeft_ = left;
        gtk_widget_add_css_class(self->groupBar_, "dragging");
    }), this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(+[](GtkGestureDrag*, double dx, double, gpointer p) {
        Editor* self = static_cast<Editor*>(p);
        const double total = gtk_widget_get_width(self->groups_) - kBar;
        if (self->dragLeft_ < 0 || total <= 0) return;
        // Past either end it stops at that side's minimum (leftGroupWidth
        // takes a share outside 0 to 1 for halves).
        const double want = std::clamp((self->dragLeft_ + dx) / total, 0.001, 0.999);
        self->setGroupFraction(EditorTabs::leftGroupWidth(total, want, kMinGroupWidth) / total);
    }), this);
    g_signal_connect(drag, "drag-end", G_CALLBACK(+[](GtkGestureDrag*, double, double, gpointer p) {
        Editor* self = static_cast<Editor*>(p);
        if (self->dragLeft_ < 0) return;
        self->dragLeft_ = -1;
        gtk_widget_remove_css_class(self->groupBar_, "dragging");
        if (self->onDocumentsChanged) self->onDocumentsChanged(nullptr);   // the session keeps it
    }), this);

    // A box only for holding them: the layout is ours, the bar last so it is
    // on top of the groups' edges.
    groups_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_layout_manager(groups_, gtk_custom_layout_new(
        [](GtkWidget*) { return GTK_SIZE_REQUEST_CONSTANT_SIZE; }, groupsMeasure, groupsAllocate));
    g_object_set_data(G_OBJECT(groups_), kEditorKey, this);
    gtk_widget_add_controller(groups_, GTK_EVENT_CONTROLLER(drag));
    gtk_widget_set_hexpand(groups_, TRUE);
    gtk_widget_set_vexpand(groups_, TRUE);
    for (GtkWidget* w : {box_, otherBox_}) {
        gtk_widget_set_overflow(w, GTK_OVERFLOW_HIDDEN);
        gtk_box_append(GTK_BOX(groups_), w);
    }
    gtk_box_append(GTK_BOX(groups_), groupBar_);
    gtk_widget_set_visible(otherBox_, FALSE);
    gtk_widget_set_visible(groupBar_, FALSE);
}

void Editor::groupsMeasure(GtkWidget* w, GtkOrientation o, int, int* min, int* nat, int* minBase,
                           int* natBase) {
    *min = *nat = 0;
    *minBase = *natBase = -1;
    Editor* self = static_cast<Editor*>(g_object_get_data(G_OBJECT(w), kEditorKey));
    if (!self) return;
    const bool beside = self->split_ && o == GTK_ORIENTATION_HORIZONTAL;
    for (GtkWidget* c : {self->box_, self->otherBox_}) {
        if (!gtk_widget_get_visible(c)) continue;
        int m = 0, n = 0;
        gtk_widget_measure(c, o, -1, &m, &n, nullptr, nullptr);
        *min = beside ? *min + m : std::max(*min, m);
        *nat = beside ? *nat + n : std::max(*nat, n);
    }
    if (beside) {
        *min += kBar;
        *nat += kBar;
    }
}

void Editor::groupsAllocate(GtkWidget* w, int width, int height, int) {
    Editor* self = static_cast<Editor*>(g_object_get_data(G_OBJECT(w), kEditorKey));
    if (!self) return;
    if (!self->split_) {
        place(self->box_, 0, width, height);
        return;
    }
    const int left = (int)std::lround(
        EditorTabs::leftGroupWidth(width - kBar, self->groupFraction_, kMinGroupWidth));
    GtkWidget* l = self->focused_ == 0 ? self->box_ : self->otherBox_;
    GtkWidget* r = self->focused_ == 0 ? self->otherBox_ : self->box_;
    place(l, 0, left, height);
    place(r, left + kBar, width - left - kBar, height);
    place(self->groupBar_, left - kGrab, kBar + 2 * kGrab, height);
}

GtkWidget* Editor::groupWidget(int group) const {
    if (!split_) return group == 0 ? box_ : nullptr;
    return group == focused_ ? box_ : otherBox_;
}

void Editor::setGroupFraction(double f) {
    groupFraction_ = std::clamp(f, 0.0, 1.0);
    gtk_widget_queue_allocate(groups_);
}

// The other group's strip and what its view shows, after any change to
// either group (refreshStrip calls it).
void Editor::refreshMirror() {
    if (!groups_) return;   // still being built
    gtk_widget_set_visible(otherBox_, split_);
    gtk_widget_set_visible(groupBar_, split_);
    gtk_widget_queue_allocate(groups_);
    Document* d = split_ ? otherActive_ : nullptr;
    if (!d) {
        CodeView::setViewBuffer(GTK_TEXT_VIEW(mirrorView_), mirrorEmpty_);
        otherStrip_->setItems({}, -1);
        return;
    }
    std::vector<std::string> paths;
    for (Document* t : other_)
        paths.push_back(t->kind() == Document::Kind::Diff ? std::string() : t->path());
    const std::vector<std::string> names = EditorTabs::labels(paths);
    std::vector<TabStrip::Item> items;
    int on = -1;
    for (size_t i = 0; i < other_.size(); i++) {
        const Document* t = other_[i];
        TabStrip::Item it;
        it.title = t->kind() == Document::Kind::Diff ? t->diffTitle() : names[i];
        if (it.title.empty()) it.title = baseName(t->path());
        it.tooltip = t->path().empty() ? it.title : t->path();
        it.dirty = t->dirty();
        it.preview = t->isPreviewTab() || t->isAgentPreview();
        items.push_back(it);
        if (t == d) on = (int)i;
    }
    otherStrip_->setItems(items, on);

    GtkTextView* tv = GTK_TEXT_VIEW(mirrorView_);
    if (showsText(d)) {
        CodeView::setViewBuffer(tv, d->buffer());
        const bool prose = d->kind() == Document::Kind::Message;
        const bool wraps = prose || (Zoom::wordWrap() && !d->largeFileFeaturesOff());
        gtk_text_view_set_monospace(tv, prose ? FALSE : TRUE);
        gtk_text_view_set_wrap_mode(tv, wraps ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
        if (prose) gtk_widget_add_css_class(mirrorView_, "minicode-prose");
        else gtk_widget_remove_css_class(mirrorView_, "minicode-prose");
        gtk_stack_set_visible_child_name(GTK_STACK(mirrorStack_), "text");
        if (mirrorScrollPending_ && mirrorTop_ && !gtk_text_mark_get_deleted(mirrorTop_) &&
            gtk_text_mark_get_buffer(mirrorTop_) == d->buffer()) {
            gtk_text_view_scroll_to_mark(tv, mirrorTop_, 0, TRUE, 0, 0);
            // Done by the view's next layout; until then its own visible
            // rect is not yet the place (mirrorPlace reads the mark).
            mirrorTicks_ = 3;
            if (!mirrorTickId_)
                mirrorTickId_ = gtk_widget_add_tick_callback(mirrorView_, [](GtkWidget*,
                        GdkFrameClock*, gpointer p) -> gboolean {
                    Editor* self = static_cast<Editor*>(p);
                    if (--self->mirrorTicks_ > 0) return G_SOURCE_CONTINUE;
                    self->mirrorScrollPending_ = false;
                    self->mirrorTickId_ = 0;
                    return G_SOURCE_REMOVE;
                }, this, nullptr);
        }
    } else {
        CodeView::setViewBuffer(tv, mirrorEmpty_);
        const std::string name = items.empty() || on < 0 ? std::string() : items[(size_t)on].title;
        gtk_label_set_text(GTK_LABEL(mirrorNote_), (name + "\n\nClick to show it here").c_str());
        gtk_stack_set_visible_child_name(GTK_STACK(mirrorStack_), "note");
    }
}

// ---------------------------------------------------------------- the groups' tabs

std::vector<Document*> Editor::documents() const {
    std::vector<Document*> out = tabs_;
    for (Document* d : other_)
        if (std::find(tabs_.begin(), tabs_.end(), d) == tabs_.end()) out.push_back(d);
    return out;
}

std::vector<Document*> Editor::groupDocuments(int group) const {
    if (!split_) return group == 0 ? tabs_ : std::vector<Document*>();
    return group == focused_ ? tabs_ : other_;
}

Document* Editor::groupActive(int group) const {
    if (!split_) return group == 0 ? active_ : nullptr;
    return group == focused_ ? active_ : otherActive_;
}

bool Editor::inOther(const Document* d) const {
    return d && std::find(other_.begin(), other_.end(), d) != other_.end();
}

bool Editor::isOpen(const Document* d) const { return indexOf(d) >= 0 || inOther(d); }

bool Editor::inBothGroups(const Document* d) const { return indexOf(d) >= 0 && inOther(d); }

// ---------------------------------------------------------------- places

void Editor::dropMirrorMarks() {
    dropMark(mirrorIns_);
    dropMark(mirrorBound_);
    dropMark(mirrorTop_);
    mirrorScrollPending_ = false;
}

void Editor::setMirrorPlace(Document* d, int selStart, int selEnd, int top) {
    dropMirrorMarks();
    if (!d || d->kind() != Document::Kind::Text || d->unread()) return;
    GtkTextBuffer* b = d->buffer();
    if (selStart >= 0 && selEnd >= 0) {
        mirrorIns_ = newMark(b, selStart);
        mirrorBound_ = newMark(b, selEnd);
    }
    if (top >= 0) {
        mirrorTop_ = newMark(b, top);
        mirrorScrollPending_ = true;
    }
}

// The other group's place for its tab on show: the selection kept for it,
// and the first line its view shows (the mark until the view has scrolled
// there).
void Editor::mirrorPlace(int* selStart, int* selEnd, int* top) const {
    *selStart = *selEnd = *top = -1;
    Document* d = otherActive_;
    if (!d || d->kind() != Document::Kind::Text || d->unread()) return;
    GtkTextBuffer* b = d->buffer();
    if (mirrorIns_ && !gtk_text_mark_get_deleted(mirrorIns_) &&
        gtk_text_mark_get_buffer(mirrorIns_) == b) {
        *selStart = offsetOf(mirrorIns_);
        *selEnd = offsetOf(mirrorBound_);
    }
    GtkTextView* tv = GTK_TEXT_VIEW(mirrorView_);
    if (!mirrorScrollPending_ && gtk_text_view_get_buffer(tv) == b &&
        gtk_widget_get_mapped(mirrorView_)) {
        GdkRectangle vis;
        gtk_text_view_get_visible_rect(tv, &vis);
        GtkTextIter it;
        gtk_text_view_get_line_at_y(tv, &it, vis.y, nullptr);
        *top = gtk_text_iter_get_offset(&it);
    } else if (mirrorTop_ && !gtk_text_mark_get_deleted(mirrorTop_) &&
               gtk_text_mark_get_buffer(mirrorTop_) == b) {
        *top = offsetOf(mirrorTop_);
    }
}

// The view's place for `d`, the tab on show: its selection and first line.
void Editor::viewPlace(Document* d, int* selStart, int* selEnd, int* top) {
    *selStart = *selEnd = *top = -1;
    if (!d || d->kind() != Document::Kind::Text || d->unread()) return;
    GtkTextIter a, b;
    gtk_text_buffer_get_selection_bounds(d->buffer(), &a, &b);
    // The insert mark first, so the caret stays the caret.
    GtkTextIter ins;
    gtk_text_buffer_get_iter_at_mark(d->buffer(), &ins, gtk_text_buffer_get_insert(d->buffer()));
    const bool insAtEnd = gtk_text_iter_equal(&ins, &b);
    *selStart = gtk_text_iter_get_offset(insAtEnd ? &b : &a);
    *selEnd = gtk_text_iter_get_offset(insAtEnd ? &a : &b);
    if (d == active_) capturePlace(d);
    const Document::Place& p = d->place();
    if (p.haveView && p.top) {
        GtkTextIter t;
        gtk_text_buffer_get_iter_at_mark(d->buffer(), &t, p.top);
        *top = gtk_text_iter_get_offset(&t);
    }
}

// `d` comes into the view at this place (the buffer's selection, and its
// place for showActive).
void Editor::givePlace(Document* d, int selStart, int selEnd, int top) {
    if (!d || d->kind() != Document::Kind::Text || d->unread()) return;
    GtkTextBuffer* b = d->buffer();
    const int chars = gtk_text_buffer_get_char_count(b);
    if (selStart >= 0 && selEnd >= 0) {
        GtkTextIter ins, bound;
        gtk_text_buffer_get_iter_at_offset(b, &ins, std::min(selStart, chars));
        gtk_text_buffer_get_iter_at_offset(b, &bound, std::min(selEnd, chars));
        gtk_text_buffer_select_range(b, &ins, &bound);
    }
    if (top >= 0) {
        Document::Place& p = d->place();
        GtkTextIter t;
        gtk_text_buffer_get_iter_at_offset(b, &t, std::min(top, chars));
        if (!p.top) p.top = gtk_text_buffer_create_mark(b, nullptr, &t, TRUE);
        else gtk_text_buffer_move_mark(b, p.top, &t);
        p.topInto = 0;
        p.haveView = true;
    }
}

// `d` becomes the other group's tab on show, read first if it was not yet,
// at its own place (the view's, when the view shows it too).
void Editor::showInMirror(Document* d) {
    if (!d) return;
    if (d->unread()) {
        loadInto(d, d->path());
        if (d->place().haveRestore && !d->loading()) {
            d->place().haveRestore = false;
            placeTab(d, d->place().restore);
        }
    }
    int s = -1, e = -1, t = -1;
    viewPlace(d, &s, &e, &t);
    setMirrorPlace(d, s, e, t);
}

// ---------------------------------------------------------------- the moves

// The focus goes to the other group: the view takes its tab at its place,
// and the other group keeps the tab the view had, at the view's place.
void Editor::swapGroups() {
    Document* a = active_;
    Document* b = otherActive_;
    int ms, me, mt, as, ae, at;
    mirrorPlace(&ms, &me, &mt);
    viewPlace(a, &as, &ae, &at);
    GtkTextBuffer* before = buffer_;
    { if (notes_) notes_->activeWillChange(); if (onActiveWillChange) onActiveWillChange(); }
    stash();
    std::swap(tabs_, other_);
    active_ = b;
    otherActive_ = a;
    focused_ = 1 - focused_;
    if (b) {
        givePlace(b, ms, me, mt);
        b->lastUsed = ++stamp_;
    }
    setMirrorPlace(a, as, ae, at);
    showActive();   // refreshStrip, refreshMirror
    // The same file in both groups: the view kept its buffer, so showSource
    // left its scroll alone; it goes to the other group's place.
    if (buffer_ == before && isSource()) restorePlace(active_);
    if (active_) active_->checkExternalChange();
    { if (onActiveChanged) onActiveChanged(); if (notes_) notes_->activeChanged(); }
}

void Editor::focusGroup(int group) {
    if (!split_) {
        if (group == 0) focusActiveDocument();
        else if (active_) splitEditor();   // VS Code's: the second group made
        else gtk_widget_error_bell(view_);
        return;
    }
    if (group != focused_) swapGroups();
    focusActiveDocument();
}

void Editor::splitEditor() {
    Document* a = active_;
    if (!a) {
        gtk_widget_error_bell(view_);
        return;
    }
    int s, e, t;
    viewPlace(a, &s, &e, &t);
    if (!split_) {
        split_ = true;
        focused_ = 0;
        other_ = {a};
    } else if (!inOther(a)) {
        // Two groups at most: the tab goes to the other one.
        const auto on = std::find(other_.begin(), other_.end(), otherActive_);
        other_.insert(other_.begin() + EditorTabs::insertionIndex((int)(on - other_.begin()),
                                                                  (int)other_.size()), a);
    }
    otherActive_ = a;
    setMirrorPlace(a, s, e, t);
    swapGroups();   // the keyboard goes to the new group, at the same place
    focusActiveDocument();
}

void Editor::moveToGroup(int direction) {
    Document* a = active_;
    const int target = EditorTabs::moveTarget(focusedGroup(), split_, direction);
    // Alone in the only group, it would leave an empty group behind.
    if (!a || target < 0 || target == focusedGroup() || (!split_ && tabs_.size() < 2)) {
        gtk_widget_error_bell(view_);
        return;
    }
    int s, e, t;
    viewPlace(a, &s, &e, &t);
    if (!split_) {
        split_ = true;
        focused_ = 0;
        other_.clear();
        otherActive_ = nullptr;
    }
    if (!inOther(a)) {
        const auto on = std::find(other_.begin(), other_.end(), otherActive_);
        const int at = otherActive_ ? (int)(on - other_.begin()) : -1;
        other_.insert(other_.begin() + EditorTabs::insertionIndex(at, (int)other_.size()), a);
    }
    otherActive_ = a;
    setMirrorPlace(a, s, e, t);
    swapGroups();         // the keyboard goes with the tab
    dropOther(a, false);  // out of the group it left, which closes if that was its last
    focusActiveDocument();
}

// Out of the other group (its close button, a move, the Trash): the group's
// most recently used tab comes on show, and with none left the group goes.
void Editor::dropOther(Document* d, bool remember) {
    const auto it = std::find(other_.begin(), other_.end(), d);
    if (it == other_.end()) return;
    const int index = (int)(it - other_.begin());
    if (remember && d->kind() != Document::Kind::Diff && !d->path().empty() && indexOf(d) < 0) {
        EditorTabs::ClosedTab c = closedTabOf(d);
        c.index = index;
        closed_.remember(c);
    }
    std::vector<uint64_t> stamps;
    for (Document* t : other_) stamps.push_back(t->lastUsed);
    const auto on = std::find(other_.begin(), other_.end(), otherActive_);
    const int next = EditorTabs::activeAfterClose(stamps, (int)(on - other_.begin()), index);
    other_.erase(other_.begin() + index);
    if (other_.empty()) {
        split_ = false;
        focused_ = 0;
        otherActive_ = nullptr;
        dropMirrorMarks();
    } else if (d == otherActive_) {
        otherActive_ = other_[(size_t)std::clamp(next, 0, (int)other_.size() - 1)];
        showInMirror(otherActive_);
    }
    if (d == diffDoc_ && !isOpen(d)) diffDoc_ = nullptr;
    if (!isOpen(d)) {
        CodeView::setViewBuffer(GTK_TEXT_VIEW(mirrorView_), mirrorEmpty_);   // lets go of it
        destroy(d);
    }
    refreshStrip();
}

// The focused group's last tab went (dropTab): the other group is the only
// one, its tab on show in the view at the place it had.
void Editor::closeFocusedGroup() {
    Document* b = otherActive_;
    int s, e, t;
    mirrorPlace(&s, &e, &t);
    GtkTextBuffer* before = buffer_;   // the closed group's tab's (stash kept it on show)
    tabs_ = other_;
    other_.clear();
    otherActive_ = nullptr;
    split_ = false;
    focused_ = 0;
    dropMirrorMarks();
    active_ = b;
    if (b) {
        givePlace(b, s, e, t);
        b->lastUsed = ++stamp_;
    }
    showActive();
    if (buffer_ == before && isSource()) restorePlace(active_);
    if (active_) active_->checkExternalChange();
    { if (onActiveChanged) onActiveChanged(); if (notes_) notes_->activeChanged(); }
}

// ---------------------------------------------------------------- the session

EditorTabs::GroupsSession Editor::groupsSession() const {
    EditorTabs::GroupsSession s;
    auto isFile = [](const Document* d) {
        return d && d->kind() != Document::Kind::Diff && !d->path().empty();
    };
    for (int g = 0; g < 2; g++) {
        for (Document* d : groupDocuments(g))
            if (isFile(d)) s.groups[g].push_back(d->path());
        if (isFile(groupActive(g))) s.active[g] = groupActive(g)->path();
    }
    s.focused = focusedGroup();
    s.fraction = groupFraction_;
    return s;
}

void Editor::restoreGroups(const EditorTabs::GroupLayout& layout) {
    if (!layout.split || split_) return;
    const std::vector<Document*> all = documents();   // what the layout counts in
    std::vector<Document*> g[2];
    for (int k = 0; k < 2; k++)
        for (int i : layout.groups[k])
            if (i >= 0 && i < (int)all.size()) g[k].push_back(all[(size_t)i]);
    if (g[0].empty() || g[1].empty()) return;
    const int f = layout.focused == 1 ? 1 : 0;
    // Nothing is left out of both (EditorTabs::restoreGroups puts any it was
    // not told of in the focused group).
    for (Document* d : all)
        if (std::find(g[0].begin(), g[0].end(), d) == g[0].end() &&
            std::find(g[1].begin(), g[1].end(), d) == g[1].end())
            g[f].push_back(d);
    { if (notes_) notes_->activeWillChange(); if (onActiveWillChange) onActiveWillChange(); }
    stash();
    split_ = true;
    focused_ = f;
    groupFraction_ = layout.fraction;
    tabs_ = g[f];
    other_ = g[1 - f];
    auto pick = [](const std::vector<Document*>& v, int i) {
        return i >= 0 && i < (int)v.size() ? v[(size_t)i] : v.front();
    };
    active_ = pick(tabs_, layout.active[f]);
    otherActive_ = pick(other_, layout.active[1 - f]);
    active_->lastUsed = ++stamp_;
    showInMirror(otherActive_);
    showActive();
    if (active_) active_->checkExternalChange();
    { if (onActiveChanged) onActiveChanged(); if (notes_) notes_->activeChanged(); }
}
