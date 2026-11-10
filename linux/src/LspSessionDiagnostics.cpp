// LspSessionDiagnostics.cpp — LspSession's diagnostics (the Mac's
// Lsp+Diagnostics.mm): what each open file's server published, kept while
// its tab is open with the text it was computed on, and drawn as squiggles
// (four tags of their own, which the gutter's dots read too) on the file on
// show, placed on its text as it is now (Lsp::diagnosticSpans); a diagnostic
// about text typed over since is left out until the server publishes again.
#include "LspSessionPrivate.h"

#include "EditSpans.h"

#include <algorithm>

using lspgtk::Server;

// Squiggles: tags of their own, never among the highlighter's (it removes
// only its own tags on a retag), so re-highlighting leaves them alone.
// Created from hint up to error, so an error's underline wins where they
// overlap (a later tag has the higher priority).
void LspSession::makeDiagnosticTags() {
    static const struct { const char* name; const char* color; } kinds[4] = {
        {"lsp-error", "#F14C4C"}, {"lsp-warning", "#CCA700"},
        {"lsp-information", "#3794FF"}, {"lsp-hint", "#9CA3AF"}};
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buffer_);
    for (int i = 3; i >= 0; --i) {
        GtkTextTag* t = gtk_text_tag_table_lookup(table, kinds[i].name);
        if (!t) {
            GdkRGBA c;
            gdk_rgba_parse(&c, kinds[i].color);
            t = gtk_text_buffer_create_tag(buffer_, kinds[i].name,
                                           "underline", PANGO_UNDERLINE_ERROR,
                                           "underline-rgba", &c, NULL);
        }
        diag_->tags[i] = t;
    }
}

bool LspSession::sameFile(const std::string& a, const std::string& b) const {
    if (b.empty()) return false;
    if (a == b) return true;
    std::string pa = Lsp::pathFromUri(a), pb = Lsp::pathFromUri(b);
    return !pa.empty() && lspui::realPath(pa) == lspui::realPath(pb);
}

// Each tag comes off its own mark's range, which always holds it (the start
// mark stays left of text typed there, the end mark goes right of it, and
// text typed at a tag's ends is never tagged). Taking every tag off the
// whole buffer, on every tab switch, cost about 25 ms a tag in a file of
// 470,000 lines: GTK queues a redisplay of the whole range for a tag taken
// off, whether it was there or not (PARITY.md 1.12).
void LspSession::clearMarks() {
    if (!buffer_) return;
    for (DiagState::Mark& m : diag_->marks) {
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_mark(buffer_, &a, m.start);
        gtk_text_buffer_get_iter_at_mark(buffer_, &b, m.end);
        gtk_text_buffer_remove_tag(buffer_, diag_->tags[m.severity - 1], &a, &b);
        gtk_text_buffer_delete_mark(buffer_, m.start);
        gtk_text_buffer_delete_mark(buffer_, m.end);
    }
    diag_->marks.clear();
}

// A publish, for a file on show or in the background: kept with the text the
// server had been sent when it computed them (Client::textFor, which is that
// text while this runs), and drawn when it is the file on show.
void LspSession::diagnosticsArrived(Server* s, const std::string& uriIn,
                                    const std::vector<Lsp::Diagnostic>& list) {
    const std::string uri = lspui::canonicalUri(uriIn);
    DiagState::Published& p = diag_->byUri[uri];
    p.list = list;
    p.base = s->client().textFor(uriIn);
    if (sameFile(uri, uri_)) {
        paintDiagnostics();
        updateStatus();
    }
}

void LspSession::forgetDiagnostics(const std::string& uri) {
    diag_->byUri.erase(uri);
    if (!uri_.empty() && sameFile(uri, uri_)) clearMarks();
}

void LspSession::forgetAllDiagnostics() {
    diag_->byUri.clear();
    clearMarks();
}

std::vector<Lsp::Diagnostic> LspSession::diagnosticsOf(const std::string& uri) const {
    auto it = diag_->byUri.find(lspui::canonicalUri(uri));
    return it == diag_->byUri.end() ? std::vector<Lsp::Diagnostic>() : it->second.list;
}

