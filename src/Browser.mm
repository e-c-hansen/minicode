// Browser.mm — embedded browser panel. Objective-C++.
#import "Browser.h"
#import "AppSettings.h"
#import "Appearance.h"
#import "BrowserPages.h"
#import "PageZoom.h"
#import "TreeDetails.h"   // MCAppDefaults
#import <WebKit/WebKit.h>

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
// while hidden.
@interface MCWebView : WKWebView
@property(nonatomic, weak) NSView *chrome;          // the BrowserView around it
@property(nonatomic) BOOL toolbarNavigation;        // the last load came from the toolbar
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

@interface BrowserView () <WKNavigationDelegate, NSTextFieldDelegate>
@property(nonatomic, strong) MCWebView   *web;        // the user's page
@property(nonatomic, strong) MCWebView   *agentWeb;   // the agent's, once it loaded one
@property(nonatomic, strong) MCPageProbe *userProbe;
@property(nonatomic, strong) MCPageProbe *agentProbe;
@property(nonatomic) BOOL agentPageShown;
@property(nonatomic, strong) NSTextField *urlBar;
@property(nonatomic, strong) NSButton    *back;
@property(nonatomic, strong) NSButton    *forward;
@property(nonatomic, strong) NSButton    *reload;
@property(nonatomic, strong) NSSegmentedControl *switcher;   // Yours | Agent
@property(nonatomic, strong) NSButton    *closeAgent;
@property(nonatomic, strong) NSTextField *zoomLabel;
@property(nonatomic) NSUInteger zoomLabelGeneration;
@end

@implementation BrowserView
- (MCWebView *)shownWeb { return self.agentPageShown && self.agentWeb ? self.agentWeb : self.web; }
- (NSString *)pageURL { return self.shownWeb.URL.absoluteString; }
- (NSString *)pageTitle { return self.shownWeb.title.length ? self.shownWeb.title : nil; }
- (BOOL)hasAgentPage { return self.agentWeb != nil; }

// A record past this size is left out; the address still comes back.
static const NSUInteger kMaxBrowserState = 2 * 1024 * 1024;

- (NSDictionary *)savedState {
    NSMutableDictionary *s = [NSMutableDictionary dictionary];
    NSString *url = self.web.URL.absoluteString;
    if (url.length) s[@"url"] = url;
    if (@available(macOS 12.0, *)) {
        id state = self.web.interactionState;
        NSData *data = nil;
        BOOL archived = NO;
        if ([state isKindOfClass:NSData.class]) {
            data = state;
        } else if (state) {
            data = [NSKeyedArchiver archivedDataWithRootObject:state requiringSecureCoding:NO
                                                         error:nil];
            archived = YES;
        }
        if (data.length && data.length <= kMaxBrowserState) {
            s[@"state"] = data;
            s[@"archived"] = @(archived);
        }
    }
    return s.count ? s : nil;
}

- (void)restoreState:(NSDictionary *)state {
    if (![state isKindOfClass:NSDictionary.class]) return;
    NSString *url = [state[@"url"] isKindOfClass:NSString.class] ? state[@"url"] : nil;
    NSData *data = [state[@"state"] isKindOfClass:NSData.class] ? state[@"state"] : nil;
    self.web.toolbarNavigation = NO;   // a restored page never takes the keyboard
    if (@available(macOS 12.0, *)) {
        if (data) {
            id record = data;
            if ([state[@"archived"] boolValue]) {
                NSKeyedUnarchiver *u = [[NSKeyedUnarchiver alloc] initForReadingFromData:data
                                                                                  error:nil];
                u.requiresSecureCoding = NO;
                record = [u decodeObjectForKey:NSKeyedArchiveRootObjectKey];
                [u finishDecoding];
            }
            @try {
                if (record) self.web.interactionState = record;
            } @catch (NSException *e) {
                (void)e;   // a record from another WebKit: the address will do
            }
            if (self.web.URL || self.web.isLoading) return;
        }
    }
    if (url.length) [self loadString:url];
}

- (instancetype)initWithHomeURL:(NSString *)url {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 600, 400)])) {
        self.wantsLayer = YES;
        [self buildChrome];
        [self applySettings];
        [[NSNotificationCenter defaultCenter]
            addObserver:self selector:@selector(applySettings)
                   name:MCSettingsDidChangeNotification object:nil];
        [self navigateToString:url];
    }
    return self;
}

- (void)dealloc {
    [self.agentProbe detach];
}

// The toolbar follows the settings file. The URL field keeps the system's
// colors unless a text color is set, since its background is the system's.
- (void)applySettings {
    AppSettings *cfg = [AppSettings shared];
    self.layer.backgroundColor = [cfg background:Surface::Browser].CGColor;
    self.urlBar.textColor = cfg.settings.textIsSet(Surface::Browser)
        ? [cfg text:Surface::Browser] : [NSColor controlTextColor];
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
    MCWebView *web = [[MCWebView alloc] initWithFrame:self.bounds configuration:cfg];
    web.chrome = self;
    web.navigationDelegate = self;
    return web;
}

- (void)buildChrome {
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

    self.web = [self makeWebViewWith:[[WKWebViewConfiguration alloc] init]];
    self.userProbe = [[MCPageProbe alloc] initRecordingConsole:NO];
    self.userProbe.webView = self.web;
    [self addSubview:self.web];
    [self updateKeyViewLoop];
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

- (void)layoutChildren {
    CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
    const CGFloat barH = 36, bw = 34, pad = 6;
    CGFloat x = pad;
    CGFloat by = H - barH + 4;
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
    const NSRect page = NSMakeRect(0, 0, W, MAX(0, H - barH));
    self.web.frame = page;
    self.agentWeb.frame = page;   // hidden pages keep their size
}

- (void)focusURLBar { [self.window makeFirstResponder:self.urlBar]; }

// ------------------------------------------------------------ the pages
// The page on show, its toolbar state and the switcher.
- (void)showPage:(BOOL)agent {
    self.agentPageShown = agent && self.agentWeb != nil;
    self.web.hidden = self.agentPageShown;
    self.agentWeb.hidden = !self.agentPageShown;
    self.switcher.hidden = self.closeAgent.hidden = self.agentWeb == nil;
    self.switcher.selectedSegment = self.agentPageShown ? 1 : 0;
    [self updateKeyViewLoop];
    [self updateToolbar];
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
    [self addSubview:self.agentWeb];
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

- (MCPageProbe *)probeFor:(WKWebView *)wv {
    return wv == self.agentWeb ? self.agentProbe : wv == self.web ? self.userProbe : nil;
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
    // The user's page takes the zoom its site was left at.
    if (wv == self.web) {
        const CGFloat z = [self rememberedZoomFor:wv.URL];
        if (fabs(wv.pageZoom - z) > 0.001) wv.pageZoom = z;
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
    if (wv != self.web) return;   // the agent page's is its own
    NSString *site = MCZoomSite(wv.URL);
    if (!site) return;
    NSMutableDictionary *all = [[MCAppDefaults() dictionaryForKey:kZoomKey] mutableCopy]
                               ?: [NSMutableDictionary dictionary];
    if (fabs(wv.pageZoom - 1.0) < 0.001) [all removeObjectForKey:site];
    else all[site] = @(wv.pageZoom);
    while (all.count > kMaxZoomSites) [all removeObjectForKey:all.allKeys.firstObject];
    [MCAppDefaults() setObject:all forKey:kZoomKey];
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
