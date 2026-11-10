// LspSessionCompletion.cpp — LspSession's completion list (the Mac's
// Lsp+Completion.mm, without signature help and snippets yet): asked for on
// Ctrl+Space and after a trigger character (".", "->", "::"), narrowed as
// the word grows, and closed when the caret leaves the word. The list is a
// popover that never takes the keyboard: the view's key controller hands it
// Up, Down, Return, Tab and Escape (completionKey). Moved here from Lsp.cpp
// unchanged when the session was split (PARITY.md item 2.0).
#include "LspSessionPrivate.h"

#include "MultiEdit.h"

#include <algorithm>

using lspgtk::Server;

void LspSession::triggerCompletion() {
    if (noServer()) return;
    requestCompletion("");
}

// One character typed into the file on show: is it the end of ".", "->" or
// "::"? The caret is already past it (the insert mark moves with text typed
// at it). Anything else typed while the list is up narrows it.
void LspSession::completionAfterEdit(const char* inserted, int len) {
    // A rename or a fix going in is not typing.
    if (MultiEdit::applying()) return;
    if (inserted && len == 1) {
        GtkTextIter cur, prev;
        cursorIter(&cur);
        prev = cur;
        const char c = inserted[0];
        const bool atCaret = gtk_text_iter_backward_char(&prev) &&
                             gtk_text_iter_get_char(&prev) == (gunichar)(unsigned char)c;
        gunichar before = 0;
        if (atCaret && gtk_text_iter_backward_char(&prev)) before = gtk_text_iter_get_char(&prev);
        std::string trigger;
        if (atCaret) {
            if (c == '.') trigger = ".";
            else if (c == '>' && before == '-') trigger = ">";
            else if (c == ':' && before == ':') trigger = ":";
        }
        if (!trigger.empty()) {
            std::vector<std::string> triggers = server_->client().completionTriggers();
            if (triggers.empty() ||
                std::find(triggers.begin(), triggers.end(), trigger) != triggers.end()) {
                // Once the text view has finished with the key press.
                cmp_->pendingTrigger = trigger;
                if (!cmp_->triggerId)
                    cmp_->triggerId = g_idle_add([](gpointer p) -> gboolean {
                        LspSession* self = static_cast<LspSession*>(p);
                        self->cmp_->triggerId = 0;
                        self->requestCompletion(self->cmp_->pendingTrigger);
                        return G_SOURCE_REMOVE;
                    }, this);
                return;
            }
        }
    }
    if (cmp_->completing) scheduleRefilter();
}

void LspSession::cancelCompletionTrigger() {
    if (cmp_->triggerId) g_source_remove(cmp_->triggerId);
    cmp_->triggerId = 0;
}

void LspSession::requestCompletion(const std::string& trigger) {
    if (!server_ || uri_.empty()) return;
    flushChanges();
    GtkTextIter cur, a;
    cursorIter(&cur);
    a = cur;
    for (GtkTextIter p = a;
         gtk_text_iter_backward_char(&p) && lspui::isIdent(gtk_text_iter_get_char(&p));)
        a = p;
    if (!cmp_->anchor) cmp_->anchor = gtk_text_buffer_create_mark(buffer_, nullptr, &a, TRUE);
    else gtk_text_buffer_move_mark(buffer_, cmp_->anchor, &a);
    cmp_->completing = true;
    cmp_->gotItems = false;
    cmp_->items.clear();
    const int gen = ++cmp_->gen;
    const std::string uri = uri_;
    std::weak_ptr<int> alive = life_;
    server_->client().completion(uri, positionOf(&cur),
        [alive, this, gen, uri](const Json& r, const Json&) {
            if (!alive.expired()) completionArrived(r, gen, uri);
        }, trigger);
}

void LspSession::completionArrived(const Json& result, int gen, const std::string& uri) {
    if (gen != cmp_->gen || uri != uri_ || !cmp_->completing) return;
    cmp_->items = Lsp::parseCompletion(result);
    cmp_->gotItems = true;
    refilter();
}

void LspSession::scheduleRefilter() {
    if (cmp_->refilterId) return;
    cmp_->refilterId = g_idle_add([](gpointer p) -> gboolean {
        LspSession* self = static_cast<LspSession*>(p);
        self->cmp_->refilterId = 0;
        self->refilter();
        return G_SOURCE_REMOVE;
    }, this);
}

