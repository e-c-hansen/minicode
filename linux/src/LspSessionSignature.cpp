// LspSessionSignature.cpp — LspSession's signature hint (the Mac's
// signature help in Lsp+Completion.mm; PARITY.md item 2.5): while a call's
// arguments are typed, the function's signature above the line, the
// parameter being typed in bold in the accent, "2 of 3" when it has
// several, and the first paragraph of that parameter's documentation (or
// the function's). Opened by the server's trigger characters ("(" and ","
// for most), asked again after a retrigger character or any other edit
// and caret move while it shows (0.15 s after), and closed by Escape, the
// view losing the keyboard, an answer with no signature, or the file
// leaving the view. A popover that never takes the keyboard, as the
// completion list.
#include "LspSessionPrivate.h"

#include "MultiEdit.h"

#include <algorithm>

namespace {

bool contains(const std::vector<std::string>& list, const std::string& s) {
    return std::find(list.begin(), list.end(), s) != list.end();
}

// The first paragraph of some documentation, as one line (Markdown's
// emphasis and code marks left as they are), at most 300 characters.
std::string firstParagraph(const std::string& doc) {
    std::string s = doc;
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    s = s.substr(a);
    const size_t gap = s.find("\n\n");
    if (gap != std::string::npos) s = s.substr(0, gap);
    for (char& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    if (g_utf8_strlen(s.data(), (gssize)s.size()) > 300) {
        const char* end = g_utf8_offset_to_pointer(s.data(), 300);
        s = s.substr(0, (size_t)(end - s.data())) + "\xE2\x80\xA6";
    }
    return s;
}

std::string escaped(const std::string& s) {
    char* e = g_markup_escape_text(s.data(), (gssize)s.size());
    std::string out = e ? e : "";
    g_free(e);
    return out;
}

}  // namespace

bool LspSession::signatureVisible() const {
    return sig_->popover && gtk_widget_get_visible(sig_->popover);
}

std::string LspSession::signatureText() const { return signatureVisible() ? sig_->text : ""; }

// One edit of the file on show, typed (textEdited leaves the session's own
// out): the server's trigger characters open the hint, and any other edit
// brings it up to date while it shows.
void LspSession::signatureAfterEdit(const char* inserted, int len) {
    if (!server_ || !server_->client().hasSignatureHelp()) return;
    Lsp::Client& client = server_->client();
    const bool typedOne = inserted && len > 0 && !MultiEdit::applying() &&
                          g_utf8_strlen(inserted, len) == 1;
    const std::string ch = typedOne ? std::string(inserted, (size_t)len) : "";
    if (typedOne && (contains(client.signatureTriggers(), ch) ||
                     (signatureVisible() && contains(client.signatureRetriggers(), ch)))) {
        later([this, ch] { requestSignature(ch); });
    } else if (signatureVisible()) {
        scheduleSignatureUpdate();
    }
}

// The caret moved: the hint follows it while the view has the keyboard.
void LspSession::signatureFollowsCaret() {
    if (!signatureVisible()) return;
    GtkRoot* root = gtk_widget_get_root(view_);
    if (root && gtk_root_get_focus(root) != view_) closeSignature();
    else scheduleSignatureUpdate();
}

void LspSession::scheduleSignatureUpdate() {
    if (sig_->updateTimer) g_source_remove(sig_->updateTimer);
    sig_->updateTimer = g_timeout_add(150, [](gpointer p) -> gboolean {
        LspSession* self = static_cast<LspSession*>(p);
        self->sig_->updateTimer = 0;
        if (self->signatureVisible()) self->requestSignature("");
        return G_SOURCE_REMOVE;
    }, this);
}

void LspSession::requestSignature(const std::string& trigger) {
    if (!server_ || uri_.empty() || !server_->client().hasSignatureHelp() || !buffer_) return;
    flushChanges();
    const int gen = ++sig_->gen;
    GtkTextIter cur;
    cursorIter(&cur);
    const int cursor = gtk_text_iter_get_offset(&cur);
    const std::string uri = uri_;
    std::weak_ptr<int> alive = life_;
    server_->client().signatureHelp(uri, positionOf(&cur),
        [alive, this, gen, uri, cursor](const Json& r, const Json&) {
            if (!alive.expired()) signatureArrived(r, gen, uri, cursor);
        }, trigger, signatureVisible());
}

void LspSession::signatureArrived(const Json& result, int gen, const std::string& uri, int cursor) {
    if (gen != sig_->gen || uri != uri_ || !buffer_) return;
    const Lsp::SignatureHelp help = Lsp::parseSignatureHelp(result);
    GtkRoot* root = gtk_widget_get_root(view_);
    if (help.signatures.empty() || (root && gtk_root_get_focus(root) != view_) ||
        cursor > gtk_text_buffer_get_char_count(buffer_)) {
        closeSignature();
        return;
    }
    const Lsp::SignatureInfo& sig = help.signatures[(size_t)help.activeSignature];
    const std::string& label = sig.label;
    // The active parameter's place in the label: UTF-16 units, as sent.
    size_t pa = 0, pb = 0;
    const int ap = help.activeParameter;
    if (ap >= 0 && (size_t)ap < sig.params.size()) {
        const std::pair<int, int> r = sig.params[(size_t)ap];
        const size_t units = utf16::fromUtf8(label).size();
        if (r.first >= 0 && r.second > r.first && (size_t)r.second <= units) {
            pa = lspui::byteColumn(label, r.first);
            pb = lspui::byteColumn(label, r.second);
        }
    }
    std::string markup, text;
    if (pb > pa) {
        markup = escaped(label.substr(0, pa)) + "<b><span foreground=\"#4EA1F7\">" +
                 escaped(label.substr(pa, pb - pa)) + "</span></b>" + escaped(label.substr(pb));
    } else {
        markup = escaped(label);
    }
    text = label;
    if (help.signatures.size() > 1) {
        const std::string count = "   " + std::to_string(help.activeSignature + 1) + " of " +
                                  std::to_string(help.signatures.size());
        markup += "<span foreground=\"#9CA3AF\" size=\"small\">" + escaped(count) + "</span>";
        text += count;
    }
    std::string doc;
    if (ap >= 0 && (size_t)ap < sig.paramDocs.size() && !sig.paramDocs[(size_t)ap].empty())
        doc = firstParagraph(sig.paramDocs[(size_t)ap]);
    else if (!sig.documentation.empty())
        doc = firstParagraph(sig.documentation);
    if (!doc.empty()) {
        markup += "\n<span foreground=\"#9CA3AF\" size=\"small\">" + escaped(doc) + "</span>";
        text += "\n" + doc;
    }
    if (!sig_->popover) {
        GtkWidget* popover = gtk_popover_new();
        sig_->popover = popover;
        gtk_widget_add_css_class(popover, "minicode-signature");
        // As the completion list: no autohide, so the keyboard stays in the
        // text view.
        gtk_popover_set_autohide(GTK_POPOVER(popover), FALSE);
        gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
        gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_TOP);
        gtk_widget_set_focusable(popover, FALSE);
        gtk_widget_set_can_focus(popover, FALSE);
        sig_->label = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(sig_->label), 0.0);
        gtk_label_set_wrap(GTK_LABEL(sig_->label), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(sig_->label), 80);
        gtk_popover_set_child(GTK_POPOVER(popover), sig_->label);
        gtk_widget_set_parent(popover, view_);
    }
    gtk_label_set_markup(GTK_LABEL(sig_->label), markup.c_str());
    sig_->text = text;
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(buffer_, &at, cursor);
    GdkRectangle r;
    caretRect(&at, &r);
    gtk_popover_set_pointing_to(GTK_POPOVER(sig_->popover), &r);
    if (!gtk_widget_get_visible(sig_->popover)) gtk_popover_popup(GTK_POPOVER(sig_->popover));
}

void LspSession::closeSignature() {
    if (sig_->updateTimer) g_source_remove(sig_->updateTimer);
    sig_->updateTimer = 0;
    sig_->gen++;
    sig_->text.clear();
    if (signatureVisible()) gtk_popover_popdown(GTK_POPOVER(sig_->popover));
}

// The popover is the view's child only by gtk_widget_set_parent (the trap
// in Lsp.cpp): it goes when the view is unrealized, and is made again.
void LspSession::dropSignaturePopover() {
    closeSignature();
    if (sig_->popover) gtk_widget_unparent(sig_->popover);
    sig_->popover = sig_->label = nullptr;
}
