// Browser.mm — embedded browser panel. Objective-C++.
#import "Browser.h"
#import <WebKit/WebKit.h>

static NSColor *BHex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8)  & 0xFF) / 255.0
                                blue:( rgb        & 0xFF) / 255.0
                               alpha:1.0];
}

@interface BrowserView () <WKNavigationDelegate, NSTextFieldDelegate>
@property(nonatomic, strong) WKWebView   *web;
@property(nonatomic, strong) NSTextField *urlBar;
@property(nonatomic, strong) NSButton    *back;
@property(nonatomic, strong) NSButton    *forward;
@property(nonatomic, strong) NSButton    *reload;
@end

@implementation BrowserView

- (instancetype)initWithHomeURL:(NSString *)url {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 600, 400)])) {
        self.wantsLayer = YES;
        self.layer.backgroundColor = BHex(0x2A2A2A).CGColor;
        [self buildChrome];
        [self navigateToString:url];
    }
    return self;
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

    self.urlBar = [[NSTextField alloc] init];
    self.urlBar.font = [NSFont systemFontOfSize:12];
    self.urlBar.placeholderString = @"Enter URL and press Return";
    self.urlBar.delegate = self;
    self.urlBar.target = self;
    self.urlBar.action = @selector(urlEntered:);
    [self addSubview:self.urlBar];

    WKWebViewConfiguration *cfg = [[WKWebViewConfiguration alloc] init];
    self.web = [[WKWebView alloc] initWithFrame:self.bounds configuration:cfg];
    self.web.navigationDelegate = self;
    [self addSubview:self.web];
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
- (void)navigateToString:(NSString *)raw {
    NSString *s = [raw stringByTrimmingCharactersInSet:
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

- (void)urlEntered:(id)sender { [self navigateToString:self.urlBar.stringValue]; }
- (void)goBack:(id)sender     { [self.web goBack]; }
- (void)goForward:(id)sender  { [self.web goForward]; }
- (void)reloadPage:(id)sender { [self.web reload]; }

- (void)webView:(WKWebView *)wv didCommitNavigation:(WKNavigation *)nav {
    self.urlBar.stringValue = wv.URL.absoluteString ?: @"";
    self.back.enabled = wv.canGoBack;
    self.forward.enabled = wv.canGoForward;
}

@end
