// GitDiffView.cpp — see GitDiffView.h. The view moved here from GitPanel.cpp
// when it gained the hunk actions (PARITY.md 2.9).
#include "GitDiffView.h"

#include <algorithm>
#include <ctime>

namespace {

const char* kAccent = "#4EA1F7";

std::string toolTipFor(GitUi::HunkOp op) {
    switch (op) {
    case GitUi::HunkOp::Stage: return "Stage this hunk; the rest of the file stays as it is.";
    case GitUi::HunkOp::Unstage: return "Take this hunk out of what is staged.";
    case GitUi::HunkOp::Discard:
        return "Put this hunk back as git has it; asks first, and Undo Discard brings it back.";
    }
    return "";
}

}  // namespace

GitDiffView::GitDiffView() { build(); }

void GitDiffView::build() {
    buffer_ = gtk_text_buffer_new(nullptr);
    auto tag = [&](const char* name, const char* color, bool bold) {
        GtkTextTag* t = gtk_text_buffer_create_tag(buffer_, name, nullptr);
        if (color) g_object_set(t, "foreground", color, nullptr);
        if (bold) g_object_set(t, "weight", PANGO_WEIGHT_BOLD, nullptr);
        return t;
    };
    tag("added", "#73C991", false);
    tag("removed", "#F14C4C", false);
    tag("muted", "#9CA3AF", false);
    tag("fileheader", "#9CA3AF", true);
    tag("hash", "#E2C08D", false);
    tag("bold", nullptr, true);
    // The hunk actions: the accent color, underlined, as links read.
    GtkTextTag* link = tag("hunklink", kAccent, false);
    g_object_set(link, "underline", PANGO_UNDERLINE_SINGLE, nullptr);
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

    // A link: pressed and let go on the same character, with nothing
    // selected by a drag between.
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(onPressed), this);
    g_signal_connect(click, "released", G_CALLBACK(onReleased), this);
    gtk_widget_add_controller(view_, GTK_EVENT_CONTROLLER(click));
    // The right click, before the text view's own menu: the hunk actions
    // when there are some for the place clicked.
    GtkGesture* menu = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(menu), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(menu), GTK_PHASE_CAPTURE);
    g_signal_connect(menu, "pressed", G_CALLBACK(onMenuPressed), this);
    gtk_widget_add_controller(view_, GTK_EVENT_CONTROLLER(menu));
    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(onKey), this);
    gtk_widget_add_controller(view_, keys);
    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(onMotion), this);
    gtk_widget_add_controller(view_, motion);
    gtk_widget_set_has_tooltip(view_, TRUE);
    g_signal_connect(view_, "query-tooltip", G_CALLBACK(onTooltip), this);

    // The menu's items: "diff.hunk" with (op, lines, first, last).
    actions_ = g_simple_action_group_new();
    GSimpleAction* hunk = g_simple_action_new("hunk", G_VARIANT_TYPE("(ubtt)"));
    g_signal_connect(hunk, "activate", G_CALLBACK(onHunkAction), this);
    g_action_map_add_action(G_ACTION_MAP(actions_), G_ACTION(hunk));
    g_object_unref(hunk);
    gtk_widget_insert_action_group(view_, "diff", G_ACTION_GROUP(actions_));
}

GitDiffView::~GitDiffView() {
    dropMenu();
    g_signal_handlers_disconnect_by_data(view_, this);
    GListModel* ctrls = gtk_widget_observe_controllers(view_);
    for (guint i = 0; i < g_list_model_get_n_items(ctrls); ++i) {
        GObject* c = static_cast<GObject*>(g_list_model_get_item(ctrls, i));
        g_signal_handlers_disconnect_by_data(c, this);
        g_object_unref(c);
    }
    g_object_unref(ctrls);
    gtk_widget_insert_action_group(view_, "diff", nullptr);
    g_object_unref(actions_);
}

