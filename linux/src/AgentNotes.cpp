// AgentNotes.cpp — see AgentNotes.h.
#include "AgentNotes.h"

#include <algorithm>

#include "Document.h"
#include "Editor.h"

namespace {

void installCss() {
    static bool done = false;
    if (done) return;
    done = true;
    GtkCssProvider* p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        p, ".agent-note { background: @theme_bg_color; border: 1px solid #E8A33D;"
           " border-radius: 6px; padding: 4px 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.35); }"
           ".agent-note .agent-note-from { color: #E8A33D; font-weight: bold; font-size: 90%; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

}  // namespace

AgentNotes::AgentNotes(Editor* editor) : ed_(editor) {}

AgentNotes::~AgentNotes() {
    unplace(false);
    if (idle_) g_source_remove(idle_);
    idle_ = 0;
    // The box hangs on the editor's overlay, which goes with the editor.
    box_ = nullptr;
}

void AgentNotes::buildBox() {
    if (box_) return;
    installCss();
    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    g_object_ref_sink(box_);
    gtk_widget_add_css_class(box_, "agent-note");
    gtk_widget_set_halign(box_, GTK_ALIGN_START);
    gtk_widget_set_valign(box_, GTK_ALIGN_START);
    gtk_widget_set_margin_start(box_, 56);
    gtk_widget_set_visible(box_, FALSE);
    GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    head_ = gtk_label_new("");
    gtk_widget_add_css_class(head_, "agent-note-from");
    gtk_label_set_xalign(GTK_LABEL(head_), 0.0);
    gtk_widget_set_hexpand(head_, TRUE);
    GtkWidget* x = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(x), FALSE);
    gtk_widget_set_focusable(x, FALSE);   // never takes the keyboard
    gtk_widget_set_focus_on_click(x, FALSE);
    gtk_widget_set_tooltip_text(x, "Close note");
    g_signal_connect(x, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        static_cast<AgentNotes*>(p)->close();
    }), this);
    gtk_box_append(GTK_BOX(top), head_);
    gtk_box_append(GTK_BOX(top), x);
    body_ = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(body_), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(body_), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(body_), 64);
    gtk_label_set_xalign(GTK_LABEL(body_), 0.0);
    gtk_label_set_selectable(GTK_LABEL(body_), FALSE);
    gtk_box_append(GTK_BOX(box_), top);
    gtk_box_append(GTK_BOX(box_), body_);
    ed_->addOverlay(box_);
}

std::string AgentNotes::label() const {
    if (!shown()) return "";
    std::string s = shownNote_.from.empty() ? "Claude" : shownNote_.from;
    if (shownNote_.tour && tourActive())
        s += " \xC2\xB7 " + std::to_string(tourIndex_ + 1) + " of " + std::to_string(stops_.size());
    return s;
}

int AgentNotes::firstLine() const {
    if (!shown() || !start_) return 0;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(buf_, &it, start_);
    return gtk_text_iter_get_line(&it) + 1;
}

int AgentNotes::lastLine() const {
    if (!shown() || !end_) return 0;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(buf_, &it, end_);
    return gtk_text_iter_get_line(&it) + 1;
}

bool AgentNotes::pendingFor(const Document* doc) const {
    for (const auto& p : pending_)
        if (p.first == doc) return true;
    return false;
}

void AgentNotes::show(Document* doc, const std::string& text, const std::string& from, int line,
                      int column, int toLine, int toColumn) {
    if (!doc) return;
    if (shownDoc_ == doc) unplace(false);
    Note n;
    n.text = text;
    n.from = from;
    n.line = line > 0 ? line : 1;
    n.column = column;
    n.toLine = toLine;
    n.toColumn = toColumn;
    pending_[doc] = n;
    if (doc == ed_->active()) place(doc);
}

