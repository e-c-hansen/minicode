// BracketView.cpp — see BracketView.h.
#include "BracketView.h"

#include "BracketMatch.h"
#include "BufferText.h"
#include "CodeView.h"
#include "Document.h"
#include "Editor.h"
#include "Settings.h"

#include <algorithm>
#include <string>

struct BracketView::Window {
    size_t lo = 0;
    std::u16string text;
    std::vector<Brackets::Span> skip;
};

namespace {

GdkRGBA toGdk(const Rgba& c) {
    return GdkRGBA{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (float)c.a};
}

bool bracketAt(const GtkTextIter* it) {
    const gunichar c = gtk_text_iter_get_char(it);
    return c < 0x80 && Brackets::isBracket((char16_t)c);
}

}  // namespace

BracketView::BracketView(Editor* editor) : editor_(editor), view_(editor->textView()) {
    pass_ = editor_->codeView()->addPass(CodeView::Layer::BelowText, 10,
                                         [this](GtkSnapshot* s, const GdkRectangle& visible) {
                                             draw(s, visible);
                                         });
    g_signal_connect(view_, "notify::buffer", G_CALLBACK(onBuffer), this);
    g_signal_connect(view_, "notify::editable", G_CALLBACK(onBuffer), this);
    g_signal_connect(view_, "notify::vadjustment", G_CALLBACK(onAdjustment), this);
    watchBuffer();
    watchAdjustment();
}

BracketView::~BracketView() {
    if (idle_) g_source_remove(idle_);
    idle_ = 0;
    if (buffer_) g_signal_handlers_disconnect_by_data(buffer_, this);
    if (adjustment_) {
        g_signal_handlers_disconnect_by_data(adjustment_, this);
        g_object_unref(adjustment_);
    }
    g_signal_handlers_disconnect_by_data(view_, this);
    if (CodeView* cv = CodeView::of(view_)) cv->removePass(pass_);
}

void BracketView::applySettings(const Settings& s) {
    color_ = toGdk(s.bracketMatchColor());
    if (!boxed_.empty()) gtk_widget_queue_draw(view_);
}

// ---------------------------------------------------------------- watching

void BracketView::watchBuffer() {
    GtkTextBuffer* now = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_));
    if (now != buffer_) {
        if (buffer_) g_signal_handlers_disconnect_by_data(buffer_, this);
        buffer_ = now;
        if (buffer_) {
            g_signal_connect_after(buffer_, "mark-set", G_CALLBACK(onMarkSet), this);
            g_signal_connect(buffer_, "changed", G_CALLBACK(onChanged), this);
        }
    }
    scheduleUpdate();
}

void BracketView::watchAdjustment() {
    GtkAdjustment* now = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view_));
    if (now == adjustment_) return;
    if (adjustment_) {
        g_signal_handlers_disconnect_by_data(adjustment_, this);
        g_object_unref(adjustment_);
    }
    adjustment_ = now;
    if (!adjustment_) return;
    g_object_ref(adjustment_);
    // A scroll, or the pane's size (the page size): the lines on screen get
    // their bracket colors before they are drawn.
    g_signal_connect(adjustment_, "value-changed", G_CALLBACK(onScrolled), this);
    g_signal_connect(adjustment_, "changed", G_CALLBACK(onScrolled), this);
}

void BracketView::onBuffer(GObject*, GParamSpec*, gpointer self) {
    static_cast<BracketView*>(self)->watchBuffer();
}

void BracketView::onAdjustment(GObject*, GParamSpec*, gpointer self) {
    static_cast<BracketView*>(self)->watchAdjustment();
}

void BracketView::onMarkSet(GtkTextBuffer* b, GtkTextIter*, GtkTextMark* mark, gpointer selfp) {
    BracketView* self = static_cast<BracketView*>(selfp);
    if (mark == gtk_text_buffer_get_insert(b) || mark == gtk_text_buffer_get_selection_bound(b))
        self->scheduleUpdate();
}

void BracketView::onChanged(GtkTextBuffer*, gpointer self) {
    static_cast<BracketView*>(self)->scheduleUpdate();
}

void BracketView::onScrolled(GtkAdjustment*, gpointer self) {
    static_cast<BracketView*>(self)->paintOnScreen();
}

// Once the edit or the selection settles (typing changes both), before the
// view draws.
void BracketView::scheduleUpdate() {
    if (idle_) return;
    idle_ = g_idle_add_full(G_PRIORITY_HIGH_IDLE, onIdle, this, nullptr);
}

gboolean BracketView::onIdle(gpointer selfp) {
    BracketView* self = static_cast<BracketView*>(selfp);
    self->idle_ = 0;
    self->update();
    self->paintOnScreen();
    return G_SOURCE_REMOVE;
}

void BracketView::paintOnScreen() {
    Document* d = editor_->active();
    if (!d || !editor_->isSource()) return;
    d->paintBracketsOnScreen();
    d->paintSemanticOnScreen();   // a server's colors too (SemanticColors.h)
}

