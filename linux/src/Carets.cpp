// Carets.cpp — see Carets.h. The carets' state, what ends them, the mouse,
// the keys and the view's movement signals, the geometry, and the drawing;
// the edits and commands are in CaretEdits.cpp.
#include "Carets.h"

#include "AutoCloseInput.h"
#include "BufferLines.h"
#include "CaretText.h"
#include "CodeView.h"
#include "Document.h"
#include "Editor.h"
#include "ThemeCss.h"

#include <algorithm>
#include <cmath>

using MultiCaret::Caret;
using MultiCaret::Move;

namespace {

size_t offsetOf(const GtkTextIter* it) { return (size_t)gtk_text_iter_get_offset(it); }

GdkRGBA toGdk(const Rgba& c) {
    return GdkRGBA{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (float)c.a};
}

}  // namespace

// ---------------------------------------------------------------- life

Carets::Carets(Editor* editor) : editor_(editor), view_(editor->textView()) {
    CodeView* cv = editor->codeView();
    belowPass_ = cv->addPass(CodeView::Layer::BelowText, 5,
                             [this](GtkSnapshot* s, const GdkRectangle& v) { drawSelections(s, v); });
    abovePass_ = cv->addPass(CodeView::Layer::AboveText, 10,
                             [this](GtkSnapshot* s, const GdkRectangle& v) { drawCarets(s, v); });
    g_signal_connect(view_, "notify::buffer", G_CALLBACK(onBuffer), this);
    // GTK's own key bindings name the moves (an arrow, a word, a line's
    // end), so these hold whatever keys GTK gives them.
    g_signal_connect(view_, "move-cursor", G_CALLBACK(onMoveCursor), this);
    g_signal_connect(view_, "delete-from-cursor", G_CALLBACK(onDeleteFrom), this);
    g_signal_connect(view_, "copy-clipboard", G_CALLBACK(onCopy), this);
    g_signal_connect(view_, "cut-clipboard", G_CALLBACK(onCut), this);
    g_signal_connect(view_, "paste-clipboard", G_CALLBACK(onPaste), this);

    // The mouse, ahead of the text view's own gestures (capture phase): a
    // press this takes is claimed, so the view never sees it. The drag is
    // grouped with the click, so claiming one claims both.
    click_ = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_), 0);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click_), GTK_PHASE_CAPTURE);
    g_signal_connect(click_, "pressed", G_CALLBACK(onPressed), this);
    drag_ = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag_), 0);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drag_), GTK_PHASE_CAPTURE);
    g_signal_connect(drag_, "drag-update", G_CALLBACK(onDragUpdate), this);
    g_signal_connect(drag_, "drag-end", G_CALLBACK(onDragEnd), this);
    gtk_gesture_group(click_, drag_);
    gtk_widget_add_controller(view_, GTK_EVENT_CONTROLLER(click_));
    gtk_widget_add_controller(view_, GTK_EVENT_CONTROLLER(drag_));
    watchBuffer();
}

// Runs while the window's widgets still exist (Editor's destructor).
Carets::~Carets() {
    alive_.reset();
    if (buffer_) {
        g_signal_handlers_disconnect_by_data(buffer_, this);
        if (BufferLines* l = BufferLines::of(buffer_)) l->extraCarets = nullptr;
        g_object_unref(buffer_);
        buffer_ = nullptr;
    }
    g_signal_handlers_disconnect_by_data(view_, this);
    g_signal_handlers_disconnect_by_data(click_, this);
    g_signal_handlers_disconnect_by_data(drag_, this);
    if (CodeView* cv = editor_->codeView()) {
        cv->removePass(belowPass_);
        cv->removePass(abovePass_);
    }
}

GtkTextView* Carets::view() const { return GTK_TEXT_VIEW(view_); }

bool Carets::inSource() const { return buffer_ && editor_->isSource(); }

bool Carets::applies() const { return inSource() && gtk_text_view_get_editable(view()); }

