// LspSessionCompletion.cpp — LspSession's completion list (the Mac's
// Lsp+Completion.mm; PARITY.md items 2.0 and 2.5): asked for on Ctrl+Space,
// after a trigger character (".", "->", "::"), and by itself on the first
// letter after one of those and on the second letter of any other word;
// asked again as the word grows when the server called the list
// incomplete; narrowed as the word grows, and closed when the caret leaves
// the word. The list is a popover that never takes the keyboard: the view's
// key handlers hand it Up, Down, Tab, Return and Escape (completionKey,
// through listKey).
//
// Tab and Return take the selected item whenever the list shows, as VS
// Code's defaults do (Lsp::completionKeyAccepts); Shift+Tab and
// Shift+Return close it and go on to the editor. A key pressed before the
// list was filtered for the last letter typed (keys waiting together) has
// it filtered first.
//
// Accepting puts the word in with the item's additionalTextEdits (an
// import, an #include) as one multi-range replacement and one undo step
// (Lsp::completionEdits, MultiEdit::apply). A snippet (insertTextFormat 2)
// goes in expanded first (Snippet::expand: variables, mirrors, the line's
// indentation, the file's unit and line break), and its tab stops are then
// the editor's SnippetSession's; a choice stop's options come back here as
// the list. When the server sends an item's additional edits only from
// completionItem/resolve, the word goes in at once and the edits follow
// when the answer comes, within a second: in the accept's undo step when
// nothing changed meanwhile, else as a step of their own, and only where
// the text they change is still as it was (Lsp::lateAdditionalEdits).
// Nothing waits for the server.
#include "LspSessionPrivate.h"

#include "AutoIndent.h"
#include "Carets.h"
#include "EditSpans.h"
#include "MultiEdit.h"
#include "Snippet.h"
#include "SnippetSession.h"

#include <algorithm>
#include <ctime>

using lspgtk::Server;

namespace {

// The most rows the list shows.
const size_t kMaxShown = 200;
// How long an accepted item's additional edits may take to come from
// completionItem/resolve, after the word went in (the Mac's
// kResolveAfterAccept).
const guint kResolveAfterAcceptMs = 1000;

size_t offsetOf(const GtkTextIter* it) { return (size_t)gtk_text_iter_get_offset(it); }

std::string sliceOf(GtkTextBuffer* b, size_t from, size_t to) {
    GtkTextIter x, y;
    gtk_text_buffer_get_iter_at_offset(b, &x, (int)from);
    gtk_text_buffer_get_iter_at_offset(b, &y, (int)to);
    char* raw = gtk_text_buffer_get_slice(b, &x, &y, TRUE);
    std::string s = raw ? raw : "";
    g_free(raw);
    return s;
}

bool identUnit(char16_t c) {
    if (c == u'_' || c == u'$') return true;
    return (c < 0xD800 || c > 0xDFFF) && g_unichar_isalnum((gunichar)c);
}

// What a snippet's variables and layout are, for one going in at `start`
// (UTF-16 units of `text`), as the Mac's snippetContextAt:.
Snippet::Context snippetContext(GtkWidget* view, const std::u16string& text, size_t start,
                                const std::string& path, const std::string& root, bool asIs) {
    Snippet::Context ctx;
    ctx.filePath = path;
    ctx.workspaceFolder = root;
    start = std::min(start, text.size());
    size_t ls = start, le = start;
    while (ls > 0 && text[ls - 1] != u'\n' && text[ls - 1] != u'\r') ls--;
    while (le < text.size() && text[le] != u'\n' && text[le] != u'\r') le++;
    ctx.currentLine = utf16::toUtf8(text.substr(ls, le - ls));
    size_t w0 = start, w1 = start;
    while (w0 > ls && identUnit(text[w0 - 1])) w0--;
    while (w1 < le && identUnit(text[w1])) w1++;
    ctx.currentWord = utf16::toUtf8(text.substr(w0, w1 - w0));
    long lineIndex = 0;
    for (size_t i = 0; i < ls; i++)
        if (text[i] == u'\n' || (text[i] == u'\r' && (i + 1 >= text.size() || text[i + 1] != u'\n')))
            lineIndex++;
    ctx.lineIndex = lineIndex;
    // The clipboard only when the snippet names $CLIPBOARD, read once. GTK
    // reads another program's clipboard only asynchronously, so this is
    // minicode's own clipboard (a copy made in it), else nothing.
    auto clipboard = std::make_shared<std::pair<bool, std::string>>();
    GdkClipboard* clip = gtk_widget_get_clipboard(view);
    ctx.readClipboard = [clipboard, clip] {
        if (!clipboard->first) {
            clipboard->first = true;
            GdkContentProvider* cp = gdk_clipboard_is_local(clip) ? gdk_clipboard_get_content(clip)
                                                                  : nullptr;
            GValue v = G_VALUE_INIT;
            g_value_init(&v, G_TYPE_STRING);
            if (cp && gdk_content_provider_get_value(cp, &v, nullptr) && g_value_get_string(&v))
                clipboard->second = g_value_get_string(&v);
            g_value_unset(&v);
        }
        return clipboard->second;
    };
    Snippet::commentsFor(ctx.filePath, ctx.lineComment, ctx.blockCommentStart, ctx.blockCommentEnd);
    const time_t now = time(nullptr);
    localtime_r(&now, &ctx.time);
    ctx.timezoneOffsetSeconds = ctx.time.tm_gmtoff;
    ctx.unixSeconds = (int64_t)now;
    ctx.random = [] { return (uint32_t)g_random_int(); };
    // The line's leading blanks (up to where the snippet goes), the file's
    // indentation unit, and its line break.
    if (!asIs) {
        for (size_t i = ls; i < start; i++) {
            if (text[i] != u' ' && text[i] != u'\t') break;
            ctx.indent += text[i];
        }
        ctx.unit = AutoIndent::detect(text, ctx.filePath).text();
    }
    const size_t nl = text.find(u'\n');
    if (nl != std::u16string::npos) ctx.eol = nl > 0 && text[nl - 1] == u'\r' ? u"\r\n" : u"\n";
    else if (text.find(u'\r') != std::u16string::npos) ctx.eol = u"\r";
    return ctx;
}

// A pending accept's buffer watch and marks let go of.
void releasePending(LspSession::CompletionState::Pending& p) {
    if (p.timer) g_source_remove(p.timer);
    p.timer = 0;
    if (!p.buffer) return;
    g_signal_handlers_disconnect_by_data(p.buffer, &p);
    for (GtkTextMark* m : {p.wordStart, p.wordEnd})
        if (m && !gtk_text_mark_get_deleted(m)) gtk_text_buffer_delete_mark(p.buffer, m);
    p.wordStart = p.wordEnd = nullptr;
    g_object_unref(p.buffer);
    p.buffer = nullptr;
}

// The pending accept with this key, out of the list (null when it went).
std::unique_ptr<LspSession::CompletionState::Pending> takePending(LspSession::CompletionState& c,
                                                                  int key) {
    for (auto it = c.pending.begin(); it != c.pending.end(); ++it)
        if ((*it)->key == key) {
            std::unique_ptr<LspSession::CompletionState::Pending> p = std::move(*it);
            c.pending.erase(it);
            return p;
        }
    return nullptr;
}

}  // namespace

