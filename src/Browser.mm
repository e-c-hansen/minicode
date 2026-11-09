// Browser.mm — embedded browser panel. Objective-C++.
#import "Browser.h"
#import "AppSettings.h"
#import "Appearance.h"
#import "BrowserPages.h"
#import "Keymap.h"
#import "PageZoom.h"
#import "TabStrip.h"
#import "TreeDetails.h"   // MCAppDefaults
#import <WebKit/WebKit.h>
#include "BrowserTabs.h"
#include "EditorTabs.h"

// The page's own context menu (Back, Reload, Inspect...) ends with Adjust
// Settings for the browser.
//
// The page never takes the keyboard by itself. WebKit makes the web view the
// first responder whenever the page focuses an element (an autofocus search
// box, a script calling focus() once loaded), which took the keyboard from
// the terminal or the editor while a page loaded, even with the browser
// hidden. So the view accepts it only for the user: a click in it, Tab from
// the toolbar, or a page the user asked for from the toolbar (address bar,
// back, forward, reload) while the keyboard is still in the toolbar. Never
// while hidden. Switching tabs hands the keyboard from the page on show to
// the next one (grantKeyboard), since the user's key or click did it.
@interface MCWebView : WKWebView
@property(nonatomic, weak) NSView *chrome;          // the BrowserView around it
@property(nonatomic) BOOL toolbarNavigation;        // the last load came from the toolbar
@property(nonatomic) BOOL grantKeyboard;            // the user's tab switch hands it over
@end
@implementation MCWebView
- (void)willOpenMenu:(NSMenu *)menu withEvent:(NSEvent *)event {
    [super willOpenMenu:menu withEvent:event];
    MCAddAdjustSettingsItem(menu, @"browser", self);
}

// Whether the keyboard is somewhere in the browser's toolbar now.
- (BOOL)keyboardInToolbar {
    NSResponder *r = self.window.firstResponder;
    if ([r isKindOfClass:NSTextView.class] && [(NSTextView *)r isFieldEditor] &&
        [[(NSTextView *)r delegate] isKindOfClass:NSView.class])
        r = (NSView *)[(NSTextView *)r delegate];
    return [r isKindOfClass:NSView.class] && self.chrome &&
           [(NSView *)r isDescendantOf:self.chrome] && ![(NSView *)r isDescendantOf:self];
}

- (BOOL)userAsksForKeyboard {
    if (self.isHiddenOrHasHiddenAncestor || !self.window) return NO;
    if (self.grantKeyboard) return YES;
    NSEvent *e = NSApp.currentEvent;
    // A click (any button) in the page, just now.
    BOOL recent = e && NSProcessInfo.processInfo.systemUptime - e.timestamp < 1.0;
    switch (e.type) {
    case NSEventTypeLeftMouseDown:
    case NSEventTypeRightMouseDown:
    case NSEventTypeOtherMouseDown:
        if (recent && e.window == self.window &&
            NSPointInRect([self convertPoint:e.locationInWindow fromView:nil], self.bounds))
            return YES;
        break;
    case NSEventTypeKeyDown:
        // Tab or Shift+Tab out of the toolbar into the page.
        if (recent && e.keyCode == 48 /* Tab */ && [self keyboardInToolbar]) return YES;
        break;
    default:
        break;
    }
    // The page the user asked for in the toolbar focusing its own field,
    // while the keyboard is still where they asked from.
    return self.toolbarNavigation && [self keyboardInToolbar];
}

- (BOOL)mc_refusesKeyboard { return ![self userAsksForKeyboard]; }
@end

// Asked before the current first responder resigns: NSWindow's own
// makeFirstResponder: takes the keyboard from the editor first and, when the
// new view then refuses, leaves it with the window itself.
@implementation MCKeyboardWindow
- (BOOL)makeFirstResponder:(NSResponder *)responder {
    if (responder != self.firstResponder &&
        [responder respondsToSelector:@selector(mc_refusesKeyboard)] &&
        [(MCWebView *)responder mc_refusesKeyboard])
        return NO;
    return [super makeFirstResponder:responder];
}
@end


// One of the user's tabs. Its page is live (a web view, and WebKit's
// process behind it) while it is on show or was used just before
// (BrowserTabs::livePages); otherwise it is suspended to its address, its
// title and WebKit's record of its history and scroll, and loads again
// when shown. Its probe stays, so `minicode page --yours` follows the tab.
@interface MCBrowserTab : NSObject
@property(nonatomic, strong) MCWebView *web;      // nil while suspended
@property(nonatomic, strong) MCPageProbe *probe;
@property(nonatomic, copy) NSString *url;         // to load when shown; "" or nil: none
@property(nonatomic, copy) NSString *title;       // the last title seen
@property(nonatomic, strong) id record;           // WebKit's interactionState, while suspended
@property(nonatomic) uint64_t lastUsed;
@end
@implementation MCBrowserTab
@end

// The + at the strip's right end, on the strip's own color.
@interface MCBrowserStripButton : NSView
@property(nonatomic, strong) NSColor *stripColor;
@property(nonatomic, copy) void (^onPress)(void);
@end
@implementation MCBrowserStripButton
- (BOOL)isFlipped { return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    [(self.stripColor ?: [NSColor windowBackgroundColor]) set];
    NSRectFill(self.bounds);
    NSDictionary *a = @{NSFontAttributeName: [NSFont systemFontOfSize:15],
                        NSForegroundColorAttributeName:
                            [NSColor colorWithSRGBRed:0x9C / 255.0 green:0xA3 / 255.0
                                                 blue:0xAF / 255.0 alpha:1]};
    NSSize sz = [@"+" sizeWithAttributes:a];
    [@"+" drawAtPoint:NSMakePoint(floor(NSMidX(self.bounds) - sz.width / 2),
                                  floor(NSMidY(self.bounds) - sz.height / 2))
       withAttributes:a];
}
- (void)mouseDown:(NSEvent *)event {
    (void)event;
    if (self.onPress) self.onPress();
}
- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)point
          userData:(void *)data {
    (void)view; (void)tag; (void)point; (void)data;
    NSString *key = MCKeymapGlyphs(@"minicode.browser.newTab");
    return key.length ? [NSString stringWithFormat:@"New tab (%@)", key] : @"New tab";
}
- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self removeAllToolTips];
    [self addToolTipRect:self.bounds owner:self userData:NULL];
}
@end

static const CGFloat kBrowserPlusWidth = 28;
// A record past this size is left out, and the records of all the tabs
// together stay under the second; the addresses still come back.
static const NSUInteger kMaxBrowserState = 2 * 1024 * 1024;
static const NSUInteger kMaxBrowserStates = 4 * 1024 * 1024;

