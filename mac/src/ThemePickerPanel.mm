// ThemePickerPanel.mm — see ThemePickerPanel.h.
#import "ThemePickerPanel.h"
#import "AppSettings.h"
#import "Appearance.h"
#import "PalettePanel.h"
#include "ThemePicker.h"

#include <memory>

void MCShowThemePicker(NSWindow *window) {
    if (!window.contentView) return;
    AppSettings *cfg = [AppSettings shared];
    // A preview left by a picker in another window ends first: the set
    // theme is the file's.
    [cfg previewTheme:nil];
    auto session = std::make_shared<ThemePicker::Session>(cfg.settings.theme().id);
    NSMutableArray<NSString *> *labels = [NSMutableArray array];
    NSMutableArray<NSString *> *details = [NSMutableArray array];
    for (const ThemePicker::Row &r : session->rows()) {
        [labels addObject:@(r.title.c_str())];
        [details addObject:@(r.group.c_str())];
    }
    MCPalettePick *pick = [MCPalettePick new];
    pick.placeholder = @"Select a color theme (↑ ↓ to preview, ⏎ to keep, ⎋ to go back)";
    pick.labels = labels;
    pick.details = details;
    pick.emptyText = @"No matching themes";
    pick.initialIndex = session->initialRow();
    pick.preview = ^(NSInteger index) {
        const std::string id = session->preview((int)index);
        if (!id.empty()) [[AppSettings shared] previewTheme:@(id.c_str())];
    };
    pick.cancel = ^{
        session->cancel();
        [[AppSettings shared] previewTheme:nil];
    };
    pick.choose = ^(NSInteger index) {
        const std::string id = session->choose((int)index);
        // Written while the preview still shows it, so the windows redraw
        // once; refused or cancelled (an unsaved settings tab, Cancel in
        // the question about the file's colors), the theme in use returns.
        if (!id.empty()) MCChooseTheme(@(id.c_str()));
        [[AppSettings shared] previewTheme:nil];
    };
    MCPaletteContext *ctx = [MCPaletteContext new];
    ctx.pick = pick;
    MCShowPalette(window, @"", ctx);
}
