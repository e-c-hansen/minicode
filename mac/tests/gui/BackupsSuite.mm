// BackupsSuite.mm — `make guitest SUITES=backups`: unsaved tabs survive
// a SIGKILL. The suite runs the app twice more as child processes in the
// same scratch world: the first (_backups_writer) edits three files without
// saving, waits for their backups, and kills itself with SIGKILL; between
// the two, one file changes on disk and another is deleted; the second
// (_backups_restore) opens the folder and finds every buffer back unsaved,
// says which file changed and which is gone, and closing with Don't Save
// leaves no backup behind. Two UTF-16 files go the same way (round 6's
// D10): one comes back and saves as UTF-16LE with its mark; the other is
// deleted meanwhile, and its backup's record of the encoding saves it as
// UTF-16BE again.
//
// In the suite's own process (round 10's workspace item 1): two windows on
// one folder with a.txt unsaved in both, as hot exit leaves them at Quit,
// each come back with their own text, and neither backup is deleted; a
// window that is not one of them takes both, the other window's text as
// "a (unsaved copy).txt" beside the file.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "BackupStore.h"
#include "Backups.h"
#include "TabSessions.h"
#include <signal.h>

@interface EditorController (BackupsSuite)
- (TabSessions::Window)windowRecordNow;
@end
@interface NSObject (BackupsSuiteApp)
- (EditorController *)openWindowAtPath:(NSString *)path restoring:(NSString *)record;
@end

namespace {
NSString *Folder() { return [MCGuiScratch() stringByAppendingPathComponent:@"bk"]; }
NSString *In(NSString *name) { return [Folder() stringByAppendingPathComponent:name]; }
NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
NSString *Disk(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}
// The backups in the store that name a file in the folder.
NSUInteger BackupsOfFolder() {
    NSString *dir = NSProcessInfo.processInfo.environment[@"MUTIBLE_BACKUP_DIR"];
    NSUInteger n = 0;
    for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil]) {
        if (![f.pathExtension isEqualToString:@"mcbackup"]) continue;
        NSString *t = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:f]
                                                encoding:NSUTF8StringEncoding error:nil];
        if ([t containsString:@"/bk/"]) n++;
    }
    return n;
}
// Text as UTF-16 with its byte order mark first.
NSData *Utf16(NSString *s, BOOL le) {
    NSMutableData *d = [NSMutableData dataWithBytes:le ? "\xFF\xFE" : "\xFE\xFF" length:2];
    [d appendData:[s dataUsingEncoding:le ? NSUTF16LittleEndianStringEncoding
                                          : NSUTF16BigEndianStringEncoding]];
    return d;
}
// In this order: c.txt is the tab on show when the writer dies, so the
// restoring window, which cannot show a gone file at launch, shows b.txt,
// and its becoming key asks about b.txt's change (checked below).
NSArray<NSString *> *Edited() {
    return @[@"d16.txt", @"e16.txt", @"a.txt", @"b.txt", @"c.txt"];
}
// Two windows' backups, as their Quit with hot exit wrote them.
NSString *TwoWindows() { return [MCGuiScratch() stringByAppendingPathComponent:@"twowin"]; }
NSString *TwoRoot() { return TwoWindows().stringByResolvingSymlinksInPath; }
Backups::Store TestStore() {
    Backups::Store st;
    st.dir = MCBackupStore.shared.directory.fileSystemRepresentation;
    st.fs = Backups::posixFs();
    return st;
}
BOOL SaveBackup(NSString *bid, NSString *name, NSString *window, double age, NSString *text) {
    Backups::Entry e;
    e.id = bid.UTF8String;
    e.path = [TwoRoot() stringByAppendingPathComponent:name].fileSystemRepresentation;
    e.root = TwoRoot().fileSystemRepresentation;
    e.window = window ? window.UTF8String : "";
    // Based on the file as it is on disk, as an unsaved tab's backup is.
    const Backups::Stat st = Backups::posixFs().stat(e.path);
    e.diskExisted = st.exists;
    e.diskMtime = st.mtime;
    e.diskInode = st.inode;
    e.saved = NSDate.date.timeIntervalSince1970 - age;
    e.ownerPid = 0;   // its process is gone
    return TestStore().save(e, text.UTF8String);
}
BOOL HasBackup(NSString *bid) {
    Backups::Entry e;
    std::string text;
    return TestStore().load(bid.UTF8String, &e, &text);
}
NSString *Record(NSString *key) {
    TabSessions::Window w;
    w.root = TwoRoot().UTF8String;
    w.backupKey = key.UTF8String;
    return @(TabSessions::encodeWindows({w}).c_str());
}
NSArray<NSString *> *TabNames(EditorController *c) {
    NSMutableArray *names = [NSMutableArray array];
    for (NSString *p in [c valueForKeyPath:@"tabs.currentPath"])
        if ([p isKindOfClass:NSString.class]) [names addObject:p.lastPathComponent];
    return [names sortedArrayUsingSelector:@selector(compare:)];
}
NSString *TextOf(EditorController *c, NSString *name) {
    if (![c showFile:[TwoRoot() stringByAppendingPathComponent:name]]) return @"(no tab)";
    return Text(c).string;
}
void CloseUnasked(EditorController *c) {
    NSArray<MCTab *> *tabs = [NSArray arrayWithArray:[c valueForKey:@"tabs"]];
    for (MCTab *t in tabs) [c closeTab:t prompt:NO];
    [c.window performClose:nil];
    [MCBackupStore.shared flush];
}
void TypeAtStart(EditorController *c, NSString *s) {
    NSTextView *tv = Text(c);
    MCGuiUndoGroup(tv.undoManager, ^{
        tv.selectedRange = NSMakeRange(0, 0);
        [tv insertText:s replacementRange:tv.selectedRange];
    });
}
}  // namespace