size_t Carets::length() const {
    return buffer_ ? (size_t)gtk_text_buffer_get_char_count(buffer_) : 0;
}

// ---------------------------------------------------------------- state

std::vector<Caret> Carets::selectionCarets() const {
    if (!buffer_) return {{0, 0}};
    GtkTextIter ins, bound;
    gtk_text_buffer_get_iter_at_mark(buffer_, &ins, gtk_text_buffer_get_insert(buffer_));
    gtk_text_buffer_get_iter_at_mark(buffer_, &bound, gtk_text_buffer_get_selection_bound(buffer_));
    return {{offsetOf(&bound), offsetOf(&ins)}};
}

std::vector<Caret> Carets::current() const { return active() ? carets_ : selectionCarets(); }

// Taking the clipboard off a buffer that holds the primary selection gives
// the selection up, and GtkTextBuffer then empties its own selection (a
// mark set, not ours, that would end the carets): so as one of our own
// moves. Null when it is off already (an edit's set()).
GdkClipboard* Carets::takePrimary() {
    if (!buffer_ || primaryTaken_ || !gtk_widget_get_realized(view_)) return nullptr;
    GdkClipboard* primary = gtk_widget_get_primary_clipboard(view_);
    applying_++;
    gtk_text_buffer_remove_selection_clipboard(buffer_, primary);
    applying_--;
    primaryTaken_ = true;
    return primary;
}

void Carets::givePrimary(GdkClipboard* primary) {
    if (!primary) return;
    primaryTaken_ = false;
    if (buffer_) gtk_text_buffer_add_selection_clipboard(buffer_, primary);
}

void Carets::selectInBuffer(const Caret& c) {
    GtkTextIter head, anchor;
    gtk_text_buffer_get_iter_at_offset(buffer_, &head, (int)c.head);
    gtk_text_buffer_get_iter_at_offset(buffer_, &anchor, (int)c.anchor);
    applying_++;
    gtk_text_buffer_select_range(buffer_, &head, &anchor);
    applying_--;
}

void Carets::set(std::vector<Caret> carets) {
    if (!buffer_) return;
    carets = MultiCaret::normalize(std::move(carets), length());
    const bool wasActive = active();
    if (carets.size() < 2) {
        carets_.clear();
        goalX_.clear();
        selectInBuffer(carets[0]);
    } else {
        // The first is the buffer's own selection, made without offering it
        // to the primary clipboard (see applyEdit).
        GdkClipboard* primary = takePrimary();
        carets_ = std::move(carets);
        selectInBuffer(carets_[0]);
        givePrimary(primary);
    }
    if (active() || wasActive) showChanged();
    if (AutoCloseInput* ac = editor_->autoClose()) ac->caretsAre(current());
}

void Carets::clear() {
    if (carets_.empty()) return;
    carets_.clear();
    goalX_.clear();
    showChanged();
}

void Carets::showChanged() {
    if (BufferLines* l = buffer_ ? BufferLines::of(buffer_) : nullptr) {
        if (active())
            l->extraCarets = [this](std::vector<size_t>& out) {
                for (size_t i = 1; i < carets_.size(); i++) out.push_back(carets_[i].head);
            };
        else
            l->extraCarets = nullptr;
        l->changed();
    }
    gtk_widget_queue_draw(view_);
}

void Carets::reveal(size_t offset) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buffer_, &it, (int)offset);
    gtk_text_view_scroll_to_iter(view(), &it, 0.0, FALSE, 0, 0);
}

void Carets::revealCaret() {
    gtk_text_view_scroll_mark_onscreen(view(), gtk_text_buffer_get_insert(buffer_));
}

// Ctrl+Up and Ctrl+Down, VS Code's scrollLineUp and scrollLineDown: the
// view a line up or down, the carets where they are.
void Carets::scrollLines(int count) {
    GtkAdjustment* v = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view_));
    if (!v) return;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(buffer_, &it, gtk_text_buffer_get_insert(buffer_));
    int y = 0, h = 0;
    gtk_text_view_get_line_yrange(view(), &it, &y, &h);
    if (h <= 0) h = 16;
    const double lower = gtk_adjustment_get_lower(v);
    const double upper = gtk_adjustment_get_upper(v) - gtk_adjustment_get_page_size(v);
    gtk_adjustment_set_value(v, std::clamp(gtk_adjustment_get_value(v) + (double)count * h, lower,
                                           std::max(lower, upper)));
}

