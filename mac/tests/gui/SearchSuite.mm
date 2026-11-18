// SearchSuite.mm — `make guitest SUITES=search`: Find in Folder's limits
// and what it says about them. A file over 1 MB is searched (read in
// pieces), and Replace in Folder says plainly that it leaves one alone; a
// file not in UTF-8 (Latin-1) is searched as Windows-1252, and Replace in
// Folder names it and leaves it alone (a binary is left out unnamed); the
// line under the results naming files left out takes its room under
// them; the results stop at 20,000 lines and the status says so; a
// pattern left to std::regex that runs past its time stops the search
// with a reason.
#import "Harness.h"
#import "EditorController.h"
#import "Search.h"
#include "TextFind.h"

namespace {
SearchPanel *Panel(EditorController *c) { return [c valueForKey:@"searchPanel"]; }
NSArray *Hits(EditorController *c) { return [Panel(c) valueForKey:@"hits"]; }
void WriteBytes(NSString *path, NSData *data) {
    [NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
    [data writeToFile:path atomically:YES];
}
}  // namespace

MC_GUI_SUITE(search) {
    __block EditorController *c = nil;
    __block NSString *scopeInclude = nil;   // what setScope: put in files to include
    __block NSUInteger alertsBefore = 0;
    NSString *dir = MCGuiProjectFile(@"searchsuite");

    MCGuiStep(@"a small file, a 2 MB one, one in Latin-1, a binary", ^{
        MCGuiWriteFile([dir stringByAppendingPathComponent:@"a.txt"], @"needle here\n");
        NSMutableData *big = [NSMutableData dataWithLength:2 * 1024 * 1024];
        memset(big.mutableBytes, 'x', big.length);
        const char tail[] = "\nneedle\n";
        [big appendBytes:tail length:sizeof tail - 1];
        WriteBytes([dir stringByAppendingPathComponent:@"big/log.txt"], big);
        const char latin[] = "caf\xE9 needle\n";
        WriteBytes([dir stringByAppendingPathComponent:@"latin1.txt"],
                   [NSData dataWithBytes:latin length:sizeof latin - 1]);
        const char bin[] = "\0\0\xFF needle";
        WriteBytes([dir stringByAppendingPathComponent:@"pic.bin"],
                   [NSData dataWithBytes:bin length:sizeof bin - 1]);
        MCGuiSetSettings(@"lsp.enabled = false\n");
        c = MCGuiController();
        [c openSearch:nil];
        SearchPanel *p = Panel(c);
        GCHECK(p != nil);
        [p setScope:dir];
        p.field.stringValue = @"needle";
        [p runSearch:nil];
    });
    MCGuiWait(@"the results", 10, ^BOOL { return !Panel(c).busy && Hits(c).count >= 3; });
    MCGuiStep(@"Shift+Cmd+G and Cmd+G step through the results, not Source Control", ^{
        // Find Previous's key in this window (Search.mm), Toggle Source
        // Control's menu key everywhere else (Commands.cpp).
        SearchPanel *p = Panel(c);
        NSMutableArray<NSString *> *shown = [NSMutableArray array];
        void (^was)(NSString *, NSInteger) = p.previewHandler;
        p.previewHandler = ^(NSString *path, NSInteger line) {
            (void)line;
            [shown addObject:path.lastPathComponent];
        };
        const BOOL git = [[c valueForKey:@"gitPanelVisible"] boolValue];
        const NSEventModifierFlags shiftCmd = NSEventModifierFlagCommand | NSEventModifierFlagShift;
        [p.window makeFirstResponder:p.field];
        GCHECK(MCGuiPerformMenuKey(p.window, 5 /* g */, shiftCmd, nil, nil));
        GCHECK(MCGuiPerformMenuKey(p.window, 5, shiftCmd, nil, nil));
        GCHECK(MCGuiPerformMenuKey(p.window, 5, NSEventModifierFlagCommand, nil, nil));
        MCGuiNote(@"shown: %@", shown);
        GCHECK(shown.count == 3);
        if (shown.count == 3) {
            GCHECK(![shown[0] isEqualToString:shown[1]]);   // the last result, then the one before
            GCHECK_EQ_STR(shown[2], shown[0]);             // and forward again
        }
        GCHECK(p.table.selectedRow >= 0);
        GCHECK([[c valueForKey:@"gitPanelVisible"] boolValue] == git);
        p.previewHandler = was;
    });
    MCGuiStep(@"the 2 MB file and the Latin-1 one searched; nothing named under the results", ^{
        SearchPanel *p = Panel(c);
        MCGuiNote(@"status: %@; note: %@ (%@)", p.status.stringValue, p.skippedNote.stringValue,
                  p.skippedNote.toolTip);
        GCHECK(Hits(c).count == 3);
        GCHECK_EQ_STR(p.status.stringValue, @"3 matches in 3 files");
        // Until 2026-11-16 a file over 1 MB was skipped ("1 larger than 1 MB"),
        // and until 2026-11-18 the Latin-1 file was named here, not searched.
        BOOL bigFound = NO, latinFound = NO;
        for (id h in Hits(c)) {
            NSString *path = [h valueForKey:@"path"];
            if ([path hasSuffix:@"/big/log.txt"]) bigFound = [[h valueForKey:@"line"] integerValue] == 2;
            if ([path hasSuffix:@"/latin1.txt"])
                latinFound = [[h valueForKey:@"text"] isEqualToString:@"caf\u00e9 needle"];
            GCHECK(![path hasSuffix:@"pic.bin"]);
        }
        GCHECK(bigFound);
        GCHECK(latinFound);
        GCHECK(p.skippedNote.hidden);
        // The line naming files left out (a line over 64 MB, too large to
        // write here) is laid out under the results, which make room for it.
        [p setSkippedNote:@"huge.log was not searched: it has a line longer than 64 MB"
                  tooltip:@"huge.log (a line longer than 64 MB)"];
        GCHECK(!p.skippedNote.hidden);
        NSView *content = p.window.contentView;
        [content layoutSubtreeIfNeeded];
        NSScrollView *scroll = p.table.enclosingScrollView;
        GCHECK(NSMinY(p.skippedNote.frame) >= NSMaxY(scroll.frame));
        GCHECK(NSMaxY(p.skippedNote.frame) <= NSHeight(content.bounds));
        MCGuiSnapshot(content, @"search-skipped");
        // A search that skips nothing hides it. What setScope: put in the
        // field is kept for the steps after: in a window whose folder holds
        // the suite's, the scope is "./searchsuite" there, and emptying the
        // field searched the whole project (another suite's ide/b.py has
        // the word too). It passed only while a window an earlier suite
        // left open, on another folder, happened to be the newest.
        scopeInclude = p.includeField.stringValue;
        p.includeField.stringValue = [scopeInclude.length ? [scopeInclude stringByAppendingString:@"/"] : @""
                                         stringByAppendingString:@"a.txt"];
        [p runSearch:nil];
    });
    MCGuiWait(@"searched again", 10, ^BOOL { return !Panel(c).busy && Hits(c).count == 1; });
    MCGuiStep(@"no line under the results; the scroll view takes the room back", ^{
        SearchPanel *p = Panel(c);
        GCHECK(p.skippedNote.hidden);
        [p.window.contentView layoutSubtreeIfNeeded];
        GCHECK(NSMaxY(p.table.enclosingScrollView.frame) == NSHeight(p.window.contentView.bounds));
        // Only the 2 MB file, for Replace in Folder.
        p.includeField.stringValue = [scopeInclude.length ? [scopeInclude stringByAppendingString:@"/"] : @""
                                         stringByAppendingString:@"big/log.txt"];
        [p runSearch:nil];
    });
    MCGuiWait(@"the large file's match", 10, ^BOOL {
        return !Panel(c).busy && Hits(c).count == 1 &&
               [[Hits(c).firstObject valueForKey:@"path"] hasSuffix:@"/big/log.txt"];
    });
    MCGuiStep(@"Replace on the large file's row", ^{
        SearchPanel *p = Panel(c);
        p.replaceField.stringValue = @"pin";
        [p replaceMatchAtRow:0];
    });
    MCGuiWait(@"it says it left the file alone", 10, ^BOOL {
        return !Panel(c).busy && [Panel(c).status.stringValue containsString:@"leaves alone"];
    });
    MCGuiStep(@"named, with the limit; Replace All says the same", ^{
        SearchPanel *p = Panel(c);
        MCGuiNote(@"status: %@", p.status.stringValue);
        GCHECK([p.status.stringValue
            containsString:@"log.txt is larger than 1 MB, which Replace in Folder leaves alone"]);
        alertsBefore = MCGuiAlertsShown().count;
        [p replaceAll:nil];
    });
    MCGuiWait(@"Replace All's answer", 10, ^BOOL {
        return !Panel(c).busy && [Panel(c).status.stringValue hasPrefix:@"Nothing replaced"];
    });
    MCGuiStep(@"no question asked; the file as it was", ^{
        SearchPanel *p = Panel(c);
        MCGuiNote(@"status: %@", p.status.stringValue);
        GCHECK([p.status.stringValue
            hasPrefix:@"Nothing replaced: the matches are in files larger than 1 MB, which "
                       "Replace in Folder leaves alone ("]);
        GCHECK([p.status.stringValue hasSuffix:@"big/log.txt)"]);
        GCHECK(MCGuiAlertsShown().count == alertsBefore);
        NSData *log = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:@"big/log.txt"]];
        GCHECK(log.length == 2 * 1024 * 1024 + 8);
        // Only the Latin-1 file: Replace in Folder writes UTF-8 or UTF-16
        // only, so it names the file and leaves it alone.
        p.includeField.stringValue = [scopeInclude.length ? [scopeInclude stringByAppendingString:@"/"] : @""
                                         stringByAppendingString:@"latin1.txt"];
        [p runSearch:nil];
    });
    MCGuiWait(@"the Latin-1 file's match", 10, ^BOOL {
        return !Panel(c).busy && Hits(c).count == 1 &&
               [[Hits(c).firstObject valueForKey:@"path"] hasSuffix:@"/latin1.txt"];
    });
    MCGuiStep(@"Replace on the Latin-1 file's row", ^{
        SearchPanel *p = Panel(c);
        p.replaceField.stringValue = @"pin";
        [p replaceMatchAtRow:0];
    });
    MCGuiWait(@"it says it left the Latin-1 file alone", 10, ^BOOL {
        return !Panel(c).busy && [Panel(c).status.stringValue containsString:@"leaves alone"];
    });
    MCGuiStep(@"named as not UTF-8; Replace All says the same", ^{
        SearchPanel *p = Panel(c);
        MCGuiNote(@"status: %@", p.status.stringValue);
        GCHECK([p.status.stringValue
            containsString:@"latin1.txt is not UTF-8 (read as Windows-1252), which Replace in "
                            "Folder leaves alone"]);
        alertsBefore = MCGuiAlertsShown().count;
        [p replaceAll:nil];
    });
    MCGuiWait(@"Replace All's answer on the Latin-1 file", 10, ^BOOL {
        return !Panel(c).busy && [Panel(c).status.stringValue hasPrefix:@"Nothing replaced"];
    });
    MCGuiStep(@"no question asked; the Latin-1 file as it was", ^{
        SearchPanel *p = Panel(c);
        MCGuiNote(@"status: %@", p.status.stringValue);
        GCHECK([p.status.stringValue
            hasPrefix:@"Nothing replaced: the matches are in files that are not UTF-8 (read as "
                       "Windows-1252), which Replace in Folder leaves alone ("]);
        GCHECK([p.status.stringValue hasSuffix:@"latin1.txt)"]);
        GCHECK(MCGuiAlertsShown().count == alertsBefore);
        NSData *latin = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:@"latin1.txt"]];
        GCHECK([latin isEqualToData:[NSData dataWithBytes:"caf\xE9 needle\n" length:12]]);
        p.replaceField.stringValue = @"";
        p.includeField.stringValue = scopeInclude ?: @"";
    });

    // ---- 20,000 lines at most
    MCGuiStep(@"a file of 20,050 matching lines", ^{
        NSMutableString *many = [NSMutableString string];
        for (int i = 0; i < 20050; i++) [many appendFormat:@"row %d needle\n", i];
        MCGuiWriteFile([dir stringByAppendingPathComponent:@"many.txt"], many);
        [Panel(c) runSearch:nil];
    });
    MCGuiWait(@"the results", 20, ^BOOL { return !Panel(c).busy && Hits(c).count >= 20000; });
    MCGuiStep(@"20,000 rows, and the status says it stopped there", ^{
        SearchPanel *p = Panel(c);
        MCGuiNote(@"%lu rows: %@", (unsigned long)Hits(c).count, p.status.stringValue);
        GCHECK(Hits(c).count == 20000);
        GCHECK([p.status.stringValue hasSuffix:@". Stopped at 20,000 results"]);
        // Failed once on CI with "in 3 files" (PR #8, run 43610029465): the
        // files, so the next time says which one was the third.
        if (!GCHECK([p.status.stringValue hasPrefix:@"20,000 matches in 2 files"])) {
            NSMutableOrderedSet *files = [NSMutableOrderedSet orderedSet];
            for (id h in Hits(c)) [files addObject:[h valueForKey:@"path"] ?: @"?"];
            MCGuiNote(@"the files: %@", [files.array componentsJoinedByString:@", "]);
        }
        // Showing them again: the tree of 20,000 lines is built on the main
        // thread, bounded in that thread's CPU time.
        NSArray *hits = [Hits(c) copy];
        struct timespec t0, t1;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
        [p performSelector:@selector(setResults:) withObject:hits];
#pragma clang diagnostic pop
        [p.table displayIfNeeded];
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t1);
        const double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        MCGuiNote(@"20,000 rows shown in %.0f ms of main-thread CPU", ms);
        GCHECK(ms < 1500);
        [NSFileManager.defaultManager removeItemAtPath:[dir stringByAppendingPathComponent:@"many.txt"]
                                                 error:nil];
    });

    // ---- a pattern std::regex runs that takes too long
    MCGuiStep(@"a backreference over a 20,000-letter line", ^{
        TextFind::setSlowPatternSeconds(0.05);
        NSString *as = [@"" stringByPaddingToLength:20000 withString:@"a" startingAtIndex:0];
        MCGuiWriteFile([dir stringByAppendingPathComponent:@"slow.txt"],
                       [as stringByAppendingString:@"\n"]);
        SearchPanel *p = Panel(c);
        p.regex = YES;   // searches
        p.field.stringValue = @"(a)(.*)\\1z";
        [p runSearch:nil];
    });
    MCGuiWait(@"the search stops", 10, ^BOOL {
        return !Panel(c).busy && [Panel(c).status.stringValue containsString:@"too long"];
    });
    MCGuiStep(@"the reason, in the error color", ^{
        SearchPanel *p = Panel(c);
        MCGuiNote(@"status: %@", p.status.stringValue);
        GCHECK_EQ_STR(p.status.stringValue, @"This pattern takes too long; the search stopped");
        GCHECK(p.status.tag == 1);
        TextFind::setSlowPatternSeconds(2.0);
        p.regex = NO;
        [p.window close];
    });
}
