// AppSettings.mm — see AppSettings.h.
#import "AppSettings.h"
#include <fcntl.h>
#include <unistd.h>

NSNotificationName const MCSettingsDidChangeNotification =
    @"MCSettingsDidChangeNotification";

NSColor *MCColor(const Rgba &c) {
    return [NSColor colorWithSRGBRed:c.r / 255.0 green:c.g / 255.0
                                blue:c.b / 255.0 alpha:c.a];
}

@implementation AppSettings {
    Settings _settings;
    NSString *_text;                 // file contents last applied
    dispatch_source_t _watch;
}

+ (instancetype)shared {
    static AppSettings *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ s = [[AppSettings alloc] init]; });
    return s;
}

- (instancetype)init {
    if ((self = [super init])) {
        NSString *env = NSProcessInfo.processInfo.environment[@"MINICODE_SETTINGS"];
        _path = env.length ? env.stringByExpandingTildeInPath
                           : [NSHomeDirectory() stringByAppendingPathComponent:
                                 @".config/minicode/settings.conf"];
        _errors = @[];
        [self load];
        [self watch];
    }
    return self;
}

- (const Settings &)settings { return _settings; }

// Returns YES if the file's contents differ from what was applied.
- (BOOL)load {
    NSString *text = [NSString stringWithContentsOfFile:_path
                                               encoding:NSUTF8StringEncoding
                                                  error:nil] ?: @"";
    if (_text && [text isEqualToString:_text]) return NO;
    _text = text;
    std::vector<SettingsError> errs;
    _settings = Settings::parse(text.UTF8String ?: "", &errs);
    NSMutableArray *list = [NSMutableArray array];
    for (const SettingsError &e : errs)
        [list addObject:[NSString stringWithFormat:@"line %d: %s", e.line,
                         e.message.c_str()]];
    _errors = list;
    for (NSString *e in list) NSLog(@"MiniCode settings %@", e);
    return YES;
}

- (void)reload {
    if ([self load])
        [[NSNotificationCenter defaultCenter]
            postNotificationName:MCSettingsDidChangeNotification object:self];
}

// Watch the file itself. Editors, MiniCode included, save by writing a new
// file and renaming it over the old one, so on delete/rename the watch is
// re-armed on whatever is at the path now. With no file yet, check back
// every couple of seconds so creating one outside MiniCode is noticed too.
- (void)watch {
    if (_watch) { dispatch_source_cancel(_watch); _watch = nil; }
    int fd = open(_path.fileSystemRepresentation, O_EVTONLY);
    __weak AppSettings *weakSelf = self;
    if (fd < 0) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
            AppSettings *s = weakSelf;
            if (s && !s->_watch) { [s reload]; [s watch]; }
        });
        return;
    }
    dispatch_source_t src = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_VNODE, fd,
        DISPATCH_VNODE_WRITE | DISPATCH_VNODE_EXTEND | DISPATCH_VNODE_DELETE |
            DISPATCH_VNODE_RENAME,
        dispatch_get_main_queue());
    dispatch_source_set_event_handler(src, ^{
        AppSettings *s = weakSelf;
        if (!s) return;
        unsigned long flags = dispatch_source_get_data(src);
        if (flags & (DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME)) {
            // Give the replacement file a moment to land.
            dispatch_source_cancel(src);
            if (s->_watch == src) s->_watch = nil;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 10),
                           dispatch_get_main_queue(), ^{
                [weakSelf reload];
                [weakSelf watch];
            });
        } else {
            [s reload];
        }
    });
    dispatch_source_set_cancel_handler(src, ^{ close(fd); });
    _watch = src;
    dispatch_resume(src);
}

- (BOOL)ensureFileExists {
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([fm fileExistsAtPath:_path]) return YES;
    [fm createDirectoryAtPath:_path.stringByDeletingLastPathComponent
  withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *body = [NSString stringWithUTF8String:Settings::defaultFileText()];
    BOOL ok = [body writeToFile:_path atomically:YES
                       encoding:NSUTF8StringEncoding error:nil];
    if (ok) { [self reload]; [self watch]; }
    return ok;
}

- (NSColor *)background:(Surface)s { return MCColor(_settings.background(s)); }
- (NSColor *)text:(Surface)s       { return MCColor(_settings.text(s)); }
- (NSColor *)syntax:(TokenStyle)s  { return MCColor(_settings.syntax(s)); }
- (NSColor *)markdown:(MarkdownColor)c { return MCColor(_settings.markdown(c)); }

- (NSVisualEffectMaterial)material {
    const std::string &m = _settings.material();
    if (m == "titlebar")   return NSVisualEffectMaterialTitlebar;
    if (m == "sidebar")    return NSVisualEffectMaterialSidebar;
    if (m == "menu")       return NSVisualEffectMaterialMenu;
    if (m == "popover")    return NSVisualEffectMaterialPopover;
    if (m == "hud")        return NSVisualEffectMaterialHUDWindow;
    if (m == "sheet")      return NSVisualEffectMaterialSheet;
    if (m == "window")     return NSVisualEffectMaterialWindowBackground;
    if (m == "header")     return NSVisualEffectMaterialHeaderView;
    if (m == "content")    return NSVisualEffectMaterialContentBackground;
    if (m == "fullscreen") return NSVisualEffectMaterialFullScreenUI;
    if (m == "tooltip")    return NSVisualEffectMaterialToolTip;
    return NSVisualEffectMaterialUnderWindowBackground;
}

@end