void GitDiffView::show(const std::string& bytes, bool commit) {
    dropMenu();
    links_.clear();
    hunks_ = false;
    bytes_.clear();
    GitUi::DiffSide side = GitUi::DiffSide::Untracked;
    std::vector<GitUi::DiffAction> actions;
    GitUi::StyledText st;
    if (commit) {
        st = GitUi::commitText(bytes, (long long)time(nullptr));
    } else if (hunksFor && hunksFor(bytes, &side)) {
        st = GitUi::diffTextWithActions(bytes, side, actions);
        hunks_ = true;
        side_ = side;
        bytes_ = bytes;
    } else {
        st = GitUi::diffText(bytes);
    }
    gtk_text_buffer_set_text(buffer_, st.text.c_str(), (int)st.text.size());
    auto apply = [&](const char* name, size_t start, size_t length) {
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)start);
        gtk_text_buffer_get_iter_at_offset(buffer_, &b, (int)(start + length));
        gtk_text_buffer_apply_tag_by_name(buffer_, name, &a, &b);
    };
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
        if (name) apply(name, r.start, r.length);
    }
    for (const GitUi::DiffAction& a : actions) {
        apply("hunklink", a.start, a.length);
        links_.push_back({a.op, a.line, (int)a.start, (int)(a.start + a.length)});
    }
    // Lines are counted at '\n' only, as git printed them (GTK would also
    // break at a lone '\r').
    lineStarts_.assign(1, 0);
    int ch = 0;
    for (size_t i = 0; i < st.text.size(); i++) {
        const unsigned char c = (unsigned char)st.text[i];
        if ((c & 0xC0) == 0x80) continue;   // a continuation byte
        ch++;
        if (c == '\n') lineStarts_.push_back(ch);
    }
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buffer_, &start);
    gtk_text_buffer_place_cursor(buffer_, &start);
    gtk_adjustment_set_value(
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_)), 0);
}

std::string GitDiffView::text() const {
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    char* t = gtk_text_buffer_get_text(buffer_, &a, &b, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

size_t GitDiffView::lineOf(int offset) const {
    auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
    return it == lineStarts_.begin() ? 0 : (size_t)(it - lineStarts_.begin() - 1);
}

int GitDiffView::linkAt(int offset) const {
    for (size_t i = 0; i < links_.size(); i++)
        if (offset >= links_[i].start && offset < links_[i].end) return (int)i;
    return -1;
}

// The character under a point of the view (widget coordinates); false off
// the text.
bool GitDiffView::iterAtPoint(double x, double y, GtkTextIter* it) const {
    int bx = 0, by = 0;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(view_), GTK_TEXT_WINDOW_WIDGET, (int)x,
                                          (int)y, &bx, &by);
    if (!gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(view_), it, bx, by)) return false;
    // get_iter_at_location answers the nearest character; a point past a
    // line's end is on no character.
    GdkRectangle r;
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(view_), it, &r);
    return bx >= r.x && bx < r.x + std::max(r.width, 1);
}

void GitDiffView::onPressed(GtkGestureClick*, int n, double x, double y, gpointer selfp) {
    GitDiffView* self = static_cast<GitDiffView*>(selfp);
    GtkTextIter it;
    self->pressOffset_ = n == 1 && self->iterAtPoint(x, y, &it) ? gtk_text_iter_get_offset(&it) : -1;
}

void GitDiffView::onReleased(GtkGestureClick* g, int n, double x, double y, gpointer selfp) {
    GitDiffView* self = static_cast<GitDiffView*>(selfp);
    const int pressed = self->pressOffset_;
    self->pressOffset_ = -1;
    if (n != 1 || pressed < 0 || !self->hunks_ || !self->onHunk) return;
    if (gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g)) &
        (GDK_SHIFT_MASK | GDK_CONTROL_MASK))
        return;   // extending a selection
    GtkTextIter it;
    if (!self->iterAtPoint(x, y, &it)) return;
    const int link = self->linkAt(gtk_text_iter_get_offset(&it));
    if (link < 0 || link != self->linkAt(pressed)) return;
    if (gtk_text_buffer_get_has_selection(self->buffer_)) {
        // A click lands as a caret; a drag over the link would select.
        GtkTextIter a, b;
        gtk_text_buffer_get_selection_bounds(self->buffer_, &a, &b);
        if (gtk_text_iter_get_offset(&a) != gtk_text_iter_get_offset(&b)) return;
    }
    const Link l = self->links_[(size_t)link];
    self->onHunk(l.op, l.line, l.line, false);
}

