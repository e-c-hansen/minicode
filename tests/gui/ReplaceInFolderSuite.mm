// ReplaceInFolderSuite.mm — `make guitest SUITES=replace_in_folder`:
// Replace in Folder (Option+Shift+Cmd+F). Replace All counts first and asks;
// a file open in a clean tab is changed in the tab, unsaved and undoable;
// a file whose tab has unsaved changes is left alone and named; the rest
// are written to disk; and replacing one result changes only its match.
#import "Harness.h"
#import "EditorController.h"
#import "Search.h"

namespace {
NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
NSString *Disk(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}
SearchPanel *Panel(EditorController *c) { return [c valueForKey:@"searchPanel"]; }
NSArray *Hits(EditorController *c) { return [Panel(c) valueForKey:@"hits"]; }
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
    MCGuiStep(@"close the panel; Don't Save the tabs", ^{
        [Panel(c).window close];
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);
        [c closeAllTabs:nil];
    });
}
