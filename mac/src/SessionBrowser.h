// SessionBrowser.h — Objective-C++. View > Show Session in Browser… (and
// Copy Session Browser Link): the browser's read-only view of a session
// (SessionWeb.h, stage 6b) served for one window's tabs from the app's own
// host (SessionLink.h, editor.session-host), so the page can be seen
// working on this Mac without `minicode host`.
//
// One server per window, made the first time it is asked for and kept while
// the window lives: 127.0.0.1 on a free port, a fresh 48-hex token, the
// Host and Origin checks of Web::answer. Its view connects to the process's
// host as a renderer that only reads (MCSessionConnectViewer) while a
// browser is connected. The window's tabs that have a document on the host
// are reported to it (Client::windowTabs) every half second while the server
// runs, and the page shows only those. Terminals do not run through the
// Mac's host yet; the page says so in one muted line. The server stops, and
// the window's tabs leave the host's state, when the window closes.
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
// Starts serving if it is not yet: NO, with why, when it cannot.
- (BOOL)start:(NSString **)error;
// http://127.0.0.1:PORT/?t=TOKEN, nil until started.
@property(nonatomic, readonly) NSString *url;
@property(nonatomic, readonly) int port;
@property(nonatomic, readonly) int browsers;   // connected now
// The window's id on the host (Client::windowTabs).
@property(nonatomic, readonly) uint64_t windowId;
// Sends the window's tabs to the host now, when they changed.
- (void)reportTabs;
- (void)stop;
@end

// The line the page shows where terminals would be.
extern NSString *const MCSessionBrowserTermNote;
// What opens the page: NSWorkspace's openURL: unless set (checks).
extern void (^MCSessionBrowserOpener)(NSURL *url);