bool GitDiffView::popupHunkMenu(size_t line, double x, double y) {
    if (!hunks_) return false;
    GtkTextIter a, b;
    const bool selection = gtk_text_buffer_get_selection_bounds(buffer_, &a, &b);
    const int s = gtk_text_iter_get_offset(&a), e = gtk_text_iter_get_offset(&b);
    const size_t first = lineOf(s), last = selection && e > s ? lineOf(e - 1) : first;
    const std::vector<GitUi::HunkMenuItem> items =
        GitUi::hunkMenu(bytes_, side_, line, first, last, selection && e > s);
    if (items.empty()) return false;
    dropMenu();
    GMenu* model = g_menu_new();
    GMenu* ops = g_menu_new();
    for (const GitUi::HunkMenuItem& it : items) {
        GMenuItem* mi = g_menu_item_new(it.title.c_str(), nullptr);
        g_menu_item_set_action_and_target_value(
            mi, "diff.hunk",
            g_variant_new("(ubtt)", (guint32)it.op, (gboolean)it.lines, (guint64)it.first,
                          (guint64)it.last));
        g_menu_append_item(ops, mi);
        g_object_unref(mi);
    }
    g_menu_append_section(model, nullptr, G_MENU_MODEL(ops));
    g_object_unref(ops);
    // The text view's own, which its actions answer.
    GMenu* edit = g_menu_new();
    g_menu_append(edit, "Copy", "clipboard.copy");
    g_menu_append(edit, "Select All", "selection.select-all");
    g_menu_append_section(model, nullptr, G_MENU_MODEL(edit));
    g_object_unref(edit);
    menu_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(model));
    g_object_unref(model);
    gtk_popover_set_has_arrow(GTK_POPOVER(menu_), FALSE);
    gtk_widget_set_halign(menu_, GTK_ALIGN_START);
    gtk_widget_set_parent(menu_, view_);
    GdkRectangle at = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(menu_), &at);
    gtk_popover_popup(GTK_POPOVER(menu_));
    return true;
}

// The popover is the view's child only by gtk_widget_set_parent, so it comes
// off by hand (the LSP popovers' trap, Lsp.cpp).
void GitDiffView::dropMenu() {
    if (!menu_) return;
    GtkWidget* m = menu_;
    menu_ = nullptr;
    gtk_popover_popdown(GTK_POPOVER(m));
    if (gtk_widget_get_parent(m)) gtk_widget_unparent(m);
}

void GitDiffView::onMenuPressed(GtkGestureClick* g, int, double x, double y, gpointer selfp) {
    GitDiffView* self = static_cast<GitDiffView*>(selfp);
    if (!self->hunks_) return;   // the text view's own menu
    GtkTextIter it;
    const size_t line = self->iterAtPoint(x, y, &it) ? self->lineOf(gtk_text_iter_get_offset(&it))
                                                     : (size_t)-1;
    if (self->popupHunkMenu(line, x, y))
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
}

// The Menu key and Shift+F10: the menu at the caret's line.
gboolean GitDiffView::onKey(GtkEventControllerKey*, guint key, guint, GdkModifierType mods,
                            gpointer selfp) {
    GitDiffView* self = static_cast<GitDiffView*>(selfp);
    const bool shift = mods & GDK_SHIFT_MASK;
    if (!(key == GDK_KEY_Menu || (key == GDK_KEY_F10 && shift))) return FALSE;
    if (!self->hunks_) return FALSE;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(self->buffer_, &it, gtk_text_buffer_get_insert(self->buffer_));
    GdkRectangle r;
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(self->view_), &it, &r);
    int wx = 0, wy = 0;
    gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(self->view_), GTK_TEXT_WINDOW_WIDGET, r.x,
                                          r.y + r.height, &wx, &wy);
    return self->popupHunkMenu(self->lineOf(gtk_text_iter_get_offset(&it)), wx, wy);
}

void GitDiffView::onMotion(GtkEventControllerMotion*, double x, double y, gpointer selfp) {
    GitDiffView* self = static_cast<GitDiffView*>(selfp);
    GtkTextIter it;
    const bool over = self->hunks_ && self->iterAtPoint(x, y, &it) &&
                      self->linkAt(gtk_text_iter_get_offset(&it)) >= 0;
    if (over == self->overLink_) return;
    self->overLink_ = over;
    gtk_widget_set_cursor_from_name(self->view_, over ? "pointer" : "text");
}

gboolean GitDiffView::onTooltip(GtkWidget*, int x, int y, gboolean keyboard, GtkTooltip* tip,
                                gpointer selfp) {
    GitDiffView* self = static_cast<GitDiffView*>(selfp);
    GtkTextIter it;
    if (keyboard || !self->hunks_ || !self->iterAtPoint(x, y, &it)) return FALSE;
    const int link = self->linkAt(gtk_text_iter_get_offset(&it));
    if (link < 0) return FALSE;
    gtk_tooltip_set_text(tip, toolTipFor(self->links_[(size_t)link].op).c_str());
    return TRUE;
}

void GitDiffView::onHunkAction(GSimpleAction*, GVariant* target, gpointer selfp) {
    GitDiffView* self = static_cast<GitDiffView*>(selfp);
    guint32 op = 0;
    gboolean lines = FALSE;
    guint64 first = 0, last = 0;
    g_variant_get(target, "(ubtt)", &op, &lines, &first, &last);
    if (self->onHunk) self->onHunk((GitUi::HunkOp)op, (size_t)first, (size_t)last, lines);
}
