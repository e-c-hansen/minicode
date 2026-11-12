// LspSessionActions.cpp — LspSession's rename, references and quick fixes
// (the Mac's Lsp+Actions.mm, PARITY.md item 2.1): F2 asks prepareRename
// where the server offers it ("This can't be renamed") and the new name in
// a box under the old one; Shift+F12 lists every reference in a picker by
// the caret, grouped by file, and a pick goes there (a jump for Go Back
// when it is another file); Ctrl+. lists the server's code actions for the
// caret or the selection, with the diagnostics there as their context, and
// one taken is resolved when it came without its edit, its edit applied,
// then its command run. A server's own workspace/applyEdit (often while
// such a command runs) goes through the same applier
// (LspSessionEdits.cpp). The picker keeps out of the keyboard's way: the
// view's key controller hands it its keys (pickerKey). With a second server
// (PARITY.md 2.2) rename and references go to the main one (serverFor), and
// Ctrl+. asks every server that offers code actions, each with its own
// diagnostics there, and lists them merged, each saying whose it is
// (askCodeActions, which Organize Imports shares).
#include "LspSessionPrivate.h"

#include "LspPicker.h"
#include "MultiEdit.h"

#include <algorithm>

using lspgtk::Server;

namespace {

// A server's error answer as a sentence, or "" when there was none.
std::string errorText(const Json& error) {
    if (error.isNull()) return "";
    std::string m = error["message"].asString();
    return m.empty() ? "The language server refused" : m;
}

std::string escape(const std::string& s) {
    char* e = g_markup_escape_text(s.c_str(), -1);
    std::string out = e ? e : "";
    g_free(e);
    return out;
}

std::string actionKindName(const std::string& kind) {
    if (kind.empty()) return "";
    if (kind.compare(0, 8, "quickfix") == 0) return "fix";
    if (kind.compare(0, 22, "source.organizeImports") == 0) return "imports";
    if (kind.compare(0, 6, "source") == 0) return "source";
    if (kind.compare(0, 8, "refactor") == 0) return "refactor";
    return kind;
}

}  // namespace

// ---- the picker and the rename box

void LspSession::closePicker() {
    if (act_->picker) act_->picker->close();
    act_->pickerKind.clear();
    act_->refRows.clear();
    act_->actions.clear();
    act_->actionServers.clear();
}

void LspSession::closeRenameBox() {
    if (act_->rename) act_->rename->close();
}

// The lists, the hover and the rename box make way for one another.
void LspSession::closeTransients() {
    closeCompletion();
    closeHover();
    closePicker();
    closeRenameBox();
}

// The file left the view: answers still on their way were for it, and the
// rename box goes, unless it is an edit going in that showed another file.
void LspSession::forgetActionRequests() {
    if (applyingEdit_) return;
    closePicker();
    closeRenameBox();
    act_->renameGen++;
    act_->referencesGen++;
    act_->actionGen++;
}

void LspSession::dropActionPopovers() {
    act_->picker.reset();
    act_->rename.reset();
    act_->pickerKind.clear();
}

// The caret moved or the text changed: the list is about where it was.
void LspSession::actionsFollowCaret() {
    if (applyingEdit_ || MultiEdit::applying()) return;
    if (act_->picker && act_->picker->visible()) closePicker();
}

bool LspSession::pickerVisible() const { return act_->picker && act_->picker->visible(); }
std::string LspSession::pickerKind() const { return pickerVisible() ? act_->pickerKind : ""; }
std::vector<std::string> LspSession::pickerRows() const {
    return act_->picker ? act_->picker->rowTexts() : std::vector<std::string>();
}
int LspSession::pickerSelectedRow() const {
    return pickerVisible() ? act_->picker->selectedRow() : -1;
}
GtkWidget* LspSession::pickerRowWidget(int row) const {
    return pickerVisible() ? act_->picker->rowWidget(row) : nullptr;
}
bool LspSession::renameBoxVisible() const { return act_->rename && act_->rename->visible(); }
GtkWidget* LspSession::renameEntry() const {
    return renameBoxVisible() ? act_->rename->entry() : nullptr;
}

