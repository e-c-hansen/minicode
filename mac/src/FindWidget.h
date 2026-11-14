// FindWidget.h — the pieces VS Code's find widget and search view are made
// of, drawn natively, for the editor's find bar (EditorFind.mm) and Find in
// Folder (Search.mm):
//
// - MCFindStyle: the theme's colors for them, VS Code's editorWidget.*,
//   input.*, inputOption.*, badge.* and diff colors, read off the theme
//   files for the themes minicode has (Settings::themes), the light or
//   dark defaults otherwise.
// - MCFindButton: VS Code's 22-point icon button (16-point codicon, 3 of
//   padding, a hover square) or, as a toggle, its 20-point option toggle
//   (a tinted square with a border when on). The icons are drawn here, in
//   the spirit of the codicons, since nothing may be bundled.
// - MCFindInput: VS Code's FindInput, a text field with its option toggles
//   inside its right end, a border that turns the focus color while it has
//   the keyboard and red for an error.
//
// Objective-C++, AppKit only.
#import <Cocoa/Cocoa.h>

@interface MCFindStyle : NSObject
+ (MCFindStyle *)current;   // for the settings as they are now
@property(nonatomic, readonly) BOOL dark;
@property(nonatomic, readonly) NSColor *widgetBackground, *widgetBorder, *widgetForeground;
@property(nonatomic, readonly) NSColor *shadow;
@property(nonatomic, readonly) NSColor *inputBackground, *inputBorder, *inputForeground;
@property(nonatomic, readonly) NSColor *placeholder, *focusBorder;
@property(nonatomic, readonly) NSColor *optionActiveBackground, *optionActiveBorder;
@property(nonatomic, readonly) NSColor *optionActiveForeground, *optionHover;
@property(nonatomic, readonly) NSColor *toolbarHover;
@property(nonatomic, readonly) NSColor *errorForeground, *errorBorder;
@property(nonatomic, readonly) NSColor *badgeBackground, *badgeForeground;
@property(nonatomic, readonly) NSColor *sideBarBackground, *sideBarForeground, *muted;
@property(nonatomic, readonly) NSColor *matchHighlight;   // findMatchHighlightBackground
@property(nonatomic, readonly) NSColor *removedText, *insertedText;   // replace previews
@property(nonatomic, readonly) NSColor *selection;        // list.activeSelectionBackground
@property(nonatomic, readonly) NSAppearance *appearance;
@end

typedef NS_ENUM(NSInteger, MCFindIcon) {
    MCFindIconCase,          // Aa: Match Case
    MCFindIconWord,          // ab over a bracket: Match Whole Word
    MCFindIconRegex,         // .*: Use Regular Expression
    MCFindIconPreserveCase,  // AB
    MCFindIconSelection,     // Find in Selection
    MCFindIconReplace,
    MCFindIconReplaceAll,
    MCFindIconUp,
    MCFindIconDown,
    MCFindIconClose,
    MCFindIconChevronRight,
    MCFindIconChevronDown,
    MCFindIconMore,          // ... (the search view's query details)
    MCFindIconCollapseAll,
    MCFindIconExpandAll,
    MCFindIconRefresh,
    MCFindIconClear,
};

// Draw an icon in a 16-point box, in a color.
void MCFindDrawIcon(MCFindIcon icon, NSRect box, NSColor *color);

@interface MCFindButton : NSButton
// An icon button (22 x 22) or an option toggle (20 x 20, or `size`).
+ (instancetype)button:(MCFindIcon)icon tip:(NSString *)tip target:(id)target action:(SEL)action;
+ (instancetype)toggle:(MCFindIcon)icon tip:(NSString *)tip target:(id)target action:(SEL)action;
@property(nonatomic) MCFindIcon icon;
@property(nonatomic, readonly) BOOL isToggle;
@property(nonatomic) BOOL on;            // a toggle's state
@property(nonatomic) CGFloat radius;     // 5 for buttons, 3 for toggles
@property(nonatomic, strong) MCFindStyle *style;
@end

@interface MCFindInput : NSView
- (instancetype)initWithPlaceholder:(NSString *)placeholder
                            toggles:(NSArray<MCFindButton *> *)toggles;
@property(nonatomic, readonly) NSTextField *field;
@property(nonatomic, readonly) NSArray<MCFindButton *> *toggles;
@property(nonatomic) BOOL error;         // a red border
@property(nonatomic) BOOL togglesHidden;
@property(nonatomic, readonly) BOOL editing;   // its field has the keyboard
@property(nonatomic, strong) MCFindStyle *style;
@property(nonatomic) CGFloat fontSize;   // 13, as VS Code's find inputs
@end
