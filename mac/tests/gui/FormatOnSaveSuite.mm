// FormatOnSaveSuite.mm — `make guitest SUITES=format_on_save`: format on
// save without hunting for its line in the settings file (FormatOnSave.h).
// Cmd+S in a project whose pyproject.toml sets up ruff, with format.on-save
// off, saves the file as typed and then offers format on save in a bar
// naming ruff and pyproject.toml; Turn On writes format.on-save = project
// and checks Edit > Format on Save; the next Cmd+S formats with ruff first.
// A file in a folder that sets up no formatter is saved as typed (project
// mode never runs Google's style). The menu item turns it off and on; the
// offer comes once a launch, and Don't Ask Again stops it for good.
#import "Harness.h"
#import "AppSettings.h"
#import "EditorController.h"
#import "EditorController+Private.h"   // closeTab:prompt:
#import "FormatOnSave.h"
#import "InstallPanel.h"
#include <sys/stat.h>

namespace {
NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
NSString *Disk(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}
void Script(NSString *path, NSString *body) {
    MCGuiWriteFile(path, body);
    chmod(path.fileSystemRepresentation, 0755);
}
NSString *SettingsText() {
    return Disk(NSProcessInfo.processInfo.environment[@"MINICODE_SETTINGS"]) ?: @"";
}
NSMenuItem *EditMenuItem(NSString *title) {
    for (NSMenuItem *top in NSApp.mainMenu.itemArray)
        if ([top.submenu.title isEqualToString:@"Edit"] || [top.title isEqualToString:@"Edit"])
            for (NSMenuItem *item in top.submenu.itemArray)
                if ([item.title isEqualToString:title]) return item;
    return nil;
}
BOOL Checked(NSMenuItem *item) {
    return [(id)item.target validateMenuItem:item] && item.state == NSControlStateValueOn;
}
MCFormatOnSaveOffer *Offer(EditorController *c) { return [c valueForKey:@"formatOnSaveOffer"]; }
BOOL BarShows(EditorController *c) {
    MCInstallBar *bar = Offer(c).bar;
    return bar && !bar.hidden && NSHeight(bar.frame) > 0;
}
void Append(EditorController *c, NSString *text) {
    [Text(c) insertText:text replacementRange:NSMakeRange(Text(c).string.length, 0)];
}
}  // namespace