// Whether the file on show's server completes: it offers it, or has not
// said yet (a request made while it starts waits for the handshake), as the
// Mac's Lsp::routeRequest decides for the main server.
static bool completes(const std::shared_ptr<Server>& s) {
    if (!s) return false;
    const Lsp::Client& c = s->client();
    return c.state() != Lsp::Client::State::Ready || c.offers(Lsp::Feature::Completion);
}

void LspSession::triggerCompletion() {
    if (noServer()) return;
    std::shared_ptr<lspgtk::Server> server = serverFor(Lsp::Feature::Completion);
    if (!completes(server)) {
        gtk_widget_error_bell(view_);
        note((server ? server : server_)->name() + " can't complete");
        return;
    }
    requestCompletion("");
}

// ---------------------------------------------------------------- asking

// One edit of the file on show, typed (never the session's own, textEdited
// leaves those out). A trigger character's end asks for the list; so does
// the first letter after one, and the second letter of any other word.
// Anything else typed while the list is up narrows it.
void LspSession::completionAfterEdit(const char* inserted, int len) {
    // Not at several carets, nor a rename or a fix going in (MultiEdit): the
    // server sees no character typed alone there, as on the Mac.
    const bool one = inserted && len > 0 && !MultiEdit::applying() &&
                     g_utf8_strlen(inserted, len) == 1 &&
                     completes(serverFor(Lsp::Feature::Completion));
    if (one) {
        const gunichar c = g_utf8_get_char(inserted);
        GtkTextIter cur, prev;
        cursorIter(&cur);
        prev = cur;
        // The caret is already past it (the insert mark moves with text
        // typed at it).
        const bool atCaret = gtk_text_iter_backward_char(&prev) && gtk_text_iter_get_char(&prev) == c;
        if (atCaret) {
            GtkTextIter b = prev;
            const gunichar before = gtk_text_iter_backward_char(&b) ? gtk_text_iter_get_char(&b) : 0;
            std::string trigger;
            std::shared_ptr<lspgtk::Server> completionServer = serverFor(Lsp::Feature::Completion);
            if (c == '.') trigger = ".";
            else if (c == '>' && before == '-') trigger = ">";
            else if (c == ':' && before == ':') trigger = ":";
            if (!trigger.empty()) {
                const std::vector<std::string> triggers = completionServer->client().completionTriggers();
                if (triggers.empty() ||
                    std::find(triggers.begin(), triggers.end(), trigger) != triggers.end()) {
                    requestCompletionSoon(trigger);
                    return;
                }
            }
            if (!cmp_->completing && lspui::isIdent(c)) {
                GtkTextIter start = prev;
                for (GtkTextIter p = start;
                     gtk_text_iter_backward_char(&p) && lspui::isIdent(gtk_text_iter_get_char(&p));)
                    start = p;
                const size_t length = offsetOf(&cur) - offsetOf(&start);
                gunichar b1 = 0, b2 = 0;
                GtkTextIter q = start;
                if (gtk_text_iter_backward_char(&q)) {
                    b1 = gtk_text_iter_get_char(&q);
                    if (gtk_text_iter_backward_char(&q)) b2 = gtk_text_iter_get_char(&q);
                }
                const bool afterAccess = b1 == '.' || (b1 == '>' && b2 == '-') || (b1 == ':' && b2 == ':');
                const gunichar first = gtk_text_iter_get_char(&start);
                const bool number = first >= '0' && first <= '9';
                if (!number && (afterAccess || length == 2)) {
                    requestCompletionSoon("");
                    return;
                }
            }
            // A list the server called incomplete is asked for again as the
            // word grows; the old one stays up, filtered, until the new one
            // is in.
            if (cmp_->completing && cmp_->incomplete && cmp_->gotItems && lspui::isIdent(c))
                requestCompletionSoon("");
        }
    }
    if (cmp_->completing) scheduleRefilter();
}