// Whether an edit from character `from` to `to` touches the note's lines.
bool AgentNotes::touches(long from, long to) const {
    if (!start_ || !end_) return false;
    GtkTextIter s, e;
    gtk_text_buffer_get_iter_at_mark(buf_, &s, start_);
    gtk_text_buffer_get_iter_at_mark(buf_, &e, end_);
    return to >= gtk_text_iter_get_offset(&s) && from <= gtk_text_iter_get_offset(&e);
}

void AgentNotes::onInsert(GtkTextBuffer*, GtkTextIter* at, gchar*, gint, gpointer selfp) {
    AgentNotes* self = static_cast<AgentNotes*>(selfp);
    const long o = gtk_text_iter_get_offset(at);
    if (self->touches(o, o)) self->close();
}

void AgentNotes::onDelete(GtkTextBuffer*, GtkTextIter* a, GtkTextIter* b, gpointer selfp) {
    AgentNotes* self = static_cast<AgentNotes*>(selfp);
    if (self->touches(gtk_text_iter_get_offset(a), gtk_text_iter_get_offset(b))) self->close();
}

void AgentNotes::place(Document* doc) {
    if (doc != ed_->active()) return;
    auto it = pending_.find(doc);
    if (it == pending_.end()) return;
    if (doc->loading()) {   // read in the background: once it is in
        doc->whenLoaded([this, doc] {
            if (doc == ed_->active()) place(doc);
        });
        return;
    }
    if (!ed_->isSource()) return;   // stays with its tab until the text shows
    unplace(true);
    const Note n = it->second;
    pending_.erase(it);

    GtkTextBuffer* b = ed_->sourceBuffer();
    const int lines = gtk_text_buffer_get_line_count(b);
    auto clamp = [&](int l) { return std::max(0, std::min(l - 1, lines - 1)); };
    int l1 = clamp(n.line), l2 = n.toLine > 0 ? clamp(n.toLine) : l1;
    if (l2 < l1) std::swap(l1, l2);
    GtkTextIter first, last, tagStart, tagEnd;
    gtk_text_buffer_get_iter_at_line(b, &first, l1);
    gtk_text_buffer_get_iter_at_line(b, &last, l2);
    if (!gtk_text_iter_ends_line(&last)) gtk_text_iter_forward_to_line_end(&last);
    tagStart = first;
    if (n.column > 1)
        for (int c = 1; c < n.column && !gtk_text_iter_ends_line(&tagStart); c++)
            gtk_text_iter_forward_char(&tagStart);
    tagEnd = last;
    if (n.toLine > 0 && n.toColumn > 0) {
        gtk_text_buffer_get_iter_at_line(b, &tagEnd, l2);
        for (int c = 1; c < n.toColumn && !gtk_text_iter_ends_line(&tagEnd); c++)
            gtk_text_iter_forward_char(&tagEnd);
    }
    GtkTextIter after = last;   // the break too, so an empty line shows
    gtk_text_iter_forward_char(&after);
    if (gtk_text_iter_compare(&tagEnd, &last) == 0) tagEnd = after;

    buf_ = b;
    tag_ = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(b), "agent-note");
    if (!tag_) {
        GdkRGBA c = {0.91f, 0.64f, 0.24f, 0.22f};
        tag_ = gtk_text_buffer_create_tag(b, "agent-note", "paragraph-background-rgba", &c,
                                          nullptr);
    }
    gtk_text_buffer_apply_tag(b, tag_, &tagStart, &tagEnd);
    start_ = gtk_text_buffer_create_mark(b, nullptr, &first, TRUE);
    end_ = gtk_text_buffer_create_mark(b, nullptr, &last, FALSE);
    shownDoc_ = doc;
    shownNote_ = n;
    insertHandler_ = g_signal_connect(b, "insert-text", G_CALLBACK(onInsert), this);
    deleteHandler_ = g_signal_connect(b, "delete-range", G_CALLBACK(onDelete), this);

    buildBox();
    gtk_label_set_text(GTK_LABEL(head_), label().c_str());
    gtk_label_set_text(GTK_LABEL(body_), n.text.c_str());
    gtk_widget_set_visible(body_, !n.text.empty());
    gtk_widget_set_visible(box_, TRUE);

    GtkTextView* view = GTK_TEXT_VIEW(ed_->textView());
    // The caret moves only in the agent's own preview tab.
    if (doc->isAgentPreview()) gtk_text_buffer_place_cursor(b, &tagStart);
    gtk_text_view_scroll_to_mark(view, start_, 0.0, TRUE, 0.0, 0.33);
    adjustment_ = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view));
    if (adjustment_) {
        g_object_ref(adjustment_);
        scrollHandler_ = g_signal_connect_swapped(adjustment_, "value-changed",
                                                  G_CALLBACK(+[](AgentNotes* s) { s->reposition(); }),
                                                  this);
    }
    reposition();
    // The layout settles after the scroll: place the box once more then.
    if (idle_) g_source_remove(idle_);
    idle_ = g_timeout_add(60, +[](gpointer p) -> gboolean {
        AgentNotes* s = static_cast<AgentNotes*>(p);
        s->idle_ = 0;
        s->reposition();
        return G_SOURCE_REMOVE;
    }, this);
    updateActions();
}