@interface BrowserView () <WKNavigationDelegate, WKUIDelegate, NSTextFieldDelegate,
                           NSMenuItemValidation> {
    NSMutableArray<MCBrowserTab *> *_tabs;
    NSInteger _selected;
    uint64_t _clock;
}
@property(nonatomic, strong) MCWebView   *agentWeb;   // the agent's, once it loaded one
@property(nonatomic, strong) MCPageProbe *agentProbe;
@property(nonatomic) BOOL agentPageShown;
@property(nonatomic, strong) MCTabStrip  *strip;
@property(nonatomic, strong) MCBrowserStripButton *plus;
@property(nonatomic, strong) NSTextField *urlBar;
@property(nonatomic, strong) NSButton    *back;
@property(nonatomic, strong) NSButton    *forward;
@property(nonatomic, strong) NSButton    *reload;
@property(nonatomic, strong) NSSegmentedControl *switcher;   // Yours | Agent
@property(nonatomic, strong) NSButton    *closeAgent;
@property(nonatomic, strong) NSTextField *zoomLabel;
@property(nonatomic) NSUInteger zoomLabelGeneration;
@end

static void *kTitleContext = &kTitleContext;

@implementation BrowserView
- (MCBrowserTab *)activeTab {
    return _selected >= 0 && _selected < (NSInteger)_tabs.count ? _tabs[_selected] : nil;
}
// The user's page on show in the tab strip (live: the tab on show always is).
- (MCWebView *)web { return self.activeTab.web; }
- (MCPageProbe *)userProbe { return self.activeTab.probe; }
- (MCWebView *)shownWeb { return self.agentPageShown && self.agentWeb ? self.agentWeb : self.web; }
- (NSString *)pageURL { return self.shownWeb.URL.absoluteString; }
- (NSString *)pageTitle { return self.shownWeb.title.length ? self.shownWeb.title : nil; }
- (BOOL)hasAgentPage { return self.agentWeb != nil; }
- (NSUInteger)tabCount { return _tabs.count; }
- (NSInteger)selectedTab { return _selected; }

- (NSUInteger)livePageCount {
    NSUInteger n = 0;
    for (MCBrowserTab *t in _tabs) n += t.web != nil;
    return n;
}

- (WKWebView *)webViewOfTab:(NSUInteger)i { return i < _tabs.count ? _tabs[i].web : nil; }

// A tab's address and title, as the strip and the session know them.
- (NSString *)addressOf:(MCBrowserTab *)t {
    NSString *u = t.web ? t.web.URL.absoluteString : nil;
    return u.length ? u : (t.url ?: @"");
}
- (NSString *)titleOf:(MCBrowserTab *)t {
    NSString *s = t.web ? t.web.title : nil;
    return s.length ? s : (t.title ?: @"");
}

- (NSArray<NSString *> *)tabAddresses {
    NSMutableArray *out = [NSMutableArray array];
    for (MCBrowserTab *t in _tabs) [out addObject:[self addressOf:t]];
    return out;
}
- (NSArray<NSString *> *)tabTitles {
    NSMutableArray *out = [NSMutableArray array];
    for (MCBrowserTab *t in _tabs) {
        std::string s = BrowserTabs::tabTitle([self titleOf:t].UTF8String ?: "",
                                              [self addressOf:t].UTF8String ?: "");
        [out addObject:[NSString stringWithUTF8String:s.c_str()] ?: @""];
    }
    return out;
}

// ------------------------------------------------------- the session
// WebKit's record of a page as data: NSData as it is, anything else
// archived (`archived` says which).
static NSData *MCRecordData(id record, BOOL *archived) {
    *archived = NO;
    if (!record) return nil;
    if ([record isKindOfClass:NSData.class]) return record;
    *archived = YES;
    return [NSKeyedArchiver archivedDataWithRootObject:record requiringSecureCoding:NO error:nil];
}

static id MCRecordFromData(NSData *data, BOOL archived) {
    if (!data.length) return nil;
    if (!archived) return data;
    NSKeyedUnarchiver *u = [[NSKeyedUnarchiver alloc] initForReadingFromData:data error:nil];
    u.requiresSecureCoding = NO;
    id record = [u decodeObjectForKey:NSKeyedArchiveRootObjectKey];
    [u finishDecoding];
    return record;
}

- (id)recordOf:(MCBrowserTab *)t {
    if (@available(macOS 12.0, *)) {
        if (t.web) return t.web.interactionState;
    }
    return t.record;
}

// Every tab: its address, title and, within the budget (BrowserTabs::
// keptRecords), WebKit's record of its history and scroll; and which one
// showed. The tab on show is also written as the one page older minicodes
// keep (url, state). The agent page is never kept.
- (NSDictionary *)savedState {
    std::vector<size_t> sizes;
    std::vector<uint64_t> used;
    NSMutableArray<NSData *> *datas = [NSMutableArray array];
    NSMutableArray<NSNumber *> *archivedFlags = [NSMutableArray array];
    for (MCBrowserTab *t in _tabs) {
        BOOL archived = NO;
        NSData *d = MCRecordData([self recordOf:t], &archived);
        [datas addObject:d ?: [NSData data]];
        [archivedFlags addObject:@(archived)];
        sizes.push_back(d.length);
        used.push_back(t.lastUsed);
    }
    const std::vector<bool> kept =
        BrowserTabs::keptRecords(sizes, used, (int)_selected, kMaxBrowserState, kMaxBrowserStates);
    NSMutableArray *tabs = [NSMutableArray array];
    NSInteger active = -1;
    NSDictionary *activeEntry = nil;
    for (NSUInteger i = 0; i < _tabs.count; i++) {
        MCBrowserTab *t = _tabs[i];
        NSString *url = [self addressOf:t];
        if (!url.length || [url isEqualToString:@"about:blank"]) continue;
        NSMutableDictionary *e = [NSMutableDictionary dictionaryWithObject:url forKey:@"url"];
        NSString *title = [self titleOf:t];
        if (title.length) e[@"title"] = title;
        if (kept[i]) {
            e[@"state"] = datas[i];
            e[@"archived"] = archivedFlags[i];
        }
        if ((NSInteger)i == _selected) {
            active = (NSInteger)tabs.count;
            activeEntry = e;
        }
        [tabs addObject:e];
    }
    if (!tabs.count) return nil;
    NSMutableDictionary *s = [NSMutableDictionary dictionary];
    s[@"tabs"] = tabs;
    s[@"active"] = @(active);
    for (NSString *k in @[@"url", @"state", @"archived"])
        if (activeEntry[k]) s[k] = activeEntry[k];
    return s;
}

