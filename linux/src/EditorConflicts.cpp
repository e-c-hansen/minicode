// EditorConflicts.cpp — see EditorConflicts.h.
#include "EditorConflicts.h"

#include "CodeView.h"
#include "DocumentView.h"
#include "Editor.h"
#include "MultiEdit.h"
#include "Settings.h"

#include <algorithm>
#include <cstring>

using MergeConflicts::Block;
using MergeConflicts::Keep;

namespace {

// Past this many characters the text is not read (large-file mode's
// threshold, LargeFile.h, as the Mac's).
const int kMaxConflictText = 20 * 1024 * 1024;

GdkRGBA toGdk(const Rgba& c) {
    return GdkRGBA{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (float)c.a};
}

Keep keepOf(int k) { return k == 1 ? Keep::Incoming : k == 2 ? Keep::Both : Keep::Current; }

// The whole buffer as UTF-8, one character for each of GtkTextIter's (a
// child anchor is U+FFFC), so character counts of it are the buffer's.
std::string wholeText(GtkTextBuffer* b) {
    GtkTextIter a, z;
    gtk_text_buffer_get_bounds(b, &a, &z);
    char* t = gtk_text_buffer_get_slice(b, &a, &z, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

// Characters in bytes [from, to) of UTF-8 text.
size_t charsIn(const std::string& t, size_t from, size_t to) {
    size_t n = 0;
    for (size_t i = from; i < to && i < t.size(); ++i)
        if (((unsigned char)t[i] & 0xC0) != 0x80) ++n;
    return n;
}

// The blocks' byte offsets made characters. Every offset is a line start
// or the text's end, and they only grow through a block and from one block
// to the next, so one pass over the text counts them all.
void toCharacters(const std::string& t, std::vector<Block>& blocks) {
    size_t byte = 0, chars = 0;
    auto conv = [&](size_t& off) {
        if (off == MergeConflicts::npos) return;
        if (off < byte) byte = 0, chars = 0;   // never, as parse gives them
        chars += charsIn(t, byte, off);
        byte = off;
        off = chars;
    };
    for (Block& b : blocks) {
        conv(b.start);
        conv(b.currentStart);
        conv(b.currentEnd);
        conv(b.baseMarker);
        conv(b.baseStart);
        conv(b.baseEnd);
        conv(b.separator);
        conv(b.incomingStart);
        conv(b.incomingEnd);
        conv(b.endMarker);
        conv(b.end);
    }
}

size_t offsetOf(const GtkTextIter* it) { return (size_t)gtk_text_iter_get_offset(it); }

}  // namespace

GdkRGBA EditorConflicts::headerOf(const GdkRGBA& c) {
    GdkRGBA h = c;
    h.alpha = std::min(1.0f, std::max(0.25f, c.alpha * 2.5f));
    return h;
}

EditorConflicts::EditorConflicts(Editor* editor) : editor_(editor), view_(editor->textView()) {
    CodeView* cv = editor_->codeView();
    // Below the bracket boxes (10) and the carets' selection (5): a tint is
    // the line's background.
    behindPass_ = cv->addPass(CodeView::Layer::BelowText, 2,
                              [this](GtkSnapshot* s, const GdkRectangle& v) { drawBehind(s, v); });
    overPass_ = cv->addPass(CodeView::Layer::AboveText, 30,
                            [this](GtkSnapshot* s, const GdkRectangle& v) { drawOver(s, v); });
    // A press on a button, in the capture phase, so the text view's own
    // selection gesture never sees it.
    click_ = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click_), GTK_PHASE_CAPTURE);
    g_signal_connect(click_, "pressed", G_CALLBACK(onPressed), this);
    gtk_widget_add_controller(view_, GTK_EVENT_CONTROLLER(click_));
    g_signal_connect_swapped(view_, "notify::buffer", G_CALLBACK(+[](gpointer self) {
        static_cast<EditorConflicts*>(self)->watchBuffer();
    }), this);
    // Shown as source (or no longer): read once the text that goes with it
    // is in.
    g_signal_connect_swapped(view_, "notify::editable", G_CALLBACK(+[](gpointer selfp) {
        EditorConflicts* self = static_cast<EditorConflicts*>(selfp);
        if (!self->idle_)
            self->idle_ = g_idle_add([](gpointer p) -> gboolean {
                EditorConflicts* s = static_cast<EditorConflicts*>(p);
                s->idle_ = 0;
                s->rescanNow();
                return G_SOURCE_REMOVE;
            }, self);
    }), this);
    watchBuffer();
}

