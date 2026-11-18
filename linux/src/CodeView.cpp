// CodeView.cpp — see CodeView.h.
#include "CodeView.h"

#include "Gutter.h"
#include "Settings.h"

#include <algorithm>
#include <cmath>
#include <string>

// ---------------------------------------------------------------- the widget
//
// A GtkTextView whose snapshot_layer and size_allocate are the CodeView's.
// The view owns its CodeView and deletes it first thing in dispose, before
// GtkTextView's own dispose takes the gutter away.

G_DECLARE_FINAL_TYPE(MutibleCodeView, mutible_code_view, MUTIBLE, CODE_VIEW, GtkTextView)

struct _MutibleCodeView {
    GtkTextView parent_instance;
    CodeView*   owner;
};

G_DEFINE_FINAL_TYPE(MutibleCodeView, mutible_code_view, GTK_TYPE_TEXT_VIEW)

static void mutible_code_view_snapshot_layer(GtkTextView* tv, GtkTextViewLayer layer,
                                              GtkSnapshot* s) {
    MutibleCodeView* self = MUTIBLE_CODE_VIEW(tv);
    if (self->owner) self->owner->snapshotLayer(layer, s);
}

static void mutible_code_view_size_allocate(GtkWidget* w, int width, int height, int baseline) {
    GTK_WIDGET_CLASS(mutible_code_view_parent_class)->size_allocate(w, width, height, baseline);
    MutibleCodeView* self = MUTIBLE_CODE_VIEW(w);
    if (self->owner) self->owner->allocated();
}

static void mutible_code_view_dispose(GObject* o) {
    MutibleCodeView* self = MUTIBLE_CODE_VIEW(o);
    CodeView* owner = self->owner;
    self->owner = nullptr;
    delete owner;
    G_OBJECT_CLASS(mutible_code_view_parent_class)->dispose(o);
}

static void mutible_code_view_class_init(MutibleCodeViewClass* klass) {
    G_OBJECT_CLASS(klass)->dispose = mutible_code_view_dispose;
    GTK_WIDGET_CLASS(klass)->size_allocate = mutible_code_view_size_allocate;
    GTK_TEXT_VIEW_CLASS(klass)->snapshot_layer = mutible_code_view_snapshot_layer;
}

static void mutible_code_view_init(MutibleCodeView* self) { self->owner = nullptr; }

namespace {
GdkRGBA toGdk(const Rgba& c) {
    return GdkRGBA{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (float)c.a};
}
}  // namespace

// ---------------------------------------------------------------- life

CodeView* CodeView::create(GtkTextBuffer* buffer) {
    GtkWidget* view = GTK_WIDGET(g_object_new(mutible_code_view_get_type(), "buffer", buffer,
                                              nullptr));
    CodeView* cv = new CodeView(view);
    MUTIBLE_CODE_VIEW(view)->owner = cv;
    return cv;
}

CodeView* CodeView::of(GtkWidget* view) {
    if (!view || !G_TYPE_CHECK_INSTANCE_TYPE(view, mutible_code_view_get_type())) return nullptr;
    return MUTIBLE_CODE_VIEW(view)->owner;
}

CodeView::CodeView(GtkWidget* view) : view_(view) {
    gutter_ = std::make_unique<Gutter>(GTK_TEXT_VIEW(view_));
    addPass(Layer::BelowText, 0, [this](GtkSnapshot* s, const GdkRectangle& visible) {
        drawRulers(s, visible);
    });
}

CodeView::~CodeView() { gutter_.reset(); }

// ---------------------------------------------------------------- the buffer

void CodeView::setBuffer(GtkTextBuffer* buffer) { setViewBuffer(textView(), buffer); }

