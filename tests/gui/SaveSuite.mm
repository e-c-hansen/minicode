// SaveSuite.mm — saving and the disk (`make guitest SUITES=save`): the one
// writer behind Cmd+S, a background tab's save and auto-save (symlinks kept,
// a UTF-8 byte order mark kept, viewers without text never written), a read
// that fails never marking old text as the disk's, reloads of clean tabs as
// one undoable step (on show and in the background, and a file deleted and
// made again), auto-save's guards (composing text, a change the date
// misses, the close question), a file too large to follow live, and a
// restored backup opening in its viewer.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "AppSettings.h"
#import "CsvTable.h"
#include <sqlite3.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace {

NSString *Dir(void) { return [MCGuiProject() stringByAppendingPathComponent:@"save"]; }
NSString *F(NSString *name) { return [Dir() stringByAppendingPathComponent:name]; }
NSTextView *TV(EditorController *c) { return [c valueForKey:@"textView"]; }
MCTab *Active(EditorController *c) { return [c valueForKey:@"activeTab"]; }
NSString *Disk(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}
NSData *Bytes(NSString *path) { return [NSData dataWithContentsOfFile:path] ?: [NSData data]; }
BOOL IsLink(NSString *path) {
    struct stat st;
    return lstat(path.fileSystemRepresentation, &st) == 0 && S_ISLNK(st.st_mode);
}
void SetSettings(NSString *text) {
    MCGuiWriteFile(NSProcessInfo.processInfo.environment[@"MINICODE_SETTINGS"], text);
    [[AppSettings shared] reload];
}
// Bytes written over the file in place (same inode), as a writer that
// truncates and writes does.
void WriteInPlace(NSString *path, NSData *bytes) {
    int fd = open(path.fileSystemRepresentation, O_WRONLY | O_TRUNC);
    if (fd < 0) return;
    if (write(fd, bytes.bytes, bytes.length) < 0) {}
    close(fd);
}
NSData *Utf8(NSString *s) { return [s dataUsingEncoding:NSUTF8StringEncoding]; }
NSData *WithBom(NSString *s) {
    NSMutableData *d = [NSMutableData dataWithBytes:"\xEF\xBB\xBF" length:3];
    [d appendData:Utf8(s)];
    return d;
}
// An edit as a key press makes it: one undo group of its own. Steps run
// from timers, where the group the undo manager opens by itself is never
// closed (Harness.h), so that one is closed first.
void OwnGroup(NSUndoManager *u, void (^block)(void)) {
    if (u.groupsByEvent && u.groupingLevel > 0) [u endUndoGrouping];
    MCGuiUndoGroup(u, block);
}
void Type(EditorController *c, NSString *s, NSUInteger at) {
    NSTextView *tv = TV(c);
    OwnGroup(tv.undoManager, ^{ [tv insertText:s replacementRange:NSMakeRange(at, 0)]; });
}
void Undo(EditorController *c) {
    NSUndoManager *u = TV(c).undoManager;
    if (u.groupsByEvent && u.groupingLevel > 0) [u endUndoGrouping];
    [u undo];
}
void Redo(EditorController *c) {
    NSUndoManager *u = TV(c).undoManager;
    if (u.groupsByEvent && u.groupingLevel > 0) [u endUndoGrouping];
    [u redo];
}
NSString *StatusText(EditorController *c) {
    NSTextField *f = [c valueForKey:@"settingsLabel"];
    return f.stringValue ?: @"";
}
NSString *ActiveKind(EditorController *c) {
    for (NSDictionary *t in [c controlTabs])
        if ([t[@"active"] boolValue]) return t[@"kind"] ?: @"";
    return @"";
}

}  // namespace

