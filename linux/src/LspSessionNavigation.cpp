// LspSessionNavigation.cpp — LspSession's hover and go to definition (the
// Mac's Lsp+Navigation.mm, without symbols yet): the tooltip under the
// pointer (a diagnostic at once, the server's hover for the word once it
// answers), Ctrl+Alt+I's popover at the caret, and F12 and Ctrl+click,
// within the file or into another through onReveal. Moved here from Lsp.cpp
// unchanged when the session was split (PARITY.md item 2.0).
#include "LspSessionPrivate.h"

#include <algorithm>

using lspgtk::Server;

void LspSession::installNavigation() {
    // Ctrl+click: go to definition.
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(onCtrlClick), this);
    gtk_widget_add_controller(view_, GTK_EVENT_CONTROLLER(click));

    gtk_widget_set_has_tooltip(view_, TRUE);
    g_signal_connect(view_, "query-tooltip", G_CALLBACK(onQueryTooltip), this);
}

const std::string& LspSession::lastHoverText() const { return nav_->lastHover; }

// The text changed or another file came on show: what the tooltip knew is
// for the text before.
void LspSession::forgetHover() {
    nav_->hoverKey = nav_->hoverAsked = -1;
    nav_->hoverText.clear();
}

void LspSession::closeHover() {
    if (nav_->hoverPopover) gtk_popover_popdown(GTK_POPOVER(nav_->hoverPopover));
}

void LspSession::dropHoverPopover() {
    if (nav_->hoverPopover) gtk_widget_unparent(nav_->hoverPopover);
    nav_->hoverPopover = nav_->hoverLabel = nullptr;
}

// ---- hover

std::string LspSession::tooltipAt(const GtkTextIter* itp, bool* requested) {
    if (requested) *requested = false;
    std::string text = diagnosticAt(itp);
    if (server_ && !uri_.empty() && lspui::isIdent(gtk_text_iter_get_char(itp))) {
        GtkTextIter ws = *itp;
        for (GtkTextIter p = ws;
             gtk_text_iter_backward_char(&p) && lspui::isIdent(gtk_text_iter_get_char(&p));)
            ws = p;
        const long key = gtk_text_iter_get_offset(&ws);
        if (key == nav_->hoverKey) {
            if (!nav_->hoverText.empty())
                text += (text.empty() ? "" : "\n\n") + nav_->hoverText;
        } else if (key != nav_->hoverAsked) {
            // Asked once per word; the tooltip is queried again on arrival.
            nav_->hoverAsked = key;
            flushChanges();
            std::weak_ptr<int> alive = life_;
            const int gen = docGen_;
            const std::string uri = uri_;
            server_->client().hover(uri, positionOf(itp),
                [alive, this, gen, uri, key](const Json& r, const Json&) {
                    if (alive.expired() || gen != docGen_ || uri != uri_ ||
                        key != nav_->hoverAsked)
                        return;
                    nav_->hoverKey = key;
                    nav_->hoverText = Lsp::parseHover(r);
                    nav_->hoverAsked = -1;
                    gtk_widget_trigger_tooltip_query(view_);
                });
            if (requested) *requested = true;
        }
    }
    return text;
}

gboolean LspSession::onQueryTooltip(GtkWidget*, int x, int y, gboolean keyboard,
                                    GtkTooltip* tip, gpointer selfp) {
    LspSession* self = static_cast<LspSession*>(selfp);
    if (!self->editor_->isSource()) return FALSE;
    GtkTextIter it;
    if (keyboard) {
        self->cursorIter(&it);
    } else {
        int bx, by;
        gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(self->view_),
                                              GTK_TEXT_WINDOW_WIDGET, x, y, &bx, &by);
        if (!gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(self->view_), &it, bx, by))
            return FALSE;   // past the end of a line, or below the text
    }
    std::string text = self->tooltipAt(&it);
    if (text.empty()) return FALSE;
    char* esc = g_markup_escape_text(text.c_str(), -1);
    std::string markup = std::string("<span font_family=\"monospace\">") + esc + "</span>";
    g_free(esc);
    gtk_tooltip_set_markup(tip, markup.c_str());
    return TRUE;
}

void LspSession::showHoverInfo() {
    if (noServer()) return;
    flushChanges();
    GtkTextIter cur;
    cursorIter(&cur);
    const int gen = ++nav_->hoverGen;
    const std::string uri = uri_;
    const long key = gtk_text_iter_get_offset(&cur);
    std::weak_ptr<int> alive = life_;
    server_->client().hover(uri, positionOf(&cur),
        [alive, this, gen, uri, key](const Json& r, const Json&) {
            if (!alive.expired()) hoverArrived(r, gen, uri, key, true);
        });
}

void LspSession::hoverArrived(const Json& result, int gen, const std::string& uri,
                              long key, bool popup) {
    if (gen != nav_->hoverGen || uri != uri_ || !popup) return;
    std::string text = Lsp::parseHover(result);
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(buffer_, &at, (int)key);
    std::string diag = diagnosticAt(&at);
    if (!diag.empty()) text = text.empty() ? diag : diag + "\n\n" + text;
    nav_->lastHover = text;
    if (text.empty()) {
        flash("No information here");
        return;
    }
    showHoverPopover(text);
}

