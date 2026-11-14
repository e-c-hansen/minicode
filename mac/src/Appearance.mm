// Appearance.mm — see Appearance.h.
#import "Appearance.h"
#import "AppSettings.h"
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
static __weak NSView *gOpacityOwner;

// ------------------------------------------------- Esc closes the pickers
// The swatch's color panel and the opacity popover close on Esc while the
// keyboard is in them or in the settings file that opened them. The color
// panel never becomes key by itself (only a click in one of its fields
// gives it the keyboard), so the key usually lands in the file, where Esc
// would otherwise do nothing. One local monitor, made with the first
// picker, that looks no further than the key code unless a picker is open.
static BOOL gSwatchPanelOpen;
static __weak NSView *gSwatchOwner;

static void MCForgetSwatchPanel(void) {
    gSwatchPanelOpen = NO;
    gSwatchOwner = nil;
    if (![NSColorPanel sharedColorPanelExists]) return;
    NSColorPanel *panel = [NSColorPanel sharedColorPanel];
    panel.target = nil;   // no later change of the shared panel counts as a pick
    panel.action = NULL;
}

BOOL MCSwatchColorPanelOpen(void) { return gSwatchPanelOpen; }

BOOL MCCloseSwatchPanels(void) {
    BOOL closed = NO;
    if (gSwatchPanelOpen) {
        NSColorPanel *panel = [NSColorPanel sharedColorPanel];
        MCForgetSwatchPanel();
        [panel close];
        closed = YES;
    }
    if (gOpacityPopover.shown) {
        [gOpacityPopover close];
        closed = YES;
    }
    gOpacityPopover = nil;
    gOpacityOwner = nil;
    return closed;
}

// Whether the keyboard is in `owner`: it, or the field editor editing it,
// is its window's first responder.
static BOOL MCKeyboardIn(NSWindow *window, NSView *owner) {
    if (!owner || owner.window != window) return NO;
    NSResponder *fr = window.firstResponder;
    if (fr == owner) return YES;
    return [fr isKindOfClass:NSTextView.class] && ((NSTextView *)fr).isFieldEditor &&
           ((NSTextView *)fr).delegate == (id)owner;
}

static void MCWatchPickerEscape(void) {
    static id monitor;
    if (monitor) return;
    monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                     handler:^NSEvent *(NSEvent *e) {
        if (e.keyCode != 53) return e;   // Esc
        const NSEventModifierFlags mods = e.modifierFlags & (NSEventModifierFlagCommand |
            NSEventModifierFlagControl | NSEventModifierFlagOption);
        if (mods || !e.window) return e;
        BOOL ours = NO;
        if (gSwatchPanelOpen)
            ours = e.window == [NSColorPanel sharedColorPanel] ||
                   MCKeyboardIn(e.window, gSwatchOwner);
        if (!ours && gOpacityPopover.shown)
            ours = e.window == gOpacityPopover.contentViewController.view.window ||
                   MCKeyboardIn(e.window, gOpacityOwner);
        if (!ours) return e;
        MCCloseSwatchPanels();
        return nil;
    }];
    // The panel's own close button, Cmd+W in it, or anything else that
    // closes it: its picks stop.
    [[NSNotificationCenter defaultCenter]
        addObserverForName:NSWindowWillCloseNotification object:nil queue:nil
                usingBlock:^(NSNotification *note) {
        if (gSwatchPanelOpen && [NSColorPanel sharedColorPanelExists] &&
            note.object == [NSColorPanel sharedColorPanel])
            MCForgetSwatchPanel();
    }];
}

void MCShowSwatchColorPanel(NSColor *color, id target, SEL action, NSView *owner) {
    MCWatchPickerEscape();
    NSColorPanel *panel = [NSColorPanel sharedColorPanel];
    panel.showsAlpha = YES;
    panel.continuous = YES;
    panel.target = nil;   // setting the starting color must not count as a pick
    panel.action = NULL;
    panel.color = color;
    panel.target = target;
    panel.action = action;
    gSwatchPanelOpen = YES;
    gSwatchOwner = owner;
    [panel orderFront:nil];
}

@implementation MCOpacityPopover
+ (void)showForValue:(double)value relativeToRect:(NSRect)rect ofView:(NSView *)view
            onChange:(void (^)(double))onChange {
    MCWatchPickerEscape();
    [gOpacityPopover close];
    gOpacityOwner = view;
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

// ------------------------------------------------------------ View > Theme
@interface MCThemeMenuTarget : NSObject <NSMenuItemValidation>
+ (instancetype)shared;
@end

@implementation MCThemeMenuTarget
+ (instancetype)shared {
    static MCThemeMenuTarget *t;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ t = [MCThemeMenuTarget new]; });
    return t;
}
- (void)chooseTheme:(NSMenuItem *)sender {
    MCChooseTheme(sender.representedObject);
}
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    NSString *current = @([AppSettings shared].settings.theme().id);
    item.state = [item.representedObject isEqual:current] ? NSControlStateValueOn
                                                          : NSControlStateValueOff;
    return YES;
}
@end

NSMenu *MCThemeSubmenu(void) {
    NSMenu *m = [[NSMenu alloc] initWithTitle:@"Theme"];
    BOOL lastDark = YES;
    for (const Theme &t : Settings::themes()) {
        if (!t.dark && lastDark && m.numberOfItems) [m addItem:[NSMenuItem separatorItem]];
        lastDark = t.dark;
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@(t.title)
                                                      action:@selector(chooseTheme:)
                                               keyEquivalent:@""];
        item.target = [MCThemeMenuTarget shared];
        item.representedObject = @(t.id);
        [m addItem:item];
    }
    return m;
}

BOOL MCChooseTheme(NSString *themeId) {
    AppSettings *cfg = [AppSettings shared];
    const Theme *theme = Settings::findTheme(themeId.UTF8String ?: "");
    if (!theme) return NO;
    const int own = Settings::colorOverrides(cfg.fileText.UTF8String ?: "");
    BOOL comment = NO;
    if (own > 0) {
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = [NSString stringWithFormat:@"Your settings file sets %d color%@ of its own",
                                                   own, own == 1 ? @"" : @"s"];
        a.informativeText = [NSString stringWithFormat:
            @"%@ covers only what they leave. Comment them out to see the whole theme; "
            @"they stay in the file, so you can bring any of them back.", @(theme->title)];
        [a addButtonWithTitle:@"Use the Theme's Colors"];
        [a addButtonWithTitle:@"Keep My Colors"];
        [a addButtonWithTitle:@"Cancel"];
        NSModalResponse r = [a runModal];
        if (r == NSAlertThirdButtonReturn) return NO;
        comment = r == NSAlertFirstButtonReturn;
    }
    if (![cfg writeTheme:themeId commentColors:comment]) {
        NSBeep();
        return NO;
    }
    return YES;
}