// The box over the text, above the first line; below the last line when
// there is no room above.
void AgentNotes::reposition() {
    if (!box_ || !shown()) return;
    GtkTextView* view = GTK_TEXT_VIEW(ed_->textView());
    GtkTextIter s, e;
    gtk_text_buffer_get_iter_at_mark(buf_, &s, start_);
    gtk_text_buffer_get_iter_at_mark(buf_, &e, end_);
    GdkRectangle r1, r2;
    gtk_text_view_get_iter_location(view, &s, &r1);
    gtk_text_view_get_iter_location(view, &e, &r2);
    int x, y1, y2;
    gtk_text_view_buffer_to_window_coords(view, GTK_TEXT_WINDOW_WIDGET, r1.x, r1.y, &x, &y1);
    gtk_text_view_buffer_to_window_coords(view, GTK_TEXT_WINDOW_WIDGET, r2.x, r2.y, &x, &y2);
    int minH = 0, natH = 0;
    gtk_widget_measure(box_, GTK_ORIENTATION_VERTICAL, -1, &minH, &natH, nullptr, nullptr);
    int top = y1 - natH - 4;
    if (top < 2) top = y2 + r2.height + 4;
    const int viewH = gtk_widget_get_height(GTK_WIDGET(view));
    if (viewH > natH) top = std::max(2, std::min(top, viewH - natH - 2));
    gtk_widget_set_margin_top(box_, top);
}

// The shown note off the screen. Kept for its tab (with its lines as they are
// now) when `keep`, else forgotten.
void AgentNotes::unplace(bool keep) {
    if (!shownDoc_) return;
    Document* doc = shownDoc_;
    Note n = shownNote_;
    if (keep) {
        n.line = firstLine();
        n.column = 0;
        n.toLine = lastLine();
        n.toColumn = 0;
    }
    if (buf_) {
        if (insertHandler_) g_signal_handler_disconnect(buf_, insertHandler_);
        if (deleteHandler_) g_signal_handler_disconnect(buf_, deleteHandler_);
        GtkTextIter a, b;
        gtk_text_buffer_get_bounds(buf_, &a, &b);
        if (tag_) gtk_text_buffer_remove_tag(buf_, tag_, &a, &b);
        if (start_) gtk_text_buffer_delete_mark(buf_, start_);
        if (end_) gtk_text_buffer_delete_mark(buf_, end_);
    }
    insertHandler_ = deleteHandler_ = 0;
    start_ = end_ = nullptr;
    tag_ = nullptr;
    buf_ = nullptr;
    if (adjustment_) {
        if (scrollHandler_) g_signal_handler_disconnect(adjustment_, scrollHandler_);
        g_object_unref(adjustment_);
    }
    adjustment_ = nullptr;
    scrollHandler_ = 0;
    if (box_) gtk_widget_set_visible(box_, FALSE);
    shownDoc_ = nullptr;
    shownNote_ = Note();
    if (keep) pending_[doc] = n;
}

