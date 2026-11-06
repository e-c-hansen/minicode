// StructureSuite.mm — `make guitest SUITES=structure`: the structure and
// build hygiene fixes of review 3's P8.
//   - A window on a folder of its own watches it with FSEvents while open
//     and stops when it closes (the stream's context holds an unretained
//     controller, which a closed window kept receiving events through until
//     it was deallocated).
//   - The file tree's F2 and Cmd+Delete are the registry's renameFile and
//     moveFileToTrash: they act in the tree only, never from the editor,
//     and keys.conf moves them (F2 used to stay the tree's whatever
//     keys.conf said).
//   - `minicode install-skill` copies the skill into ~/.claude/skills.
#import <objc/runtime.h>
#import <Carbon/Carbon.h>

#import "Harness.h"
#import "EditorController.h"
#import "Keymap.h"

namespace {
// The controller's FSEvents stream, read through the runtime (it is a
// plain pointer ivar, which KVC cannot read).
void *FsStream(EditorController *c) {
    Ivar iv = class_getInstanceVariable([EditorController class], "_fsStream");
    if (!iv) return (void *)-1;
    return *(void **)((char *)(__bridge void *)c + ivar_getOffset(iv));
}
NSString *F2() { return [NSString stringWithFormat:@"%C", (unichar)NSF2FunctionKey]; }
void PressF2(EditorController *c) {
    MCGuiSendKeyToApp(c.window, kVK_F2, NSEventModifierFlagFunction, F2(), F2());
}
void PressCmdDelete(EditorController *c) {
    MCGuiSendKeyToApp(c.window, kVK_Delete, NSEventModifierFlagCommand, @"\x7f", @"\x7f");
}
// The alerts shown since `before` (their message texts).
NSArray<NSString *> *AlertsSince(NSUInteger before) {
    NSArray<NSString *> *all = MCGuiAlertsShown();
    return before < all.count ? [all subarrayWithRange:NSMakeRange(before, all.count - before)]
                              : @[];
}
void WriteKeys(NSString *text) {
    MCGuiWriteFile([MCKeymap shared].path, text);
    [[MCKeymap shared] reload];
}
NSMenuItem *FindItem(NSMenu *menu, SEL action) {
    for (NSMenuItem *it in menu.itemArray) {
        if (it.submenu) {
            NSMenuItem *found = FindItem(it.submenu, action);
            if (found) return found;
        }
        if (it.action == action && !it.isHidden) return it;
    }
    return nil;
}
}  // namespace