// The references or quick fix list's keys: the arrows move through it,
// Page Up and Page Down by ten, Return takes a row, Escape closes it. Any
// other key goes to the editor, and the list closes when the text or the
// caret changes.
bool LspSession::pickerKey(guint keyval) {
    if (!pickerVisible()) return false;
    lspgtk::Picker& p = *act_->picker;
    switch (keyval) {
    case GDK_KEY_Up: case GDK_KEY_KP_Up:
        p.moveSelection(-1);
        return true;
    case GDK_KEY_Down: case GDK_KEY_KP_Down:
        p.moveSelection(1);
        return true;
    case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up:
        p.moveSelection(-10);
        return true;
    case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down:
        p.moveSelection(10);
        return true;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_ISO_Enter:
        acceptPickerRow(p.selectedRow());
        return true;
    case GDK_KEY_Escape:
        closePicker();
        return true;
    default:
        return false;
    }
}

void LspSession::showPicker(const std::vector<lspgtk::PickerRow>& rows, int select, int width,
                            int maxRows) {
    if (!act_->picker) {
        act_->picker = std::make_unique<lspgtk::Picker>(view_);
        act_->picker->onAccept = [this](int row) { acceptPickerRow(row); };
    }
    GtkTextIter cur;
    cursorIter(&cur);
    GdkRectangle at;
    caretRect(&cur, &at);
    act_->picker->show(rows, select, at, width, maxRows);
}

void LspSession::acceptPickerRow(int row) {
    if (!pickerVisible() || row < 0) return;
    if (act_->pickerKind == "references" && (size_t)row < act_->refRows.size() &&
        !act_->refRows[(size_t)row].uri.empty()) {
        const Lsp::Location loc = act_->refRows[(size_t)row];
        closePicker();
        revealLocation(loc);
    } else if (act_->pickerKind == "fixes" && (size_t)row < act_->actions.size()) {
        const Lsp::CodeAction action = act_->actions[(size_t)row];
        std::shared_ptr<Server> server;
        if (action.origin >= 0 && (size_t)action.origin < act_->actionServers.size())
            server = act_->actionServers[(size_t)action.origin].lock();
        closePicker();
        runCodeAction(action, server);
    }
}

// Whether the caret and the text are as they were when a request went out.
bool LspSession::stillAsAsked(int gen, int wanted, const std::string& uri) const {
    if (gen != wanted || uri != uri_ || docGen_ != act_->askedDocGen) return false;
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_mark(buffer_, &a, gtk_text_buffer_get_insert(buffer_));
    gtk_text_buffer_get_iter_at_mark(buffer_, &b, gtk_text_buffer_get_selection_bound(buffer_));
    return gtk_text_iter_get_offset(&a) == act_->askedInsert &&
           gtk_text_iter_get_offset(&b) == act_->askedBound;
}

void LspSession::rememberAsked() {
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_mark(buffer_, &a, gtk_text_buffer_get_insert(buffer_));
    gtk_text_buffer_get_iter_at_mark(buffer_, &b, gtk_text_buffer_get_selection_bound(buffer_));
    act_->askedInsert = gtk_text_iter_get_offset(&a);
    act_->askedBound = gtk_text_iter_get_offset(&b);
    act_->askedDocGen = docGen_;
}

// The file on show's server that answers `what` (the main one, else the
// second one when it offers it), when it is ready; else the bell and a note,
// `missing` saying what it cannot do.
std::shared_ptr<Server> LspSession::serverOffers(Lsp::Feature what, const char* missing) {
    if (noServer()) return nullptr;
    std::shared_ptr<Server> server = serverFor(what);
    // None answers: say so of the main one, or of the only one there is.
    std::shared_ptr<Server> named = server ? server : server_ ? server_ : alsoServer_;
    if (!named) return nullptr;
    if (named->client().state() != Lsp::Client::State::Ready) {
        gtk_widget_error_bell(view_);
        note(named->name() + " is still starting");
        return nullptr;
    }
    if (!server || !server->client().offers(what)) {
        gtk_widget_error_bell(view_);
        note(named->name() + " can't " + missing);
        return nullptr;
    }
    return server;
}