void CodeView::setViewBuffer(GtkTextView* view, GtkTextBuffer* buffer) {
    GtkTextBuffer* old = gtk_text_view_get_buffer(view);
    if (old == buffer) return;
    // The view goes to its top before it takes the new buffer, which it
    // then shows from its start. GtkTextView's scroll offset is the view's,
    // not the buffer's: set_buffer puts the view's first line at the new
    // buffer's start but keeps the offset and leaves the adjustments as they
    // were. Far down a long file or a Markdown page, GTK then set the
    // adjustment to 0 for the new text before laying any of it out, and an
    // unlaid buffer's lines have no height, so it took the view's first line
    // to be the last one; laid out, that line moved down and GTK moved the
    // offset after it, asking the adjustment for a value it clamped back to
    // the 0 it had, so value-changed, the one thing that puts the offset back
    // in step, never came. A short file opened next showed its empty last
    // line or nothing at all, the adjustment at 0 and the view past the text
    // (`symbols` then `brackets` in the nightly soak; `gutter_switch` checks
    // it). At 0 first, while the old text is laid out, there is nothing for
    // GTK to move. A tab's own place is put back after this
    // (Editor::restorePlace), which is captured before (Editor::stash).
    // Before the flush below, never after it: scrolling the old buffer lays
    // its lines out again into the layout's line cache, and lines of a buffer
    // the view no longer shows crashed the next draw
    // ("gtk_text_btree_line_number couldn't find line", semantic_tokens
    // closing its tab, main's GTK job after #165).
    GtkAdjustment* adjs[] = {gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(view)),
                             gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view))};
    if (old)
        for (GtkAdjustment* a : adjs)
            if (a) gtk_adjustment_set_value(a, 0);
    if (old) {
        static const char kFlush[] = "minicode-layout-flush";
        GtkTextTagTable* table = gtk_text_buffer_get_tag_table(old);
        GtkTextTag* tag = gtk_text_tag_table_lookup(table, kFlush);
        if (!tag) {
            tag = gtk_text_tag_new(kFlush);   // never applied to any text
            gtk_text_tag_table_add(table, tag);
            g_object_unref(tag);
        }
        gtk_text_tag_changed(tag, FALSE);
    }
    gtk_text_view_set_buffer(view, buffer);
}

// ---------------------------------------------------------------- passes

int CodeView::addPass(Layer layer, int order, Pass pass) {
    Entry e{++nextId_, layer, order, std::move(pass)};
    auto at = std::upper_bound(passes_.begin(), passes_.end(), e, [](const Entry& a, const Entry& b) {
        return a.layer != b.layer ? a.layer < b.layer : a.order < b.order;
    });
    passes_.insert(at, std::move(e));
    queueDraw();
    return nextId_;
}

void CodeView::removePass(int id) {
    passes_.erase(std::remove_if(passes_.begin(), passes_.end(),
                                 [id](const Entry& e) { return e.id == id; }),
                  passes_.end());
    queueDraw();
}

void CodeView::snapshotLayer(GtkTextViewLayer layer, GtkSnapshot* s) {
    const Layer want = layer == GTK_TEXT_VIEW_LAYER_BELOW_TEXT ? Layer::BelowText : Layer::AboveText;
    GdkRectangle visible;
    gtk_text_view_get_visible_rect(textView(), &visible);
    // A copy: a pass may add or remove passes.
    const std::vector<Entry> passes = passes_;
    for (const Entry& e : passes)
        if (e.layer == want) e.pass(s, visible);
}

// The view's width changed (the pane, the gutter's digits): lines move, and
// the gutter, which the view only reallocates, draws again.
void CodeView::allocated() {
    if (gutter_) gutter_->queueDraw();
}

// ---------------------------------------------------------------- settings

void CodeView::applySettings(const Settings& s) {
    setRulers(s.rulers(), toGdk(s.rulerColor()));
    gutter_->applySettings(s);
}

void CodeView::setRulers(std::vector<int> columns, const GdkRGBA& color) {
    rulers_ = std::move(columns);
    rulerColor_ = color;
    queueDraw();
}

// ---------------------------------------------------------------- the ruler

double CodeView::columnX(int column) const {
    const double left = gtk_text_view_get_left_margin(textView());
    if (column <= 0) return left;
    const std::string zeros((size_t)column, '0');
    PangoLayout* layout = gtk_widget_create_pango_layout(view_, zeros.c_str());
    PangoRectangle logical;
    pango_layout_get_extents(layout, nullptr, &logical);
    g_object_unref(layout);
    return left + (double)logical.width / PANGO_SCALE;
}

// Drawn with the background, under the text and the selection, as the Mac
// draws it in drawViewBackgroundInRect:.
void CodeView::drawRulers(GtkSnapshot* s, const GdkRectangle& visible) {
    rulerXs_.clear();
    if (rulers_.empty() || !gtk_text_view_get_editable(textView())) return;
    const double scale = gtk_widget_get_scale_factor(view_);
    for (int column : rulers_) {
        // On the device pixel grid, a logical pixel wide.
        const double x = std::round(columnX(column) * scale) / scale;
        if (x + 1 < visible.x || x > visible.x + visible.width) continue;
        const graphene_rect_t line =
            GRAPHENE_RECT_INIT((float)x, (float)visible.y, 1.0f, (float)visible.height);
        gtk_snapshot_append_color(s, &rulerColor_, &line);
        rulerXs_.push_back(x);
    }
}
