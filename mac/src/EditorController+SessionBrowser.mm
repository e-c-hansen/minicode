// EditorController+SessionBrowser.mm — EditorController: View > Show
// Session in Browser… and Copy Session Browser Link (SessionBrowser.h). The
// window's server is kept as an associated object, so the class gains no
// ivars, and it stops itself when the window closes. Show asks whether the
// page may edit, the last answer first (editable until one is given), and
// remembers it; Copy uses the remembered one. A page's save is the tab's
// own Cmd+S, its questions about the disk included. A click on a file in
// the page's tree opens it here as a click in the window's tree would.
#import "EditorController+Private.h"
#import "AppSettings.h"
#import "SessionBrowser.h"
#import "SessionLink.h"
#import "TreeDetails.h"
#include <objc/runtime.h>

static char kSessionBrowserKey;
static NSString *const kSessionBrowserEditableKey = @"MCSessionBrowserEditable";

// Editable until the owner says otherwise.
static BOOL MCSessionBrowserEditableChoice(void) {
    id v = [MCAppDefaults() objectForKey:kSessionBrowserEditableKey];
    return v ? [v boolValue] : YES;
}

@implementation EditorController (SessionBrowser)

- (MCSessionBrowser *)sessionBrowser {
    return objc_getAssociatedObject(self, &kSessionBrowserKey);
}

// With the setting off, says what the page needs and offers to turn it on:
// YES once it is on.
- (BOOL)sessionHostReadyAsking {
    if (MCSessionHostEnabled()) return YES;
    NSAlert *a = [NSAlert new];
    a.messageText = @"The session host is off";
    a.informativeText = @"The browser's view shows the tabs the session host holds, and "
                        @"editor.session-host is off. Turn it on? Tabs opened from then on appear "
                        @"on the page; a tab open now appears once it is closed and opened again.";
    [a addButtonWithTitle:@"Turn On"];
    [a addButtonWithTitle:@"Cancel"];
    if ([a runModal] != NSAlertFirstButtonReturn) return NO;
    if (![[AppSettings shared] writeValue:@"on" forKey:@"editor.session-host"]) {
        NSBeep();   // the settings file has unsaved changes in a tab: said in the status bar
        return NO;
    }
    return MCSessionHostEnabled();
}

// Whether the page may edit, asked with the last answer first; NO on
// Cancel.
- (BOOL)askSessionBrowserEditable:(BOOL *)editable {
    const BOOL last = MCSessionBrowserEditableChoice();
    NSAlert *a = [NSAlert new];
    a.messageText = @"Show this window's session in the browser";
    a.informativeText = @"Editable lets the page type into this window's files, undo, and save "
                        @"them, as you can here. Anyone on this Mac with the link can do the "
                        @"same until the window closes. Read-only shows the files and follows "
                        @"your typing.";
    [a addButtonWithTitle:last ? @"Editable" : @"Read-Only"];
    [a addButtonWithTitle:last ? @"Read-Only" : @"Editable"];
    [a addButtonWithTitle:@"Cancel"];
    const NSModalResponse r = [a runModal];
    if (r != NSAlertFirstButtonReturn && r != NSAlertSecondButtonReturn) return NO;
    const BOOL chose = r == NSAlertFirstButtonReturn ? last : !last;
    [MCAppDefaults() setBool:chose forKey:kSessionBrowserEditableKey];
    *editable = chose;
    return YES;
}

// A page's save: every edit it made is in the tab first, then the tab's
// own save, as Cmd+S on it (or on the tab on show) would.
- (void)saveSessionDocument:(uint64_t)docId {
    MCSessionClient().dispatch();
    for (MCTab *t in [self tabs]) {
        if (!t.session || t.session.docId != docId) continue;
        if (t == _activeTab) [self saveCurrentFile:self];
        else [self saveTab:t];
        return;
    }
}

// A click on a file in the page's tree: the same tab a click in this
// window's tree gives (the preview tab, or a kept one for a double-click),
// the file selected in the tree too, which stays focused as it is.
- (void)openSessionPath:(NSString *)path keep:(BOOL)keep {
    [self revealPath:path
                open:YES
                from:keep ? EditorTabs::OpenFrom::TreeCommandClick : EditorTabs::OpenFrom::TreeClick
               widen:NO];
}

// The window's server, started now (editable or not) if it is not yet; nil,
// said in the status bar, when it cannot be.
- (MCSessionBrowser *)startedSessionBrowser:(BOOL)editable {
    MCSessionBrowser *b = self.sessionBrowser;
    if (!b) {
        __weak EditorController *weakSelf = self;
        b = [[MCSessionBrowser alloc] initWithWindow:self.window
            folder:self.rootPath
            tabs:^NSArray<NSArray<MCTab *> *> * {
                EditorController *s = weakSelf;
                if (!s) return @[];
                NSArray<MCTab *> *parked = [s parkedTabs];
                return parked.count ? @[ [s groupTabs], parked ] : @[ [s groupTabs] ];
            }
            active:^NSArray<MCTab *> * {
                EditorController *s = weakSelf;
                if (!s) return @[];
                NSMutableArray<MCTab *> *on = [NSMutableArray array];
                [on addObject:s->_activeTab ?: (id)NSNull.null];
                if ([s parkedTabs].count) [on addObject:[s parkedActive] ?: (id)NSNull.null];
                return on;
            }];
        b.save = ^(uint64_t docId) { [weakSelf saveSessionDocument:docId]; };
        b.open = ^(NSString *path, BOOL keep) { [weakSelf openSessionPath:path keep:keep]; };
        // This window's terminals on the host, and no other window's.
        b.terminals = ^NSArray<NSNumber *> * {
            EditorController *s = weakSelf;
            if (!s) return @[];
            NSMutableArray<NSNumber *> *ids = [NSMutableArray array];
            for (TerminalView *t in [s controlTerminals])
                if (t.sessionTerminal) [ids addObject:@(t.sessionTerminal)];
            return ids;
        };
        objc_setAssociatedObject(self, &kSessionBrowserKey, b, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    b.editable = editable;
    NSString *error = nil;
    if (![b start:&error]) {
        [self flashStatus:[NSString stringWithFormat:@"The session's page could not start: %@", error]];
        return nil;
    }
    return b;
}

- (void)showSessionInBrowser:(id)sender {
    if (![self sessionHostReadyAsking]) return;
    BOOL editable = YES;
    if (![self askSessionBrowserEditable:&editable]) return;
    MCSessionBrowser *b = [self startedSessionBrowser:editable];
    if (!b) return;
    NSURL *url = [NSURL URLWithString:b.url];
    if (MCSessionBrowserOpener) MCSessionBrowserOpener(url);
    else [NSWorkspace.sharedWorkspace openURL:url];
    [self flashStatus:editable
                          ? @"The session's page opened in the browser; it can edit this window's files, "
                            @"and opens only on this Mac."
                          : @"The session's page opened in the browser; it is read-only and only on this Mac."];
}

- (void)copySessionBrowserLink:(id)sender {
    if (![self sessionHostReadyAsking]) return;
    MCSessionBrowser *b = [self startedSessionBrowser:MCSessionBrowserEditableChoice()];
    if (!b) return;
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    [pb clearContents];
    [pb setString:b.url forType:NSPasteboardTypeString];
    [self flashStatus:@"Copied the session's link: it opens only on this Mac, and holds its key."];
}

@end
