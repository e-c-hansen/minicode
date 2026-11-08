// EditorFont.mm — see EditorFont.h.
#import "EditorFont.h"
#import "AppSettings.h"
#import "TreeDetails.h"   // MCAppDefaults

NSNotificationName const MCEditorZoomDidChangeNotification = @"MCEditorZoomDidChange";

NSNotificationName const MCTerminalZoomDidChangeNotification = @"MCTerminalZoomDidChange";

static NSString *const kZoomKey = @"MCEditorZoom";
// Kept even at 0, so that a missing value means "never set": the first
// read then starts from the editor's zoom, which the terminals shared
// until they had one of their own.
static NSString *const kTerminalZoomKey = @"MCTerminalZoom";

CGFloat MCEditorZoom(void) {
    return (CGFloat)[MCAppDefaults() doubleForKey:kZoomKey];
}

CGFloat MCTerminalZoom(void) {
    NSUserDefaults *d = MCAppDefaults();
    if (![d objectForKey:kTerminalZoomKey]) [d setDouble:MCEditorZoom() forKey:kTerminalZoomKey];
    return (CGFloat)[d doubleForKey:kTerminalZoomKey];
}

NSFont *MCMonoFont(CGFloat size, NSFontWeight weight) {
    for (int i = 0; i < 3; i++) {
        NSFont *f = [NSFont monospacedSystemFontOfSize:size weight:weight];
        if (f) return f;
    }
    return [NSFont userFixedPitchFontOfSize:size] ?: [NSFont systemFontOfSize:size];
}

CGFloat MCEditorFontSize(void) {
    const CGFloat size = (CGFloat)[AppSettings shared].settings.editorFontSize() + MCEditorZoom();
    return MAX((CGFloat)Settings::kMinFontSize, MIN((CGFloat)Settings::kMaxFontSize, size));
}

NSFont *MCEditorFont(void) {
    return MCMonoFont(MCEditorFontSize(), NSFontWeightRegular);
}

CGFloat MCTerminalFontSize(void) {
    const CGFloat size = (CGFloat)[AppSettings shared].settings.terminalFontSize() + MCTerminalZoom();
    return MAX((CGFloat)Settings::kMinFontSize, MIN((CGFloat)Settings::kMaxFontSize, size));
}

NSFont *MCTerminalFont(BOOL bold, BOOL italic) {
    static CGFloat cachedSize;
    static NSFont *fonts[4];
    const CGFloat size = MCTerminalFontSize();
    if (size != cachedSize) {
        for (NSFont *__strong &f : fonts) f = nil;
        cachedSize = size;
    }
    const int i = (bold ? 1 : 0) | (italic ? 2 : 0);
    if (!fonts[i]) {
        NSFont *f = MCMonoFont(size, bold ? NSFontWeightBold : NSFontWeightRegular);
        if (italic) {
            NSFontDescriptor *d = [f.fontDescriptor fontDescriptorWithSymbolicTraits:
                f.fontDescriptor.symbolicTraits | NSFontDescriptorTraitItalic];
            f = [NSFont fontWithDescriptor:d size:size] ?: f;
        }
        fonts[i] = f;
    }
    return fonts[i];
}

CGFloat MCEditorScale(void) { return MCEditorFontSize() / 13.0; }

CGFloat MCZoomScale(void) { return MAX((CGFloat)0.5, (13 + MCEditorZoom()) / 13.0); }

CGFloat MCZoomed(CGFloat points) { return round(points * MCZoomScale() * 2) / 2; }

BOOL MCEditorZoomBy(CGFloat points) {
    const CGFloat base = (CGFloat)[AppSettings shared].settings.editorFontSize();
    const CGFloat old = MCEditorZoom();
    CGFloat zoom = points == 0 ? 0 : old + points;
    // The zoom keeps the whole size within the limits.
    zoom = MAX((CGFloat)Settings::kMinFontSize - base, MIN((CGFloat)Settings::kMaxFontSize - base, zoom));
    if (zoom == old) return points == 0;
    if (zoom == 0) [MCAppDefaults() removeObjectForKey:kZoomKey];
    else [MCAppDefaults() setDouble:zoom forKey:kZoomKey];
    [[NSNotificationCenter defaultCenter] postNotificationName:MCEditorZoomDidChangeNotification
                                                        object:nil];
    return YES;
}

BOOL MCTerminalZoomBy(CGFloat points) {
    const CGFloat base = (CGFloat)[AppSettings shared].settings.terminalFontSize();
    const CGFloat old = MCTerminalZoom();
    CGFloat zoom = points == 0 ? 0 : old + points;
    zoom = MAX((CGFloat)Settings::kMinFontSize - base, MIN((CGFloat)Settings::kMaxFontSize - base, zoom));
    if (zoom == old) return points == 0;
    [MCAppDefaults() setDouble:zoom forKey:kTerminalZoomKey];
    [[NSNotificationCenter defaultCenter] postNotificationName:MCTerminalZoomDidChangeNotification
                                                        object:nil];
    return YES;
}

NSNotificationName const MCEditorWrapDidChangeNotification = @"MCEditorWrapDidChange";

static int gWrapOverride = -1;   // the toggle's choice, -1 for the setting's
static int gWrapSetting = -1;    // the setting when the toggle was used

BOOL MCEditorWordWrap(void) {
    const int setting = [AppSettings shared].settings.wordWrap() ? 1 : 0;
    // The setting changed since the toggle: the setting's new value wins.
    if (gWrapOverride >= 0 && setting != gWrapSetting) gWrapOverride = -1;
    return gWrapOverride >= 0 ? gWrapOverride == 1 : setting == 1;
}

void MCEditorToggleWordWrap(void) {
    const BOOL now = MCEditorWordWrap();
    gWrapSetting = [AppSettings shared].settings.wordWrap() ? 1 : 0;
    gWrapOverride = now ? 0 : 1;
    [[NSNotificationCenter defaultCenter] postNotificationName:MCEditorWrapDidChangeNotification
                                                        object:nil];
}

@implementation NSApplication (MCEditorZoom)
- (IBAction)editorZoomIn:(id)sender {
    (void)sender;
    if (!MCEditorZoomBy(1)) NSBeep();
}
- (IBAction)editorZoomOut:(id)sender {
    (void)sender;
    if (!MCEditorZoomBy(-1)) NSBeep();
}
- (IBAction)editorZoomReset:(id)sender {
    (void)sender;
    MCEditorZoomBy(0);
}
- (IBAction)terminalZoomIn:(id)sender {
    (void)sender;
    if (!MCTerminalZoomBy(1)) NSBeep();
}
- (IBAction)terminalZoomOut:(id)sender {
    (void)sender;
    if (!MCTerminalZoomBy(-1)) NSBeep();
}
- (IBAction)terminalZoomReset:(id)sender {
    (void)sender;
    MCTerminalZoomBy(0);
}
@end
