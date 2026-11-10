// OpacityPicker.cpp — part of Editor (Editor.h): the settings file's opacity
// values (editor.opacity = 0.9, sidebar.opacity = 80%) as links, a click
// opening a slider under the value, the Mac's MCOpacityPopover
// (Appearance.mm) and pickOpacityAtIndex: (EditorController+Settings.mm),
// PARITY.md 2.11. Like the color picker beside it in Editor.cpp, each move
// of the slider rewrites the line (uncommenting it) and the file is saved
// 150 ms after the last one, so the panel changes as the slider moves; it
// shares the picker's popover slot (colorPop_), so one is up at a time.
#include "Editor.h"

#include "Document.h"
#include "Utf8Offsets.h"

#include <algorithm>
#include <cmath>

// Is there an opacity value under widget point (x, y)? Reports its line.
bool Editor::opacityAt(double x, double y, int* line) const {
    if (!isSource() || !isSettingsFile()) return false;
    int bx, by;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(view_), GTK_TEXT_WINDOW_WIDGET,
                                          (int)x, (int)y, &bx, &by);
    // The character under the point: get_iter_at_location moves on to the
    // next one over a character's right half, which missed a one-digit
    // value half the time (the swatches checks).
    GtkTextIter it;
    int trailing = 0;
    if (!gtk_text_view_get_iter_at_position(GTK_TEXT_VIEW(view_), &it, &trailing, bx, by))
        return false;
    GtkTextTag* tag =
        gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer_), Document::kOpacityTag);
    if (!tag || !gtk_text_iter_has_tag(&it, tag)) return false;
    if (line) *line = gtk_text_iter_get_line(&it);
    return true;
}

// A slider from 0 to 100% in a popover under the value, at the value the
// line holds. The slider's own number says the percentage.
void Editor::pickOpacity(int line) {
    GtkTextIter ls, le;
    gtk_text_buffer_get_iter_at_line(buffer_, &ls, line);
    le = ls;
    if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
    char* raw = gtk_text_buffer_get_text(buffer_, &ls, &le, FALSE);
    const std::u16string text = utf16::fromUtf8(raw ? raw : "");
    g_free(raw);
    ValueSpan span;
    double value = 1;
    if (!Settings::findOpacity(text, span, value)) return;
    dropColorPopover();

    // Point at the value itself.
    GtkTextIter a = ls, b = ls;
    gtk_text_iter_forward_chars(&a, (int)utf16::toCharOffset(text, span.start));
    gtk_text_iter_forward_chars(&b, (int)utf16::toCharOffset(text, span.start + span.length));
    GdkRectangle ra, rb;
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(view_), &a, &ra);
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(view_), &b, &rb);
    int x, y;
    gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(view_), GTK_TEXT_WINDOW_WIDGET, ra.x,
                                          ra.y, &x, &y);
    GdkRectangle at = {x, y, std::max(1, rb.x - ra.x), std::max(1, ra.height)};

    GtkWidget* scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_widget_add_css_class(scale, "minicode-opacity-slider");
    gtk_widget_set_size_request(scale, 200, -1);
    gtk_scale_set_digits(GTK_SCALE(scale), 0);
    gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
    gtk_scale_set_format_value_func(GTK_SCALE(scale), [](GtkScale*, double v, gpointer) {
        return g_strdup_printf("%d%%", (int)std::lround(v));
    }, nullptr, nullptr);
    gtk_range_set_increments(GTK_RANGE(scale), 1, 10);
    gtk_range_set_value(GTK_RANGE(scale), std::round(value * 100));
    gtk_widget_set_tooltip_text(scale, "Opacity");
    // Connected after the value is set, so opening changes nothing.
    g_signal_connect(scale, "value-changed", G_CALLBACK(onOpacityChanged), this);

    GtkWidget* pop = gtk_popover_new();
    gtk_widget_add_css_class(pop, "minicode-opacity-popover");
    gtk_popover_set_child(GTK_POPOVER(pop), scale);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &at);
    gtk_widget_set_parent(pop, view_);
    // Closing: the last move is saved now rather than 150 ms later, and the
    // popover comes off the view from an idle, as the color picker's does.
    g_signal_connect(pop, "closed", G_CALLBACK(+[](GtkPopover* p, gpointer selfp) {
        Editor* self = static_cast<Editor*>(selfp);
        if (self->colorSaveTimer_) self->saveColorNow();
        if (self->colorPop_ == GTK_WIDGET(p)) self->colorPop_ = nullptr;
        g_idle_add([](gpointer w) -> gboolean {
            GtkWidget* widget = GTK_WIDGET(w);
            if (gtk_widget_get_parent(widget)) gtk_widget_unparent(widget);
            g_object_unref(widget);
            return G_SOURCE_REMOVE;
        }, g_object_ref(p));
    }), this);

    colorPop_ = pop;
    colorLine_ = line;
    colorDoc_ = active_;
    gtk_popover_popup(GTK_POPOVER(pop));
    gtk_widget_grab_focus(scale);   // the arrow keys move it, Escape closes it
}

void Editor::onOpacityChanged(GtkRange* range, gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    if (!self->colorPop_ || self->active_ != self->colorDoc_ || !self->isSource()) return;
    self->setLineOpacity(self->colorLine_, std::round(gtk_range_get_value(range)) / 100.0);
    self->scheduleColorSave();
}

// Rewrite one line's opacity (as a percentage if it was written as one,
// uncommenting it if it was a commented-out default). The caller saves.
void Editor::setLineOpacity(int line, double value) {
    if (!isSource() || line < 0 || line >= gtk_text_buffer_get_line_count(buffer_)) return;
    GtkTextIter ls, le;
    gtk_text_buffer_get_iter_at_line(buffer_, &ls, line);
    le = ls;
    if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
    char* raw = gtk_text_buffer_get_text(buffer_, &ls, &le, FALSE);
    const std::u16string text = utf16::fromUtf8(raw ? raw : "");
    g_free(raw);
    const std::u16string updated = Settings::setOpacity(text, value);
    if (updated == text) return;
    const std::string out = utf16::toUtf8(updated);

    // The caret stays where it was rather than jumping to the line's end.
    GtkTextIter cur;
    gtk_text_buffer_get_iter_at_mark(buffer_, &cur, gtk_text_buffer_get_insert(buffer_));
    const int curLine = gtk_text_iter_get_line(&cur);
    const int curCol = gtk_text_iter_get_line_offset(&cur);

    gtk_text_buffer_begin_user_action(buffer_);
    gtk_text_buffer_delete(buffer_, &ls, &le);
    gtk_text_buffer_insert(buffer_, &ls, out.c_str(), (int)out.size());
    gtk_text_buffer_end_user_action(buffer_);

    if (curLine == line) {
        GtkTextIter back;
        gtk_text_buffer_get_iter_at_line(buffer_, &back, line);
        const int len = gtk_text_iter_get_chars_in_line(&back);
        gtk_text_iter_set_line_offset(&back, std::min(curCol, std::max(0, len - 1)));
        gtk_text_buffer_place_cursor(buffer_, &back);
    }
}
