// LspSessionProblems.cpp — LspSession's Problems list and F8 (PARITY.md item
// 2.2), the Mac's in Lsp+Diagnostics.mm: every diagnostic the window's
// servers published, as rows for ProblemsPanel (a heading per file, files
// with errors first, then each file's problems by severity, line and
// column: Lsp::problemRows), placed in the text as it is now for a file
// open in a tab (Lsp::mergedDiagnosticSpans, leaving out ones about text
// typed over since), as the server said for a file no tab has. Hints are
// left out, as VS Code leaves them out of its Problems: pyright sends one
// for every name not used yet, which would drown the rest. Each file's rows
// are kept until its diagnostics change or its text does, so a publish for
// one file rebuilds that file's rows and not every file's.
//
// F8 and Shift+F8 step through the squiggles drawn on the file on show
// (Lsp::stepProblem: wrapping, and through several at one place), each
// problem's message in the hover box at the caret.
#include "LspSessionPrivate.h"

#include <set>

using lspgtk::Server;

// Something changed: onProblemsChanged once things settle, 50 ms on.
void LspSession::problemsChanged() {
    if (probs_->reportTimer) g_source_remove(probs_->reportTimer);
    probs_->reportTimer = g_timeout_add(50, [](gpointer p) -> gboolean {
        LspSession* self = static_cast<LspSession*>(p);
        self->probs_->reportTimer = 0;
        if (self->onProblemsChanged) self->onProblemsChanged();
        return G_SOURCE_REMOVE;
    }, this);
}

std::vector<Lsp::Problem> LspSession::collectProblems() {
    std::vector<Lsp::Problem> out;
    const Lsp::DiagnosticStore& store = diagnosticStore();
    std::set<std::string> seen;
    for (const std::string& uri : store.uris()) {
        seen.insert(uri);
        ProblemsState::File& file = probs_->files[uri];
        const uint64_t gen = store.generation(uri);
        if (file.generation == 0) file.path = treePathForUri(uri);   // two realpaths, once
        Document* d = file.path.empty() ? nullptr : documentForPath(file.path);
        const bool inTab = d && d->kind() == Document::Kind::Text && !d->unread() && !d->loading();
        const uint64_t docId = inTab ? d->id() : 0;
        const uint64_t edits = inTab ? documentEdits(docId) : 0;
        const bool fresh = file.generation == gen && file.inTab == inTab &&
                           (!inTab || (file.docId == docId && file.edits == edits));
        if (!fresh) {
            file.generation = gen;
            file.inTab = inTab;
            file.docId = docId;
            file.edits = edits;
            file.problems.clear();
            if (!file.path.empty()) {
                auto add = [&](const Lsp::Diagnostic& diag, Lsp::Position a, Lsp::Position b) {
                    if (diag.severity == Lsp::Severity::Hint) return;
                    Lsp::Problem p;
                    p.path = file.path;
                    p.line = a.line;
                    p.column = a.character;
                    p.endLine = b.line;
                    p.endColumn = b.character;
                    p.severity = diag.severity;
                    p.message = diag.message;
                    p.source = diag.source;
                    p.code = diag.code;
                    file.problems.push_back(p);
                };
                const std::vector<const Lsp::DiagnosticStore::Set*> sets = store.forUri(uri);
                if (!inTab) {   // a file no tab has: as the server said
                    for (const Lsp::DiagnosticStore::Set* set : sets)
                        for (const Lsp::Diagnostic& diag : set->list)
                            add(diag, diag.range.start, diag.range.end);
                } else {
                    const std::u16string now = utf16::fromUtf8(d->text());
                    const Lsp::PositionIndex index(now);
                    for (const Lsp::MergedSpan& sp : Lsp::mergedDiagnosticSpans(sets, now))
                        add(*sp.diag, index.position(sp.start), index.position(sp.end));
                }
            }
        }
        out.insert(out.end(), file.problems.begin(), file.problems.end());
    }
    for (auto it = probs_->files.begin(); it != probs_->files.end();) {
        if (seen.count(it->first)) ++it;
        else it = probs_->files.erase(it);
    }
    return out;
}

std::vector<LspProblem> LspSession::problemRows() {
    const std::vector<Lsp::Problem> problems = collectProblems();
    std::vector<LspProblem> rows;
    for (const Lsp::ProblemRow& r : Lsp::problemRows(problems)) {
        LspProblem p;
        p.heading = r.heading;
        p.path = r.path;
        if (r.heading) {
            p.errors = r.errors;
            p.warnings = r.warnings;
            p.others = r.others;
        } else {
            const Lsp::Problem& q = problems[r.problem];
            p.path = q.path;
            p.line = q.line;
            p.column = q.column;
            p.endLine = q.endLine;
            p.endColumn = q.endColumn;
            p.severity = (int)q.severity;
            p.fullMessage = q.message;
            p.message = Lsp::problemOneLine(q.message);
            p.source = q.source;
            p.code = q.code;
        }
        rows.push_back(std::move(p));
    }
    return rows;
}

// What the list shows, counted: diagnostics about text typed over since
// they came are not counted, as they are not listed (each file's rows are
// kept, so this reads only files whose text or diagnostics moved).
std::string LspSession::problemCountsText() {
    return Lsp::problemCountsText(Lsp::countProblems(collectProblems()));
}
int LspSession::problemErrors() { return Lsp::countProblems(collectProblems()).errors; }
int LspSession::problemWarnings() { return Lsp::countProblems(collectProblems()).warnings; }

bool LspSession::problemsKnown() const {
    if (!diagnosticStore().empty()) return true;
    for (const auto& [key, s] : servers_)
        if (s && s->running() && !s->stopping()) return true;
    return false;
}

void LspSession::openProblem(const LspProblem& problem) {
    if (problem.heading || problem.path.empty()) return;
    Lsp::Location loc;
    loc.uri = lspui::uriOf(problem.path);
    loc.range.start = Lsp::Position{problem.line, problem.column};
    loc.range.end = Lsp::Position{problem.endLine, problem.endColumn};
    revealLocation(loc);
}

void LspSession::nextProblem() { goToProblem(true); }
void LspSession::previousProblem() { goToProblem(false); }

// The squiggles drawn now (they follow typing), hints left out as in the
// list; from the selection's start, wrapping.
void LspSession::goToProblem(bool forward) {
    uint64_t gen = 0;
    const std::vector<std::pair<size_t, size_t>> spans = problemMarks(&gen);
    if (uri_.empty() || spans.empty() || !buffer_) {
        gtk_widget_error_bell(view_);
        flash("No problems in this file");
        return;
    }
    GtkTextIter a, b;
    gtk_text_buffer_get_selection_bounds(buffer_, &a, &b);
    const long last = gen == probs_->stepMarks ? probs_->stepIndex : -1;
    const long i = Lsp::stepProblem(spans, (size_t)gtk_text_iter_get_offset(&a), last, forward);
    if (i < 0) return;
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(buffer_, &at, (int)spans[(size_t)i].first);
    const int line = gtk_text_iter_get_line(&at);
    const size_t byteColumn = (size_t)gtk_text_iter_get_line_index(&at);
    closeHover();
    editor_->revealLine(line + 1, byteColumn, 0);   // closes what was showing
    probs_->stepMarks = gen;
    probs_->stepIndex = i;
    // Its message (and any other problem there) in the box at the caret,
    // which the next caret move closes.
    gtk_text_buffer_get_iter_at_offset(buffer_, &at, (int)spans[(size_t)i].first);
    const std::string text = diagnosticAt(&at);
    if (!text.empty()) showProblemNote(text);
}