// The tabs as savedState wrote them, or the one page an older minicode
// kept. Only the tab on show loads; the others wait to be shown.
- (void)restoreState:(NSDictionary *)state {
    if (![state isKindOfClass:NSDictionary.class]) return;
    NSArray *entries = [state[@"tabs"] isKindOfClass:NSArray.class] ? state[@"tabs"] : nil;
    NSInteger active = [state[@"active"] respondsToSelector:@selector(integerValue)]
                           ? [state[@"active"] integerValue] : -1;
    if (!entries) {
        entries = @[state];
        active = 0;
    }
    NSMutableArray<MCBrowserTab *> *tabs = [NSMutableArray array];
    for (NSDictionary *e in entries) {
        if (![e isKindOfClass:NSDictionary.class]) continue;
        NSString *url = [e[@"url"] isKindOfClass:NSString.class] ? e[@"url"] : nil;
        NSData *data = [e[@"state"] isKindOfClass:NSData.class] ? e[@"state"] : nil;
        if (!url.length && !data) continue;
        MCBrowserTab *t = [self makeTab];
        t.url = url;
        t.title = [e[@"title"] isKindOfClass:NSString.class] ? e[@"title"] : nil;
        if (@available(macOS 12.0, *)) t.record = MCRecordFromData(data, [e[@"archived"] boolValue]);
        [tabs addObject:t];
    }
    if (!tabs.count) return;
    for (MCBrowserTab *t in [_tabs copy]) [self discardTab:t];
    [_tabs setArray:tabs];
    _selected = -1;
    [self selectTab:(NSUInteger)BrowserTabs::restoredActive((int)active, (int)tabs.count)];
}

- (instancetype)initWithHomeURL:(NSString *)url {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 600, 400)])) {
        _tabs = [NSMutableArray array];
        _selected = -1;
        _liveLimit = BrowserTabs::kLivePages;
        self.wantsLayer = YES;
        [self buildChrome];
        [self applySettings];
        [[NSNotificationCenter defaultCenter]
            addObserver:self selector:@selector(applySettings)
                   name:MCSettingsDidChangeNotification object:nil];
        [_tabs addObject:[self makeTab]];
        [self selectTab:0];
        [self navigateToString:url];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    for (MCBrowserTab *t in _tabs) [self suspendWebOf:t keepRecord:NO];
    [self.agentProbe detach];
}

// The toolbar follows the settings file. The URL field keeps the system's
// colors unless a text color is set, since its background is the system's.
// The strip is the editor's: the sidebar's color, with the tab on show in
// the browser's own, running into the toolbar below it.
- (void)applySettings {
    AppSettings *cfg = [AppSettings shared];
    self.layer.backgroundColor = [cfg background:Surface::Browser].CGColor;
    self.urlBar.textColor = cfg.settings.textIsSet(Surface::Browser)
        ? [cfg text:Surface::Browser] : [NSColor controlTextColor];
    self.strip.stripColor = [cfg background:Surface::Sidebar];
    self.strip.activeColor = [cfg background:Surface::Browser];
    self.strip.activeText = cfg.settings.textIsSet(Surface::Browser)
        ? [cfg text:Surface::Browser] : [cfg text:Surface::Sidebar];
    self.strip.inactiveText = [[cfg text:Surface::Sidebar] colorWithAlphaComponent:0.7];
    self.strip.needsDisplay = YES;
    self.plus.stripColor = self.strip.stripColor;
    self.plus.needsDisplay = YES;
}

// A right click on the toolbar.
- (NSMenu *)menuForEvent:(NSEvent *)event {
    (void)event;
    return MCAdjustSettingsMenu(@"browser", self);
}

- (NSButton *)navButton:(NSString *)title action:(SEL)sel {
    NSButton *b = [NSButton buttonWithTitle:title target:self action:sel];
    b.bezelStyle = NSBezelStyleRounded;
    b.font = [NSFont systemFontOfSize:14];
    return b;
}

- (MCWebView *)makeWebViewWith:(WKWebViewConfiguration *)cfg {
    // A video's full screen button (and any element's requestFullscreen)
    // fills the screen, as in Safari.
    if (@available(macOS 12.3, *)) cfg.preferences.elementFullscreenEnabled = YES;
    MCWebView *web = [[MCWebView alloc] initWithFrame:[self pageRect] configuration:cfg];
    web.chrome = self;
    web.navigationDelegate = self;
    return web;
}

- (void)buildChrome {
    __weak BrowserView *weakSelf = self;
    self.strip = [[MCTabStrip alloc] initWithFrame:NSZeroRect];
    self.strip.surface = @"browser";
    self.strip.onSelect = ^(NSInteger i) { [weakSelf stripSelected:i]; };
    self.strip.onClose = ^(NSInteger i) { [weakSelf closeTab:(NSUInteger)i]; };
    self.strip.onMove = ^(NSInteger from, NSInteger to) {
        [weakSelf moveTab:(NSUInteger)from to:(NSUInteger)to];
    };
    self.strip.menuForTab = ^NSMenu *(NSInteger i) { return [weakSelf menuForTab:(NSUInteger)i]; };
    [self addSubview:self.strip];
    self.plus = [[MCBrowserStripButton alloc] initWithFrame:NSZeroRect];
    self.plus.onPress = ^{
        BrowserView *s = weakSelf;
        [s newTab];
        [s focusURLBar];
    };
    [self addSubview:self.plus];

    self.back    = [self navButton:@"◀" action:@selector(goBack:)];
    self.forward = [self navButton:@"▶" action:@selector(goForward:)];
    self.reload  = [self navButton:@"⟳" action:@selector(reloadPage:)];
    [self addSubview:self.back];
    [self addSubview:self.forward];
    [self addSubview:self.reload];

    // The switcher and the agent page's close button show once an agent
    // has a page.
    self.switcher = [NSSegmentedControl segmentedControlWithLabels:@[@"Yours", @"Agent"]
                                                      trackingMode:NSSegmentSwitchTrackingSelectOne
                                                            target:self
                                                            action:@selector(switcherChanged:)];
    self.switcher.font = [NSFont systemFontOfSize:11];
    self.switcher.selectedSegment = 0;
    self.switcher.hidden = YES;
    self.switcher.toolTip = @"Your page, or the page an agent loaded with minicode browse";
    [self addSubview:self.switcher];
    self.closeAgent = [self navButton:@"✕" action:@selector(closeAgentPage:)];
    self.closeAgent.font = [NSFont systemFontOfSize:11];
    self.closeAgent.toolTip = @"Close the agent page";
    self.closeAgent.hidden = YES;
    [self addSubview:self.closeAgent];

    MCSurfaceTextField *urlBar = [[MCSurfaceTextField alloc] init];
    urlBar.appearanceSurface = @"browser";
    self.urlBar = urlBar;
    self.urlBar.font = [NSFont systemFontOfSize:12];
    self.urlBar.placeholderString = @"Enter URL and press Return";
    self.urlBar.delegate = self;
    self.urlBar.target = self;
    self.urlBar.action = @selector(urlEntered:);
    [self addSubview:self.urlBar];

    // The zoom, over the address bar's right end for a moment after it
    // changes.
    NSTextField *zoom = [NSTextField labelWithString:@""];
    zoom.font = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightMedium];
    zoom.alignment = NSTextAlignmentCenter;
    zoom.drawsBackground = YES;
    zoom.backgroundColor = [NSColor controlAccentColor];
    zoom.textColor = NSColor.whiteColor;
    zoom.wantsLayer = YES;
    zoom.layer.cornerRadius = 4;
    zoom.layer.masksToBounds = YES;
    zoom.hidden = YES;
    self.zoomLabel = zoom;
    [self addSubview:zoom];
}