// The list follows the word being typed: it narrows as it grows (prefix
// matches first, then subsequences) and closes when the caret leaves it.
void LspSession::refilter() {
    if (!cmp_->completing || !cmp_->anchor) return;
    GtkTextIter selA, selB, anc;
    if (gtk_text_buffer_get_selection_bounds(buffer_, &selA, &selB)) {
        closeCompletion();
        return;
    }
    GtkTextIter cur;
    cursorIter(&cur);
    gtk_text_buffer_get_iter_at_mark(buffer_, &anc, cmp_->anchor);
    GtkRoot* root = gtk_widget_get_root(view_);
    if (gtk_text_iter_compare(&cur, &anc) < 0 || (root && gtk_root_get_focus(root) != view_)) {
        closeCompletion();
        return;
    }
    for (GtkTextIter p = anc; gtk_text_iter_compare(&p, &cur) < 0; gtk_text_iter_forward_char(&p))
        if (!lspui::isIdent(gtk_text_iter_get_char(&p))) {
            closeCompletion();
            return;
        }
    if (!cmp_->gotItems) return;   // still waiting for the server
    char* raw = gtk_text_buffer_get_slice(buffer_, &anc, &cur, TRUE);
    const std::string prefix = raw ? raw : "";
    g_free(raw);
    std::vector<Lsp::CompletionItem>& shown = cmp_->shown;
    shown = Lsp::filterCompletions(cmp_->items, prefix);
    // Nothing to offer, or only exactly what is already typed.
    if (shown.empty() || (shown.size() == 1 && shown[0].textToInsert() == prefix)) {
        closeCompletion();
        return;
    }
    if (shown.size() > 200) shown.resize(200);

    if (!cmp_->popover) buildPopover();
    // Keep the highlighted item while the list narrows, else the first.
    std::string previous;
    GtkListBoxRow* sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(cmp_->list));
    if (sel && gtk_widget_get_visible(cmp_->popover)) {
        GtkWidget* label = gtk_list_box_row_get_child(sel);
        if (label) previous = static_cast<const char*>(g_object_get_data(G_OBJECT(label), "item"));
    }
    gtk_list_box_remove_all(GTK_LIST_BOX(cmp_->list));
    int keep = 0;
    for (size_t i = 0; i < shown.size(); ++i) {
        const Lsp::CompletionItem& item = shown[i];
        char* l = g_markup_escape_text(item.label.c_str(), -1);
        std::string markup = l;
        g_free(l);
        if (!item.detail.empty()) {
            char* d = g_markup_escape_text(item.detail.c_str(), -1);
            markup += std::string("   <span foreground=\"#9CA3AF\">") + d + "</span>";
            g_free(d);
        }
        GtkWidget* label = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(label), markup.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
        g_object_set_data_full(G_OBJECT(label), "item", g_strdup(item.label.c_str()), g_free);
        gtk_list_box_append(GTK_LIST_BOX(cmp_->list), label);
        GtkWidget* row = gtk_widget_get_parent(label);
        if (row) gtk_widget_set_focusable(row, FALSE);
        if (!previous.empty() && item.label == previous) keep = (int)i;
    }
    GdkRectangle r;
    caretRect(&anc, &r);
    gtk_popover_set_pointing_to(GTK_POPOVER(cmp_->popover), &r);
    if (!gtk_widget_get_visible(cmp_->popover)) gtk_popover_popup(GTK_POPOVER(cmp_->popover));
    GtkListBoxRow* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(cmp_->list), keep);
    gtk_list_box_select_row(GTK_LIST_BOX(cmp_->list), row);
    GtkAdjustment* adj =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(cmp_->listScroll));
    if (keep == 0) gtk_adjustment_set_value(adj, 0);
    else moveSelection(0);
}

void LspSession::buildPopover() {
    GtkWidget* popover = gtk_popover_new();
    cmp_->popover = popover;
    gtk_widget_add_css_class(popover, "minicode-completion");
    // Not autohide: an autohide popover grabs the keyboard, and typing must
    // keep going to the text view, which forwards the list's keys (onKey).
    gtk_popover_set_autohide(GTK_POPOVER(popover), FALSE);
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
    gtk_widget_set_focusable(popover, FALSE);
    gtk_widget_set_can_focus(popover, FALSE);

    cmp_->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(cmp_->list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(cmp_->list), TRUE);
    gtk_widget_set_focusable(cmp_->list, FALSE);
    gtk_widget_set_can_focus(cmp_->list, FALSE);
    g_signal_connect(cmp_->list, "row-activated", G_CALLBACK(onRowActivated), this);

    cmp_->listScroll = gtk_scrolled_window_new();
    GtkScrolledWindow* scroll = GTK_SCROLLED_WINDOW(cmp_->listScroll);
    gtk_scrolled_window_set_policy(scroll, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(scroll, TRUE);
    gtk_scrolled_window_set_max_content_height(scroll, 220);
    gtk_widget_set_size_request(cmp_->listScroll, 420, -1);
    gtk_widget_set_can_focus(cmp_->listScroll, FALSE);
    gtk_scrolled_window_set_child(scroll, cmp_->list);
    gtk_popover_set_child(GTK_POPOVER(popover), cmp_->listScroll);
    gtk_widget_set_parent(popover, view_);
}

void LspSession::onRowActivated(GtkListBox*, GtkListBoxRow* row, gpointer selfp) {
    static_cast<LspSession*>(selfp)->acceptCompletion(gtk_list_box_row_get_index(row));
}

int LspSession::selectedRow() const {
    if (!cmp_->list) return -1;
    GtkListBoxRow* row = gtk_list_box_get_selected_row(GTK_LIST_BOX(cmp_->list));
    return row ? gtk_list_box_row_get_index(row) : -1;
}

// Move the highlight (wrapping around) and keep it in view.
void LspSession::moveSelection(int delta) {
    const int n = (int)cmp_->shown.size();
    if (!cmp_->list || n == 0) return;
    const int row = ((std::max(selectedRow(), 0) + delta) % n + n) % n;
    GtkListBoxRow* r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(cmp_->list), row);
    if (!r) return;
    gtk_list_box_select_row(GTK_LIST_BOX(cmp_->list), r);
    graphene_rect_t b;
    if (!gtk_widget_compute_bounds(GTK_WIDGET(r), cmp_->list, &b)) return;
    GtkAdjustment* adj =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(cmp_->listScroll));
    const double v = gtk_adjustment_get_value(adj), page = gtk_adjustment_get_page_size(adj);
    const double top = b.origin.y, bottom = top + b.size.height;
    if (top < v) gtk_adjustment_set_value(adj, top);
    else if (bottom > v + page) gtk_adjustment_set_value(adj, bottom - page);
}