// ------------------------------------------------------- the parent's part
MC_GUI_SUITE(backups) {
    MCGuiStep(@"a folder of five files, two of them UTF-16", ^{
        MCGuiWriteFile(In(@"a.txt"), @"alpha\n");
        MCGuiWriteFile(In(@"b.txt"), @"beta\n");
        MCGuiWriteFile(In(@"c.txt"), @"gamma\n");
        GCHECK([Utf16(@"delta\r\n", YES) writeToFile:In(@"d16.txt") atomically:NO]);
        GCHECK([Utf16(@"epsilon\n", NO) writeToFile:In(@"e16.txt") atomically:NO]);
    });
    MCGuiAsync(@"a first app edits all five, unsaved, and is SIGKILLed", 90, ^(MCGuiDone done) {
        MCGuiRunChild(@"_backups_writer", Folder(), ^(int status, NSString *out) {
            MCGuiCheck(status == SIGKILL, [NSString stringWithFormat:@"the writer died of SIGKILL (%d)", status],
                       __FILE__, __LINE__);
            if (status != SIGKILL || [out containsString:@"FAIL"]) MCGuiNote(@"writer:\n%@", out);
            GCHECK(![out containsString:@"FAIL"]);
            done();
        });
    });
    MCGuiStep(@"its backups are on disk; the files are not changed", ^{
        GCHECK(BackupsOfFolder() == 5);
        GCHECK_EQ_STR(Disk(In(@"a.txt")), @"alpha\n");
        GCHECK([[NSData dataWithContentsOfFile:In(@"d16.txt")] isEqualToData:Utf16(@"delta\r\n", YES)]);
        // Meanwhile, outside: b.txt changes, c.txt and e16.txt go.
        MCGuiWriteFile(In(@"b.txt"), @"beta, changed outside\n");
        [[NSFileManager defaultManager] removeItemAtPath:In(@"c.txt") error:nil];
        [[NSFileManager defaultManager] removeItemAtPath:In(@"e16.txt") error:nil];
    });
    MCGuiAsync(@"a second app restores them", 90, ^(MCGuiDone done) {
        MCGuiRunChild(@"_backups_restore", Folder(), ^(int status, NSString *out) {
            GCHECK(status == 0);
            if (status != 0) MCGuiNote(@"restore:\n%@", out);
            NSRange r = [out rangeOfString:@"_backups_restore: "];
            if (r.location != NSNotFound)
                MCGuiNote(@"the restoring app: %@",
                          [[out substringFromIndex:r.location]
                              componentsSeparatedByString:@"\n"].firstObject);
            done();
        });
    });
    MCGuiStep(@"Don't Save left no backup, and wrote nothing", ^{
        GCHECK(BackupsOfFolder() == 0);
        GCHECK_EQ_STR(Disk(In(@"a.txt")), @"alpha\n");
        GCHECK_EQ_STR(Disk(In(@"b.txt")), @"beta, changed outside\n");
        GCHECK(![[NSFileManager defaultManager] fileExistsAtPath:In(@"c.txt")]);
        // The UTF-16 ones, saved by the restoring app in their encodings.
        NSData *d = [NSData dataWithContentsOfFile:In(@"d16.txt")];
        NSData *e = [NSData dataWithContentsOfFile:In(@"e16.txt")];
        if (![d isEqualToData:Utf16(@"unsaved d16.txt delta\r\n", YES)]) MCGuiNote(@"d16.txt: %@", d);
        if (![e isEqualToData:Utf16(@"unsaved e16.txt epsilon\n", NO)]) MCGuiNote(@"e16.txt: %@", e);
        GCHECK([d isEqualToData:Utf16(@"unsaved d16.txt delta\r\n", YES)]);
        GCHECK([e isEqualToData:Utf16(@"unsaved e16.txt epsilon\n", NO)]);
    });

    // --------------------------- two windows, one file unsaved in both
    NSString *k1 = NSUUID.UUID.UUIDString, *k2 = NSUUID.UUID.UUIDString;
    NSString *a1 = NSUUID.UUID.UUIDString, *b2 = NSUUID.UUID.UUIDString,
             *c3 = NSUUID.UUID.UUIDString;
    __block EditorController *one = nil, *two = nil;
    MCGuiStep(@"two windows on one folder quit with a.txt unsaved in both", ^{
        MCGuiWriteFile([TwoWindows() stringByAppendingPathComponent:@"a.txt"], @"a on disk\n");
        MCGuiWriteFile([TwoWindows() stringByAppendingPathComponent:@"b.txt"], @"b on disk\n");
        GCHECK(SaveBackup(a1, @"a.txt", k1, 30, @"window one's a\n"));
        GCHECK(SaveBackup(b2, @"a.txt", k2, 20, @"window two's a\n"));   // newer
        GCHECK(SaveBackup(c3, @"b.txt", k2, 10, @"window two's b\n"));
    });
    MCGuiStep(@"the first window back takes its own a.txt and leaves the second's", ^{
        // As a launch reserves every window it brings back (windowsToRestore).
        [MCBackupStore.shared reserveWindowKeys:@[k1, k2]];
        one = [(NSObject *)NSApp.delegate openWindowAtPath:TwoWindows() restoring:Record(k1)];
        MCGuiNote(@"window one: %@", TabNames(one));
        GCHECK_EQ_STR([TabNames(one) componentsJoinedByString:@" "], @"a.txt");
        GCHECK_EQ_STR(TextOf(one, @"a.txt"), @"window one's a\n");
        GCHECK([[one valueForKey:@"dirty"] boolValue]);
        GCHECK_EQ_STR([one backupKey], k1);
        // Nothing deleted: the second window's backups wait for it.
        GCHECK(HasBackup(a1) && HasBackup(b2) && HasBackup(c3));
    });
    MCGuiStep(@"the second window back takes its own a.txt and b.txt", ^{
        two = [(NSObject *)NSApp.delegate openWindowAtPath:TwoWindows() restoring:Record(k2)];
        MCGuiNote(@"window two: %@", TabNames(two));
        GCHECK_EQ_STR([TabNames(two) componentsJoinedByString:@" "], @"a.txt b.txt");
        GCHECK_EQ_STR(TextOf(two, @"a.txt"), @"window two's a\n");
        GCHECK_EQ_STR(TextOf(two, @"b.txt"), @"window two's b\n");
        // Each window writes its own key down for the next launch.
        GCHECK_EQ_STR(@([two windowRecordNow].backupKey.c_str()), k2);
        CloseUnasked(one);
        CloseUnasked(two);
        MCGuiSetKeyWindow(nil);
    });
    MCGuiWait(@"closed unasked, their backups go", 10, ^BOOL {
        return !HasBackup(a1) && !HasBackup(b2) && !HasBackup(c3);
    });
    __block EditorController *any = nil;
    NSString *d1 = NSUUID.UUID.UUIDString, *d2 = NSUUID.UUID.UUIDString;
    MCGuiStep(@"a window that is neither takes both: the other text as a copy beside it", ^{
        GCHECK(SaveBackup(d1, @"a.txt", k1, 30, @"window one's a\n"));
        GCHECK(SaveBackup(d2, @"a.txt", k2, 20, @"window two's a\n"));
        any = [(NSObject *)NSApp.delegate openWindowAtPath:TwoWindows() restoring:nil];
        // (The folder's own session may add b.txt, closed unsaved above.)
        MCGuiNote(@"the window: %@", TabNames(any));
        GCHECK([TabNames(any) containsObject:@"a.txt"]);
        GCHECK([TabNames(any) containsObject:@"a (unsaved copy).txt"]);
        GCHECK(![TabNames(any) containsObject:@"a (unsaved copy 2).txt"]);
        GCHECK_EQ_STR(TextOf(any, @"a.txt"), @"window two's a\n");   // the newer
        MCTab *copy = nil;
        for (MCTab *t in [any valueForKey:@"tabs"])
            if ([t.currentPath.lastPathComponent isEqualToString:@"a (unsaved copy).txt"]) copy = t;
        GCHECK(copy.dirty && copy.untitled);
        GCHECK_EQ_STR(copy.draftOf.lastPathComponent, @"a.txt");
        GCHECK_EQ_STR(copy.sourceText, @"window one's a\n");
        // Never on disk until saved; its backup is kept under its new name.
        GCHECK(![[NSFileManager defaultManager]
            fileExistsAtPath:[TwoRoot() stringByAppendingPathComponent:@"a (unsaved copy).txt"]]);
        Backups::Entry e;
        std::string text;
        GCHECK(TestStore().load(d1.UTF8String, &e, &text) && text == "window one's a\n" &&
               [@(e.path.c_str()) hasSuffix:@"/a (unsaved copy).txt"]);
        GCHECK(HasBackup(d2));
        CloseUnasked(any);
        MCGuiSetKeyWindow(nil);
    });
    MCGuiWait(@"closed unasked, these backups go too", 10, ^BOOL {
        return !HasBackup(d1) && !HasBackup(d2);
    });
}

