// LspFixesSuite.mm — `make guitest SUITES=lsp_fixes`: the language server
// fixes of 2026-11-05, against the scripted server (build/fakelsp), so they
// run anywhere: diagnostics for an old version dropped, every tab's file
// open on the server with its unsaved text, completion with
// additionalTextEdits as one undo step, rename across an unsaved tab, a
// rename that cannot be made changing nothing, and Ruff's second server:
// which server answers what, its quick fixes, a command's
// workspace/applyEdit, and Organize Imports preferring Ruff's.
#import "Harness.h"
#import "EditorController.h"
#import "Lsp.h"
#import "LspPicker.h"

namespace {

NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
LspSession *Lsp(EditorController *c) { return [c valueForKey:@"lsp"]; }
NSString *Current(EditorController *c) { return [c valueForKey:@"currentPath"]; }
NSString *Disk(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}

// realpath's spelling (/private/tmp), as the app gives the server.
NSString *Real(NSString *p) {
    char buf[PATH_MAX];
    return realpath(p.fileSystemRepresentation, buf) ? @(buf) : p;
}

NSDictionary *Pos(int line, int ch) { return @{@"line": @(line), @"character": @(ch)}; }
NSDictionary *Rng(int l0, int c0, int l1, int c1) {
    return @{@"start": Pos(l0, c0), @"end": Pos(l1, c1)};
}
NSDictionary *Edit(int l0, int c0, int l1, int c1, NSString *text) {
    return @{@"range": Rng(l0, c0, l1, c1), @"newText": text};
}
NSDictionary *Diag(int line, NSString *message, NSString *source, NSString *code) {
    NSMutableDictionary *d = [@{@"range": Rng(line, 0, line, 3), @"severity": @2,
                                @"message": message} mutableCopy];
    if (source) d[@"source"] = source;
    if (code) d[@"code"] = code;
    return d;
}
NSDictionary *Publish(NSString *version, NSArray *diagnostics) {
    return @{@"method": @"textDocument/publishDiagnostics",
             @"params": @{@"uri": @"${uri}", @"version": version, @"diagnostics": diagnostics}};
}

// The last text the server was sent for a file: its didOpen, or a
// didChange after it (full sync).
NSString *ServerText(NSString *server, NSString *name) {
    NSString *text = nil;
    for (NSDictionary *m in MCGuiFakeLspReceived(server, nil)) {
        NSDictionary *p = m[@"params"];
        if (![p isKindOfClass:NSDictionary.class]) continue;
        NSString *uri = p[@"textDocument"][@"uri"];
        if (![uri isKindOfClass:NSString.class] || ![uri hasSuffix:[@"/" stringByAppendingString:name]])
            continue;
        if ([m[@"method"] isEqual:@"textDocument/didOpen"]) text = p[@"textDocument"][@"text"];
        else if ([m[@"method"] isEqual:@"textDocument/didChange"])
            text = [p[@"contentChanges"] lastObject][@"text"];
        else if ([m[@"method"] isEqual:@"textDocument/didClose"]) text = nil;
    }
    return text;
}

BOOL Running(EditorController *c, NSUInteger servers, NSString *log) {
    LspSession *lsp = Lsp(c);
    return lsp.hasServer && lsp.fileServerNames.count == servers &&
           MCGuiFakeLspReceived(log, @"initialized").count > 0 &&
           ![lsp.statusText containsString:@"starting"];
}

NSArray<NSString *> *Messages(EditorController *c) {
    NSMutableArray *out = [NSMutableArray array];
    for (MCDiagnosticMark *m in ((CodeTextView *)Text(c)).diagnostics) [out addObject:m.message];
    return out;
}

void TypeAt(EditorController *c, NSUInteger at, NSString *s) {
    NSTextView *tv = Text(c);
    MCGuiUndoGroup(tv.undoManager, ^{
        tv.selectedRange = NSMakeRange(at, 0);
        [tv insertText:s replacementRange:tv.selectedRange];
    });
}

}  // namespace