// Once the text view has finished with the key press.
void LspSession::requestCompletionSoon(const std::string& trigger) {
    if (!cmp_->triggerId || !trigger.empty()) cmp_->pendingTrigger = trigger;
    if (cmp_->triggerId) return;
    cmp_->triggerId = g_idle_add([](gpointer p) -> gboolean {
        LspSession* self = static_cast<LspSession*>(p);
        self->cmp_->triggerId = 0;
        self->requestCompletion(self->cmp_->pendingTrigger);
        return G_SOURCE_REMOVE;
    }, this);
}

void LspSession::cancelCompletionTrigger() {
    if (cmp_->triggerId) g_source_remove(cmp_->triggerId);
    cmp_->triggerId = 0;
}

void LspSession::requestCompletion(const std::string& trigger) {
    // The main server, else the second one when it completes (serverFor).
    std::shared_ptr<lspgtk::Server> server = serverFor(Lsp::Feature::Completion);
    if (!completes(server) || uri_.empty()) return;
    flushChanges();
    GtkTextIter cur, a;
    cursorIter(&cur);
    a = cur;
    for (GtkTextIter p = a;
         gtk_text_iter_backward_char(&p) && lspui::isIdent(gtk_text_iter_get_char(&p));)
        a = p;
    // A new word: the old list goes. The same word asked again (an
    // incomplete list) keeps it showing until the answer.
    bool same = false;
    if (cmp_->completing && cmp_->anchor && !gtk_text_mark_get_deleted(cmp_->anchor)) {
        GtkTextIter old;
        gtk_text_buffer_get_iter_at_mark(buffer_, &old, cmp_->anchor);
        same = gtk_text_iter_equal(&old, &a);
    }
    if (!same) {
        cmp_->items.clear();
        cmp_->gotItems = false;
        cmp_->navigated = false;
    }
    if (!cmp_->anchor) cmp_->anchor = gtk_text_buffer_create_mark(buffer_, nullptr, &a, TRUE);
    else gtk_text_buffer_move_mark(buffer_, cmp_->anchor, &a);
    cmp_->choiceMode = false;
    cmp_->completing = true;
    const int gen = ++cmp_->gen;
    const std::string uri = uri_;
    std::weak_ptr<int> alive = life_;
    server->client().completion(uri, positionOf(&cur),
        [alive, this, gen, uri](const Json& r, const Json&) {
            if (!alive.expired()) completionArrived(r, gen, uri);
        }, trigger);
}

void LspSession::completionArrived(const Json& result, int gen, const std::string& uri) {
    if (gen != cmp_->gen || uri != uri_ || !cmp_->completing) return;
    bool incomplete = false;
    cmp_->items = Lsp::parseCompletion(result, &incomplete);
    cmp_->incomplete = incomplete;
    cmp_->gotItems = true;
    if (cmp_->items.empty()) {
        closeCompletion();
        return;
    }
    refilter();
}

// ---------------------------------------------------------------- the list

