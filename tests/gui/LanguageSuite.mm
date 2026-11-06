// LanguageSuite.mm — `make guitest SUITES=language`: the language server
// fixes of the third review, against a scripted server (tests/gui/
// fake_lsp.py, run by python3; the server parts are skipped with a note
// when python3 is missing).
//
//   - didSave for a tab that is not on show (documentSavedAtPath:text:);
//   - diagnostic tags: marks faded and struck through, and the counts;
//   - an auto-import completion leaving a fold between it and the word shut;
//   - a rename refused when its file changed while the server worked, and
//     applied when it did not;
//   - Go to Symbol in Workspace finishing when the server exits mid-search;
//   - a server that stops reading being stopped;
//   - workspace edits: another window's unsaved tab, and coming back to a
//     preview's tab;
//   - the folder's own .venv: nothing runs from it until the user says so.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "InstallPanel.h"
#import "Lsp.h"
#import "LspServer.h"
#import "FoldView.h"
#include "LspClient.h"
#include <sys/stat.h>

namespace {

CodeTextView *Text(EditorController *c) { return (CodeTextView *)[c valueForKey:@"textView"]; }
LspSession *Session(EditorController *c) { return [c valueForKey:@"lsp"]; }

NSString *Real(NSString *p) {
    char buf[PATH_MAX];
    return realpath(p.fileSystemRepresentation, buf) ? @(buf) : p;
}
NSString *Uri(NSString *path) { return @(Lsp::uriFromPath(Real(path).UTF8String).c_str()); }

NSString *JsonText(id obj) {
    NSData *d = [NSJSONSerialization dataWithJSONObject:obj options:0 error:nil];
    return [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
}

// Every message the fake server has read, oldest first.
NSArray<NSDictionary *> *Logged(NSString *log) {
    NSString *text = [NSString stringWithContentsOfFile:log encoding:NSUTF8StringEncoding error:nil];
    NSMutableArray *out = [NSMutableArray array];
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        if (!line.length) continue;
        id j = [NSJSONSerialization JSONObjectWithData:[line dataUsingEncoding:NSUTF8StringEncoding]
                                               options:0 error:nil];
        if ([j isKindOfClass:NSDictionary.class]) [out addObject:j];
    }
    return out;
}

NSArray<NSDictionary *> *LoggedMethod(NSString *log, NSString *method) {
    NSMutableArray *out = [NSMutableArray array];
    for (NSDictionary *m in Logged(log))
        if ([m[@"method"] isEqual:method]) [out addObject:m];
    return out;
}

NSDictionary *Range(int l0, int c0, int l1, int c1) {
    return @{@"start": @{@"line": @(l0), @"character": @(c0)},
             @"end": @{@"line": @(l1), @"character": @(c1)}};
}

void Type(CodeTextView *tv, NSRange at, NSString *s) {
    MCGuiUndoGroup(tv.undoManager, ^{
        tv.selectedRange = at;
        [tv insertText:s replacementRange:at];
    });
}

// How far, on average, a rect's pixels are from the editor's background:
// faded text is nearer to it than the same text drawn plainly.
double Ink(NSView *view, NSRect rect) {
    NSBitmapImageRep *rep = [view bitmapImageRepForCachingDisplayInRect:rect];
    if (!rep) return 0;
    [view cacheDisplayInRect:rect toBitmapImageRep:rep];
    NSColor *bg = [[AppSettings shared] background:Surface::Editor];
    bg = [bg colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    double sum = 0;
    NSInteger n = 0;
    for (NSInteger y = 0; y < rep.pixelsHigh; y++)
        for (NSInteger x = 0; x < rep.pixelsWide; x++) {
            NSColor *c = [[rep colorAtX:x y:y] colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
            sum += fabs(c.redComponent - bg.redComponent) + fabs(c.greenComponent - bg.greenComponent) +
                   fabs(c.blueComponent - bg.blueComponent);
            n++;
        }
    return n ? sum / n : 0;
}

NSRect RectOf(CodeTextView *tv, NSRange r) {
    NSLayoutManager *lm = tv.layoutManager;
    NSRange g = [lm glyphRangeForCharacterRange:r actualCharacterRange:NULL];
    NSRect rect = [lm boundingRectForGlyphRange:g inTextContainer:tv.textContainer];
    return NSOffsetRect(rect, tv.textContainerOrigin.x, tv.textContainerOrigin.y);
}

}  // namespace

MC_GUI_SUITE(language) {
    NSString *settings = NSProcessInfo.processInfo.environment[@"MINICODE_SETTINGS"];
    NSString *mainPy = MCGuiProjectFile(@"lang/main.py");
    NSString *utilPy = MCGuiProjectFile(@"lang/util.py");
    NSString *notes = MCGuiProjectFile(@"lang/notes.md");
    NSString *dir = [MCGuiScratch() stringByAppendingPathComponent:@"fakelsp"];
    NSString *scriptPath = [dir stringByAppendingPathComponent:@"script.json"];
    NSString *logPath = [dir stringByAppendingPathComponent:@"log.jsonl"];
    NSString *server = [[MCGuiBuildDir() stringByDeletingLastPathComponent]
        stringByAppendingPathComponent:@"tests/gui/fake_lsp.py"];
    NSString *python = MCFindProgram(@"python3");
    NSString *const kMain =
        @"import os\n"                    // 0
        @"\n"                             // 1
        @"def helper(a):\n"               // 2
        @"    b = a + 1\n"                // 3
        @"    return b\n"                 // 4
        @"\n"                             // 5
        @"unused = 1\n"                   // 6
        @"old_name(1)\n"                  // 7
        @"broken(\n"                      // 8
        @"value = helper(2)\n"            // 9
        @"p = Pa\n";                      // 10
    NSString *const kUtil = @"def value_of(x):\n    return x\n";
    __block EditorController *c = nil;
    __block CodeTextView *tv = nil;
    __block NSMutableDictionary *script = nil;
    void (^writeScript)(void) = ^{ MCGuiWriteFile(scriptPath, JsonText(script)); };

    MCGuiStep(@"the project and the scripted server", ^{
        MCGuiWriteFile(mainPy, kMain);
        MCGuiWriteFile(utilPy, kUtil);
        MCGuiWriteFile(notes, @"# Notes\n\nSome text.\n");
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                                   attributes:nil error:nil];
        script = [@{@"capabilities": @{
                        @"textDocumentSync": @1,
                        @"renameProvider": @YES,
                        @"workspaceSymbolProvider": @YES,
                        @"completionProvider": @{@"resolveProvider": @NO},
                    },
                    @"responses": [NSMutableDictionary dictionary],
                    @"publish": @[]} mutableCopy];
        writeScript();
        c = MCGuiController();
        tv = Text(c);
        GCHECK(c && tv);
    });

    if (!python) {
        MCGuiStep(@"python3 is not installed: the scripted server checks are skipped", ^{
            MCGuiNote(@"python3 not found; skipping the scripted language server checks");
        });
    } else {
        MCGuiStep(@"the scripted server for Python files", ^{
            MCGuiWriteFile(settings, [NSString stringWithFormat:
                @"lsp.enabled = true\nlsp.python = %@ %@ %@ %@\nlsp.python.also = off\n",
                python, server, scriptPath, logPath]);
        });
        MCGuiPause(1.0);
        MCGuiStep(@"open util.py, then main.py", ^{
            GCHECK([c showFile:utilPy]);
            GCHECK([c showFile:mainPy]);
            GCHECK_EQ_STR(tv.string, kMain);
        });
        MCGuiWait(@"the server is initialized", 20, ^BOOL {
            return LoggedMethod(logPath, @"initialized").count > 0 && Session(c).hasServer;
        });
        MCGuiWait(@"both files are open on it", 5, ^BOOL {
            return LoggedMethod(logPath, @"textDocument/didOpen").count >= 2;
        });

        // ---------------------------------------------- didSave, any tab
        MCGuiStep(@"a background tab's save tells the server; a file it lacks sends nothing", ^{
            [Session(c) documentSavedAtPath:utilPy text:kUtil];
            [Session(c) documentSavedAtPath:notes text:@"# Notes\n"];
        });
        MCGuiWait(@"didSave for util.py", 5, ^BOOL {
            return LoggedMethod(logPath, @"textDocument/didSave").count > 0;
        });
        MCGuiStep(@"only util.py's", ^{
            NSArray *saves = LoggedMethod(logPath, @"textDocument/didSave");
            GCHECK(saves.count == 1);
            GCHECK_EQ_STR(saves.firstObject[@"params"][@"textDocument"][@"uri"], Uri(utilPy));
        });

        // ---------------------------------------------- tags and counts
        MCGuiStep(@"the server publishes a hint that is unused, a deprecated warning, an error", ^{
            script[@"publish"] = @[@{@"uri": Uri(mainPy), @"diagnostics": @[
                @{@"range": Range(6, 0, 6, 6), @"severity": @4, @"message": @"unused is not accessed",
                  @"tags": @[@1]},
                @{@"range": Range(7, 0, 7, 8), @"severity": @2, @"message": @"old_name is deprecated",
                  @"tags": @[@2]},
                @{@"range": Range(8, 0, 8, 6), @"severity": @1, @"message": @"broken call"},
            ]}];
            writeScript();
            Type(tv, NSMakeRange(tv.string.length, 0), @" ");
        });
        MCGuiWait(@"three marks", 5, ^BOOL { return tv.diagnostics.count == 3; });
        MCGuiStep(@"faded, struck through, and the counts are one error and one warning", ^{
            MCDiagnosticMark *hint = nil, *dep = nil;
            for (MCDiagnosticMark *m in tv.diagnostics) {
                if (m.severity == 4) hint = m;
                if (m.severity == 2) dep = m;
            }
            GCHECK(hint.unnecessary && !hint.deprecated);
            GCHECK(dep.deprecated && !dep.unnecessary);
            GCHECK(Session(c).problemErrors == 1 && Session(c).problemWarnings == 1);
            // "unused" (faded) has less ink than the same letters plain on
            // line 3 ("b = a" is not the same word, so compare with "unused"
            // typed into the line below the error, then taken out).
            NSRange faded = NSMakeRange([tv.string rangeOfString:@"unused = 1"].location, 6);
            [tv.layoutManager ensureLayoutForTextContainer:tv.textContainer];
            double fadedInk = Ink(tv, RectOf(tv, faded));
            NSRange plain = [tv.string rangeOfString:@"helper(a)"];
            double plainInk = Ink(tv, RectOf(tv, NSMakeRange(plain.location, 6)));
            MCGuiNote(@"ink: faded %.4f, plain %.4f", fadedInk, plainInk);
            GCHECK(fadedInk > 0 && fadedInk < plainInk * 0.8);
            MCGuiSnapshot(tv, @"language-tags");
        });
        MCGuiStep(@"typing over the error: Problems no longer counts it, before any publish", ^{
            script[@"publish"] = @[];
            writeScript();
            NSRange err = [tv.string rangeOfString:@"broken("];
            Type(tv, NSMakeRange(err.location + 3, 0), @"X");
            GCHECK(Session(c).problemErrors == 0);
            GCHECK(Session(c).problemWarnings == 1);
            [tv.undoManager undo];
        });

        // -------------------------------- an auto-import and a fold between
        __block NSUInteger defLine = 2;
        MCGuiStep(@"fold helper, then ask for completions at Pa", ^{
            script[@"responses"][@"textDocument/completion"] = @{@"result": @{
                @"isIncomplete": @NO, @"items": @[@{
                    @"label": @"Path", @"kind": @7, @"insertText": @"Path",
                    @"additionalTextEdits": @[@{@"range": Range(0, 0, 0, 0),
                                                @"newText": @"from pathlib import Path\n"}]}]}};
            writeScript();
            GCHECK([tv.folds toggleLine:defLine]);
            GCHECK([tv.folds lineStartsFold:defLine]);
            [c.window makeFirstResponder:tv];
            NSRange pa = [tv.string rangeOfString:@"p = Pa"];
            tv.selectedRange = NSMakeRange(NSMaxRange(pa), 0);
            [Session(c) triggerCompletion];
        });
        MCGuiWait(@"the list", 5, ^BOOL { return Session(c).completionVisible; });
        MCGuiStep(@"Tab takes Path with its import; the fold stays shut", ^{
            GCHECK([Session(c).completionLabels containsObject:@"Path"]);
            MCGuiUndoGroup(tv.undoManager, ^{
                GCHECK([Session(c) handleCommand:@selector(insertTab:)]);
            });
            GCHECK([tv.string hasPrefix:@"from pathlib import Path\nimport os\n"]);
            GCHECK([tv.string containsString:@"p = Path\n"]);
            GCHECK([tv.folds lineStartsFold:defLine + 1]);   // one line further down
            [tv.folds unfoldAll];
            [tv.undoManager undo];
            GCHECK([tv.string hasPrefix:@"import os\n"]);
        });

        // --------------------------------------------------- stale rename
        MCGuiStep(@"rename helper; util.py changes while the server takes its time", ^{
            [script[@"responses"] removeObjectForKey:@"textDocument/completion"];
            script[@"responses"][@"textDocument/rename"] = @{@"delay": @1.5, @"result": @{
                @"changes": @{Uri(utilPy): @[@{@"range": Range(0, 4, 0, 12), @"newText": @"VALUE_OF"}]}}};
            writeScript();
            NSRange h = [tv.string rangeOfString:@"helper"];
            tv.selectedRange = NSMakeRange(h.location + 1, 0);
            [Session(c) renameSymbol];
            NSTextField *field = Session(c).renameField;
            GCHECK(field != nil);
            field.stringValue = @"assist";
            [(id<NSControlTextEditingDelegate>)field.delegate
                                    control:field
                                   textView:(NSTextView *)field.currentEditor ?: [NSTextView new]
                        doCommandBySelector:@selector(insertNewline:)];
            // util.py moves on while the server works: a save of other text
            // from its background tab (keep-sorted on save does that), which
            // goes to the server as didChange v+1 then didSave. A tab switch
            // would not do: it drops the rename's answer anyway.
            [Session(c) documentSavedAtPath:utilPy text:@"def value_of(x):\n    return x  # saved\n"];
        });
        MCGuiWait(@"the rename's answer is refused", 5, ^BOOL {
            return [Session(c).lastNote containsString:@"changed while the language server"];
        });
        MCGuiStep(@"util.py kept its own text; nothing was renamed", ^{
            GCHECK_EQ_STR(Session(c).lastNote,
                          @"util.py changed while the language server worked on it; try again");
            GCHECK([c showFile:utilPy]);
            GCHECK_EQ_STR(tv.string, kUtil);
            GCHECK([c showFile:mainPy]);
        });
        MCGuiStep(@"the same rename with nothing changed meanwhile goes in", ^{
            NSRange h = [tv.string rangeOfString:@"helper"];
            tv.selectedRange = NSMakeRange(h.location + 1, 0);
            [Session(c) renameSymbol];
            NSTextField *field = Session(c).renameField;
            field.stringValue = @"assist";
            [(id<NSControlTextEditingDelegate>)field.delegate
                                    control:field
                                   textView:(NSTextView *)field.currentEditor ?: [NSTextView new]
                        doCommandBySelector:@selector(insertNewline:)];
        });
        MCGuiWait(@"renamed", 5, ^BOOL { return [Session(c).lastNote hasPrefix:@"Renamed"]; });
        MCGuiStep(@"util.py has the new name, unsaved; main.py on show again", ^{
            GCHECK([[c valueForKey:@"currentPath"] isEqualToString:mainPy]);
            GCHECK([c showFile:utilPy]);
            GCHECK([tv.string hasPrefix:@"def VALUE_OF(x)"]);
            NSString *disk = [NSString stringWithContentsOfFile:utilPy encoding:NSUTF8StringEncoding
                                                          error:nil];
            GCHECK_EQ_STR(disk, kUtil);
            [tv.undoManager undo];   // the rename
            GCHECK_EQ_STR(tv.string, kUtil);
            GCHECK([c showFile:mainPy]);
        });

        // ------------------------- another window's unsaved tab, a preview's tab
        __block LspSession *other = nil;
        MCGuiStep(@"another window holding util.py with changes: the edit is refused", ^{
            CodeTextView *otherView = [[CodeTextView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
            other = [[LspSession alloc] initWithTextView:otherView root:MCGuiProject()];
            other.bufferText = ^NSString *(NSString *p) {
                return [Real(p) isEqualToString:Real(utilPy)] ? @"def changed(): pass\n" : nil;
            };
            NSString *failure = nil;
            NSInteger n = [Session(c) applyEditsToPaths:@[utilPy]
                                                 ranges:@[@[[NSValue valueWithRange:NSMakeRange(0, 3)]]]
                                                strings:@[@[@"DEF"]]
                                             actionName:@"Replace in Folder"
                                                failure:&failure];
            GCHECK(n < 0);
            GCHECK([failure containsString:@"unsaved changes in another window"]);
            // The same text as the disk there is no conflict.
            other.bufferText = ^NSString *(NSString *p) {
                return [Real(p) isEqualToString:Real(utilPy)] ? kUtil : nil;
            };
            GCHECK([c showFile:notes]);
            GCHECK([[c valueForKey:@"previewMode"] boolValue]);
            failure = nil;
            n = [Session(c) applyEditsToPaths:@[utilPy]
                                       ranges:@[@[[NSValue valueWithRange:NSMakeRange(0, 3)]]]
                                      strings:@[@[@"DEF"]]
                                   actionName:@"Replace in Folder"
                                      failure:&failure];
            if (failure) MCGuiNote(@"failure: %@", failure);
            GCHECK(n == 1);
            // Back on the preview it started from, not on util.py.
            GCHECK([[c valueForKey:@"currentPath"] isEqualToString:notes]);
            GCHECK([[c valueForKey:@"previewMode"] boolValue]);
            [other shutdown];
            other = nil;
            GCHECK([c showFile:utilPy]);
            GCHECK([tv.string hasPrefix:@"DEF value_of"]);
            [tv.undoManager undo];
            GCHECK_EQ_STR(tv.string, kUtil);
            GCHECK([c showFile:mainPy]);
        });

        // ------------------------------------ workspace symbols, server exit
        __block BOOL complete = NO;
        __block NSInteger answers = 0;
        MCGuiStep(@"a workspace search the server never answers", ^{
            script[@"responses"][@"workspace/symbol"] = @"never";
            writeScript();
            [Session(c) workspaceSymbols:@"val" completion:^(NSArray *items, BOOL done,
                                                              NSString *message) {
                (void)items; (void)message;
                answers++;
                if (done) complete = YES;
            }];
        });
        MCGuiWait(@"the request reached the server", 5, ^BOOL {
            return LoggedMethod(logPath, @"workspace/symbol").count > 0;
        });
        MCGuiStep(@"still searching; then the server is killed", ^{
            GCHECK(!complete);
            for (MCLspServerInfo *info in Session(c).serverInfos)
                if ([info.key isEqualToString:@"python"] && info.pid > 0) kill(info.pid, SIGKILL);
        });
        MCGuiWait(@"the search finishes with nothing", 5, ^BOOL { return complete; });
        MCGuiStep(@"one answer", ^{ GCHECK(answers == 1); });
        MCGuiWait(@"the server is back", 10, ^BOOL {
            for (MCLspServerInfo *info in Session(c).serverInfos)
                if ([info.key isEqualToString:@"python"] && info.state == MCLspStateRunning)
                    return YES;
            return NO;
        });

        // ------------------------------------- a server that stops reading
        MCGuiStep(@"a server that stops reading after it starts", ^{
            script[@"stopReading"] = @YES;
            writeScript();
            MCLspSetMaxQueuedBytes(256 << 10);
            [Session(c) restartServer:@"python"];
        });
        MCGuiWait(@"it started", 10, ^BOOL {
            for (MCLspServerInfo *info in Session(c).serverInfos)
                if ([info.key isEqualToString:@"python"] && info.state == MCLspStateRunning)
                    return YES;
            return NO;
        });
        MCGuiPause(0.5);
        MCGuiStep(@"half a megabyte for it to read", ^{
            NSMutableString *big = [NSMutableString string];
            for (int i = 0; i < 4000; i++) [big appendFormat:@"line_%d = %d  # padding padding\n", i, i];
            for (int k = 0; k < 8; k++)
                [Session(c) documentSavedAtPath:mainPy
                                           text:[big stringByAppendingFormat:@"# %d\n", k]];
        });
        MCGuiWait(@"it is stopped for not reading", 10, ^BOOL {
            for (MCLspServerInfo *info in Session(c).serverInfos)
                if ([info.key isEqualToString:@"python"] &&
                    [info.message containsString:@"stopped reading"])
                    return YES;
            return NO;
        });
        MCGuiStep(@"the scripted server off", ^{
            MCLspSetMaxQueuedBytes(0);
            MCGuiWriteFile(settings, @"lsp.enabled = false\n");
        });
        MCGuiPause(1.0);
    }

    // ------------------------------------------ the folder's own .venv
    NSString *venvBin = MCGuiProjectFile(@".venv/bin");
    NSString *marker = [MCGuiScratch() stringByAppendingPathComponent:@"venv-server-ran"];
    MCGuiStep(@"a .venv whose bin holds the only Python server", ^{
        NSString *pyExe = [venvBin stringByAppendingPathComponent:@"python"];
        NSString *srv = [venvBin stringByAppendingPathComponent:@"mc-venv-server"];
        MCGuiWriteFile(pyExe, @"#!/bin/sh\nexit 0\n");
        MCGuiWriteFile(srv, [NSString stringWithFormat:@"#!/bin/sh\ntouch '%@'\ncat > /dev/null\n",
                                                       marker]);
        chmod(pyExe.fileSystemRepresentation, 0755);
        chmod(srv.fileSystemRepresentation, 0755);
        MCGuiWriteFile(settings, @"lsp.enabled = true\nlsp.python = mc-venv-server\n"
                                 @"lsp.python.also = off\n");
    });
    MCGuiPause(1.0);
    MCGuiStep(@"opening a Python file asks, and runs nothing from .venv", ^{
        GCHECK([c showFile:notes]);
        GCHECK([c showFile:mainPy]);
        GCHECK_EQ_STR(Session(c).folderEnvironmentAwaitingConsent, @".venv");
        GCHECK(c.installBar && !c.installBar.hidden);
        GCHECK_EQ_STR(c.installBar.consentEnvironment, @".venv");
        GCHECK([c.installBar.text containsString:@".venv"]);
        GCHECK_EQ_STR(c.installBar.installButton.title, @"Use It");
        GCHECK(NSHeight(c.installBar.frame) > 0);
        NSString *source = nil;
        GCHECK([Session(c) serverPythonInterpreterWithSource:&source] == nil ||
               ![[Session(c) serverPythonInterpreterWithSource:nil] containsString:@".venv"]);
        GCHECK([source containsString:@"waits for your answer"]);
        // A run the user starts still uses it.
        GCHECK([[Session(c) pythonInterpreterWithSource:nil] hasSuffix:@".venv/bin/python"]);
        MCGuiSnapshot(c.installBar, @"language-consent-bar");
    });
    MCGuiPause(1.0);
    MCGuiStep(@"nothing ran", ^{
        GCHECK(![[NSFileManager defaultManager] fileExistsAtPath:marker]);
    });
    MCGuiStep(@"Don't Use It: the bar goes, the answer is kept, still nothing runs", ^{
        NSButton *mute = nil;
        for (NSView *v in c.installBar.subviews)
            if ([v isKindOfClass:NSButton.class] && v != c.installBar.installButton &&
                [((NSButton *)v).title containsString:@"Use"])
                mute = (NSButton *)v;
        GCHECK(mute != nil);
        [mute performClick:nil];
        GCHECK_EQ_STR([Session(c) folderEnvironmentConsent], @"deny");
        GCHECK(Session(c).folderEnvironmentAwaitingConsent == nil);
        GCHECK(c.installBar.consentEnvironment == nil);
        NSString *source = nil;
        [Session(c) serverPythonInterpreterWithSource:&source];
        GCHECK([source containsString:@"not used"]);
    });
    MCGuiPause(1.0);
    MCGuiStep(@"still nothing ran; the answer is forgotten for the next check", ^{
        GCHECK(![[NSFileManager defaultManager] fileExistsAtPath:marker]);
        NSUserDefaults *d = [[NSUserDefaults alloc]
            initWithSuiteName:NSProcessInfo.processInfo.environment[@"MINICODE_DEFAULTS_SUITE"]];
        [d removeObjectForKey:@"MCPythonEnvironmentConsent"];
        GCHECK([c showFile:notes]);
        GCHECK([c showFile:mainPy]);
    });
    MCGuiWait(@"the bar asks again", 5, ^BOOL {
        return [c.installBar.consentEnvironment isEqualToString:@".venv"] && !c.installBar.hidden;
    });
    MCGuiStep(@"Use It", ^{ [c.installBar.installButton performClick:nil]; });
    MCGuiWait(@"the server from .venv runs", 10, ^BOOL {
        return [[NSFileManager defaultManager] fileExistsAtPath:marker];
    });
    MCGuiStep(@"allowed, the bar gone, and the servers analyse with .venv's python", ^{
        GCHECK(Session(c).folderEnvironmentAllowed);
        GCHECK(c.installBar.hidden || c.installBar.consentEnvironment == nil);
        GCHECK([[Session(c) serverPythonInterpreterWithSource:nil] hasSuffix:@".venv/bin/python"]);
    });
    MCGuiStep(@"put off with ×: hidden for this launch, still not allowed", ^{
        MCGuiWriteFile(settings, @"lsp.enabled = false\n");
        NSUserDefaults *d = [[NSUserDefaults alloc]
            initWithSuiteName:NSProcessInfo.processInfo.environment[@"MINICODE_DEFAULTS_SUITE"]];
        [d removeObjectForKey:@"MCPythonEnvironmentConsent"];
    });
    MCGuiPause(1.0);
    MCGuiStep(@"×", ^{
        MCGuiWriteFile(settings, @"lsp.enabled = true\nlsp.python = mc-venv-server\n"
                                 @"lsp.python.also = off\n");
    });
    MCGuiPause(1.0);
    MCGuiStep(@"the question once more, then ×", ^{
        GCHECK([c showFile:notes]);
        GCHECK([c showFile:mainPy]);
        GCHECK_EQ_STR(c.installBar.consentEnvironment, @".venv");
        NSButton *close = nil;
        for (NSView *v in c.installBar.subviews)
            if ([v isKindOfClass:NSButton.class] && [((NSButton *)v).title isEqualToString:@"×"])
                close = (NSButton *)v;
        GCHECK(close != nil);
        [close performClick:nil];
        GCHECK(c.installBar.hidden);
        GCHECK(Session(c).folderEnvironmentConsent == nil);
        GCHECK(!Session(c).folderEnvironmentAllowed);
    });
    MCGuiStep(@"tidy up: servers off, no .venv", ^{
        MCGuiWriteFile(settings, @"lsp.enabled = false\n");
        [[NSFileManager defaultManager] removeItemAtPath:MCGuiProjectFile(@".venv") error:nil];
    });
    MCGuiPause(1.0);
}
