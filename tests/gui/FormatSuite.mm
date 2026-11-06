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
