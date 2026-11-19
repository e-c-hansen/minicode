// ReplaceInFolderSuite.mm — `make guitest SUITES=replace_in_folder`:
// Replace in Folder (Option+Shift+Cmd+F). Replace All counts first and asks;
// a file open in a clean tab is changed in the tab, unsaved and undoable;
// a file whose tab has unsaved changes is left alone and named; the rest
// are written to disk; and replacing one result changes only its match.
// Then the window as VS Code's search view: the frames, the tree of files
// with their counts, files to include and exclude, Preserve Case and the
// replace preview, Dismiss, the TODO list in the same window, and pictures.
// Then the keys (1.6.4, after a walker's typing went into the replace field
// and Return rewrote a file): Return in the replace field only searches;
// Option+Cmd+Return asks, as the button does, and Cancel writes nothing;
// a dismissed row is left alone on disk; and Ctrl+` from the Search window
// gives the terminal the keyboard, the editor window made key, so typing
// reaches the shell and not the replace field.
#import "Harness.h"
#import "EditorController.h"
#import "Search.h"
#import "FindWidget.h"
#import "Terminal.h"
#import "TerminalGridView.h"

namespace {
NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
NSString *Disk(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}
SearchPanel *Panel(EditorController *c) { return [c valueForKey:@"searchPanel"]; }
NSArray *Hits(EditorController *c) { return [Panel(c) valueForKey:@"hits"]; }
// A plain key down for the window, handed to its first responder as the
// window does with a key that has no modifiers. Not [NSApp sendEvent:]: an
// offscreen window is never AppKit's key window, so it would not reach it.
// Not [w sendEvent:] either: once the window has been closed, as the
// search suite closes the Search window this one reuses when it runs
// first, it is never ordered in again offscreen and drops every key sent
// to it (the soak's "the search Return ran" in every shuffled order).
void Press(NSWindow *w, unsigned short code, NSString *chars) {
    NSEvent *e = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0
                                 timestamp:NSProcessInfo.processInfo.systemUptime
                              windowNumber:w.windowNumber context:nil characters:chars
               charactersIgnoringModifiers:chars isARepeat:NO keyCode:code];
    GCHECK(w.firstResponder != nil);
    [w.firstResponder keyDown:e];
}
}  // namespace

