// FolderPickerSuite.mm — `make guitest SUITES=folder_picker`: minicode's own
// folder picker (PalettePanel.h, MCFolderPick; the core's FolderPicker.h),
// which Open Folder… and the Dock's Open Folder… show in place of the
// system's open panel. In a scratch tree: the window's subfolders listed
// (hidden ones only when a dot is typed), a typed path completed with Tab
// and with '/', Return opening the folder typed out, Cmd+Return opening
// the text's folder, Esc changing nothing, Choose in Finder… last, a
// folder of 3,000 subfolders filtered quickly, and the Dock's picker
// opening a new window.
#import "Harness.h"
#import "EditorController.h"
#import "PalettePanel.h"
#import "RecentFolders.h"

namespace {
NSView *Picker(EditorController *c) { return MCCommandPaletteIn(c.window); }
NSArray<NSString *> *Labels(EditorController *c) { return MCCommandPaletteLabels(Picker(c)); }
NSString *Selected(EditorController *c) {
    NSInteger i = MCCommandPaletteSelection(Picker(c));
    NSArray *l = Labels(c);
    return i >= 0 && i < (NSInteger)l.count ? l[(NSUInteger)i] : nil;
}
NSTextView *FieldEditor(EditorController *c) {
    return (NSTextView *)MCCommandPaletteField(Picker(c)).currentEditor;
}
NSString *FieldText(EditorController *c) { return MCCommandPaletteField(Picker(c)).stringValue; }
void Type(EditorController *c, NSString *s) {
    NSTextView *ed = FieldEditor(c);
    [ed insertText:s replacementRange:ed.selectedRange];
}
void Command(EditorController *c, SEL sel) { [FieldEditor(c) doCommandBySelector:sel]; }
// Rows spell paths with "~" for home, and the window's folder as the
// window spells it (/tmp or /private/tmp): compared resolved.
BOOL SamePath(NSString *a, NSString *b) {
    if (!a || !b) return NO;
    return [a.stringByExpandingTildeInPath.stringByResolvingSymlinksInPath
        isEqualToString:b.stringByExpandingTildeInPath.stringByResolvingSymlinksInPath];
}
BOOL Lists(NSArray<NSString *> *labels, NSString *path) {
    for (NSString *l in labels)
        if (SamePath(l, path)) return YES;
    return NO;
}
void MakeDir(NSString *path) {
    [[NSFileManager defaultManager] createDirectoryAtPath:path withIntermediateDirectories:YES
                                               attributes:nil error:nil];
}
}  // namespace

