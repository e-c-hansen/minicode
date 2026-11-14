// ConfigWritesSuite.mm — `make guitest SUITES=config_writes`: minicode's own
// writes to settings.conf and keys.conf (D4). With each file a symlink into
// a dotfiles folder, View > Theme, the Claude Code IDE link toggle and
// Keyboard Shortcuts' first write change the file the link names and leave
// the link a link (a dangling one too); with settings.conf open in a tab
// with unsaved changes, a theme is refused, the reason shows in the status
// bar, and neither the tab nor the disk changes.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "AppSettings.h"
#import "Appearance.h"
#import "IdeLinkServer.h"
#import "Keymap.h"
#include <sys/stat.h>

namespace {

NSTextView *TV(EditorController *c) { return [c valueForKey:@"textView"]; }
MCTab *Active(EditorController *c) { return [c valueForKey:@"activeTab"]; }
NSString *Disk(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}
BOOL IsLink(NSString *path) {
    struct stat st;
    return lstat(path.fileSystemRepresentation, &st) == 0 && S_ISLNK(st.st_mode);
}
NSString *StatusText(EditorController *c) {
    NSTextField *f = [c valueForKey:@"settingsLabel"];
    return f.stringValue ?: @"";
}
NSString *Dots(NSString *name) {
    return [[MCGuiScratch() stringByAppendingPathComponent:@"dots"] stringByAppendingPathComponent:name];
}

}  // namespace

MC_GUI_SUITE(config_writes) {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *settings = [AppSettings shared].path;
    NSString *keys = [MCKeymap shared].path;
    __block NSString *before = nil;
    __block EditorController *c = nil;
    __block NSString *configBefore = nil;

    MCGuiStep(@"settings.conf is a symlink into a dotfiles folder", ^{
        c = MCGuiController();
        before = Disk(settings) ?: @"lsp.enabled = false\n";
        MCGuiWriteFile(Dots(@"settings.conf"), before);
        [fm removeItemAtPath:settings error:nil];
        GCHECK([fm createSymbolicLinkAtPath:settings withDestinationPath:Dots(@"settings.conf") error:nil]);
        [[AppSettings shared] reload];
        GCHECK(IsLink(settings));
    });
    MCGuiStep(@"View > Theme writes the dotfiles copy and keeps the link", ^{
        // Keep any colors of the file's own; the question comes only then,
        // and an answer left queued would go to a later suite's alert.
        if (Settings::colorOverrides([AppSettings shared].fileText.UTF8String ?: "") > 0)
            MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);
        GCHECK(MCChooseTheme(@"monokai"));
        GCHECK(IsLink(settings));
        GCHECK([Disk(Dots(@"settings.conf")) containsString:@"theme = monokai"]);
        GCHECK([[AppSettings shared].fileText containsString:@"theme = monokai"]);
        // No temporary left beside either.
        for (NSString *dir in @[Dots(@""), settings.stringByDeletingLastPathComponent])
            for (NSString *name in [fm contentsOfDirectoryAtPath:dir error:nil])
                GCHECK(![name containsString:@".minicode-"]);
    });
    MCGuiStep(@"the IDE link toggle too, on and off", ^{
        // With no Claude Code folder, so the link never starts: the idelink
        // suite needs its first start (which clears dead lock files) itself.
        const char *config = getenv("CLAUDE_CONFIG_DIR");
        configBefore = config ? @(config) : nil;
        setenv("CLAUDE_CONFIG_DIR", Dots(@"no-claude").fileSystemRepresentation, 1);
        NSMenuItem *item = MCIdeLinkMenuItem();
        const BOOL was = [AppSettings shared].settings.ideLink();
        GCHECK([NSApp sendAction:item.action to:item.target from:item]);
        GCHECK(IsLink(settings));
        GCHECK([AppSettings shared].settings.ideLink() == !was);
        GCHECK([NSApp sendAction:item.action to:item.target from:item]);
        GCHECK(IsLink(settings));
        GCHECK([AppSettings shared].settings.ideLink() == was);
        GCHECK([Disk(Dots(@"settings.conf")) containsString:@"agent.ide-link = "]);
    });

    // ------------------------------------------- a tab with unsaved changes
    __block NSString *disk = nil;
    MCGuiStep(@"with settings.conf open and edited, a theme is refused", ^{
        [c focusEditor:nil];
        GCHECK([c showFile:settings]);
        GCHECK(TV(c).editable);
        NSUndoManager *u = TV(c).undoManager;
        if (u.groupsByEvent && u.groupingLevel > 0) [u endUndoGrouping];
        MCGuiUndoGroup(u, ^{
            [TV(c) insertText:@"# mine, not saved\n" replacementRange:NSMakeRange(0, 0)];
        });
        GCHECK(c.dirty);
        disk = Disk(Dots(@"settings.conf"));
        GCHECK(!MCChooseTheme(@"solarized-light"));
        GCHECK_EQ_STR(Disk(Dots(@"settings.conf")), disk);
        GCHECK(IsLink(settings));
        GCHECK([TV(c).string hasPrefix:@"# mine, not saved\n"] && c.dirty);
        GCHECK([StatusText(c) containsString:@"unsaved changes"]);
        MCGuiNote(@"status: %@", StatusText(c));
        // The IDE link toggle is refused the same way.
        NSMenuItem *item = MCIdeLinkMenuItem();
        const BOOL was = [AppSettings shared].settings.ideLink();
        [NSApp sendAction:item.action to:item.target from:item];
        GCHECK([AppSettings shared].settings.ideLink() == was);
        GCHECK_EQ_STR(Disk(Dots(@"settings.conf")), disk);
        if (configBefore) setenv("CLAUDE_CONFIG_DIR", configBefore.fileSystemRepresentation, 1);
        else unsetenv("CLAUDE_CONFIG_DIR");
    });
    MCGuiStep(@"once the edit is undone, the theme is written", ^{
        NSUndoManager *u = TV(c).undoManager;
        if (u.groupsByEvent && u.groupingLevel > 0) [u endUndoGrouping];
        [u undo];
        GCHECK(!c.dirty);
        GCHECK(MCChooseTheme(@"dark-plus"));
        GCHECK([Disk(Dots(@"settings.conf")) containsString:@"theme = dark-plus"]);
        GCHECK(IsLink(settings));
        [c closeTab:Active(c) prompt:NO];
    });

    // ------------------------------------------------------------ keys.conf
    MCGuiStep(@"Keyboard Shortcuts writes keys.conf through a dangling symlink", ^{
        [fm removeItemAtPath:keys error:nil];
        [fm createDirectoryAtPath:keys.stringByDeletingLastPathComponent withIntermediateDirectories:YES
                       attributes:nil error:nil];
        [fm removeItemAtPath:Dots(@"keys.conf") error:nil];
        GCHECK([fm createSymbolicLinkAtPath:keys withDestinationPath:Dots(@"keys.conf") error:nil]);
        [c openKeyboardShortcuts:nil];
        GCHECK(IsLink(keys));
        NSString *text = Disk(Dots(@"keys.conf"));
        GCHECK(text.length > 100 && [text containsString:@"# "]);
        [c closeTab:Active(c) prompt:NO];
    });
    MCGuiStep(@"put the plain files back", ^{
        [fm removeItemAtPath:keys error:nil];
        [fm removeItemAtPath:Dots(@"keys.conf") error:nil];
        [[MCKeymap shared] reload];
        [fm removeItemAtPath:settings error:nil];
        MCGuiWriteFile(settings, before);
        [[AppSettings shared] reload];
        GCHECK(!IsLink(settings) && [Disk(settings) isEqualToString:before]);
    });
}