std::string LspSession::typedPrefix() const {
    if (!cmp_->anchor || gtk_text_mark_get_deleted(cmp_->anchor) || !buffer_) return "";
    GtkTextIter anc, cur;
    gtk_text_buffer_get_iter_at_mark(buffer_, &anc, cmp_->anchor);
    cursorIter(&cur);
    if (gtk_text_iter_compare(&cur, &anc) < 0) return "";
    char* raw = gtk_text_buffer_get_slice(buffer_, &anc, &cur, TRUE);
    std::string s = raw ? raw : "";
    g_free(raw);
    return s;
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

// The list follows the word being typed: it narrows as it grows (exact,
// then prefix matches, then subsequences) and closes when the caret leaves
// it.
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
    const std::string prefix = typedPrefix();
    // The item highlighted before, by label and text (an incomplete list
    // asked for again numbers its items afresh).
    std::string prevLabel, prevText;
    const int prevRow = completionVisible() ? selectedRow() : -1;
    if (prevRow >= 0 && (size_t)prevRow < cmp_->shown.size()) {
        prevLabel = cmp_->shown[(size_t)prevRow].label;
        prevText = cmp_->shown[(size_t)prevRow].textToInsert();
    }
    std::vector<Lsp::CompletionItem>& shown = cmp_->shown;
    shown = Lsp::filterCompletions(cmp_->items, prefix);
    cmp_->filteredFor = prefix;
    // Nothing to offer, or only exactly what is already typed.
    if (shown.empty() || (shown.size() == 1 && shown[0].textToInsert() == prefix)) {
        closeCompletion();
        return;
    }
    if (shown.size() > kMaxShown) shown.resize(kMaxShown);
    // The best match (filterCompletions puts it first), unless the arrows
    // chose an item that is still listed (the Mac's rule: a highlight kept
    // from a wider list used to stay on an item that no longer fit best).
    int select = 0;
    if (cmp_->navigated) {
        int kept = -1;
        for (size_t i = 0; i < shown.size() && kept < 0; i++)
            if (shown[i].label == prevLabel && shown[i].textToInsert() == prevText) kept = (int)i;
        if (kept >= 0) select = kept;
        else cmp_->navigated = false;
    }
    showList(select, &anc);
}

// The rows of `shown`, the popover by `at`, row `select` highlighted.
void LspSession::showList(int select, const GtkTextIter* at) {
    if (!cmp_->popover) buildPopover();
    gtk_list_box_remove_all(GTK_LIST_BOX(cmp_->list));
    for (const Lsp::CompletionItem& item : cmp_->shown) {
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
        gtk_list_box_append(GTK_LIST_BOX(cmp_->list), label);
        GtkWidget* row = gtk_widget_get_parent(label);
        if (row) gtk_widget_set_focusable(row, FALSE);
    }
    GdkRectangle r;
    caretRect(at, &r);
    gtk_popover_set_pointing_to(GTK_POPOVER(cmp_->popover), &r);
    if (!gtk_widget_get_visible(cmp_->popover)) gtk_popover_popup(GTK_POPOVER(cmp_->popover));
    GtkListBoxRow* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(cmp_->list), select);
    gtk_list_box_select_row(GTK_LIST_BOX(cmp_->list), row);
    GtkAdjustment* adj =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(cmp_->listScroll));
    if (select == 0) gtk_adjustment_set_value(adj, 0);
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
    cmp_->incomplete = false;
    cmp_->navigated = false;
    cmp_->choiceMode = false;
    cmp_->filteredFor.clear();
    cmp_->items.clear();
    cmp_->shown.clear();
    if (cmp_->refilterId) g_source_remove(cmp_->refilterId);
    cmp_->refilterId = 0;
    if (cmp_->choiceCheckId) g_source_remove(cmp_->choiceCheckId);
    cmp_->choiceCheckId = 0;
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

bool LspSession::completionVisible() const {
    return cmp_->popover && gtk_widget_get_visible(cmp_->popover);
}

std::vector<std::string> LspSession::completionLabels() const {
    std::vector<std::string> out;
    if (!completionVisible()) return out;
    for (const auto& item : cmp_->shown) out.push_back(item.label);
    return out;
}

std::string LspSession::completionSelectedLabel() const {
    const int row = completionVisible() ? selectedRow() : -1;
    return row >= 0 && (size_t)row < cmp_->shown.size() ? cmp_->shown[(size_t)row].label : "";
}

