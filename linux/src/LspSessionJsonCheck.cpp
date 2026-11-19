// LspSessionJsonCheck.cpp — LspSession's own check of JSON (the Mac's
// Lsp+JsonCheck.mm, #213): a JSON file no language server checks is checked
// by the editor itself, as VS Code's own JSON support does. Its parse error
// (the core's JsonOutline::problems: the place and the message, a trailing
// comma on the comma) goes into the diagnostics store as the list of a
// server of its own, "mutible.json", so it is drawn, listed in Problems and
// counted in the status bar as a server's would be, and follows typing
// until the next check. Checked when the file comes on show and 0.3 s after
// typing, on a thread of its own; nothing once it parses. JSON with comments
// (JsonOutline::allowsComments: tsconfig.json, .vscode's files, .jsonc) may
// have comments and trailing commas. A server that takes the file replaces
// the check, and a tab's list goes when the tab closes.
#include "LspSessionPrivate.h"

#include "JsonOutline.h"

#include <thread>

namespace {

const char* const kJsonServer = "mutible.json";
// Past this many bytes no check: such a file is in large-file mode before
// long, and its parse is a second outline's worth of memory.
const size_t kJsonCheckLimit = 8u << 20;

}  // namespace

const std::string& LspSession::shownUri() const { return uri_.empty() ? json_->uri : uri_; }

void LspSession::jsonCheckDocument() {
    std::string uri = !path_.empty() && JsonOutline::checksAsJson(path_) ? lspui::uriOf(path_)
                                                                         : std::string();
    if (!uri.empty() && (server_ || alsoServer_)) {   // a server's list replaces the editor's
        if (diag_->store.erase(uri, kJsonServer)) problemsChanged();
        uri.clear();
    }
    if (json_->timer) g_source_remove(json_->timer);
    json_->timer = 0;
    json_->generation++;   // an answer on its way was for what was here before
    json_->uri = uri;
    // documentChanged paints what an earlier check found, until this answers.
    checkJsonAfter(0);
}

void LspSession::jsonDocumentClosed() {
    if (json_->timer) g_source_remove(json_->timer);
    json_->timer = 0;
    json_->generation++;
    json_->uri.clear();
}

void LspSession::checkJsonAfter(guint ms) {
    if (json_->uri.empty()) return;
    if (json_->timer) g_source_remove(json_->timer);
    json_->timer = g_timeout_add(ms, [](gpointer p) -> gboolean {
        LspSession* self = static_cast<LspSession*>(p);
        self->json_->timer = 0;
        self->checkJsonNow();
        return G_SOURCE_REMOVE;
    }, this);
}

void LspSession::checkJsonNow() {
    const std::string uri = json_->uri;
    if (uri.empty() || !buffer_) return;
    const uint64_t gen = ++json_->generation;
    std::string text = lspui::bufferText(buffer_);
    if (text.size() > kJsonCheckLimit) {
        jsonChecked({}, uri, nullptr);
        return;
    }
    const bool jsonc = JsonOutline::allowsComments(path_);
    std::weak_ptr<int> alive = life_;
    std::thread([this, alive, uri, gen, jsonc, text = std::move(text)] {
        std::vector<Lsp::Diagnostic> list;
        for (const JsonOutline::Problem& p : JsonOutline::problems(text, jsonc)) {
            Lsp::Diagnostic d;
            d.range.start = Lsp::Position{(int)p.line, (int)p.column};
            d.range.end = Lsp::Position{(int)p.endLine, (int)p.endColumn};
            d.severity = Lsp::Severity::Error;
            d.message = p.message;
            d.source = "json";   // as VS Code names its own
            list.push_back(std::move(d));
        }
        // The text they were found in, so they are drawn where that text is
        // after any typing since (Lsp::mergedDiagnosticSpans).
        std::shared_ptr<const std::u16string> base;
        if (!list.empty()) base = std::make_shared<const std::u16string>(utf16::fromUtf8(text));
        lspui::afterMs(0, [this, alive, uri, gen, list = std::move(list), base] {
            if (alive.expired() || gen != json_->generation) return;   // a newer check is coming
            jsonChecked(list, uri, base);
        });
    }).detach();
}

void LspSession::jsonChecked(const std::vector<Lsp::Diagnostic>& list, const std::string& uri,
                             std::shared_ptr<const std::u16string> base) {
    if (!diag_->store.publish(uri, kJsonServer, list, std::move(base))) return;   // none then, none now
    if (uri == json_->uri) {
        paintDiagnostics();
        updateStatus();
    }
    problemsChanged();
}

void LspSession::forgetJsonChecksOfClosedTabs() {
    bool changed = false;
    for (const std::string& key : diag_->store.uris()) {
        if (!diag_->store.find(key, kJsonServer) || key == json_->uri) continue;
        const std::string path = Lsp::pathFromUri(key);
        if (path.empty() || !documentForPath(path)) changed |= diag_->store.erase(key, kJsonServer);
    }
    if (changed) problemsChanged();
}
