// Utf16Suite.mm — `make guitest SUITES=utf16`: UTF-16 files with a byte
// order mark open as text (round 6's D10; until then a Find in Folder
// result in one opened Quick Look, since its NULs read as binary). A
// UTF-16LE and a UTF-16BE file are opened from Find in Folder's results
// at their lines, edited and saved, and their bytes compared: the same
// encoding and mark, CRLF kept. The change bars of a committed UTF-16
// file read its index blob as UTF-16 (no bars when unchanged, one for one
// edited line), Stage Change writes UTF-16 to the index and Revert Change
// puts the line back; a clean UTF-16 tab follows its file changed outside;
// Replace in Folder edits clean UTF-16 tabs in place rather than calling
// them unsaved; and UTF-16 without a mark, or with a lone surrogate (in the
// head, or past it), still opens in Quick Look. The backup restore of
// UTF-16 tabs is in BackupsSuite.mm.
#import "Harness.h"
#import "EditorController.h"
#import "EditorTab.h"
#import "FileViewers.h"
#import "GitGutter.h"
#import "GitPanel.h"
#import "Search.h"

namespace {
NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
SearchPanel *Panel(EditorController *c) { return [c valueForKey:@"searchPanel"]; }
NSArray *Hits(EditorController *c) { return [Panel(c) valueForKey:@"hits"]; }
MCGitGutter *Bars(EditorController *c) { return [c valueForKey:@"gitGutter"]; }
// Text as UTF-16 with its byte order mark first.
NSData *Utf16(NSString *s, BOOL le, BOOL mark = YES) {
    NSMutableData *d = [NSMutableData data];
    if (mark) [d appendBytes:le ? "\xFF\xFE" : "\xFE\xFF" length:2];
    [d appendData:[s dataUsingEncoding:le ? NSUTF16LittleEndianStringEncoding
                                          : NSUTF16BigEndianStringEncoding]];
    return d;
}
NSUInteger CaretLine(EditorController *c) {
    NSTextView *tv = Text(c);
    NSString *before = [tv.string substringToIndex:MIN(tv.selectedRange.location, tv.string.length)];
    return [before componentsSeparatedByString:@"\n"].count - 1;
}
void TypeAt(EditorController *c, NSUInteger at, NSString *s) {
    NSTextView *tv = Text(c);
    MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:s replacementRange:NSMakeRange(at, 0)]; });
}
// Opens a Find in Folder result as a double-click does.
BOOL OpenHit(EditorController *c, NSString *path) {
    NSOutlineView *ov = (NSOutlineView *)Panel(c).table;
    for (NSInteger r = 0; r < ov.numberOfRows; r++) {
        id item = [ov itemAtRow:r];
        if (![NSStringFromClass([item class]) isEqualToString:@"SearchHit"]) continue;
        if (![[item valueForKey:@"path"] isEqualToString:path]) continue;
        [ov selectRowIndexes:[NSIndexSet indexSetWithIndex:r] byExtendingSelection:NO];
        [NSApp sendAction:@selector(openSelected:) to:Panel(c) from:nil];
        return YES;
    }
    return NO;
}
NSInteger Encoding(EditorController *c) {
    return [[c valueForKeyPath:@"activeTab.encoding"] integerValue];
}
void Shell(NSString *what, NSString *line) {
    MCGuiAsync(what, 60, ^(MCGuiDone done) {
        MCGuiRunProcess(@[@"/bin/sh", @"-c", line], nil, ^(int s, NSString *out, NSString *err) {
            GCHECK(s == 0);
            if (s) MCGuiNote(@"%@: %@%@", what, out, err);
            done();
        });
    });
}
}  // namespace