// ---- rename

// The identifier at or just before a character, as character offsets, or
// an empty range at it.
std::pair<int, int> LspSession::nameRangeAt(int offset) const {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buffer_, &it, offset);
    GtkTextIter a = it;
    if (!lspui::isIdent(gtk_text_iter_get_char(&a))) {
        GtkTextIter p = a;
        if (!gtk_text_iter_backward_char(&p) || !lspui::isIdent(gtk_text_iter_get_char(&p)))
            return {offset, offset};
        a = p;
    }
    GtkTextIter b = a;
    for (GtkTextIter p = a;
         gtk_text_iter_backward_char(&p) && lspui::isIdent(gtk_text_iter_get_char(&p));)
        a = p;
    while (!gtk_text_iter_is_end(&b) && lspui::isIdent(gtk_text_iter_get_char(&b)))
        gtk_text_iter_forward_char(&b);
    return {gtk_text_iter_get_offset(&a), gtk_text_iter_get_offset(&b)};
}

void LspSession::renameSymbol() {
    std::shared_ptr<Server> server = serverOffers(Lsp::Feature::Rename, "rename");
    if (!server) return;
    closeTransients();
    flushChanges();
    const int gen = ++act_->renameGen;
    rememberAsked();
    act_->renameServer = server;
    GtkTextIter cur;
    cursorIter(&cur);
    const int offset = gtk_text_iter_get_offset(&cur);
    if (!server->client().canPrepareRename()) {
        const std::pair<int, int> r = nameRangeAt(offset);
        openRename(r.first, r.second, gen, "");
        return;
    }
    const std::string uri = uri_;
    std::weak_ptr<int> alive = life_;
    server->client().prepareRename(uri, positionOf(&cur),
        [alive, this, gen, uri, offset](const Json& r, const Json& e) {
            if (alive.expired() || !stillAsAsked(gen, act_->renameGen, uri)) return;
            const Lsp::PrepareRename p = Lsp::parsePrepareRename(r);
            if (!p.ok) {
                // Null for a keyword, a literal, or a name from a library.
                gtk_widget_error_bell(view_);
                const std::string why = errorText(e);
                note(why.empty() ? "This can't be renamed" : why);
                return;
            }
            std::pair<int, int> range = nameRangeAt(offset);
            if (p.hasRange) {
                GtkTextIter a, b;
                iterAt(p.range.start, &a);
                iterAt(p.range.end, &b);
                if (gtk_text_iter_compare(&a, &b) < 0)
                    range = {gtk_text_iter_get_offset(&a), gtk_text_iter_get_offset(&b)};
            }
            openRename(range.first, range.second, gen, p.placeholder);
        });
}

void LspSession::openRename(int a, int b, int gen, const std::string& placeholder) {
    if (b <= a) {
        gtk_widget_error_bell(view_);
        note("No name at the cursor to rename");
        return;
    }
    act_->renameStart = a;
    act_->renameEnd = b;
    GtkTextIter ia, ib;
    gtk_text_buffer_get_iter_at_offset(buffer_, &ia, a);
    gtk_text_buffer_get_iter_at_offset(buffer_, &ib, b);
    char* raw = gtk_text_buffer_get_slice(buffer_, &ia, &ib, TRUE);
    const std::string old = raw ? raw : "";
    g_free(raw);
    act_->renameOld = old;
    GdkRectangle ra, rb;
    caretRect(&ia, &ra);
    caretRect(&ib, &rb);
    if (rb.y == ra.y && rb.x > ra.x) ra.width = rb.x - ra.x;
    if (!act_->rename) {
        act_->rename = std::make_unique<lspgtk::RenameBox>(view_);
        act_->rename->onClose = [this] {
            if (!gtk_widget_has_focus(view_)) gtk_widget_grab_focus(view_);
        };
    }
    act_->rename->onCommit = [this, gen](const std::string& name) { commitRename(name, gen); };
    act_->rename->show(placeholder.empty() ? old : placeholder, ra);
}