// Tab from the address bar goes into the page on show (MCWebView lets a Tab
// out of the toolbar in), and out of the page's last field back to the
// toolbar. The buttons are key views only with full keyboard access on;
// otherwise the loop passes them by.
- (void)updateKeyViewLoop {
    self.urlBar.nextKeyView = self.shownWeb;
    self.shownWeb.nextKeyView = self.back;
    self.back.nextKeyView = self.forward;
    self.forward.nextKeyView = self.reload;
    self.reload.nextKeyView = self.urlBar;
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self layoutChildren];
}

static const CGFloat kBrowserBarHeight = 36;

// Below the strip and the toolbar.
- (NSRect)pageRect {
    const CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
    return NSMakeRect(0, 0, W, MAX(0, H - kMCTabStripHeight - kBrowserBarHeight));
}

- (void)layoutChildren {
    CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
    const CGFloat stripH = MIN(kMCTabStripHeight, H), plusW = MIN(kBrowserPlusWidth, W);
    self.strip.frame = NSMakeRect(0, H - stripH, W - plusW, stripH);
    self.plus.frame = NSMakeRect(W - plusW, H - stripH, plusW, stripH);
    const CGFloat bw = 34, pad = 6;
    CGFloat x = pad;
    CGFloat by = H - stripH - kBrowserBarHeight + 4;
    self.back.frame    = NSMakeRect(x, by, bw, 26); x += bw + 2;
    self.forward.frame = NSMakeRect(x, by, bw, 26); x += bw + 2;
    self.reload.frame  = NSMakeRect(x, by, bw, 26); x += bw + pad;
    if (!self.switcher.hidden) {
        const CGFloat sw = MAX(110, self.switcher.intrinsicContentSize.width);
        self.switcher.frame = NSMakeRect(x, by + 1, sw, 24); x += sw + 2;
        self.closeAgent.frame = NSMakeRect(x, by, 28, 26); x += 28 + pad;
    }
    self.urlBar.frame  = NSMakeRect(x, by, MAX(0, W - x - pad), 24);
    self.zoomLabel.frame = NSMakeRect(MAX(x, NSMaxX(self.urlBar.frame) - 50), by + 4, 46, 16);
    const NSRect page = [self pageRect];
    for (MCBrowserTab *t in _tabs) t.web.frame = page;   // hidden pages keep their size
    self.agentWeb.frame = page;
}

- (void)focusURLBar { [self.window makeFirstResponder:self.urlBar]; }

// ------------------------------------------------------------- the tabs
- (MCBrowserTab *)makeTab {
    MCBrowserTab *t = [MCBrowserTab new];
    t.probe = [[MCPageProbe alloc] initRecordingConsole:NO];
    return t;
}

// The tab's page made live: a web view (from `cfg` when WebKit made the
// configuration, for a page a script opened), then its record or its
// address loaded.
- (void)makeLive:(MCBrowserTab *)t configuration:(WKWebViewConfiguration *)cfg load:(BOOL)load {
    if (t.web) return;
    MCWebView *web = [self makeWebViewWith:cfg ?: [[WKWebViewConfiguration alloc] init]];
    web.UIDelegate = self;
    web.hidden = YES;
    t.web = web;
    t.probe.webView = web;
    [web addObserver:self forKeyPath:@"title" options:0 context:kTitleContext];
    // At the back, under the toolbar's zoom label and the strip.
    [self addSubview:web positioned:NSWindowBelow relativeTo:nil];
    if (!load) return;
    id record = t.record;
    t.record = nil;
    web.toolbarNavigation = NO;   // a page coming back never takes the keyboard
    if (@available(macOS 12.0, *)) {
        if (record) {
            @try {
                web.interactionState = record;
            } @catch (NSException *e) {
                (void)e;   // a record from another WebKit: the address will do
            }
            if (web.URL || web.isLoading) return;
        }
    }
    NSURL *u = t.url.length ? [NSURL URLWithString:t.url] : nil;
    if (u && ![t.url isEqualToString:@"about:blank"]) [web loadRequest:[NSURLRequest requestWithURL:u]];
}

// The tab's page let go: its address, title and record kept (keepRecord),
// the web view and its WebKit process gone.
- (void)suspendWebOf:(MCBrowserTab *)t keepRecord:(BOOL)keepRecord {
    MCWebView *web = t.web;
    if (!web) return;
    if (keepRecord) {
        t.url = [self addressOf:t];
        t.title = [self titleOf:t];
        if (@available(macOS 12.0, *)) t.record = web.interactionState;
    }
    [web removeObserver:self forKeyPath:@"title" context:kTitleContext];
    [web stopLoading];
    web.navigationDelegate = nil;
    web.UIDelegate = nil;
    [web removeFromSuperview];
    t.web = nil;
    t.probe.webView = nil;
}

