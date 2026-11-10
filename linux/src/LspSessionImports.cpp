// LspSessionImports.cpp — LspSession's Organize Imports (PARITY.md item 2.2),
// the Mac's in Lsp+Actions.mm: Shift+Alt+O (VS Code's Linux key) asks the
// file's servers for their source.organizeImports actions for the whole
// file (askCodeActions, LspSessionActions.cpp) and runs one at once: a
// tool's own (source.organizeImports.ruff) before a general server's bare
// one (Lsp::pickOrganizeImports), through the same all-or-nothing applier
// as a quick fix. editor.organize-imports-on-save does the same before
// Ctrl+S formats and writes (Formatting.cpp), never on auto-save. It keeps
// no state of its own.
#include "LspSessionPrivate.h"

using lspgtk::Server;

namespace {

// The whole of a buffer as an LSP range.
Lsp::Range wholeText(GtkTextBuffer* b) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(b, &end);
    GtkTextIter ls = end;
    gtk_text_iter_set_line_offset(&ls, 0);
    char* s = gtk_text_buffer_get_slice(b, &ls, &end, TRUE);
    Lsp::Range r;
    r.end.line = gtk_text_iter_get_line(&end);
    r.end.character = (int)utf16::fromUtf8(s ? s : "").size();
    g_free(s);
    return r;
}

}  // namespace

void LspSession::organizeImports() {
    if (!serverOffers(Lsp::Feature::CodeActions, "organize imports")) return;
    closeTransients();
    flushChanges();
    const std::string uri = uri_;
    std::weak_ptr<int> alive = life_;
    askCodeActions(wholeText(buffer_), {"source.organizeImports"},
        [alive, this, uri](std::vector<Lsp::CodeAction> actions,
                           std::vector<std::shared_ptr<Server>> servers, const std::string& error) {
            if (alive.expired()) return;
            const int i = Lsp::pickOrganizeImports(actions);
            if (i < 0) {
                gtk_widget_error_bell(view_);
                note(error.empty() ? "The language servers have no way to organize these imports"
                                   : error);
                return;
            }
            const Lsp::CodeAction action = actions[(size_t)i];
            std::shared_ptr<Server> server =
                action.origin >= 0 && (size_t)action.origin < servers.size()
                    ? servers[(size_t)action.origin] : nullptr;
            const std::string name = server ? server->name() : std::string();
            const std::string before = editor_->text();
            runCodeAction(action, server, [alive, this, uri, before, name] {
                if (alive.expired() || uri != uri_) return;
                if (editor_->text() == before) note("The imports are in order");
                else note("Organized imports (" + name + ")");
            });
        });
}

bool LspSession::organizeImportsBeforeSave(std::function<void()> done) {
    if (uri_.empty() || !buffer_ || serversFor(Lsp::Feature::CodeActions).empty()) return false;
    closeTransients();
    flushChanges();
    // Called once, whichever comes first: the edit, nothing to do, or the
    // deadline (askCodeActions gives a silent server 3 s; a resolve or a
    // command may take longer still).
    auto called = std::make_shared<bool>(false);
    auto once = [called, done] {
        if (*called) return;
        *called = true;
        done();
    };
    lspui::afterMs(5000, once);
    const std::string uri = uri_;
    std::weak_ptr<int> alive = life_;
    askCodeActions(wholeText(buffer_), {"source.organizeImports"},
        [alive, this, uri, once, called](std::vector<Lsp::CodeAction> actions,
                                         std::vector<std::shared_ptr<Server>> servers,
                                         const std::string& error) {
            const int i = Lsp::pickOrganizeImports(actions);
            if (alive.expired() || *called || i < 0) {
                if (!alive.expired() && !error.empty() && !*called) note(error);
                once();
                return;
            }
            const Lsp::CodeAction action = actions[(size_t)i];
            std::shared_ptr<Server> server =
                action.origin >= 0 && (size_t)action.origin < servers.size()
                    ? servers[(size_t)action.origin] : nullptr;
            const std::string name = server ? server->name() : std::string();
            const std::string before = editor_->text();
            runCodeAction(action, server, [alive, this, before, name, once, called] {
                if (!alive.expired() && !*called && editor_->text() != before)
                    note("Organized imports (" + name + ")");
                once();
            });
        });
    return true;
}
