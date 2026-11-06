// Browser.h — an embedded WKWebView with a minimal chrome (URL bar + nav).
// WebKit is a macOS system framework, so this adds no third-party dependency.
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

// The editor window. The browser's page asks to be the first responder
// whenever it focuses one of its fields (an autofocus search box, a script),
// and this window turns that down unless the user asked: a click in the
// page, Tab from the toolbar, or a page loaded from the toolbar while the
// keyboard is still there; never while the browser is hidden.
@interface MCKeyboardWindow : NSWindow
@end

@class MCPageProbe;

// Two pages share the pane: the user's, and once an agent has loaded one
// (`minicode browse`), the agent page beside it, with a switcher in the
// toolbar showing "Yours" and "Agent". An agent never navigates the
// user's page; closing the agent page (its x) shows the user's page as it
// was. The toolbar's back, forward and reload act on the page on show;
// an address typed in it, or a link from elsewhere in minicode, loads in
// the user's page.
@interface BrowserView : NSView
- (instancetype)initWithHomeURL:(NSString *)url;
- (void)focusURLBar;
// Loads an address in the user's page, or searches for words that are not
// one, and shows it.
- (void)navigateToString:(NSString *)raw;
// The agent page's probe (BrowserPages.h), the page made on first use; it
// is not shown by this.
- (MCPageProbe *)openAgentPage;
// Loads `url` (an address PageCheck::agentAddress made) in the agent page.
- (WKNavigation *)loadInAgentPage:(NSString *)url;
- (WKNavigation *)reloadAgentPage;
- (WKNavigation *)reloadUserPage;
// Shows the agent page, unless the keyboard is in the pane (the user is
// using their page or the address bar): then it stays behind the switcher
// and this answers NO.
- (BOOL)showAgentPageQuietly;
- (void)showUserPage;
- (void)closeAgentPage;
@property(nonatomic, readonly) BOOL hasAgentPage;
@property(nonatomic, readonly) BOOL agentPageShown;
@property(nonatomic, readonly) MCPageProbe *userProbe;
@property(nonatomic, readonly) MCPageProbe *agentProbe;    // nil without an agent page
// Whether the page on show is the user's and has loaded something.
@property(nonatomic, readonly) BOOL showsUserPage;
// The user's page, to bring back after a restart or a relaunch: the
// address, and WebKit's own record of the back and forward list and the
// scroll position (macOS 12 and later). restoreState: takes it back, and
// loads the address alone when the record is missing or refused. The agent
// page is never kept.
- (NSDictionary *)savedState;
- (void)restoreState:(NSDictionary *)state;
// The page on show: its address and title (nil before any page).
@property(nonatomic, readonly) NSString *pageURL;
@property(nonatomic, readonly) NSString *pageTitle;

// Zoom (PageZoom.h), of the page on show: a step of Safari's in or out
// (dir 0: actual size), or as small as it takes for the page's content to
// fit the pane's width (never larger than actual size). The user's page
// keeps a zoom per site, across launches, as Safari does; the agent page's
// lasts until it closes. The toolbar shows the new percentage for a moment.
- (void)zoomStep:(int)dir;
- (void)zoomToFitWidth;
@property(nonatomic, readonly) CGFloat pageZoom;
@property(nonatomic, readonly) NSTextField *zoomLabel;   // "75%", shown for a moment
@end
