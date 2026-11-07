// Lsp+Diagnostics.mm — Objective-C++. LspSession (Diagnostics): what the
// servers publish (kept per file and server in the store), the squiggles
// drawn on the text as it is now, the Problems list over every file, and
// F8 / Shift+F8.
#import "Lsp+Private.h"
#import "Lsp+DiagnosticsIvars.h"

@implementation MCProblem
@end

// One file's rows of the Problems list, kept until its diagnostics change
// (the store's generation) or the text they were placed in does, so a
// publish for one file rebuilds that file's rows and not every file's.
@interface MCProblemFile : NSObject {
@public
    std::vector<Lsp::Problem> problems;
    uint64_t generation;
}
@property(nonatomic, copy) NSString *path;   // the tree's spelling, nil when none
@property(nonatomic, copy) NSString *text;   // what they were placed in; nil: as published
@end
@implementation MCProblemFile
@end

@implementation LspSession (Diagnostics)

// ---- diagnostics
- (BOOL)uri:(const std::string &)a isSameFileAs:(NSString *)b {
    if (!b) return NO;
    if (NS(a) && [NS(a) isEqualToString:b]) return YES;
    NSString *pa = NS(Lsp::pathFromUri(a)), *pb = NS(Lsp::pathFromUri(Std(b)));
    return pa.length && [RealPath(pa) isEqualToString:RealPath(pb)];
}

// `base` is the text the server had been sent for the file when it
// published (Client::textFor), or null when it does not have it open (a
// file no tab has: listed in Problems only, and opening it brings fresh
// ones). Each server's list stands beside the others'.
- (void)diagnosticsArrived:(const std::string &)uri
                      list:(const std::vector<Lsp::Diagnostic> &)list
                      base:(const std::string *)base
                    server:(MCLspServer *)server {
    std::string key = CanonicalURI(uri);
    std::shared_ptr<const std::u16string> text;
    if (base) text = std::make_shared<const std::u16string>(Lsp::toUtf16(*base));
    if (!_store.publish(key, Std(server.serverId), list, text, Std(server.name))) return;
    if ([self uri:uri isSameFileAs:_uri]) [self paintDiagnostics];
    [self problemsChanged];
}

// ---- the Problems list
- (void)problemsChanged {
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(reportProblems)
                                               object:nil];
    [self performSelector:@selector(reportProblems) withObject:nil afterDelay:0.05];
}

- (void)reportProblems {
    if (self.onProblemsChanged) self.onProblemsChanged();
}

// Every published diagnostic as an Lsp::Problem, placed in the text as it
// is now for files open in a tab (Lsp::diagnosticSpans, leaving out ones
// about text typed over since), as the server said for the others. Hints
// are left out, as VS Code leaves them out of its Problems: pyright sends
// one for every name not used yet, which would drown the rest.
- (std::vector<Lsp::Problem>)collectProblems {
    std::vector<Lsp::Problem> out;
    NSMutableSet<NSString *> *seen = [NSMutableSet set];
    for (const std::string &key : _store.uris()) {
        NSString *uri = NS(key);
        [seen addObject:uri];
        BOOL onShow = _uri && [self uri:key isSameFileAs:_uri];
        // Listed under the tree's spelling of the path, as the files no tab
        // has are, so they sort together; the tab's own spelling finds its text.
        NSString *tabPath = onShow ? _path : [self tabPathForURI:uri];
        NSString *text = onShow ? _tv.string
                       : tabPath && self.bufferText ? self.bufferText(tabPath) : nil;
        MCProblemFile *file = _problemFiles[uri];
        const uint64_t gen = _store.generation(key);
        BOOL fresh = file && file->generation == gen &&
                     (text ? [file.text isEqualToString:text] : file.text == nil);
        if (!fresh) {
            if (!file) {
                file = [MCProblemFile new];
                file.path = [self treePathForURI:key];   // two realpaths, once per file
                _problemFiles[uri] = file;
            }
            file->generation = gen;
            file.text = text;   // copied: the text view's string is its live storage
            file->problems = [self problemsOfURI:key path:file.path text:text];
        }
        out.insert(out.end(), file->problems.begin(), file->problems.end());
    }
    for (NSString *uri in _problemFiles.allKeys)
        if (![seen containsObject:uri]) [_problemFiles removeObjectForKey:uri];
    return out;
}

// One file's problems: placed in the text as it is now for a file open in a
// tab (`text`), as the server said for the others.
- (std::vector<Lsp::Problem>)problemsOfURI:(const std::string &)key path:(NSString *)path
                                      text:(NSString *)text {
    std::vector<Lsp::Problem> out;
    if (!path.length) return out;
    auto add = [&](const Lsp::Diagnostic &d, Lsp::Position a, Lsp::Position b) {
        if (d.severity == Lsp::Severity::Hint) return;
        Lsp::Problem p;
        p.path = Std(path);
        p.line = a.line;
        p.column = a.character;
        p.endLine = b.line;
        p.endColumn = b.character;
        p.severity = d.severity;
        p.message = d.message;
        p.source = d.source;
        p.code = d.code;
        out.push_back(p);
    };
    std::vector<const Lsp::DiagnosticStore::Set *> sets = _store.forUri(key);
    if (!text) {   // a file no tab has: as the server said
        for (const Lsp::DiagnosticStore::Set *set : sets)
            for (const Lsp::Diagnostic &d : set->list) add(d, d.range.start, d.range.end);
        return out;
    }
    const std::u16string now = U16Of(text);
    const Lsp::PositionIndex index(now);
    for (const Lsp::MergedSpan &sp : Lsp::mergedDiagnosticSpans(sets, now))
        add(*sp.diag, index.position(sp.start), index.position(sp.end));
    return out;
}

