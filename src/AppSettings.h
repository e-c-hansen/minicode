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
@end

NSColor *MCColor(const Rgba &c);
