// HtmlPageView.h — an .html file shown as its page in the editor's slot, the
// viewer over the tab's text that FileViewers makes for MCViewerHtml
// (Shift+Cmd+V flips to the source and back, as for a CSV table).
//
// The page is the file on disk, not the buffer: unsaved source shows once
// it is saved. It loads with read access to the window's folder (the
// file's own folder when the file is outside it), so relative stylesheets,
// scripts, pictures and pages load; no server runs. When the file changes
// (a save, an agent rewriting it) `showFile:` and `reloadIfChanged` load
// it again keeping the scroll, read and put back in an isolated world.
//
// Scripts follow html.scripts (Settings.h): with "ask", a page whose text
// holds anything that would run (FileSniff::htmlMayRunScripts) shows with
// scripts off under a bar asking whether to run them, with a box to
// remember the answer in the settings file; an answer not remembered
// lasts for that file until minicode quits. Once answered, the bar stays
// as a line saying what was chosen (with a close box) until another file
// shows, so the page does not move up under the pointer. The web view is an MCWebView
// (Browser.h), so the page never takes the keyboard by itself, and its
// data store is not persistent: nothing it stores outlives the page.
//
// A link clicked in the page opens through `onOpenLink` (a local file in
// its own tab, a web address in the browser pane), so the page on show is
// always the tab's file; a link to a place in the same page scrolls there.
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

@class MCWebView;

@interface MCHtmlPage : NSView
// The folder the page may read from: the window's.
@property(nonatomic, copy) NSString * (^readAccessFolder)(void);
// A link clicked in the page, or a page it tried to open elsewhere.
@property(nonatomic, copy) void (^onOpenLink)(NSURL *url);

// Shows the file: loads it when it is another file or changed since it
// was loaded (keeping the scroll), else leaves the page as it is.
- (void)showFile:(NSString *)path;
// Loads the file on show again, keeping the scroll, when it changed on
// disk since it was loaded.
- (void)reloadIfChanged;
// Another tab shows instead: the page stays loaded, hidden and paused,
// so it comes back as it was (on macOS 11 it is let go of).
- (void)clear;
// Lets go of the page, and of its WebKit process.
- (void)dropPage;
// Gives the page the keyboard: the user asked (Cmd+1, Shift+Cmd+V).
- (void)takeKeyboard;

// The answer to the bar's question, as its buttons give it.
- (void)answerScripts:(BOOL)run remember:(BOOL)remember;
// Takes the answered line away, as its close box does.
- (void)endNote;

@property(nonatomic, readonly) MCWebView *webView;   // nil until a file shows
@property(nonatomic, readonly, copy) NSString *path;
@property(nonatomic, readonly) BOOL runsScripts;     // the page on show runs them
@property(nonatomic, readonly) BOOL asking;          // the scripts bar asks
@property(nonatomic, readonly) BOOL noting;          // it says what was answered
@property(nonatomic, readonly) NSView *questionBar;
@property(nonatomic, readonly) NSButton *rememberBox;
@property(nonatomic, readonly) NSUInteger loads;     // pages loaded, for checks
// A load keeping the place is under way, or the place is being put back.
@property(nonatomic, readonly) BOOL restoringPlace;
@end
