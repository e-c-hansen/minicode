// BrowserPages.h — Objective-C++. What an agent can learn from a page in
// the browser pane (`minicode browse`, `minicode page`; the rules are in
// PageCheck.h). An MCPageProbe watches one web view: the HTTP status of its
// page (from the navigation response), whether it is loading, loads to wait
// for, and, for the agent page only, the console.
//
// Nothing an agent sends ever runs in a page. The few scripts here are
// minicode's own and fixed: reading the text and the page's height run in
// an isolated script world (WKContentWorld), which the page can neither see
// nor change, and so does the console capture's listener, which hears
// uncaught errors and unhandled rejections and hands everything to the app
// through a message handler only that world can reach. console.log and the
// rest live in the page's own world, which an isolated world cannot reach,
// so on the agent page only a short wrapper installed at document start
// passes each call on to the listener as a DOM event. A page could see that
// wrapper and write its own lines through it; it could write them with
// console.log anyway, and it can never reach the record or the handler.
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include "PageCheck.h"

@interface MCPageProbe : NSObject
// The agent page's configuration: the console capture installed at document
// start, reporting to `probe`. The user's page gets none.
+ (WKWebViewConfiguration *)agentPageConfigurationFor:(MCPageProbe *)probe;
- (instancetype)initRecordingConsole:(BOOL)console;
@property(nonatomic, weak) WKWebView *webView;
@property(nonatomic, readonly) BOOL recordsConsole;

// The page on show.
@property(nonatomic, readonly) NSString *url;      // "" before any page
@property(nonatomic, readonly) NSString *title;
@property(nonatomic, readonly) int status;         // 0: none (a file, no answer)
@property(nonatomic, readonly) BOOL loading;
// url, status, title and loading as `page --url` prints them.
- (Json)stateJson;
// The console since the page loaded (agent page).
- (const PageCheck::ConsoleLog *)consoleLog;

// The browser view forwards its navigation delegate's calls.
- (void)noteResponse:(WKNavigationResponse *)response;
- (void)noteCommit:(WKNavigation *)navigation;
- (void)noteFinish:(WKNavigation *)navigation;
- (void)noteFail:(WKNavigation *)navigation error:(NSError *)error;

// Calls `done` once the navigation finishes (error nil), fails (why), or
// after 15 seconds (timedOut). A nil navigation is done at once.
- (void)waitFor:(WKNavigation *)navigation
           done:(void (^)(NSString *error, BOOL timedOut))done;

// document.body.innerText, up to 1 MB, read in the isolated world.
- (void)readText:(void (^)(NSString *text, BOOL cut, NSString *error))done;
// A PNG of the visible area, or of the page from its top with `full` (up
// to 16,384 pixels tall), written to `path`. `info` has the picture's
// width and height in pixels and whether it was cut.
- (void)snapshotTo:(NSString *)path full:(BOOL)full
              done:(void (^)(NSDictionary *info, NSString *error))done;
// Lets go of the message handler (the agent page closing).
- (void)detach;
@end
