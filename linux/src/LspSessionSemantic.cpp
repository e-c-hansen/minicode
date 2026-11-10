// LspSessionSemantic.cpp — LspSession's semantic tokens (the Mac's
// Lsp+Semantic.mm, PARITY.md item 2.6): the server's tokens for the file on
// show (textDocument/semanticTokens/full, then /full/delta from the last
// answer when the server offers it), asked for as VS Code's
// editor.semanticHighlighting asks: when the file is shown or its server
// becomes ready, 0.3 s after typing stops, and when the server says they
// changed (workspace/semanticTokens/refresh). One request is out at a time.
// An answer is decoded against the text it was computed on (the client's
// copy, Client::textFor) and handed to the document (Document::
// takeSemanticSpans, in UTF-8 bytes) only while no edit came since; the
// document paints the lines on screen (SemanticColors.h).
#include "LspSessionPrivate.h"

using lspgtk::Server;

std::size_t LspSession::semanticRequests() const { return sem_->requests; }
std::size_t LspSession::semanticDeltaRequests() const { return sem_->deltas; }
std::size_t LspSession::semanticAnswers() const { return sem_->answers; }

bool LspSession::semanticWanted() const {
    Document* d = editor_->active();
    return !uri_.empty() && (server_ || alsoServer_) && settings_.semanticHighlighting() && d &&
           editor_->isSource() && d->buffer() == buffer_ && d->highlighter();
}

void LspSession::scheduleSemanticTokens(guint ms) {
    if (sem_->timer) g_source_remove(sem_->timer);
    sem_->timer = 0;
    // Whether it is wanted is asked when it fires: the server may become
    // ready, and the highlighter catch up, meanwhile.
    if (uri_.empty() || !settings_.semanticHighlighting()) return;
    sem_->timer = g_timeout_add(ms, [](gpointer p) -> gboolean {
        LspSession* self = static_cast<LspSession*>(p);
        self->sem_->timer = 0;
        self->requestSemanticTokens();
        return G_SOURCE_REMOVE;
    }, this);
}

void LspSession::requestSemanticTokens() {
    if (!semanticWanted()) return;
    std::shared_ptr<Server> server = serverFor(Lsp::Feature::SemanticTokens);
    // A server still starting is asked once it is ready (semanticServerChanged).
    if (!server || server->stopping() ||
        server->client().state() != Lsp::Client::State::Ready ||
        !server->client().offers(Lsp::Feature::SemanticTokens))
        return;
    // One that can no longer be answered (its server went, or has said
    // nothing for 10 s) is let go; else its answer asks again.
    if (sem_->request) {
        std::shared_ptr<Server> asked = sem_->server.lock();
        if (asked == server && !asked->stopping() &&
            g_get_monotonic_time() - sem_->askedAt < 10 * G_USEC_PER_SEC) {
            sem_->again = true;
            return;
        }
        if (asked && !asked->stopping()) asked->client().cancel(sem_->request);
        sem_->request = 0;
        sem_->tokens = Lsp::SemanticTokens();
    }
    flushChanges();
    if (server != sem_->server.lock() || uri_ != sem_->uri) {
        sem_->tokens = Lsp::SemanticTokens();
        sem_->server = server;
        sem_->uri = uri_;
    }
    const std::string uri = uri_;
    const int gen = docGen_;
    std::weak_ptr<int> alive = life_;
    std::weak_ptr<Server> weak = server;
    auto answered = [alive, this, weak, uri, gen](Json& result, const Json& error) {
        std::shared_ptr<Server> s = weak.lock();
        if (alive.expired() || !s) return;
        semanticAnswer(result, error, s.get(), uri, gen);
    };
    Lsp::Client& client = server->client();
    const bool delta =
        !sem_->tokens.resultId.empty() && Lsp::offersSemanticTokensDelta(client.capabilities());
    sem_->request = delta ? client.semanticTokensDelta(uri, sem_->tokens.resultId, answered)
                          : client.semanticTokensFull(uri, answered);
    if (sem_->request) {
        sem_->requests++;
        if (delta) sem_->deltas++;
    }
    sem_->askedAt = g_get_monotonic_time();
}

void LspSession::semanticAnswer(Json& result, const Json& error, Server* server,
                                const std::string& uri, int gen) {
    // A server on its way out answers nothing anyone waits for: a request
    // given up on is cancelled, except on such a server.
    if (server->stopping()) return;
    sem_->request = 0;
    const bool again = sem_->again;
    sem_->again = false;
    // Another file or server since: what replaced it asks for itself.
    if (server != sem_->server.lock().get() || uri != sem_->uri) return;
    // The tokens are kept even when the text has moved on, since the
    // server's next delta is from this answer. One it could not make
    // (ContentModified, a failure) or that does not fit is forgotten, and
    // the next request asks for the whole document.
    const bool understood = error.isNull() && sem_->tokens.apply(result);
    if (!understood) sem_->tokens = Lsp::SemanticTokens();
    if (again) {
        scheduleSemanticTokens(0);
        return;
    }
    // Typing since the request: its own request, a moment after it stops,
    // brings the tokens for the new text.
    if (!understood || gen != docGen_ || uri != uri_ || !semanticWanted()) return;
    std::shared_ptr<const std::u16string> text = server->client().textFor(uri);
    if (!text) return;
    const Lsp::SemanticLegend legend = Lsp::SemanticLegend::from(server->client().capabilities());
    const Lsp::PositionIndex index(*text);
    size_t bytes = 0;
    std::vector<Lsp::SemanticSpan> spans = Lsp::semanticSpansInUtf8(
        *text, Lsp::decodeSemanticTokens(sem_->tokens.data, legend, index), &bytes);
    if (editor_->active()->takeSemanticSpans(std::move(spans), bytes)) sem_->answers++;
}

void LspSession::forgetSemanticTokens() {
    if (sem_->timer) g_source_remove(sem_->timer);
    sem_->timer = 0;
    std::shared_ptr<Server> server = sem_->server.lock();
    if (sem_->request && server && !server->stopping()) server->client().cancel(sem_->request);
    sem_->request = 0;
    sem_->again = false;
    sem_->tokens = Lsp::SemanticTokens();
    sem_->server.reset();
    sem_->uri.clear();
}

void LspSession::semanticServerChanged(Server* s) {
    if (s && (s == server_.get() || s == alsoServer_.get())) scheduleSemanticTokens(0);
}

// Turned on asks; turned off, every document drops its colors itself
// (Document::semanticSettingChanged, from the editor's settings).
void LspSession::semanticSettingChanged(bool before) {
    const bool now = settings_.semanticHighlighting();
    if (now == before) return;
    if (now) scheduleSemanticTokens(0);
    else forgetSemanticTokens();
}
