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
@class MCTerminalFindBar;

// The user's pages are tabs, in a strip like the terminal's above the
// toolbar (BrowserTabs.h has the rules): + or Cmd+T opens one, a tab's x,
// a middle click or Cmd+W closes one, and Ctrl+Tab, Shift+Cmd+] and [ and
// Ctrl+1 to 9 move between them while the pane has the keyboard; tabs
// drag along the strip. Only the tab on show and the one used just before
// it keep a live page (a web view and its WebKit process); the others keep
// their address, title and WebKit's record of their history and scroll
// (macOS 12 and later), and load again when shown. A link Command-clicked
// or middle-clicked opens in a new tab behind, and a page a script or a
// target=_blank link opens comes up in a new tab beside its opener.
//
// Beside them, once an agent has loaded one (`minicode browse`), is the
// agent page, with a switcher in the toolbar showing "Yours" and "Agent".
// An agent never navigates the user's pages; closing the agent page (its
// x) shows the user's tab as it was. The toolbar's back, forward and
// reload act on the page on show; an address typed in it, or a link from
// elsewhere in minicode, loads in the user's tab on show.
@interface BrowserView : NSView
- (instancetype)initWithHomeURL:(NSString *)url;
// Find in the page on show (Cmd+F while the pane has the keyboard, the
// address bar too; Cmd+G and Shift+Cmd+G, Return and Shift+Return, Esc): the
// terminal's bar over the page's top right, WebKit doing the finding. Nil
// until first asked for.
@property(nonatomic, readonly) MCTerminalFindBar *findBar;
- (void)focusURLBar;
// The tabs. newTab opens an empty one right of the one on show and shows
// it (the caller gives the address bar the keyboard); closing the last
// leaves an empty tab and calls onEmpty, which hides the pane.
@property(nonatomic, readonly) NSUInteger tabCount;
@property(nonatomic, readonly) NSInteger selectedTab;
- (void)newTab;
- (void)selectTab:(NSUInteger)index;
- (void)closeTab:(NSUInteger)index;
- (BOOL)moveTab:(NSUInteger)from to:(NSUInteger)to;
@property(nonatomic, copy) void (^onEmpty)(void);
// What the strip shows, and each tab's address and full page title (""
// before the page has one).
- (NSArray<NSString *> *)tabTitles;
- (NSArray<NSString *> *)tabAddresses;
- (NSArray<NSString *> *)tabPageTitles;
// Opens each address in a tab of its own (bookmarks, `minicode
// https://...`): a tab already at that address is used again, the empty
// tab on show (a pane just made, or left by closing the last tab) takes
// the first, and the rest go right of the one on show, in order. The first
// shows; the others load when shown. The keyboard stays where it is.
- (void)openAddresses:(NSArray<NSString *> *)urls;
// The keyboard to the user's page on show (a page that has loaded or is
// loading), as a click in it would.
- (void)focusPage;
// How many tabs may keep a live page (BrowserTabs::kLivePages; at least
// 1), how many do, and a tab's web view (nil while it is suspended).
@property(nonatomic) NSInteger liveLimit;
@property(nonatomic, readonly) NSUInteger livePageCount;
- (WKWebView *)webViewOfTab:(NSUInteger)index;
// Whether the keyboard is in the pane: a page, the address bar, a button.
- (BOOL)keyboardInPane;
// Loads an address in the user's tab on show, or searches for words that
// are not one, and shows it.
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
@property(nonatomic, readonly) MCPageProbe *userProbe;   // the user's tab on show
@property(nonatomic, readonly) MCPageProbe *agentProbe;    // nil without an agent page
// Whether the page on show is the user's and has loaded something.
@property(nonatomic, readonly) BOOL showsUserPage;
// The user's tabs, to bring back after a restart or a relaunch: each
// one's address and title, WebKit's own record of its back and forward
// list and scroll position (macOS 12 and later; 2 MB a tab and 4 MB in
// all, the tab on show first), and which one showed; the tab on show is
// also written as the single page older versions kept ("url", "state").
// restoreState: takes either back, loading only the tab on show, and the
// address alone when a record is missing or refused. The agent page is
// never kept.
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