MC_GUI_SUITE(structure) {
    __block EditorController *c = nil;
    NSString *dir = [MCGuiScratch() stringByAppendingPathComponent:@"structure"];
    NSString *file = [dir stringByAppendingPathComponent:@"a.txt"];
    __block NSUInteger alerts = 0;

    MCGuiStep(@"a window on a folder of its own watches it", ^{
        MCGuiWriteFile(file, @"a\n");
        NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:dir]]];
        GCHECK(MCGuiControllers().count == before + 1);
        c = MCGuiController();
        GCHECK(FsStream(c) != NULL && FsStream(c) != (void *)-1);
    });

    MCGuiStep(@"F2 and Cmd+Delete in the tree ask to rename and to trash", ^{
        [c revealPath:file.stringByResolvingSymlinksInPath andOpen:NO];
        NSOutlineView *tree = [c valueForKey:@"outline"];
        GCHECK(tree.selectedRow >= 0);
        [c.window makeFirstResponder:tree];
        alerts = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn), @(NSAlertSecondButtonReturn)]);
        PressF2(c);
        PressCmdDelete(c);
        NSArray *shown = AlertsSince(alerts);
        MCGuiNote(@"alerts: %@", shown);
        GCHECK(shown.count == 2);
        GCHECK(shown.count == 2 && [shown[0] isEqualToString:@"Rename to:"]);
        GCHECK(shown.count == 2 && [shown[1] containsString:@"to the Trash?"]);
        GCHECK([[NSFileManager defaultManager] fileExistsAtPath:file]);
        // The menu items carry no key: a menu key would act from every pane.
        GCHECK_EQ_STR(FindItem(NSApp.mainMenu, @selector(renameSelected:)).keyEquivalent, @"");
        GCHECK_EQ_STR(FindItem(NSApp.mainMenu, @selector(deleteSelected:)).keyEquivalent, @"");
    });
    MCGuiStep(@"in the editor neither touches the tree's file", ^{
        GCHECK([c showFile:file]);
        [c.window makeFirstResponder:[c valueForKey:@"textView"]];
        alerts = MCGuiAlertsShown().count;
        PressF2(c);
        PressCmdDelete(c);
        NSArray *shown = AlertsSince(alerts);
        GCHECK(![shown containsObject:@"Rename to:"]);
        for (NSString *s in shown) GCHECK(![s containsString:@"Trash"]);
    });
    MCGuiStep(@"keys.conf moves the tree's rename off F2", ^{
        WriteKeys(@"f2 = -renameFile\nctrl+alt+cmd+r = renameFile\n");
        GCHECK([MCKeymap shared].errors.count == 0);
        NSOutlineView *tree = [c valueForKey:@"outline"];
        [c revealPath:file.stringByResolvingSymlinksInPath andOpen:NO];
        [c.window makeFirstResponder:tree];
        alerts = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);
        PressF2(c);
        GCHECK(![AlertsSince(alerts) containsObject:@"Rename to:"]);
        MCGuiSendKeyToApp(c.window, kVK_ANSI_R,
                          NSEventModifierFlagControl | NSEventModifierFlagOption |
                              NSEventModifierFlagCommand,
                          @"\x12", @"r");
        GCHECK([AlertsSince(alerts) containsObject:@"Rename to:"]);
        GCHECK_EQ_STR(FindItem(NSApp.mainMenu, @selector(renameSelected:)).keyEquivalent, @"");
        WriteKeys(@"");
        GCHECK([MCKeymap shared].errors.count == 0);
    });

    MCGuiStep(@"closing it stops the stream, while the controller still lives", ^{
        [c.window close];
        GCHECK(FsStream(c) == NULL);
    });
    MCGuiWait(@"the window gone", 5, ^BOOL { return ![MCGuiControllers() containsObject:c]; });
    MCGuiStep(@"done with the window", ^{
        c = nil;
        MCGuiSetKeyWindow(nil);
    });

    // `minicode install-skill`, run from the checkout's scripts/ (the
    // bundle's copy finds its own in Contents/Resources), into a scratch home.
    NSString *repo = MCGuiBuildDir().stringByDeletingLastPathComponent;
    NSString *launcher = [repo stringByAppendingPathComponent:@"scripts/minicode-launcher"];
    NSString *home = [MCGuiScratch() stringByAppendingPathComponent:@"skillhome"];
    NSString *installed = [home stringByAppendingPathComponent:@".claude/skills/minicode/SKILL.md"];
    NSDictionary *env = @{@"HOME": home, @"CLAUDE_CONFIG_DIR": @""};
    MCGuiAsync(@"install-skill copies the skill into ~/.claude/skills", 20, ^(MCGuiDone done) {
        GCHECK([home hasPrefix:MCGuiScratch()]);
        MCGuiRunProcess(@[@"/bin/bash", launcher, @"install-skill"], env,
                        ^(int s, NSString *out, NSString *err) {
            GCHECK(s == 0);
            if (s) MCGuiNote(@"install-skill: %@%@", out, err);
            GCHECK([out containsString:@"Installed the minicode skill"]);
            NSData *a = [NSData dataWithContentsOfFile:installed];
            NSData *b = [NSData dataWithContentsOfFile:
                [repo stringByAppendingPathComponent:@"skills/minicode/SKILL.md"]];
            GCHECK(a.length > 0 && [a isEqualToData:b]);
            done();
        });
    });
    MCGuiAsync(@"again: up to date", 20, ^(MCGuiDone done) {
        MCGuiRunProcess(@[@"/bin/bash", launcher, @"install-skill"], env,
                        ^(int s, NSString *out, NSString *) {
            GCHECK(s == 0 && [out containsString:@"up to date"]);
            done();
        });
    });
}