bool LspSession::completionKey(guint keyval, GdkModifierType mods) {
    if (!completionVisible()) return false;
    const bool shift = mods & GDK_SHIFT_MASK;
    switch (keyval) {
    case GDK_KEY_Up: case GDK_KEY_KP_Up:
        cmp_->navigated = true;
        moveSelection(-1);
        return true;
    case GDK_KEY_Down: case GDK_KEY_KP_Down:
        cmp_->navigated = true;
        moveSelection(1);
        return true;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_ISO_Enter:
    case GDK_KEY_Tab: case GDK_KEY_KP_Tab: case GDK_KEY_ISO_Left_Tab: {
        const bool ret = keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter ||
                         keyval == GDK_KEY_ISO_Enter;
        const bool backTab = keyval == GDK_KEY_ISO_Left_Tab || (!ret && shift);
        // A letter typed just before the key (a quick typist's keys waiting
        // together) has not been filtered in yet: the list is filtered for
        // the word as it is now first, so the key takes the item for that
        // word, or goes to the editor when nothing is left to take.
        if (!cmp_->choiceMode && cmp_->gotItems && typedPrefix() != cmp_->filteredFor) {
            refilter();
            if (!completionVisible()) return false;
        }
        const int row = selectedRow();
        if (row < 0 || (size_t)row >= cmp_->shown.size()) {
            // Nothing selected: the key is the editor's (Tab indents).
            closeCompletion();
            return false;
        }
        const Lsp::CompletionKey key = ret ? (shift ? Lsp::CompletionKey::ShiftReturn
                                                    : Lsp::CompletionKey::Return)
                                       : backTab ? Lsp::CompletionKey::ShiftTab
                                                 : Lsp::CompletionKey::Tab;
        if (!Lsp::completionKeyAccepts(key)) {
            // Shift+Return and Shift+Tab: the list closes, and the key does
            // what it does in the editor (a new line, an outdent).
            closeCompletion();
            return false;
        }
        acceptCompletion(row);
        return true;
    }
    case GDK_KEY_Escape:
        closeCompletion();
        return true;
    default:
        return false;
    }
}

// The caret moved: the list follows the word, or closes; a choice stop's
// options go once its text is no longer what is selected.
void LspSession::completionFollowsCaret() {
    if (cmp_->choiceMode && completionVisible() && !cmp_->choiceCheckId)
        cmp_->choiceCheckId = g_idle_add([](gpointer p) -> gboolean {
            LspSession* self = static_cast<LspSession*>(p);
            self->cmp_->choiceCheckId = 0;
            Carets* carets = self->editor_->carets();
            if (!self->cmp_->choiceMode || (carets && carets->active()) || !self->buffer_)
                return G_SOURCE_REMOVE;
            GtkTextIter a, b;
            gtk_text_buffer_get_selection_bounds(self->buffer_, &a, &b);
            if (offsetOf(&a) != self->cmp_->choiceStart || offsetOf(&b) != self->cmp_->choiceEnd)
                self->closeCompletion();
            return G_SOURCE_REMOVE;
        }, this);
    if (cmp_->completing) scheduleRefilter();
}

// ---------------------------------------------------------------- accepting

// GtkTextHistory joins an action that only inserts to the typing just
// before it, so one Ctrl+Z would take what was typed away with the word
// (the Mac's accept is a step of its own); an action that also deletes is
// a step of its own. So an accept that only inserts takes the last
// character it put in out and puts it back, in the same action.
bool LspSession::applyAcceptEdits(const std::vector<Lsp::PointEdit>& edits) {
    if (edits.empty() || !buffer_) return false;
    std::vector<MultiEdit::Change> changes;
    bool deletes = false;
    size_t lastEnd = 0;   // where the last non-empty insertion ends, once all are in
    long shift = 0;
    bool inserts = false;
    for (const Lsp::PointEdit& e : edits) {
        changes.push_back({e.start, e.length, e.text});
        const size_t chars = (size_t)g_utf8_strlen(e.text.data(), (gssize)e.text.size());
        if (e.length) deletes = true;
        if (chars) {
            inserts = true;
            lastEnd = (size_t)((long)e.start + shift) + chars;
        }
        shift += (long)chars - (long)e.length;
    }
    gtk_text_buffer_begin_user_action(buffer_);
    const bool ok = MultiEdit::apply(buffer_, changes, false);
    if (ok && !deletes && inserts && lastEnd > 0) {
        const std::string last = sliceOf(buffer_, lastEnd - 1, lastEnd);
        GtkTextIter x, y;
        gtk_text_buffer_get_iter_at_offset(buffer_, &x, (int)(lastEnd - 1));
        gtk_text_buffer_get_iter_at_offset(buffer_, &y, (int)lastEnd);
        gtk_text_buffer_delete(buffer_, &x, &y);
        gtk_text_buffer_insert(buffer_, &x, last.data(), (int)last.size());
    }
    gtk_text_buffer_end_user_action(buffer_);
    return ok;
}

