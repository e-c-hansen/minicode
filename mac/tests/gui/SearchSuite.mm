// SearchSuite.mm — `make guitest SUITES=search`: Find in Folder's limits
// and what it says about them. A file over 1 MB is searched (read in
// pieces), and Replace in Folder says plainly that it leaves one alone; a
// file not in UTF-8 (Latin-1) is searched as Windows-1252, and Replace in
// Folder names it and leaves it alone (a binary is left out unnamed); the
// line under the results naming files left out takes its room under
// them; the results stop at 20,000 lines and the status says so; a
// pattern left to std::regex that runs past its time stops the search
// with a reason. Each hit shows its line number and the lines just above
// and below (one a neighbouring hit shows already, once), and opens at its
// own line with the keyboard in the text, though the tree had it. A hit
// in a Windows-1252 file opens it as text at its line, the status bar
// naming the encoding, and it is saved in it, byte for byte, or as UTF-8,
// said, once it holds what Windows-1252 cannot.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
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
// An edit as a key press makes it: one undo group of its own (SaveSuite).
void Type(EditorController *c, NSString *s, NSUInteger at) {
    NSTextView *tv = c.textView;
    NSUndoManager *u = tv.undoManager;
    if (u.groupsByEvent && u.groupingLevel > 0) [u endUndoGrouping];
    MCGuiUndoGroup(u, ^{ [tv insertText:s replacementRange:NSMakeRange(at, 0)]; });
}
NSUInteger CaretLine(EditorController *c) {
    NSString *text = c.textView.string;
    const NSUInteger at = MIN(c.textView.selectedRange.location, text.length);
    NSUInteger line = 1;
    for (NSUInteger i = 0; i < at; i++)
        if ([text characterAtIndex:i] == '\n') line++;
    return line;
}
NSData *Bytes(const char *s) { return [NSData dataWithBytes:s length:strlen(s)]; }
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

    // ---- each hit in its excerpt (C3, 2026-11-18: a bare line each): its
    // line number, the lines just above and below with theirs, muted, the
    // match highlighted, and a line two hits share shown once
    MCGuiStep(@"a file whose hits have lines around them", ^{
        MCGuiWriteFile([dir stringByAppendingPathComponent:@"excerpt.txt"],
                       @"alpha\nbravo\ncharlie needle\ndelta\necho\nfoxtrot needle\nneedle golf\nhotel\n");
        SearchPanel *p = Panel(c);
        p.includeField.stringValue = [scopeInclude.length ? [scopeInclude stringByAppendingString:@"/"] : @""
                                         stringByAppendingString:@"excerpt.txt"];
        [p runSearch:nil];
    });
    MCGuiWait(@"its three hits", 10, ^BOOL {
        return !Panel(c).busy && Hits(c).count == 3 &&
               [[Hits(c).firstObject valueForKey:@"path"] hasSuffix:@"/excerpt.txt"];
    });
    MCGuiStep(@"each hit's number and the lines around it; the match highlighted", ^{
        SearchPanel *p = Panel(c);
        NSArray *hits = Hits(c);
        NSArray *wantBefore = @[@[@"bravo"], @[@"echo"], @[]];
        NSArray *wantAfter = @[@[@"delta"], @[], @[@"hotel"]];
        for (NSUInteger i = 0; i < 3; i++) {
            MCGuiNote(@"hit %lu, line %@: before %@, after %@", (unsigned long)i,
                      [hits[i] valueForKey:@"line"], [hits[i] valueForKey:@"before"],
                      [hits[i] valueForKey:@"after"]);
            GCHECK([[hits[i] valueForKey:@"before"] isEqualToArray:wantBefore[i]]);
            GCHECK([[hits[i] valueForKey:@"after"] isEqualToArray:wantAfter[i]]);
        }
        // The tree: the file's row, then a row per hit as tall as its lines.
        NSTableView *t = p.table;
        GCHECK(t.numberOfRows == 4);
        if (t.numberOfRows != 4) return;
        GCHECK(NSHeight([t rectOfRow:0]) == 22);
        GCHECK(NSHeight([t rectOfRow:1]) == 58);   // above, the line, below
        GCHECK(NSHeight([t rectOfRow:2]) == 40);
        GCHECK(NSHeight([t rectOfRow:3]) == 40);
        NSView *cell = [t viewAtColumn:0 row:1 makeIfNecessary:YES];
        [cell layoutSubtreeIfNeeded];
        NSTextField *number = [cell valueForKey:@"number"];
        NSTextField *name = [cell valueForKey:@"name"];
        NSArray<NSTextField *> *numbers = [cell valueForKey:@"contextNumbers"];
        NSArray<NSTextField *> *texts = [cell valueForKey:@"contextTexts"];
        GCHECK_EQ_STR(number.stringValue, @"3");
        GCHECK_EQ_STR(name.stringValue, @"charlie needle");
        GCHECK(numbers.count == 2 && texts.count == 2);
        if (numbers.count != 2 || texts.count != 2) return;
        GCHECK(!texts[0].hidden && !texts[1].hidden);
        GCHECK_EQ_STR(numbers[0].stringValue, @"2");
        GCHECK_EQ_STR(texts[0].stringValue, @"bravo");
        GCHECK_EQ_STR(numbers[1].stringValue, @"4");
        GCHECK_EQ_STR(texts[1].stringValue, @"delta");
        // Top down: the line above, the line, the line below; the numbers
        // in a gutter left of the text.
        GCHECK(NSMinY(texts[0].frame) < NSMinY(name.frame) && NSMinY(name.frame) < NSMinY(texts[1].frame));
        GCHECK(NSMaxX(number.frame) < NSMinX(name.frame) && NSMinX(texts[0].frame) == NSMinX(name.frame));
        // The match highlighted in the line itself.
        NSAttributedString *a = name.attributedStringValue;
        const NSRange at = [a.string rangeOfString:@"needle"];
        GCHECK(at.location != NSNotFound &&
               [a attribute:NSBackgroundColorAttributeName atIndex:at.location effectiveRange:NULL] != nil);
        GCHECK([a attribute:NSBackgroundColorAttributeName atIndex:0 effectiveRange:NULL] == nil);
        // The last hit: nothing above (the hit before shows its line), the
        // line below shown.
        NSView *last = [t viewAtColumn:0 row:3 makeIfNecessary:YES];
        [last layoutSubtreeIfNeeded];
        NSArray<NSTextField *> *lastTexts = [last valueForKey:@"contextTexts"];
        GCHECK_EQ_STR([[last valueForKey:@"number"] stringValue], @"7");
        GCHECK(lastTexts.count == 2 && lastTexts[0].hidden && !lastTexts[1].hidden);
        if (lastTexts.count == 2) GCHECK_EQ_STR(lastTexts[1].stringValue, @"hotel");
        MCGuiSnapshot(p.window.contentView, @"search-excerpts");
        // Opened, a hit goes to its own line, not a line around it, and
        // its text has the keyboard though the tree had it before.
        [c.window makeFirstResponder:c.outline];
        [t selectRowIndexes:[NSIndexSet indexSetWithIndex:3] byExtendingSelection:NO];
        [NSApp sendAction:t.doubleAction to:t.target from:t];
    });
    MCGuiWait(@"the file open at the last hit's line", 10, ^BOOL {
        return [c.currentPath hasSuffix:@"/excerpt.txt"];
    });
    MCGuiStep(@"the caret on line 7", ^{
        NSString *text = c.textView.string;
        const NSUInteger at = MIN(c.textView.selectedRange.location, text.length);
        NSUInteger line = 1;
        for (NSUInteger i = 0; i < at; i++)
            if ([text characterAtIndex:i] == '\n') line++;
        MCGuiNote(@"caret at %lu, line %lu", (unsigned long)at, (unsigned long)line);
        GCHECK(line == 7);
        MCGuiNote(@"the editor window's first responder: %@", c.window.firstResponder.className);
        GCHECK(c.window.firstResponder == c.textView);
        [NSFileManager.defaultManager removeItemAtPath:[dir stringByAppendingPathComponent:@"excerpt.txt"]
                                                 error:nil];
        Panel(c).includeField.stringValue = scopeInclude ?: @"";
    });

    // ---- a Windows-1252 file opens as text at its hit's line, says its
    // encoding in the status bar, and is saved in it (C3b, 2026-11-18:
    // the hit opened Quick Look, with no line)
    static const char *const k1252 =
        "first line\ncaf\xE9 au lait\nthe \x93needle\x94 costs \x80" "5\nlast \x81 byte\n";
    MCGuiStep(@"a Windows-1252 file with a hit on line 3", ^{
        WriteBytes([dir stringByAppendingPathComponent:@"cp1252.txt"], Bytes(k1252));
        SearchPanel *p = Panel(c);
        p.includeField.stringValue = [scopeInclude.length ? [scopeInclude stringByAppendingString:@"/"] : @""
                                         stringByAppendingString:@"cp1252.txt"];
        [p runSearch:nil];
    });
    MCGuiWait(@"its hit", 10, ^BOOL {
        return !Panel(c).busy && Hits(c).count == 1 &&
               [[Hits(c).firstObject valueForKey:@"path"] hasSuffix:@"/cp1252.txt"];
    });
    MCGuiStep(@"the hit opened", ^{
        SearchPanel *p = Panel(c);
        GCHECK_EQ_STR([Hits(c).firstObject valueForKey:@"text"],
                      @"the \u201Cneedle\u201D costs \u20AC5");
        NSTableView *t = p.table;
        GCHECK(t.numberOfRows == 2);
        [t selectRowIndexes:[NSIndexSet indexSetWithIndex:1] byExtendingSelection:NO];
        [NSApp sendAction:t.doubleAction to:t.target from:t];
    });
    MCGuiWait(@"the Windows-1252 file open", 10, ^BOOL {
        return [c.currentPath hasSuffix:@"/cp1252.txt"];
    });
    MCGuiStep(@"as text, at line 3, the status bar saying Windows-1252", ^{
        NSTextField *enc = [c valueForKey:@"encodingLabel"];
        MCGuiNote(@"viewer %ld, editable %d, caret line %lu, encoding \"%@\" (hidden %d)",
                  (long)c.viewerKind, c.textView.editable, (unsigned long)CaretLine(c),
                  enc.stringValue, enc.hidden);
        GCHECK(c.viewerKind == MCViewerNone);
        GCHECK(c.textView.editable && !c.showingMessage);
        GCHECK_EQ_STR(c.textView.string,
                      @"first line\ncaf\u00E9 au lait\nthe \u201Cneedle\u201D costs \u20AC5\n"
                       "last \u0081 byte\n");
        GCHECK(CaretLine(c) == 3);
        GCHECK(!enc.hidden);
        GCHECK_EQ_STR(enc.stringValue, @"Windows-1252");
        GCHECK(!c.dirty);
        // Laid out in the bar, left of what is right of it.
        GCHECK(enc.superview == c.statusBar && NSWidth(enc.frame) > 20);
        // Saved with nothing changed: the same bytes.
        [c saveCurrentFile:nil];
        NSData *disk = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:@"cp1252.txt"]];
        GCHECK([disk isEqualToData:Bytes(k1252)]);
        // An edit Windows-1252 holds is written in it.
        [c focusEditor:nil];
        Type(c, @"\u00E0 la ", 0);
        GCHECK(c.dirty);
        [c saveCurrentFile:nil];
        GCHECK(!c.dirty);
        disk = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:@"cp1252.txt"]];
        NSMutableData *want = [Bytes("\xE0 la ") mutableCopy];
        [want appendData:Bytes(k1252)];
        MCGuiNote(@"on disk: %@", disk);
        GCHECK([disk isEqualToData:want]);
        GCHECK_EQ_STR(enc.stringValue, @"Windows-1252");
    });
    MCGuiStep(@"a character Windows-1252 cannot store: saved as UTF-8, and said", ^{
        NSTextField *enc = [c valueForKey:@"encodingLabel"];
        NSTextField *note = [c valueForKey:@"settingsLabel"];
        Type(c, @"\u2192 ", 0);
        [c saveCurrentFile:nil];
        GCHECK(!c.dirty);
        NSData *disk = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:@"cp1252.txt"]];
        NSString *text = [[NSString alloc] initWithData:disk encoding:NSUTF8StringEncoding];
        MCGuiNote(@"note: %@; encoding \"%@\" (hidden %d)", note.stringValue, enc.stringValue,
                  enc.hidden);
        GCHECK_EQ_STR(text, c.textView.string);
        GCHECK([text hasPrefix:@"\u2192 \u00E0 la first line\ncaf\u00E9"]);
        GCHECK_EQ_STR(note.stringValue,
                      @"Saved cp1252.txt as UTF-8: Windows-1252 cannot store \u201C\u2192\u201D");
        GCHECK(enc.hidden);   // plain UTF-8 now, which the bar leaves unnamed
        GCHECK([c closeTab:[c valueForKey:@"activeTab"] prompt:NO]);
        [NSFileManager.defaultManager removeItemAtPath:[dir stringByAppendingPathComponent:@"cp1252.txt"]
                                                 error:nil];
        Panel(c).includeField.stringValue = scopeInclude ?: @"";
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
        // a.txt, latin1.txt (searched since 2026-11-18) and many.txt, in
        // the walk's order (big/ comes after). Failed once on CI with "in 3
        // files" when it was two (PR #8, run 43610029465): the files, so
        // the next time says which one was the extra.
        if (!GCHECK([p.status.stringValue hasPrefix:@"20,000 matches in 3 files"])) {
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