// ---------------------------------------------------------------- what ends them

void Carets::watchBuffer() {
    GtkTextBuffer* now = gtk_text_view_get_buffer(view());
    if (now == buffer_) return;
    // Held, as FindBar holds it: a tab's buffer can go with its document
    // while the view lets go of it.
    if (buffer_) {
        g_signal_handlers_disconnect_by_data(buffer_, this);
        if (BufferLines* l = BufferLines::of(buffer_)) {
            l->extraCarets = nullptr;
            l->changed();
        }
        g_object_unref(buffer_);
    }
    buffer_ = now;
    carets_.clear();
    goalX_.clear();
    hasAnchor_ = hasFind_ = false;
    press_ = Press::None;
    if (!buffer_) return;
    g_object_ref(buffer_);
    // Any change not made here (an undo, a reload, an input method's text,
    // another window's Replace in Folder) and any caret GTK moves (a click,
    // Page Down, Select All, Find) ends the carets, as on the Mac.
    g_signal_connect(buffer_, "insert-text", G_CALLBACK(onInsert), this);
    g_signal_connect(buffer_, "delete-range", G_CALLBACK(onDelete), this);
    g_signal_connect_after(buffer_, "mark-set", G_CALLBACK(onMarkSet), this);
}

void Carets::onBuffer(GObject*, GParamSpec*, gpointer self) {
    static_cast<Carets*>(self)->watchBuffer();
}

void Carets::onInsert(GtkTextBuffer*, GtkTextIter*, char*, int, gpointer p) {
    Carets* self = static_cast<Carets*>(p);
    if (!self->applying() && self->active()) self->clear();
}

void Carets::onDelete(GtkTextBuffer*, GtkTextIter*, GtkTextIter*, gpointer p) {
    Carets* self = static_cast<Carets*>(p);
    if (!self->applying() && self->active()) self->clear();
}

void Carets::onMarkSet(GtkTextBuffer* b, GtkTextIter*, GtkTextMark* mark, gpointer p) {
    Carets* self = static_cast<Carets*>(p);
    if (self->applying() || !self->active()) return;
    if (mark == gtk_text_buffer_get_insert(b) || mark == gtk_text_buffer_get_selection_bound(b))
        self->clear();
}

// ---------------------------------------------------------------- keys

bool Carets::keyPressed(guint key, GdkModifierType mods) {
    if (!active()) return false;
    if (!applies()) {
        clear();
        return false;
    }
    // An input method composing keeps its keys; what it commits goes in at
    // the first caret and ends the others (onInsert).
    if (editor_->composing()) return false;
    if (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) return false;
    const bool shift = mods & GDK_SHIFT_MASK;
    switch (key) {
    case GDK_KEY_Escape:
        if (shift) return false;
        collapse();
        return true;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter:
        newline();
        return true;
    case GDK_KEY_Tab:
    case GDK_KEY_KP_Tab:
        if (!shift) tab();
        return true;
    case GDK_KEY_ISO_Left_Tab:
        return true;
    case GDK_KEY_BackSpace:
        backspace();
        return true;
    default:
        break;
    }
    const gunichar c = gdk_keyval_to_unicode(key);
    if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0)) return false;
    typeChar(c);
    return true;
}