- (void)discardTab:(MCBrowserTab *)t { [self suspendWebOf:t keepRecord:NO]; }

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object
                        change:(NSDictionary *)change context:(void *)context {
    if (context != kTitleContext) {
        [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
        return;
    }
    [self reloadStrip];
}

// Pages beyond the live limit suspended: all but the tab on show and the
// ones used just before it.
- (void)trimLivePages {
    std::vector<uint64_t> used;
    for (MCBrowserTab *t in _tabs) used.push_back(t.lastUsed);
    const std::vector<bool> live =
        BrowserTabs::livePages(used, (int)_selected, (int)MAX(1, self.liveLimit));
    for (NSUInteger i = 0; i < _tabs.count; i++)
        if (!live[i] && (NSInteger)i != _selected) [self suspendWebOf:_tabs[i] keepRecord:YES];
}

- (void)setLiveLimit:(NSInteger)liveLimit {
    _liveLimit = liveLimit;
    [self trimLivePages];
}

- (void)reloadStrip {
    NSArray<NSString *> *titles = [self tabTitles];
    NSMutableArray<MCTabStripItem *> *items = [NSMutableArray array];
    for (NSUInteger i = 0; i < _tabs.count; i++) {
        MCTabStripItem *item = [MCTabStripItem new];
        item.title = titles[i];
        NSString *url = [self addressOf:_tabs[i]];
        item.toolTip = url.length ? url : @"New tab";
        [items addObject:item];
    }
    self.strip.items = items;
    // The agent page on show: none of the user's tabs is.
    self.strip.activeIndex = self.agentPageShown ? -1 : _selected;
    self.strip.needsDisplay = YES;
}

// Whether the keyboard is in the user's page on show (not the toolbar).
- (BOOL)keyboardInUserPage {
    NSResponder *r = self.window.firstResponder;
    return self.web && [r isKindOfClass:NSView.class] && [(NSView *)r isDescendantOf:self.web];
}

// Shows tab i (making its page live) and lets the pages beyond the limit
// go. The keyboard, when it was in the page on show, goes to the new one.
- (void)selectTab:(NSUInteger)i {
    if (i >= _tabs.count) return;
    const BOOL pageHadKeyboard = [self keyboardInUserPage] && !self.agentPageShown;
    MCBrowserTab *t = _tabs[i];
    t.lastUsed = ++_clock;
    _selected = (NSInteger)i;
    [self makeLive:t configuration:nil load:YES];
    for (MCBrowserTab *o in _tabs) o.web.hidden = YES;
    [self trimLivePages];
    [self showPage:NO];
    if (!self.urlBar.currentEditor || pageHadKeyboard) self.urlBar.stringValue = [self addressOf:t];
    else self.urlBar.currentEditor.string = [self addressOf:t];
    if (pageHadKeyboard) [self giveKeyboardToPage];
}

- (void)giveKeyboardToPage {
    MCWebView *web = self.web;
    if (!web || web.isHiddenOrHasHiddenAncestor) return;
    web.grantKeyboard = YES;
    [self.window makeFirstResponder:web];
    web.grantKeyboard = NO;
}

// A click on a tab: it shows and takes the keyboard, its page when it has
// one, else the address bar.
- (void)stripSelected:(NSInteger)i {
    [self selectTab:(NSUInteger)i];
    if (self.web.URL) [self giveKeyboardToPage];
    else [self focusURLBar];
}

// A new tab right of the one on show, shown, empty; its address bar is
// the place to type (the caller gives it the keyboard).
- (void)newTab {
    MCBrowserTab *t = [self makeTab];
    const int at = EditorTabs::insertionIndex((int)_selected, (int)_tabs.count);
    [_tabs insertObject:t atIndex:(NSUInteger)at];
    [self selectTab:(NSUInteger)at];
}

// A tab for `url` right of the one on show, behind it: nothing loads until
// it is shown.
- (void)openInBackgroundTab:(NSURL *)url {
    MCBrowserTab *t = [self makeTab];
    t.url = url.absoluteString;
    const int at = EditorTabs::insertionIndex((int)_selected, (int)_tabs.count);
    [_tabs insertObject:t atIndex:(NSUInteger)at];
    [self reloadStrip];
}

// Closes tab i; the one used most recently shows next (EditorTabs). The
// last tab closing leaves an empty one and tells the owner (onEmpty), which
// hides the pane. The keyboard in the closed page goes to the next one.
- (void)closeTab:(NSUInteger)i {
    if (i >= _tabs.count) return;
    const BOOL closingShown = (NSInteger)i == _selected;
    const BOOL pageHadKeyboard = closingShown && [self keyboardInUserPage];
    std::vector<uint64_t> used;
    for (MCBrowserTab *t in _tabs) used.push_back(t.lastUsed);
    const int next = EditorTabs::activeAfterClose(used, (int)_selected, (int)i);
    MCBrowserTab *gone = _tabs[i];
    if (pageHadKeyboard) [self.window makeFirstResponder:self.urlBar];
    [self discardTab:gone];
    [_tabs removeObjectAtIndex:i];
    if (_tabs.count == 0) {
        [_tabs addObject:[self makeTab]];
        _selected = -1;
        [self selectTab:0];
        if (self.onEmpty) self.onEmpty();
        return;
    }
    if (!closingShown) {
        _selected = next;
        [self reloadStrip];
        return;
    }
    _selected = -1;
    [self selectTab:(NSUInteger)MAX(0, next)];
    if (pageHadKeyboard) [self giveKeyboardToPage];
}

- (void)closeOtherTabs:(NSUInteger)keep {
    if (keep >= _tabs.count) return;
    MCBrowserTab *k = _tabs[keep];
    for (MCBrowserTab *t in [_tabs copy])
        if (t != k) [self discardTab:t];
    [_tabs setArray:@[k]];
    _selected = -1;
    [self selectTab:0];
}

- (BOOL)moveTab:(NSUInteger)from to:(NSUInteger)to {
    if (from >= _tabs.count || to >= _tabs.count || from == to) return NO;
    MCBrowserTab *shown = self.activeTab;
    MCBrowserTab *t = _tabs[from];
    [_tabs removeObjectAtIndex:from];
    [_tabs insertObject:t atIndex:to];
    _selected = (NSInteger)[_tabs indexOfObject:shown];
    [self reloadStrip];
    return YES;
}

- (NSMenu *)menuForTab:(NSUInteger)i {
    NSMenu *m = [[NSMenu alloc] initWithTitle:@""];
    NSMenuItem *(^add)(NSString *, SEL) = ^NSMenuItem *(NSString *title, SEL sel) {
        NSMenuItem *it = [m addItemWithTitle:title action:sel keyEquivalent:@""];
        it.target = self;
        it.tag = (NSInteger)i;
        return it;
    };
    add(@"New Tab", @selector(newTabFromMenu:));
    add(@"Reload", @selector(reloadTabFromMenu:));
    add(@"Copy Address", @selector(copyTabAddress:)).enabled = [self addressOf:_tabs[i]].length > 0;
    [m addItem:[NSMenuItem separatorItem]];
    add(@"Close Tab", @selector(closeTabFromMenu:));
    add(@"Close Other Tabs", @selector(closeOtherTabsFromMenu:));
    return m;
}

- (void)newTabFromMenu:(NSMenuItem *)item {
    (void)item;
    [self newTab];
    [self focusURLBar];
}
- (void)reloadTabFromMenu:(NSMenuItem *)item {
    if ((NSInteger)item.tag != _selected) [self selectTab:(NSUInteger)item.tag];
    self.web.toolbarNavigation = NO;
    [self.web reload];
}
- (void)copyTabAddress:(NSMenuItem *)item {
    if ((NSUInteger)item.tag >= _tabs.count) return;
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    [pb clearContents];
    [pb setString:[self addressOf:_tabs[(NSUInteger)item.tag]] forType:NSPasteboardTypeString];
}
- (void)closeTabFromMenu:(NSMenuItem *)item { [self closeTab:(NSUInteger)item.tag]; }
- (void)closeOtherTabsFromMenu:(NSMenuItem *)item { [self closeOtherTabs:(NSUInteger)item.tag]; }

// ------------------------------------- keys, while the pane has the keyboard
// The pane is in the responder chain of whatever in it has the keyboard, so
// the menu's actions reach it before the window's controller: Cmd+T (the
// registry's page key, minicode.browser.newTab) opens a tab, Cmd+W closes
// the page on show, Ctrl+Tab, Shift+Cmd+] and [ and Ctrl+1 to 9 move
// between tabs. With one tab those last go on to the editor's tabs, as
// before the pane had tabs.
- (void)newBrowserTab:(id)sender {
    (void)sender;
    [self newTab];
    [self focusURLBar];
}

- (BOOL)isRepeatKey {
    NSEvent *e = NSApp.currentEvent;
    return e.type == NSEventTypeKeyDown && e.isARepeat;
}

// Cmd+W: the agent page when it shows, else the tab on show; the pane hides
// with the last tab (onEmpty). A held key closes one.
- (void)closeFocused:(id)sender {
    (void)sender;
    if ([self isRepeatKey]) return;
    if (self.agentPageShown) {
        [self closeAgentPage];
        return;
    }
    [self closeTab:(NSUInteger)_selected];
}

- (BOOL)passOn:(SEL)action from:(id)sender {
    id next = self.window.delegate;
    if (![next respondsToSelector:action]) return NO;
    return [NSApp sendAction:action to:next from:sender];
}

- (void)selectNextTab:(id)sender {
    if (_tabs.count < 2 || self.agentPageShown) { [self passOn:_cmd from:sender]; return; }
    [self selectTab:(NSUInteger)EditorTabs::step((int)_selected, (int)_tabs.count, 1)];
}
- (void)selectPreviousTab:(id)sender {
    if (_tabs.count < 2 || self.agentPageShown) { [self passOn:_cmd from:sender]; return; }
    [self selectTab:(NSUInteger)EditorTabs::step((int)_selected, (int)_tabs.count, -1)];
}
- (void)selectTabNumber:(id)sender {
    if (_tabs.count < 2 || self.agentPageShown) { [self passOn:_cmd from:sender]; return; }
    const int i = EditorTabs::indexForNumber((int)[sender tag], (int)_tabs.count);
    if (i >= 0) [self selectTab:(NSUInteger)i];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    const SEL a = item.action;
    if (a == @selector(closeFocused:)) {
        item.title = self.agentPageShown ? @"Close Agent Page"
                     : _tabs.count > 1   ? @"Close Browser Tab"
                                         : @"Close Browser";
        return YES;
    }
    if (a == @selector(selectNextTab:) || a == @selector(selectPreviousTab:) ||
        a == @selector(selectTabNumber:)) {
        if (_tabs.count < 2 || self.agentPageShown) {
            id next = self.window.delegate;
            return [next respondsToSelector:@selector(validateMenuItem:)] &&
                   [(id<NSMenuItemValidation>)next validateMenuItem:item];
        }
        if (a == @selector(selectTabNumber:))
            return EditorTabs::indexForNumber((int)item.tag, (int)_tabs.count) >= 0;
        return YES;
    }
    if (a == @selector(reloadTabFromMenu:) || a == @selector(closeTabFromMenu:) ||
        a == @selector(closeOtherTabsFromMenu:) || a == @selector(copyTabAddress:))
        return (NSUInteger)item.tag < _tabs.count &&
               (a != @selector(copyTabAddress:) || [self addressOf:_tabs[(NSUInteger)item.tag]].length) &&
               (a != @selector(closeOtherTabsFromMenu:) || _tabs.count > 1);
    return YES;
}

// ------------------------------------------------------------ the pages
// The page on show, its toolbar state and the switcher.
- (void)showPage:(BOOL)agent {
    self.agentPageShown = agent && self.agentWeb != nil;
    for (MCBrowserTab *t in _tabs) t.web.hidden = self.agentPageShown || t != self.activeTab;
    self.agentWeb.hidden = !self.agentPageShown;
    self.switcher.hidden = self.closeAgent.hidden = self.agentWeb == nil;
    self.switcher.selectedSegment = self.agentPageShown ? 1 : 0;
    [self updateKeyViewLoop];
    [self updateToolbar];
    [self reloadStrip];
    [self layoutChildren];
}

- (void)updateToolbar {
    MCWebView *wv = self.shownWeb;
    // An address being typed stays.
    if (!self.urlBar.currentEditor) self.urlBar.stringValue = wv.URL.absoluteString ?: @"";
    self.back.enabled = wv.canGoBack;
    self.forward.enabled = wv.canGoForward;
}

- (MCPageProbe *)openAgentPage {
    if (self.agentWeb) return self.agentProbe;
    self.agentProbe = [[MCPageProbe alloc] initRecordingConsole:YES];
    self.agentWeb = [self makeWebViewWith:[MCPageProbe agentPageConfigurationFor:self.agentProbe]];
    self.agentProbe.webView = self.agentWeb;
    self.agentWeb.hidden = YES;
    [self addSubview:self.agentWeb positioned:NSWindowBelow relativeTo:nil];
    [self showPage:self.agentPageShown];
    return self.agentProbe;
}

- (WKNavigation *)loadInAgentPage:(NSString *)url {
    [self openAgentPage];
    NSURL *u = [NSURL URLWithString:url];
    if (!u) return nil;
    self.agentWeb.toolbarNavigation = NO;
    if (u.isFileURL)
        return [self.agentWeb loadFileURL:u allowingReadAccessToURL:u.URLByDeletingLastPathComponent];
    return [self.agentWeb loadRequest:[NSURLRequest requestWithURL:u]];
}

- (WKNavigation *)reloadAgentPage {
    self.agentWeb.toolbarNavigation = NO;
    return self.agentWeb.URL ? [self.agentWeb reload] : nil;
}

- (WKNavigation *)reloadUserPage {
    self.web.toolbarNavigation = NO;
    return self.web.URL ? [self.web reload] : nil;
}

// Whether the keyboard is in the pane: a page, the address bar (its field
// editor), or a button.
- (BOOL)keyboardInPane {
    NSResponder *r = self.window.firstResponder;
    if ([r isKindOfClass:NSTextView.class] && [(NSTextView *)r isFieldEditor] &&
        [[(NSTextView *)r delegate] isKindOfClass:NSView.class])
        r = (NSView *)[(NSTextView *)r delegate];
    return [r isKindOfClass:NSView.class] && [(NSView *)r isDescendantOf:self];
}

- (BOOL)showAgentPageQuietly {
    if (!self.agentWeb) return NO;
    if (self.agentPageShown) return YES;
    if (!self.hiddenOrHasHiddenAncestor && [self keyboardInPane]) return NO;
    [self showPage:YES];
    return YES;
}

- (void)showUserPage { [self showPage:NO]; }

- (void)closeAgentPage {
    if (!self.agentWeb) return;
    // The keyboard in the agent page goes to the user's, which it shows.
    const BOOL hadKeyboard = [self keyboardInPane] &&
        [self.window.firstResponder isKindOfClass:NSView.class] &&
        [(NSView *)self.window.firstResponder isDescendantOf:self.agentWeb];
    [self.agentProbe detach];
    [self.agentWeb stopLoading];
    [self.agentWeb removeFromSuperview];
    self.agentWeb.navigationDelegate = nil;
    self.agentWeb = nil;
    self.agentProbe = nil;
    [self showPage:NO];
    if (hadKeyboard) [self.window makeFirstResponder:self.urlBar];
}

- (void)closeAgentPage:(id)sender {
    (void)sender;
    [self closeAgentPage];
}

- (void)switcherChanged:(id)sender {
    (void)sender;
    [self showPage:self.switcher.selectedSegment == 1];
}

- (BOOL)showsUserPage {
    return !self.agentPageShown && self.web.backForwardList.currentItem != nil;
}

// -------------------------------------------------------------- navigation
// From outside (a link in the terminal or a Markdown file): the user's
// page, which shows; it may not take the keyboard when it loads.
- (void)navigateToString:(NSString *)raw {
    self.web.toolbarNavigation = NO;
    if (self.agentPageShown) [self showPage:NO];
    [self loadString:raw];
}

- (WKNavigation *)loadString:(NSString *)raw {
    NSString *s =[raw stringByTrimmingCharactersInSet:
                   [NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (s.length == 0) return nil;
    NSString *urlStr = s;
    BOOL hasScheme = [s hasPrefix:@"http://"] || [s hasPrefix:@"https://"] ||
                     [s hasPrefix:@"file://"];
    BOOL looksLikeDomain = [s containsString:@"."] && ![s containsString:@" "];
    if (!hasScheme) {
        if (looksLikeDomain)
            urlStr = [@"https://" stringByAppendingString:s];
        else  // treat as a web search
            urlStr = [@"https://duckduckgo.com/?q="
                stringByAppendingString:
                    [s stringByAddingPercentEncodingWithAllowedCharacters:
                        [NSCharacterSet URLQueryAllowedCharacterSet]]];
    }
    NSURL *url = [NSURL URLWithString:urlStr];
    return url ? [self.web loadRequest:[NSURLRequest requestWithURL:url]] : nil;
}

// From the toolbar: an address typed there is the user's, so it loads in
// their page, which shows. The page asked for may focus its own field, as
// long as the keyboard is still in the toolbar when it does.
- (void)urlEntered:(id)sender {
    (void)sender;
    if (self.agentPageShown) [self showPage:NO];
    self.web.toolbarNavigation = YES;
    [self loadString:self.urlBar.stringValue];
}
// Tab and Shift+Tab in the address bar go into the page. Moved here, while
// the field editor still has the keyboard: when NSTextField moves on after
// editing ends, the window itself is first responder, so the page cannot
// tell the Tab came from the toolbar and refuses it, and the keyboard was
// left with the window.
- (BOOL)control:(NSControl *)control textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    (void)tv;
    if (control != self.urlBar) return NO;
    if (sel == @selector(insertTab:)) {
        [self.window selectKeyViewFollowingView:self.urlBar];
        return YES;
    }
    if (sel == @selector(insertBacktab:)) {
        [self.window selectKeyViewPrecedingView:self.urlBar];
        return YES;
    }
    return NO;
}

- (void)goBack:(id)sender     { self.shownWeb.toolbarNavigation = YES; [self.shownWeb goBack]; }
- (void)goForward:(id)sender  { self.shownWeb.toolbarNavigation = YES; [self.shownWeb goForward]; }
- (void)reloadPage:(id)sender { self.shownWeb.toolbarNavigation = YES; [self.shownWeb reload]; }

- (MCBrowserTab *)tabFor:(WKWebView *)wv {
    if (!wv) return nil;
    for (MCBrowserTab *t in _tabs)
        if (t.web == wv) return t;
    return nil;
}

- (MCPageProbe *)probeFor:(WKWebView *)wv {
    return wv == self.agentWeb ? self.agentProbe : [self tabFor:wv].probe;
}

// A link Command-clicked or middle-clicked in one of the user's pages
// opens in a new tab behind the one on show, as in Safari; everything else
// loads where it was asked.
- (void)webView:(WKWebView *)wv decidePolicyForNavigationAction:(WKNavigationAction *)action
    decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    const BOOL newTab = action.navigationType == WKNavigationTypeLinkActivated &&
                        [self tabFor:wv] != nil &&
                        ((action.modifierFlags & NSEventModifierFlagCommand) ||
                         action.buttonNumber == 2) &&
                        action.request.URL;
    if (newTab) {
        [self openInBackgroundTab:action.request.URL];
        decisionHandler(WKNavigationActionPolicyCancel);
        return;
    }
    decisionHandler(WKNavigationActionPolicyAllow);
}

// A page of the user's opening another (a target=_blank link, window.open):
// a new tab right of it, shown, with the page WebKit asked for, so the
// opener keeps its link to it.
- (WKWebView *)webView:(WKWebView *)wv createWebViewWithConfiguration:(WKWebViewConfiguration *)cfg
       forNavigationAction:(WKNavigationAction *)action windowFeatures:(WKWindowFeatures *)features {
    (void)action; (void)features;
    if (![self tabFor:wv]) return nil;
    const BOOL pageHadKeyboard = [self keyboardInUserPage];
    MCBrowserTab *t = [self makeTab];
    [self makeLive:t configuration:cfg load:NO];
    const int at = EditorTabs::insertionIndex((int)_selected, (int)_tabs.count);
    [_tabs insertObject:t atIndex:(NSUInteger)at];
    [self selectTab:(NSUInteger)at];
    if (pageHadKeyboard) [self giveKeyboardToPage];
    return t.web;
}

// window.close() from a page a script opened.
- (void)webViewDidClose:(WKWebView *)wv {
    MCBrowserTab *t = [self tabFor:wv];
    if (t) [self closeTab:[_tabs indexOfObject:t]];
}

// The HTTP status of each page's document comes from here. What a page may
// show is what WebKit allows without this method: what it can display.
- (void)webView:(WKWebView *)wv decidePolicyForNavigationResponse:(WKNavigationResponse *)response
    decisionHandler:(void (^)(WKNavigationResponsePolicy))decisionHandler {
    [[self probeFor:wv] noteResponse:response];
    decisionHandler(response.canShowMIMEType ? WKNavigationResponsePolicyAllow
                                             : WKNavigationResponsePolicyCancel);
}

- (void)webView:(WKWebView *)wv didCommitNavigation:(WKNavigation *)nav {
    [[self probeFor:wv] noteCommit:nav];
    // The user's pages take the zoom their site was left at.
    if ([self tabFor:wv]) {
        const CGFloat z = [self rememberedZoomFor:wv.URL];
        if (fabs(wv.pageZoom - z) > 0.001) wv.pageZoom = z;
        [self reloadStrip];
    }
    if (wv == self.shownWeb) [self updateToolbar];
}

// ------------------------------------------------------------------ zoom
static NSString *const kZoomKey = @"MCBrowserZoom";   // site -> zoom
static const NSUInteger kMaxZoomSites = 200;

// A site, for the zoom it keeps: the host, or one entry for local files.
static NSString *MCZoomSite(NSURL *u) {
    if (!u) return nil;
    if (u.isFileURL) return @"file:";
    return u.host.length ? u.host.lowercaseString : nil;
}

- (CGFloat)rememberedZoomFor:(NSURL *)url {
    NSString *site = MCZoomSite(url);
    id z = site ? [MCAppDefaults() dictionaryForKey:kZoomKey][site] : nil;
    const double v = [z isKindOfClass:NSNumber.class] ? [z doubleValue] : 1.0;
    return v >= kMCZoomFitMin && v <= 3.0 ? v : 1.0;
}

- (void)rememberZoomOf:(MCWebView *)wv {
    if (![self tabFor:wv]) return;   // the agent page's is its own
    NSString *site = MCZoomSite(wv.URL);
    if (!site) return;
    NSMutableDictionary *all = [[MCAppDefaults() dictionaryForKey:kZoomKey] mutableCopy]
                               ?: [NSMutableDictionary dictionary];
    if (fabs(wv.pageZoom - 1.0) < 0.001) [all removeObjectForKey:site];
    else all[site] = @(wv.pageZoom);
    while (all.count > kMaxZoomSites) [all removeObjectForKey:all.allKeys.firstObject];
    [MCAppDefaults() setObject:all forKey:kZoomKey];
    // The site's other live pages follow.
    for (MCBrowserTab *t in _tabs)
        if (t.web && t.web != wv && [MCZoomSite(t.web.URL) isEqualToString:site])
            t.web.pageZoom = wv.pageZoom;
}

- (CGFloat)pageZoom { return self.shownWeb.pageZoom; }

- (void)setZoom:(CGFloat)zoom of:(MCWebView *)wv {
    wv.pageZoom = zoom;
    [self rememberZoomOf:wv];
    [self flashZoom];
}

- (void)flashZoom {
    self.zoomLabel.stringValue =
        [NSString stringWithFormat:@"%.0f%%", round(self.shownWeb.pageZoom * 100)];
    self.zoomLabel.hidden = NO;
    const NSUInteger gen = ++self.zoomLabelGeneration;
    __weak BrowserView *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        BrowserView *s = weakSelf;
        if (s && s.zoomLabelGeneration == gen) s.zoomLabel.hidden = YES;
    });
}