void LspSession::showHoverPopover(const std::string& text) {
    if (!nav_->hoverPopover) {
        GtkWidget* popover = gtk_popover_new();
        nav_->hoverPopover = popover;
        gtk_widget_add_css_class(popover, "minicode-hover");
        gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
        GtkWidget* scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
        gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), TRUE);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 360);
        nav_->hoverLabel = gtk_label_new("");
        gtk_label_set_wrap(GTK_LABEL(nav_->hoverLabel), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(nav_->hoverLabel), 72);
        gtk_label_set_xalign(GTK_LABEL(nav_->hoverLabel), 0.0);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), nav_->hoverLabel);
        gtk_popover_set_child(GTK_POPOVER(popover), scroll);
        gtk_widget_set_parent(popover, view_);
    }
    gtk_label_set_text(GTK_LABEL(nav_->hoverLabel), text.c_str());
    GtkTextIter cur;
    cursorIter(&cur);
    GdkRectangle r;
    caretRect(&cur, &r);
    gtk_popover_set_pointing_to(GTK_POPOVER(nav_->hoverPopover), &r);
    gtk_popover_popup(GTK_POPOVER(nav_->hoverPopover));
}

// ---- go to definition

void LspSession::goToDefinition() {
    if (noServer()) return;
    GtkTextIter cur;
    cursorIter(&cur);
    definitionAt(&cur);
}

void LspSession::definitionAt(const GtkTextIter* it) {
    if (!server_ || uri_.empty()) return;
    flushChanges();
    const int gen = ++nav_->definitionGen;
    const std::string uri = uri_;
    std::weak_ptr<int> alive = life_;
    server_->client().definition(uri, positionOf(it),
        [alive, this, gen, uri](const Json& r, const Json&) {
            if (!alive.expired()) definitionArrived(r, gen, uri);
        });
}

void LspSession::definitionArrived(const Json& result, int gen, const std::string& uri) {
    if (gen != nav_->definitionGen || uri != uri_) return;
    std::vector<Lsp::Location> locs = Lsp::parseLocations(result);
    if (locs.empty()) {
        gtk_widget_error_bell(view_);
        flash("No definition found");
        return;
    }
    revealLocation(locs[0]);
}

// Within the file on show, the editor's own reveal (the ten-line rule of Go
// Back's history records a far one); another file through onReveal, as a
// Find in Folder match opens, a jump.
void LspSession::revealLocation(const Lsp::Location& loc) {
    const std::string target = Lsp::pathFromUri(loc.uri);
    if (target.empty()) return;
    const Lsp::Position s = loc.range.start, e = loc.range.end;

    if (sameFile(loc.uri, uri_)) {
        // The buffer, not the disk: it may hold unsaved edits.
        GtkTextIter ls, le;
        gtk_text_buffer_get_iter_at_line(buffer_, &ls, std::max(0, s.line));
        le = ls;
        if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
        char* raw = gtk_text_buffer_get_slice(buffer_, &ls, &le, TRUE);
        const std::string line = raw ? raw : "";
        g_free(raw);
        const size_t a = lspui::byteColumn(line, s.character);
        const size_t b = e.line == s.line ? lspui::byteColumn(line, e.character) : a;
        editor_->revealLine(s.line + 1, a, b > a ? b - a : 0);
        return;
    }

    // Another file: opened under the tree's own spelling of the path when it
    // is inside the folder (clangd answers with resolved paths), then the
    // same open-and-reveal as a Find in Folder match. Its text is its tab's
    // when it has one (unsaved changes move the lines), else the disk's.
    const std::string path = treePathForUri(loc.uri);
    const std::string content = editorTextOf(path);
    if (content.empty() && !g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) return;
    std::string line;
    size_t a = 0, b = 0;
    if (lspui::lineOf(content, s.line, line)) {
        a = lspui::byteColumn(line, s.character);
        b = e.line == s.line ? lspui::byteColumn(line, e.character) : a;
    }
    if (onReveal) onReveal(path, s.line + 1, a, b > a ? b - a : 0);
}

void LspSession::onCtrlClick(GtkGestureClick* g, int n, double x, double y, gpointer selfp) {
    LspSession* self = static_cast<LspSession*>(selfp);
    GdkModifierType state =
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    if (n != 1 || !(state & GDK_CONTROL_MASK) || !self->server_ || self->uri_.empty()) return;
    int bx, by;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(self->view_), GTK_TEXT_WINDOW_WIDGET,
                                          (int)x, (int)y, &bx, &by);
    GtkTextIter it;
    if (!gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(self->view_), &it, bx, by)) return;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_text_buffer_place_cursor(self->buffer_, &it);
    gtk_widget_grab_focus(self->view_);
    self->definitionAt(&it);
}