// The moves GTK's key bindings make: at every caret while there are
// several; with one, only the word moves (VS Code's words) and Ctrl+Up and
// Ctrl+Down (a line's scroll), in source text. A Markdown page, a message
// or the welcome text keep GTK's.
void Carets::onMoveCursor(GtkTextView* v, GtkMovementStep step, int count, gboolean extend,
                          gpointer p) {
    Carets* self = static_cast<Carets*>(p);
    if (!self->inSource() || count == 0) return;
    const int n = std::abs(count), dir = count > 0 ? 1 : -1;
    if (step == GTK_MOVEMENT_PARAGRAPHS) {
        if (extend) return;
        self->scrollLines(count);
        g_signal_stop_emission_by_name(v, "move-cursor");
        return;
    }
    if (!self->active()) {
        if (step != GTK_MOVEMENT_WORDS) return;
        for (int i = 0; i < n; i++) self->moveAll(dir > 0 ? Move::WordRight : Move::WordLeft, extend);
        g_signal_stop_emission_by_name(v, "move-cursor");
        return;
    }
    switch (step) {
    case GTK_MOVEMENT_LOGICAL_POSITIONS:
    case GTK_MOVEMENT_VISUAL_POSITIONS:
        for (int i = 0; i < n; i++) self->moveAll(dir > 0 ? Move::Right : Move::Left, extend);
        break;
    case GTK_MOVEMENT_WORDS:
        for (int i = 0; i < n; i++) self->moveAll(dir > 0 ? Move::WordRight : Move::WordLeft, extend);
        break;
    case GTK_MOVEMENT_DISPLAY_LINES:
        for (int i = 0; i < n; i++) self->moveVertically(dir, extend);
        break;
    case GTK_MOVEMENT_DISPLAY_LINE_ENDS:
    case GTK_MOVEMENT_PARAGRAPH_ENDS:
        self->moveAll(dir > 0 ? Move::LineEnd : Move::LineStart, extend);
        break;
    case GTK_MOVEMENT_BUFFER_ENDS:
        self->moveAll(dir > 0 ? Move::DocumentEnd : Move::DocumentStart, extend);
        break;
    default:
        // Page Up and Down and the like: from the first caret, as GTK does
        // with one.
        self->collapse();
        return;
    }
    g_signal_stop_emission_by_name(v, "move-cursor");
}

// Ctrl+Backspace and Ctrl+Delete by VS Code's words, with one caret or
// several; Delete and the line-end deletes at every caret.
void Carets::onDeleteFrom(GtkTextView* v, GtkDeleteType type, int count, gpointer p) {
    Carets* self = static_cast<Carets*>(p);
    if (!self->applies() || count == 0 || self->editor_->composing()) return;
    if (type == GTK_DELETE_WORD_ENDS || type == GTK_DELETE_WORDS) {
        self->deleteBy(count > 0 ? Move::WordRight : Move::WordLeft);
    } else if (!self->active()) {
        return;
    } else if (type == GTK_DELETE_CHARS) {
        if (count > 0) self->deleteForward();
        else self->backspace();
    } else if (type == GTK_DELETE_DISPLAY_LINE_ENDS || type == GTK_DELETE_PARAGRAPH_ENDS) {
        self->deleteBy(count > 0 ? Move::LineEnd : Move::LineStart);
    } else {
        self->collapse();
        return;
    }
    g_signal_stop_emission_by_name(v, "delete-from-cursor");
}

void Carets::onCopy(GtkTextView* v, gpointer p) {
    if (static_cast<Carets*>(p)->copy(false)) g_signal_stop_emission_by_name(v, "copy-clipboard");
}

void Carets::onCut(GtkTextView* v, gpointer p) {
    if (static_cast<Carets*>(p)->copy(true)) g_signal_stop_emission_by_name(v, "cut-clipboard");
}

void Carets::onPaste(GtkTextView* v, gpointer p) {
    if (static_cast<Carets*>(p)->paste()) g_signal_stop_emission_by_name(v, "paste-clipboard");
}

// ---------------------------------------------------------------- geometry

// The insertion point nearest (bx, by), in buffer coordinates: never past
// the end of its line.
size_t Carets::offsetAt(int bx, int by) const {
    GtkTextIter it;
    int trailing = 0;
    gtk_text_view_get_iter_at_position(view(), &it, &trailing, bx, by);
    GtkTextIter end = it;
    if (!gtk_text_iter_ends_line(&end)) gtk_text_iter_forward_to_line_end(&end);
    gtk_text_iter_forward_chars(&it, trailing);
    return std::min(offsetOf(&it), offsetOf(&end));
}

