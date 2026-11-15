// HtmlPage.mm — see HtmlPage.h.
#import "HtmlPage.h"
#import "AppSettings.h"
#import "Browser.h"
#import "EditorTab.h"   // MCStampOfFile
#include "FileSniff.h"
#include "Settings.h"
#include <string>

static const CGFloat kQuestionBarHeight = 34;

// The answers not remembered in the settings file, by file, until quit.
static NSMutableDictionary<NSString *, NSNumber *> *ScriptAnswers() {
    static NSMutableDictionary *answers;
    if (!answers) answers = [NSMutableDictionary dictionary];
    return answers;
}

static BOOL SameStamp(MCDiskStamp *a, MCDiskStamp *b) {
    if (!a || !b) return a == b;
    return [a.modDate isEqualToDate:b.modDate] && a.fileID == b.fileID && a.size == b.size;
}

// An address without its #fragment.
static NSURL *WithoutFragment(NSURL *url) {
    if (!url) return nil;
    NSURLComponents *c = [NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
    c.fragment = nil;
    return c.URL ?: url;
}

@interface MCHtmlPage () <WKNavigationDelegate, WKUIDelegate>
@property(nonatomic, strong, readwrite) MCWebView *webView;
@property(nonatomic, copy, readwrite) NSString *path;
@property(nonatomic, readwrite) BOOL runsScripts;
@property(nonatomic, readwrite) BOOL asking;
@property(nonatomic, strong, readwrite) NSView *questionBar;
@property(nonatomic, strong, readwrite) NSButton *rememberBox;
@property(nonatomic, readwrite) NSUInteger loads;
@end

@implementation MCHtmlPage {
    MCDiskStamp *_stamp;      // the file as last loaded
    BOOL _hasScripts;         // its text holds something that would run
    NSUInteger _generation;   // loads asked for; a scroll read for an older one is dropped
    BOOL _restoreScroll;
    double _scrollX, _scrollY;
    NSTextField *_question;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        [self makeQuestionBar];
        [[NSNotificationCenter defaultCenter] addObserver:self
                                                 selector:@selector(settingsChanged:)
                                                     name:MCSettingsDidChangeNotification
                                                   object:nil];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

// ------------------------------------------------------------ the bar
- (void)makeQuestionBar {
    NSView *bar = [[NSView alloc] initWithFrame:NSZeroRect];
    bar.wantsLayer = YES;
    bar.hidden = YES;
    _question = [NSTextField labelWithString:@"This page has scripts. Run them?"];
    _question.font = [NSFont systemFontOfSize:12];
    _question.lineBreakMode = NSLineBreakByTruncatingTail;
    [bar addSubview:_question];
    NSButton *run = [NSButton buttonWithTitle:@"Run Scripts" target:self
                                       action:@selector(runPressed:)];
    run.bezelStyle = NSBezelStyleRounded;
    run.controlSize = NSControlSizeSmall;
    run.font = [NSFont systemFontOfSize:11];
    run.tag = 1;
    [bar addSubview:run];
    NSButton *dont = [NSButton buttonWithTitle:@"Don't Run" target:self
                                        action:@selector(dontRunPressed:)];
    dont.bezelStyle = NSBezelStyleRounded;
    dont.controlSize = NSControlSizeSmall;
    dont.font = [NSFont systemFontOfSize:11];
    dont.tag = 2;
    [bar addSubview:dont];
    NSButton *remember = [NSButton checkboxWithTitle:@"Remember for every HTML file"
                                              target:nil action:nil];
    remember.font = [NSFont systemFontOfSize:11];
    remember.controlSize = NSControlSizeSmall;
    remember.toolTip = @"Writes html.scripts = on or off in the settings file";
    [bar addSubview:remember];
    self.rememberBox = remember;
    self.questionBar = bar;
    [self addSubview:bar];
    [self applyBarColors];
}

- (void)applyBarColors {
    AppSettings *cfg = [AppSettings shared];
    NSColor *bg = [cfg background:Surface::Editor];
    NSColor *text = [cfg text:Surface::Editor];
    NSColor *mix = [bg blendedColorWithFraction:0.08 ofColor:text] ?: bg;
    self.questionBar.layer.backgroundColor = mix.CGColor;
    _question.textColor = text;
}

- (void)runPressed:(id)sender {
    (void)sender;
    [self answerScripts:YES remember:self.rememberBox.state == NSControlStateValueOn];
}

- (void)dontRunPressed:(id)sender {
    (void)sender;
    [self answerScripts:NO remember:self.rememberBox.state == NSControlStateValueOn];
}

- (void)answerScripts:(BOOL)run remember:(BOOL)remember {
    if (self.path) ScriptAnswers()[self.path] = @(run);
    self.rememberBox.state = NSControlStateValueOff;
    // A settings write that is refused (the file has unsaved changes in a
    // tab) says why in the status bar; the answer still holds until quit.
    if (remember) [[AppSettings shared] writeValue:run ? @"on" : @"off" forKey:@"html.scripts"];
    [self followScriptsRule];
}

// ------------------------------------------------------------ the rule
// Whether the page should run its scripts, and whether to ask.
- (void)wantsScripts:(BOOL *)run ask:(BOOL *)ask {
    *run = NO;
    *ask = NO;
    switch ([AppSettings shared].settings.htmlScripts()) {
    case Settings::HtmlScripts::On: *run = YES; return;
    case Settings::HtmlScripts::Off: return;
    case Settings::HtmlScripts::Ask: break;
    }
    if (!_hasScripts) return;
    NSNumber *answer = self.path ? ScriptAnswers()[self.path] : nil;
    if (answer) *run = answer.boolValue;
    else *ask = YES;
}

// After an answer or a settings change: the bar as it should be, and the
// page loaded again if its scripts should now run, or stop.
- (void)followScriptsRule {
    if (!self.path) return;
    BOOL run = NO, ask = NO;
    [self wantsScripts:&run ask:&ask];
    if (ask != self.asking) {
        self.asking = ask;
        [self layoutPage];
    }
    if (run != self.runsScripts) [self loadKeepingScroll:YES];
}

- (void)settingsChanged:(NSNotification *)note {
    (void)note;
    [self applyBarColors];
    [self followScriptsRule];
}

// ------------------------------------------------------------ loading
- (MCWebView *)makeWebView {
    WKWebViewConfiguration *cfg = [WKWebViewConfiguration new];
    cfg.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
    MCWebView *web = [[MCWebView alloc] initWithFrame:self.bounds configuration:cfg];
    web.chrome = self;
    web.navigationDelegate = self;
    web.UIDelegate = self;
    [self addSubview:web positioned:NSWindowBelow relativeTo:self.questionBar];
    return web;
}

- (void)showFile:(NSString *)path {
    if (!path) return;
    const BOOL same = self.webView && [path isEqualToString:self.path];
    if (same && SameStamp(MCStampOfFile(path), _stamp)) {
        [self followScriptsRule];   // the rule may have changed meanwhile
        return;
    }
    if (!same) _restoreScroll = NO;
    self.path = path;
    [self loadKeepingScroll:same];
}

- (void)reloadIfChanged {
    if (self.path && self.webView) [self showFile:self.path];
}

// Reads the file's text for the scripts question, decides, and loads,
// first reading where the page on show is scrolled when it is kept.
- (void)loadKeepingScroll:(BOOL)keep {
    NSString *path = self.path;
    if (!path) return;
    _stamp = MCStampOfFile(path);
    NSData *data = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil];
    _hasScripts = FileSniff::htmlMayRunScripts(
        data ? std::string((const char *)data.bytes, data.length) : std::string());
    BOOL run = NO, ask = NO;
    [self wantsScripts:&run ask:&ask];
    self.runsScripts = run;
    self.asking = ask;
    if (!self.webView) {
        self.webView = [self makeWebView];
        keep = NO;
    }
    [self layoutPage];
    const NSUInteger generation = ++_generation;
    if (!keep || self.webView.URL == nil) {
        [self startLoad:generation];
        return;
    }
    __weak MCHtmlPage *weakSelf = self;
    [self.webView evaluateJavaScript:@"[window.scrollX, window.scrollY]"
                             inFrame:nil
                      inContentWorld:WKContentWorld.defaultClientWorld
                   completionHandler:^(id value, NSError *error) {
        (void)error;
        MCHtmlPage *me = weakSelf;
        if (!me || me->_generation != generation) return;
        NSArray *xy = [value isKindOfClass:NSArray.class] ? value : nil;
        if (xy.count == 2) {
            me->_scrollX = [xy[0] doubleValue];
            me->_scrollY = [xy[1] doubleValue];
            me->_restoreScroll = YES;
        }
        [me startLoad:generation];
    }];
}

- (void)startLoad:(NSUInteger)generation {
    if (generation != _generation || !self.webView || !self.path) return;
    NSURL *file = [NSURL fileURLWithPath:self.path];
    NSString *access = self.path.stringByDeletingLastPathComponent;
    NSString *root = (self.readAccessFolder ? self.readAccessFolder() : nil).stringByStandardizingPath;
    if (root.length && ([root isEqualToString:@"/"] ||
                        [self.path hasPrefix:[root stringByAppendingString:@"/"]]))
        access = root;
    self.loads++;
    // From the origin each time, so a changed stylesheet or picture beside
    // it is read again too.
    if ([WithoutFragment(self.webView.URL) isEqual:file]) [self.webView reloadFromOrigin];
    else [self.webView loadFileURL:file allowingReadAccessToURL:[NSURL fileURLWithPath:access]];
}

- (void)webView:(WKWebView *)wv didFinishNavigation:(WKNavigation *)nav {
    (void)nav;
    if (!_restoreScroll) return;
    _restoreScroll = NO;
    NSString *js = [NSString stringWithFormat:@"window.scrollTo(%f, %f)", _scrollX, _scrollY];
    [wv evaluateJavaScript:js inFrame:nil inContentWorld:WKContentWorld.defaultClientWorld
         completionHandler:nil];
}

// Scripts on or off for every navigation of the page. The page on show
// stays the tab's file: a link clicked (or a script or form) taking the
// page anywhere else goes to onOpenLink instead, which opens a local file
// in its own tab and a web address in the browser pane; a link to a place
// in the same page scrolls there. Frames inside the page load as they ask.
- (void)webView:(WKWebView *)wv decidePolicyForNavigationAction:(WKNavigationAction *)action
                                                     preferences:(WKWebpagePreferences *)prefs
                                                 decisionHandler:(void (^)(WKNavigationActionPolicy,
                                                                           WKWebpagePreferences *))handler {
    (void)wv;
    prefs.allowsContentJavaScript = self.runsScripts;
    NSURL *url = action.request.URL;
    const BOOL mainFrame = action.targetFrame == nil || action.targetFrame.isMainFrame;
    const BOOL ownPage = url.isFileURL && url.query == nil && self.path &&
                         [url.path.stringByStandardizingPath
                             isEqualToString:self.path.stringByStandardizingPath];
    const BOOL blank = [url.scheme.lowercaseString isEqualToString:@"about"];
    if (url && mainFrame && !ownPage && !blank) {
        handler(WKNavigationActionPolicyCancel, prefs);
        void (^open)(NSURL *) = self.onOpenLink;
        if (open) dispatch_async(dispatch_get_main_queue(), ^{ open(url); });
        return;
    }
    handler(WKNavigationActionPolicyAllow, prefs);
}

// A target=_blank link or window.open: opened as a link is.
- (WKWebView *)webView:(WKWebView *)wv createWebViewWithConfiguration:(WKWebViewConfiguration *)cfg
       forNavigationAction:(WKNavigationAction *)action windowFeatures:(WKWindowFeatures *)features {
    (void)wv; (void)cfg; (void)features;
    NSURL *url = action.request.URL;
    void (^open)(NSURL *) = self.onOpenLink;
    if (url && open) dispatch_async(dispatch_get_main_queue(), ^{ open(url); });
    return nil;
}

// Another tab shows: the page stays loaded behind it, so it comes back
// where it was scrolled (another file replaces it), with any sound or
// video it plays paused; on macOS 11, which cannot pause them, it goes.
- (void)clear {
    if (@available(macOS 12.0, *)) {
        [self.webView pauseAllMediaPlaybackWithCompletionHandler:nil];
        return;
    }
    [self dropPage];
}

- (void)dropPage {
    _generation++;
    _restoreScroll = NO;
    [self.webView stopLoading];
    [self.webView removeFromSuperview];
    self.webView = nil;
    self.path = nil;
    _stamp = nil;
    self.asking = NO;
    self.runsScripts = NO;
    [self layoutPage];
}

- (void)takeKeyboard {
    MCWebView *web = self.webView;
    if (!web || web.isHiddenOrHasHiddenAncestor || !self.window) return;
    web.grantKeyboard = YES;
    [self.window makeFirstResponder:web];
    web.grantKeyboard = NO;
}

// ------------------------------------------------------------ layout
- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self layoutPage];
}

