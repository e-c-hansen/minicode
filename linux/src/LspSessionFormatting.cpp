// LspSessionFormatting.cpp — LspSession: Format Document through the file's
// language server (PARITY.md 2.4), the Mac's Lsp+Formatting.mm:
// textDocument/formatting, or rangeFormatting for a selection where the
// server offers it, for a file no formatter is planned for
// (Formatter::serverMayFormat): a kind of file minicode has no formatter
// for, or Markdown, JSON, YAML and CSS in a project not set up for
// Prettier. A project's own formatter is never replaced by a server's
// (Formatting.cpp asks here only then). The answer is refused if the text
// changed meanwhile, and its edits go in through the workspace edit applier
// (LspSessionEdits.cpp, item 2.1), as the Mac's go through its own: one
// undo step, refused for a file that moved on since the server was asked.
// It keeps no state of its own.
#include "LspSessionPrivate.h"

#include "AutoIndent.h"

#include <memory>

using lspgtk::Server;

using lspui::afterMs;

namespace {

std::string baseName(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

}  // namespace

// The first of the file's servers (the main one first) that is running and
// formats, a range where one is selected and a server offers that.
std::shared_ptr<Server> LspSession::formattingServer(bool range, bool* useRange) const {
    const std::vector<std::shared_ptr<Server>> servers = fileServers();
    std::vector<Lsp::ServerView> views;
    for (const std::shared_ptr<Server>& s : servers) {
        Lsp::ServerView v;
        v.main = !Lsp::isAlsoKey(s->key());
        v.ready = s->client().state() == Lsp::Client::State::Ready;
        v.capabilities = &s->client().capabilities();
        views.push_back(v);
    }
    bool ranged = false;
    const int i = Lsp::routeFormatting(views, range, &ranged);
    if (i < 0) return nullptr;
    if (useRange) *useRange = ranged;
    return servers[(size_t)i];
}

bool LspSession::canFormatDocument() const { return formattingServer(false, nullptr) != nullptr; }

bool LspSession::formatDocument(long selStart, long selEnd,
                                std::function<void(const std::string& note, bool error)> done) {
    bool useRange = false;
    const std::shared_ptr<Server> formatter = formattingServer(selEnd > selStart, &useRange);
    if (!formatter || !buffer_) return false;
    Lsp::Client* c = &formatter->client();
    const std::string name = formatter->name();
    closeCompletion();
    flushChanges();
    const std::string uri = uri_;
    const int gen = docGen_;
    const std::string before = editor_->text();
    // The file's own indentation, as Return and Tab use it.
    const AutoIndent::Unit unit = AutoIndent::detect(utf16::fromUtf8(before), baseName(path_));

    // Called once: the edit, nothing to change, a refusal, or the deadline
    // (a save waiting on this is never lost to a silent server).
    auto called = std::make_shared<bool>(false);
    auto once = [done, called](const std::string& note, bool error) {
        if (*called) return;
        *called = true;
        done(note, error);
    };
    afterMs(5000, [once, name] { once(name + " did not format the file within 5 seconds.", true); });

    // The server's documents' versions as asked: the applier refuses a file
    // that moved on since.
    std::weak_ptr<Server> asker = formatter;
    auto asked = std::make_shared<const Lsp::DocVersions>(c->versions());
    std::weak_ptr<int> alive = life_;
    auto handler = [alive, this, uri, gen, name, before, once, asker, asked](const Json& r,
                                                                          const Json& e) {
        const Json result = r, error = e;
        // Applied once the reply has been read: applying talks to the server.
        afterMs(0, [alive, this, uri, gen, name, before, once, asker, asked, result, error] {
            if (alive.expired()) {
                once("", false);
                return;
            }
            // Another file, or typed into: the answer is for text no longer there.
            if (uri != uri_ || gen != docGen_ || !buffer_) {
                once("", false);
                return;
            }
            if (!error.isNull()) {
                const std::string m = error["message"].asString();
                once(name + " could not format the file: " +
                         (m.empty() ? std::string("The language server refused") : m),
                     true);
                return;
            }
            const Lsp::WorkspaceEdit we = Lsp::formattingEdit(uri, result);
            if (!we.problem.empty()) {
                once(name + " could not format the file: " + we.problem, true);
                return;
            }
            if (we.editCount() == 0) {
                once("Already formatted (" + name + ").", false);
                return;
            }
            // Every edit is in this file (formattingEdit names only `uri`),
            // through the applier: one multi-range replacement, one undo
            // step, nothing saved.
            std::string failure;
            const std::shared_ptr<Server> server = asker.lock();
            if (applyEdit(we, server.get(), asked.get(), &failure) < 0) {
                once(name + " could not format the file: " + failure, true);
                return;
            }
            if (editor_->text() == before) once("Already formatted (" + name + ").", false);
            else once("", false);
        });
    };
    if (useRange) {
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)selStart);
        gtk_text_buffer_get_iter_at_offset(buffer_, &b, (int)selEnd);
        c->rangeFormatting(uri, Lsp::Range{positionOf(&a), positionOf(&b)}, unit.width, !unit.tabs,
                           handler);
    } else {
        c->formatting(uri, unit.width, !unit.tabs, handler);
    }
    return true;
}