int Carets::xOf(size_t offset) const {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buffer_, &it, (int)offset);
    GdkRectangle strong;
    gtk_text_view_get_cursor_locations(view(), &it, &strong, nullptr);
    return strong.x;
}

// Where `offset` goes a display line up (direction < 0) or down at x, or -1
// when there is no line there. GTK lays out the lines it is asked about.
long Carets::acrossLine(size_t offset, int dir, int x) const {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buffer_, &it, (int)offset);
    GdkRectangle box;
    gtk_text_view_get_cursor_locations(view(), &it, &box, nullptr);
    if (box.height <= 0) box.height = 1;
    // The caret is a little shorter than its line (the line's spacing), so
    // down is the first y past it that lands on another display line; past
    // the last line every y lands on that line, and there is none.
    for (int step = 0; step <= box.height; step++) {
        const int y = dir > 0 ? box.y + box.height + step : box.y - 1 - step;
        if (y < 0) return -1;
        const size_t p = offsetAt(x, y);
        GtkTextIter at;
        gtk_text_buffer_get_iter_at_offset(buffer_, &at, (int)p);
        GdkRectangle got;
        gtk_text_view_get_cursor_locations(view(), &at, &got, nullptr);
        if (dir > 0 ? got.y > box.y : got.y < box.y) return (long)p;
    }
    return -1;
}

// The carets of a column from (x0 on a0's line) to (x1 on a1's line): one
// per line, from the character at x0 to the one at x1, each clamped to its
// line; a line too short for either gets a caret at its end.
std::vector<Caret> Carets::column(size_t a0, int x0, size_t a1, int x1) const {
    GtkTextIter i0, i1;
    gtk_text_buffer_get_iter_at_offset(buffer_, &i0, (int)std::min(a0, a1));
    gtk_text_buffer_get_iter_at_offset(buffer_, &i1, (int)std::max(a0, a1));
    const int first = gtk_text_iter_get_line(&i0), last = gtk_text_iter_get_line(&i1);
    std::vector<Caret> out;
    out.reserve((size_t)(last - first + 1));
    for (int line = first; line <= last; line++) {
        GtkTextIter ls;
        gtk_text_buffer_get_iter_at_line(buffer_, &ls, line);
        GdkRectangle box;
        gtk_text_view_get_iter_location(view(), &ls, &box);
        const int y = box.y + std::max(box.height, 1) / 2;
        out.push_back({offsetAt(x0, y), offsetAt(x1, y)});
    }
    return out;
}

// ---------------------------------------------------------------- the mouse

