// LspSyncSuite.mm — `make guitest SUITES=lsp_sync`: incremental didChange
// and accepting a completion whose import comes later, against the
// scripted server (build/fakelsp).
//
// The server takes ranged changes (textDocumentSync change 2), applies them
// to its own copy, and after every change publishes one diagnostic whose
// message is that copy (${text}), so the check compares it with the
// editor's text: edits at two places in one pause, a character outside the
// BMP and CRLF lines go as ranges, and a batch larger than the text goes
// whole.
//
// Completion: the server offers Path without its import and sends the
// import only from completionItem/resolve, after 300 ms. Accepting puts
// the word in at once (no nested run loop waiting for the answer; the old
// accept waited up to 0.4 s), and the import follows when the answer
// comes: in the same undo step when nothing was typed meanwhile, as a step
// of its own when typing came between, left out with a note when the text
// it would change was edited first, not at all when the accept was undone
// before the answer, and with a note when the server does not answer
// within a second.
#import "Harness.h"
#import "EditorController.h"
#import "Lsp.h"
#import <QuartzCore/QuartzCore.h>

namespace {
LspSession *Session(EditorController *c) { return [c valueForKey:@"lsp"]; }
CodeTextView *Text(EditorController *c) { return (CodeTextView *)[c valueForKey:@"textView"]; }

// Typed at `at`, replacing `length` characters, as one undo step.
void TypeAt(EditorController *c, NSUInteger at, NSUInteger length, NSString *text) {
    NSTextView *tv = Text(c);
    MCGuiUndoGroup(tv.undoManager, ^{
        [tv insertText:text replacementRange:NSMakeRange(at, length)];
    });
}

// What the server holds for the file, as its last diagnostic says
// ("Info: <text> (copy)").
NSString *ServerCopy(EditorController *c) {
    for (MCDiagnosticMark *m in Text(c).diagnostics) {
        NSString *s = m.message;
        if (![s hasPrefix:@"Info: "] || ![s hasSuffix:@" (copy)"]) continue;
        return [s substringWithRange:NSMakeRange(6, s.length - 6 - 7)];
    }
    return nil;
}

NSUInteger Count(NSArray<NSDictionary *> *changes, BOOL ranged) {
    NSUInteger n = 0;
    for (NSDictionary *m in changes) {
        NSArray *list = m[@"params"][@"contentChanges"];
        BOOL hasRange = NO;
        for (NSDictionary *one in list)
            if (one[@"range"]) hasRange = YES;
        if (hasRange == ranged) n++;
    }
    return n;
}

// The import goes on the second line, after "import os".
NSString *const kImport = @"from pathlib import Path\r\n";
NSString *const kImported = @"import os\r\nfrom pathlib import Path\r\n";
}  // namespace

