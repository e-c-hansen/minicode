// EditorController+SessionBrowser.mm — EditorController: View > Show
// Session in Browser… and Copy Session Browser Link (SessionBrowser.h). The
// window's server is kept as an associated object, so the class gains no
// ivars, and it stops itself when the window closes. Show asks whether the
// page may edit, the last answer first (editable until one is given), and
// remembers it; Copy uses the remembered one. Each makes a fresh link, good
// for one browser (SessionWebAccess.h). A page's save is the tab's
// own Cmd+S, its questions about the disk included. On an editable page a
// click on a file in the page's tree opens it here as a click in the
// window's tree would, and a tab's close button closes it here (never one
// with unsaved changes); a read-only page changes nothing here. The
// browser pane's tabs and which panes the window shows go with the
// window's tabs, and an editable page's address opens in the pane as a
// bookmark does. A LaTeX document on the page shows what this window's
// preview of it typeset (R3's third slice): each tab's preview tells the
// server of every typeset, and a page asking is given the preview's PDF,
// or, with no preview of the file in the window, one the server's own
// preview typesets from the tab's text or the disk's.
#import "EditorController+Private.h"
#import "AppSettings.h"
#import "Browser.h"
#import "Latex.h"
#import "SessionBrowser.h"
#import "SessionLink.h"
#import "SessionWindow.h"
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
                        @"them, as you can here. Each link opens the page in one browser, and "
                        @"mutible asks before a browser it does not remember connects. Read-only "
                        @"shows the files and follows your typing.";
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

// What the window's host sent, taken now.
- (void)dispatchSessionLink {
    MCSessionWindow *w = [self sessionWindow];
    if (w) w.link.link->dispatch();
    else MCSessionClient().dispatch();
}

// A page's save: every edit it made is in the tab first, then the tab's
// own save, as Cmd+S on it (or on the tab on show) would.
- (void)saveSessionDocument:(uint64_t)docId {
    [self dispatchSessionLink];
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

// An editable page's close button on one of this window's tabs: closed in
// both groups, as the tab's own close leaves the page nothing of it. One
// with unsaved changes stays (the core refused it already; typing since
// may have made it so), said in the status bar.
- (void)closeSessionDocument:(uint64_t)docId {
    [self dispatchSessionLink];
    for (MCTab *t in [self tabs]) {
        if (!t.session || t.session.docId != docId) continue;
        if (t.dirty) {
            [self flashStatus:@"A tab with unsaved changes was not closed from the browser's page"];
            return;
        }
        [self closeTabEverywhere:t prompt:YES];
        return;
    }
}

// Which panes the window shows, for the page's layout: the browser pane and
// the terminal panel open or not (Maximize Pane aside), and the browser's
// share of the width beside the editor.
- (NSDictionary *)sessionBrowserPanes {
    return @{@"browser": @(self.browserVisible && self.browser != nil),
             @"terminal": @(self.terminalVisible && self.terminalPanel != nil),
             @"browserShare": @(self.browserFraction)};
}

// The browser pane's tabs for the page, nil while the window has no pane.
- (NSDictionary *)sessionBrowserTabs {
    BrowserView *b = self.browser;
    if (!b) return nil;
    NSArray<NSString *> *urls = [b tabAddresses], *titles = [b tabPageTitles];
    NSMutableArray *tabs = [NSMutableArray array];
    for (NSUInteger i = 0; i < urls.count; i++)
        [tabs addObject:@{@"url": urls[i] ?: @"", @"title": i < titles.count ? titles[i] : @""}];
    return @{@"tabs": tabs, @"active": @((int)b.selectedTab)};
}

// An editable page's address (http or https, the core has checked): the
// pane made and shown if need be, a tab already at the address shown again,
// else a new one, the keyboard left where it was.
- (void)openSessionAddress:(NSString *)url {
    [self agentBrowserPane];
    [self keepKeyboardDuring:^{ [self.browser openAddresses:@[ url ]]; }];
}

// A tab's LaTeX preview, made: every typeset of it goes to the window's
// page, when one is served.
- (void)wireSessionTypesetOf:(LatexView *)view {
    __weak EditorController *weakSelf = self;
    view.onTypeset = ^(NSString *path, NSData *pdf, NSString *error) {
        [weakSelf.sessionBrowser typesetOf:path pdf:pdf error:error];
    };
}

// A page shows a LaTeX document: what this window's own preview of it has
// (never typeset again for the page), else a typeset of the server's own
// preview, from the tab's text when the window has the file open, else
// from the disk.
- (void)typesetForSessionPage:(NSString *)path {
    MCSessionBrowser *b = self.sessionBrowser;
    if (!b) return;
    NSString *real = path.stringByResolvingSymlinksInPath;
    MCTab *tab = nil;
    for (MCTab *t in [self tabs])
        if (t.currentPath && [t.currentPath.stringByResolvingSymlinksInPath isEqualToString:real]) {
            tab = t;
            if (t.latex) break;
        }
    if (tab.latex) {
        [b typesetOf:path view:tab.latex];
        return;
    }
    NSString *text = tab ? tab.sourceText
                         : [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    if (!text) {
        [b typesetOf:path pdf:nil error:@"The file could not be read as UTF-8 text."];
        return;
    }
    [b typesetInBackground:path source:text];
}

// The window's server, started now (editable or not) if it is not yet; nil,
// said in the status bar, when it cannot be.
- (MCSessionBrowser *)startedSessionBrowser:(BOOL)editable {
    MCSessionBrowser *b = self.sessionBrowser;
    if (!b) {
        __weak EditorController *weakSelf = self;
        MCSessionWindow *session = [self sessionWindow];
        if (!session) return nil;
        b = [[MCSessionBrowser alloc] initWithWindow:self.window session:session];
        __weak MCSessionBrowser *weakBrowser = b;
        session.onReported = ^{ [weakBrowser lookNow]; };
        b.save = ^(uint64_t docId) { [weakSelf saveSessionDocument:docId]; };
        b.open = ^(NSString *path, BOOL keep) { [weakSelf openSessionPath:path keep:keep]; };
        b.close = ^(uint64_t docId) { [weakSelf closeSessionDocument:docId]; };
        b.browse = ^(NSString *url) { [weakSelf openSessionAddress:url]; };
        b.typeset = ^(NSString *path) { [weakSelf typesetForSessionPage:path]; };
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
    NSURL *url = [NSURL URLWithString:[b newLink]];
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
    [pb setString:[b newLink] forType:NSPasteboardTypeString];
    [self flashStatus:@"Copied a fresh link to the session's page: it opens once, only on this Mac."];
}

@end