void Carets::onPressed(GtkGestureClick* g, int, double x, double y, gpointer p) {
    Carets* self = static_cast<Carets*>(p);
    self->press_ = Press::None;
    const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    // The clicks are counted here, as GTK counts them (its double-click
    // time and distance): the gesture's own count starts again whenever the
    // text view's gestures claim a press, so a double-click right after
    // one this took came as two single clicks (the harness suite's).
    const guint32 time = gtk_event_controller_get_current_event_time(GTK_EVENT_CONTROLLER(g));
    int doubleTime = 400, doubleDistance = 5;
    g_object_get(gtk_widget_get_settings(self->view_), "gtk-double-click-time", &doubleTime,
                 "gtk-double-click-distance", &doubleDistance, nullptr);
    const bool again = self->clicks_ > 0 && button == self->clickButton_ &&
                       time - self->clickTime_ <= (guint32)doubleTime &&
                       std::fabs(x - self->clickX_) <= doubleDistance &&
                       std::fabs(y - self->clickY_) <= doubleDistance;
    self->clicks_ = again ? self->clicks_ + 1 : 1;
    self->clickButton_ = button;
    self->clickTime_ = time;
    self->clickX_ = x;
    self->clickY_ = y;
    const int n = self->clicks_;
    if (!self->inSource()) return;   // a Markdown page keeps GTK's words
    // Over the text, not the gutter (a child of the view, under it here).
    if (gtk_widget_pick(self->view_, x, y, GTK_PICK_DEFAULT) != self->view_) return;
    const GdkModifierType mods =
        (GdkModifierType)(gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g)) &
                          (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK));
    int bx = 0, by = 0;
    gtk_text_view_window_to_buffer_coords(self->view(), GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y,
                                          &bx, &by);
    const bool editable = self->applies();
    Press press = Press::None;
    if (button == GDK_BUTTON_PRIMARY && mods == 0 && n == 2) press = Press::Word;
    else if (button == GDK_BUTTON_PRIMARY && mods == 0 && n >= 3) press = Press::Line;
    else if (button == GDK_BUTTON_PRIMARY && mods == GDK_ALT_MASK && editable) press = Press::AltClick;
    else if (button == GDK_BUTTON_PRIMARY && mods == (GDK_ALT_MASK | GDK_SHIFT_MASK) && editable)
        press = Press::Column;
    else if (button == GDK_BUTTON_MIDDLE && mods == 0 && editable) press = Press::Middle;
    if (press == Press::None) return;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    if (!gtk_widget_has_focus(self->view_)) gtk_widget_grab_focus(self->view_);
    self->press_ = press;
    self->pressWx_ = x;
    self->pressWy_ = y;
    self->pressX_ = bx;
    self->pressY_ = by;
    self->dragged_ = false;
    self->pressIndex_ = self->offsetAt(bx, by);

    if (press == Press::Word || press == Press::Line) {
        // The character the press is on (its index, not the nearest gap).
        GtkTextIter on;
        gtk_text_view_get_iter_at_location(self->view(), &on, bx, by);
        CaretText reader(self->buffer_);
        const MultiCaret::Text t = reader.text();
        Caret w;
        if (press == Press::Word) {
            w = MultiCaret::wordSelection(t, offsetOf(&on));
        } else {
            // VS Code's triple click: the whole line with its break.
            w = MultiCaret::expandLineSelection(t, {{offsetOf(&on), offsetOf(&on)}})[0];
        }
        self->wordAnchor_ = w;
        self->set({w});
        return;
    }
    if (press == Press::Column) {
        // VS Code's box selection: from the caret (its selection's start),
        // or from where the last column began while that column holds.
        const bool extending = self->hasAnchor_ && self->anchorLength_ == self->length() &&
                               self->current() == self->anchorCarets_;
        if (extending) {
            self->colAnchor_ = self->anchorIndex_;
            self->colAnchorX_ = self->anchorX_;
        } else {
            self->colAnchor_ = std::min(self->current().front().anchor, self->length());
            self->colAnchorX_ = self->xOf(self->colAnchor_);
        }
        self->set(self->column(self->colAnchor_, self->colAnchorX_, self->pressIndex_, bx));
        return;
    }
    // Alt and the middle button wait to see whether the press drags.
    if (n > 1 && press == Press::AltClick) self->press_ = Press::None;   // a quick second press there
}

