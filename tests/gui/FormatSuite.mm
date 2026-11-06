// FormatSuite.mm — `make guitest SUITES=format`: Format Document
// (Shift+Option+F) through a formatter named in the settings. 300 changed
// lines spread over a 3,000-line file go in as one undo step with one
// didChangeText (each hunk had its own, and NSTextView relaid the file out
// from each: 58 s for 298 hunks in 100,000 lines), the caret stays on its
// text, a formatter that fails leaves the buffer alone, and nothing is
// saved.
#import "Harness.h"
#import "EditorController.h"
#import "Lsp.h"
#import "TreeDetails.h"   // MCAppDefaults
#include <sys/stat.h>

namespace {
NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
NSString *Disk(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}
void Script(NSString *path, NSString *body) {
    MCGuiWriteFile(path, body);
    chmod(path.fileSystemRepresentation, 0755);
}
}  // namespace

MC_GUI_SUITE(format) {
    __block EditorController *c = nil;
    __block NSUInteger changes = 0;
    __block id observer = nil;
    __block NSString *expected = nil, *original = nil;
    NSString *file = MCGuiProjectFile(@"fmt/spaced.py");
    NSString *fmt = [MCGuiScratch() stringByAppendingPathComponent:@"bin/spacer"];
    NSString *bad = [MCGuiScratch() stringByAppendingPathComponent:@"bin/failing-formatter"];

    MCGuiStep(@"a 3,000-line file with 300 lines to change, and a formatter", ^{
        NSMutableString *before = [NSMutableString string], *after = [NSMutableString string];
        for (int i = 0; i < 3000; i++) {
            if (i % 10 == 5) {
                [before appendFormat:@"x%d=%d\n", i, i];
                [after appendFormat:@"x%d = %d\n", i, i];
            } else {
                [before appendFormat:@"# line %d stays as it is\n", i];
                [after appendFormat:@"# line %d stays as it is\n", i];
            }
        }
        original = before;
        expected = after;
        MCGuiWriteFile(file, before);
        // Reads the buffer on stdin, as every formatter here does.
        Script(fmt, @"#!/bin/sh\nexec /usr/bin/sed -E 's/^(x[0-9]+)=([0-9]+)$/\\1 = \\2/'\n");
        Script(bad, @"#!/bin/sh\ncat >/dev/null\necho 'spaced.py:3: cannot parse' >&2\nexit 1\n");
        MCGuiSetSettings([NSString stringWithFormat:@"lsp.enabled = false\nformat.python = %@\n", fmt]);
        c = MCGuiController();
        [c focusEditor:nil];
        GCHECK([c showFile:file]);
        // The caret on line 1506 (x1505=1505), after the "=".
        NSRange line = [Text(c).string rangeOfString:@"x1505=1505"];
        Text(c).selectedRange = NSMakeRange(line.location + 6, 0);
        observer = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSTextDidChangeNotification object:Text(c) queue:nil
                    usingBlock:^(NSNotification *) { changes++; }];
    });
    MCGuiStep(@"Shift+Option+F", ^{
        changes = 0;
        // kVK_ANSI_F with Shift and Option: the key monitor offers it to the
        // menu first while the editor has the keyboard (it is also a
        // character, Ï).
        [c.window makeFirstResponder:Text(c)];
        MCGuiSendKeyToApp(c.window, 3, NSEventModifierFlagShift | NSEventModifierFlagOption,
                          @"Ï", @"f");
    });
    MCGuiWait(@"the formatted text", 15, ^BOOL { return [Text(c).string isEqualToString:expected]; });
    MCGuiStep(@"one didChangeText, the caret on its text, unsaved, one undo", ^{
        MCGuiNote(@"didChangeText for 300 hunks: %lu", (unsigned long)changes);
        GCHECK(changes == 1);
        NSRange line = [Text(c).string rangeOfString:@"x1505 = 1505"];
        // After "x1505=", the caret maps into the changed line's "= "
        // (counted by non-blank characters within the hunk).
        GCHECK(Text(c).selectedRange.location >= line.location + 5 &&
               Text(c).selectedRange.location <= line.location + 8);
        GCHECK([[c valueForKey:@"dirty"] boolValue]);
        GCHECK_EQ_STR(Disk(file), original);
        [Text(c).undoManager undo];
        GCHECK_EQ_STR(Text(c).string, original);
        GCHECK(![[c valueForKey:@"dirty"] boolValue]);
        [Text(c).undoManager redo];
        GCHECK_EQ_STR(Text(c).string, expected);
        [Text(c).undoManager undo];
    });
    MCGuiStep(@"a formatter that fails", ^{
        MCGuiSetSettings([NSString stringWithFormat:@"lsp.enabled = false\nformat.python = %@\n", bad]);
        changes = 0;
        [c formatDocument:nil];
    });
    MCGuiWait(@"its note", 15, ^BOOL {
        return [[c valueForKey:@"lastFormatNote"] containsString:@"cannot parse"];
    });
    MCGuiStep(@"the buffer is untouched, the formatter's complaint shown", ^{
        MCGuiNote(@"the note: %@", [c valueForKey:@"lastFormatNote"]);
        GCHECK(changes == 0);
        GCHECK_EQ_STR(Text(c).string, original);
        [[NSNotificationCenter defaultCenter] removeObserver:observer];
        MCGuiSetSettings(@"lsp.enabled = false\n");
    });
}

