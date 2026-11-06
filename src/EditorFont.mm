// EditorFont.mm — see EditorFont.h.
#import "EditorFont.h"
#import "AppSettings.h"
#import "TreeDetails.h"   // MCAppDefaults

NSNotificationName const MCEditorZoomDidChangeNotification = @"MCEditorZoomDidChange";

static NSString *const kZoomKey = @"MCEditorZoom";

CGFloat MCEditorZoom(void) {
    return (CGFloat)[MCAppDefaults() doubleForKey:kZoomKey];
}

CGFloat MCEditorFontSize(void) {
    const CGFloat size = (CGFloat)[AppSettings shared].settings.editorFontSize() + MCEditorZoom();
    return MAX((CGFloat)Settings::kMinFontSize, MIN((CGFloat)Settings::kMaxFontSize, size));
}

NSFont *MCEditorFont(void) {
    return [NSFont monospacedSystemFontOfSize:MCEditorFontSize() weight:NSFontWeightRegular];
}

CGFloat MCTerminalFontSize(void) {
    const CGFloat size = (CGFloat)[AppSettings shared].settings.terminalFontSize() + MCEditorZoom();
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
        NSFont *f = [NSFont monospacedSystemFontOfSize:size
                                                weight:bold ? NSFontWeightBold : NSFontWeightRegular];
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
@end
