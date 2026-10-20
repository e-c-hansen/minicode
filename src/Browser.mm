// Browser.mm — embedded browser panel. Objective-C++.
#import "Browser.h"
#import "AppSettings.h"
#import "Appearance.h"
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
@property(nonatomic, strong) MCWebView   *web;
@property(nonatomic, strong) NSTextField *urlBar;
@property(nonatomic, strong) NSButton    *back;
@property(nonatomic, strong) NSButton    *forward;
@property(nonatomic, strong) NSButton    *reload;
@end

@implementation BrowserView

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

- (void)buildChrome {
    self.back    = [self navButton:@"◀" action:@selector(goBack:)];
    self.forward = [self navButton:@"▶" action:@selector(goForward:)];
    self.reload  = [self navButton:@"⟳" action:@selector(reloadPage:)];
    [self addSubview:self.back];
    [self addSubview:self.forward];
    [self addSubview:self.reload];

    MCSurfaceTextField *urlBar = [[MCSurfaceTextField alloc] init];
    urlBar.appearanceSurface = @"browser";
    self.urlBar = urlBar;
    self.urlBar.font = [NSFont systemFontOfSize:12];
    self.urlBar.placeholderString = @"Enter URL and press Return";
    self.urlBar.delegate = self;
    self.urlBar.target = self;
    self.urlBar.action = @selector(urlEntered:);
    [self addSubview:self.urlBar];

    WKWebViewConfiguration *cfg = [[WKWebViewConfiguration alloc] init];
    self.web = [[MCWebView alloc] initWithFrame:self.bounds configuration:cfg];
    self.web.chrome = self;
    self.web.navigationDelegate = self;
    [self addSubview:self.web];
    // Tab from the address bar goes into the page (MCWebView lets a Tab out
    // of the toolbar in), and out of the page's last field back to the
    // toolbar. The buttons are key views only with full keyboard access on;
    // otherwise the loop passes them by.
    self.urlBar.nextKeyView = self.web;
    self.web.nextKeyView = self.back;
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
    self.urlBar.frame  = NSMakeRect(x, by, MAX(0, W - x - pad), 24);
    self.web.frame     = NSMakeRect(0, 0, W, MAX(0, H - barH));
}

- (void)focusURLBar { [self.window makeFirstResponder:self.urlBar]; }

// -------------------------------------------------------------- navigation
// From outside (a link in the terminal or a Markdown file): the page may not
// take the keyboard when it loads.
- (void)navigateToString:(NSString *)raw {
    self.web.toolbarNavigation = NO;
    [self loadString:raw];
}

- (void)loadString:(NSString *)raw {
    NSString *s =[raw stringByTrimmingCharactersInSet:
                   [NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (s.length == 0) return;
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
    if (url) [self.web loadRequest:[NSURLRequest requestWithURL:url]];
}

// From the toolbar: the page asked for may focus its own field, as long as
// the keyboard is still in the toolbar when it does.
- (void)urlEntered:(id)sender {
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

- (void)goBack:(id)sender     { self.web.toolbarNavigation = YES; [self.web goBack]; }
- (void)goForward:(id)sender  { self.web.toolbarNavigation = YES; [self.web goForward]; }
- (void)reloadPage:(id)sender { self.web.toolbarNavigation = YES; [self.web reload]; }

- (void)webView:(WKWebView *)wv didCommitNavigation:(WKNavigation *)nav {
    self.urlBar.stringValue = wv.URL.absoluteString ?: @"";
    self.back.enabled = wv.canGoBack;
    self.forward.enabled = wv.canGoForward;
}

@end
