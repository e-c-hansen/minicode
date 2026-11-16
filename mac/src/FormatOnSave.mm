// FormatOnSave.mm — Objective-C++. See FormatOnSave.h.
#import "FormatOnSave.h"
#import "AppSettings.h"
#import "FormatRunner.h"
#import "InstallPanel.h"

NSString *const MCFormatOnSaveOfferKey = @"format-on-save";

// ------------------------------------------------------------- the menu item
@interface MCFormatOnSaveMenuTarget : NSObject
@end
@implementation MCFormatOnSaveMenuTarget
- (void)toggleFormatOnSave:(id)sender {
    (void)sender;
    const BOOL on = [AppSettings shared].settings.formatOnSave();
    if (![[AppSettings shared] writeValue:on ? @"false" : @"project" forKey:@"format.on-save"])
        NSBeep();
}
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(toggleFormatOnSave:))
        item.state = [AppSettings shared].settings.formatOnSave() ? NSControlStateValueOn
                                                                  : NSControlStateValueOff;
    return YES;
}
@end

NSMenuItem *MCFormatOnSaveMenuItem(void) {
    static MCFormatOnSaveMenuTarget *target;
    if (!target) target = [MCFormatOnSaveMenuTarget new];
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Format on Save"
                                                  action:@selector(toggleFormatOnSave:)
                                           keyEquivalent:@""];
    item.target = target;
    item.toolTip = @"Cmd+S formats the file first with the formatter its project sets up (ruff, "
                   @"black, Prettier, a .clang-format, ...) or one format.<lang> names in the "
                   @"settings, never in a style the project did not choose (format.on-save = "
                   @"project). format.on-save = true also formats the rest in Google's style.";
    return item;
}

// ------------------------------------------------------------------ the offer
static BOOL gOffered = NO;   // shown once per launch, across windows

@implementation MCFormatOnSaveOffer {
    __weak NSView *_parent;
    void (^_relayout)(void);
    void (^_note)(NSString *);
    MCInstallBar *_bar;
    BOOL _checking;   // one look at a time
}

+ (void)resetOfferedForTesting { gOffered = NO; }

- (instancetype)initInView:(NSView *)parent relayout:(void (^)(void))relayout
                      note:(void (^)(NSString *))note {
    if (!(self = [super init])) return nil;
    _parent = parent;
    _relayout = [relayout copy];
    _note = [note copy];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(settingsChanged:)
                                                 name:MCSettingsDidChangeNotification object:nil];
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_bar removeFromSuperview];
}

- (MCInstallBar *)bar { return _bar; }

static BOOL OfferDue(void) {
    return !gOffered && !MCInstallBarMuted(MCFormatOnSaveOfferKey) &&
           ![AppSettings shared].settings.formatOnSave();
}

- (void)offerAfterSaving:(NSString *)path root:(NSString *)root {
    if (_checking || !path.length || !OfferDue()) return;
    _checking = YES;
    __weak MCFormatOnSaveOffer *weakSelf = self;
    MCFindFormatOnSaveOffer(path, root, ^(NSString *question) {
        MCFormatOnSaveOffer *s = weakSelf;
        if (!s) return;
        s->_checking = NO;
        // Answered elsewhere meanwhile, or another window offered it.
        if (question.length && OfferDue()) [s show:question];
    });
}

- (void)show:(NSString *)question {
    NSView *parent = _parent;
    if (!parent) return;
    gOffered = YES;
    if (!_bar) {
        _bar = [[MCInstallBar alloc] initWithFrame:NSZeroRect];
        __weak MCFormatOnSaveOffer *weakSelf = self;
        _bar.onInstall = ^(NSButton *sender) {
            (void)sender;
            [weakSelf turnOn];
        };
        _bar.onMute = ^{
            MCInstallBarSetMuted(MCFormatOnSaveOfferKey, YES);
            [weakSelf hide];
        };
        _bar.onClose = ^{ [weakSelf hide]; };
        [parent addSubview:_bar];
    }
    [_bar showMessage:question
                  tip:@"Turn On writes format.on-save = project in the settings: from the next "
                       "save on, Cmd+S runs the formatter a project sets up, or one format.<lang> "
                       "names, and leaves other files as typed. Edit > Format on Save turns it "
                       "off again. Don't Ask Again stops this offer; the menu item stays."
               button:@"Turn On"
                 mute:@"Don't Ask Again"
                close:@"Close (asked again after minicode starts again)"];
    [self applyColors];
    _bar.hidden = NO;
    if (_relayout) _relayout();
}

- (void)turnOn {
    if (![[AppSettings shared] writeValue:@"project" forKey:@"format.on-save"]) {
        NSBeep();   // the settings file has unsaved changes in a tab: said in the status bar
        return;
    }
    [self hide];
    if (_note)
        _note(@"Format on save is on for formatters a project sets up. "
               "Edit > Format on Save turns it off.");
}

- (void)hide {
    if (!_bar || _bar.hidden) return;
    _bar.hidden = YES;
    if (_relayout) _relayout();
}

- (NSRect)layoutIn:(NSRect)slot shown:(BOOL)shown {
    if (!_bar) return slot;
    if (!shown || _bar.hidden) {
        _bar.frame = NSZeroRect;
        return slot;
    }
    const CGFloat h = MIN(kMCInstallBarHeight, NSHeight(slot));
    _bar.frame = NSMakeRect(NSMinX(slot), NSMaxY(slot) - h, NSWidth(slot), h);
    return NSMakeRect(NSMinX(slot), NSMinY(slot), NSWidth(slot), NSHeight(slot) - h);
}

- (void)applyColors {
    AppSettings *cfg = [AppSettings shared];
    [_bar setBackground:[cfg background:Surface::Sidebar] text:[cfg text:Surface::Sidebar]];
}

- (void)settingsChanged:(NSNotification *)n {
    (void)n;
    if (!_bar) return;
    [self applyColors];
    // Turned on some other way (the menu, the file): nothing left to offer.
    if ([AppSettings shared].settings.formatOnSave()) [self hide];
}
@end