EditorConflicts::~EditorConflicts() {
    if (timer_) g_source_remove(timer_);
    if (idle_) g_source_remove(idle_);
    timer_ = idle_ = 0;
    if (buffer_) {
        g_signal_handlers_disconnect_by_data(buffer_, this);
        g_object_unref(buffer_);
        buffer_ = nullptr;
    }
    g_signal_handlers_disconnect_by_data(view_, this);
    if (click_) gtk_widget_remove_controller(view_, GTK_EVENT_CONTROLLER(click_));
    if (CodeView* cv = CodeView::of(view_)) {
        cv->removePass(behindPass_);
        cv->removePass(overPass_);
    }
}

void EditorConflicts::applySettings(const Settings& s) {
    current_ = toGdk(s.mergeCurrentColor());
    incoming_ = toGdk(s.mergeIncomingColor());
    background_ = toGdk(s.background(Surface::Editor));
    background_.alpha = 1;
    ink_ = toGdk(s.text(Surface::Editor));
    if (!blocks_.empty()) gtk_widget_queue_draw(view_);
}

std::vector<std::string> EditorConflicts::blockDescriptions() const {
    std::vector<std::string> out;
    if (!buffer_) return out;
    auto lines = [this](size_t from, size_t to) {
        if (to <= from) return 0;
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)from);
        gtk_text_buffer_get_iter_at_offset(buffer_, &b, (int)to);
        return gtk_text_iter_get_line(&b) - gtk_text_iter_get_line(&a);
    };
    for (const Block& b : blocks_)
        out.push_back("current " + b.currentLabel + " " +
                      std::to_string(lines(b.currentStart, b.currentEnd)) + ", incoming " +
                      b.incomingLabel + " " + std::to_string(lines(b.incomingStart, b.incomingEnd)) +
                      (b.hasBase() ? ", base" : ""));
    return out;
}

// ------------------------------------------------------------ reading
bool EditorConflicts::reads() const {
    Document* d = editor_->active();
    if (!buffer_ || !d || !editor_->isSource() || d->path().empty() || d->loading() ||
        d->largeFileFeaturesOff() || !gtk_text_view_get_editable(GTK_TEXT_VIEW(view_)))
        return false;
    return gtk_text_buffer_get_char_count(buffer_) <= kMaxConflictText;
}

void EditorConflicts::rescanSoon() {
    if (timer_) g_source_remove(timer_);
    timer_ = g_timeout_add(200, [](gpointer p) -> gboolean {
        EditorConflicts* self = static_cast<EditorConflicts*>(p);
        self->timer_ = 0;
        self->rescanNow();
        return G_SOURCE_REMOVE;
    }, this);
}

void EditorConflicts::rescanNow() {
    if (timer_) g_source_remove(timer_);
    timer_ = 0;
    std::vector<Block> found;
    if (reads()) {
        const std::string text = wholeText(buffer_);
        if (text.find("<<<<<<<") != std::string::npos) {
            found = MergeConflicts::parse(text);
            toCharacters(text, found);
        }
    }
    setBlocks(std::move(found));
}

void EditorConflicts::setBlocks(std::vector<Block> found) {
    bool same = found.size() == blocks_.size();
    for (size_t i = 0; same && i < found.size(); i++)
        same = found[i].start == blocks_[i].start && found[i].end == blocks_[i].end &&
               found[i].separator == blocks_[i].separator;
    const bool counted = found.size() != blocks_.size();
    blocks_.swap(found);
    if (!same) gtk_widget_queue_draw(view_);
    if (counted && onBlocksChanged) onBlocksChanged();
}

