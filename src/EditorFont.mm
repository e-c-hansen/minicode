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
