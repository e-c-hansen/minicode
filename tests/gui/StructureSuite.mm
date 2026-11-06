// StructureSuite.mm — `make guitest SUITES=structure`: the structure and
// build hygiene fixes of review 3's P8. A window on a folder of its own
// watches it with FSEvents while open and stops when it closes (the
// stream's context holds an unretained controller, which a closed window
// kept receiving events through until it was deallocated).
#import <objc/runtime.h>

#import "Harness.h"
#import "EditorController.h"

namespace {
// The controller's FSEvents stream, read through the runtime (it is a
// plain pointer ivar, which KVC cannot read).
void *FsStream(EditorController *c) {
    Ivar iv = class_getInstanceVariable([EditorController class], "_fsStream");
    if (!iv) return (void *)-1;
    return *(void **)((char *)(__bridge void *)c + ivar_getOffset(iv));
}
}  // namespace

MC_GUI_SUITE(structure) {
    __block EditorController *c = nil;
    NSString *dir = [MCGuiScratch() stringByAppendingPathComponent:@"structure"];

    MCGuiStep(@"a window on a folder of its own watches it", ^{
        MCGuiWriteFile([dir stringByAppendingPathComponent:@"a.txt"], @"a\n");
        NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:dir]]];
        GCHECK(MCGuiControllers().count == before + 1);
        c = MCGuiController();
        GCHECK(FsStream(c) != NULL && FsStream(c) != (void *)-1);
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
    // bundle's copy finds its own in Contents/Resources), into the scratch
    // home this process runs with.
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