void EditorConflicts::watchBuffer() {
    GtkTextBuffer* now = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_));
    if (now != buffer_) {
        if (buffer_) {
            g_signal_handlers_disconnect_by_data(buffer_, this);
            g_object_unref(buffer_);
        }
        buffer_ = now ? GTK_TEXT_BUFFER(g_object_ref(now)) : nullptr;
        if (buffer_) {
            // Before the default handlers, so the iters are where the edit
            // starts, in the text as it was.
            g_signal_connect(buffer_, "insert-text", G_CALLBACK((+[](GtkTextBuffer*, GtkTextIter* at,
                                                                    const char* text, int len,
                                                                    gpointer self) {
                const size_t n = len < 0 ? std::strlen(text) : (size_t)len;
                bool marker = n > 4096;
                for (size_t i = 0; !marker && i < n; ++i)
                    marker = std::strchr("<|=>", text[i]) != nullptr && text[i];
                static_cast<EditorConflicts*>(self)->edited(
                    offsetOf(at), 0, (size_t)g_utf8_strlen(text, (gssize)n), marker, false);
            })), this);
            g_signal_connect(buffer_, "delete-range", G_CALLBACK((+[](GtkTextBuffer* b,
                                                                     GtkTextIter* from,
                                                                     GtkTextIter* to,
                                                                     gpointer self) {
                size_t a = offsetOf(from), z = offsetOf(to);
                if (z < a) std::swap(a, z);
                const bool whole = a == 0 && z == (size_t)gtk_text_buffer_get_char_count(b);
                static_cast<EditorConflicts*>(self)->edited(a, z - a, 0, false, whole);
            })), this);
        }
        setBlocks({});
    }
    // Another file in the view (or the same one shown again): read it as
    // soon as the editor has put it there.
    if (!idle_)
        idle_ = g_idle_add([](gpointer p) -> gboolean {
            EditorConflicts* s = static_cast<EditorConflicts*>(p);
            s->idle_ = 0;
            s->rescanNow();
            return G_SOURCE_REMOVE;
        }, this);
}

// An edit of the shown buffer, in the text as it was before it: the blocks
// move at once, and the text is read again after the pause when the edit
// could have changed what the blocks are. Only text holding a marker's
// character can finish a block in a file with none; anything removed is
// read again too, to be safe.
void EditorConflicts::edited(size_t at, size_t removed, size_t inserted, bool mayMakeMarker,
                             bool whole) {
    const size_t before = blocks_.size();
    if (before) {
        MergeConflicts::followEdit(blocks_, at, removed, inserted);
        gtk_widget_queue_draw(view_);
        if (blocks_.size() != before && onBlocksChanged) onBlocksChanged();
    }
    if (whole) {
        // The whole text replaced (a reload): read it as soon as the new
        // text is in, so the tints come with it.
        if (!idle_)
            idle_ = g_idle_add([](gpointer p) -> gboolean {
                EditorConflicts* s = static_cast<EditorConflicts*>(p);
                s->idle_ = 0;
                s->rescanNow();
                return G_SOURCE_REMOVE;
            }, this);
    } else if (before > 0 || removed > 0 || mayMakeMarker) {
        rescanSoon();
    }
}

// ------------------------------------------------------------ drawing
GdkRectangle EditorConflicts::visibleRect() const {
    GdkRectangle r{0, 0, 0, 0};
    gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(view_), &r);
    return r;
}

void EditorConflicts::visibleChars(const GdkRectangle& visible, size_t* from, size_t* to) const {
    GtkTextView* tv = GTK_TEXT_VIEW(view_);
    GtkTextIter top, bottom;
    int ignore = 0;
    gtk_text_view_get_line_at_y(tv, &top, visible.y, &ignore);
    gtk_text_view_get_line_at_y(tv, &bottom, visible.y + visible.height, &ignore);
    if (!gtk_text_iter_ends_line(&bottom)) gtk_text_iter_forward_to_line_end(&bottom);
    *from = offsetOf(&top);
    *to = offsetOf(&bottom);
}

