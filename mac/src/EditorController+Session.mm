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

- (void)sessionTab:(MCTab *)t savedAs:(NSString *)saved {
    if (!saved) [t.session saved];
    else [t.session savedAs:saved];
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

// ---------------------------------------------------- saving through the host
// Stage 7 (docs/notes/session-host.md, "The Mac's tabs save through the
// host"): the window asks every question it asks (Save As's name, the disk,
// format and organize on save, the formatter's consent) and the host writes
// the file, so the host is its one writer and a page may save it too.

- (BOOL)hostWritesText:(NSString *)text ofTab:(MCTab *)t {
    if (!t.session.hostSavesFile) return NO;
    // The host writes its own text, so the tab's must be what is to be
    // written. A text of its own (a keep-sorted sort of a tab behind) goes
    // in first as an edit; the storage is never set behind the view's back,
    // and a tab on show writes what it holds, so this never refuses there.
    if ([text isEqualToString:t.currentText ?: @""]) return YES;
    return !t.followsStorage && !(t == _activeTab && _liveHighlight);
}

- (BOOL)writeTextThroughHost:(NSString *)text
                       toTab:(MCTab *)t
                 interactive:(BOOL)interactive
                     failure:(NSString **)failure {
    MCSessionDoc *d = t.session;
    NSString *name = t.currentPath.lastPathComponent;
    if (![text isEqualToString:t.currentText ?: @""]) t.sourceText = text;   // one edit on the host
    // An untitled buffer the host held is given its file's path first: the
    // host writes where its document says.
    const BOOL wasUntitled = t.untitled;
    if (wasUntitled) {
        t.untitled = NO;
        [d pathChanged];
    }
    const MCHostSave r = [d saveOnHostEncoding:(FolderSearch::TextEncoding)t.encoding bom:t.bom];
    if (!r.ok) {
        if (wasUntitled) {
            t.untitled = YES;
            [d pathChanged];
        }
        NSString *why = !r.answered ? @"The session's host did not answer."
                        : r.why.empty() ? @"The session's host could not write it."
                                        : @(r.why.c_str());
        if (failure) {
            *failure = why;
        } else if (interactive) {
            NSAlert *a = [NSAlert new];
            a.messageText = [NSString stringWithFormat:@"“%@” could not be saved.", name];
            a.informativeText = why;
            [a runModal];
        } else {
            [self showStatusNote:[NSString stringWithFormat:@"Could not auto-save %@: %@", name, why]];
        }
        return NO;
    }
    // The encoding written: a Windows-1252 file whose text holds what it
    // cannot store is UTF-8 now, and the host's note says so in the Mac's
    // words.
    t.encoding = (MCTextEncoding)r.encoding;
    t.bom = r.bom;
    if (!r.note.empty()) [self showStatusNote:@(r.note.c_str())];
    // The text the file holds: the tab's, unless someone's edit came in
    // after the host wrote (then the file's, as written). A file on another
    // machine (stage 8) has no stamp here and is not read here.
    const BOOL remote = [self sessionIsRemote];
    NSString *saved = r.inStep ? [t.currentText copy] : remote ? nil : MCReadDiskText(t.currentPath, ULLONG_MAX).text;
    if (!saved) saved = text;
    MCDiskStamp *stamp = remote ? nil
                         : r.written.exists ? MCDiskStampFromFileStamp(r.written)
                                            : MCStampOfFile(t.currentPath);
    [self tab:t tookSaveOf:saved stamp:stamp dirty:![saved isEqualToString:t.currentText ?: @""]];
    return YES;
}

// The host wrote the file for another renderer (an editable page's save):
// the tab takes it as its own save, so its dot, the title and the page's
// dot agree, and its next save is not taken for a change made outside.
- (void)tab:(MCTab *)t writtenByHost:(id)write {
    MCSessionWrite *w = write;
    if (![self holdsTab:t] || !t.loaded || ![self tabHasTextToWrite:t]) return;
    if ([self sessionIsRemote]) return;   // no stamps here of a file on another machine
    if (w.path && ![w.path isEqualToString:t.currentPath ?: @""]) return;   // renamed since
    // Written over again since: a change made outside, as the disk checks
    // see it.
    const FolderSearch::FileStamp now = FolderSearch::stampOf(t.currentPath.fileSystemRepresentation);
    if (w.stamp.exists && now != w.stamp) return;
    NSString *saved = w.text;
    if (w.knowsEncoding) {
        t.encoding = (MCTextEncoding)w.encoding;
        t.bom = w.bom;
    }
    if (!saved) {   // the tab's text was not the host's then: the file's
        MCDiskText *disk = MCReadDiskText(t.currentPath, ULLONG_MAX);
        if (!disk.text) return;
        saved = disk.text;
        t.encoding = disk.encoding;
        t.bom = disk.bom;
    }
    t.untitled = NO;
    [self tab:t tookSaveOf:saved
           stamp:MCDiskStampFromFileStamp(w.stamp.exists ? w.stamp : now)
           dirty:![saved isEqualToString:t.currentText ?: @""]];
}

NSUInteger MCDirtyFromHostMarks = 0;

// The host's count of the tab as unsaved flipped while the tab's text was
// the host's (MCSessionDoc checks): the window's dot is the host's, as the
// page's is. Never for a tab whose unsaved state is more than its text
// against the file (an untitled buffer, a deleted file), nor one not read
// yet.
- (void)tab:(MCTab *)t dirtyFromHost:(BOOL)dirty {
    if (![self holdsTab:t] || !t.loaded || t.heldFromHost || t.untitled || t.gone || t.dirty == dirty) return;
    if (dirty) MCDirtyFromHostMarks++;
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
    if (wasLost) [EditorController moveWindowsToLink:w.link folder:w.link.link->key()];
    return w;
}

// ---------------------------------------------------- a window over SSH (stage 8)
// docs/notes/session-host.md, "The Mac over SSH (stage 8)": the window is a
// renderer of `mutible host --stdio` on the other machine. Its tabs' files
// are read through that host (FsRead "read") and saved by it, its
// terminals run there, and nothing of its folder is looked for on this
// Mac's disk: no stamps, no FSEvents, no restored session.

- (void)useRemoteSessionLink:(id)link {
    MCSessionLinkRef *ref = link;
    if (!ref.link || [self existingSessionWindow]) return;
    MCSessionWindow *w = [[MCSessionWindow alloc] initWithLink:ref folder:_root.path];
    objc_setAssociatedObject(self, &kSessionWindowKey, w, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [self wireSessionWindow:w];
    [w start];
    [self startRemoteWorkspace];   // the tree, Quick Open and Find in Folder through the host
}

- (BOOL)sessionIsRemote {
    MCSessionWindow *w = [self existingSessionWindow];
    return w && w.link.link && w.link.link->kind() == MCSession::Link::Kind::Remote;
}

- (MCSessionLinkRef *)sessionRemoteLink {
    return [self sessionIsRemote] ? [self existingSessionWindow].link : nil;
}

- (NSString *)sessionRemoteLabel {
    return [self sessionIsRemote] ? @([self existingSessionWindow].link.link->label().c_str()) : nil;
}

- (NSString *)sessionRemoteKey {
    return [self sessionIsRemote] ? @([self existingSessionWindow].link.link->key().c_str()) : nil;
}

- (BOOL)sessionHostLost {
    MCSessionWindow *w = [self existingSessionWindow];
    return w && w.link.link && w.link.link->lost();
}

- (NSString *)sessionRemoteAddress {
    return [self sessionIsRemote] ? @([self existingSessionWindow].link.link->address().c_str()) : nil;
}

- (MCDiskText *)sessionRemoteTextOf:(MCTab *)t {
    if (![self sessionIsRemote]) return nil;
    return [self existingSessionWindow].link.link->readText(t.currentPath.fileSystemRepresentation ?: "");
}

// Every tab of a remote window has its document on the remote host, the
// setting or not: the host is where its file is.
- (BOOL)tabNeedsSession:(MCTab *)t {
    return [self sessionIsRemote];
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
    // `mutible host --stdio` starts afresh with each connection: nothing
    // held, and nothing of it is on this Mac's disk to compare with.
    if (w.link.link->kind() == MCSession::Link::Kind::Remote) return;
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

- (MCTab *)openUntitledSessionTab {
    if (![self sessionWindow]) return nil;
    NSString *path = [self untitledPathBeside:MCSession::Held()];
    if (!path.length) return nil;
    MCTab *t = [MCTab new];
    t.untitled = YES;   // before the document: the host keeps it without a path
    t.currentPath = path;
    [self groupAppendTab:t preview:Model::Preview::None];
    t.sourceText = @"";   // its document on the host, this window's (MCTab setSourceText:)
    t.savedText = @"";
    t.heldFromHost = YES;   // shown from its own text: there is no file to read
    [self recordStamp:nil ofTab:t];
    [self activateTab:t];
    t.dirty = NO;   // nothing typed yet: closing it asks nothing
    [self updateTitle];
    [self reloadTabStrip];
    return t;
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
    // A window over SSH ends its connection with it (stage 8): `mutible
    // host --stdio` keeps nothing for a next window, and ends as its input
    // closes, its shells with it.
    if (w.link.link->kind() == MCSession::Link::Kind::Remote) w.link.link->detach();
}

// ---------------------------------------------------- the host gone, and back

- (void)sessionHostWentAway {
    MCSessionWindow *w = [self existingSessionWindow];
    if (!w || _closing) return;
    if (w.link.link->kind() == MCSession::Link::Kind::Remote) {
        // Over SSH: the connection dropped. The text stays in the window;
        // Reconnect opens it on the other machine again.
        const std::string &why = w.link.link->failure();
        [self flashStatus:[NSString stringWithFormat:@"Disconnected from %s%s%s Your text is kept here; Reconnect "
                                                     @"opens it there again",
                                                     w.link.link->label().c_str(), why.empty() ? "." : ": ",
                                                     why.c_str()]];
        [self showReconnectButton:YES];
        [self refreshWindowTitle];
        return;
    }
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
    if (w.link.link->kind() == MCSession::Link::Kind::Remote) {
        // Over SSH: the same address again. A host that cannot be had says
        // why and the window waits, its text kept, for another try.
        std::string why;
        std::shared_ptr<MCSession::Link> link = MCSession::forRemote(w.link.link->address(), &why);
        if (!link) {
            [self flashStatus:[NSString stringWithFormat:@"Could not reconnect to %s: %s",
                                                         w.link.link->label().c_str(), why.c_str()]];
            return;
        }
        [EditorController moveWindowsToLink:[[MCSessionLinkRef alloc] initWithLink:link] folder:w.link.link->key()];
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
    if ([self sessionIsRemote]) return nil;   // its file is on the other machine
    MCDiskText *disk = MCReadDiskText(t.currentPath, ULLONG_MAX);
    if (disk.text) return disk.text;
    return [NSFileManager.defaultManager fileExistsAtPath:t.currentPath] ? nil : @"";
}

- (void)moveToSessionLink:(MCSessionLinkRef *)link folder:(const std::string &)folder {
    MCSessionWindow *w = [self existingSessionWindow];
    if (!w || _closing || w.link.link == link.link) return;
    // `folder` is the lost link's key(): its folder, and for a host over
    // SSH its machine too.
    if (!w.link.link->lost() || w.link.link->key() != folder) return;
    [w moveTo:link folder:w.folder];
    [self.sessionBrowser stop];   // its host went: the next Show serves the new one
    for (MCTab *t in [self tabs]) {
        MCSessionDoc *d = t.session;
        if (!d || !d.link.link->lost() || d.link.link->key() != folder) continue;
        [d rehostOn:link saved:[self hostSavedTextOf:t]];
    }
    for (TerminalView *t in self.terminalPanel.terminals)
        if (t.sessionLink && t.sessionLink.link->lost() && t.sessionLink.link->key() == folder) [t rehostOn:link];
    [self remoteWorkspaceMovedToLink];   // over SSH: the tree listed and watched on the new host
    [self showReconnectButton:NO];
    [w report];
    [self refreshWindowTitle];
    if (link.link->kind() == MCSession::Link::Kind::Remote)
        [self flashStatus:[NSString stringWithFormat:@"Connected to %s again: your files are open there again",
                                                     link.link->label().c_str()]];
    else if (link.link->kind() == MCSession::Link::Kind::Socket)
        [self flashStatus:[NSString stringWithFormat:@"The session's host for %s is back: your files are open on "
                                                     @"it again",
                                                     folder.c_str()]];
}

@end
