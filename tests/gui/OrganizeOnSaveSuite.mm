// OrganizeOnSaveSuite.mm — `make guitest SUITES=organize_on_save`:
// editor.organize-imports-on-save against the scripted server
// (build/fakelsp) run as Ruff beside a main Python server. Cmd+S organizes
// the imports through Ruff's source.organizeImports.ruff, then formats
// (format.on-save, a script that adds a line here) and writes both to disk; the
// setting off leaves the imports alone; an auto-save never asks; and a
// server that never answers holds a save up for 3 seconds (5 at most).
#import "Harness.h"
#import "EditorController.h"
#import "Lsp.h"
#include <sys/stat.h>

namespace {

NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
LspSession *Lsp(EditorController *c) { return [c valueForKey:@"lsp"]; }
NSString *Disk(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}
NSDictionary *Pos(int line, int ch) { return @{@"line": @(line), @"character": @(ch)}; }
NSDictionary *Edit(int l0, int c0, int l1, int c1, NSString *text) {
    return @{@"range": @{@"start": Pos(l0, c0), @"end": Pos(l1, c1)}, @"newText": text};
}

// How many organize-imports requests the server named `name` has had.
NSUInteger OrganizeAsks(NSString *name) {
    NSUInteger n = 0;
    for (NSDictionary *m in MCGuiFakeLspReceived(name, @"textDocument/codeAction")) {
        NSData *d = [NSJSONSerialization dataWithJSONObject:m options:0 error:nil];
        NSString *s = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
        if ([s containsString:@"source.organizeImports"]) n++;
    }
    return n;
}

// The file's text as the server had it when it was first asked to
// organize: its didOpen, or the last didChange (full sync) before.
NSString *TextWhenAsked(NSString *server, NSString *name) {
    NSString *text = nil;
    for (NSDictionary *m in MCGuiFakeLspReceived(server, nil)) {
        NSDictionary *p = m[@"params"];
        if (![p isKindOfClass:NSDictionary.class]) continue;
        NSString *uri = p[@"textDocument"][@"uri"];
        if (![uri isKindOfClass:NSString.class] || ![uri hasSuffix:name]) continue;
        if ([m[@"method"] isEqual:@"textDocument/didOpen"]) text = p[@"textDocument"][@"text"];
        else if ([m[@"method"] isEqual:@"textDocument/didChange"])
            text = [p[@"contentChanges"] lastObject][@"text"];
        else if ([m[@"method"] isEqual:@"textDocument/codeAction"]) return text;
    }
    return nil;
}

NSString *kUnsorted = @"import sys\nimport os\n\nprint(sys, os)\n";
NSString *kSorted = @"import os\nimport sys\n\nprint(sys, os)\n";

id RuffScript(int delayMs) {
    NSMutableDictionary *organize = [@{
        @"method": @"textDocument/codeAction",
        @"when": @{@"paramsContain": @"source.organizeImports", @"textContains": @"import sys\nimport os"},
        @"result": @[@{@"title": @"Ruff: Organize imports", @"kind": @"source.organizeImports.ruff",
                       @"edit": @{@"changes": @{@"${uri}": @[Edit(0, 0, 2, 0, @"import os\nimport sys\n")]}}}]
    } mutableCopy];
    if (delayMs) organize[@"delayMs"] = @(delayMs);
    return @{@"name": @"ruff",
             @"capabilities": @{@"textDocumentSync": @1,
                                @"codeActionProvider": @{@"codeActionKinds":
                                    @[@"quickfix", @"source.organizeImports.ruff"]}},
             @"rules": @[organize, @{@"method": @"textDocument/codeAction", @"result": @[]}]};
}

}  // namespace

