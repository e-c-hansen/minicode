// SessionBrowser.h — Objective-C++. View > Show Session in Browser… (and
// Copy Session Browser Link): the browser's view of a session (SessionWeb.h,
// stages 6b and 8b) served for one window's tabs from the app's own host
// (SessionLink.h, editor.session-host), so the page works on this Mac
// without `mutible host`. Editable (the choice Show asks for), the page
// types into the window's documents through the host, undoes through its
// history, and saves through the window's own Cmd+S; read-only, it only
// looks.
//
// One server per window, made the first time it is asked for and kept while
// the window lives: 127.0.0.1 on a free port, the Host and Origin checks of
// Web::answer. Each Show and Copy makes a fresh link, good for one browser
// (SessionWebAccess.h): the page trades its token for a cookie and takes it
// out of the address bar, and the credential ends after
// editor.session-web-idle-minutes with nothing from its browser. A browser
// this Mac does not remember waits while a sheet on the window asks (Allow,
// Deny, Always Allow), naming the browser and where it connects from (this
// Mac, or the peer of an SSH tunnel into it); Always Allow remembers its id
// in the app's defaults, which View > Remembered Browsers… lists and
// forgets. Its view connects to the process's
// host as a renderer that only reads, or as a guest that may edit
// (MCSessionConnectViewer), while a browser is connected. The window's tabs that have a document on the host
// are reported to it (Client::windowTabs) every half second while the server
// runs, and the page shows only those. Terminals opened while
// editor.session-host is on run through the host; the window reports its
// own with its tabs, and the page sees, and types into, only those (the
// core refuses any other id a page names). Until there is one, the page
// says so in one muted line. The server stops, and
// the window's tabs leave the host's state, when the window closes. The
// page's tree lists the window's folder through the host (FsRead, confined
// to the folder the window reports). On an editable page a click on a file
// in it opens the file in the window, as a click in the window's tree does,
// and a tab's close button closes the window's tab (`close`, never one with
// unsaved changes); a read-only page changes nothing in the window: it
// reads the file for a tab of its own, which it closes itself. The window's
// browser pane's tabs (address, title, the one on show) go with its tabs
// (`browserTabs`), and which panes it shows (`panes`), so the page lays out
// its editor, browser and terminal as the window does and frames the
// browser tab picked; an editable page may ask the pane to open an http or
// https address (`browse`, held by the core's Web::webAddress). A LaTeX
// document on the page shows the PDF the window's own preview typeset
// (R3's third slice): each typeset of the window's previews goes to the
// server as it starts and ends, and a page that shows a document asks the
// window (`typeset`), which answers with what its preview of it has, or,
// for a document no tab of the window previews, has a preview of this
// object's own typeset it from the tab's text or the disk's; tectonic never
// runs again for a page when the window's PDF is current. The window's open
// commit and worktree pages go with its tabs (`reviews`), and the page shows
// each read-only, read by the server with git from what it checked (a
// commit's hex name, a worktree its repository lists), with a review's
// pictures served from the read (R3's last slice).
#pragma once
#import <Cocoa/Cocoa.h>

@class MCTab, MCSessionWindow, LatexView;

@interface MCSessionBrowser : NSObject
// `session` is the window's on the session's host (SessionWindow.h): its
// link, its id there and its record, which the page shows.
- (instancetype)initWithWindow:(NSWindow *)window session:(MCSessionWindow *)session;
// Whether the page may edit. Changing it stops a server that runs; the next
// start serves anew, at a new address.
@property(nonatomic) BOOL editable;
// Saves the window's document with this id on the host as Cmd+S does; run
// on the main thread for a page's save.
@property(nonatomic, copy) void (^save)(uint64_t docId);
// Opens a file of the window's folder as a click in its tree does (`keep`
// for a kept tab rather than the preview tab); run on the main thread for a
// click in the page's tree. The page asks only for files the core held
// inside the folder (Shared::confinedPath).
@property(nonatomic, copy) void (^open)(NSString *path, BOOL keep);
// Closes the window's tab for a document on the host as the window's own
// close does; run on the main thread for an editable page's close button.
// The core has refused a tab with unsaved changes; the window checks again.
@property(nonatomic, copy) void (^close)(uint64_t docId);
// Opens an http or https address in the window's browser pane (a tab
// already at it shown again, else a new one); run on the main thread for an
// editable page's address field. The core refuses every other scheme first.
@property(nonatomic, copy) void (^browse)(NSString *url);
// Says what the window has of the typeset of a LaTeX document of its
// folder, for any page that shows it (one that only reads too: a typeset
// changes nothing of the user's); run on the main thread each time a page
// shows the document. The window answers with typesetOf:view: from its own
// preview of the file, else typesetInBackground:source:.
@property(nonatomic, copy) void (^typeset)(NSString *path);
// A typeset of the document at `path` began (`pdf` and `error` nil), worked
// (`pdf`), or failed (`error`), as a preview saw it: the pages showing it
// hear of it. Nothing while no server runs.
- (void)typesetOf:(NSString *)path pdf:(NSData *)pdf error:(NSString *)error;
// What `view`, a preview of the document at `path`, has now: its last PDF
// (kept while another typeset runs), a typeset under way or due, or why the
// last one failed.
- (void)typesetOf:(NSString *)path view:(LatexView *)view;
// A preview of this object's own for a document no tab of the window
// previews: typeset from `source` (the tab's text, or the disk's), again
// only when `source` changed since. At most four are kept; they stop with
// the server.
- (void)typesetInBackground:(NSString *)path source:(NSString *)source;
// Starts serving if it is not yet: NO, with why, when it cannot.
- (BOOL)start:(NSString **)error;
// A fresh link, http://127.0.0.1:PORT/v/ID/?t=TOKEN, good for one browser;
// nil until started.
- (NSString *)newLink;
// The last link newLink made, nil until one was.
@property(nonatomic, readonly) NSString *url;
@property(nonatomic, readonly) int port;
@property(nonatomic, readonly) int browsers;   // connected now
// Browsers waiting for the window's answer, the one asked about included.
@property(nonatomic, readonly) NSUInteger asking;
// The window's id on the host (Client::windowTabs).
@property(nonatomic, readonly) uint64_t windowId;
// Sends the window's record (MCSessionWindow report) now, when it
// changed, and has the page's view look at once.
- (void)reportTabs;
// The page's view looks at the host's state now (a record was sent).
- (void)lookNow;
// The window's folder changed on disk (its FSEvents): a page showing
// Source Control reads the folder's changes again (R3's fourth slice; at
// most one read a second, none while no page shows them).
- (void)gitChanged;
- (void)stop;
@end

// The line the page shows where terminals would be.
extern NSString *const MCSessionBrowserTermNote;
// The browsers Always Allow remembered, each {id, browser, address, since}
// (any thread), and forgetting them (the main thread).
NSArray<NSDictionary *> *MCSessionBrowsersRemembered(void);
void MCSessionBrowsersForget(void);
// View > Remembered Browsers…: lists them, with Forget All.
void MCSessionBrowsersShowRemembered(void);
// Where a connection to the server on `serverPort` from loopback port
// `peerPort` comes from: "this Mac", or "SSH from ADDRESS" when an SSH
// tunnel into this Mac holds the other end (any thread).
NSString *MCSessionBrowserPeer(int peerPort, int serverPort);
// What opens the page: NSWorkspace's openURL: unless set (checks).
extern void (^MCSessionBrowserOpener)(NSURL *url);
// Checks: when set, a browser's sheet is shown only once this calls the
// block it is given, so a check can see the page wait first.
extern void (^MCSessionBrowserAskDelay)(dispatch_block_t ask);
