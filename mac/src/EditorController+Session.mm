// EditorController+Session.mm — EditorController: the session's host
// (SessionLink.h, editor.session-host). The window's hooks only: the
// editor's storage reports each edit of the tab on show to the tab's
// document on the host, the text view asks which document it shows (for
// Undo and Redo), and the host's edits to a tab not in the storage reach
// its title and backups. The rest is MCSessionDoc's.
//
// Stage 6 (docs/notes/session-host.md, "The Mac reattaches"): the window's
// link and record (MCSessionWindow, an associated object), a window closing
// detaching from its host and one opening taking what a closed one left,
// the close question's Keep Unsaved on a headless host, and Reconnect when
// that host goes.
#import "EditorController+Private.h"
#import "SessionBrowser.h"
#import "SessionLink.h"
#import "SessionWindow.h"
#import "SourceStorage.h"
#include <objc/runtime.h>
#include <sys/stat.h>

#include <set>
#include <string>

#include "Backups.h"

static char kSessionWindowKey;

@implementation EditorController (Session)

// Once, when the text view is made: the storage's edits, while it holds
// the tab on show as source (_liveHighlight), go to the tab's document.
- (void)installSessionHooks {
    __weak EditorController *weakSelf = self;
    CodeTextView *tv = self.textView;
    __weak CodeTextView *weakView = tv;
    MCSourceStorage *storage = (MCSourceStorage *)tv.textStorage;
    if (![storage isKindOfClass:[MCSourceStorage class]]) return;
    storage.willReplaceCharacters = ^(NSRange range, NSString *string) {
        EditorController *c = weakSelf;
        if (!c || !c->_liveHighlight) return;
        MCTab *t = c->_activeTab;
        MCSessionDoc *d = t.session;
        if (!d) return;
        // The tab reads the storage from here on, so its text is the
        // storage's at once, never a copy from before this edit.
        CodeTextView *view = weakView;
        if (!view) return;
        [t followStorage:view.textStorage];
        [d storageWillReplace:range with:string view:view];
    };
    tv.sessionDocProvider = ^MCSessionDoc * {
        EditorController *c = weakSelf;
        if (!c || !c->_liveHighlight) return nil;
        return c->_activeTab.session;
    };
}

// A tab's text read afresh: the host's history goes too.
- (void)sessionForgetHistoryOf:(MCTab *)t {
    [t.session forgetHistoryWithText:t.currentText];
}

- (void)sessionRestoredBackupOf:(MCTab *)t saved:(NSString *)saved {
    [t.session hostSaved:saved];
}

// ---- what MCSessionDoc asks (MCTabOwner's optional methods)
- (CodeTextView *)textViewShowingSourceOf:(MCTab *)t {
    return t && t == _activeTab && _liveHighlight ? self.textView : nil;
}

- (void)tabTextChangedByHost:(MCTab *)t {
    if (t == _activeTab) {
        [self noteEdited];
        [self refreshDisplay];
        return;
    }
    NSString *now = t.currentText ?: @"", *saved = t.savedText ?: @"";
    const BOOL dirty = t.gone || ![now isEqualToString:saved];
    [self scheduleBackups];
    if (dirty == t.dirty) return;
    t.dirty = dirty;
    [self updateTitle];
}

// ---------------------------------------------------- the window on its host

- (MCSessionLinkRef *)sessionLinkForTab:(MCTab *)t {
    return [self sessionWindow].link;
}

- (MCSessionWindow *)existingSessionWindow {
    return objc_getAssociatedObject(self, &kSessionWindowKey);
}

