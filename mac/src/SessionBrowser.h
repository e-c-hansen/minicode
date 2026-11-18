// SessionBrowser.h — Objective-C++. View > Show Session in Browser… (and
// Copy Session Browser Link): the browser's view of a session (SessionWeb.h,
// stages 6b and 8b) served for one window's tabs from the app's own host
// (SessionLink.h, editor.session-host), so the page works on this Mac
// without `minicode host`. Editable (the choice Show asks for), the page
// types into the window's documents through the host, undoes through its
// history, and saves through the window's own Cmd+S; read-only, it only
// looks.
//
// One server per window, made the first time it is asked for and kept while
// the window lives: 127.0.0.1 on a free port, a fresh 48-hex token, the
// Host and Origin checks of Web::answer. Its view connects to the process's
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
// to the folder the window reports), and a click on a file in it opens the
// file in the window, as a click in the window's tree does, read-only pages
// included: opening a tab changes no text. The window's browser pane's tabs
// (address, title, the one on show) go with its tabs (`browserTabs`), so the
// page lists them and frames the one picked; an editable page may ask the
// pane to open an http or https address (`browse`, held by the core's
// Web::webAddress).
#pragma once
#import <Cocoa/Cocoa.h>

@class MCTab;

@interface MCSessionBrowser : NSObject
// `tabs` gives the window's groups: an array of arrays of MCTab (the
// focused group first), and `active` each group's tab on show.
- (instancetype)initWithWindow:(NSWindow *)window
                        folder:(NSString *)folder
                          tabs:(NSArray<NSArray<MCTab *> *> * (^)(void))tabs
                        active:(NSArray<MCTab *> * (^)(void))active;
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
// The window's terminals on the host (TerminalView sessionTerminal, 0s
// left out), reported with the tabs; unset, the page shows none.
@property(nonatomic, copy) NSArray<NSNumber *> * (^terminals)(void);
// The window's browser pane, nil while it has none: @{@"tabs": @[@{@"url",
// @"title"}], @"active": @(index on show, -1 for none)}. Read with the tabs.
@property(nonatomic, copy) NSDictionary * (^browserTabs)(void);
// Opens an http or https address in the window's browser pane (a tab
// already at it shown again, else a new one); run on the main thread for an
// editable page's address field. The core refuses every other scheme first.
@property(nonatomic, copy) void (^browse)(NSString *url);
// Starts serving if it is not yet: NO, with why, when it cannot.
- (BOOL)start:(NSString **)error;
// http://127.0.0.1:PORT/?t=TOKEN, nil until started.
@property(nonatomic, readonly) NSString *url;
@property(nonatomic, readonly) int port;
@property(nonatomic, readonly) int browsers;   // connected now
// The window's id on the host (Client::windowTabs).
@property(nonatomic, readonly) uint64_t windowId;
// Sends the window's tabs and terminals to the host now, when they
// changed, and has the page's view look at once.
- (void)reportTabs;
- (void)stop;
@end

// The line the page shows where terminals would be.
extern NSString *const MCSessionBrowserTermNote;
// What opens the page: NSWorkspace's openURL: unless set (checks).
extern void (^MCSessionBrowserOpener)(NSURL *url);