MC_GUI_SUITE(lsp_sync) {
    __block EditorController *c = nil;
    NSString *file = MCGuiProjectFile(@"sync/main.py");
    // CRLF lines, an emoji (two UTF-16 units), and enough text that a few
    // ranges are smaller than the whole.
    NSMutableString *body = [NSMutableString stringWithString:@"import os\r\n\r\n\U0001F600 = 1\r\np = Pa\r\n"];
    for (int k = 0; k < 20; k++) [body appendFormat:@"# line %02d of padding, so ranges pay\r\n", k];
    NSString *original = [body copy];
    NSDictionary *publish = @{@"method": @"textDocument/publishDiagnostics", @"params": @{
        @"uri": @"${uri}", @"diagnostics": @[@{
            @"range": @{@"start": @{@"line": @0, @"character": @0},
                        @"end": @{@"line": @0, @"character": @1}},
            @"severity": @3, @"source": @"copy", @"message": @"${text}"}]}};
    NSDictionary *pathItem = @{@"label": @"Path", @"kind": @7, @"insertText": @"Path",
                               @"data": @{@"id": @1}};
    NSDictionary *resolved = @{@"label": @"Path", @"kind": @7, @"insertText": @"Path",
        @"additionalTextEdits": @[@{@"range": @{@"start": @{@"line": @1, @"character": @0},
                                                @"end": @{@"line": @1, @"character": @0}},
                                    @"newText": kImport}]};
    NSDictionary *(^script)(int) = ^NSDictionary *(int resolveDelayMs) {
        return @{@"name": @"fakelsp",
                 @"capabilities": @{
                     @"textDocumentSync": @{@"openClose": @YES, @"change": @2,
                                            @"save": @{@"includeText": @NO}},
                     @"completionProvider": @{@"resolveProvider": @YES}},
                 @"rules": @[
                     @{@"method": @"textDocument/didOpen", @"when": @{@"uriEndsWith": @"main.py"},
                       @"notify": @[publish]},
                     @{@"method": @"textDocument/didChange", @"when": @{@"uriEndsWith": @"main.py"},
                       @"notify": @[publish]},
                     @{@"method": @"textDocument/completion", @"result":
                         @{@"isIncomplete": @NO, @"items": @[pathItem]}},
                     @{@"method": @"completionItem/resolve", @"delayMs": @(resolveDelayMs),
                       @"result": resolved}]};
    };

    MCGuiStep(@"a server that takes ranged changes", ^{
        c = MCGuiController();
        MCGuiWriteFile(file, original);
        NSString *cmd = MCGuiFakeLsp(@"sync", script(300), nil, nil);
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = off\n", cmd]);
        GCHECK([c showFile:file]);
    });
    MCGuiWait(@"its copy of the file arrives", 20, ^BOOL {
        return [ServerCopy(c) isEqualToString:Text(c).string];
    });

    // ------------------------------------------------ incremental didChange
    MCGuiStep(@"edits at two places, after the emoji and in a CRLF line, in one pause", ^{
        MCGuiFakeLspClearLog(@"sync");
        NSTextView *tv = Text(c);
        [c.window makeFirstResponder:tv];
        NSString *s = tv.string;
        NSUInteger emoji = NSMaxRange([s rangeOfString:@"\U0001F600"]);
        TypeAt(c, emoji, 0, @"x");
        NSRange pad = [tv.string rangeOfString:@"line 07"];
        TypeAt(c, pad.location, 4, @"row\U0001F42A");
        NSRange second = [tv.string rangeOfString:@"\r\n# line 12"];
        TypeAt(c, second.location, 2, @"");   // joins two lines, CRLF and all
    });
    MCGuiWait(@"the server's copy is the editor's text", 10, ^BOOL {
        return [ServerCopy(c) isEqualToString:Text(c).string];
    });
    MCGuiStep(@"and it came as ranges", ^{
        NSArray *changes = MCGuiFakeLspReceived(@"sync", @"textDocument/didChange");
        GCHECK(changes.count >= 1);
        GCHECK(Count(changes, NO) == 0);
        NSDictionary *last = changes.lastObject;
        GCHECK([last[@"params"][@"contentChanges"] count] == 3);
        MCGuiNote(@"%lu didChange, %lu with ranges", (unsigned long)changes.count,
                  (unsigned long)Count(changes, YES));
        MCGuiFakeLspClearLog(@"sync");
        // Select All and type: more than the text in ranges, so the whole.
        NSTextView *tv = Text(c);
        TypeAt(c, 0, tv.string.length, original);
    });
    MCGuiWait(@"the whole text goes", 10, ^BOOL {
        return [ServerCopy(c) isEqualToString:Text(c).string] &&
               MCGuiFakeLspReceived(@"sync", @"textDocument/didChange").count > 0;
    });
    MCGuiStep(@"as one full change", ^{
        NSArray *changes = MCGuiFakeLspReceived(@"sync", @"textDocument/didChange");
        GCHECK(Count(changes, YES) == 0);
        GCHECK_EQ_STR(Text(c).string, original);
    });

    // ------------------------------------------------ a pause in a 2.5 MB file
    // The flush reads only what was typed (and finds its positions from the
    // line starts the client keeps); converting the whole buffer to UTF-8,
    // as it did, cost about 9 ms of the main thread here. The baseline is
    // that conversion, timed on this thread in the same step.
    NSString *big = MCGuiProjectFile(@"sync/big.py");
    MCGuiStep(@"a 2.5 MB file", ^{
        NSMutableString *s = [NSMutableString string];
        while (s.length < 2500000)
            [s appendFormat:@"def f_%lu(x):  # \U0001F600 café\r\n    return x\r\n",
                            (unsigned long)s.length];
        MCGuiWriteFile(big, s);
        MCGuiFakeLspClearLog(@"sync");
        GCHECK([c showFile:big]);
    });
    MCGuiWait(@"it is open on the server", 20, ^BOOL {
        for (NSDictionary *m in MCGuiFakeLspReceived(@"sync", @"textDocument/didOpen"))
            if ([m[@"params"][@"textDocument"][@"uri"] hasSuffix:@"big.py"]) return YES;
        return NO;
    });
    MCGuiStep(@"typing at two places, then the pause", ^{
        MCGuiFakeLspClearLog(@"sync");
        NSTextView *tv = Text(c);
        [c.window makeFirstResponder:tv];
        NSRange mid = [tv.string rangeOfString:@"return x" options:0
                                         range:NSMakeRange(tv.string.length / 2, 200)];
        GCHECK(mid.location != NSNotFound);
        TypeAt(c, mid.location, 0, @"zz");
        TypeAt(c, 10, 0, @"q");
        const double c0 = MCGuiThreadCpuMs();
        const char *whole = tv.string.UTF8String;   // the old flush's first step
        const double baseline = MCGuiThreadCpuMs() - c0;
        GCHECK(strlen(whole) > 2500000);
        [Session(c) performSelector:@selector(flushChanges)];
        const double flush = Session(c).lastFlushCpuMs;
        MCGuiNote(@"the flush: %.2f ms of the main thread's CPU (the buffer as UTF-8: %.2f ms)",
                  flush, baseline);
        // About 0.8 ms here, most of it moving the client's copy along
        // (the text after each edit, and its line starts); room is left
        // for an efficiency core (3.2 ms at load 100 to 125, with several
        // agents building), and the baseline ratio holds either way. The
        // old flush took 9 to 13 ms, so 6 still catches a return to it.
        GCHECK_TIME(flush, 6, @"a pause's flush in a 2.5 MB file");
        // The ratio, for a slow machine; a flush that fast needs none (one
        // loaded run's stalled sample failed it at a 15 to 1 norm).
        GCHECK(MCGuiTimingSkipped() || flush < 1.5 || flush * 4 < baseline);
    });
    MCGuiWait(@"one ranged change with both edits", 10, ^BOOL {
        return MCGuiFakeLspReceived(@"sync", @"textDocument/didChange").count > 0;
    });
    MCGuiStep(@"as ranges, with only what was typed", ^{
        NSArray *changes = MCGuiFakeLspReceived(@"sync", @"textDocument/didChange");
        GCHECK(changes.count == 1 && Count(changes, NO) == 0);
        // Each piece holds what was typed there, and at most the rest of
        // its line: an edit's editedRange can come joined to an attribute
        // fix in the same processEditing (seen after other suites ran:
        // "zzreturn x\r\n" for the two letters), and the client sends the
        // text of the range it was told of, which is still right.
        NSArray *texts = [changes.lastObject[@"params"][@"contentChanges"] valueForKey:@"text"];
        NSArray *typed = @[@"zz", @"q"];
        BOOL pieces = texts.count == typed.count;
        for (NSUInteger i = 0; pieces && i < typed.count; i++)
            pieces = [texts[i] hasPrefix:typed[i]] && [texts[i] length] <= 64;
        if (!pieces) MCGuiNote(@"the change: %@", changes.lastObject);
        GCHECK(pieces);
        GCHECK([c showFile:file]);
    });
    MCGuiWait(@"back on main.py, the server's copy is the editor's", 10, ^BOOL {
        return [ServerCopy(c) isEqualToString:Text(c).string];
    });

    // ------------------------------------- the import after the word went in
    // The accept's main-thread CPU time (ms): that it did not wait for the
    // server is the import's absence right after it, not a wall time.
    __block double acceptTook = 0;
    void (^askAtPa)(void) = ^{
        NSTextView *tv = Text(c);
        [c.window makeFirstResponder:tv];
        tv.selectedRange = NSMakeRange(NSMaxRange([tv.string rangeOfString:@"p = Pa"]), 0);
        [Session(c) triggerCompletion];
    };
    void (^accept)(void) = ^{
        GCHECK([Session(c).completionLabels containsObject:@"Path"]);
        NSTextView *tv = Text(c);
        const CFTimeInterval t0 = CACurrentMediaTime();
        const double c0 = MCGuiThreadCpuMs();
        MCGuiUndoGroup(tv.undoManager, ^{
            GCHECK([Session(c) handleCommand:@selector(insertTab:)]);
        });
        acceptTook = MCGuiThreadCpuMs() - c0;
        MCGuiNote(@"accepting took %.1f ms (%.1f ms of CPU)", (CACurrentMediaTime() - t0) * 1000,
                  acceptTook);
    };
    MCGuiStep(@"ask for completions at Pa", askAtPa);
    MCGuiWait(@"the list", 10, ^BOOL { return Session(c).completionVisible; });
    MCGuiStep(@"Tab: the word goes in at once, before the server's import", ^{
        accept();
        GCHECK_TIME(acceptTook, 200, @"accepting");   // the server takes 300 ms
        NSString *s = Text(c).string;
        GCHECK([s containsString:@"p = Path\r\n"]);
        GCHECK(![s containsString:@"pathlib"]);
    });
    MCGuiWait(@"the import follows", 5, ^BOOL {
        return [Text(c).string hasPrefix:kImported];
    });
    MCGuiStep(@"word and import are one undo step; redo brings both", ^{
        NSTextView *tv = Text(c);
        GCHECK([tv.string containsString:@"p = Path\r\n"]);
        // The caret stayed after the word.
        NSUInteger after = NSMaxRange([tv.string rangeOfString:@"p = Path"]);
        GCHECK(tv.selectedRange.location == after);
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, original);
        [tv.undoManager redo];
        GCHECK([tv.string hasPrefix:kImported]);
        GCHECK([tv.string containsString:@"p = Path\r\n"]);
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, original);
    });

    MCGuiStep(@"again, taken back at once", askAtPa);
    MCGuiWait(@"the list", 10, ^BOOL { return Session(c).completionVisible; });
    MCGuiStep(@"Tab, then Cmd+Z before the server answers", ^{
        accept();
        [Text(c).undoManager undo];
        GCHECK_EQ_STR(Text(c).string, original);
    });
    MCGuiPause(0.6);   // negative: the late answer must add nothing
    MCGuiStep(@"the answer adds nothing", ^{ GCHECK_EQ_STR(Text(c).string, original); });

    MCGuiStep(@"again, typing right after the word", askAtPa);
    MCGuiWait(@"the list", 10, ^BOOL { return Session(c).completionVisible; });
    MCGuiStep(@"Tab, then () before the server answers", ^{
        accept();
        NSTextView *tv = Text(c);
        TypeAt(c, tv.selectedRange.location, 0, @"()");
        GCHECK([tv.string containsString:@"p = Path()\r\n"]);
    });
    MCGuiWait(@"the import still follows", 5, ^BOOL {
        return [Text(c).string hasPrefix:kImported];
    });
    MCGuiStep(@"as its own step: undo takes the import, then (), then the word", ^{
        NSTextView *tv = Text(c);
        GCHECK([tv.string containsString:@"p = Path()\r\n"]);
        [tv.undoManager undo];
        GCHECK(![tv.string containsString:@"pathlib"]);
        GCHECK([tv.string containsString:@"p = Path()\r\n"]);
        [tv.undoManager undo];
        GCHECK([tv.string containsString:@"p = Path\r\n"]);
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, original);
    });

    MCGuiStep(@"again, editing the top before the server answers", askAtPa);
    MCGuiWait(@"the list", 10, ^BOOL { return Session(c).completionVisible; });
    MCGuiStep(@"Tab, then a change on the first line", ^{
        accept();
        TypeAt(c, 0, 0, @"#");
    });
    MCGuiWait(@"the import is left out, and the status bar says so", 5, ^BOOL {
        return [Session(c).lastNote containsString:@"left out"];
    });
    MCGuiStep(@"the text has the word and the edit, no import", ^{
        NSTextView *tv = Text(c);
        GCHECK([tv.string hasPrefix:@"#import os"]);
        GCHECK(![tv.string containsString:@"pathlib"]);
        GCHECK([tv.string containsString:@"p = Path\r\n"]);
        [tv.undoManager undo];
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, original);
        // A server that takes longer than a second.
        MCGuiFakeLsp(@"sync", script(1600), nil, nil);
    });
    // No wait: the server reads the script again (by its nanosecond mtime)
    // on its next message.
    MCGuiStep(@"a slow server: ask once more", askAtPa);
    MCGuiWait(@"the list", 10, ^BOOL { return Session(c).completionVisible; });
    MCGuiStep(@"Tab", ^{
        accept();
        GCHECK_TIME(acceptTook, 200, @"accepting");
        GCHECK([Text(c).string containsString:@"p = Path\r\n"]);
    });
    MCGuiWait(@"after a second, the status bar says the import may be missing", 5, ^BOOL {
        return [Session(c).lastNote containsString:@"within a second"];
    });
    MCGuiPause(1.0);   // negative, as above
    MCGuiStep(@"and the late answer adds nothing", ^{
        NSTextView *tv = Text(c);
        GCHECK(![tv.string containsString:@"pathlib"]);
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, original);
    });
    MCGuiWait(@"the server's copy is the editor's text after it all", 10, ^BOOL {
        return [ServerCopy(c) isEqualToString:Text(c).string];
    });
    MCGuiStep(@"servers off", ^{
        MCGuiSetSettings(@"lsp.enabled = false\n");
        [c closeAllTabs:nil];
    });
    MCGuiWait(@"the server is gone", 10, ^BOOL { return !Session(c).hasServer; });
}
