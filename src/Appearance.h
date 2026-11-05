// Appearance.h — Objective-C++. "Adjust Settings…" in context menus: a right
// click on a part of the window offers to open the settings file at the keys
// that color that part (Settings::appearanceSurfaces has the map).
//
// A new view adds it with one line, at the end of its context menu:
//
//     MCAddAdjustSettingsItem(menu, @"terminal", self);
//
// or, for a plain view with no menu of its own, `view.menu =
// MCAdjustSettingsMenu(@"statusbar", view)`. The surface is an id from
// Settings::appearanceSurfaces(); the view picks the window whose
// EditorController opens the file.
#import <Cocoa/Cocoa.h>

// Appends a separator (when the menu is not empty) and "Adjust Settings…".
void MCAddAdjustSettingsItem(NSMenu *menu, NSString *surface, NSView *source);
// A menu holding only that item.
NSMenu *MCAdjustSettingsMenu(NSString *surface, NSView *source);
// View > Adjust Settings: a row per surface, each sending adjustAppearance:
// up the responder chain with the surface id as its representedObject.
NSMenu *MCAdjustSettingsSubmenu(void);

// View > Theme: a row per theme (Settings::themes), the one in use checked.
// Choosing one writes `theme = <id>` into the settings file, first asking,
// when the file sets colors of its own, whether to comment those out so the
// whole theme shows.
NSMenu *MCThemeSubmenu(void);
// The same choice without the menu (tests, the palette): YES when written.
BOOL MCChooseTheme(NSString *themeId);

// What the window's EditorController answers.
@protocol MCAppearanceTarget
- (void)showSettingsForSurface:(NSString *)surface;
@end

// A text field whose editing menu (the field editor's) ends with the item.
@interface MCSurfaceTextField : NSTextField
@property(nonatomic, copy) NSString *appearanceSurface;
@end

// A small popover with a slider, for an opacity value in the settings file.
// onChange gets every new value (0 to 1) while the slider moves.
@interface MCOpacityPopover : NSObject
+ (void)showForValue:(double)value relativeToRect:(NSRect)rect ofView:(NSView *)view
            onChange:(void (^)(double value))onChange;
+ (NSPopover *)current;   // the last one shown, for tests
@end