MC_GUI_SUITE(organize_on_save) {
    __block EditorController *c = nil;
    __block NSString *settings = nil;
    NSString *file = MCGuiProjectFile(@"organize/imports.py");

    MCGuiStep(@"Python with Ruff beside a main server, organize and format on save", ^{
        c = MCGuiController();
        NSString *mainCmd = MCGuiFakeLsp(@"om", @{
            @"name": @"fakelsp",
            @"capabilities": @{@"textDocumentSync": @1, @"codeActionProvider": @YES},
            @"rules": @[@{@"method": @"textDocument/codeAction", @"result": @[]}]}, nil, nil);
        NSString *ruffCmd = MCGuiFakeLsp(@"oruff", RuffScript(0), @"ruff", @"server");
        // A formatter that adds a line, so its run shows in the file.
        NSString *fmt = [MCGuiScratch() stringByAppendingPathComponent:@"bin/add-a-line"];
        MCGuiWriteFile(fmt, @"#!/bin/sh\n/bin/cat\necho '# formatted'\n");
        chmod(fmt.fileSystemRepresentation, 0755);
        settings = [NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = %@\nformat.python = %@\n",
            mainCmd, ruffCmd, fmt];
        MCGuiSetSettings([settings stringByAppendingString:@"editor.organize-imports-on-save = true\n"
                                                           @"format.on-save = true\n"]);
        MCGuiWriteFile(file, @"import sys\n\nprint(sys)\n");
        GCHECK([c showFile:file]);
    });
    MCGuiWait(@"both servers run", 20, ^BOOL {
        return Lsp(c).fileServerNames.count == 2 &&
               MCGuiFakeLspReceived(@"oruff", @"initialized").count > 0 &&
               MCGuiFakeLspReceived(@"om", @"initialized").count > 0;
    });
    MCGuiStep(@"an import added; Cmd+S", ^{
        NSTextView *tv = Text(c);
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@"import os\n" replacementRange:NSMakeRange(11, 0)];
            [tv insertText:@" os" replacementRange:NSMakeRange(tv.string.length - 2, 0)];
            [tv insertText:@"," replacementRange:NSMakeRange(tv.string.length - 5, 0)];
        });
        GCHECK_EQ_STR(tv.string, kUnsorted);
        [c saveCurrentFile:c];
    });
    MCGuiWait(@"organized, formatted and written", 15, ^BOOL {
        return [Disk(file) isEqualToString:[kSorted stringByAppendingString:@"# formatted\n"]];
    });
    MCGuiStep(@"Ruff was asked before the formatter ran; the tab is clean", ^{
        GCHECK(OrganizeAsks(@"oruff") == 1);
        NSString *asked = TextWhenAsked(@"oruff", @"imports.py");
        GCHECK_EQ_STR(asked, kUnsorted);
        GCHECK_EQ_STR(Text(c).string, [kSorted stringByAppendingString:@"# formatted\n"]);
        GCHECK(![[c valueForKey:@"dirty"] boolValue]);
        GCHECK_EQ_STR(Lsp(c).lastNote, @"Organized imports (ruff)");
    });

    MCGuiStep(@"the setting off: Cmd+S leaves the imports as they are", ^{
        MCGuiSetSettings(settings);
        MCGuiFakeLspClearLog(@"oruff");
        NSTextView *tv = Text(c);
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:kUnsorted replacementRange:NSMakeRange(0, tv.string.length)];
        });
        [c saveCurrentFile:c];
    });
    MCGuiWait(@"written as typed", 10, ^BOOL { return [Disk(file) isEqualToString:kUnsorted]; });
    MCGuiStep(@"no organize request went out", ^{
        GCHECK(OrganizeAsks(@"oruff") == 0);
    });

    MCGuiStep(@"auto-save with the setting on: written unasked, imports untouched", ^{
        MCGuiSetSettings([settings stringByAppendingString:@"editor.organize-imports-on-save = true\n"
                                                           @"editor.auto-save = after-delay\n"
                                                           @"editor.auto-save-delay = 200\n"]);
        NSTextView *tv = Text(c);
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@"# more\n" replacementRange:NSMakeRange(tv.string.length, 0)];
        });
    });
    MCGuiWait(@"auto-saved", 10, ^BOOL {
        return [Disk(file) isEqualToString:[kUnsorted stringByAppendingString:@"# more\n"]];
    });
    MCGuiPause(0.5);
    MCGuiStep(@"still no organize request", ^{
        GCHECK(OrganizeAsks(@"oruff") == 0);
        GCHECK_EQ_STR(Text(c).string, [kUnsorted stringByAppendingString:@"# more\n"]);
    });

    __block NSDate *saveAsked = nil;
    MCGuiStep(@"a server that never answers in time: Cmd+S saves after 3 s", ^{
        MCGuiFakeLsp(@"oruff", RuffScript(9000), @"ruff", @"server");
        MCGuiSetSettings([settings stringByAppendingString:@"editor.organize-imports-on-save = true\n"]);
        NSTextView *tv = Text(c);
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@"# slow\n" replacementRange:NSMakeRange(tv.string.length, 0)];
        });
        saveAsked = [NSDate date];
        [c saveCurrentFile:c];
    });
    MCGuiWait(@"saved anyway", 12, ^BOOL { return [Disk(file) hasSuffix:@"# slow\n"]; });
    MCGuiStep(@"when the servers' 3 s are up, not before, and unorganized", ^{
        NSTimeInterval took = -saveAsked.timeIntervalSinceNow;
        MCGuiNote(@"the save took %.1f s", took);
        GCHECK(took >= 2.5 && took < 6);
        GCHECK([Disk(file) hasPrefix:@"import sys\nimport os\n"]);
    });
    MCGuiPause(6.5);   // the slow answer arrives and is dropped
    MCGuiStep(@"the late answer changed nothing", ^{
        GCHECK([Text(c).string hasPrefix:@"import sys\nimport os\n"]);
        GCHECK(![[c valueForKey:@"dirty"] boolValue]);
    });

    MCGuiStep(@"language servers off again; unsaved tabs closed unasked", ^{
        MCGuiSetSettings(@"lsp.enabled = false\n");
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);   // Don't Save
        [c closeAllTabs:nil];
    });
    MCGuiPause(0.3);
}