void Carets::onDragUpdate(GtkGestureDrag*, double dx, double dy, gpointer p) {
    Carets* self = static_cast<Carets*>(p);
    if (self->press_ == Press::None || !self->buffer_) return;
    int bx = 0, by = 0;
    gtk_text_view_window_to_buffer_coords(self->view(), GTK_TEXT_WINDOW_WIDGET,
                                          (int)(self->pressWx_ + dx), (int)(self->pressWy_ + dy),
                                          &bx, &by);
    if (!self->dragged_ && std::fabs(dx) < 3 && std::fabs(dy) < 3) return;
    self->dragged_ = true;
    switch (self->press_) {
    case Press::Word:
    case Press::Line: {
        // Whole words (or lines) from the one pressed to the one under the
        // pointer, either way.
        GtkTextIter on;
        gtk_text_view_get_iter_at_location(self->view(), &on, bx, by);
        CaretText reader(self->buffer_);
        const MultiCaret::Text t = reader.text();
        const Caret w = self->press_ == Press::Word
                            ? MultiCaret::wordSelection(t, offsetOf(&on))
                            : MultiCaret::expandLineSelection(t, {{offsetOf(&on), offsetOf(&on)}})[0];
        const Caret& a = self->wordAnchor_;
        if (w.lo() < a.lo()) self->set({{a.hi(), w.lo()}});
        else self->set({{a.lo(), std::max(w.hi(), a.hi())}});
        break;
    }
    case Press::AltClick:
    case Press::Middle:
        // A drag: a fresh column from the press.
        self->colAnchor_ = self->pressIndex_;
        self->colAnchorX_ = self->pressX_;
        self->press_ = Press::Column;
        [[fallthrough]];
    case Press::Column:
        self->set(self->column(self->colAnchor_, self->colAnchorX_, self->offsetAt(bx, by), bx));
        break;
    default:
        break;
    }
}

void Carets::onDragEnd(GtkGestureDrag*, double, double, gpointer p) {
    Carets* self = static_cast<Carets*>(p);
    const Press press = self->press_;
    self->press_ = Press::None;
    if (press == Press::None || !self->buffer_) return;
    if (press == Press::Column) {
        // Remembered, so a Shift+Alt press can stretch it.
        self->hasAnchor_ = true;
        self->anchorIndex_ = self->colAnchor_;
        self->anchorX_ = self->colAnchorX_;
        self->anchorLength_ = self->length();
        self->anchorCarets_ = self->current();
        return;
    }
    if (press == Press::AltClick) {
        // A click: a caret there, or one taken away.
        std::vector<Caret> carets = self->current();
        const size_t at = self->pressIndex_;
        for (size_t i = 0; i < carets.size(); i++) {
            if (carets[i].empty() && carets[i].head == at) {
                if (carets.size() > 1) {
                    carets.erase(carets.begin() + (long)i);
                    self->set(carets);
                }
                return;
            }
        }
        if (carets.size() >= kMaxCarets) {
            if (self->onNote)
                self->onNote("The number of cursors has been limited to " + std::to_string(kMaxCarets));
            return;
        }
        carets.push_back({at, at});
        self->set(carets);
        return;
    }
    if (press == Press::Middle) {
        // A middle click that did not drag pastes the primary selection
        // there, as GTK's own would (gtk-enable-primary-paste).
        gboolean paste = TRUE;
        g_object_get(gtk_widget_get_settings(self->view_), "gtk-enable-primary-paste", &paste, nullptr);
        if (!paste) return;
        self->clear();
        GtkTextIter at;
        gtk_text_buffer_get_iter_at_offset(self->buffer_, &at, (int)self->pressIndex_);
        gtk_text_buffer_paste_clipboard(self->buffer_, gtk_widget_get_primary_clipboard(self->view_),
                                        &at, gtk_text_view_get_editable(self->view()));
    }
}

// ---------------------------------------------------------------- drawing

// The characters of the lines on screen.
std::pair<size_t, size_t> Carets::visibleChars(const GdkRectangle& visible) const {
    GtkTextIter top, bottom;
    gtk_text_view_get_line_at_y(view(), &top, visible.y, nullptr);
    gtk_text_view_get_line_at_y(view(), &bottom, visible.y + visible.height, nullptr);
    if (!gtk_text_iter_ends_line(&bottom)) gtk_text_iter_forward_to_line_end(&bottom);
    return {offsetOf(&top), offsetOf(&bottom)};
}