void LspSession::acceptCompletion(int row) {
    if (cmp_->choiceMode) {
        acceptChoice(row);
        return;
    }
    if (row < 0 || (size_t)row >= cmp_->shown.size() || !cmp_->anchor || !buffer_) {
        closeCompletion();
        return;
    }
    const Lsp::CompletionItem item = cmp_->shown[(size_t)row];
    const std::shared_ptr<Server> server = server_;
    GtkTextIter cur, anc;
    cursorIter(&cur);
    gtk_text_buffer_get_iter_at_mark(buffer_, &anc, cmp_->anchor);
    const size_t cursor = offsetOf(&cur);
    const size_t start = std::min(offsetOf(&anc), cursor);
    closeCompletion();
    // Answers for the list still on their way must not bring it back.
    cmp_->gen++;
    // An item whose additional edits come only from resolve
    // (typescript-language-server's auto-imports) goes in now, and the
    // edits follow when the answer comes: nothing waits for the server.
    const bool later =
        server && Lsp::completionWantsResolve(item, server->client().canResolveCompletion());
    const std::u16string text = utf16::fromUtf8(lspui::bufferText(buffer_));
    size_t startU = EditSpans::unitsAt(text, start);
    const size_t cursorU = EditSpans::unitsAt(text, cursor);
    if (item.hasEdit) {
        // The edit's range was computed on the text as it was when asked;
        // everything before the word is unchanged since, so its start holds.
        const size_t editStart = Lsp::offsetForPosition(text, item.editRange.start);
        if (editStart <= cursorU) startU = editStart;
    }
    // A snippet goes in expanded (variables, mirrors, the line's
    // indentation), and its tab stops follow (SnippetSession).
    Snippet::Expansion expansion;
    std::u16string insert;
    if (item.isSnippet) {
        expansion = Snippet::expand(item.textToInsert(),
                                    snippetContext(view_, text, startU, path_, root_, item.insertAsIs));
        insert = expansion.text;
    } else {
        insert = Lsp::toUtf16(item.textToInsert());
    }
    // The word, and the edits that go with it (an import at the top, an
    // #include), as one replacement: one undo step, one change for the
    // highlighter and each for the server.
    const Lsp::CompletionEdits ce =
        Lsp::completionEdits(text, startU, cursorU, insert, item.additionalEdits);
    const std::vector<Lsp::PointEdit> edits = Lsp::pointEdits(text, ce.edits);
    size_t main = 0;
    for (size_t i = 0; i < ce.edits.size(); i++)
        if (ce.edits[i].start == startU && ce.edits[i].length == cursorU - startU &&
            ce.edits[i].text == insert) {
            main = i;
            break;
        }
    if (edits.size() != ce.edits.size() || main >= edits.size()) return;
    // Where the inserted text starts once everything is in.
    long shift = 0;
    for (size_t i = 0; i < main; i++)
        shift += (long)g_utf8_strlen(edits[i].text.data(), (gssize)edits[i].text.size()) -
                 (long)edits[i].length;
    const size_t base = (size_t)((long)edits[main].start + shift);
    const size_t insertChars =
        (size_t)g_utf8_strlen(edits[main].text.data(), (gssize)edits[main].text.size());
    const size_t before = (size_t)gtk_text_buffer_get_char_count(buffer_);
    const std::string removedText =
        sliceOf(buffer_, edits[main].start, edits[main].start + edits[main].length);

    applyingEdit_ = true;
    const bool ok = applyAcceptEdits(edits);
    applyingEdit_ = false;
    if (!ok) return;

    SnippetSession* snippets = editor_->snippets();
    GtkTextIter a, b;
    if (item.isSnippet && expansion.hasStops() && snippets) {
        snippets->start(expansion, base);
    } else if (item.isSnippet) {
        // Only a final stop ($0, maybe inside the text): the caret goes there.
        if (snippets) snippets->end();
        size_t points = 0;
        const Snippet::Expansion e = Snippet::inCodePoints(expansion, &points);
        const Snippet::Range f = e.stops.back().ranges.front();
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)(base + f.start));
        gtk_text_buffer_get_iter_at_offset(buffer_, &b, (int)(base + f.end));
        gtk_text_buffer_select_range(buffer_, &b, &a);
    } else {
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)(base + insertChars));
        gtk_text_buffer_place_cursor(buffer_, &a);
    }
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(view_), gtk_text_buffer_get_insert(buffer_));
    if (later)
        resolveAfterAccepting(item, server, edits[main].start, edits[main].length, removedText,
                              edits[main].text, before);
}

// ---------------------------------------------------------------- a choice stop's options