MC_GUI_SUITE(utf16) {
    __block EditorController *c = nil;
    __block NSUInteger diffs = 0;
    __block NSString *stageFailure = nil;
    NSString *git = MCGitExecutable();
    NSString *G = git ? [NSString stringWithFormat:@"'%@'", git] : nil;
    NSString *dir = [MCGuiScratch() stringByAppendingPathComponent:@"utf16"];
    NSString *le = [dir stringByAppendingPathComponent:@"le.ps1"];
    NSString *be = [dir stringByAppendingPathComponent:@"be.txt"];
    NSString *leText = @"first\r\nthe needle here\r\nlast café \U0001F600\r\n";
    NSString *beText = @"alpha\nbeta\ngamma needle\n";

    MCGuiStep(@"a folder: le.ps1 in UTF-16LE (CRLF), be.txt in UTF-16BE", ^{
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                                   attributes:nil error:nil];
        GCHECK([Utf16(leText, YES) writeToFile:le atomically:NO]);
        GCHECK([Utf16(beText, NO) writeToFile:be atomically:NO]);
    });
    if (G)
        Shell(@"committed", [NSString stringWithFormat:
            @"set -e; cd '%@'; %@ init -q -b main; %@ add -A; %@ commit -qm first", dir, G, G, G]);
    MCGuiStep(@"a window on it; Find in Folder for needle", ^{
        MCGuiSetSettings(@"lsp.enabled = false\n");
        NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:dir]]];
        GCHECK(MCGuiControllers().count == before + 1);
        c = MCGuiController();
        [c openSearch:nil];
        SearchPanel *p = Panel(c);
        [p setScope:dir];
        p.field.stringValue = @"needle";
        [p runSearch:nil];
    });
    MCGuiWait(@"two results", 10, ^BOOL { return !Panel(c).busy && Hits(c).count == 2; });

    // ---- UTF-16LE from its result, at its line
    MCGuiStep(@"le.ps1's result opens it as text at line 2", ^{
        GCHECK(OpenHit(c, le));
        GCHECK_EQ_STR([c valueForKey:@"currentPath"], le);
        GCHECK([[c valueForKey:@"viewerKind"] integerValue] == MCViewerNone);
        GCHECK(Text(c).editable);
        GCHECK_EQ_STR(Text(c).string, leText);
        GCHECK(Encoding(c) == MCTextEncodingUtf16LE);
        GCHECK([[c valueForKeyPath:@"activeTab.bom"] boolValue]);
        MCGuiNote(@"caret on line %lu", (unsigned long)CaretLine(c));
        GCHECK(CaretLine(c) == 1);
    });
    MCGuiStep(@"edited and saved: UTF-16LE with its mark, CRLF kept", ^{
        TypeAt(c, [Text(c).string rangeOfString:@"the needle"].location, @"X");
        GCHECK([[c valueForKey:@"dirty"] boolValue]);
        [c saveCurrentFile:nil];
        GCHECK(![[c valueForKey:@"dirty"] boolValue]);
        NSData *want = Utf16(@"first\r\nXthe needle here\r\nlast café \U0001F600\r\n", YES);
        NSData *got = [NSData dataWithContentsOfFile:le];
        if (![got isEqualToData:want]) MCGuiNote(@"le.ps1: %@", got);
        GCHECK([got isEqualToData:want]);
    });

    // ---- UTF-16BE likewise
    MCGuiStep(@"be.txt's result opens it at line 3; edited and saved as UTF-16BE", ^{
        GCHECK(OpenHit(c, be));
        GCHECK_EQ_STR([c valueForKey:@"currentPath"], be);
        GCHECK_EQ_STR(Text(c).string, beText);
        GCHECK(Encoding(c) == MCTextEncodingUtf16BE);
        GCHECK(CaretLine(c) == 2);
        TypeAt(c, 0, @"Å");
        [c saveCurrentFile:nil];
        NSData *got = [NSData dataWithContentsOfFile:be];
        if (![got isEqualToData:Utf16(@"Åalpha\nbeta\ngamma needle\n", NO)])
            MCGuiNote(@"be.txt: %@", got);
        GCHECK([got isEqualToData:Utf16(@"Åalpha\nbeta\ngamma needle\n", NO)]);
    });

    // ---- the change bars against a UTF-16 index blob
    if (G) {
        MCGuiStep(@"le.ps1 again: line 1 differs from the index", ^{
            diffs = Bars(c).diffs;
            GCHECK([c showFile:le]);
        });
        MCGuiWait(@"one bar, line 1 only (not every line)", 15, ^BOOL {
            return Bars(c).diffs > diffs &&
                   [Bars(c).barDescriptions isEqualToArray:@[@"modified 1+1 (was 1+1)"]];
        });
        MCGuiStep(@"Stage Change on line 1", ^{
            MCGuiNote(@"bars: %@", Bars(c).barDescriptions);
            NSTextView *tv = Text(c);
            [tv setSelectedRange:NSMakeRange([tv.string rangeOfString:@"Xthe"].location, 0)];
            stageFailure = @"(not yet)";
            Bars(c).onStaged = ^(NSString *failure) { stageFailure = failure; };
            diffs = Bars(c).diffs;
            [c stageChange:nil];
        });
        MCGuiWait(@"staged, and no bars", 15, ^BOOL {
            return stageFailure == nil && Bars(c).diffs > diffs && Bars(c).barCount == 0;
        });
        MCGuiStep(@"the index holds the file's bytes: UTF-16LE with its mark", ^{
            MCGitResult *r = MCGitRunSync(dir, @[@"show", @":le.ps1"]);
            NSData *want = [NSData dataWithContentsOfFile:le];
            if (![r.output isEqualToData:want]) MCGuiNote(@"the index: %@", r.output);
            GCHECK([r.output isEqualToData:want]);
            // A line edited, for Revert Change.
            TypeAt(c, [Text(c).string rangeOfString:@"last"].location, @"Y");
            diffs = Bars(c).diffs;
        });
        MCGuiWait(@"line 2's bar", 15, ^BOOL {
            return Bars(c).diffs > diffs &&
                   [Bars(c).barDescriptions isEqualToArray:@[@"modified 2+1 (was 2+1)"]];
        });
        MCGuiStep(@"Revert Change: the line back as it was, no U+FEFF", ^{
            NSTextView *tv = Text(c);
            [tv setSelectedRange:NSMakeRange([tv.string rangeOfString:@"Ylast"].location, 0)];
            [c revertChange:nil];
            GCHECK_EQ_STR(tv.string, @"first\r\nXthe needle here\r\nlast café \U0001F600\r\n");
            GCHECK(![[c valueForKey:@"dirty"] boolValue]);
        });
    }

    // ---- a clean UTF-16 tab follows its file
    MCGuiStep(@"le.ps1 shown, clean; rewritten outside", ^{
        GCHECK([c showFile:le]);
        GCHECK(![[c valueForKey:@"dirty"] boolValue]);
        GCHECK([Utf16(@"changed outside, needle\r\n", YES) writeToFile:le atomically:YES]);
    });
    MCGuiWait(@"the tab has the new text", 15, ^BOOL {
        return [Text(c).string isEqualToString:@"changed outside, needle\r\n"];
    });
    MCGuiStep(@"still clean and UTF-16LE", ^{
        GCHECK(![[c valueForKey:@"dirty"] boolValue]);
        GCHECK(Encoding(c) == MCTextEncodingUtf16LE);
    });

    // ---- Replace in Folder edits the clean tab in place
    MCGuiStep(@"Replace All needle with pin", ^{
        [c openReplaceInFolder:nil];
        SearchPanel *p = Panel(c);
        [p setScope:dir];
        p.field.stringValue = @"needle";
        p.replaceField.stringValue = @"pin";
        [p runSearch:nil];
    });
    MCGuiWait(@"the results", 10, ^BOOL { return !Panel(c).busy && Hits(c).count == 2; });
    MCGuiStep(@"replaced", ^{
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        [Panel(c) replaceAll:nil];
    });
    MCGuiWait(@"done", 10, ^BOOL {
        return !Panel(c).busy && [Panel(c).status.stringValue hasPrefix:@"Replaced"];
    });
    MCGuiStep(@"both changed in their clean tabs, unsaved, then saved in their encodings", ^{
        MCGuiNote(@"status: %@", Panel(c).status.stringValue);
        // Before, a clean UTF-16 tab read as holding unsaved changes (the
        // disk's text was read as UTF-8) and was left alone.
        GCHECK([Panel(c).status.stringValue containsString:@"(2 open in tabs, unsaved)"]);
        GCHECK([Panel(c).status.stringValue rangeOfString:@"Left alone"].location == NSNotFound);
        [Panel(c).window close];
        GCHECK([c showFile:le]);
        GCHECK_EQ_STR(Text(c).string, @"changed outside, pin\r\n");
        GCHECK([[c valueForKey:@"dirty"] boolValue]);
        GCHECK([[NSData dataWithContentsOfFile:le]
            isEqualToData:Utf16(@"changed outside, needle\r\n", YES)]);
        [c saveCurrentFile:nil];
        GCHECK([[NSData dataWithContentsOfFile:le] isEqualToData:Utf16(@"changed outside, pin\r\n", YES)]);
        GCHECK([c showFile:be]);
        GCHECK_EQ_STR(Text(c).string, @"\u00C5alpha\nbeta\ngamma pin\n");
        [c saveCurrentFile:nil];
        GCHECK([[NSData dataWithContentsOfFile:be]
            isEqualToData:Utf16(@"\u00C5alpha\nbeta\ngamma pin\n", NO)]);
    });

    // ---- what stays Quick Look
    NSString *nomark = [dir stringByAppendingPathComponent:@"nomark.txt"];
    NSString *lone = [dir stringByAppendingPathComponent:@"lone.txt"];
    NSString *late = [dir stringByAppendingPathComponent:@"late.txt"];
    MCGuiStep(@"no mark, a lone surrogate, one past the first 8 KB: Quick Look", ^{
        GCHECK([Utf16(@"no mark here\n", YES, NO) writeToFile:nomark atomically:NO]);
        NSMutableData *l = [Utf16(@"a ", YES) mutableCopy];
        [l appendBytes:"\x00\xD8" length:2];   // U+D800 alone
        [l appendData:Utf16(@" b\n", YES, NO)];
        GCHECK([l writeToFile:lone atomically:NO]);
        NSMutableData *far = [Utf16([@"" stringByPaddingToLength:6000 withString:@"x"
                                                     startingAtIndex:0], YES) mutableCopy];
        [far appendBytes:"\x00\xD8" length:2];
        GCHECK([far writeToFile:late atomically:NO]);
        for (NSString *p in @[nomark, lone, late]) {
            GCHECK([c showFile:p]);
            const NSInteger kind = [[c valueForKey:@"viewerKind"] integerValue];
            MCGuiCheck(kind == MCViewerQuickLook,
                       [NSString stringWithFormat:@"%@ in Quick Look (viewer %ld)",
                                                  p.lastPathComponent, (long)kind],
                       __FILE__, __LINE__);
            GCHECK(!Text(c).editable);
        }
    });

    MCGuiStep(@"the window closes", ^{
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);   // Don't Save
        [c.window performClose:nil];
    });
    MCGuiWait(@"gone", 10, ^BOOL { return ![MCGuiControllers() containsObject:c]; });
}