void LspSession::countProblems(int& errors, int& warnings) const {
    errors = warnings = 0;
    auto it = diag_->byUri.find(lspui::canonicalUri(uri_));
    if (it == diag_->byUri.end()) return;
    for (const Lsp::Diagnostic& d : it->second.list) {
        if (d.severity == Lsp::Severity::Error) ++errors;
        else if (d.severity == Lsp::Severity::Warning) ++warnings;
    }
}

// The file on show's squiggles, from what its server published, placed on
// its text as it is now.
void LspSession::paintDiagnostics() {
    clearMarks();
    if (!buffer_ || uri_.empty()) return;
    auto it = diag_->byUri.find(lspui::canonicalUri(uri_));
    if (it == diag_->byUri.end() || it->second.list.empty()) return;
    const std::vector<Lsp::Diagnostic>& list = it->second.list;
    const std::u16string now = utf16::fromUtf8(lspui::bufferText(buffer_));
    const std::vector<Lsp::DiagnosticSpan> spans =
        Lsp::diagnosticSpans(it->second.base.get(), now, list);
    // UTF-16 offsets to characters, in one pass over the text.
    std::vector<size_t> units;
    for (const Lsp::DiagnosticSpan& sp : spans) {
        units.push_back(std::min(sp.start, now.size()));
        units.push_back(std::min(sp.end, now.size()));
    }
    std::sort(units.begin(), units.end());
    units.erase(std::unique(units.begin(), units.end()), units.end());
    const std::vector<size_t> points = EditSpans::codePoints(now, units);
    auto charAt = [&](size_t unit) {
        unit = std::min(unit, now.size());
        const size_t k = (size_t)(std::lower_bound(units.begin(), units.end(), unit) - units.begin());
        return k < points.size() ? points[k] : 0;
    };
    for (const Lsp::DiagnosticSpan& sp : spans) {
        const Lsp::Diagnostic& d = list[sp.index];
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)charAt(sp.start));
        gtk_text_buffer_get_iter_at_offset(buffer_, &b, (int)charAt(sp.end));
        if (gtk_text_iter_compare(&b, &a) <= 0) {
            // An empty range: mark the word there, or at least one character.
            b = a;
            while (!gtk_text_iter_is_end(&b) && lspui::isIdent(gtk_text_iter_get_char(&b)))
                gtk_text_iter_forward_char(&b);
            if (gtk_text_iter_equal(&a, &b)) {
                if (!gtk_text_iter_ends_line(&a)) gtk_text_iter_forward_char(&b);
                else if (!gtk_text_iter_starts_line(&a)) gtk_text_iter_backward_char(&a);
            }
        }
        const int sev = std::min(std::max((int)d.severity, 1), 4);
        gtk_text_buffer_apply_tag(buffer_, diag_->tags[sev - 1], &a, &b);
        DiagState::Mark m;
        // The range stretches when text is typed at either end, the way the
        // Mac's marks do, until the server sends fresh ones.
        m.start = gtk_text_buffer_create_mark(buffer_, nullptr, &a, TRUE);
        m.end = gtk_text_buffer_create_mark(buffer_, nullptr, &b, FALSE);
        m.severity = sev;
        const char* kind = sev == 1 ? "Error" : sev == 2 ? "Warning"
                         : sev == 3 ? "Info" : "Hint";
        m.message = std::string(kind) + ": " + d.message;
        diag_->marks.push_back(m);
    }
}

std::string LspSession::diagnosticAt(const GtkTextIter* it) const {
    std::string out;
    for (const DiagState::Mark& m : diag_->marks) {
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_mark(buffer_, &a, m.start);
        gtk_text_buffer_get_iter_at_mark(buffer_, &b, m.end);
        const bool inside = gtk_text_iter_equal(&a, &b)
            ? gtk_text_iter_equal(it, &a)
            : gtk_text_iter_compare(it, &a) >= 0 && gtk_text_iter_compare(it, &b) < 0;
        if (inside) out += (out.empty() ? "" : "\n") + m.message;
    }
    return out;
}
