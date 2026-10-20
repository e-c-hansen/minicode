// Appearance.mm — see Appearance.h.
#import "Appearance.h"
#include "Settings.h"

// The menu item's target. NSMenuItem holds its target weakly, so the item
// keeps this alive as its representedObject; the view is weak, so a menu a
// view keeps (the file tree's) makes no cycle.
@interface MCAdjustSettingsAction : NSObject
@property(nonatomic, copy) NSString *surface;
@property(nonatomic, weak) NSView *source;
- (void)adjust:(id)sender;
@end

@implementation MCAdjustSettingsAction
- (void)adjust:(id)sender {
    (void)sender;
    NSWindow *w = self.source.window ?: NSApp.keyWindow ?: NSApp.mainWindow;
    id delegate = w.delegate;
    if ([delegate respondsToSelector:@selector(showSettingsForSurface:)]) {
        [(id<MCAppearanceTarget>)delegate showSettingsForSurface:self.surface];
        return;
    }
    // Not in an editor window (a view in a panel of its own): let the app
    // delegate pick the current window.
    NSMenuItem *carrier = [[NSMenuItem alloc] init];
    carrier.representedObject = self.surface;
    [NSApp sendAction:NSSelectorFromString(@"adjustAppearance:") to:nil from:carrier];
}
@end

void MCAddAdjustSettingsItem(NSMenu *menu, NSString *surface, NSView *source) {
    if (!menu || !Settings::appearanceSurface(surface.UTF8String ?: "")) return;
    if (menu.numberOfItems > 0 && !menu.itemArray.lastObject.isSeparatorItem)
        [menu addItem:[NSMenuItem separatorItem]];
    MCAdjustSettingsAction *action = [[MCAdjustSettingsAction alloc] init];
    action.surface = surface;
    action.source = source;
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Adjust Settings…"
                                                  action:@selector(adjust:)
                                           keyEquivalent:@""];
    item.target = action;
    item.representedObject = action;
    [menu addItem:item];
}

NSMenu *MCAdjustSettingsMenu(NSString *surface, NSView *source) {
    NSMenu *m = [[NSMenu alloc] init];
    MCAddAdjustSettingsItem(m, surface, source);
    return m;
}

NSMenu *MCAdjustSettingsSubmenu(void) {
    NSMenu *m = [[NSMenu alloc] initWithTitle:@"Adjust Settings"];
    for (const AppearanceSurface &s : Settings::appearanceSurfaces()) {
        if (s.placeholder) continue;
        NSMenuItem *item = [[NSMenuItem alloc]
            initWithTitle:[NSString stringWithUTF8String:s.title]
                   action:NSSelectorFromString(@"adjustAppearance:")
            keyEquivalent:@""];
        item.representedObject = [NSString stringWithUTF8String:s.id];
        [m addItem:item];
    }
    return m;
}

@implementation MCSurfaceTextField
// The field editor asks its delegate, which is the field being edited.
- (NSMenu *)textView:(NSTextView *)tv menu:(NSMenu *)menu forEvent:(NSEvent *)event
             atIndex:(NSUInteger)index {
    (void)tv; (void)event; (void)index;
    if (self.appearanceSurface) MCAddAdjustSettingsItem(menu, self.appearanceSurface, self);
    return menu;
}
@end

// ------------------------------------------------------------ opacity slider
@interface MCOpacityController : NSViewController
@property(nonatomic, copy) void (^onChange)(double);
@property(nonatomic, strong) NSSlider *slider;
@property(nonatomic, strong) NSTextField *valueLabel;
@property(nonatomic, assign) double initial;
@end

@implementation MCOpacityController
- (void)loadView {
    NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 240, 44)];
    NSTextField *title = [NSTextField labelWithString:@"Opacity"];
    title.font = [NSFont systemFontOfSize:12];
    title.frame = NSMakeRect(12, 14, 52, 16);
    [v addSubview:title];

    self.slider = [NSSlider sliderWithValue:self.initial * 100 minValue:0 maxValue:100
                                     target:self action:@selector(slid:)];
    self.slider.continuous = YES;
    self.slider.frame = NSMakeRect(64, 12, 120, 20);
    [v addSubview:self.slider];

    self.valueLabel = [NSTextField labelWithString:@""];
    self.valueLabel.font = [NSFont monospacedDigitSystemFontOfSize:12
                                                            weight:NSFontWeightRegular];
    self.valueLabel.alignment = NSTextAlignmentRight;
    self.valueLabel.frame = NSMakeRect(186, 14, 42, 16);
    [v addSubview:self.valueLabel];
    [self showValue:self.initial];
    self.view = v;
}

- (void)showValue:(double)value {
    self.valueLabel.stringValue =
        [NSString stringWithFormat:@"%d%%", (int)lround(value * 100)];
}

- (void)slid:(NSSlider *)slider {
    double value = round(slider.doubleValue) / 100.0;
    [self showValue:value];
    if (self.onChange) self.onChange(value);
}
@end

static NSPopover *gOpacityPopover;

@implementation MCOpacityPopover
+ (void)showForValue:(double)value relativeToRect:(NSRect)rect ofView:(NSView *)view
            onChange:(void (^)(double))onChange {
    [gOpacityPopover close];
    MCOpacityController *vc = [[MCOpacityController alloc] init];
    vc.initial = value;
    vc.onChange = onChange;
    NSPopover *p = [[NSPopover alloc] init];
    p.behavior = NSPopoverBehaviorTransient;
    p.contentViewController = vc;
    gOpacityPopover = p;
    [p showRelativeToRect:rect ofView:view preferredEdge:NSRectEdgeMaxY];
}
+ (NSPopover *)current { return gOpacityPopover; }
@end
