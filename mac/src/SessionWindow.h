// SessionWindow.h — Objective-C++. One editor window on the session's host
// (SessionLink.h, editor.session-host on or headless; stage 6, docs/notes/
// session-host.md, "The Mac reattaches"): the window's link (its folder's
// headless host, or the process's), its id on that host, and its record,
// the window's tabs and terminals in Shared::windowJson's shape, sent every
// half second when it changed. The browser's view of the window
// (SessionBrowser.h) shows that record.
//
// A window opening takes what the host holds for its folder (takeHeld): the
// latest record a closed window left, with its documents (made into tabs by
// EditorController+Session.mm) and terminals (handed to the terminal panel
// one by one, nextTermPlace). A window closing leaves its record, marked
// detached, for the folder's next window (detachKeeping:).
//
// Kept as an associated object of its EditorController, so the controller
// gains no instance variables.
#pragma once
#import <Cocoa/Cocoa.h>

#include "Json.h"
#include "SessionLink.h"
#include "TermDockRecord.h"

@class MCTab;

@interface MCSessionWindow : NSObject
- (instancetype)initWithFolder:(NSString *)folder;
@property(nonatomic, readonly, copy) NSString *folder;
@property(nonatomic, readonly) MCSessionLinkRef *link;
// The window's id on its host (Client::windowTabs), the record's once it
// took one.
@property(nonatomic, readonly) uint64_t windowId;

// What the record is made of, read at each report: the groups' tabs (the
// focused group first) and each group's tab on show (NSNull for none), the
// window's terminals on the host, its browser pane's tabs (nil without a
// pane: @{@"tabs": @[@{@"url", @"title"}], @"active": @(index)}), and which
// panes it shows (@{@"browser", @"terminal", @"browserShare"}).
@property(nonatomic, copy) NSArray<NSArray<MCTab *> *> * (^tabs)(void);
@property(nonatomic, copy) NSArray *(^active)(void);
@property(nonatomic, copy) NSArray<NSNumber *> * (^terminals)(void);
// How the terminal dock holds them (MCTerminalPanel sessionDock), filled
// into `out`: the record's "termLayout" (Shared::termLayoutJson), written
// beside "terms". nil, or nothing filled: no layout.
@property(nonatomic, copy) void (^termDock)(Shared::TermDock *out);
@property(nonatomic, copy) NSDictionary * (^browserTabs)(void);
@property(nonatomic, copy) NSDictionary * (^panes)(void);
// After a record was sent (the browser's view looks at once).
@property(nonatomic, copy) void (^onReported)(void);

// The record now; `leaveOut` tabs are not in it.
- (Json)recordLeavingOut:(NSSet<MCTab *> *)leaveOut;
// Sent when it differs from what was sent last. Every half second from
// start until stop.
- (void)report;
// The same on the main queue's next turn, once however often it is asked
// (the dock's changes, MCTerminalPanel onChanged).
- (void)reportSoon;
- (void)start;
- (void)stop;

// What the host holds for this window, taken now: the record's id becomes
// the window's, and its terminals wait for the panel (nextTermPlace) in the
// order its dock is built (heldDock).
- (MCSession::Held)takeHeld;
// The dock the record's "termLayout" and the terminals taken make
// (Shared::termDockFrom): every held terminal once, in nextTermPlace's
// order (TermDock::order()); a tab each when the record has no layout.
- (const Shared::TermDock &)heldDock;
// Where the next terminal the panel makes goes: the window's link, with the
// next held terminal while any is left.
- (MCSessionTermPlace *)nextTermPlace;
@property(nonatomic, readonly) NSUInteger heldTermsLeft;
// The tab on show in the record taken, 0 for none, and how many held
// documents the window made tabs of.
@property(nonatomic, readonly) uint64_t heldActiveDoc;
@property(nonatomic, assign) NSUInteger heldDocsAttached;

// Tabs the close question's Don't Save named: closed on the host with the
// window, not left there.
@property(nonatomic, readonly) NSMutableSet<MCTab *> *dropOnClose;
// The window closes: its record left on the host for the folder's next
// window (`keep`), else closed there. Nothing is reported after it.
- (void)detachKeeping:(BOOL)keep leavingOut:(NSSet<MCTab *> *)leaveOut;
// The window moves to another link (Open Folder, or its host found again):
// the old record is closed there, and the window starts anew on `link`.
- (void)moveTo:(MCSessionLinkRef *)link folder:(NSString *)folder;

// Told on the main queue when the window's host goes (a headless one).
@property(nonatomic, copy) void (^onLost)(void);
// The status bar's Reconnect, while the host is gone.
@property(nonatomic, weak) NSButton *reconnectButton;
@end