// Made the first time it is asked for while the setting is on, for the
// window's life (moved by Open Folder and Reconnect).
- (MCSessionWindow *)sessionWindow {
    MCSessionWindow *w = [self existingSessionWindow];
    if (w || !MCSessionHostEnabled() || _closing) return w;
    // A folder whose host went: the link made for it now takes the other
    // windows still on the old one too.
    const BOOL wasLost = MCSession::folderLost(_root.path.fileSystemRepresentation ?: "");
    w = [[MCSessionWindow alloc] initWithFolder:_root.path];
    objc_setAssociatedObject(self, &kSessionWindowKey, w, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [self wireSessionWindow:w];
    [w start];
    if (wasLost) [EditorController moveWindowsToLink:w.link folder:w.link.link->folder()];
    return w;
}

- (void)wireSessionWindow:(MCSessionWindow *)w {
    __weak EditorController *weakSelf = self;
    w.tabs = ^NSArray<NSArray<MCTab *> *> * {
        EditorController *s = weakSelf;
        if (!s) return @[];
        NSArray<MCTab *> *parked = [s parkedTabs];
        return parked.count ? @[ [s groupTabs], parked ] : @[ [s groupTabs] ];
    };
    w.active = ^NSArray * {
        EditorController *s = weakSelf;
        if (!s) return @[];
        NSMutableArray *on = [NSMutableArray array];
        [on addObject:s->_activeTab ?: (id)NSNull.null];
        if ([s parkedTabs].count) [on addObject:[s parkedActive] ?: (id)NSNull.null];
        return on;
    };
    // This window's terminals on the host, and no other window's.
    w.terminals = ^NSArray<NSNumber *> * {
        EditorController *s = weakSelf;
        if (!s) return @[];
        NSMutableArray<NSNumber *> *ids = [NSMutableArray array];
        for (TerminalView *t in [s controlTerminals])
            if (t.sessionTerminal) [ids addObject:@(t.sessionTerminal)];
        return ids;
    };
    // How the dock holds them (the record's "termLayout").
    w.termDock = ^(Shared::TermDock *out) {
        EditorController *s = weakSelf;
        [s.terminalPanel sessionDock:out];
    };
    w.panes = ^NSDictionary * { return [weakSelf sessionBrowserPanes]; };
    w.browserTabs = ^NSDictionary * { return [weakSelf sessionBrowserTabs]; };
    w.reviews = ^NSArray<NSDictionary *> * { return [weakSelf sessionBrowserReviews]; };
    w.onLost = ^{ [weakSelf sessionHostWentAway]; };
}

// Where the terminal panel's next terminal goes (MCTerminalPanel
// sessionPlace): this window's link, with a held terminal while any is left.
- (MCSessionTermPlace *)sessionTerminalPlace {
    return [[self sessionWindow] nextTermPlace];
}

// The dock changed (MCTerminalPanel onChanged): the record goes on the next
// turn, not at the next half second's report.
- (void)sessionDockChanged {
    [[self existingSessionWindow] reportSoon];
}

// The window took terminals from its host: its dock is the record's
// (finishSessionAttach), not the window list's.
- (BOOL)sessionHoldsTerminals {
    return [self existingSessionWindow].heldTermsLeft > 0;
}

// ---------------------------------------------------- a window opening

// What the folder's host holds for a window opening (before the session
// restores the rest, which passes over a path open already): the latest
// record a closed window left, each of its documents as a tab with the
// host's text, unsaved where it differs from the file; its terminals wait
// for the panel (finishSessionAttach). What is taken is the core's choice
// (Shared::reattachFrom): never two documents of one path, and an untitled
// buffer (another renderer's, with no path) as the tab of a file not on
// disk yet, named as a backup of it is (Backups::untitledPath).
- (void)attachSessionHeld {
    MCSessionWindow *w = [self sessionWindow];
    if (!w || w.link.link->lost()) return;
    const MCSession::Held h = [w takeHeld];
    for (const MCSession::HeldDoc &d : h.docs) {
        NSString *path = d.path.empty() ? [self untitledPathBeside:h] : @(d.path.c_str());
        if (!path.length || [self tabForPath:path]) continue;
        MCTab *t = [MCTab new];
        t.untitled = d.path.empty();   // before the document: the host keeps it without a path
        t.currentPath = path;
        if (![t attachHeld:d.id link:w.link]) continue;
        if (t.untitled) {
            t.dirty = YES;   // nothing of it is on disk
            [self recordStamp:nil ofTab:t];
        } else {
            MCDiskText *disk = MCReadDiskText(path, ULLONG_MAX);
            t.dirty = !disk.text || ![disk.text isEqualToString:t.sourceText ?: @""];
            t.gone = !disk.stamp;
            // Saved on the host and changed on disk since: the file wins, as
            // a clean tab follows its file (read afresh when first shown).
            if (!d.dirty && disk.text && t.dirty) {
                t.dirty = NO;
                t.heldFromHost = NO;
            }
            // Saved on the host and gone from disk since: all of it is
            // unsaved, on the host too, so a headless host keeps it (and
            // backs it up) if this window leaves it again.
            if (!d.dirty && !disk.stamp) [t.session hostSaved:@""];
            [self recordStamp:disk.stamp ofTab:t];
        }
        [self groupAppendTab:t preview:Model::Preview::None];
        w.heldDocsAttached++;
    }
}

// The name an untitled buffer the host held takes: Untitled-1, -2, ... in
// the window's folder, the first that no file, no tab and no other held
// document has. The folder as the host names it (resolved: /private/tmp,
// where the window's root says /tmp), so the tab is the file the host's
// backup of it names.
- (NSString *)untitledPathBeside:(const MCSession::Held &)h {
    MCSessionWindow *w = [self existingSessionWindow];
    const std::string hostFolder = w ? w.link.link->folder() : std::string();
    NSString *folder = hostFolder.empty() ? _root.path : @(hostFolder.c_str());
    if (!folder.length) return nil;
    const std::string p = Backups::untitledPath(folder.fileSystemRepresentation, [&](const std::string &x) {
        NSString *path = @(x.c_str());
        struct stat st;
        if (lstat(x.c_str(), &st) == 0 || [self tabForPath:path]) return true;
        for (const MCSession::HeldDoc &o : h.docs)
            if (o.path == x) return true;
        return false;
    });
    return p.empty() ? nil : @(p.c_str());
}

// After the session restored the rest: the record's tab on show, and the
// held terminals in the dock as the record's "termLayout" had them (its
// tabs, splits, shares, active panes and tab on show; a tab each without
// one), after any tabs the dock has of its own (Open Folder).
- (void)finishSessionAttach {
    MCSessionWindow *w = [self existingSessionWindow];
    if (!w) return;
    if (w.heldActiveDoc) {
        for (MCTab *t in [self tabs])
            if (t.session && t.session.docId == w.heldActiveDoc && t != _activeTab) {
                [self activateTab:t];
                break;
            }
    }
    if (!_activeTab && w.heldDocsAttached && [self tabs].count) [self activateTab:[self tabs].firstObject];
    // The held documents are this window's now: their backups written at
    // once, since a headless host lets go of its own as an app opens them.
    if (w.heldDocsAttached) [self writeBackups];
    w.heldDocsAttached = 0;
    if (w.heldTermsLeft) {
        const BOOL fresh = self.terminalPanel == nil;
        [self showTerminalPanel];   // its first terminal takes the first held one
        [self.terminalPanel restoreHostDock:w.heldDock fresh:fresh];
        while (w.heldTermsLeft) [self.terminalPanel addTab];   // none: the dock placed each
    }
    [w report];
}

// Open Folder: the window's record on the old folder's host closes (its
// tabs were closed there), and it takes what the new folder's holds.
- (void)sessionFolderChanged {
    MCSessionWindow *w = [self existingSessionWindow];
    if (!w) {
        [self attachSessionHeld];
        return;
    }
    const std::string folder = _root.path.fileSystemRepresentation ?: "";
    const uint64_t was = w.windowId;
    [w moveTo:[[MCSessionLinkRef alloc] initWithLink:MCSession::forFolder(folder)] folder:_root.path];
    [self attachSessionHeld];
    // The browser's page serves one window id on one host: a new one starts
    // with the next Show.
    if (w.windowId != was) [self.sessionBrowser stop];
}

// ---------------------------------------------------- a window closing

- (BOOL)sessionKeepsUnsaved {
    MCSessionWindow *w = [self existingSessionWindow];
    return w && w.link.link->keepsUnsaved();
}

// The close question's Don't Save: these close on the host with the window.
- (void)sessionDropOnClose:(NSArray<MCTab *> *)tabs {
    MCSessionWindow *w = [self existingSessionWindow];
    if (!tabs) [w.dropOnClose removeAllObjects];
    else [w.dropOnClose addObjectsFromArray:tabs];
}

// The window is closing (windowWillClose:, and quitting): its record,
// documents and terminals stay on their hosts, but the documents the close
// question's Don't Save dropped, which close there with their tabs. What
// was typed and not yet sent goes first (the settle), since a headless
// link may be let go of next.
- (void)detachFromSessionHost {
    MCSessionWindow *w = [self existingSessionWindow];
    if (!w) return;
    std::set<MCSession::Link *> links = {w.link.link.get()};
    NSSet<MCTab *> *drop = [w.dropOnClose copy];
    BOOL keep = NO;
    for (TerminalView *t in self.terminalPanel.terminals)
        if (t.sessionTerminal) {
            keep = YES;
            break;
        }
    for (MCTab *t in [self tabs])
        if (t.session.open && ![drop containsObject:t]) keep = YES;
    [w detachKeeping:keep leavingOut:drop];
    for (TerminalView *t in self.terminalPanel.terminals) {
        if (t.sessionLink) links.insert(t.sessionLink.link.get());
        [t detachFromSessionHost];
    }
    for (MCTab *t in [self tabs]) {
        MCSessionDoc *d = t.session;
        if (!d) continue;
        links.insert(d.link.link.get());
        if ([drop containsObject:t]) [d close];
        else [d detach];
    }
    [w.dropOnClose removeAllObjects];
    for (MCSession::Link *l : links)
        if (!l->lost()) l->client().settle(5000);
    [w.reconnectButton removeFromSuperview];
}

// ---------------------------------------------------- the host gone, and back

- (void)sessionHostWentAway {
    MCSessionWindow *w = [self existingSessionWindow];
    if (!w || _closing) return;
    [self flashStatus:[NSString stringWithFormat:@"The session's host for %s went away: undo is off in its "
                                                 @"files and its terminals ended",
                                                 w.link.link->folder().c_str()]];
    [self showReconnectButton:YES];
}

- (void)showReconnectButton:(BOOL)show {
    MCSessionWindow *w = [self existingSessionWindow];
    NSButton *b = w.reconnectButton;
    if (!show) {
        b.hidden = YES;
        return;
    }
    if (!b && self.statusBar) {
        b = [NSButton buttonWithTitle:@"Reconnect" target:self action:@selector(reconnectSessionHost:)];
        b.bezelStyle = NSBezelStyleInline;
        b.controlSize = NSControlSizeSmall;
        b.font = [NSFont systemFontOfSize:11];
        b.toolTip = @"Find or start the folder's session host again and reopen your files on it";
        [b sizeToFit];
        const NSRect bar = self.statusBar.bounds;
        b.frame = NSMakeRect(NSWidth(bar) - 34 - NSWidth(b.frame), 2, NSWidth(b.frame), 18);
        b.autoresizingMask = NSViewMinXMargin;
        [self.statusBar addSubview:b];
        w.reconnectButton = b;
    }
    b.hidden = NO;
}

// The status bar's Reconnect: the folder's host found or started again, and
// every window on the old link moved to it.
- (void)reconnectSessionHost:(id)sender {
    MCSessionWindow *w = [self existingSessionWindow];
    if (!w || !w.link.link->lost()) {
        [self showReconnectButton:NO];
        return;
    }
    // A host that cannot be had again: the process's own, so nothing typed
    // is lost either way.
    const std::string folder = w.link.link->folder();
    std::shared_ptr<MCSession::Link> link = MCSession::forFolder(folder);
    if (link->kind() != MCSession::Link::Kind::Socket)
        [self flashStatus:@"The folder's session host could not be started again: your files are kept in mutible"];
    [EditorController moveWindowsToLink:[[MCSessionLinkRef alloc] initWithLink:link] folder:folder];
}

// Every window still on a lost link of `link`'s folder moves to it: its
// buffers opened there anew from their text (nothing typed is lost; their
// undo went with the old host), its terminals' shells started again.
+ (void)moveWindowsToLink:(MCSessionLinkRef *)link folder:(const std::string &)folder {
    if (!link) return;
    for (NSWindow *win in NSApp.windows) {
        EditorController *c = (EditorController *)win.delegate;
        if (![c isKindOfClass:EditorController.class]) continue;
        [c moveToSessionLink:link folder:folder];
    }
}

// The file's text as last read or saved, for the host to count the tab's
// unsaved changes against: "" for a file not on disk (an untitled buffer, a
// file gone), so all of it is unsaved; nil when it does not read as text.
- (NSString *)hostSavedTextOf:(MCTab *)t {
    if (t.untitled || t.gone) return @"";
    if (t.loaded) return t.savedText ?: @"";
    MCDiskText *disk = MCReadDiskText(t.currentPath, ULLONG_MAX);
    if (disk.text) return disk.text;
    return [NSFileManager.defaultManager fileExistsAtPath:t.currentPath] ? nil : @"";
}

- (void)moveToSessionLink:(MCSessionLinkRef *)link folder:(const std::string &)folder {
    MCSessionWindow *w = [self existingSessionWindow];
    if (!w || _closing || w.link.link == link.link) return;
    if (!w.link.link->lost() || w.link.link->folder() != folder) return;
    [w moveTo:link folder:w.folder];
    [self.sessionBrowser stop];   // its host went: the next Show serves the new one
    for (MCTab *t in [self tabs]) {
        MCSessionDoc *d = t.session;
        if (!d || !d.link.link->lost() || d.link.link->folder() != folder) continue;
        [d rehostOn:link saved:[self hostSavedTextOf:t]];
    }
    for (TerminalView *t in self.terminalPanel.terminals)
        if (t.sessionLink && t.sessionLink.link->lost() && t.sessionLink.link->folder() == folder) [t rehostOn:link];
    [self showReconnectButton:NO];
    [w report];
    if (link.link->kind() == MCSession::Link::Kind::Socket)
        [self flashStatus:[NSString stringWithFormat:@"The session's host for %s is back: your files are open on "
                                                     @"it again",
                                                     folder.c_str()]];
}

@end