void EditorConflicts::drawBehind(GtkSnapshot* s, const GdkRectangle& visible) {
    if (blocks_.empty() || !buffer_ || !gtk_text_view_get_editable(GTK_TEXT_VIEW(view_))) return;
    GtkTextView* tv = GTK_TEXT_VIEW(view_);
    size_t from = 0, to = 0;
    visibleChars(visible, &from, &to);
    const GdkRGBA base{0x60 / 255.0f, 0x60 / 255.0f, 0x60 / 255.0f, 0x29 / 255.0f};
    // Characters [a, b) are whole lines (b the text's end at worst): from
    // the top of a's line to the bottom of the line before b, across the
    // view. A folded line has no height and takes nothing.
    auto fill = [&](size_t a, size_t b, const GdkRGBA& color) {
        if (b <= a || b <= from || a > to + 1) return;
        GtkTextIter ia, ib;
        gtk_text_buffer_get_iter_at_offset(buffer_, &ia, (int)a);
        gtk_text_buffer_get_iter_at_offset(buffer_, &ib, (int)(b - 1));
        int y0 = 0, h0 = 0, y1 = 0, h1 = 0;
        gtk_text_view_get_line_yrange(tv, &ia, &y0, &h0);
        gtk_text_view_get_line_yrange(tv, &ib, &y1, &h1);
        const int top = std::max(y0, visible.y);
        const int bottom = std::min(y1 + h1, visible.y + visible.height);
        if (bottom <= top) return;
        const graphene_rect_t r = GRAPHENE_RECT_INIT((float)visible.x, (float)top,
                                                     (float)visible.width, (float)(bottom - top));
        gtk_snapshot_append_color(s, &color, &r);
    };
    for (const Block& b : blocks_) {
        if (b.end <= from || b.start > to) continue;
        fill(b.start, b.currentStart, headerOf(current_));
        fill(b.currentStart, b.currentEnd, current_);
        if (b.hasBase()) {
            fill(b.baseMarker, b.baseStart, headerOf(base));
            fill(b.baseStart, b.baseEnd, base);
        }
        fill(b.incomingStart, b.incomingEnd, incoming_);
        fill(b.endMarker, b.end, headerOf(incoming_));
    }
}

// The buttons' words: the view's font family made the interface's, a size
// smaller, as the Mac's 11-point medium system font beside 13-point code.
PangoLayout* EditorConflicts::layout(const std::string& text) const {
    PangoLayout* l = gtk_widget_create_pango_layout(view_, text.c_str());
    PangoContext* context = gtk_widget_get_pango_context(view_);
    PangoFontDescription* fd =
        pango_font_description_copy(pango_context_get_font_description(context));
    pango_font_description_set_family(fd, "Sans");
    pango_font_description_set_weight(fd, PANGO_WEIGHT_MEDIUM);
    const int size = pango_font_description_get_size(fd);
    if (size > 0) {
        if (pango_font_description_get_size_is_absolute(fd))
            pango_font_description_set_absolute_size(fd, size * 0.85);
        else
            pango_font_description_set_size(fd, size * 85 / 100);
    }
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    return l;
}

// The buttons of the blocks whose "<<<<<<<" line is on screen, in buffer
// coordinates: after the line's text, or against the right edge of what
// shows when that is too far right.
std::vector<EditorConflicts::Button> EditorConflicts::buttonsIn(const GdkRectangle& visible,
                                                                std::vector<Label>* labels) const {
    std::vector<Button> out;
    GtkTextView* tv = GTK_TEXT_VIEW(view_);
    if (blocks_.empty() || !buffer_ || !gtk_text_view_get_editable(tv)) return out;
    size_t from = 0, to = 0;
    visibleChars(visible, &from, &to);
    const Keep keeps[3] = {Keep::Current, Keep::Incoming, Keep::Both};
    int widths[3] = {0, 0, 0}, textHeight = 0;
    for (int k = 0; k < 3; k++) {
        PangoLayout* l = layout(MergeConflicts::buttonTitle(keeps[k]));
        int w = 0, h = 0;
        pango_layout_get_pixel_size(l, &w, &h);
        g_object_unref(l);
        widths[k] = w + 2 * 8;
        textHeight = std::max(textHeight, h);
    }
    const int gap = 6;
    for (size_t i = 0; i < blocks_.size(); i++) {
        const Block& b = blocks_[i];
        if (b.start < from || b.start > to) continue;
        GtkTextIter at;
        gtk_text_buffer_get_iter_at_offset(buffer_, &at, (int)b.start);
        int ly = 0, lh = 0;
        gtk_text_view_get_line_yrange(tv, &at, &ly, &lh);
        if (lh <= 0) continue;   // folded away
        GtkTextIter end = at;
        if (!gtk_text_iter_ends_line(&end)) gtk_text_iter_forward_to_line_end(&end);
        GdkRectangle er{0, 0, 0, 0};
        gtk_text_view_get_iter_location(tv, &end, &er);
        const int lineH = er.height > 0 ? er.height : lh;
        const int h = std::max(textHeight + 2, lineH - 2);
        const std::string position = MergeConflicts::positionText(i, blocks_.size());
        PangoLayout* pl = layout(position);
        int labelW = 0, labelH = 0;
        pango_layout_get_pixel_size(pl, &labelW, &labelH);
        g_object_unref(pl);
        const int total = widths[0] + widths[1] + widths[2] + 3 * gap;
        int x = er.x + 16;
        const int right = visible.x + visible.width - 10;
        if (x + total + labelW > right) x = std::max(visible.x, right - total - labelW);
        const int y = er.y + (lineH - h) / 2;
        for (int k = 0; k < 3; k++) {
            out.push_back({keeps[k], b.start, GdkRectangle{x, y, widths[k], h}});
            x += widths[k] + gap;
        }
        if (labels) labels->push_back({position, GdkRectangle{x, y, labelW, h}});
    }
    return out;
}