void LspSession::commitRename(const std::string& name, int gen) {
    std::shared_ptr<Server> server = act_->renameServer.lock();
    if (gen != act_->renameGen || !server || (server != server_ && server != alsoServer_) ||
        uri_.empty() || docGen_ != act_->askedDocGen)
        return;
    if (name == act_->renameOld) return;
    // Every tab's text as it is, so uses in unsaved tabs are renamed too.
    flushChanges();
    syncOpenDocuments();
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(buffer_, &at, act_->renameStart);
    // What every open file's version was when asked, to refuse an answer for
    // a file that changed meanwhile (Lsp::staleEditFile).
    auto asked = std::make_shared<const Lsp::DocVersions>(server->client().versions());
    const std::string uri = uri_;
    std::weak_ptr<int> alive = life_;
    std::weak_ptr<Server> weak = server;
    server->client().rename(uri, positionOf(&at), name,
        [alive, this, gen, uri, weak, asked](const Json& r, const Json& e) {
            if (alive.expired()) return;
            // Applied once the answer has been read: applying switches tabs,
            // which talks to the server.
            Json result = r, error = e;
            later([this, gen, uri, weak, asked, result, error] {
                std::shared_ptr<Server> s = weak.lock();
                if (!s || gen != act_->renameGen || uri != uri_ || docGen_ != act_->askedDocGen)
                    return;
                const std::string why = errorText(error);
                if (!why.empty()) {
                    gtk_widget_error_bell(view_);
                    note(why);
                    return;
                }
                const Lsp::WorkspaceEdit we = Lsp::parseWorkspaceEdit(result, lspui::canonicalUri);
                if (we.editCount() == 0 && we.problem.empty()) {
                    gtk_widget_error_bell(view_);
                    note("Nothing to rename");
                    return;
                }
                std::string failure;
                const int files = applyEdit(we, s.get(), asked.get(), &failure);
                if (files < 0) {
                    gtk_widget_error_bell(view_);
                    note(failure);
                    return;
                }
                const size_t n = we.editCount();
                note("Renamed " + std::to_string(n) + " occurrence" + (n == 1 ? "" : "s") +
                     " in " + std::to_string(files) + " file" + (files == 1 ? "" : "s"));
            });
        });
}

// ---- references

void LspSession::findReferences() {
    std::shared_ptr<Server> server = serverOffers(Lsp::Feature::References, "find references");
    if (!server) return;
    closeTransients();
    flushChanges();
    syncOpenDocuments();
    const int gen = ++act_->referencesGen;
    rememberAsked();
    GtkTextIter cur;
    cursorIter(&cur);
    const std::string uri = uri_;
    std::weak_ptr<int> alive = life_;
    server->client().references(uri, positionOf(&cur), true,
        [alive, this, gen, uri](const Json& r, const Json& e) {
            if (!alive.expired()) referencesArrived(r, e, gen, uri);
        });
}

// The text a file has in the editor: its tab's, else the disk's.
std::string LspSession::editorTextOf(const std::string& path) const {
    Document* d = documentForPath(path);
    if (d && d->kind() == Document::Kind::Text && !d->unread() && !d->loading()) return d->text();
    std::string text;
    if (Document::readText(path, text) != Document::Read::Ok) return "";
    return text;
}