void LspSession::closeCompletion() {
    cmp_->completing = false;
    cmp_->gotItems = false;
    cmp_->items.clear();
    cmp_->shown.clear();
    if (cmp_->refilterId) g_source_remove(cmp_->refilterId);
    cmp_->refilterId = 0;
    if (cmp_->popover && gtk_widget_get_visible(cmp_->popover))
        gtk_popover_popdown(GTK_POPOVER(cmp_->popover));
}

// The buffer on show is going: the anchor is one of its marks.
void LspSession::forgetCompletionAnchor() {
    if (cmp_->anchor && buffer_ && !gtk_text_mark_get_deleted(cmp_->anchor))
        gtk_text_buffer_delete_mark(buffer_, cmp_->anchor);
    cmp_->anchor = nullptr;
}

void LspSession::dropCompletionPopover() {
    closeCompletion();
    cancelCompletionTrigger();
    if (cmp_->popover) gtk_widget_unparent(cmp_->popover);
    cmp_->popover = cmp_->list = cmp_->listScroll = nullptr;
}

void LspSession::acceptCompletion(int row) {
    const std::vector<Lsp::CompletionItem>& shown = cmp_->shown;
    if (row < 0 || (size_t)row >= shown.size() || !cmp_->anchor) {
        closeCompletion();
        return;
    }
    const Lsp::CompletionItem item = shown[(size_t)row];
    GtkTextIter cur, start;
    cursorIter(&cur);
    gtk_text_buffer_get_iter_at_mark(buffer_, &start, cmp_->anchor);
    if (item.hasEdit) {
        // The edit's range was computed on the text as it was when asked;
        // everything before the word is unchanged since, so its start holds.
        GtkTextIter es;
        iterAt(item.editRange.start, &es);
        if (gtk_text_iter_compare(&es, &cur) <= 0) start = es;
    }
    closeCompletion();
    const std::string insert = item.textToInsert();
    gtk_text_buffer_begin_user_action(buffer_);
    gtk_text_buffer_delete(buffer_, &start, &cur);
    gtk_text_buffer_insert(buffer_, &start, insert.c_str(), (int)insert.size());
    gtk_text_buffer_end_user_action(buffer_);
    gtk_text_buffer_place_cursor(buffer_, &start);   // just past the insertion
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(view_), gtk_text_buffer_get_insert(buffer_));
}

bool LspSession::completionVisible() const {
    return cmp_->popover && gtk_widget_get_visible(cmp_->popover);
}

std::vector<std::string> LspSession::completionLabels() const {
    std::vector<std::string> out;
    if (!completionVisible()) return out;
    for (const auto& item : cmp_->shown) out.push_back(item.label);
    return out;
}

bool LspSession::completionKey(guint keyval) {
    if (!completionVisible()) return false;
    switch (keyval) {
    case GDK_KEY_Up: case GDK_KEY_KP_Up:
        moveSelection(-1);
        return true;
    case GDK_KEY_Down: case GDK_KEY_KP_Down:
        moveSelection(1);
        return true;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_ISO_Enter: case GDK_KEY_Tab:
        acceptCompletion(selectedRow());
        return true;
    case GDK_KEY_Escape:
        closeCompletion();
        return true;
    default:
        return false;
    }
}

void LspSession::onFocusLeave(GtkEventControllerFocus*, gpointer selfp) {
    static_cast<LspSession*>(selfp)->closeCompletion();
}

void LspSession::onMarkSet(GtkTextBuffer* buf, GtkTextIter*, GtkTextMark* mark, gpointer selfp) {
    LspSession* self = static_cast<LspSession*>(selfp);
    if (self->cmp_->completing && mark == gtk_text_buffer_get_insert(buf)) self->scheduleRefilter();
}