// `make guitest SUITES=format_consent`: a formatter in the project's own
// .venv runs only once the folder's own programs are allowed, the answer
// the language servers ask for (MCPythonEnvironmentConsent, one per
// folder); format.<lang> in the settings never asks while the tool is
// installed outside the folder (the owner's `format.python = ruff ...`).
MC_GUI_SUITE(format_consent) {
    __block EditorController *c = nil;
    __block id consentBefore = nil;
    __block NSUInteger alerts = 0;
    NSString *dir = MCGuiProjectFile(@"fmtc");
    NSString *file = [dir stringByAppendingPathComponent:@"a.py"];
    NSString *local = [dir stringByAppendingPathComponent:@".venv/bin/mcfmt"];
    NSString *localBlack = [dir stringByAppendingPathComponent:@".venv/bin/black"];
    NSString *outside = [NSHomeDirectory() stringByAppendingPathComponent:@".local/bin/mcfmt"];
    NSString *ranLocal = [MCGuiScratch() stringByAppendingPathComponent:@"fmtc-local-ran"];
    NSString *ranOutside = [MCGuiScratch() stringByAppendingPathComponent:@"fmtc-outside-ran"];
    NSString *ranBlack = [MCGuiScratch() stringByAppendingPathComponent:@"fmtc-black-ran"];
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *(^script)(NSString *) = ^NSString *(NSString *marker) {
        return [NSString stringWithFormat:
            @"#!/bin/sh\n/usr/bin/touch '%@'\nexec /usr/bin/sed -E 's/^x=1$/x = 1/'\n", marker];
    };
    void (^forget)(void) = ^{
        [MCAppDefaults() removeObjectForKey:@"MCPythonEnvironmentConsent"];
    };
    NSString *(^answer)(void) = ^NSString * { return MCFolderToolsConsent(MCGuiProject()); };
    BOOL (^ran)(NSString *) = ^BOOL(NSString *p) { return [fm fileExistsAtPath:p]; };

    MCGuiStep(@"mcfmt in the folder's fmtc/.venv and in ~/.local/bin; format.python = mcfmt", ^{
        consentBefore = [MCAppDefaults() objectForKey:@"MCPythonEnvironmentConsent"];
        forget();
        MCGuiWriteFile(file, @"x=1\n");
        Script(local, script(ranLocal));
        Script(outside, script(ranOutside));
        MCGuiSetSettings(@"lsp.enabled = false\nformat.python = mcfmt\n");
        c = MCGuiController();
        GCHECK([c showFile:file]);
        alerts = MCGuiAlertsShown().count;
        [c formatDocument:nil];
    });
    MCGuiWait(@"formatted by the copy outside the folder", 15, ^BOOL {
        return [Text(c).string isEqualToString:@"x = 1\n"];
    });
    MCGuiStep(@"no question asked, the folder's copy not run, nothing stored", ^{
        GCHECK(MCGuiAlertsShown().count == alerts);
        GCHECK(ran(ranOutside));
        GCHECK(!ran(ranLocal));
        GCHECK(answer() == nil);
        [Text(c).undoManager undo];
        GCHECK_EQ_STR(Text(c).string, @"x=1\n");
        [fm removeItemAtPath:outside error:nil];
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);   // Don't Allow
        [c formatDocument:nil];
    });
    MCGuiWait(@"only the folder has it: asked, Don't Allow, a note", 15, ^BOOL {
        return MCGuiAlertsShown().count == alerts + 1 &&
               [[c valueForKey:@"lastFormatNote"] containsString:@"not allowed"];
    });
    MCGuiStep(@"the question named the tool and its folder; nothing ran; deny kept", ^{
        NSString *asked = MCGuiAlertsShown().lastObject;
        MCGuiNote(@"asked: %@; the note: %@", asked, [c valueForKey:@"lastFormatNote"]);
        GCHECK([asked containsString:@"mcfmt"] && [asked containsString:@"fmtc/.venv"]);
        GCHECK(!ran(ranLocal));
        GCHECK_EQ_STR(Text(c).string, @"x=1\n");
        GCHECK_EQ_STR(answer(), @"deny");
        [c setValue:nil forKey:@"lastFormatNote"];
        [c formatDocument:nil];
    });
    MCGuiWait(@"formatting again: the note at once", 15, ^BOOL {
        return [[c valueForKey:@"lastFormatNote"] containsString:@"not allowed"];
    });
    MCGuiStep(@"not asked again, still nothing ran; then forgotten, and Allow", ^{
        GCHECK(MCGuiAlertsShown().count == alerts + 1);
        GCHECK(!ran(ranLocal));
        forget();
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);   // Allow
        [c formatDocument:nil];
    });
    MCGuiWait(@"Allow: the folder's copy formats", 15, ^BOOL {
        return [Text(c).string isEqualToString:@"x = 1\n"];
    });
    MCGuiStep(@"asked once, the folder's copy ran, allow kept", ^{
        GCHECK(MCGuiAlertsShown().count == alerts + 2);
        GCHECK(ran(ranLocal));
        GCHECK_EQ_STR(answer(), @"allow");
        [Text(c).undoManager undo];
        [fm removeItemAtPath:ranLocal error:nil];
        [c formatDocument:nil];
    });
    MCGuiWait(@"a second format runs it without asking", 15, ^BOOL {
        return [Text(c).string isEqualToString:@"x = 1\n"];
    });
    MCGuiStep(@"the answer persists", ^{
        GCHECK(MCGuiAlertsShown().count == alerts + 2);
        GCHECK(ran(ranLocal));
        [Text(c).undoManager undo];
        [fm removeItemAtPath:ranLocal error:nil];
        // The project chooses Black ([tool.black]) and has it in its .venv:
        // asked, whatever black is installed elsewhere; Cancel stores
        // nothing and runs nothing.
        forget();
        MCGuiWriteFile([dir stringByAppendingPathComponent:@"pyproject.toml"],
                       @"[tool.black]\nline-length = 88\n");
        Script(localBlack, script(ranBlack));
        MCGuiSetSettings(@"lsp.enabled = false\n");
        MCGuiAnswerAlerts(@[@(NSAlertThirdButtonReturn)]);   // Cancel
        [c formatDocument:nil];
    });
    MCGuiWait(@"the project's black: asked", 15, ^BOOL {
        return MCGuiAlertsShown().count == alerts + 3;
    });
    MCGuiPause(0.5);   // before a negative check: black must not start
    MCGuiStep(@"Cancel: black not run, nothing stored, the buffer as it was", ^{
        GCHECK([MCGuiAlertsShown().lastObject containsString:@"black"]);
        GCHECK(!ran(ranBlack));
        GCHECK(answer() == nil);
        GCHECK_EQ_STR(Text(c).string, @"x=1\n");
        // format.on-save: Cmd+S asks too, and Don't Allow saves unformatted.
        [fm removeItemAtPath:[dir stringByAppendingPathComponent:@"pyproject.toml"] error:nil];
        MCGuiSetSettings(@"lsp.enabled = false\nformat.python = mcfmt\nformat.on-save = true\n");
        [Text(c) insertText:@"y=2\n" replacementRange:NSMakeRange(Text(c).string.length, 0)];
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);   // Don't Allow
        [c saveCurrentFile:c];
    });
    MCGuiWait(@"Cmd+S: asked, then saved as typed", 15, ^BOOL {
        return MCGuiAlertsShown().count == alerts + 4 &&
               [Disk(file) isEqualToString:@"x=1\ny=2\n"];
    });
    MCGuiStep(@"nothing from the folder ran; put everything back", ^{
        GCHECK(!ran(ranLocal));
        GCHECK_EQ_STR(answer(), @"deny");
        if (consentBefore)
            [MCAppDefaults() setObject:consentBefore forKey:@"MCPythonEnvironmentConsent"];
        else
            forget();
        [fm removeItemAtPath:dir error:nil];
        MCGuiSetSettings(@"lsp.enabled = false\n");
    });
}