void LspSession::referencesArrived(const Json& result, const Json& error, int gen,
                                   const std::string& uri) {
    if (!stillAsAsked(gen, act_->referencesGen, uri)) return;
    const std::string why = errorText(error);
    if (!why.empty()) {
        gtk_widget_error_bell(view_);
        note(why);
        return;
    }
    const std::vector<Lsp::ReferenceFile> files =
        Lsp::groupReferences(Lsp::parseLocations(result), uri_, lspui::canonicalUri);
    if (files.empty()) {
        gtk_widget_error_bell(view_);
        note("No references found");
        return;
    }
    std::vector<lspgtk::PickerRow> rows;
    std::vector<Lsp::Location> where;
    int select = -1;
    GtkTextIter cur;
    cursorIter(&cur);
    const Lsp::Position caret = positionOf(&cur);
    const std::string rootReal = lspui::realPath(root_);
    size_t total = 0;
    for (const Lsp::ReferenceFile& f : files) {
        const std::string path = treePathForUri(f.uri);
        const std::u16string text = utf16::fromUtf8(editorTextOf(path.empty() ? f.uri : path));
        const Lsp::PositionIndex index(text);   // one pass, then a search per reference
        const bool current = sameFile(f.uri, uri_);
        // The heading: the file's name, its folder within the project, and
        // how many references it holds.
        const std::string shown = path.empty() ? f.uri : path;
        const size_t slash = shown.find_last_of('/');
        const std::string name = slash == std::string::npos ? shown : shown.substr(slash + 1);
        std::string folder = slash == std::string::npos ? "" : lspui::realPath(shown.substr(0, slash));
        if (!rootReal.empty() && folder.rfind(rootReal, 0) == 0) {
            folder = folder.substr(rootReal.size());
            if (!folder.empty() && folder[0] == '/') folder = folder.substr(1);
        }
        lspgtk::PickerRow heading;
        heading.heading = true;
        heading.markup = "<b>" + escape(name) + "</b>  <span foreground=\"#9CA3AF\">" +
                         escape(folder) + (folder.empty() ? "" : "  ") +
                         std::to_string(f.ranges.size()) + "</span>";
        rows.push_back(heading);
        where.push_back(Lsp::Location());
        for (const Lsp::Range& r : f.ranges) {
            std::u16string line = Lsp::lineOf(index, r.start.line);
            if (line.size() > 400) line.resize(400);   // a minified line
            // Leading space goes; the match is highlighted where it is.
            size_t indent = 0;
            while (indent < line.size() && (line[indent] == u' ' || line[indent] == u'\t')) indent++;
            const size_t a = std::min((size_t)std::max(r.start.character, 0), line.size());
            const size_t b = r.end.line == r.start.line
                                 ? std::min((size_t)std::max(r.end.character, 0), line.size())
                                 : line.size();
            char number[16];
            g_snprintf(number, sizeof number, "%5d  ", r.start.line + 1);
            std::string markup = "<span foreground=\"#9CA3AF\">" + std::string(number) + "</span>";
            if (b > a && a >= indent) {
                markup += escape(Lsp::toUtf8(line.substr(indent, a - indent))) +
                          "<span background=\"#1F4E79\" foreground=\"#FFFFFF\">" +
                          escape(Lsp::toUtf8(line.substr(a, b - a))) + "</span>" +
                          escape(Lsp::toUtf8(line.substr(b)));
            } else {
                markup += escape(Lsp::toUtf8(line.substr(indent)));
            }
            // The reference under the caret starts selected.
            if (current && select < 0 && !Lsp::positionBefore(caret, r.start) &&
                !Lsp::positionBefore(r.end, caret))
                select = (int)rows.size();
            lspgtk::PickerRow row;
            row.markup = markup;
            rows.push_back(row);
            where.push_back(Lsp::Location{f.uri, r});
            total++;
        }
    }
    act_->refRows = where;
    act_->pickerKind = "references";
    showPicker(rows, select, 620, 14);
    note(std::to_string(total) + " reference" + (total == 1 ? "" : "s") + " in " +
         std::to_string(files.size()) + " file" + (files.size() == 1 ? "" : "s"));
}

