// ConfigWritesSuite.mm — `make guitest SUITES=config_writes`: mutible's own
// writes to settings.conf and keys.conf (D4). With each file a symlink into
// a dotfiles folder, View > Theme, the Claude Code IDE link toggle and
// Keyboard Shortcuts' first write change the file the link names and leave
// the link a link (a dangling one too); with settings.conf open in a tab
// with unsaved changes, a theme is refused, the reason shows in the status
// bar, and neither the tab nor the disk changes. The theme picker
// (Ctrl+Cmd+T): each row the list comes to shows in the window, by the
// arrow keys or the pointer, within a bound of main-thread CPU; Esc puts
// the theme back and leaves the file byte for byte; Return writes the
// theme line and keeps the file's comments.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "AppSettings.h"
#import "Appearance.h"
#import "IdeLinkServer.h"
#import "Keymap.h"
#import "EditorPanes.h"
#import "PalettePanel.h"
#include "ThemePicker.h"
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
NSView *Palette(EditorController *c) { return MCCommandPaletteIn(c.window); }
void PaletteCommand(EditorController *c, SEL sel) {
    [(NSTextView *)MCCommandPaletteField(Palette(c)).currentEditor doCommandBySelector:sel];
}
// Whether a pane's color is the theme's background for that surface, as
// drawn (opacity aside).
BOOL IsThemeBackground(NSColor *color, const std::string &themeId, Surface surface) {
    const Theme *t = Settings::findTheme(themeId);
    NSColor *got = [color colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    if (!t || !got) return NO;
    const Rgba want = Rgba::hex(t->background[(int)surface]);
    const BOOL same = fabs(got.redComponent * 255 - want.r) < 1.5 &&
                      fabs(got.greenComponent * 255 - want.g) < 1.5 &&
                      fabs(got.blueComponent * 255 - want.b) < 1.5;
    if (!same)
        MCGuiNote(@"%s: wanted #%06X, the pane has %@", themeId.c_str(),
                  t->background[(int)surface], got);
    return same;
}
NSColor *EditorBackground(EditorController *c) {
    return ((PanelScrollView *)[c valueForKey:@"editorScroll"]).panelColor;
}
NSColor *TreeBackground(EditorController *c) {
    return ((PanelScrollView *)[c valueForKey:@"sidebarScroll"]).panelColor;
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
                GCHECK(![name containsString:@".mutible-"]);
    });
    MCGuiStep(@"the IDE link toggle too, on and off", ^{
        // With no Claude Code folder, so the link never starts: the idelink
        // suite needs its first start (which clears dead lock files) itself.
        const char *config = getenv("CLAUDE_CONFIG_DIR");
        configBefore = config ? @(config) : nil;
        MCSetEnv("CLAUDE_CONFIG_DIR", Dots(@"no-claude").fileSystemRepresentation);
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
        if (configBefore) MCSetEnv("CLAUDE_CONFIG_DIR", configBefore.fileSystemRepresentation);
        else MCSetEnv("CLAUDE_CONFIG_DIR", nullptr);
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

    // ------------------------------------------------------ the theme picker
    __block NSString *pickerFile = nil;
    __block std::string setId, thirdId, lightId;
    __block NSInteger start = 0;
    MCGuiStep(@"Ctrl+Cmd+T opens the theme picker on the theme in use", ^{
        // A window with several tabs, a highlighted one on show.
        NSMutableString *py = [NSMutableString string];
        for (int i = 0; i < 400; i++)
            [py appendFormat:@"def f%d(x):\n    # a comment\n    return x + %d  # \"s\"\n", i, i];
        MCGuiWriteFile(MCGuiProjectFile(@"src/picker.py"), py);
        [c focusEditor:nil];
        GCHECK([c showFile:MCGuiProjectFile(@"README.md")]);
        GCHECK([c showFile:MCGuiProjectFile(@"big.txt")]);
        GCHECK([c showFile:MCGuiProjectFile(@"src/picker.py")]);
        pickerFile = [@"# the theme picker keeps this comment\n" stringByAppendingString:before];
        MCGuiWriteFile(settings, pickerFile);
        [[AppSettings shared] reload];
        setId = [AppSettings shared].settings.theme().id;
        const std::vector<ThemePicker::Row> rows = ThemePicker::rows();
        start = ThemePicker::rowOf(rows, setId);
        GCHECK(start >= 0);
        thirdId = rows[(size_t)((start + 2) % (NSInteger)rows.size())].id;
        for (const ThemePicker::Row &r : rows)
            if (!r.dark && r.id != thirdId && r.id != setId) { lightId = r.id; break; }
        [c.window makeFirstResponder:TV(c)];
        GCHECK(MCGuiPerformMenuKey(c.window, 17, NSEventModifierFlagControl | NSEventModifierFlagCommand,
                                   nil, nil));
        GCHECK(Palette(c) != nil);
        NSArray<NSString *> *labels = MCCommandPaletteLabels(Palette(c));
        GCHECK(labels.count == rows.size());
        for (NSUInteger i = 0; i < MIN(labels.count, rows.size()); i++)
            GCHECK_EQ_STR(labels[i], @(rows[i].title.c_str()));
        GCHECK(MCCommandPaletteSelection(Palette(c)) == start);
        GCHECK_EQ_STR(MCCommandPaletteKeys(Palette(c)).firstObject, @"dark themes");
        GCHECK(IsThemeBackground(EditorBackground(c), setId, Surface::Editor));
    });
    MCGuiStep(@"down twice shows the third theme, each step within its bound", ^{
        double worst = 0;
        for (int i = 0; i < 2; i++) {
            const double c0 = MCGuiThreadCpuMs();
            PaletteCommand(c, @selector(moveDown:));
            worst = MAX(worst, MCGuiThreadCpuMs() - c0);
        }
        GCHECK_TIME(worst, 100, @"a theme preview step (main-thread CPU)");
        GCHECK(MCCommandPaletteSelection(Palette(c)) ==
               (start + 2) % (NSInteger)ThemePicker::rows().size());
        GCHECK([[AppSettings shared].previewedTheme isEqualToString:@(thirdId.c_str())]);
        GCHECK(IsThemeBackground(EditorBackground(c), thirdId, Surface::Editor));
        GCHECK(IsThemeBackground(TreeBackground(c), thirdId, Surface::Sidebar));
        GCHECK_EQ_STR(Disk(settings), pickerFile);
    });
    MCGuiStep(@"the pointer over a light theme shows it, controls and all", ^{
        const std::vector<ThemePicker::Row> rows = ThemePicker::rows();
        const int row = ThemePicker::rowOf(rows, lightId);
        GCHECK(row >= 0);
        MCCommandPaletteHoverRow(Palette(c), row);
        GCHECK(MCCommandPaletteSelection(Palette(c)) == row);
        GCHECK(IsThemeBackground(EditorBackground(c), lightId, Surface::Editor));
        GCHECK([c.window.appearance.name isEqualToString:NSAppearanceNameAqua]);
    });
    MCGuiStep(@"Esc puts the theme back and leaves the file byte for byte", ^{
        PaletteCommand(c, @selector(cancelOperation:));
        GCHECK(Palette(c) == nil);
        GCHECK([AppSettings shared].previewedTheme == nil);
        GCHECK(std::string([AppSettings shared].settings.theme().id) == setId);
        GCHECK(IsThemeBackground(EditorBackground(c), setId, Surface::Editor));
        GCHECK(IsThemeBackground(TreeBackground(c), setId, Surface::Sidebar));
        GCHECK_EQ_STR(Disk(settings), pickerFile);
    });
    MCGuiStep(@"View > Theme > Choose Theme…: down twice and Return", ^{
        if (Settings::colorOverrides(pickerFile.UTF8String ?: "") > 0)
            MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);
        NSMenu *view = [NSApp.mainMenu itemWithTitle:@"View"].submenu;
        NSMenuItem *choose = [[view itemWithTitle:@"Theme"].submenu itemWithTitle:@"Choose Theme…"];
        GCHECK(choose != nil);
        if (!choose) return;
        [choose.menu update];
        GCHECK(choose.isEnabled);
        GCHECK([NSApp sendAction:choose.action to:choose.target from:choose]);
        GCHECK(Palette(c) != nil);
        if (!Palette(c)) return;
        GCHECK(MCCommandPaletteSelection(Palette(c)) == start);
        PaletteCommand(c, @selector(moveDown:));
        PaletteCommand(c, @selector(moveDown:));
        PaletteCommand(c, @selector(insertNewline:));
        GCHECK(Palette(c) == nil);
    });
    MCGuiWait(@"the theme written", 5, ^BOOL {
        return [Disk(settings) containsString:[NSString stringWithFormat:@"theme = %s", thirdId.c_str()]];
    });
    MCGuiStep(@"the file has the theme line, its comments kept, and the theme shows", ^{
        const std::string want = Settings::withTheme(pickerFile.UTF8String ?: "", thirdId, false);
        GCHECK_EQ_STR(Disk(settings), @(want.c_str()));
        GCHECK([Disk(settings) hasPrefix:@"# the theme picker keeps this comment\n"]);
        GCHECK([AppSettings shared].previewedTheme == nil);
        GCHECK(std::string([AppSettings shared].settings.theme().id) == thirdId);
        GCHECK(IsThemeBackground(EditorBackground(c), thirdId, Surface::Editor));
    });
    MCGuiStep(@"put the settings back", ^{
        for (int i = 0; i < 3; i++) [c closeTab:Active(c) prompt:NO];
        MCGuiWriteFile(settings, before);
        [[AppSettings shared] reload];
        GCHECK([Disk(settings) isEqualToString:before]);
    });
}