void EditorConflicts::drawOver(GtkSnapshot* s, const GdkRectangle& visible) {
    if (blocks_.empty()) return;
    std::vector<Label> labels;
    GdkRectangle wider = visible;
    wider.y -= 40;
    wider.height += 80;
    const std::vector<Button> buttons = buttonsIn(wider, &labels);
    if (buttons.empty()) return;
    const GdkRGBA gray{0.5f, 0.5f, 0.5f, 1};
    const GdkRGBA both{background_.red * 0.7f + gray.red * 0.3f,
                       background_.green * 0.7f + gray.green * 0.3f,
                       background_.blue * 0.7f + gray.blue * 0.3f, 1};
    for (const Button& b : buttons) {
        const graphene_rect_t g = GRAPHENE_RECT_INIT((float)b.rect.x, (float)b.rect.y,
                                                     (float)b.rect.width, (float)b.rect.height);
        GskRoundedRect round;
        gsk_rounded_rect_init_from_rect(&round, &g, 4);
        const GdkRGBA fill = b.keep == Keep::Current    ? headerOf(current_)
                             : b.keep == Keep::Incoming ? headerOf(incoming_)
                                                        : both;
        gtk_snapshot_push_rounded_clip(s, &round);
        // A backing first, so text under a pinned button does not show
        // through a translucent fill.
        gtk_snapshot_append_color(s, &background_, &g);
        gtk_snapshot_append_color(s, &fill, &g);
        gtk_snapshot_pop(s);
        PangoLayout* l = layout(MergeConflicts::buttonTitle(b.keep));
        int w = 0, h = 0;
        pango_layout_get_pixel_size(l, &w, &h);
        gtk_snapshot_save(s);
        const graphene_point_t p =
            GRAPHENE_POINT_INIT((float)b.rect.x + (float)(b.rect.width - w) / 2,
                                (float)b.rect.y + (float)(b.rect.height - h) / 2);
        gtk_snapshot_translate(s, &p);
        gtk_snapshot_append_layout(s, l, &ink_);
        gtk_snapshot_restore(s);
        g_object_unref(l);
    }
    GdkRGBA muted = ink_;
    muted.alpha *= 0.6f;
    for (const Label& lab : labels) {
        PangoLayout* l = layout(lab.text);
        int w = 0, h = 0;
        pango_layout_get_pixel_size(l, &w, &h);
        gtk_snapshot_save(s);
        const graphene_point_t p = GRAPHENE_POINT_INIT(
            (float)lab.rect.x, (float)lab.rect.y + (float)(lab.rect.height - h) / 2);
        gtk_snapshot_translate(s, &p);
        gtk_snapshot_append_layout(s, l, &muted);
        gtk_snapshot_restore(s);
        g_object_unref(l);
    }
}

// ------------------------------------------------------------ acting
graphene_rect_t EditorConflicts::toWidget(const GdkRectangle& r) const {
    int wx = 0, wy = 0;
    gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(view_), GTK_TEXT_WINDOW_WIDGET, r.x, r.y,
                                          &wx, &wy);
    return GRAPHENE_RECT_INIT((float)wx, (float)wy, (float)r.width, (float)r.height);
}

graphene_rect_t EditorConflicts::buttonRect(int keep, size_t block) const {
    if (block >= blocks_.size()) return GRAPHENE_RECT_INIT(0, 0, 0, 0);
    const size_t start = blocks_[block].start;
    for (const Button& b : buttonsIn(visibleRect(), nullptr))
        if (b.blockStart == start && b.keep == keepOf(keep)) return toWidget(b.rect);
    return GRAPHENE_RECT_INIT(0, 0, 0, 0);
}

