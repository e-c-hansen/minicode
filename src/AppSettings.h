// AppSettings.h — Objective-C++. Loads the settings file, watches it for
// changes, and hands the parsed values to the views as NSColors. Views listen
// for MCSettingsDidChangeNotification and reapply.
#import <Cocoa/Cocoa.h>
#include "Settings.h"

extern NSNotificationName const MCSettingsDidChangeNotification;

@interface AppSettings : NSObject
+ (instancetype)shared;

// ~/.config/minicode/settings.conf, or $MINICODE_SETTINGS when set.
@property(nonatomic, readonly, copy) NSString *path;
// Problems in the file, as "line N: message"; empty when it parsed cleanly.
@property(nonatomic, readonly, copy) NSArray<NSString *> *errors;

- (const Settings &)settings;
- (NSColor *)background:(Surface)surface;
- (NSColor *)text:(Surface)surface;
- (NSColor *)syntax:(TokenStyle)style;
- (NSColor *)markdown:(MarkdownColor)color;
- (NSVisualEffectMaterial)material;

// Write the commented default file if there is none yet.
- (BOOL)ensureFileExists;
// Re-read the file now and notify if anything changed.
- (void)reload;
// The file's text as it is on disk now ("" when there is none).
- (NSString *)fileText;
// Writes `theme = <themeId>` into the file (Settings::withTheme), commenting
// out the file's own colors with `commentColors`, and applies it at once.
- (BOOL)writeTheme:(NSString *)themeId commentColors:(BOOL)commentColors;
// Writes `key = value` into the file (Settings::withValue) and applies it
// at once.
- (BOOL)writeValue:(NSString *)value forKey:(NSString *)key;
// writeTheme: and writeValue:forKey: are refused while a tab holds the
// file with unsaved changes, since the tab and the disk would then part
// ways; the reason goes to writeProblemHandler. Both are set once by the
// editor windows (EditorController+Settings.mm): `unsavedTextCheck` says
// whether any window has a tab of `path` with unsaved changes, and
// `writeProblemHandler` shows a message (the status bar).
@property(class, nonatomic, copy) BOOL (^unsavedTextCheck)(NSString *path);
@property(class, nonatomic, copy) void (^writeProblemHandler)(NSString *message);
@end

// Writes `text` to a file minicode keeps for the user (settings.conf,
// keys.conf) through FolderSearch::writeFileAtomically: a symlink, a
// dangling one too, stays a link and the file it names changes; a hard
// link is written in place; mode and extended attributes are kept. With
// `checkTabs`, refused while AppSettings.unsavedTextCheck says a tab holds
// the file with unsaved changes. NO with *why set when nothing was written.
BOOL MCWriteConfigFile(NSString *path, NSString *text, BOOL checkTabs, NSString **why);

NSColor *MCColor(const Rgba &c);