MC_GUI_SUITE(format_on_save) {
    __block EditorController *c = nil;
    NSString *dir = MCGuiProjectFile(@"fos");
    NSString *file = [dir stringByAppendingPathComponent:@"a.py"];
    NSString *plainDir = MCGuiProjectFile(@"fosplain");
    NSString *plain = [plainDir stringByAppendingPathComponent:@"b.py"];
    NSString *bin = [NSHomeDirectory() stringByAppendingPathComponent:@".local/bin"];
    NSString *ruff = [bin stringByAppendingPathComponent:@"ruff"];
    NSString *pyink = [bin stringByAppendingPathComponent:@"pyink"];
    NSString *yapf = [bin stringByAppendingPathComponent:@"yapf"];
    NSString *ranGoogle = [MCGuiScratch() stringByAppendingPathComponent:@"fos-google-ran"];
    NSFileManager *fm = [NSFileManager defaultManager];

    MCGuiStep(@"pyproject.toml sets up ruff, ruff installed, format.on-save off; Cmd+S", ^{
        MCInstallBarSetMuted(MCFormatOnSaveOfferKey, NO);
        [MCFormatOnSaveOffer resetOfferedForTesting];
        MCGuiWriteFile([dir stringByAppendingPathComponent:@"pyproject.toml"],
                       @"[tool.ruff.format]\nquote-style = \"single\"\n");
        MCGuiWriteFile(file, @"x=1\n");
        MCGuiWriteFile(plain, @"x=1\n");
        // Reads the buffer on stdin, as ruff does; a real ruff formats
        // these lines the same way.
        Script(ruff, @"#!/bin/sh\nexec /usr/bin/sed -E 's/^x=1$/x = 1/'\n");
        // Google's default for a folder with nothing set up, which project
        // mode must never run.
        NSString *google = [NSString stringWithFormat:
            @"#!/bin/sh\n/usr/bin/touch '%@'\nexec /usr/bin/sed -E 's/^x=1$/x = 1/'\n", ranGoogle];
        Script(pyink, google);
        Script(yapf, google);
        MCGuiSetSettings(@"lsp.enabled = false\n");
        c = MCGuiController();
        GCHECK([c showFile:file]);
        GCHECK(!Checked(EditMenuItem(@"Format on Save")));
        Append(c, @"z = 3\n");
        [c saveCurrentFile:c];
    });
    MCGuiWait(@"the offer", 15, ^BOOL { return BarShows(c); });
    MCGuiStep(@"saved as typed; the bar names ruff and pyproject.toml; Turn On", ^{
        GCHECK_EQ_STR(Disk(file), @"x=1\nz = 3\n");
        MCGuiNote(@"the offer: %@", Offer(c).bar.text);
        GCHECK_EQ_STR(Offer(c).bar.text,
                      @"This project formats Python files with ruff, set up in pyproject.toml. "
                       "Format them each time you save?");
        GCHECK_EQ_STR(Offer(c).bar.installButton.title, @"Turn On");
        [Offer(c).bar.installButton performClick:nil];
        GCHECK([SettingsText() containsString:@"format.on-save = project"]);
        GCHECK([AppSettings shared].settings.formatOnSaveMode() == Settings::FormatOnSave::Project);
        GCHECK(!BarShows(c));
        GCHECK(Checked(EditMenuItem(@"Format on Save")));
        // Nothing was formatted by the answer itself.
        GCHECK_EQ_STR(Text(c).string, @"x=1\nz = 3\n");
        Append(c, @"w = 4\n");
        [c saveCurrentFile:c];
    });
    MCGuiWait(@"Cmd+S formats with ruff, then saves", 15, ^BOOL {
        return [Disk(file) isEqualToString:@"x = 1\nz = 3\nw = 4\n"];
    });
    MCGuiStep(@"one undo step; a folder with no formatter set up: Cmd+S", ^{
        GCHECK_EQ_STR(Text(c).string, @"x = 1\nz = 3\nw = 4\n");
        GCHECK([c showFile:plain]);
        Append(c, @"y = 2\n");
        [c saveCurrentFile:c];
    });
    MCGuiWait(@"saved", 15, ^BOOL { return [Disk(plain) isEqualToString:@"x=1\ny = 2\n"]; });
    MCGuiStep(@"as typed, Google's style never ran; the menu turns it off and on", ^{
        GCHECK(![fm fileExistsAtPath:ranGoogle]);
        GCHECK_EQ_STR(Text(c).string, @"x=1\ny = 2\n");
        NSMenuItem *item = EditMenuItem(@"Format on Save");
        GCHECK(item != nil);
        if (!item) return;
        GCHECK([NSApp sendAction:item.action to:item.target from:item]);
        GCHECK([SettingsText() containsString:@"format.on-save = false"]);
        GCHECK(!Checked(item));
        GCHECK([NSApp sendAction:item.action to:item.target from:item]);
        GCHECK([SettingsText() containsString:@"format.on-save = project"]);
        GCHECK(Checked(item));
        // format.on-save = true checks it too.
        MCGuiSetSettings(@"lsp.enabled = false\nformat.on-save = true\n");
        GCHECK(Checked(item));
        // Off again: the offer was made this launch, so Cmd+S says nothing.
        MCGuiSetSettings(@"lsp.enabled = false\n");
        GCHECK([c showFile:file]);
        [c saveCurrentFile:c];
    });
    MCGuiPause(1.0);   // a negative check: the offer must not come back
    MCGuiStep(@"not offered twice a launch; next launch, Don't Ask Again", ^{
        GCHECK(!BarShows(c));
        [MCFormatOnSaveOffer resetOfferedForTesting];   // as after a restart
        [c saveCurrentFile:c];
    });
    MCGuiWait(@"offered again", 15, ^BOOL { return BarShows(c); });
    MCGuiStep(@"Don't Ask Again: gone, and kept", ^{
        Offer(c).bar.onMute();
        GCHECK(!BarShows(c));
        GCHECK(MCInstallBarMuted(MCFormatOnSaveOfferKey));
        GCHECK([AppSettings shared].settings.formatOnSaveMode() == Settings::FormatOnSave::Off);
        [MCFormatOnSaveOffer resetOfferedForTesting];
        [c saveCurrentFile:c];
    });
    MCGuiPause(1.0);   // a negative check: muted, so no offer
    MCGuiStep(@"never offered again; put everything back", ^{
        GCHECK(!BarShows(c));
        MCInstallBarSetMuted(MCFormatOnSaveOfferKey, NO);
        [MCFormatOnSaveOffer resetOfferedForTesting];
        // The tabs go before their files, so later suites see no deleted tab.
        for (NSString *p in @[file, plain]) {
            GCHECK([c showFile:p]);
            GCHECK([c closeTab:[c valueForKey:@"activeTab"] prompt:NO]);
        }
        for (NSString *p in @[dir, plainDir, ruff, pyink, yapf, ranGoogle])
            [fm removeItemAtPath:p error:nil];
        MCGuiSetSettings(@"lsp.enabled = false\n");
    });
}
