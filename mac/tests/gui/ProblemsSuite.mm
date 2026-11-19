// ProblemsSuite.mm — `make guitest SUITES=problems`: the Problems list
// (Shift+Cmd+M) and F8 / Shift+F8, against the scripted language server.
// Every file's problems are listed under a heading per file with its
// counts, including a file no tab has open (a server checking the whole
// folder), errors before warnings; the status bar counts them; Return on a
// row opens its file at the problem; F8 steps through the file's problems,
// wrapping, with the message at the caret; and closing a tab drops what its
// server said about it while open. A JSON file no server has is checked by
// the editor itself: its parse error is a problem like a server's.
#import "Harness.h"
#import "EditorController.h"
#import "Lsp.h"
#import "ProblemsPanel.h"

namespace {
NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
LspSession *Lsp(EditorController *c) { return [c valueForKey:@"lsp"]; }
MCProblemsPanel *Problems(EditorController *c) { return [c valueForKey:@"problemsPanel"]; }
NSString *Real(NSString *p) {
    char buf[PATH_MAX];
    return realpath(p.fileSystemRepresentation, buf) ? @(buf) : p;
}
NSDictionary *Diag(int line, int c0, int c1, int severity, NSString *message, NSString *code) {
    return @{@"range": @{@"start": @{@"line": @(line), @"character": @(c0)},
                         @"end": @{@"line": @(line), @"character": @(c1)}},
             @"severity": @(severity), @"message": message, @"source": @"fake", @"code": code};
}
NSUInteger LineStart(NSString *text, NSUInteger line) {   // from 0
    NSUInteger at = 0;
    for (NSUInteger l = 0; l < line; l++) at = NSMaxRange([text lineRangeForRange:NSMakeRange(at, 0)]);
    return at;
}
BOOL F8(EditorController *c, BOOL shift) {
    NSString *f8 = [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
    return MCGuiPerformMenuKey(c.window, 100, shift ? NSEventModifierFlagShift : 0, f8, f8);
}
}  // namespace

MC_GUI_SUITE(problems) {
    __block EditorController *c = nil;
    NSString *dir = MCGuiProjectFile(@"probs");
    NSString *p1 = [dir stringByAppendingPathComponent:@"first.py"];
    NSString *p2 = [dir stringByAppendingPathComponent:@"second.py"];
    NSString *p3 = [dir stringByAppendingPathComponent:@"unopened.py"];
    NSString *text1 = @"import os\nx = undefined_name\n\n\ndef f():\n    return y\n";

    MCGuiStep(@"three files, and a server with problems in each", ^{
        MCGuiWriteFile(p1, text1);
        MCGuiWriteFile(p2, @"print('two')\n");
        MCGuiWriteFile(p3, @"print('three')\n");
        NSString *unopenedUri = [@"file://" stringByAppendingString:Real(p3)];
        NSDictionary *publishAll = @{@"method": @"textDocument/publishDiagnostics", @"params": @{
            @"uri": unopenedUri,
            @"diagnostics": @[Diag(0, 0, 5, 2, @"`print` is shadowed", @"W1")]}};
        NSString *cmd = MCGuiFakeLsp(@"problems", @{@"name": @"fakelsp", @"rules": @[
            @{@"method": @"textDocument/didOpen", @"when": @{@"uriEndsWith": @"first.py"},
              @"notify": @[@{@"method": @"textDocument/publishDiagnostics", @"params": @{
                  @"uri": @"${uri}", @"version": @"${version}",
                  @"diagnostics": @[Diag(0, 7, 9, 2, @"`os` imported but unused", @"F401"),
                                    Diag(1, 4, 18, 1, @"`undefined_name` is not defined", @"F821"),
                                    Diag(5, 11, 12, 1, @"`y` is not defined", @"F821")]}},
                           publishAll]},
            @{@"method": @"textDocument/didOpen", @"when": @{@"uriEndsWith": @"second.py"},
              @"notify": @[@{@"method": @"textDocument/publishDiagnostics", @"params": @{
                  @"uri": @"${uri}", @"version": @"${version}",
                  @"diagnostics": @[Diag(0, 0, 5, 3, @"a note about print", @"I1")]}}]}]},
            nil, nil);
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = off\n", cmd]);
        c = MCGuiController();
        // The tree has not seen the new folder yet (FSEvents would bring
        // it in a moment later), and a row opens its file through the tree.
        [c refreshTree:nil];
        [c focusEditor:nil];
        GCHECK([c showFile:p2]);
        GCHECK([c showFile:p1]);
    });
    MCGuiWait(@"the problems arrive", 20, ^BOOL {
        return Lsp(c).problemErrors == 2 && Lsp(c).problemWarnings == 2 &&
               ((CodeTextView *)Text(c)).diagnostics.count == 3;
    });
    MCGuiStep(@"Shift+Cmd+M lists them, by file, errors first", ^{
        MCGuiNote(@"counts: %@", Lsp(c).problemCountsText);
        GCHECK(MCGuiPerformMenuKey(c.window, 46, NSEventModifierFlagCommand | NSEventModifierFlagShift,
                                   nil, nil));
        GCHECK(Problems(c) != nil && !Problems(c).hidden);
        GCHECK(Problems(c).hasFocus);
        NSArray<NSString *> *rows = Problems(c).rowTexts;
        MCGuiNote(@"rows: %@", rows);
        // A heading per file, the unopened file's too.
        NSUInteger headings = 0, firstAt = NSNotFound, unopenedAt = NSNotFound;
        for (NSUInteger i = 0; i < rows.count; i++) {
            if ([rows[i] containsString:@"first.py"] && firstAt == NSNotFound) firstAt = i;
            if ([rows[i] containsString:@"unopened.py"]) unopenedAt = i;
        }
        for (MCProblem *p in [Lsp(c) problemRows]) headings += p.heading;
        GCHECK(headings == 3);
        GCHECK(firstAt != NSNotFound && unopenedAt != NSNotFound);
        // In first.py, the two errors come before the warning.
        NSArray<MCProblem *> *list = [Lsp(c) problemRows];
        NSMutableArray<NSNumber *> *severities = [NSMutableArray array];
        BOOL inFirst = NO;
        for (MCProblem *p in list) {
            if (p.heading) { inFirst = [p.path hasSuffix:@"first.py"]; continue; }
            if (inFirst) [severities addObject:@(p.severity)];
        }
        GCHECK([severities isEqualToArray:(@[@1, @1, @2])]);
    });
    MCGuiStep(@"opening a row: the unopened file at its problem", ^{
        MCProblem *target = nil;
        for (MCProblem *p in [Lsp(c) problemRows])
            if (!p.heading && [p.path hasSuffix:@"unopened.py"]) target = p;
        GCHECK(target != nil);
        if (target) [Lsp(c) openProblem:target];
    });
    MCGuiWait(@"unopened.py on show", 5, ^BOOL {
        return [[c valueForKey:@"currentPath"] hasSuffix:@"/unopened.py"];
    });
    MCGuiStep(@"F8 steps through first.py's problems, Shift+F8 back, wrapping", ^{
        GCHECK([c showFile:p1]);
        [c.window makeFirstResponder:Text(c)];
        NSString *t = Text(c).string;
        Text(c).selectedRange = NSMakeRange(0, 0);
        GCHECK(F8(c, NO));
        GCHECK(Text(c).selectedRange.location == 7);                      // os
        GCHECK(F8(c, NO));
        GCHECK(Text(c).selectedRange.location == LineStart(t, 1) + 4);    // undefined_name
        GCHECK(F8(c, NO));
        GCHECK(Text(c).selectedRange.location == LineStart(t, 5) + 11);   // y
        GCHECK(F8(c, NO));
        GCHECK(Text(c).selectedRange.location == 7);                      // round again
        GCHECK(F8(c, YES));
        GCHECK(Text(c).selectedRange.location == LineStart(t, 5) + 11);
        GCHECK(Lsp(c).hoverVisible);
        MCGuiNote(@"at the caret: %@", Lsp(c).lastHoverText);
        GCHECK([Lsp(c).lastHoverText containsString:@"`y` is not defined"]);
    });
    MCGuiStep(@"closing second.py's tab drops its problems", ^{
        GCHECK([c showFile:p2]);
        [c closeFocused:nil];
    });
    MCGuiWait(@"one heading fewer", 5, ^BOOL {
        for (MCProblem *p in [Lsp(c) problemRows])
            if ([p.path hasSuffix:@"second.py"]) return NO;
        return YES;
    });
    MCGuiStep(@"Shift+Cmd+M twice more hides it; servers off", ^{
        // The list has the keyboard after the first; the second hides it.
        [c toggleProblems:nil];
        if (Problems(c) && !Problems(c).hidden) [c toggleProblems:nil];
        GCHECK(!Problems(c) || Problems(c).hidden);
        MCGuiSetSettings(@"lsp.enabled = false\n");
    });

    // ---------------------------------------------- JSON, which no server has
    // The editor checks it itself, as VS Code does (a grader's walk,
    // 2026-11-18: the outline said "Not valid JSON" and the status bar "No
    // problems"): a trailing comma is one error, drawn on the comma, listed
    // and counted; taken out, it goes, and typed back it returns; JSON with
    // comments (tsconfig.json) may have comments and trailing commas.
    NSString *pkg = [dir stringByAppendingPathComponent:@"package.json"];
    NSString *pkgText = @"{\n  \"name\": \"x\",\n  \"version\": \"1.0.0\",\n}\n";
    NSString *tsconfig = [dir stringByAppendingPathComponent:@"tsconfig.json"];
    NSString *tsText = @"// the compiler's options\n{\n  /* target */ \"target\": \"es2022\",\n"
                       @"  \"lib\": [\"dom\",],\n}\n";
    const NSUInteger comma = LineStart(pkgText, 2) + 20;   // after "1.0.0"
    NSTextField *(^label)(void) = ^NSTextField * { return [c valueForKey:@"problemsLabel"]; };
    NSArray<MCDiagnosticMark *> *(^marks)(void) = ^NSArray<MCDiagnosticMark *> * {
        return ((CodeTextView *)Text(c)).diagnostics;
    };
    NSArray<MCProblem *> *(^rowsOf)(NSString *) = ^NSArray<MCProblem *> *(NSString *name) {
        NSMutableArray<MCProblem *> *rows = [NSMutableArray array];
        for (MCProblem *p in [Lsp(c) problemRows])
            if (!p.heading && [p.path.lastPathComponent isEqualToString:name]) [rows addObject:p];
        return rows;
    };
    MCGuiStep(@"a JSON file with a trailing comma, and no server", ^{
        MCGuiWriteFile(pkg, pkgText);
        MCGuiWriteFile(tsconfig, tsText);
        GCHECK_EQ_STR([pkgText substringWithRange:NSMakeRange(comma, 1)], @",");
        GCHECK([c showFile:pkg]);
        [c.window makeFirstResponder:Text(c)];
    });
    MCGuiWait(@"one problem, and the status bar says so", 10, ^BOOL {
        return marks().count == 1 && Lsp(c).problemErrors == 1 &&
               [label().stringValue containsString:@"1 error"];
    });
    MCGuiStep(@"drawn on the comma, listed in Problems at its line", ^{
        MCDiagnosticMark *m = marks().firstObject;
        MCGuiNote(@"mark %@: %@; status bar: %@", m ? NSStringFromRange(m.range) : @"none",
                  m.message, label().stringValue);
        GCHECK(m && NSEqualRanges(m.range, NSMakeRange(comma, 1)));
        GCHECK(m && m.severity == 1 && [m.message containsString:@"Trailing comma"]);
        GCHECK(!label().hidden);
        NSArray<MCProblem *> *rows = rowsOf(@"package.json");
        GCHECK(rows.count == 1);
        MCProblem *p = rows.firstObject;
        GCHECK(p && p.line == 2 && p.column == 20 && p.severity == 1);
        GCHECK_EQ_STR(p.message, @"Trailing comma");
        GCHECK_EQ_STR(p.source, @"json");
    });
    MCGuiStep(@"the comma taken out", ^{
        MCGuiUndoGroup(Text(c).undoManager, ^{
            [Text(c) insertText:@"" replacementRange:NSMakeRange(comma, 1)];
        });
        GCHECK([Text(c).string containsString:@"\"1.0.0\"\n}"]);
    });
    MCGuiWait(@"no problems, and the status bar says so", 10, ^BOOL {
        return marks().count == 0 && Lsp(c).problemErrors == 0 && rowsOf(@"package.json").count == 0 &&
               [label().stringValue containsString:@"No problems"];
    });
    MCGuiStep(@"typed back", ^{
        Text(c).selectedRange = NSMakeRange(comma, 0);
        MCGuiUndoGroup(Text(c).undoManager, ^{ [Text(c) insertText:@"," replacementRange:Text(c).selectedRange]; });
        GCHECK_EQ_STR(Text(c).string, pkgText);
    });
    MCGuiWait(@"the problem is back", 10, ^BOOL {
        return marks().count == 1 && NSEqualRanges(marks().firstObject.range, NSMakeRange(comma, 1));
    });
    MCGuiStep(@"tsconfig.json: JSON with comments, its comments and trailing commas allowed", ^{
        GCHECK([c showFile:tsconfig]);
        GCHECK_EQ_STR(Text(c).string, tsText);
    });
    MCGuiWait(@"the status bar counts tsconfig.json's problems", 10, ^BOOL {
        return !label().hidden && [label().stringValue containsString:@"1 error"];   // package.json's
    });
    // A negative check: the check of tsconfig.json runs at once, on a queue;
    // a second is long enough for it to have answered.
    MCGuiPause(1.0);
    MCGuiStep(@"none for tsconfig.json; package.json's stays listed while its tab is open", ^{
        MCGuiNote(@"marks: %lu; rows: %@", (unsigned long)marks().count, [Lsp(c) problemRows]);
        GCHECK(marks().count == 0);
        GCHECK(rowsOf(@"tsconfig.json").count == 0);
        GCHECK(rowsOf(@"package.json").count == 1);
    });
}
