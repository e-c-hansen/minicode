// SessionLink.h — Objective-C++. The Mac's side of the session's host
// (SESSION-DESIGN.md, stage 4b; the core's SessionHost.h, SessionClient.h
// and Ot.h). With editor.session-host on, each tab's text, version and undo
// history belong to a Shared::SessionHost running on a thread of its own in
// the app's process, and the tab's text (the editor's storage while it is
// on show, the tab's own string while it is not) is the host's mirror:
//
//   - every change of the mirror goes to the host as it is made, from the
//     storage (MCSourceStorage willReplaceCharacters, before the store
//     changes, so a surrogate pair is never split) or, for a text set
//     whole (a viewer's edit, a preview's, a reload), as the one range in
//     which the two differ (MCTab setSourceText:); a storage edit outside
//     a change of the text view's (CodeTextView sessionChanging) is in no
//     undo step, and the host's history moves past it;
//   - the edits of one run-loop turn are one undo step, as NSUndoManager's
//     grouping by event made them, named by the first name given
//     (replaceRanges:withStrings:actionName:, setActionName:), else
//     "Typing" (joining the step before while the caret stays put) for
//     what is typed and "Edit" for the rest;
//   - undo and redo ask the host (the tab's undo manager, MCTabUndoManager
//     in EditorTab.mm, and CodeTextView undo:, redo:), whose answer comes
//     back as an edit put into the storage through
//     replaceRanges:withStrings:actionName:, recording nothing (and with
//     the tab's undo registration off); what NSTextView and the editor
//     register still goes on NSUndoManager's stacks, never undone then;
//   - an edit the host sends while the storage is in the middle of an edit
//     of its own (inside processEditing, a storage notification, a wait
//     that dispatched it there) waits for the next run-loop turn, and an
//     edit made meanwhile is moved past it (Ot::transform).
//
// One client for the process, in UTF-16 units (what NSString counts), its
// messages dispatched on the main queue as they arrive. MINICODE_HOST=
// loopback sends every message through the protocol's encoding and back;
// MINICODE_HOST_DELAY_US delays each one on the host's thread;
// MINICODE_SESSION_HOST=on turns the setting on for every tab (the GUI
// checks' second pass).
#pragma once
#import <Cocoa/Cocoa.h>

#include <string>

#include "SessionClient.h"

@class MCTab, CodeTextView;

// editor.session-host, or MINICODE_SESSION_HOST=on.
BOOL MCSessionHostEnabled(void);
// The process's client, the host started with it.
Shared::Client &MCSessionClient(void);
BOOL MCSessionLoopback(void);

// One tab's document on the host (made by MCTab when its text is first set
// with the setting on, closed with the tab).
@interface MCSessionDoc : NSObject
- (instancetype)initWithTab:(MCTab *)tab text:(NSString *)text;
@property(nonatomic, readonly) Shared::DocId docId;
@property(nonatomic, weak, readonly) MCTab *tab;
// The host's edit is going into the mirror: nothing is recorded.
@property(nonatomic, readonly) BOOL applying;
// Edits from the host waiting for the storage to finish an edit of its own.
@property(nonatomic, readonly) NSUInteger deferredEdits;

// ---- the mirror's changes
// The storage is about to replace `range` with `string` (the store still
// holds the old text). `view` shows it.
- (void)storageWillReplace:(NSRange)range with:(NSString *)string view:(CodeTextView *)view;
// The tab's text is being set whole, from `old` to `now`.
- (void)textWillChangeFrom:(NSString *)old to:(NSString *)now;
// The step being recorded takes this name (when it has none yet), or a new
// step opens with it.
- (void)nameStep:(NSString *)name;
// replaceRanges:withStrings:actionName:'s edit named `name` (nil: by what
// is going on) begins and ends: a step of its own, as its undo group was,
// unless it is inside a group or follows this turn's typing.
- (void)beginNamed:(NSString *)name view:(CodeTextView *)view;
- (void)endNamed;
- (void)breakCoalescing;
- (void)noteSelection:(NSArray<NSValue *> *)ranges;
// The text was read afresh: nothing to undo, the host's copy starts again
// from `text`; a text set whole in this run-loop turn starts it again too.
- (void)forgetHistoryWithText:(NSString *)text;
// The step of this turn, closed now (before an undo, or asking about one).
- (void)closeStep;
// The undo history forgotten, the text kept (the tab's undo manager's
// removeAllActions).
- (void)forgetHistory;
// The tab's text was written to its file.
- (void)saved;

// ---- undo
- (BOOL)canUndo:(BOOL)redo name:(NSString **)name;
// An undo (1) or redo (2) of the host's is going into the text, else 0.
@property(nonatomic, readonly) int undoing;
// How many undos and redos the host has made of this document.
@property(nonatomic, readonly) NSUInteger undoCount;
// The host takes its last step back (or puts it back); the edit is in the
// view before this returns. NO when there was nothing to do or it was
// refused.
- (BOOL)undo:(BOOL)redo inView:(CodeTextView *)view;

// ---- the host's text, for the language server sync
// A copy of the host's text kept from the host's own edits, everything
// sent so far applied; null when the host does not answer. `owner` lets
// it go with releaseHostTextFor:.
- (const Model::Text *)hostTextFor:(const void *)owner;
- (void)releaseHostTextFor:(const void *)owner;

- (void)close;
@end

// For the GUI checks: whether the host's text of each of `tabs` with a
// document on the host is the tab's text, and the host reported nothing
// wrong. Empty when so, else what differs.
NSString *MCSessionCheckTabs(NSArray<MCTab *> *tabs);