MC_GUI_SUITE(lsp_fixes) {
    __block EditorController *c = nil;
    NSString *a = MCGuiProjectFile(@"lsp/a.py");
    NSString *b = MCGuiProjectFile(@"lsp/b.py");
    NSString *comp = MCGuiProjectFile(@"lsp/comp.py");
    NSString *notes = MCGuiProjectFile(@"lsp/notes.md");
    NSString *aText = @"def helper():\n    return 1\n\nhelper()\n";
    NSString *bText = @"from a import helper\nhelper()\n";
    NSString *compText = @"import sys\n\nsys.pa";

    // The main server's script: diagnostics on every open and change, an
    // old version's first (which must be dropped), then the current one's.
    NSDictionary *(^mainScript)(NSArray *extra) = ^NSDictionary *(NSArray *extra) {
        NSMutableArray *rules = [NSMutableArray array];
        for (NSString *m in @[@"textDocument/didOpen", @"textDocument/didChange"])
            [rules addObject:@{@"method": m, @"when": @{@"uriEndsWith": @"a.py"},
                               @"notify": @[Publish(@"${version-1}", @[Diag(0, @"stale one", nil, nil)]),
                                            Publish(@"${version}", @[Diag(0, @"fresh one", nil, nil)])]}];
        [rules addObject:@{@"method": @"textDocument/completion",
                           @"result": @{@"isIncomplete": @NO, @"items": @[
                               @{@"label": @"path", @"kind": @6, @"insertText": @"path",
                                 @"additionalTextEdits": @[Edit(0, 0, 0, 0, @"import os\n")]},
                               @{@"label": @"platform", @"kind": @6, @"insertText": @"platform"}]}}];
        [rules addObjectsFromArray:extra ?: @[]];
        return @{@"name": @"fakelsp", @"rules": rules};
    };

    MCGuiStep(@"three Python files and a note; the scripted server for Python", ^{
        MCGuiWriteFile(a, aText);
        MCGuiWriteFile(b, bText);
        MCGuiWriteFile(comp, compText);
        MCGuiWriteFile(notes, @"# Notes\n\nSome text.\n");
        NSString *cmd = MCGuiFakeLsp(@"main", mainScript(nil), nil, nil);
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = off\n", cmd]);
        c = MCGuiController();
        [c focusEditor:nil];
        GCHECK([c showFile:a]);
    });
    MCGuiWait(@"the server runs for a.py", 20, ^BOOL { return Running(c, 1, @"main"); });

    // ------------------------------------------- version-checked diagnostics
    MCGuiWait(@"a.py's diagnostics", 10, ^BOOL { return Messages(c).count > 0; });
    MCGuiStep(@"only the current version's diagnostics are drawn", ^{
        NSArray *m = Messages(c);
        GCHECK(m.count == 1 && [m.firstObject containsString:@"fresh one"]);
        if (m.count != 1) MCGuiNote(@"diagnostics: %@", m);
        TypeAt(c, Text(c).string.length, @"helper()\n");
    });
    MCGuiWait(@"the edit reaches the server", 5, ^BOOL {
        return [ServerText(@"main", @"a.py") hasSuffix:@"helper()\nhelper()\n"];
    });
    MCGuiPause(0.3);
    MCGuiStep(@"after the edit, still only the fresh ones", ^{
        NSArray *m = Messages(c);
        GCHECK(m.count == 1 && [m.firstObject containsString:@"fresh one"]);
        NSArray *changes = MCGuiFakeLspReceived(@"main", @"textDocument/didChange");
        NSInteger last = [[changes.lastObject valueForKeyPath:@"params.textDocument.version"] integerValue];
        NSInteger opened = [[MCGuiFakeLspReceived(@"main", @"textDocument/didOpen").firstObject
            valueForKeyPath:@"params.textDocument.version"] integerValue];
        GCHECK(last > opened);
        [Text(c).undoManager undo];
        GCHECK_EQ_STR(Text(c).string, aText);
    });

    // ------------------------------------------------ every tab on the server
    MCGuiStep(@"b.py in a tab, changed and not saved; back to a.py", ^{
        GCHECK([c showFile:b]);
        TypeAt(c, 0, @"# new\n");
        GCHECK([c showFile:a]);
    });
    MCGuiWait(@"the server has b.py's unsaved text", 5, ^BOOL {
        return [ServerText(@"main", @"b.py") isEqualToString:[@"# new\n" stringByAppendingString:bText]];
    });
    MCGuiStep(@"b.py on disk as it was; a.py the file on show", ^{
        GCHECK_EQ_STR(Disk(b), bText);
        GCHECK([Current(c) isEqualToString:a]);
        GCHECK([MCGuiFakeLspReceived(@"main", @"textDocument/didOpen") count] >= 2);
    });

    // ------------------------------------------ rename across an unsaved tab
    // The server's edits for b.py are in its unsaved text (a line further
    // down than on disk): they go in only because the server has that text.
    MCGuiStep(@"rename helper to assist", ^{
        NSDictionary *changes = @{
            @"${uri:a.py}": @[Edit(0, 4, 0, 10, @"assist"), Edit(3, 0, 3, 6, @"assist")],
            @"${uri:b.py}": @[Edit(1, 14, 1, 20, @"assist"), Edit(2, 0, 2, 6, @"assist")]};
        MCGuiFakeLsp(@"main", mainScript(@[
            @{@"method": @"textDocument/rename", @"times": @1, @"result": @{@"changes": changes}},
            @{@"method": @"textDocument/rename",
              @"result": @{@"changes": @{@"${uri:a.py}": @[Edit(0, 4, 0, 10, @"other")],
                                         [@"${uri:" stringByAppendingFormat:@"%@}", Real(notes)]:
                                             @[Edit(2, 0, 2, 4, @"More")]}}}]),
                     nil, nil);
        Text(c).selectedRange = NSMakeRange(6, 0);   // in "helper"
        [Lsp(c) renameSymbol];
        MCRenameBox *box = [Lsp(c) valueForKey:@"rename"];
        GCHECK(box.visible);
        GCHECK_EQ_STR(Lsp(c).renameField.stringValue, @"helper");
        if (box.onCommit) box.onCommit(@"assist");
        [box close];
    });
    MCGuiWait(@"renamed", 10, ^BOOL { return [Lsp(c).lastNote hasPrefix:@"Renamed"]; });
    MCGuiStep(@"both files renamed, b.py at its unsaved places, nothing saved", ^{
        GCHECK_EQ_STR(Lsp(c).lastNote, @"Renamed 4 occurrences in 2 files");
        GCHECK_EQ_STR(Text(c).string, @"def assist():\n    return 1\n\nassist()\n");
        GCHECK([Current(c) isEqualToString:a]);
        GCHECK_EQ_STR(Lsp(c).bufferText(b), @"# new\nfrom a import assist\nassist()\n");
        GCHECK_EQ_STR(Disk(a), aText);
        GCHECK_EQ_STR(Disk(b), bText);
        // One undo step per file: a.py's goes back on its own.
        [Text(c).undoManager undo];
        GCHECK_EQ_STR(Text(c).string, aText);
        [Text(c).undoManager redo];
        GCHECK_EQ_STR(Text(c).string, @"def assist():\n    return 1\n\nassist()\n");
    });

    // A rename that also names a file shown as a preview (not text to edit)
    // changes nothing, and the tab it opened for the edit closes again.
    MCGuiStep(@"a rename touching a Markdown preview", ^{
        Text(c).selectedRange = NSMakeRange(6, 0);
        [Lsp(c) renameSymbol];
        MCRenameBox *box = [Lsp(c) valueForKey:@"rename"];
        if (box.onCommit) box.onCommit(@"other");
        [box close];
    });
    MCGuiWait(@"refused", 10, ^BOOL { return [Lsp(c).lastNote containsString:@"notes.md"]; });
    MCGuiStep(@"nothing changed, and no tab left for notes.md", ^{
        MCGuiNote(@"the note: %@", Lsp(c).lastNote);
        GCHECK_EQ_STR(Text(c).string, @"def assist():\n    return 1\n\nassist()\n");
        GCHECK([Current(c) isEqualToString:a]);
        BOOL notesOpen = NO;
        // This suite's notes.md (another suite may leave a notes.md of its own open).
        for (NSString *p in Lsp(c).openPaths()) if ([Real(p) isEqualToString:Real(notes)]) notesOpen = YES;
        GCHECK(!notesOpen);
    });

    // ---------------------------------------- completion's additionalTextEdits
    MCGuiStep(@"comp.py: Ctrl+Space after sys.pa", ^{
        GCHECK([c showFile:comp]);
        Text(c).selectedRange = NSMakeRange(compText.length, 0);
        [Lsp(c) triggerCompletion];
    });
    MCGuiWait(@"the list", 10, ^BOOL { return Lsp(c).completionVisible; });
    MCGuiStep(@"Tab takes path with its import, as one undo step", ^{
        GCHECK([Lsp(c).completionLabels containsObject:@"path"]);
        GCHECK_EQ_STR(Lsp(c).completionSelectedLabel, @"path");
        NSTextView *tv = Text(c);
        MCGuiUndoGroup(tv.undoManager, ^{ GCHECK([Lsp(c) handleCommand:@selector(insertTab:)]); });
        GCHECK_EQ_STR(tv.string, @"import os\nimport sys\n\nsys.path");
        GCHECK(tv.selectedRange.location == tv.string.length);
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, compText);
    });

    // ------------------------------------------------- Ruff's second server
    MCGuiStep(@"a second server for Python, run as ruff", ^{
        NSString *mainCmd = MCGuiFakeLsp(@"main2", @{
            @"name": @"fakelsp",
            @"capabilities": @{@"textDocumentSync": @1, @"hoverProvider": @YES,
                               @"codeActionProvider": @YES},
            @"rules": @[
                @{@"method": @"textDocument/hover", @"result": @{@"contents": @"from the main server"}},
                @{@"method": @"textDocument/codeAction",
                  @"when": @{@"paramsContain": @"source.organizeImports"},
                  @"result": @[@{@"title": @"Organize Imports", @"kind": @"source.organizeImports",
                                 @"edit": @{@"changes": @{@"${uri}": @[Edit(0, 0, 0, 0, @"# main\n")]}}}]},
                @{@"method": @"textDocument/codeAction", @"result": @[]}]}, nil, nil);
        NSString *ruffCmd = MCGuiFakeLsp(@"ruff", @{
            @"name": @"ruff",
            @"capabilities": @{@"textDocumentSync": @1,
                               @"codeActionProvider": @{@"codeActionKinds":
                                   @[@"quickfix", @"source.organizeImports.ruff"]},
                               @"executeCommandProvider": @{@"commands": @[@"ruff.applyAutofix"]}},
            @"rules": @[
                @{@"method": @"textDocument/didOpen", @"when": @{@"uriEndsWith": @"imports.py"},
                  @"notify": @[Publish(@"${version}", @[Diag(1, @"`os` imported but unused",
                                                             @"Ruff", @"F401")])]},
                @{@"method": @"textDocument/codeAction",
                  @"when": @{@"paramsContain": @"source.organizeImports"},
                  @"result": @[@{@"title": @"Ruff: Organize imports",
                                 @"kind": @"source.organizeImports.ruff",
                                 @"edit": @{@"changes": @{@"${uri}":
                                     @[Edit(0, 0, 2, 0, @"import os\nimport sys\n")]}}}]},
                @{@"method": @"textDocument/codeAction",
                  @"result": @[
                      @{@"title": @"Remove unused import: `os`", @"kind": @"quickfix",
                        @"isPreferred": @YES,
                        @"edit": @{@"changes": @{@"${uri}": @[Edit(1, 0, 2, 0, @"")]}}},
                      @{@"title": @"Fix all auto-fixable problems", @"kind": @"source.fixAll.ruff",
                        @"command": @{@"title": @"Fix all", @"command": @"ruff.applyAutofix",
                                      @"arguments": @[@"${uri}"]}}]},
                @{@"method": @"workspace/executeCommand",
                  @"request": @[@{@"method": @"workspace/applyEdit",
                                  @"params": @{@"label": @"Fix all",
                                               @"edit": @{@"changes": @{@"${uri:imports.py}":
                                                   @[Edit(0, 0, 0, 0, @"# fixed\n")]}}}}],
                  @"result": [NSNull null]}]}, @"ruff", @"server");
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = %@\n", mainCmd, ruffCmd]);
        MCGuiWriteFile(MCGuiProjectFile(@"lsp/imports.py"), @"import sys\nimport os\n\nprint(sys)\n");
        GCHECK([c showFile:MCGuiProjectFile(@"lsp/imports.py")]);
    });
    MCGuiWait(@"both servers run", 20, ^BOOL {
        return Running(c, 2, @"main2") && MCGuiFakeLspReceived(@"ruff", @"initialized").count > 0;
    });
    MCGuiWait(@"Ruff's diagnostic", 10, ^BOOL { return Messages(c).count > 0; });
    MCGuiStep(@"its diagnostic names it; the servers are fakelsp and ruff", ^{
        GCHECK_EQ_STR(Lsp(c).fileServerNames.firstObject, @"fakelsp");
        GCHECK_EQ_STR(Lsp(c).fileServerNames.lastObject, @"ruff");
        GCHECK([Messages(c).firstObject containsString:@"(Ruff F401)"]);
        Text(c).selectedRange = NSMakeRange(12, 0);   // on "import os"
        [Lsp(c) showHoverInfo];
    });
    MCGuiWait(@"the hover", 10, ^BOOL { return Lsp(c).lastHoverText.length > 0; });
    MCGuiStep(@"hover from the main server only; quick fixes from both", ^{
        GCHECK([Lsp(c).lastHoverText containsString:@"from the main server"]);
        GCHECK(MCGuiFakeLspReceived(@"ruff", @"textDocument/hover").count == 0);
        [Lsp(c) showQuickFixes];
    });
    MCGuiWait(@"the fixes", 10, ^BOOL { return [Lsp(c).pickerKind isEqualToString:@"fixes"]; });
    MCGuiStep(@"Ruff's preferred fix first, named; Return applies it", ^{
        NSArray<NSString *> *rows = Lsp(c).pickerRows;
        MCGuiNote(@"fixes: %@", rows);
        GCHECK(rows.count == 2);
        GCHECK([rows.firstObject containsString:@"Remove unused import"] &&
               [rows.firstObject containsString:@"ruff"]);
        GCHECK(MCGuiFakeLspReceived(@"main2", @"textDocument/codeAction").count >= 1);
        NSTextView *tv = Text(c);
        MCGuiUndoGroup(tv.undoManager, ^{ [Lsp(c) acceptPickerRow:0]; });
    });
    MCGuiWait(@"the fix goes in", 5, ^BOOL {
        return [Text(c).string isEqualToString:@"import sys\n\nprint(sys)\n"];
    });
    MCGuiStep(@"undo; the command's fix (workspace/applyEdit)", ^{
        [Text(c).undoManager undo];
        GCHECK_EQ_STR(Text(c).string, @"import sys\nimport os\n\nprint(sys)\n");
        Text(c).selectedRange = NSMakeRange(12, 0);
        [Lsp(c) showQuickFixes];
    });
    MCGuiWait(@"the fixes again", 10, ^BOOL { return [Lsp(c).pickerKind isEqualToString:@"fixes"]; });
    MCGuiStep(@"Fix all runs ruff.applyAutofix", ^{
        GCHECK(Lsp(c).pickerRows.count == 2);
        [Lsp(c) acceptPickerRow:1];
    });
    MCGuiWait(@"the server's edit arrives and is answered", 10, ^BOOL {
        return [Text(c).string hasPrefix:@"# fixed\n"];
    });
    MCGuiPause(0.2);
    MCGuiStep(@"applied: true went back; Organize Imports", ^{
        GCHECK(MCGuiFakeLspReceived(@"ruff", @"workspace/executeCommand").count == 1);
        BOOL answered = NO;
        for (NSDictionary *m in MCGuiFakeLspReceived(@"ruff", nil))
            if (!m[@"method"] && [[m valueForKeyPath:@"result.applied"] boolValue]) answered = YES;
        GCHECK(answered);
        [Text(c).undoManager undo];
        GCHECK_EQ_STR(Text(c).string, @"import sys\nimport os\n\nprint(sys)\n");
        [Lsp(c) organizeImports];
    });
    MCGuiWait(@"organized", 10, ^BOOL { return [Lsp(c).lastNote hasPrefix:@"Organized"]; });
    MCGuiStep(@"Ruff's organize imports, not the main server's", ^{
        GCHECK_EQ_STR(Lsp(c).lastNote, @"Organized imports (ruff)");
        GCHECK_EQ_STR(Text(c).string, @"import os\nimport sys\n\nprint(sys)\n");
        GCHECK(![Text(c).string containsString:@"# main"]);
    });

    MCGuiStep(@"language servers off again; unsaved tabs closed unasked", ^{
        MCGuiSetSettings(@"lsp.enabled = false\n");
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);   // Don't Save
        [c closeAllTabs:nil];
    });
    MCGuiPause(0.3);
}