MC_GUI_SUITE(replace_in_folder) {
    __block EditorController *c = nil;
    NSString *dir = MCGuiProjectFile(@"rif");
    NSString *one = [dir stringByAppendingPathComponent:@"one.txt"];
    NSString *two = [dir stringByAppendingPathComponent:@"two.txt"];
    NSString *three = [dir stringByAppendingPathComponent:@"three.txt"];
    NSString *four = [dir stringByAppendingPathComponent:@"sub/four.txt"];

    MCGuiStep(@"four files; two.txt open and clean, three.txt open with unsaved changes", ^{
        MCGuiWriteFile(one, @"cat and cat\n");
        MCGuiWriteFile(two, @"a cat\n");
        MCGuiWriteFile(three, @"cat three\n");
        MCGuiWriteFile(four, @"no cats here? cat\n");
        MCGuiSetSettings(@"lsp.enabled = false\n");
        c = MCGuiController();
        [c focusEditor:nil];
        GCHECK([c showFile:three]);
        MCGuiUndoGroup(Text(c).undoManager, ^{
            Text(c).selectedRange = NSMakeRange(0, 0);
            [Text(c) insertText:@"edited " replacementRange:Text(c).selectedRange];
        });
        GCHECK([c showFile:two]);
        [c openReplaceInFolder:nil];
        SearchPanel *p = Panel(c);
        GCHECK(p != nil);
        GCHECK(p.window.firstResponder == p.replaceField.currentEditor);
        [p setScope:dir];
        p.field.stringValue = @"cat";
        p.replaceField.stringValue = @"dog";
        [p runSearch:nil];
    });
    MCGuiWait(@"the results", 10, ^BOOL { return !Panel(c).busy && Hits(c).count >= 4; });
    MCGuiStep(@"Replace All asks first", ^{
        MCGuiNote(@"%lu results: %@", (unsigned long)Hits(c).count, Panel(c).status.stringValue);
        // Every match, as Replace All counts them, though one.txt's two and
        // four.txt's two are a row each (it said "4 matches in 4 files").
        GCHECK(Hits(c).count == 4);
        GCHECK_EQ_STR(Panel(c).status.stringValue, @"6 matches in 4 files");
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        [Panel(c) replaceAll:nil];
    });
    MCGuiWait(@"replaced", 10, ^BOOL {
        return !Panel(c).busy && [Disk(one) isEqualToString:@"dog and dog\n"];
    });
    MCGuiStep(@"5 matches in 3 files; three.txt named and untouched", ^{
        NSString *asked = MCGuiAlertsShown().lastObject;
        MCGuiNote(@"asked: %@; status: %@", asked, Panel(c).status.stringValue);
        GCHECK_EQ_STR(asked, @"Replace 5 matches in 3 files?");
        GCHECK_EQ_STR(Disk(four), @"no dogs here? dog\n");
        // two.txt in its tab, unsaved; the disk as it was.
        GCHECK_EQ_STR(Text(c).string, @"a dog\n");
        GCHECK([[c valueForKey:@"dirty"] boolValue]);
        GCHECK_EQ_STR(Disk(two), @"a cat\n");
        // three.txt: neither its tab nor its file.
        GCHECK_EQ_STR(Disk(three), @"cat three\n");
        GCHECK([Panel(c).status.stringValue containsString:@"three.txt"]);
        // The tab's change undoes there.
        [Text(c).undoManager undo];
        GCHECK_EQ_STR(Text(c).string, @"a cat\n");
        GCHECK([c showFile:three]);
        GCHECK_EQ_STR(Text(c).string, @"edited cat three\n");
    });
    MCGuiWait(@"the search ran again", 10, ^BOOL { return !Panel(c).busy; });
    MCGuiStep(@"one match of one.txt", ^{
        MCGuiWriteFile(one, @"cat cat\ncat\n");
        [Panel(c) runSearch:nil];
    });
    MCGuiWait(@"its results", 10, ^BOOL {
        for (id h in Hits(c))
            if ([[h valueForKey:@"path"] hasSuffix:@"/one.txt"] && [[h valueForKey:@"line"] integerValue] == 2)
                return !Panel(c).busy;
        return NO;
    });
    MCGuiStep(@"replace only line 2's", ^{
        NSArray *hits = Hits(c);
        NSInteger row = -1;
        for (NSUInteger i = 0; i < hits.count; i++)
            if ([[hits[i] valueForKey:@"path"] hasSuffix:@"/one.txt"] &&
                [[hits[i] valueForKey:@"line"] integerValue] == 2)
                row = (NSInteger)i;
        GCHECK(row >= 0);
        [Panel(c) replaceMatchAtRow:row];
    });
    MCGuiWait(@"that one replaced", 10, ^BOOL {
        return !Panel(c).busy && [Disk(one) isEqualToString:@"cat cat\ndog\n"];
    });
    // ---- VS Code's search view: checked by frames, the tree, and pictures.
    // rif is inside the window's folder when this suite runs alone; after
    // other suites the window may show another folder, and rif is then the
    // folder searched.
    __block NSString *rif = @"./rif";
    MCGuiStep(@"the scope is ./rif in files to include; the layout", ^{
        SearchPanel *p = Panel(c);
        if (!p.includeField.stringValue.length) {
            MCGuiNote(@"rif is outside the window's folder: searched itself");
            rif = @".";
            [p setValue:@YES forKey:@"detailsShown"];
        } else {
            GCHECK_EQ_STR(p.includeField.stringValue, @"./rif");
        }
        GCHECK(p.detailsShown && p.replaceShown);
        NSView *content = p.window.contentView;
        [content layoutSubtreeIfNeeded];
        MCFindInput *query = (MCFindInput *)p.field.superview;
        MCFindInput *replace = (MCFindInput *)p.replaceField.superview;
        GCHECK([query isKindOfClass:[MCFindInput class]] && [replace isKindOfClass:[MCFindInput class]]);
        // The query from 20 (2 + 18), 26 high, 6 from the top; its three
        // toggles inside its right end.
        GCHECK(NSEqualRects(query.frame, NSMakeRect(20, 6, NSWidth(content.bounds) - 32, 26)));
        GCHECK(query.toggles.count == 3 && NSMaxX(query.toggles.lastObject.frame) == NSWidth(query.bounds) - 2);
        // The replace row 6 below, narrower by Replace All beside it, which
        // sits 4 after it; Preserve Case inside.
        GCHECK(NSMinY(replace.frame) == 38 && NSMinX(replace.frame) == 20);
        GCHECK(replace.toggles.count == 1 && replace.toggles[0].icon == MCFindIconPreserveCase);
        MCFindButton *chevron = nil, *replaceAll = nil;
        for (NSView *v in content.subviews)
            if ([v isKindOfClass:[MCFindButton class]]) {
                MCFindButton *b = (MCFindButton *)v;
                if (b.icon == MCFindIconChevronDown) chevron = b;
                if (b.icon == MCFindIconReplaceAll) replaceAll = b;
            }
        GCHECK(chevron && NSMinX(chevron.frame) == 2 && NSWidth(chevron.frame) == 16 &&
               NSMinY(chevron.frame) == 6 && NSMaxY(chevron.frame) == NSMaxY(replace.frame));
        GCHECK(replaceAll && NSMinX(replaceAll.frame) == NSMaxX(replace.frame) + 4);
        GCHECK([replaceAll.toolTip containsString:@"Replace All"]);
        // The include box under the replace row.
        GCHECK(NSMinY(p.includeField.superview.frame) > NSMaxY(replace.frame));
    });
    MCGuiStep(@"the tree: a row per file with its count, its lines under it", ^{
        NSOutlineView *ov = (NSOutlineView *)Panel(c).table;
        GCHECK([ov isKindOfClass:[NSOutlineView class]]);
        NSInteger files = 0, lines = 0;
        for (NSInteger r = 0; r < ov.numberOfRows; r++) {
            id item = [ov itemAtRow:r];
            if ([ov levelForRow:r] == 0) {
                files++;
                GCHECK([ov isItemExpanded:item]);
            } else {
                lines++;
            }
        }
        MCGuiNote(@"%ld files, %ld lines: %@", (long)files, (long)lines, Panel(c).status.stringValue);
        NSMutableSet *paths = [NSMutableSet set];
        NSUInteger firstCount = 0;
        NSString *firstPath = [Hits(c).firstObject valueForKey:@"path"];
        for (id h in Hits(c)) {
            [paths addObject:[h valueForKey:@"path"]];
            if ([[h valueForKey:@"path"] isEqualToString:firstPath])
                firstCount += [[h valueForKey:@"count"] unsignedIntegerValue];
        }
        GCHECK(files == (NSInteger)paths.count && lines == (NSInteger)Hits(c).count && files >= 2);
        // The first file's row: its count is its matches, not its lines.
        NSView *cell = [ov viewAtColumn:0 row:0 makeIfNecessary:YES];
        id badge = [cell valueForKey:@"badge"];
        GCHECK_EQ_STR([badge valueForKey:@"text"], ([NSString stringWithFormat:@"%lu", (unsigned long)firstCount]));
        MCGuiSnapshot(Panel(c).window.contentView, @"search-dark");
    });
    MCGuiStep(@"files to exclude: *.txt leaves nothing", ^{
        Panel(c).excludeField.stringValue = @"*.txt";
        [Panel(c) runSearch:nil];
    });
    MCGuiWait(@"no results", 10, ^BOOL { return !Panel(c).busy; });
    MCGuiStep(@"nothing, and the include box decides alone again", ^{
        GCHECK(Hits(c).count == 0);
        MCGuiNote(@"%@", Panel(c).status.stringValue);
        Panel(c).excludeField.stringValue = @"";
        Panel(c).includeField.stringValue = @"three.txt";
        [Panel(c) runSearch:nil];
    });
    MCGuiWait(@"three.txt only", 10, ^BOOL { return !Panel(c).busy && Hits(c).count == 1; });
    MCGuiStep(@"Preserve Case in Replace in Folder: the preview, then Replace All", ^{
        GCHECK([[Hits(c).firstObject valueForKey:@"path"] hasSuffix:@"/three.txt"]);
        Panel(c).includeField.stringValue = [rif stringByAppendingString:@"/sub"];
        MCGuiWriteFile(four, @"Cat CAT cat\n");
        Panel(c).preserveCase = YES;
        Panel(c).replaceField.stringValue = @"dog";
        [Panel(c) runSearch:nil];
    });
    MCGuiWait(@"four.txt", 10, ^BOOL { return !Panel(c).busy && Hits(c).count == 1; });
    MCGuiStep(@"the preview strikes the match through and shows its replacement", ^{
        NSOutlineView *ov = (NSOutlineView *)Panel(c).table;
        NSTableCellView *cell = [ov viewAtColumn:0 row:1 makeIfNecessary:YES];
        NSAttributedString *a = cell.textField.attributedStringValue;
        MCGuiNote(@"preview: %@", a.string);
        GCHECK_EQ_STR(a.string, @"CatDog CATDOG catdog");
        GCHECK([a attribute:NSStrikethroughStyleAttributeName atIndex:0 effectiveRange:nil] != nil);
        MCGuiSnapshot(Panel(c).window.contentView, @"search-replace-dark");
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        [Panel(c) replaceAll:nil];
    });
    MCGuiWait(@"replaced", 10, ^BOOL {
        return !Panel(c).busy && [Disk(four) isEqualToString:@"Dog DOG dog\n"];
    });
    MCGuiStep(@"Dismiss takes a file out of the results", ^{
        Panel(c).preserveCase = NO;
        Panel(c).includeField.stringValue = rif;
        Panel(c).replaceField.stringValue = @"";
        Panel(c).field.stringValue = @"dog";
        [Panel(c) runSearch:nil];
    });
    MCGuiWait(@"the dogs", 10, ^BOOL { return !Panel(c).busy && Hits(c).count >= 2; });
    MCGuiStep(@"dismissed", ^{
        NSOutlineView *ov = (NSOutlineView *)Panel(c).table;
        const NSUInteger before = Hits(c).count;
        id first = [ov itemAtRow:0];
        NSUInteger itsLines = [[first valueForKey:@"hits"] count];
        [ov selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
        [NSApp sendAction:@selector(dismissSelected:) to:Panel(c) from:nil];
        GCHECK(Hits(c).count == before - itsLines);
    });
    MCGuiStep(@"the light theme", ^{
        MCGuiSetSettings(@"theme = light-modern\nlsp.enabled = false\n");
        Panel(c).replaceField.stringValue = @"cat";
        [Panel(c) runSearch:nil];
    });
    MCGuiWait(@"searched", 10, ^BOOL { return !Panel(c).busy; });
    MCGuiStep(@"its picture", ^{
        MCGuiSnapshot(Panel(c).window.contentView, @"search-light");
        MCGuiSetSettings(@"lsp.enabled = false\n");
    });
    // ---- the TODO list shares the window and its tree
    MCGuiStep(@"TODOs in the folder", ^{
        MCGuiWriteFile([dir stringByAppendingPathComponent:@"todo.py"], @"# TODO: first\nx = 1  # FIXME: second\n");
        [c showTodos:nil];
        [(SearchPanel *)[c valueForKey:@"todoPanel"] setScope:dir];
    });
    MCGuiWait(@"the scan", 10, ^BOOL {
        SearchPanel *t = [c valueForKey:@"todoPanel"];
        return t && !t.busy && [[t valueForKey:@"hits"] count] >= 2;
    });
    MCGuiStep(@"a file row over its two TODOs; no replace row, no toggles", ^{
        SearchPanel *t = [c valueForKey:@"todoPanel"];
        GCHECK(t.replaceField == nil && !t.replaceShown);
        NSOutlineView *ov = (NSOutlineView *)t.table;
        NSInteger row = -1;
        for (NSInteger r = 0; r < ov.numberOfRows; r++)
            if ([[[ov itemAtRow:r] valueForKey:@"path"] hasSuffix:@"/todo.py"] && [ov levelForRow:r] == 0)
                row = r;
        GCHECK(row >= 0 && [[[ov itemAtRow:row] valueForKey:@"hits"] count] == 2);
        GCHECK(((MCFindInput *)t.field.superview).toggles.count == 0);
        MCGuiSnapshot(t.window.contentView, @"todos-dark");
        [t.window close];
    });
    // ---- UTF-16 with a byte order mark (round 5's workspace 3): found,
    // and replaced in its own encoding.
    NSString *wide = [MCGuiScratch() stringByAppendingPathComponent:@"rif16"];
    NSString *le = [wide stringByAppendingPathComponent:@"le.ps1"];
    MCGuiStep(@"a UTF-16LE file, searched", ^{
        NSMutableData *d = [NSMutableData dataWithBytes:"\xFF\xFE" length:2];
        [d appendData:[@"$a = \"cat\"\r\nWrite-Host cat\r\n"
                          dataUsingEncoding:NSUTF16LittleEndianStringEncoding]];
        [[NSFileManager defaultManager] createDirectoryAtPath:wide withIntermediateDirectories:YES
                                                   attributes:nil error:nil];
        GCHECK([d writeToFile:le atomically:NO]);
        SearchPanel *p = Panel(c);
        [p setScope:wide];
        p.field.stringValue = @"cat";
        p.replaceField.stringValue = @"dog";
        [p runSearch:nil];
    });
    MCGuiWait(@"its two lines", 10, ^BOOL { return !Panel(c).busy && Hits(c).count == 2; });
    MCGuiStep(@"found, named nowhere as skipped; Replace All", ^{
        MCGuiNote(@"status: %@; rows: %@", Panel(c).status.stringValue,
                  [Hits(c) valueForKey:@"text"]);
        GCHECK(Panel(c).skippedNote.hidden);
        GCHECK_EQ_STR([Hits(c)[1] valueForKey:@"text"], @"Write-Host cat");
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        [Panel(c) replaceAll:nil];
    });
    MCGuiWait(@"replaced", 10, ^BOOL {
        NSData *d = [NSData dataWithContentsOfFile:le];
        return !Panel(c).busy && d.length > 2 &&
               [[[NSString alloc] initWithData:[d subdataWithRange:NSMakeRange(2, d.length - 2)]
                                      encoding:NSUTF16LittleEndianStringEncoding]
                   containsString:@"dog"];
    });
    MCGuiStep(@"still UTF-16LE with its mark, every match replaced", ^{
        NSMutableData *want = [NSMutableData dataWithBytes:"\xFF\xFE" length:2];
        [want appendData:[@"$a = \"dog\"\r\nWrite-Host dog\r\n"
                             dataUsingEncoding:NSUTF16LittleEndianStringEncoding]];
        GCHECK([[NSData dataWithContentsOfFile:le] isEqualToData:want]);
    });

    // ---- the keys: Return, Option+Cmd+Return, a dismissed row, Ctrl+`
    NSString *keys = [MCGuiScratch() stringByAppendingPathComponent:@"rifkeys"];
    NSString *k1 = [keys stringByAppendingPathComponent:@"k1.txt"];
    NSString *k2 = [keys stringByAppendingPathComponent:@"k2.txt"];
    __block NSUInteger asked = 0;
    MCGuiStep(@"two files, three cats, the replace field holding dog", ^{
        MCGuiWriteFile(k1, @"cat a\ncat b\n");
        MCGuiWriteFile(k2, @"cat c\n");
        SearchPanel *p = Panel(c);
        [p showReplace];
        [p setScope:keys];
        p.field.stringValue = @"cat";
        p.replaceField.stringValue = @"dog";
        [p runSearch:nil];
    });
    MCGuiWait(@"three rows", 10, ^BOOL { return !Panel(c).busy && Hits(c).count == 3; });
    MCGuiStep(@"Return in the replace field: no question, nothing written", ^{
        SearchPanel *p = Panel(c);
        MCGuiSetKeyWindow(p.window);
        [p.window makeFirstResponder:p.replaceField];
        GCHECK(p.window.firstResponder == p.replaceField.currentEditor);
        asked = MCGuiAlertsShown().count;
        // The query changed but not yet searched: Return's search shows it.
        p.field.stringValue = @"cat a";
        // No answer queued: were Return still Replace All, the unanswered
        // question would take Replace and the files would change.
        Press(p.window, 36 /* Return */, @"\r");
    });
    MCGuiWait(@"the search Return ran", 10, ^BOOL { return !Panel(c).busy && Hits(c).count == 1; });
    MCGuiStep(@"the files as they were", ^{
        MCGuiNote(@"alerts: %@; status: %@", MCGuiAlertsShown(), Panel(c).status.stringValue);
        GCHECK(MCGuiAlertsShown().count == asked);
        GCHECK_EQ_STR(Disk(k1), @"cat a\ncat b\n");
        GCHECK_EQ_STR(Disk(k2), @"cat c\n");
        GCHECK_EQ_STR(Panel(c).replaceField.stringValue, @"dog");
        Panel(c).field.stringValue = @"cat";
        [Panel(c) runSearch:nil];
    });
    MCGuiWait(@"three rows again", 10, ^BOOL { return !Panel(c).busy && Hits(c).count == 3; });
    MCGuiStep(@"Option+Cmd+Return asks as the button does; Cancel", ^{
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);
        GCHECK(MCGuiPerformMenuKey(Panel(c).window, 36,
                                   NSEventModifierFlagCommand | NSEventModifierFlagOption, nil, nil));
    });
    MCGuiWait(@"asked", 10, ^BOOL { return !Panel(c).busy && MCGuiAlertsShown().count == asked + 1; });
    MCGuiStep(@"Cancel: nothing written; then a row dismissed", ^{
        MCGuiNote(@"asked: %@; status: %@", MCGuiAlertsShown().lastObject, Panel(c).status.stringValue);
        GCHECK_EQ_STR(MCGuiAlertsShown().lastObject, @"Replace 3 matches in 2 files?");
        GCHECK_EQ_STR(Disk(k1), @"cat a\ncat b\n");
        GCHECK_EQ_STR(Disk(k2), @"cat c\n");
        GCHECK_EQ_STR(Panel(c).status.stringValue, @"Nothing replaced");
        // The x on k1.txt's line 2.
        NSOutlineView *ov = (NSOutlineView *)Panel(c).table;
        NSInteger row = -1;
        for (NSInteger r = 0; r < ov.numberOfRows; r++) {
            id item = [ov itemAtRow:r];
            if ([ov levelForRow:r] == 1 && [[item valueForKey:@"path"] hasSuffix:@"/k1.txt"] &&
                [[item valueForKey:@"line"] integerValue] == 2)
                row = r;
        }
        GCHECK(row >= 0);
        if (row < 0) return;
        NSView *cell = [ov viewAtColumn:0 row:row makeIfNecessary:YES];
        NSButton *dismiss = [[cell valueForKey:@"actions"] lastObject];
        GCHECK_EQ_STR(dismiss.toolTip, @"Dismiss");
        [NSApp sendAction:dismiss.action to:dismiss.target from:dismiss];
        GCHECK(Hits(c).count == 2 && Panel(c).dismissedCount == 1);
        MCGuiNote(@"after the dismiss: %@", Panel(c).status.stringValue);
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        GCHECK(MCGuiPerformMenuKey(Panel(c).window, 36,
                                   NSEventModifierFlagCommand | NSEventModifierFlagOption, nil, nil));
    });
    MCGuiWait(@"replaced, but the dismissed row", 10, ^BOOL {
        return !Panel(c).busy && [Disk(k2) isEqualToString:@"dog c\n"];
    });
    MCGuiStep(@"asked for two; the dismissed line unchanged on disk", ^{
        MCGuiNote(@"asked: %@ (%@)", MCGuiAlertsShown().lastObject, MCGuiLastAlertInformative());
        GCHECK(MCGuiAlertsShown().count == asked + 2);
        GCHECK_EQ_STR(MCGuiAlertsShown().lastObject, @"Replace 2 matches in 2 files?");
        GCHECK([MCGuiLastAlertInformative() containsString:@"1 match in dismissed rows is left alone"]);
        GCHECK_EQ_STR(Disk(k1), @"dog a\ncat b\n");
    });
    MCGuiWait(@"searched again", 10, ^BOOL { return !Panel(c).busy; });
    __block TerminalView *term = nil;
    MCGuiStep(@"Ctrl+` in the replace field: the terminal shows and has the keyboard", ^{
        SearchPanel *p = Panel(c);
        if ([[c valueForKey:@"terminalVisible"] boolValue]) [c toggleTerminal:nil];
        GCHECK(![[c valueForKey:@"terminalVisible"] boolValue]);
        MCGuiSetKeyWindow(p.window);
        [p.window makeFirstResponder:p.replaceField];
        p.replaceField.stringValue = @"dog";
        MCGuiSendKeyToApp(p.window, 50 /* ` */, NSEventModifierFlagControl, @"`", @"`");
        term = [c valueForKey:@"terminal"];
        MCGuiNote(@"made key: %@; first responder: %@", MCGuiWindowMadeKey().title,
                  c.window.firstResponder.className);
        GCHECK([[c valueForKey:@"terminalVisible"] boolValue]);
        GCHECK(MCGuiWindowMadeKey() == c.window);
        GCHECK(term && c.window.firstResponder == [term valueForKey:@"grid"]);
    });
    MCGuiWait(@"its prompt", 20, ^BOOL { return term.atPrompt && !term.busy; });
    __block NSMutableData *sent = nil;
    MCGuiStep(@"typing goes to the shell, not the replace field", ^{
        MCGuiSetKeyWindow(c.window);
        sent = [NSMutableData data];
        TerminalGridView *g = [term valueForKey:@"grid"];
        void (^forward)(NSData *) = g.onInput;
        g.onInput = ^(NSData *d) {
            [sent appendData:d];
            if (forward) forward(d);
        };
        // To the window the app made key, which hands keys to its first
        // responder.
        Press(c.window, 16 /* y */, @"y");
        Press(c.window, 51 /* Delete */, @"\x7f");
        g.onInput = forward;
        NSString *got = [[NSString alloc] initWithData:sent encoding:NSUTF8StringEncoding];
        MCGuiNote(@"the shell got %lu bytes: %@", (unsigned long)sent.length, got);
        GCHECK([got hasPrefix:@"y"]);
        GCHECK_EQ_STR(Panel(c).replaceField.stringValue, @"dog");
    });
    MCGuiStep(@"Ctrl+` from the Search window with the terminal on show: focused, not hidden", ^{
        SearchPanel *p = Panel(c);
        [c.window makeFirstResponder:nil];
        MCGuiSetKeyWindow(p.window);
        [p.window makeFirstResponder:p.replaceField];
        MCGuiSendKeyToApp(p.window, 50, NSEventModifierFlagControl, @"`", @"`");
        GCHECK([[c valueForKey:@"terminalVisible"] boolValue]);
        GCHECK(MCGuiWindowMadeKey() == c.window);
        GCHECK(c.window.firstResponder == [term valueForKey:@"grid"]);
        GCHECK_EQ_STR(p.replaceField.stringValue, @"dog");
        MCGuiSetKeyWindow(nil);
        [c toggleTerminal:nil];   // hidden again
    });

    MCGuiStep(@"close the panel; Don't Save the tabs", ^{
        [Panel(c).window close];
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);
        [c closeAllTabs:nil];
    });
}