// ---- quick fixes

// Every one of the file's servers that offers code actions (or is still
// starting) asked for those at `range`, each with its own diagnostics
// there, which it matches its fixes on; the merged list handed on once all
// have answered, or after 3 s with what has (a silent server holds the
// others up no longer).
void LspSession::askCodeActions(Lsp::Range range, const std::vector<std::string>& only,
                                CodeActionsDone done) {
    // Every server asked has the file's open text first, a second server
    // that started after the file was shown included.
    syncOpenDocuments();
    const std::vector<std::shared_ptr<Server>> servers = serversFor(Lsp::Feature::CodeActions);
    const int gen = ++act_->actionGen;
    rememberAsked();
    const std::string uri = uri_;
    struct Pending {
        std::vector<std::vector<Lsp::CodeAction>> lists;
        std::vector<std::shared_ptr<const Lsp::DocVersions>> versions;
        size_t waiting = 0;
        bool finished = false;
        std::string error;
    };
    auto pending = std::make_shared<Pending>();
    pending->lists.resize(servers.size());
    pending->waiting = servers.size();
    // Each server's open documents' versions as asked: the actions' edits
    // are for those texts (the applier refuses newer ones).
    for (const std::shared_ptr<Server>& server : servers)
        pending->versions.push_back(
            std::make_shared<const Lsp::DocVersions>(server->client().versions()));
    std::weak_ptr<int> alive = life_;
    auto finish = [alive, this, pending, gen, uri, servers, done] {
        if (alive.expired() || pending->finished) return;
        pending->finished = true;
        // Another file, the caret moved, or the text changed: the actions
        // are for what was there before.
        if (!stillAsAsked(gen, act_->actionGen, uri)) return;
        std::vector<Lsp::CodeAction> merged = Lsp::mergeCodeActions(pending->lists);
        for (Lsp::CodeAction& a : merged)
            if (a.origin >= 0 && (size_t)a.origin < pending->versions.size())
                a.asked = pending->versions[(size_t)a.origin];
        done(merged, servers, merged.empty() ? pending->error : std::string());
    };
    for (size_t i = 0; i < servers.size(); i++) {
        const std::shared_ptr<Server>& server = servers[i];
        const Json context = Lsp::diagnosticsInRange(diagnosticsOf(uri, server->key()), range);
        const bool canResolve = server->client().canResolveCodeAction();
        server->client().codeActions(uri, range, context,
            [pending, i, canResolve, finish](const Json& r, const Json& e) {
                if (pending->finished) return;
                const std::string why = errorText(e);
                if (!why.empty()) pending->error = why;
                else pending->lists[i] = Lsp::parseCodeActions(r, canResolve);
                // Once the answer has been read: what follows talks to servers.
                if (--pending->waiting == 0) lspui::afterMs(0, finish);
            }, only);
    }
    if (servers.empty()) lspui::afterMs(0, finish);
    else lspui::afterMs(3000, finish);
}

void LspSession::showQuickFixes() {
    if (!serverOffers(Lsp::Feature::CodeActions, "offer quick fixes")) return;
    closeTransients();
    flushChanges();
    GtkTextIter a, b;
    gtk_text_buffer_get_selection_bounds(buffer_, &a, &b);
    const std::vector<Lsp::Diagnostic> diags = diagnosticsOf(uri_);
    const Lsp::Range range = Lsp::quickFixRange(diags, Lsp::Range{positionOf(&a), positionOf(&b)});
    askCodeActions(range, {}, [this](std::vector<Lsp::CodeAction> actions,
                                     std::vector<std::shared_ptr<Server>> servers,
                                     const std::string& error) {
        if (!error.empty()) {
            gtk_widget_error_bell(view_);
            note(error);
            return;
        }
        if (actions.empty()) {
            note("No quick fixes");
            return;
        }
        std::vector<lspgtk::PickerRow> rows;
        for (const Lsp::CodeAction& a : actions) {
            std::string kind = actionKindName(a.kind);
            if (a.preferred) kind = kind.empty() ? "preferred" : kind + ", preferred";
            // Which server offers it, when the file has more than one.
            if (servers.size() > 1 && a.origin >= 0 && (size_t)a.origin < servers.size()) {
                const std::string& name = servers[(size_t)a.origin]->name();
                kind = kind.empty() ? name : kind + ", " + name;
            }
            lspgtk::PickerRow row;
            row.markup = escape(a.title);
            if (!kind.empty())
                row.markup += "   <span foreground=\"#9CA3AF\">" + escape(kind) + "</span>";
            rows.push_back(row);
        }
        act_->actions = actions;
        act_->actionServers.assign(servers.begin(), servers.end());
        act_->pickerKind = "fixes";
        showPicker(rows, 0, 440, 10);
    });
}