- (void)layoutPage {
    const NSRect b = self.bounds;
    const CGFloat barH = self.asking ? MIN(kQuestionBarHeight, NSHeight(b)) : 0;
    self.questionBar.hidden = !self.asking;
    self.questionBar.frame = NSMakeRect(0, NSHeight(b) - barH, NSWidth(b), barH);
    if (self.asking) {
        const CGFloat pad = 10, h = NSHeight(self.questionBar.bounds);
        NSButton *run = [self.questionBar viewWithTag:1];
        NSButton *dont = [self.questionBar viewWithTag:2];
        [run sizeToFit];
        [dont sizeToFit];
        [self.rememberBox sizeToFit];
        CGFloat x = NSWidth(b) - pad;
        x -= NSWidth(self.rememberBox.frame);
        self.rememberBox.frame = NSMakeRect(x, round((h - NSHeight(self.rememberBox.frame)) / 2),
                                            NSWidth(self.rememberBox.frame),
                                            NSHeight(self.rememberBox.frame));
        x -= pad + NSWidth(dont.frame);
        dont.frame = NSMakeRect(x, round((h - NSHeight(dont.frame)) / 2), NSWidth(dont.frame),
                                NSHeight(dont.frame));
        x -= 4 + NSWidth(run.frame);
        run.frame = NSMakeRect(x, round((h - NSHeight(run.frame)) / 2), NSWidth(run.frame),
                               NSHeight(run.frame));
        const CGFloat labelH = ceil(_question.intrinsicContentSize.height);
        _question.frame = NSMakeRect(pad, round((h - labelH) / 2), MAX(0, x - 2 * pad), labelH);
    }
    self.webView.frame = NSMakeRect(0, 0, NSWidth(b), MAX(0, NSHeight(b) - barH));
}
@end