MC_GUI_SUITE(save) {
    // ------------------------------------------------ symlinks and the writer
    MCGuiStep(@"a file reached through a symlink, saved with Cmd+S", ^{
        SetSettings(@"lsp.enabled = false\n");
        MCGuiWriteFile(F(@"AGENTS.md"), @"agents\n");
        [[NSFileManager defaultManager] createSymbolicLinkAtPath:F(@"CLAUDE.md")
                                             withDestinationPath:@"AGENTS.md" error:nil];
        MCGuiWriteFile(F(@"other.txt"), @"other\n");
        EditorController *c = MCGuiController();
        [c focusEditor:nil];
        GCHECK([c showFile:F(@"CLAUDE.md")]);
        if (c.previewMode) [c togglePreview:nil];
        GCHECK(TV(c).editable);
        Type(c, @"x", 0);
        GCHECK(c.dirty);
        [c saveCurrentFile:nil];
        GCHECK(!c.dirty);
        GCHECK(IsLink(F(@"CLAUDE.md")));
        GCHECK_EQ_STR(Disk(F(@"AGENTS.md")), @"xagents\n");
    });
    MCGuiStep(@"a change to the file the link names is seen", ^{
        EditorController *c = MCGuiController();
        MCGuiWriteFile(F(@"AGENTS.md"), @"from outside\n");
        [c checkExternalChange];
        GCHECK_EQ_STR(TV(c).string, @"from outside\n");
        GCHECK(!c.dirty);
    });
    MCGuiStep(@"auto-save after 200 ms through the link", ^{
        SetSettings(@"lsp.enabled = false\neditor.auto-save = after-delay\n"
                    @"editor.auto-save-delay = 200\n");
        Type(MCGuiController(), @"y", 0);
    });
    MCGuiWait(@"auto-saved", 5, ^BOOL {
        return [Disk(F(@"AGENTS.md")) isEqualToString:@"yfrom outside\n"];
    });
    MCGuiStep(@"still a link", ^{
        GCHECK(IsLink(F(@"CLAUDE.md")));
        GCHECK(!MCGuiController().dirty);
        SetSettings(@"lsp.enabled = false\n");
    });
    MCGuiStep(@"a background tab saved through the link", ^{
        EditorController *c = MCGuiController();
        Type(c, @"z", 0);
        MCTab *link = Active(c);
        GCHECK([c showFile:F(@"other.txt")]);
        GCHECK(link.dirty && Active(c) != link);
        GCHECK([c saveTab:link]);
        GCHECK(!link.dirty);
        GCHECK(IsLink(F(@"CLAUDE.md")));
        GCHECK_EQ_STR(Disk(F(@"AGENTS.md")), @"zyfrom outside\n");
    });

    // ----------------------------------------------------- the byte order mark
    MCGuiStep(@"a text file with a BOM keeps it", ^{
        EditorController *c = MCGuiController();
        [WithBom(@"hello\n") writeToFile:F(@"bom.txt") atomically:YES];
        GCHECK([c showFile:F(@"bom.txt")]);
        GCHECK_EQ_STR(TV(c).string, @"hello\n");   // not in the text
        Type(c, @"x", 0);
        [c saveCurrentFile:nil];
        GCHECK([Bytes(F(@"bom.txt")) isEqualToData:WithBom(@"xhello\n")]);
    });
    MCGuiStep(@"a CSV with a BOM: one cell edited, one line changed, the BOM kept", ^{
        EditorController *c = MCGuiController();
        [WithBom(@"a,b\n1,2\n3,4\n") writeToFile:F(@"excel.csv") atomically:YES];
        GCHECK([c showFile:F(@"excel.csv")]);
        GCHECK_EQ_STR(ActiveKind(c), @"table");
        MCCsvTableView *t = [[c valueForKey:@"viewers"] csv];
        GCHECK_EQ_STR([t cellAtRow:0 column:0], @"a");
        GCHECK([t setCell:@"9" row:1 column:1]);
        [c saveCurrentFile:nil];
        GCHECK([Bytes(F(@"excel.csv")) isEqualToData:WithBom(@"a,b\n1,9\n3,4\n")]);
    });
    MCGuiStep(@"a file without one does not get one", ^{
        EditorController *c = MCGuiController();
        GCHECK([c showFile:F(@"other.txt")]);
        Type(c, @"o", 0);
        [c saveCurrentFile:nil];
        GCHECK([Bytes(F(@"other.txt")) isEqualToData:Utf8(@"oother\n")]);
    });

    // ------------------------------------------------- viewers without text
    MCGuiStep(@"a SQLite tab is never written, even marked unsaved", ^{
        EditorController *c = MCGuiController();
        NSString *db = F(@"data.sqlite");
        sqlite3 *h = nullptr;
        GCHECK(sqlite3_open(db.fileSystemRepresentation, &h) == SQLITE_OK);
        sqlite3_exec(h, "create table t(x); insert into t values (1);", nullptr, nullptr, nullptr);
        sqlite3_close(h);
        NSData *before = Bytes(db);
        GCHECK([c showFile:db]);
        GCHECK(MCViewerKindWithoutText(Active(c).viewerKind));
        MCTab *tab = Active(c);
        tab.dirty = YES;
        [c saveCurrentFile:nil];
        GCHECK([Bytes(db) isEqualToData:before]);
        GCHECK(![c writeText:@"" toTab:tab interactive:NO]);
        GCHECK([Bytes(db) isEqualToData:before]);
        tab.dirty = NO;
    });

    // -------------------------------------- a read that fails, and reloads
    MCGuiStep(@"a file that no longer reads as UTF-8 leaves the change to be seen again", ^{
        EditorController *c = MCGuiController();
        MCGuiWriteFile(F(@"enc.txt"), @"hello\n");
        GCHECK([c showFile:F(@"enc.txt")]);
        WriteInPlace(F(@"enc.txt"), [NSData dataWithBytes:"\xFF\xFEhi" length:4]);
        [c checkExternalChange];
        GCHECK_EQ_STR(TV(c).string, @"hello\n");
        GCHECK(!c.dirty);
        // Not marked as in step with the disk: the next look reads it again.
        GCHECK([Active(c) fileSize] != 4);
        GCHECK([c tab:Active(c) changedOnDisk:MCStampOfFile(F(@"enc.txt"))]);
        WriteInPlace(F(@"enc.txt"), Utf8(@"fixed\n"));
        [c checkExternalChange];
        GCHECK_EQ_STR(TV(c).string, @"fixed\n");
        GCHECK(![c tab:Active(c) changedOnDisk:MCStampOfFile(F(@"enc.txt"))]);
    });
    MCGuiStep(@"a clean tab on show follows its file", ^{
        EditorController *c = MCGuiController();
        MCGuiWriteFile(F(@"reload.txt"), @"one\ntwo\n");
        GCHECK([c showFile:F(@"reload.txt")]);
        Type(c, @"x", 0);
        [c saveCurrentFile:nil];
        MCGuiWriteFile(F(@"reload.txt"), @"xone\nTWO\nthree\n");
        [c checkTabsOnDisk];
    });
    MCGuiWait(@"the new text, read off the main thread", 5, ^BOOL {
        return [TV(MCGuiController()).string isEqualToString:@"xone\nTWO\nthree\n"];
    });
    MCGuiStep(@"the reload is one undo step, and the typing before it stays", ^{
        EditorController *c = MCGuiController();
        GCHECK(!c.dirty);
        GCHECK_EQ_STR(TV(c).undoManager.undoActionName, @"Reload from Disk");
        Undo(c);
        GCHECK_EQ_STR(TV(c).string, @"xone\ntwo\n");
        GCHECK(c.dirty);   // differs from the disk now
        Undo(c);
        GCHECK_EQ_STR(TV(c).string, @"one\ntwo\n");
        Redo(c);
        Redo(c);
        GCHECK_EQ_STR(TV(c).string, @"xone\nTWO\nthree\n");
        GCHECK(!c.dirty);
    });
    MCGuiStep(@"a clean tab in the background follows its file", ^{
        EditorController *c = MCGuiController();
        MCGuiWriteFile(F(@"bg.txt"), @"alpha\n");
        GCHECK([c showFile:F(@"bg.txt")]);
        GCHECK([c showFile:F(@"other.txt")]);
        MCGuiWriteFile(F(@"bg.txt"), @"beta\n");
        [c checkTabsOnDisk];
    });
    MCGuiWait(@"its text in the tab", 5, ^BOOL {
        return [[MCGuiController() tabForPath:F(@"bg.txt")].sourceText isEqualToString:@"beta\n"];
    });
    MCGuiStep(@"shown, Cmd+Z brings the old text back, and redo the new", ^{
        EditorController *c = MCGuiController();
        GCHECK(![c tabForPath:F(@"bg.txt")].dirty);
        GCHECK([c showFile:F(@"bg.txt")]);
        GCHECK_EQ_STR(TV(c).string, @"beta\n");
        Undo(c);
        GCHECK_EQ_STR(TV(c).string, @"alpha\n");
        GCHECK(c.dirty);
        Redo(c);
        GCHECK_EQ_STR(TV(c).string, @"beta\n");
        GCHECK(!c.dirty);
    });
    MCGuiStep(@"a file deleted and made again is read in the same pass", ^{
        EditorController *c = MCGuiController();
        MCGuiWriteFile(F(@"dr.txt"), @"first\n");
        GCHECK([c showFile:F(@"dr.txt")]);
        GCHECK([c showFile:F(@"other.txt")]);
        [[NSFileManager defaultManager] removeItemAtPath:F(@"dr.txt") error:nil];
        [c checkTabsOnDisk];
        GCHECK([c tabForPath:F(@"dr.txt")].gone);
        MCGuiWriteFile(F(@"dr.txt"), @"second\n");
        [c checkTabsOnDisk];
        GCHECK(![c tabForPath:F(@"dr.txt")].gone);
    });
    MCGuiWait(@"the new text, clean", 5, ^BOOL {
        MCTab *t = [MCGuiController() tabForPath:F(@"dr.txt")];
        return [t.sourceText isEqualToString:@"second\n"] && !t.dirty;
    });

    // ------------------------------------------------------ auto-save's guards
    MCGuiStep(@"text an input method is composing is not auto-saved", ^{
        SetSettings(@"lsp.enabled = false\neditor.auto-save = after-delay\n"
                    @"editor.auto-save-delay = 60000\n");
        EditorController *c = MCGuiController();
        MCGuiWriteFile(F(@"ime.txt"), @"ab\n");
        GCHECK([c showFile:F(@"ime.txt")]);
        NSTextView *tv = TV(c);
        Type(c, @"z", 0);
        GCHECK(c.dirty);
        tv.selectedRange = NSMakeRange(3, 0);
        [tv setMarkedText:@"か" selectedRange:NSMakeRange(1, 0) replacementRange:NSMakeRange(3, 0)];
        GCHECK(tv.hasMarkedText);
        [c autoSaveNow];
        GCHECK_EQ_STR(Disk(F(@"ime.txt")), @"ab\n");
        [tv unmarkText];
        [c autoSaveNow];
        GCHECK_EQ_STR(Disk(F(@"ime.txt")), @"zabか\n");
    });
    MCGuiStep(@"a change outside that keeps the date is still not written over", ^{
        EditorController *c = MCGuiController();
        Type(c, @"!", 0);
        // Written in place and its date put back, as a coarse file system
        // can leave it: the size tells.
        struct stat st;
        stat(F(@"ime.txt").fileSystemRepresentation, &st);
        WriteInPlace(F(@"ime.txt"), Utf8(@"outside, longer\n"));
        struct timespec times[2] = {st.st_atimespec, st.st_mtimespec};
        utimensat(AT_FDCWD, F(@"ime.txt").fileSystemRepresentation, times, 0);
        GCHECK([MCStampOfFile(F(@"ime.txt")).modDate isEqualToDate:Active(c).fileModDate]);
        [c autoSaveNow];
        GCHECK_EQ_STR(Disk(F(@"ime.txt")), @"outside, longer\n");
        GCHECK([StatusText(c) containsString:@"Not auto-saved: ime.txt"]);
        // Let go, so nothing asks about it later.
        [c reloadTabFromDisk:Active(c)];
        GCHECK(!c.dirty);
    });
    MCGuiStep(@"focus-change auto-save writes before the close question", ^{
        SetSettings(@"lsp.enabled = false\neditor.auto-save = focus-change\n");
        EditorController *c = MCGuiController();
        MCGuiWriteFile(F(@"close.txt"), @"keep\n");
        GCHECK([c showFile:F(@"close.txt")]);
        Type(c, @"k", 0);
        const NSUInteger alerts = MCGuiAlertsShown().count;
        GCHECK([c closeTab:Active(c) prompt:YES]);
        GCHECK(MCGuiAlertsShown().count == alerts);   // nothing asked
        GCHECK_EQ_STR(Disk(F(@"close.txt")), @"kkeep\n");
        GCHECK([c tabForPath:F(@"close.txt")] == nil);
        SetSettings(@"lsp.enabled = false\n");
    });

    // --------------------------------------------- too large to follow live
    MCGuiStep(@"a 9 MB log in a clean tab, appended to", ^{
        EditorController *c = MCGuiController();
        NSMutableString *big = [NSMutableString stringWithCapacity:9500000];
        while (big.length < 9 * 1024 * 1024) [big appendString:@"a line of the log, as a job writes it\n"];
        MCGuiWriteFile(F(@"train.log"), big);
        GCHECK([c showFile:F(@"train.log")]);
        GCHECK(TV(c).string.length == big.length);
        NSFileHandle *h = [NSFileHandle fileHandleForWritingAtPath:F(@"train.log")];
        [h seekToEndOfFile];
        [h writeData:Utf8(@"one more\n")];
        [h closeFile];
        [c checkTabsOnDisk];
    });
    MCGuiWait(@"said once, not read live", 5, ^BOOL {
        return [StatusText(MCGuiController()) containsString:@"too large to follow live"];
    });
    MCGuiStep(@"read when the window comes forward", ^{
        EditorController *c = MCGuiController();
        GCHECK(![TV(c).string hasSuffix:@"one more\n"]);
        GCHECK(Active(c).tooLargeToFollow);
        [c checkExternalChange];
        GCHECK([TV(c).string hasSuffix:@"one more\n"]);
        GCHECK(!c.dirty);
        GCHECK(!Active(c).tooLargeToFollow);
    });

    // ---------------------------------------------- a backup in its viewer
    MCGuiStep(@"a restored backup of a CSV opens as its table", ^{
        EditorController *c = MCGuiController();
        MCGuiWriteFile(F(@"restored.csv"), @"a,b\n1,2\n");
        MCTab *t = [MCTab new];
        t.currentPath = F(@"restored.csv");
        t.restoredBackup = YES;
        t.dirty = YES;
        t.sourceText = @"a,b\n1,7\n";
        [c insertTab:t];
        [c activateTab:t];
        GCHECK_EQ_STR(ActiveKind(c), @"table");
        GCHECK(c.dirty);
        MCCsvTableView *table = [[c valueForKey:@"viewers"] csv];
        GCHECK_EQ_STR([table cellAtRow:1 column:1], @"7");
        [c saveCurrentFile:nil];
        GCHECK_EQ_STR(Disk(F(@"restored.csv")), @"a,b\n1,7\n");
        GCHECK(!c.dirty);
    });
}