graphene_rect_t EditorConflicts::positionRect(size_t block) const {
    std::vector<Label> labels;
    if (block >= blocks_.size()) return GRAPHENE_RECT_INIT(0, 0, 0, 0);
    const std::vector<Button> buttons = buttonsIn(visibleRect(), &labels);
    const size_t start = blocks_[block].start;
    size_t n = 0;
    for (size_t i = 0; i + 2 < buttons.size(); i += 3, n++)
        if (buttons[i].blockStart == start && n < labels.size()) return toWidget(labels[n].rect);
    return GRAPHENE_RECT_INIT(0, 0, 0, 0);
}

void EditorConflicts::onPressed(GtkGestureClick* g, int, double x, double y, gpointer p) {
    EditorConflicts* self = static_cast<EditorConflicts*>(p);
    if (self->blocks_.empty()) return;
    const GdkModifierType mods =
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    if (mods & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) return;
    // Claimed first, so the text view's gestures let the press go.
    int bx = 0, by = 0;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(self->view_), GTK_TEXT_WINDOW_WIDGET,
                                          (int)x, (int)y, &bx, &by);
    for (const Button& b : self->buttonsIn(self->visibleRect(), nullptr)) {
        const GdkRectangle& r = b.rect;
        if (bx < r.x || bx >= r.x + r.width || by < r.y || by >= r.y + r.height) continue;
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
        self->keepAt(b.keep, b.blockStart);
        gtk_widget_grab_focus(self->view_);
        return;
    }
}

bool EditorConflicts::pressAt(double x, double y) {
    if (blocks_.empty()) return false;
    int bx = 0, by = 0;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(view_), GTK_TEXT_WINDOW_WIDGET, (int)x,
                                          (int)y, &bx, &by);
    for (const Button& b : buttonsIn(visibleRect(), nullptr)) {
        const GdkRectangle& r = b.rect;
        if (bx < r.x || bx >= r.x + r.width || by < r.y || by >= r.y + r.height) continue;
        return keepAt(b.keep, b.blockStart);
    }
    return false;
}

bool EditorConflicts::keep(int k, size_t block) {
    rescanNow();
    if (block >= blocks_.size()) {
        gtk_widget_error_bell(view_);
        return false;
    }
    return keepAt(keepOf(k), blocks_[block].start);
}

// The block that starts at `start` (read again first, so a stale block is
// never replaced) resolved as one undo step.
bool EditorConflicts::keepAt(Keep keep, size_t start) {
    rescanNow();
    long index = -1;
    for (size_t i = 0; i < blocks_.size(); i++)
        if (blocks_[i].start == start) index = (long)i;
    if (index < 0 || !buffer_ || !gtk_text_view_get_editable(GTK_TEXT_VIEW(view_))) {
        gtk_widget_error_bell(view_);
        return false;
    }
    // The splice from the UTF-8 text (the same parse, so the same blocks in
    // the same order), its byte offsets made characters.
    const std::string text = wholeText(buffer_);
    const std::vector<Block> bytes = MergeConflicts::parse(text);
    if ((size_t)index >= bytes.size() || bytes.size() != blocks_.size()) {
        gtk_widget_error_bell(view_);
        return false;
    }
    const MergeConflicts::Splice8 sp = MergeConflicts::resolve(text, bytes[(size_t)index], keep);
    MultiEdit::Change change;
    change.start = charsIn(text, 0, sp.start);
    change.length = charsIn(text, sp.start, sp.start + sp.length);
    change.text = sp.text;
    const std::string step = MergeConflicts::actionName(keep);
    if (!MultiEdit::apply(buffer_, {change}, false, step.c_str())) {
        gtk_widget_error_bell(view_);
        return false;
    }
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(buffer_, &at, (int)change.start);
    gtk_text_buffer_place_cursor(buffer_, &at);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(view_), gtk_text_buffer_get_insert(buffer_), 0.1,
                                 FALSE, 0, 0);
    rescanNow();
    gtk_widget_queue_draw(view_);
    if (onResolved) onResolved(blocks_.size());
    return true;
}

bool EditorConflicts::goToNext(bool forward) {
    rescanNow();
    if (!buffer_) return false;
    GtkTextIter caret;
    gtk_text_buffer_get_iter_at_mark(buffer_, &caret, gtk_text_buffer_get_insert(buffer_));
    const long i = MergeConflicts::step(blocks_, offsetOf(&caret), forward);
    if (i < 0) return false;
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(buffer_, &at, (int)blocks_[(size_t)i].start);
    gtk_text_buffer_place_cursor(buffer_, &at);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(view_), gtk_text_buffer_get_insert(buffer_), 0.1,
                                 FALSE, 0, 0);
    return true;
}