- (NSArray<MCProblem *> *)problemRows {
    std::vector<Lsp::Problem> problems = [self collectProblems];
    NSMutableArray<MCProblem *> *rows = [NSMutableArray array];
    for (const Lsp::ProblemRow &r : Lsp::problemRows(problems)) {
        MCProblem *p = [MCProblem new];
        p.heading = r.heading;
        p.path = NS(r.path);
        if (r.heading) {
            p.errors = r.errors;
            p.warnings = r.warnings;
            p.others = r.others;
        } else {
            const Lsp::Problem &q = problems[r.problem];
            p.line = q.line;
            p.column = q.column;
            p.endLine = q.endLine;
            p.endColumn = q.endColumn;
            p.severity = (NSInteger)q.severity;
            p.fullMessage = NS(q.message);
            p.message = NS(Lsp::problemOneLine(q.message));
            p.source = NS(q.source);
            p.code = NS(q.code);
        }
        [rows addObject:p];
    }
    return rows;
}

// What the list shows, counted: diagnostics about text typed over since
// they came are not counted, as they are not listed (the per-file rows are
// kept, so this costs a compare of each open file's text).
- (Lsp::ProblemCounts)problemCounts {
    return Lsp::countProblems([self collectProblems]);
}

- (NSString *)problemCountsText { return NS(Lsp::problemCountsText([self problemCounts])); }
- (NSInteger)problemErrors { return [self problemCounts].errors; }
- (NSInteger)problemWarnings { return [self problemCounts].warnings; }

- (BOOL)problemsKnown {
    if (!_store.empty()) return YES;
    for (MCLspEntry *e in _entries.allValues)
        if (e.server) return YES;
    return NO;
}

- (void)openProblem:(MCProblem *)problem {
    if (problem.heading || !problem.path.length) return;
    Lsp::Location loc;
    loc.uri = Lsp::uriFromPath(Std(RealPath(problem.path)));
    loc.range.start = Lsp::Position{(int)problem.line, (int)problem.column};
    loc.range.end = Lsp::Position{(int)problem.endLine, (int)problem.endColumn};
    [self revealLocation:loc];
}

- (void)goToProblem:(BOOL)forward {
    // The marks drawn now (they follow typing), hints left out as in the list.
    NSArray<MCDiagnosticMark *> *all = _tv.diagnostics;
    NSMutableArray<MCDiagnosticMark *> *marks = [NSMutableArray array];
    for (MCDiagnosticMark *m in all) if (m.severity != 4) [marks addObject:m];
    if (!_uri || marks.count == 0) {
        NSBeep();
        [self flash:@"No problems in this file"];
        return;
    }
    std::vector<std::pair<size_t, size_t>> spans;
    for (MCDiagnosticMark *m in marks) spans.push_back({m.range.location, NSMaxRange(m.range)});
    long last = all == _stepMarks ? _stepIndex : -1;
    long i = Lsp::stepProblem(spans, _tv.selectedRange.location, last, forward);
    if (i < 0) return;
    MCDiagnosticMark *m = marks[(NSUInteger)i];
    NSRange caret = NSMakeRange(m.range.location, 0);
    [_tv setSelectedRange:caret];   // closes what was showing
    [_tv scrollRangeToVisible:m.range];
    _stepMarks = all;
    _stepIndex = i;
    // Its message (and any other problem there) in the hover box at the
    // caret, which the next caret move closes.
    [self closeHover];
    [self showHoverAtCaret:caret.location];
}

// The file on show's diagnostics, drawn on its text as it is now: those
// computed on text typed over since are left out until the server
// publishes for the new text (Lsp::diagnosticSpans).
- (void)paintDiagnostics {
    if (!_uri) return;
    std::vector<const Lsp::DiagnosticStore::Set *> sets = _store.forUri(Std(_uri));
    if (sets.empty()) {
        _tv.diagnostics = @[];
        [self updateStatus];
        return;
    }
    std::u16string text = U16Of(_tv.string);
    NSMutableArray *marks = [NSMutableArray array];
    for (const Lsp::MergedSpan &span : Lsp::mergedDiagnosticSpans(sets, text)) {
        const Lsp::Diagnostic &d = *span.diag;
        size_t a = span.start;
        size_t b = span.end;
        if (b <= a) {
            // An empty range: mark the word there, or at least one character.
            b = a;
            while (b < text.size() && IsIdentChar(text[b])) b++;
            if (b == a && a < text.size() && text[a] != u'\n') b = a + 1;
            else if (b == a && a > 0) a--;
        }
        MCDiagnosticMark *m = [MCDiagnosticMark new];
        m.range = NSMakeRange(a, b - a);
        m.severity = (NSInteger)d.severity;
        m.unnecessary = d.unnecessary;
        m.deprecated = d.deprecated;
        NSString *kind = d.severity == Lsp::Severity::Error ? @"Error"
                       : d.severity == Lsp::Severity::Warning ? @"Warning"
                       : d.severity == Lsp::Severity::Information ? @"Info" : @"Hint";
        // Which server says so, as VS Code adds it: "(ruff F401)".
        NSString *from = NS(d.code.empty() ? d.source
                            : d.source.empty() ? d.code : d.source + " " + d.code);
        m.message = from.length
            ? [NSString stringWithFormat:@"%@: %@ (%@)", kind, NS(d.message), from]
            : [NSString stringWithFormat:@"%@: %@", kind, NS(d.message)];
        [marks addObject:m];
    }
    _tv.diagnostics = marks;
    [self updateStatus];
}

@end