MC_GUI_SUITE(folder_picker) {
    NSString *root = [MCGuiScratch() stringByAppendingPathComponent:@"picker-tree"];
    NSString *alpha = [root stringByAppendingPathComponent:@"alpha"];
    NSString *innerOne = [alpha stringByAppendingPathComponent:@"inner-one"];
    NSString *innerTwo = [alpha stringByAppendingPathComponent:@"inner-two"];
    NSString *beta = [root stringByAppendingPathComponent:@"beta"];
    NSString *secret = [root stringByAppendingPathComponent:@".secret"];
    NSString *many = [root stringByAppendingPathComponent:@"many"];
    __block EditorController *w = nil;

    MCGuiStep(@"a window of the suite's own on a scratch tree", ^{
        for (NSString *d in @[ innerOne, innerTwo, beta, secret ]) MakeDir(d);
        for (int i = 0; i < 3000; i++)
            MakeDir([many stringByAppendingPathComponent:[NSString stringWithFormat:@"d%d", i]]);
        MCGuiWriteFile([root stringByAppendingPathComponent:@"not-a-folder.txt"], @"x\n");
        [(id<NSApplicationDelegate>)NSApp.delegate
            application:NSApp
               openURLs:@[ [NSURL fileURLWithPath:root isDirectory:YES] ]];
        for (EditorController *e in MCGuiControllers())
            if (SamePath(e.rootPath, root)) w = e;
        GCHECK(w != nil);
        MCGuiSetKeyWindow(w.window);
    });
    MCGuiStep(@"Open Folder… shows the picker, not the system's panel", ^{
        [w openFolder:nil];
        GCHECK(Picker(w) != nil);
        GCHECK(w.window.firstResponder == FieldEditor(w));
        GCHECK_EQ_STR(FieldText(w), @"");
    });
    MCGuiWait(@"the window's subfolders listed", 5, ^BOOL {
        return Lists(Labels(w), alpha);
    });
    MCGuiStep(@"recent folders first, then the subfolders; no hidden ones, no files; Finder last", ^{
        NSArray *l = Labels(w);
        MCGuiNote(@"rows: %@", l);
        GCHECK(Lists(l, beta));
        GCHECK(!Lists(l, secret));
        GCHECK(!Lists(l, [root stringByAppendingPathComponent:@"not-a-folder.txt"]));
        GCHECK_EQ_STR(l.lastObject, @"Choose in Finder…");
        // The window's own folder was opened just now, so it is recent.
        NSArray *keys = MCCommandPaletteKeys(Picker(w));
        GCHECK(keys.count && [keys[0] isEqualToString:@"recent"]);
        NSUInteger alphaAt = [l indexOfObjectPassingTest:^BOOL(NSString *x, NSUInteger i, BOOL *stop) {
            (void)i; (void)stop;
            return SamePath(x, alpha);
        }];
        GCHECK(alphaAt != NSNotFound && alphaAt > 0);
        // A name filters with the palette's ranking.
        Type(w, @"bet");
        GCHECK(SamePath(Selected(w), beta));
    });
    MCGuiStep(@"a typed path lists its folder's subfolders", ^{
        NSTextView *ed = FieldEditor(w);
        ed.selectedRange = NSMakeRange(0, ed.string.length);
        Type(w, [root stringByAppendingString:@"/al"]);
    });
    MCGuiWait(@"the tree's folder listed", 5, ^BOOL { return SamePath(Selected(w), alpha); });
    MCGuiStep(@"Tab completes the selected folder and goes into it", ^{
        Command(w, @selector(insertTab:));
        GCHECK_EQ_STR(FieldText(w), [root stringByAppendingString:@"/alpha/"]);
        GCHECK(Picker(w) != nil);
    });
    MCGuiWait(@"alpha's subfolders listed", 5, ^BOOL {
        return Lists(Labels(w), innerTwo);
    });
    MCGuiStep(@"ending in '/', Open This Folder comes first; '/' after a name descends", ^{
        GCHECK_EQ_STR(Selected(w), @"Open This Folder");
        GCHECK(Lists(Labels(w), innerOne));
        Type(w, @"inner-t");
        GCHECK(SamePath(Selected(w), innerTwo));
        Type(w, @"/");
        GCHECK_EQ_STR(FieldText(w), [root stringByAppendingString:@"/alpha/inner-two/"]);
    });
    MCGuiWait(@"inner-two read (it has no subfolders)", 5, ^BOOL {
        return [Selected(w) isEqual:@"Open This Folder"];
    });
    MCGuiStep(@"Return opens it: the window moves to the folder", ^{
        Command(w, @selector(insertNewline:));
        GCHECK(Picker(w) == nil);
    });
    MCGuiWait(@"the window on inner-two", 5, ^BOOL { return SamePath(w.rootPath, innerTwo); });
    MCGuiStep(@"a dot typed lists hidden folders; Esc changes nothing", ^{
        [w openFolder:nil];
        Type(w, [root stringByAppendingString:@"/.se"]);
    });
    MCGuiWait(@"the hidden folder listed", 5, ^BOOL { return SamePath(Selected(w), secret); });
    MCGuiStep(@"Esc closes the picker and the window keeps its folder", ^{
        Command(w, @selector(cancelOperation:));
        GCHECK(Picker(w) == nil);
        GCHECK(SamePath(w.rootPath, innerTwo));
    });
    MCGuiStep(@"Cmd+Return opens the folder the text names", ^{
        [w openFolder:nil];
        Type(w, [root stringByAppendingString:@"/beta"]);
    });
    MCGuiWait(@"beta offered as typed", 5, ^BOOL {
        return [Labels(w) containsObject:@"Open This Folder"];
    });
    MCGuiStep(@"Cmd+Return, through the window's keys, ahead of the menu", ^{
        GCHECK(MCGuiPerformMenuKey(w.window, 36, NSEventModifierFlagCommand, nil, nil));
        GCHECK(Picker(w) == nil);
    });
    MCGuiWait(@"the window on beta", 5, ^BOOL { return SamePath(w.rootPath, beta); });
    MCGuiStep(@"a folder of 3,000 subfolders", ^{
        [w openFolder:nil];
        Type(w, [many stringByAppendingString:@"/"]);
    });
    MCGuiWait(@"its subfolders listed", 10, ^BOOL {
        return Lists(Labels(w), [many stringByAppendingPathComponent:@"d2999"]);
    });
    MCGuiStep(@"typing filters it quickly", ^{
        const double c0 = MCGuiThreadCpuMs();
        Type(w, @"d29");
        Type(w, @"9");
        GCHECK_TIME(MCGuiThreadCpuMs() - c0, 150, @"two keys filtering 3,000 folders");
        GCHECK(SamePath(Selected(w), [many stringByAppendingPathComponent:@"d299"]));
        Command(w, @selector(cancelOperation:));
    });
    __block NSUInteger windowsBefore = 0;
    MCGuiStep(@"the Dock's Open Folder… shows the same picker over the window in front", ^{
        windowsBefore = MCGuiControllers().count;
        MCDockFolderChooser = nil;
        [NSApp sendAction:@selector(openFolderInNewWindow:) to:NSApp.delegate from:nil];
        GCHECK(Picker(w) != nil);
        Type(w, [root stringByAppendingString:@"/alpha/inner-one/"]);
    });
    MCGuiWait(@"inner-one read", 5, ^BOOL { return [Selected(w) isEqual:@"Open This Folder"]; });
    __block EditorController *docked = nil;
    MCGuiStep(@"Return opens a new window there; the window in front keeps its folder", ^{
        Command(w, @selector(insertNewline:));
    });
    MCGuiWait(@"a window on inner-one", 5, ^BOOL {
        for (EditorController *e in MCGuiControllers())
            if (SamePath(e.rootPath, innerOne)) docked = e;
        return docked != nil;
    });
    MCGuiStep(@"close the suite's windows", ^{
        GCHECK(docked != w);
        GCHECK(MCGuiControllers().count == windowsBefore + 1);
        GCHECK(SamePath(w.rootPath, beta));
        [docked.window close];
        [w.window close];
        docked = nil;
        w = nil;
        MCGuiSetKeyWindow(nil);
    });
}