void LspSession::showSnippetChoices(const std::vector<std::string>& choices, size_t start,
                                    size_t end) {
    closeCompletion();
    if (choices.empty() || !buffer_) return;
    for (const std::string& c : choices) {
        Lsp::CompletionItem it;
        it.label = c;
        it.insertText = c;
        it.index = (int)cmp_->shown.size();
        it.resolved = true;
        cmp_->shown.push_back(it);
    }
    cmp_->choiceMode = true;
    cmp_->choiceStart = start;
    cmp_->choiceEnd = end;
    cmp_->navigated = false;
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(buffer_, &at, (int)start);
    showList(0, &at);
}

// An option chosen: it replaces the stop's text at every caret (its mirrors
// too), and the snippet goes on from there.
void LspSession::acceptChoice(int row) {
    const bool have = row >= 0 && (size_t)row < cmp_->shown.size();
    const std::string choice = have ? cmp_->shown[(size_t)row].label : "";
    closeCompletion();
    if (!have || !buffer_) return;
    Carets* carets = editor_->carets();
    if (carets && carets->active()) {
        carets->insertText(choice);
        return;
    }
    GtkTextIter a, b;
    gtk_text_buffer_get_selection_bounds(buffer_, &a, &b);
    const size_t from = offsetOf(&a), to = offsetOf(&b);
    if (!MultiEdit::apply(buffer_, {{from, to - from, choice}}, false)) return;
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(
        buffer_, &at, (int)(from + (size_t)g_utf8_strlen(choice.data(), (gssize)choice.size())));
    gtk_text_buffer_place_cursor(buffer_, &at);
}

// ---------------------------------------------------------------- additional edits that come later

void LspSession::resolveAfterAccepting(const Lsp::CompletionItem& item,
                                       const std::shared_ptr<Server>& server, size_t start,
                                       size_t removed, const std::string& removedText,
                                       const std::string& insert, size_t before) {
    auto p = std::make_unique<CompletionState::Pending>();
    CompletionState::Pending* raw = p.get();
    p->self = this;
    p->key = ++cmp_->nextPendingKey;
    p->server = server;
    p->uri = uri_;
    p->label = item.label;
    p->buffer = buffer_;
    g_object_ref(buffer_);
    p->docGen = docGen_;
    p->limit = start;
    p->start = start;
    p->removed = removed;
    p->before = before;
    p->removedText = removedText;
    p->insert = insert;
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)start);
    gtk_text_buffer_get_iter_at_offset(
        buffer_, &b, (int)(start + (size_t)g_utf8_strlen(insert.data(), (gssize)insert.size())));
    p->wordStart = gtk_text_buffer_create_mark(buffer_, nullptr, &a, FALSE);
    p->wordEnd = gtk_text_buffer_create_mark(buffer_, nullptr, &b, TRUE);
    // Every edit since moves the limit back to its start: the text before
    // that is still as it was. Before the buffer changes.
    g_signal_connect(buffer_, "insert-text", G_CALLBACK(+[](GtkTextBuffer*, GtkTextIter* at, char*,
                                                            int, gpointer pp) {
        auto* q = static_cast<CompletionState::Pending*>(pp);
        q->limit = std::min(q->limit, offsetOf(at));
    }), raw);
    g_signal_connect(buffer_, "delete-range", G_CALLBACK(+[](GtkTextBuffer*, GtkTextIter* x,
                                                             GtkTextIter* y, gpointer pp) {
        auto* q = static_cast<CompletionState::Pending*>(pp);
        q->limit = std::min(q->limit, std::min(offsetOf(x), offsetOf(y)));
    }), raw);
    g_signal_connect(buffer_, "undo", G_CALLBACK(+[](GtkTextBuffer*, gpointer pp) {
        auto* q = static_cast<CompletionState::Pending*>(pp);
        if (!q->self->cmp_->replaying) q->sawUndo = true;
    }), raw);
    const int key = p->key;
    std::weak_ptr<int> alive = life_;
    p->requestId = server->client().resolveCompletion(item,
        [alive, this, key, item](const Json& result, const Json& error) {
            if (alive.expired()) return;
            Lsp::CompletionItem resolved = item;
            Lsp::applyResolved(resolved, error.isNull() ? result : Json());
            pendingAcceptResolved(key, resolved);
        });
    if (!p->requestId) {
        releasePending(*p);
        return;
    }
    p->timer = g_timeout_add(kResolveAfterAcceptMs, [](gpointer pp) -> gboolean {
        auto* q = static_cast<CompletionState::Pending*>(pp);
        q->timer = 0;
        q->self->pendingAcceptTimedOut(q->key);
        return G_SOURCE_REMOVE;
    }, raw);
    cmp_->pending.push_back(std::move(p));
}