// ---------------------------------------------------------------- the match

bool BracketView::applies() const {
    return editor_->isSource() && gtk_text_view_get_editable(GTK_TEXT_VIEW(view_)) && buffer_ &&
           buffer_ == editor_->active()->buffer();
}

bool BracketView::window(size_t caret, Window& w) {
    GtkTextBuffer* b = buffer_;
    const size_t n = (size_t)gtk_text_buffer_get_char_count(b);
    const size_t reach = Brackets::kDefaultScan + 1;
    w.lo = caret > reach ? caret - reach : 0;
    const size_t hi = std::min(n, caret + reach);
    w.text = BufferText::slice(b, w.lo, hi);
    w.skip.clear();
    const std::unique_ptr<AutoClose::Context> ctx = BufferText::context(editor_->active(), b);
    for (const AutoClose::Span& s : ctx->nonCode(w.lo, hi)) {
        const size_t a = s.start - w.lo, e = s.end - w.lo;
        if (!w.skip.empty() && w.skip.back().end >= a) w.skip.back().end = std::max(w.skip.back().end, e);
        else w.skip.push_back({a, e});
    }
    return true;
}

void BracketView::update() {
    updates_++;
    std::vector<size_t> boxes;
    // No boxes in large-file mode, as on the Mac (PARITY.md 1.12); Go to
    // Bracket still goes.
    if (applies() && !editor_->active()->largeFileFeaturesOff() &&
        !gtk_text_buffer_get_has_selection(buffer_)) {
        GtkTextIter at;
        gtk_text_buffer_get_iter_at_mark(buffer_, &at, gtk_text_buffer_get_insert(buffer_));
        GtkTextIter before = at;
        const bool near = bracketAt(&at) || (gtk_text_iter_backward_char(&before) && bracketAt(&before));
        if (near) {
            lookups_++;
            const size_t caret = (size_t)gtk_text_iter_get_offset(&at);
            Window w;
            window(caret, w);
            const Brackets::Pair p =
                Brackets::match(w.text.data(), w.text.size(), caret - w.lo, w.skip);
            if (p.found()) boxes = {w.lo + p.open, w.lo + p.close};
        }
    }
    setBoxed(std::move(boxes));
}

void BracketView::setBoxed(std::vector<size_t> boxed) {
    if (boxed == boxed_) return;
    boxed_ = std::move(boxed);
    gtk_widget_queue_draw(view_);
}

bool BracketView::goToBracket() {
    if (!applies()) return false;
    GtkTextIter s, e;
    gtk_text_buffer_get_selection_bounds(buffer_, &s, &e);
    const size_t caret = (size_t)std::max(gtk_text_iter_get_offset(&s), gtk_text_iter_get_offset(&e));
    Window w;
    window(caret, w);
    const size_t to = Brackets::jumpTarget(w.text.data(), w.text.size(), caret - w.lo, w.skip);
    if (to == Brackets::npos) return false;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buffer_, &it, (int)(w.lo + to));
    gtk_text_buffer_place_cursor(buffer_, &it);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(view_), gtk_text_buffer_get_insert(buffer_));
    return true;
}

// ---------------------------------------------------------------- drawing

// Each box in the visible part: the character's box, a little shorter than
// its line (which holds the spacing below the text), filled at a quarter of
// the color's strength and outlined in it, as the Mac draws it.
void BracketView::draw(GtkSnapshot* s, const GdkRectangle& visible) {
    drawn_.clear();
    if (boxed_.empty() || !buffer_ || !gtk_text_view_get_editable(GTK_TEXT_VIEW(view_))) return;
    const size_t n = (size_t)gtk_text_buffer_get_char_count(buffer_);
    GdkRGBA fill = color_;
    fill.alpha *= 0.25f;
    for (size_t at : boxed_) {
        if (at >= n) continue;
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_offset(buffer_, &it, (int)at);
        GdkRectangle r;
        gtk_text_view_get_iter_location(GTK_TEXT_VIEW(view_), &it, &r);
        if (r.width <= 0 || r.y + r.height < visible.y || r.y > visible.y + visible.height ||
            r.x + r.width < visible.x || r.x > visible.x + visible.width)
            continue;
        // Whole pixels: GTK draws a border inside its rectangle, where the
        // Mac strokes a path half a point in from the glyph's box.
        const float h = std::max(4.0f, (float)r.height - 2.0f);
        const graphene_rect_t box = GRAPHENE_RECT_INIT((float)r.x, (float)r.y, (float)r.width, h);
        gtk_snapshot_append_color(s, &fill, &box);
        GskRoundedRect rr;
        gsk_rounded_rect_init_from_rect(&rr, &box, 0);
        const float widths[4] = {1, 1, 1, 1};
        const GdkRGBA colors[4] = {color_, color_, color_, color_};
        gtk_snapshot_append_border(s, &rr, widths, colors);
        drawn_.push_back(r);
    }
}
