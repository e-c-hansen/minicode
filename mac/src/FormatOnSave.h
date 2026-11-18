// FormatOnSave.h — Objective-C++. Turning format on save on and off
// without finding its line in the settings file:
//   - Edit > Format on Save (mutible.toggleFormatOnSave), checked while
//     format.on-save is on (true or project); it writes false, or project,
//     the setting that never formats a file in a style its project did not
//     choose (Formatter::projectChose);
//   - the offer: after Cmd+S saves a file whose project sets up its
//     formatter (a ruff section in pyproject.toml, a .clang-format, a
//     .prettierrc; Formatter::offersOnSave) while format.on-save is off and
//     that formatter is installed, a bar under the tab strip asks "This
//     project formats Python files with ruff, set up in pyproject.toml.
//     Format them each time you save?" with Turn On (writes project), Don't
//     Ask Again (kept in the app's defaults, as the install bar's Don't
//     Show Again), and × (this launch). Offered once per launch, in the
//     window where the save was. The save itself never waits for it, and
//     nothing is formatted until the next Cmd+S.
#import <Cocoa/Cocoa.h>

@class MCInstallBar;

// Edit > Format on Save, with its own target.
NSMenuItem *MCFormatOnSaveMenuItem(void);

// The offer's key in the install bar's muted list ("format-on-save").
extern NSString *const MCFormatOnSaveOfferKey;

// One window's offer bar, made with the window and laid out by it.
@interface MCFormatOnSaveOffer : NSObject
// `parent` holds the bar; `relayout` lays the window out again when the bar
// comes or goes; `note` shows a line in the status bar.
- (instancetype)initInView:(NSView *)parent relayout:(void (^)(void))relayout
                      note:(void (^)(NSString *message))note;
// Cmd+S saved `path` in the folder `root`: offer format on save if it is
// due (format.on-save off, not muted, not offered yet this launch, the
// project sets up an installed formatter). The check runs off the main
// thread; the bar shows when it answers.
- (void)offerAfterSaving:(NSString *)path root:(NSString *)root;
// The bar takes the top of `slot` while it shows and `shown` (the editor's
// side is on screen); the rest of the slot is returned.
- (NSRect)layoutIn:(NSRect)slot shown:(BOOL)shown;
// The bar, nil until offered (tests: its text and buttons).
@property(nonatomic, readonly) MCInstallBar *bar;
// Tests: forget that the offer was made this launch.
+ (void)resetOfferedForTesting;
@end
