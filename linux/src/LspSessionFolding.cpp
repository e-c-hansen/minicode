// LspSessionFolding.cpp — the server's folding ranges (PARITY.md item 2.13),
// the Mac's requestFoldingRanges in Lsp+Sync.mm: textDocument/foldingRange
// for the file on show 0.1 s after it comes on show and 0.6 s after typing
// pauses, from the file's server that offers it (serverFor), its answer
// (Lsp::parseFoldingRanges) handed to the document's folds, which take it
// over Fold::compute's regions only while the text is the one it was asked
// about (DocumentFolds::setServerRanges, Folds.h). The limit on hidden
// lines (D8) is the folds' own, whoever found the regions.
#include "LspSessionPrivate.h"

#include "Document.h"
#include "Folds.h"

using lspgtk::Server;

void LspSession::scheduleFoldingRanges(guint ms) {
    if (fold_->timer) g_source_remove(fold_->timer);
    fold_->timer = g_timeout_add(ms, [](gpointer p) -> gboolean {
        LspSession* self = static_cast<LspSession*>(p);
        self->fold_->timer = 0;
        self->requestFoldingRanges();
        return G_SOURCE_REMOVE;
    }, this);
}

// The file left the view: no question for it goes out, and an answer on
// its way is dropped.
void LspSession::forgetFoldingRanges() {
    if (fold_->timer) g_source_remove(fold_->timer);
    fold_->timer = 0;
    fold_->gen++;
}

void LspSession::requestFoldingRanges() {
    Document* d = editor_->active();
    if (uri_.empty() || !d || d->buffer() != buffer_) return;
    DocumentFolds* folds = d->folds();
    if (!folds || !folds->applies()) return;
    std::shared_ptr<Server> server = serverFor(Lsp::Feature::FoldingRange);
    if (!server || !server->running() || server->stopping()) return;
    // Ready and not offering it: nothing to ask (a starting server is asked
    // once its handshake is through, as the client queues the question).
    if (server->client().state() == Lsp::Client::State::Ready &&
        !server->client().offers(Lsp::Feature::FoldingRange))
        return;
    flushChanges();
    const int gen = ++fold_->gen;
    const uint64_t textGen = folds->generation(), docId = d->id();
    const std::string uri = uri_;
    std::weak_ptr<int> alive = life_;
    server->client().foldingRanges(uri, [alive, this, gen, textGen, docId, uri](const Json& r,
                                                                               const Json& e) {
        if (alive.expired() || gen != fold_->gen || !e.isNull() || uri != uri_) return;
        Document* doc = editor_->active();
        if (!doc || doc->id() != docId || !doc->folds()) return;
        std::vector<Fold::Range> ranges;
        for (const Lsp::FoldingRange& f : Lsp::parseFoldingRanges(r)) {
            if (f.startLine < 0 || f.endLine <= f.startLine) continue;
            ranges.push_back({(size_t)f.startLine, (size_t)f.endLine, Fold::kindFromLsp(f.kind)});
        }
        doc->folds()->setServerRanges(std::move(ranges), textGen);
    });
}