- (void)zoomStep:(int)dir {
    MCWebView *wv = self.shownWeb;
    const CGFloat z = MCZoomStep(wv.pageZoom, dir);
    if (dir != 0 && fabs(z - wv.pageZoom) < 0.001) NSBeep();   // at an end
    [self setZoom:z of:wv];
}

// The page at actual size first, since a page laid out for a narrower
// view (zoomed in) or a wider one (zoomed out) measures differently; then
// as small as it takes for what is widest to show.
- (void)zoomToFitWidth {
    MCWebView *wv = self.shownWeb;
    MCPageProbe *probe = [self probeFor:wv];
    wv.pageZoom = 1.0;
    __weak BrowserView *weakSelf = self;
    __weak MCWebView *weakWeb = wv;
    [probe readWidths:^(double shown, double content, NSString *error) {
        BrowserView *s = weakSelf;
        MCWebView *w = weakWeb;
        if (!s || !w) return;
        CGFloat z = 1.0;
        // Rounded down, so a fraction of a pixel never leaves a scroll bar.
        if (!error && shown > 0 && content > shown)
            z = MAX(kMCZoomFitMin, floor(shown / content * 1000) / 1000);
        [s setZoom:z of:w];
    }];
}

- (void)webView:(WKWebView *)wv didFinishNavigation:(WKNavigation *)nav {
    [[self probeFor:wv] noteFinish:nav];
    if (wv == self.shownWeb) [self updateToolbar];
}

- (void)webView:(WKWebView *)wv didFailNavigation:(WKNavigation *)nav withError:(NSError *)error {
    [[self probeFor:wv] noteFail:nav error:error];
}

- (void)webView:(WKWebView *)wv didFailProvisionalNavigation:(WKNavigation *)nav
      withError:(NSError *)error {
    [[self probeFor:wv] noteFail:nav error:error];
}

@end