// ------------------------------------------- the first child: edit and die
MC_GUI_SUITE(_backups_writer) {
    __block EditorController *c = nil;
    MCGuiStep(@"edit all five without saving", ^{
        c = MCGuiController();
        GCHECK([c.rootPath.lastPathComponent isEqualToString:@"bk"]);
        [c focusEditor:nil];
        for (NSString *name in Edited()) {
            GCHECK([c showFile:In(name)]);
            TypeAtStart(c, [NSString stringWithFormat:@"unsaved %@ ", name]);
        }
        GCHECK([[c valueForKey:@"dirty"] boolValue]);
    });
    // Written about a second after typing stops.
    MCGuiWait(@"five backups", 15, ^BOOL { return BackupsOfFolder() == 5; });
    MCGuiStep(@"SIGKILL", ^{
        fflush(stdout);
        kill(getpid(), SIGKILL);
    });
}

// ------------------------------------------ the second child: find them back
MC_GUI_SUITE(_backups_restore) {
    __block EditorController *c = nil;
    MCGuiStep(@"the folder's window has the five tabs back", ^{
        c = MCGuiController();
        GCHECK([c.rootPath.lastPathComponent isEqualToString:@"bk"]);
        NSArray *paths = [c valueForKeyPath:@"tabs.currentPath"];
        MCGuiNote(@"tabs: %@", [paths valueForKey:@"lastPathComponent"]);
        for (NSString *name in Edited()) {
            BOOL found = NO;
            for (NSString *p in paths) found |= [p hasSuffix:[@"/" stringByAppendingString:name]];
            MCGuiCheck(found, [NSString stringWithFormat:@"a tab for %@", name], __FILE__, __LINE__);
        }
    });
    MCGuiStep(@"a.txt: the unsaved text, dirty, the disk's text as saved", ^{
        GCHECK([c showFile:In(@"a.txt")]);
        GCHECK_EQ_STR(Text(c).string, @"unsaved a.txt alpha\n");
        GCHECK([[c valueForKey:@"dirty"] boolValue]);
        GCHECK(!Text(c).undoManager.canUndo);
        NSString *note = [[c valueForKey:@"settingsLabel"] stringValue];
        GCHECK_EQ_STR(note, @"Restored unsaved changes to a.txt");
    });
    // The backup was made from the file as it was; it has changed since, so
    // the app asks Keep Mine or Reload when the tab shows (answered Keep
    // Mine here, the first button), and nothing is written meanwhile.
    MCGuiStep(@"b.txt: the unsaved text, and the question about the disk", ^{
        GCHECK([c showFile:In(@"b.txt")]);
        GCHECK_EQ_STR(Text(c).string, @"unsaved b.txt beta\n");
        GCHECK([[c valueForKey:@"dirty"] boolValue]);
        GCHECK([MCGuiAlertsShown() containsObject:@"“b.txt” changed on disk."]);
        if (![MCGuiAlertsShown() containsObject:@"“b.txt” changed on disk."])
            MCGuiNote(@"alerts: %@", MCGuiAlertsShown());
        GCHECK_EQ_STR(Disk(In(@"b.txt")), @"beta, changed outside\n");
    });
    MCGuiStep(@"c.txt: gone from disk, the text held, (deleted) in the title", ^{
        MCTab *gone = nil;
        for (MCTab *t in [c valueForKey:@"tabs"])
            if ([[(id)t valueForKey:@"currentPath"] hasSuffix:@"/c.txt"]) gone = t;
        GCHECK(gone != nil);
        if (gone) [c activateTab:gone];
        GCHECK([[(id)gone valueForKey:@"gone"] boolValue]);
        GCHECK_EQ_STR(Text(c).string, @"unsaved c.txt gamma\n");
        GCHECK([[c valueForKey:@"dirty"] boolValue]);
        GCHECK([c.window.title containsString:@"(deleted)"]);
        MCGuiNote(@"title: %@", c.window.title);
    });
    // A UTF-16 file restored over its disk text, and one whose file went:
    // both saved in their own encodings (the parent compares the bytes).
    MCGuiStep(@"d16.txt: the unsaved text, saved as UTF-16LE", ^{
        GCHECK([c showFile:In(@"d16.txt")]);
        GCHECK_EQ_STR(Text(c).string, @"unsaved d16.txt delta\r\n");
        GCHECK([[c valueForKey:@"dirty"] boolValue]);
        GCHECK([[c valueForKeyPath:@"activeTab.encoding"] integerValue] == MCTextEncodingUtf16LE);
        [c saveCurrentFile:nil];
        GCHECK(![[c valueForKey:@"dirty"] boolValue]);
    });
    MCGuiStep(@"e16.txt: gone, the text held; saved as UTF-16BE, as its backup says", ^{
        MCTab *gone = nil;
        for (MCTab *t in [c valueForKey:@"tabs"])
            if ([[(id)t valueForKey:@"currentPath"] hasSuffix:@"/e16.txt"]) gone = t;
        GCHECK(gone != nil);
        if (gone) [c activateTab:gone];
        GCHECK([[(id)gone valueForKey:@"gone"] boolValue]);
        GCHECK_EQ_STR(Text(c).string, @"unsaved e16.txt epsilon\n");
        GCHECK([[c valueForKeyPath:@"activeTab.encoding"] integerValue] == MCTextEncodingUtf16BE);
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);   // "no longer on disk": Save
        [c saveCurrentFile:nil];
        GCHECK(![[c valueForKey:@"dirty"] boolValue]);
    });
    MCGuiStep(@"close the window: Don't Save", ^{
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);
        [c.window performClose:nil];
        [c.window close];
    });
    MCGuiWait(@"the backups are gone", 10, ^BOOL { return BackupsOfFolder() == 0; });
}