// The extra carets' selections, under the text, in the selection's color
// (GTK draws the first's): a box a line, to the line's end and a little
// past it where the selection takes the line's break too; a wrapped line's
// rows each get theirs.
void Carets::drawSelections(GtkSnapshot* s, const GdkRectangle& visible) {
    drawnSelections_ = 0;
    if (!active() || !buffer_ || gtk_text_view_get_buffer(view()) != buffer_) return;
    const auto [lo, hi] = visibleChars(visible);
    GdkRGBA color = toGdk(theme::editorSelection());
    if (!gtk_widget_has_focus(view_)) color.alpha *= 0.6f;
    const int right = visible.x + visible.width;
    auto box = [&](int x0, int y, int x1, int h) {
        if (x1 <= x0 || h <= 0) return;
        const graphene_rect_t r = GRAPHENE_RECT_INIT((float)x0, (float)y, (float)(x1 - x0), (float)h);
        gtk_snapshot_append_color(s, &color, &r);
    };
    auto first = std::lower_bound(carets_.begin() + 1, carets_.end(), lo,
                                  [](const Caret& c, size_t at) { return c.hi() < at; });
    for (auto it = first; it != carets_.end() && it->lo() <= hi; ++it) {
        if (it->empty()) continue;
        drawnSelections_++;
        const size_t to = std::min(it->hi(), hi);
        GtkTextIter a;
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)std::max(it->lo(), lo));
        for (;;) {
            GtkTextIter end = a;
            if (!gtk_text_iter_ends_line(&end)) gtk_text_iter_forward_to_line_end(&end);
            const size_t segEnd = std::min(offsetOf(&end), it->hi());
            const bool goesOn = it->hi() > offsetOf(&end);   // the line's break is selected
            GtkTextIter b, start = a;
            gtk_text_buffer_get_iter_at_offset(buffer_, &b, (int)segEnd);
            gtk_text_iter_set_line_offset(&start, 0);
            GdkRectangle ra, rb, rs;
            gtk_text_view_get_iter_location(view(), &a, &ra);
            gtk_text_view_get_iter_location(view(), &b, &rb);
            gtk_text_view_get_iter_location(view(), &start, &rs);
            const int x1 = rb.x + (goesOn ? std::max(rb.width, 7) : 0);
            if (ra.y == rb.y) {
                box(ra.x, ra.y, x1, ra.height);
            } else {
                box(ra.x, ra.y, right, ra.height);
                box(rs.x, ra.y + ra.height, right, rb.y - ra.y - ra.height);
                box(rs.x, rb.y, x1, rb.height);
            }
            if (!goesOn || segEnd >= to) break;
            if (!gtk_text_iter_forward_line(&a) || offsetOf(&a) > to) break;
        }
    }
}

// The extra carets, over the text, in the text's color as GTK draws its
// caret, dimmer while the view does not have the keyboard.
void Carets::drawCarets(GtkSnapshot* s, const GdkRectangle& visible) {
    drawnCarets_.clear();
    if (!active() || !buffer_ || gtk_text_view_get_buffer(view()) != buffer_) return;
    const auto [lo, hi] = visibleChars(visible);
    GdkRGBA color;
    gtk_widget_get_color(view_, &color);
    if (!gtk_widget_has_focus(view_)) color.alpha *= 0.45f;
    auto first = std::lower_bound(carets_.begin() + 1, carets_.end(), lo,
                                  [](const Caret& c, size_t at) { return c.head < at; });
    for (auto it = first; it != carets_.end() && it->head <= hi; ++it) {
        GtkTextIter at;
        gtk_text_buffer_get_iter_at_offset(buffer_, &at, (int)it->head);
        GdkRectangle box;
        gtk_text_view_get_cursor_locations(view(), &at, &box, nullptr);
        if (box.y + box.height < visible.y || box.y > visible.y + visible.height) continue;
        // GTK's own width: a pixel, and more for a tall line
        // (gtk-cursor-aspect-ratio, 0.04).
        const float w = std::max(1.0f, std::floor((float)box.height * 0.04f + 1.0f));
        const graphene_rect_t r =
            GRAPHENE_RECT_INIT((float)box.x, (float)box.y, w, (float)box.height);
        gtk_snapshot_append_color(s, &color, &r);
        drawnCarets_.push_back(it->head);
    }
}