void AgentNotes::close() {
    const bool wasTour = shownDoc_ && shownNote_.tour;
    unplace(false);
    if (wasTour) endTour();
    updateActions();
}

bool AgentNotes::closeFromKey() {
    if (!shown()) return false;
    close();
    return true;
}

void AgentNotes::endTour() {
    stops_.clear();
    tourIndex_ = -1;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.tour) it = pending_.erase(it);
        else ++it;
    }
    updateActions();
}

void AgentNotes::activeWillChange() { unplace(true); }

void AgentNotes::activeChanged() {
    if (Document* d = ed_->active()) place(d);
}

void AgentNotes::forget(Document* doc) {
    if (shownDoc_ == doc) {
        const bool wasTour = shownNote_.tour;
        unplace(false);
        if (wasTour) endTour();
    }
    pending_.erase(doc);
}

bool AgentNotes::openStop(int index, bool quiet, std::string* error) {
    const Stop& s = stops_[(size_t)index];
    Document* d = ed_->openForAgent(s.path, true, quiet);
    if (!d) {
        if (error) *error = "could not open " + s.path;
        return false;
    }
    // The earlier stops' notes are done with.
    if (shownDoc_ && shownNote_.tour) unplace(false);
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.tour) it = pending_.erase(it);
        else ++it;
    }
    show(d, s.note, tourFrom_, s.line, s.column, s.toLine, 0);
    Note* n = nullptr;
    if (shownDoc_ == d) n = &shownNote_;
    else if (pending_.count(d)) n = &pending_[d];
    if (n) n->tour = true;
    if (shownDoc_ == d) {
        gtk_label_set_text(GTK_LABEL(head_), label().c_str());
    }
    return true;
}

bool AgentNotes::startTour(const std::vector<Stop>& stops, const std::string& from, bool quiet,
                           std::string* error) {
    if (stops.empty()) {
        if (error) *error = "a tour needs stops";
        return false;
    }
    if (shownDoc_ && shownNote_.tour) unplace(false);
    stops_ = stops;
    tourFrom_ = from;
    tourIndex_ = 0;
    if (!openStop(0, quiet, error)) {
        endTour();
        return false;
    }
    updateActions();
    return true;
}

bool AgentNotes::step(int delta) {
    if (!tourActive()) return false;
    const int next = tourIndex_ + delta;
    if (next < 0 || next >= (int)stops_.size()) return false;
    tourIndex_ = next;
    std::string error;
    if (!openStop(next, false, &error)) return false;
    updateActions();
    return true;
}

void AgentNotes::installActions(GActionMap* window) {
    actions_ = window;
    struct Def {
        const char* name;
        int delta;
    };
    for (const Def& d : {Def{"tournext", 1}, Def{"tourprevious", -1}}) {
        GSimpleAction* a = g_simple_action_new(d.name, nullptr);
        g_object_set_data(G_OBJECT(a), "delta", GINT_TO_POINTER(d.delta));
        g_signal_connect(a, "activate", G_CALLBACK(+[](GSimpleAction* a, GVariant*, gpointer p) {
            AgentNotes* s = static_cast<AgentNotes*>(p);
            if (!s->step(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(a), "delta"))))
                gtk_widget_error_bell(s->ed_->textView());   // at either end
        }), this);
        g_action_map_add_action(window, G_ACTION(a));
        g_object_unref(a);
    }
    updateActions();
}

void AgentNotes::updateActions() {
    if (!actions_) return;
    for (const char* name : {"tournext", "tourprevious"})
        if (GAction* a = g_action_map_lookup_action(actions_, name))
            g_simple_action_set_enabled(G_SIMPLE_ACTION(a), tourActive());
}
