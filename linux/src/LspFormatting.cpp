// LspFormatting.cpp — LspSession: Format Document through the file's
// language server (PARITY.md 2.4), the Mac's Lsp+Formatting.mm:
// textDocument/formatting, or rangeFormatting for a selection where the
// server offers it, for a file no formatter is planned for
// (Formatter::serverMayFormat): a kind of file minicode has no formatter
// for, or Markdown, JSON, YAML and CSS in a project not set up for
// Prettier. A project's own formatter is never replaced by a server's
// (Formatting.cpp asks here only then). The answer is refused if the text
// changed meanwhile, and its edits go in as one undo step.
//
// The Mac applies the answer through its all-or-nothing workspace edit
// applier; this port's (item 2.1) is being built beside this, so the edits,
// which are all in the file on show, go through MultiEdit::apply (D5) on its
// buffer instead. When 2.1's applier lands it should take this over.
#include "Lsp.h"

#include "AutoIndent.h"
#include "MultiEdit.h"
#include "Utf8Offsets.h"

#include <memory>

namespace {

// `fn` on the main loop after `ms` milliseconds (0: the next turn).
void later(guint ms, std::function<void()> fn) {
    auto* f = new std::function<void()>(std::move(fn));
    g_timeout_add(ms, [](gpointer p) -> gboolean {
        auto* f = static_cast<std::function<void()>*>(p);
        (*f)();
        delete f;
        return G_SOURCE_REMOVE;
    }, f);
}

std::string baseName(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

}  // namespace

Lsp::Client* LspSession::formattingClient(bool range, bool* useRange, std::string* name) const {
    std::string n;
    Lsp::Client* c = clientOnShow(&n);
    if (!c) return nullptr;
    // One server a file on this port (the second server is item 2.2).
    Lsp::ServerView v;
    v.main = true;
    v.ready = c->state() == Lsp::Client::State::Ready;
    v.capabilities = &c->capabilities();
    bool ranged = false;
    if (Lsp::routeFormatting({v}, range, &ranged) < 0) return nullptr;
    if (useRange) *useRange = ranged;
    if (name) *name = n;
    return c;
}

bool LspSession::canFormatDocument() const {
    return formattingClient(false, nullptr, nullptr) != nullptr;
}

bool LspSession::formatDocument(long selStart, long selEnd,
                                std::function<void(const std::string& note, bool error)> done) {
    bool useRange = false;
    std::string name;
    Lsp::Client* c = formattingClient(selEnd > selStart, &useRange, &name);
    if (!c || !buffer_) return false;
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
    later(5000, [once, name] { once(name + " did not format the file within 5 seconds.", true); });

    std::weak_ptr<int> alive = life_;
    auto handler = [alive, this, uri, gen, name, before, once](const Json& r, const Json& e) {
        const Json result = r, error = e;
        // Applied once the reply has been read: applying talks to the server.
        later(0, [alive, this, uri, gen, name, before, once, result, error] {
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
            // Every edit is in this file (formattingEdit names only `uri`):
            // its positions to characters, ascending and apart.
            std::vector<MultiEdit::Change> changes;
            for (const Lsp::FileEdit& f : we.files)
                for (const Lsp::TextEdit& t : f.edits) {
                    GtkTextIter a, b;
                    iterAt(t.range.start, &a);
                    iterAt(t.range.end, &b);
                    MultiEdit::Change ch;
                    ch.start = (std::size_t)gtk_text_iter_get_offset(&a);
                    ch.length = (std::size_t)std::max(0, gtk_text_iter_get_offset(&b) -
                                                             gtk_text_iter_get_offset(&a));
                    ch.text = t.newText;
                    changes.push_back(std::move(ch));
                }
            if (!MultiEdit::apply(buffer_, changes, true)) {
                once(name + "'s edits overlap or reach past the end of the file, so none were "
                            "made.",
                     true);
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