// The document left the view, or the session goes: the answers still on
// their way were for it.
void LspSession::dropPendingAccepts() {
    for (auto& p : cmp_->pending) {
        std::shared_ptr<Server> s = p->server.lock();
        if (s && p->requestId && s->running() && !s->stopping()) s->client().cancel(p->requestId);
        releasePending(*p);
    }
    cmp_->pending.clear();
}

std::size_t LspSession::pendingAccepts() const { return cmp_->pending.size(); }

void LspSession::pendingAcceptTimedOut(int key) {
    std::unique_ptr<CompletionState::Pending> p = takePending(*cmp_, key);
    if (!p) return;
    std::shared_ptr<Server> s = p->server.lock();
    if (s && p->requestId && s->running() && !s->stopping()) s->client().cancel(p->requestId);
    releasePending(*p);
    // The server may have had an import to add with it (its answer would
    // have said): the status bar says so rather than leave it unseen.
    note(p->label + " went in without what the language server may have added with it (an "
                    "import): it did not answer within a second");
}

void LspSession::pendingAcceptResolved(int key, const Lsp::CompletionItem& item) {
    std::unique_ptr<CompletionState::Pending> p = takePending(*cmp_, key);
    if (!p) return;
    // Taken back before the answer came (an undo, and the word no longer
    // where it went in): its import is not wanted either.
    bool undone = false;
    if (p->sawUndo && p->wordStart && p->wordEnd) {
        GtkTextIter x, y;
        gtk_text_buffer_get_iter_at_mark(p->buffer, &x, p->wordStart);
        gtk_text_buffer_get_iter_at_mark(p->buffer, &y, p->wordEnd);
        char* raw = gtk_text_buffer_get_slice(p->buffer, &x, &y, TRUE);
        undone = !raw || p->insert != raw;
        g_free(raw);
    }
    const bool sameBuffer = p->buffer == buffer_;
    releasePending(*p);
    if (item.additionalEdits.empty() || undone) return;
    std::shared_ptr<Server> server = p->server.lock();
    if (!server || server != server_ || uri_ != p->uri || !sameBuffer) return;

    const std::u16string now = utf16::fromUtf8(lspui::bufferText(buffer_));
    std::vector<Lsp::OffsetEdit> units;
    if (!Lsp::lateAdditionalEdits(now, item.additionalEdits, EditSpans::unitsAt(now, p->limit),
                                  units)) {
        note("The language server's addition for " + p->label +
             " (an import) was left out: the text it would change was edited before it "
             "answered");
        return;
    }
    if (units.empty()) return;
    const std::vector<Lsp::PointEdit> late = Lsp::pointEdits(now, units);
    std::vector<EditSpans::Span> spans;
    for (const Lsp::PointEdit& e : late)
        spans.push_back({e.start, e.length, (size_t)g_utf8_strlen(e.text.data(), (gssize)e.text.size())});
    // The carets (a snippet's stops included) stay on their text, all of it
    // after the edits.
    Carets* carets = editor_->carets();
    const std::vector<MultiCaret::Caret> was =
        carets ? carets->current() : std::vector<MultiCaret::Caret>();
    SnippetSession* snippets = editor_->snippets();

    applyingEdit_ = true;
    bool ok = false;
    if (docGen_ == p->docGen && gtk_text_buffer_get_can_undo(buffer_)) {
        // Nothing has changed since the word went in: the same undo step,
        // as on the Mac. GtkTextHistory cannot add to a step once made, so
        // the accept is taken back and put in again with them, as one.
        if (snippets) snippets->hold();
        cmp_->replaying = true;
        gtk_text_buffer_undo(buffer_);
        cmp_->replaying = false;
        if ((size_t)gtk_text_buffer_get_char_count(buffer_) == p->before &&
            sliceOf(buffer_, p->start, p->start + p->removed) == p->removedText) {
            std::vector<Lsp::PointEdit> all = late;
            all.push_back(Lsp::PointEdit{p->start, p->removed, p->insert});
            ok = applyAcceptEdits(all);
        }
        // Not the text the accept was made on (the step undone was another):
        // put back as it was, and the edits go as a step of their own.
        if (!ok) gtk_text_buffer_redo(buffer_);
        if (snippets) snippets->release(ok ? spans : std::vector<EditSpans::Span>());
    }
    // Typing came between: a step of its own, so each undoes in turn.
    if (!ok) ok = applyAcceptEdits(late);
    applyingEdit_ = false;
    if (!ok) return;
    if (carets && !was.empty()) {
        std::vector<MultiCaret::Caret> moved;
        for (const MultiCaret::Caret& c : was)
            moved.push_back({EditSpans::mapThrough(c.anchor, spans), EditSpans::mapThrough(c.head, spans)});
        carets->set(moved);
    }
    if (cmp_->completing) refilter();
}