// A chosen action: resolved first when it came without its edit and the
// server can fill it in, then its edit applied and its command run.
void LspSession::runCodeAction(const Lsp::CodeAction& action, std::shared_ptr<Server> server,
                               std::function<void()> then) {
    if (!server || server->stopping() || !server->running()) return;
    std::weak_ptr<Server> weak = server;
    if (!action.hasEdit() && server->client().canResolveCodeAction() && action.raw) {
        std::weak_ptr<int> alive = life_;
        server->client().resolveCodeAction(action, [alive, this, action, weak, then](
                                                       const Json& r, const Json& e) {
            if (alive.expired()) return;
            Lsp::CodeAction done = action;
            if (e.isNull()) {
                const Lsp::CodeAction resolved = Lsp::parseCodeAction(r);
                if (resolved.hasEdit()) done.edit = resolved.edit;
                if (!done.hasCommand() && resolved.hasCommand()) done.command = resolved.command;
            }
            later([this, done, weak, then] { performCodeAction(done, weak.lock(), then); });
        });
        return;
    }
    later([this, action, weak, then] { performCodeAction(action, weak.lock(), then); });
}

void LspSession::performCodeAction(const Lsp::CodeAction& action, std::shared_ptr<Server> server,
                                   std::function<void()> then) {
    if (action.hasEdit()) {
        std::string failure;
        const int n = applyEdit(Lsp::parseWorkspaceEdit(action.edit, lspui::canonicalUri),
                                server.get(), action.asked.get(), &failure);
        if (n < 0) {
            gtk_widget_error_bell(view_);
            note(failure);
            return;
        }
        if (then && !action.hasCommand()) then();
    }
    if (action.hasCommand() && server && server->running() && !server->stopping()) {
        // The server makes its own edits with workspace/applyEdit while it
        // runs the command (serverAppliesEdit).
        std::weak_ptr<int> alive = life_;
        server->client().executeCommand(action.command, [alive, this, then](const Json&,
                                                                            const Json& e) {
            if (alive.expired()) return;
            const std::string why = errorText(e);
            if (!why.empty()) note(why);
            else if (then) later(then);
        });
    } else if (!action.hasEdit()) {
        note("The language server gave nothing to apply");
    }
}

// workspace/applyEdit: a server's own edit, through the same applier, once
// the request has been read; the answer says whether it went in.
void LspSession::serverAppliesEdit(const std::weak_ptr<Server>& weak, const Json& params,
                                   Lsp::Client::ApplyEditReply reply) {
    later([this, weak, params, reply] {
        std::shared_ptr<Server> s = weak.lock();
        if (!s || !s->running()) return;   // its client, which answers, is gone
        if (s->stopping()) {
            reply(false, "the server is stopping");
            return;
        }
        std::string failure;
        const int n = applyEdit(Lsp::parseWorkspaceEdit(params["edit"], lspui::canonicalUri),
                                s.get(), nullptr, &failure);
        if (n < 0) note(failure);
        reply(n >= 0, n < 0 ? failure : "");
    });
}
