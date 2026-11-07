// SearchSuite.mm — `make guitest SUITES=search`: Find in Folder's limits
// and what it says about them. A file over 1 MB and one not in UTF-8 are
// named in a line under the results (a binary is not); the results stop at
// 20,000 lines and the status says so; a pattern left to std::regex that
// runs past its time stops the search with a reason.
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
    MCGuiWait(@"the results", 10, ^BOOL { return !Panel(c).busy && Hits(c).count >= 1; });
    MCGuiStep(@"one match; the 2 MB file and the Latin-1 one named under the results", ^{
        SearchPanel *p = Panel(c);
        MCGuiNote(@"status: %@; note: %@ (%@)", p.status.stringValue, p.skippedNote.stringValue,
                  p.skippedNote.toolTip);
        GCHECK(Hits(c).count == 1);
        GCHECK_EQ_STR(p.status.stringValue, @"1 match in 1 file");
        GCHECK(!p.skippedNote.hidden);
        GCHECK_EQ_STR(p.skippedNote.stringValue,
                      @"2 files skipped: 1 larger than 1 MB, 1 not UTF-8");
        GCHECK([p.skippedNote.toolTip containsString:@"big/log.txt (larger than the limit)"]);
        GCHECK([p.skippedNote.toolTip containsString:@"latin1.txt (not UTF-8)"]);
        GCHECK(![p.skippedNote.toolTip containsString:@"pic.bin"]);
        // Laid out under the results, which make room for it.
        NSView *content = p.window.contentView;
        [content layoutSubtreeIfNeeded];
        NSScrollView *scroll = p.table.enclosingScrollView;
        GCHECK(NSMinY(p.skippedNote.frame) >= NSMaxY(scroll.frame));
        GCHECK(NSMaxY(p.skippedNote.frame) <= NSHeight(content.bounds));
        MCGuiSnapshot(content, @"search-skipped");
        // A search that skips nothing hides it.
        p.includeField.stringValue = @"a.txt";
        [p runSearch:nil];
    });
    MCGuiWait(@"searched again", 10, ^BOOL { return !Panel(c).busy && Hits(c).count == 1; });
    MCGuiStep(@"no line under the results; the scroll view takes the room back", ^{
        SearchPanel *p = Panel(c);
        GCHECK(p.skippedNote.hidden);
        [p.window.contentView layoutSubtreeIfNeeded];
        GCHECK(NSMaxY(p.table.enclosingScrollView.frame) == NSHeight(p.window.contentView.bounds));
        p.includeField.stringValue = @"";
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
        GCHECK([p.status.stringValue hasPrefix:@"20,000 matches in 2 files"]);
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
