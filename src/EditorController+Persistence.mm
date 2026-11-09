// EditorController+Persistence.mm — EditorController: the tabs between runs, backups and hot exit, auto-save, files changed, moved or deleted outside.
#import "EditorController+Private.h"
#import "EditorController+PersistenceIvars.h"

@implementation EditorController (Persistence)

// ----------------------------------------------- the tabs between runs
// Each folder's open files and which was on show, in the user defaults by
// the folder's real path; the most recently used 50 folders are kept. Off
// for the demos and the memory benchmark, which must start the same way
// every time.
static NSString *const kTabSessionsKey = @"MCTabSessions";
static const NSUInteger kMaxTabSessions = 50;
// The largest file shown at launch, the GTK port's limit for opening one.
static const unsigned long long kMaxRestoredFileBytes = 16ull * 1024 * 1024;

- (NSUserDefaults *)sessionDefaults {
    // Tests point this at a scratch suite, away from the user's own.
    NSString *suite = NSProcessInfo.processInfo.environment[@"MINICODE_DEFAULTS_SUITE"];
    return suite.length ? [[NSUserDefaults alloc] initWithSuiteName:suite]
                        : [NSUserDefaults standardUserDefaults];
}

- (BOOL)sessionsEnabled {
    NSDictionary *env = NSProcessInfo.processInfo.environment;
    return !env[@"MINICODE_DEMO"] && !env[@"MINICODE_BENCH"];
}

- (NSString *)sessionKey { return _root.path.stringByResolvingSymlinksInPath; }

// The files this window would reopen, in tab order.
- (NSArray<NSString *> *)sessionFilePaths {
    NSMutableArray<NSString *> *files = [NSMutableArray array];
    for (MCTab *t in _tabs)
        if (t.currentPath && !t.agentPreview) [files addObject:t.currentPath];
    return files;
}

- (void)saveSession {
    if (![self sessionsEnabled] || !_root.path) return;
    // The file on show, or with a diff on show the file shown last.
    // An agent's preview tab is left out, so it is never the one on show.
    MCTab *shown = _activeTab.currentPath && !_activeTab.agentPreview ? _activeTab : nil;
    for (MCTab *t in _tabs) {
        if (!t.currentPath || t.agentPreview) continue;
        if (!shown) shown = t;
        else if (shown != _activeTab && t.lastActive > shown.lastActive) shown = t;
    }
    NSMutableArray<NSString *> *files = [NSMutableArray array];
    NSMutableArray<NSString *> *besides = [NSMutableArray array];   // with the page to the side
    NSInteger active = 0;
    NSString *preview = nil;   // the user's preview tab comes back as one
    for (MCTab *t in _tabs) {
        // A diff is not reopened, nor an agent's preview tab (its note is gone).
        if (!t.currentPath || t.agentPreview) continue;
        if (t == shown) active = (NSInteger)files.count;
        [files addObject:t.currentPath];
        if (t.sidePreview) [besides addObject:t.currentPath];
        if (t.preview) preview = t.currentPath;
    }
    NSString *key = [self sessionKey];
    // Windows open on the same folder share its session: this window's
    // files first, then the other windows' not already there, so whichever
    // saves last, quitting with two windows on a folder reopens all their
    // files. A window that closes while another stays open drops out of the
    // next save, so what is remembered is what was open when the folder's
    // last window went.
    id app = NSApp.delegate;
    NSArray *windows = [app respondsToSelector:@selector(controllers)]
                           ? [app valueForKey:@"controllers"] : nil;
    for (EditorController *other in windows) {
        if (other == self || ![other isKindOfClass:[EditorController class]] ||
            other->_closing || ![[other sessionKey] isEqualToString:key])
            continue;
        if (_closing) {   // the folder stays open in another window: it speaks for it
            [other saveSession];
            return;
        }
        for (NSString *f in [other sessionFilePaths])
            if (![files containsObject:f]) [files addObject:f];
    }
    NSUserDefaults *d = [self sessionDefaults];
    NSMutableDictionary *all = [[d dictionaryForKey:kTabSessionsKey] mutableCopy]
                               ?: [NSMutableDictionary dictionary];
    // The browser panel: where it was, and whether it showed.
    NSDictionary *browser = [self browserSessionState];
    if (files.count == 0 && !browser) {
        [all removeObjectForKey:key];
    } else {
        NSMutableDictionary *entry = [@{@"files": files, @"active": @(active),
                                        @"used": @([NSDate date].timeIntervalSince1970)}
                                      mutableCopy];
        if (browser) entry[@"browser"] = browser;
        if (besides.count) entry[@"sidePreview"] = besides;
        if (preview) entry[@"preview"] = preview;
        // Two editor groups: each one's files (`files` stays all of them).
        if (NSDictionary *groups = [self editorGroupsSessionState]) entry[@"groups"] = groups;
        all[key] = entry;
        while (all.count > kMaxTabSessions) {
            NSString *oldest = nil;
            double when = DBL_MAX;
            for (NSString *k in all) {
                double used = [all[k][@"used"] doubleValue];
                if (used < when) { when = used; oldest = k; }
            }
            if (!oldest) break;
            [all removeObjectForKey:oldest];
        }
    }
    [d setObject:all forKey:kTabSessionsKey];
}

// The browser panel's part of the session: its page (BrowserView
// savedState), whether it showed, and its share of the width. nil with no
// browser, or one that never loaded a page.
- (NSDictionary *)browserSessionState {
    if (!self.browser) return nil;
    NSDictionary *page = [self.browser savedState];
    if (!page) return nil;
    return @{@"page": page, @"visible": @(self.browserVisible),
             @"fraction": @(self.browserFraction)};
}

// A new window's browser comes back as it was: made now if the folder's
// session had one, shown if it showed, without taking the keyboard.
- (void)restoreBrowserFromSession {
    if (![self sessionsEnabled]) return;
    NSDictionary *s = [[self sessionDefaults] dictionaryForKey:kTabSessionsKey][[self sessionKey]];
    NSDictionary *b = [s isKindOfClass:NSDictionary.class] ? s[@"browser"] : nil;
    if (![b isKindOfClass:NSDictionary.class] || self.browser) return;
    [self makeBrowserWithHome:@""];
    [self.browser restoreState:b[@"page"]];
    const double fraction = [b[@"fraction"] doubleValue];
    if (fraction > 0.05 && fraction < 0.95) self.browserFraction = fraction;
    if ([b[@"visible"] boolValue]) {
        self.browserVisible = YES;
        [self showRightArea];
    }
    [self relayoutRightArea];
}

// Open the tabs this folder had last time; only the one on show is read
// now, the others when they are first shown.
- (void)restoreSession {
    if (![self sessionsEnabled]) return;
    NSDictionary *s = [[self sessionDefaults] dictionaryForKey:kTabSessionsKey][[self sessionKey]];
    EditorTabs::Session saved;
    NSSet *besides = nil;   // the files whose page was beside their source
    if ([s isKindOfClass:NSDictionary.class]) {
        for (id f in s[@"files"])
            if ([f isKindOfClass:NSString.class]) saved.files.push_back(Utf8String(f));
        saved.active = [s[@"active"] intValue];
        if ([s[@"sidePreview"] isKindOfClass:NSArray.class])
            besides = [NSSet setWithArray:s[@"sidePreview"]];
        if ([s[@"preview"] isKindOfClass:NSString.class])
            for (size_t i = 0; i < saved.files.size(); i++)
                if (saved.files[i] == Utf8String(s[@"preview"])) saved.preview = (int)i;
    }
    // Unsaved buffers kept from a crash, a force quit or a restart: a file of
    // the session with a backup comes back even if it is gone from disk, and
    // a backup of a file the session lost comes back after the session's.
    std::map<std::string, std::string> backupFor;
    std::vector<std::string> extra;
    if (MCBackupStore *store = [MCBackupStore shared]) {
        [store cleanUpOnce];
        Backups::Restore plan = [store planForRoot:[self sessionKey] sessionFiles:saved.files];
        for (const Backups::Restore::Item &item : plan.items) {
            backupFor[item.path] = item.id;
            if (!item.inSession) extra.push_back(item.path);
        }
    }
    NSFileManager *fm = [NSFileManager defaultManager];
    EditorTabs::Session r = EditorTabs::restorable(saved, [fm, &backupFor](const std::string &f) {
        BOOL dir = NO;
        return backupFor.count(f) ||
               ([fm fileExistsAtPath:[NSString stringWithUTF8String:f.c_str()] isDirectory:&dir] &&
                !dir);
    });
    const std::string previewFile = r.preview >= 0 ? r.files[(size_t)r.preview] : "";
    for (const std::string &f : extra) r.files.push_back(f);
    if (r.files.empty()) return;
    if (r.active < 0) r.active = 0;
    for (const std::string &f : r.files) {
        MCTab *t = [MCTab new];
        t.currentPath = [NSString stringWithUTF8String:f.c_str()];
        t.sidePreview = [besides containsObject:t.currentPath];
        auto b = backupFor.find(f);
        if (b != backupFor.end() && ![self takeBackup:@(b->second.c_str()) intoTab:t] &&
            ![fm fileExistsAtPath:t.currentPath])
            continue;   // nothing to show: the backup does not read and the file is gone
        // The preview tab comes back as one, unless unsaved changes came
        // back into it from a backup (an edit keeps a tab).
        t.preview = !previewFile.empty() && f == previewFile && !t.dirty;
        [_tabs addObject:t];
    }
    if (_tabs.count == 0) return;
    if (r.active >= (int)_tabs.count) r.active = (int)_tabs.count - 1;
    // The tab on show is read now, while the window opens. One holding a
    // file too big for that (it held up every launch) or no plain file at
    // all waits for a click instead, and the nearest other tab is shown.
    auto quick = [fm](NSString *path) {
        NSDictionary *a = [fm attributesOfItemAtPath:path error:nil];
        return [a.fileType isEqualToString:NSFileTypeRegular] &&
               a.fileSize <= kMaxRestoredFileBytes;
    };
    NSInteger active = -1;
    for (NSInteger d = 0; active < 0 && d < (NSInteger)_tabs.count; d++)
        for (NSInteger i : {(NSInteger)r.active - d, (NSInteger)r.active + d})
            if (active < 0 && i >= 0 && i < (NSInteger)_tabs.count && quick(_tabs[(NSUInteger)i].currentPath))
                active = i;
    if (active < 0) {
        [self reloadTabStrip];
        [self updateTitle];
        return;
    }
    [self activateTab:_tabs[(NSUInteger)active]];
    if ([s isKindOfClass:NSDictionary.class]) [self restoreEditorGroups:s[@"groups"]];
}

// ------------------------------------------------- backups and hot exit
// A tab whose buffer differs from disk has it written to the backup store
// (BackupStore) a second after typing stops, and at least every five seconds
// while it goes on; the backup goes as soon as the tab is clean again or is
// closed. Quitting still asks about unsaved tabs, and Don't Save deletes
// their backups; a crash, a force quit or a power loss leaves them, and the
// folder's next window brings them back as unsaved tabs. Request a Change's
// restart writes them at once and asks nothing about them.
static const NSTimeInterval kBackupDelay = 1.0, kBackupMaxWait = 5.0;

// Only text read from a file has anything to keep.
- (BOOL)tabTakesBackup:(MCTab *)t {
    return t.currentPath && t.sourceText && !t.isImage && !t.isPDF && !t.isMedia &&
           !t.isDiff && !t.showingMessage;
}

- (BOOL)tabNeedsBackup:(MCTab *)t {
    return t.dirty && [self tabTakesBackup:t] &&
           (t.backedUpText != t.sourceText || ![t.backedUpPath isEqualToString:t.currentPath]);
}

// After every edit and every change of a dirty flag: a clean tab's backup
// goes now, and a changed buffer is written when typing pauses. Costs a
// pointer comparison per tab, so it can run on every keystroke.
- (void)scheduleBackups {
    if (_closing || ![MCBackupStore shared]) return;
    BOOL pending = NO;
    for (MCTab *t in _tabs) {
        if (t.dirty && [self tabTakesBackup:t]) pending = pending || [self tabNeedsBackup:t];
        else if (t.hasBackup) [self discardBackupOf:t];
    }
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(writeBackups)
                                               object:nil];
    if (!pending) {
        _backupFirstPending = nil;
        return;
    }
    NSDate *now = [NSDate date];
    if (!_backupFirstPending) _backupFirstPending = now;
    const NSTimeInterval waited = [now timeIntervalSinceDate:_backupFirstPending];
    [self performSelector:@selector(writeBackups) withObject:nil
               afterDelay:MAX(0, MIN(kBackupDelay, kBackupMaxWait - waited))];
}

// How a tab's file is written, as its backup records it.
static NSString *MCTabEncodingName(MCTab *t) {
    return @(FolderSearch::encodingName((FolderSearch::TextEncoding)t.encoding, t.bom));
}

// Every changed buffer to the store's queue; the main thread only hands
// over the (immutable) text.
- (void)writeBackups {
    _backupFirstPending = nil;
    MCBackupStore *store = [MCBackupStore shared];
    if (_closing || !store) return;
    for (MCTab *t in _tabs) {
        if (![self tabNeedsBackup:t]) continue;
        if (!t.backupID) t.backupID = [store newID];
        [store writeID:t.backupID path:t.currentPath root:[self sessionKey]
              diskDate:t.fileModDate inode:t.fileID fileEncoding:MCTabEncodingName(t)
                  text:t.sourceText];
        t.backedUpText = t.sourceText;
        t.backedUpPath = t.currentPath;
        t.hasBackup = YES;
    }
}

- (void)discardBackupOf:(MCTab *)t {
    if (t.backupID && t.hasBackup) [[MCBackupStore shared] removeID:t.backupID];
    t.hasBackup = NO;
    t.backedUpText = nil;
    t.backedUpPath = nil;
}

// Request a Change's restart: every unsaved buffer written now, so the new
// build brings them back. NO when backups are off or one could not be
// written, and the usual question is asked instead.
- (BOOL)keepUnsavedTabsForRestart {
    MCBackupStore *store = [MCBackupStore shared];
    if (!store || ![self sessionsEnabled]) return NO;
    [self saveBeforeLeavingFormatSave];
    if (self.textView.editable && _activeTab && !_activeTab.showingMessage)
        self.sourceText = self.textView.string;
    for (MCTab *t in _tabs) {
        if (!t.dirty || ![self tabTakesBackup:t]) continue;
        if (!t.backupID) t.backupID = [store newID];
        if (![store writeNowID:t.backupID path:t.currentPath root:[self sessionKey]
                      diskDate:t.fileModDate inode:t.fileID
                  fileEncoding:MCTabEncodingName(t) text:t.sourceText])
            return NO;
        t.backedUpText = t.sourceText;
        t.backedUpPath = t.currentPath;
        t.hasBackup = YES;
    }
    [self saveSession];
    return YES;
}

// Quitting: the questions were asked (applicationShouldTerminate), so what
// is still unsaved was let go with Don't Save, and its backups go. A restart
// keeps them (keepUnsavedTabsForRestart wrote them).
- (void)backupsAppWillTerminate:(NSNotification *)note {
    MCBackupStore *store = [MCBackupStore shared];
    if (!store) return;
    id app = NSApp.delegate;
    SEL restartApp = NSSelectorFromString(@"restartApp");
    const BOOL restarting = [app respondsToSelector:restartApp] && [app valueForKey:@"restartApp"];
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(writeBackups)
                                               object:nil];
    if (!restarting)
        for (MCTab *t in _tabs) [self discardBackupOf:t];
    [store flush];
}

// A tab restored from the session takes its backup: the text, unsaved, and
// what the disk file was when the backup was taken (fileModDate and fileID,
// so a save checks the disk against that). The disk's own text is read when
// the tab is first shown.
- (BOOL)takeBackup:(NSString *)bid intoTab:(MCTab *)t {
    Backups::Entry e;
    NSString *text = nil;
    if (![[MCBackupStore shared] loadID:bid entry:&e text:&text]) return NO;
    t.backupID = bid;
    t.hasBackup = YES;        // written again soon under this process's name
    t.restoredBackup = YES;
    t.sourceText = text;
    t.dirty = YES;
    t.fileModDate = e.diskExisted ? [NSDate dateWithTimeIntervalSince1970:e.diskMtime] : nil;
    t.fileID = e.diskInode;
    t.fileSize = -1;   // a backup does not say
    // How the file is written, for a file gone by now; one still there
    // says so itself when it is read (loadActiveTab).
    FolderSearch::TextEncoding encoding;
    bool bom;
    if (FolderSearch::parseEncodingName(e.fileEncoding, &encoding, &bom)) {
        t.encoding = (MCTextEncoding)encoding;
        t.bom = bom;
    }
    MCDiskStamp *a = MCStampOfFile(t.currentPath);
    t.gone = e.diskExisted && !a;
    // Unchanged since: the disk's own date, exactly, so a save in the
    // background does not take a rounding for a change made outside.
    if (e.diskExisted && a && fabs(a.modDate.timeIntervalSince1970 - e.diskMtime) < 0.001 &&
        (!e.diskInode || a.fileID == e.diskInode))
        t.fileModDate = a.modDate;
    return YES;
}

// The active tab was just read from disk (loadActiveTab) and had a backup:
// the backup's text goes in as the unsaved buffer, with nothing to undo, and
// the disk's text stays savedText. A file changed since the backup keeps the
// backup's date, so the next check (window focus, Cmd+S) asks Keep Mine or
// Reload; a file gone is a gone tab holding the text.
- (void)showRestoredText:(NSString *)text base:(NSDate *)base inode:(uint64_t)inode {
    MCTab *t = _activeTab;
    NSString *name = t.currentPath.lastPathComponent;
    NSString *note = [NSString stringWithFormat:@"Restored unsaved changes to %@", name];
    self.sourceText = text;
    if (base && !t.fileModDate) {
        t.gone = YES;
        t.fileModDate = base;
        t.fileID = inode;
        note = [note stringByAppendingString:@"; the file is no longer on disk"];
    } else if (base && (fabs([t.fileModDate timeIntervalSinceDate:base]) > 0.001 ||
                        (inode && t.fileID && inode != t.fileID))) {
        t.fileModDate = base;
        t.fileID = inode;
        note = [note stringByAppendingString:@"; the file has changed on disk since"];
    }
    self.dirty = t.gone || ![text isEqualToString:self.savedText ?: @""];
    if (self.dirty) [self showStatusNote:note];
}

// ------------------------------------------------------------- auto-save
// editor.auto-save (Settings.h). after-delay writes every tab with unsaved
// changes once typing has paused for editor.auto-save-delay, as VS Code's
// files.autoSave afterDelay; focus-change writes them when the editor loses
// the keyboard, the window stops being key, or another tab is shown. An
// auto-save is a plain write: no formatter (format.on-save stays with
// Cmd+S, as VS Code leaves it with afterDelay), no keep-sorted (it would
// rewrite text under the caret), and never a question. A file changed on
// disk since it was read is skipped and named in the status bar, and Cmd+S
// asks Keep Mine or Reload as before; a file gone from disk, a tab never
// read yet, and a tab whose Cmd+S waits for its formatter are left alone.
// Claude Code editing the same files is the case this is for: buffers are
// written within a second, so they rarely differ from the disk when it
// writes, and clean tabs follow its writes quietly (checkTabsOnDisk).

- (Settings::AutoSave)autoSaveMode {
    return [AppSettings shared].settings.autoSave();
}

// Not a tab with an inline agent edit waiting for Keep or Discard (the
// agent edits only the tab on show): it is written once the bar closes.
- (BOOL)tabTakesAutoSave:(MCTab *)t {
    return t.dirty && t.loaded && !t.gone && [self tabTakesBackup:t] &&
           t != _formatSaveTab && !(t == _activeTab && _agent.editPending);
}

// After every edit and every change of a dirty flag, beside scheduleBackups.
- (void)scheduleAutoSave {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(autoSaveAfterDelay)
                                               object:nil];
    if (_closing || [self autoSaveMode] != Settings::AutoSave::AfterDelay) return;
    BOOL any = NO;
    for (MCTab *t in _tabs) any = any || [self tabTakesAutoSave:t];
    if (!any) return;
    const int ms = [AppSettings shared].settings.autoSaveDelay();
    [self performSelector:@selector(autoSaveAfterDelay) withObject:nil afterDelay:ms / 1000.0];
}
- (void)autoSaveAfterDelay { [self autoSaveNow]; }

// focus-change: the editor lost the keyboard, the window lost key, or the
// tab on show is about to change. Saved on the next turn of the run loop,
// not inside the responder change (a selector of its own, which the
// debounce above never cancels).
- (void)autoSaveOnFocusChange {
    if (_closing || [self autoSaveMode] != Settings::AutoSave::FocusChange) return;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(autoSaveNow)
                                               object:nil];
    [self performSelector:@selector(autoSaveNow) withObject:nil afterDelay:0];
}

// Every tab that takes an auto-save, written now, unasked.
- (void)autoSaveNow {
    if (_closing || [self autoSaveMode] == Settings::AutoSave::Off) return;
    if (_activeTab && self.textView.editable && !_activeTab.showingMessage)
        self.sourceText = self.textView.string;
    NSMutableArray<NSString *> *changed = [NSMutableArray array];
    for (MCTab *t in [_tabs copy]) {
        if (![self tabTakesAutoSave:t]) continue;
        // Text an input method is still composing is not written; the edit
        // that ends the composition schedules the next auto-save.
        if (t == _activeTab && self.textView.hasMarkedText) continue;
        MCDiskStamp *disk = MCStampOfFile(t.currentPath);
        if (!disk && t.fileModDate) {   // deleted or moved: marked, not written again
            [self tabWentMissing:t];
            continue;
        }
        // Changed (or made) on disk since it was read: never written over
        // unasked. Cmd+S asks. The size and inode count as well as the date,
        // which file systems with coarse times (SMB, exFAT) can leave alone.
        if ([self tab:t changedOnDisk:disk]) {
            [changed addObject:t.currentPath.lastPathComponent ?: @"?"];
            continue;
        }
        [self writeTabForAutoSave:t];
    }
    if (changed.count)
        [self showStatusNote:[NSString stringWithFormat:
            @"Not auto-saved: %@ changed on disk. ⌘S asks what to do.",
            [changed componentsJoinedByString:@", "]]];
}

- (BOOL)writeTabForAutoSave:(MCTab *)t {
    const BOOL shown = t == _activeTab;
    NSString *text = shown && self.textView.editable && !t.showingMessage
                         ? [self.textView.string copy] : (t.sourceText ?: @"");
    if (![self writeText:text toTab:t interactive:NO]) return NO;
    t.sourceText = text;
    return YES;
}

// A clean tab whose file is larger than this is not followed live: each
// change would be read and diffed whole (a log a job appends to, several
// times a second). It is read again when shown or when the window comes
// forward, and the status bar says so once.
static const unsigned long long kFollowLiveBytes = 8ull * 1024 * 1024;

// A tab with nothing unsaved whose file changed on disk takes the new text
// with no question, as one undoable step ("Reload from Disk"), so the steps
// before it stay good and Cmd+Z brings the old text back. The tab on show
// keeps its caret and the line at the top of the pane (the change goes in
// as the lines that differ, as Format Document's does). Read here, on the
// main thread: the window came forward or the tab was shown.
- (void)reloadCleanTab:(MCTab *)t {
    [self takeDiskRead:MCReadDiskText(t.currentPath, ULLONG_MAX) intoCleanTab:t diff:nullptr];
}

// The same from FSEvents: read, compared and diffed on another thread,
// then put in on the main thread if nothing changed meanwhile; a file over
// kFollowLiveBytes is only noted.
- (void)followCleanTabInBackground:(MCTab *)t {
    const NSUInteger generation = ++t.diskReadGeneration;
    NSString *path = t.currentPath, *saved = t.savedText ?: @"";
    // The text a diff is made against: the text view's, when it shows the
    // tab as editable text (equal to saved, the tab being clean).
    const BOOL shownText = t == _activeTab && self.textView.editable && !t.showingMessage;
    __weak EditorController *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        MCDiskText *d = MCReadDiskText(path, kFollowLiveBytes);
        std::shared_ptr<MCSourceDiff> diff;
        if (d.text && shownText && ![d.text isEqualToString:saved]) {
            diff = std::make_shared<MCSourceDiff>();
            diff->from = U16(saved);
            diff->to = U16(d.text);
            diff->hunks = LineDiff::diff(diff->from, diff->to);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            EditorController *s = weakSelf;
            if (!s || s->_closing || t.diskReadGeneration != generation ||
                ![t.currentPath isEqualToString:path] || t.dirty || !t.loaded ||
                ![t.savedText ?: @"" isEqualToString:saved] || ![s->_tabs containsObject:t])
                return;
            if (d.tooLarge) {
                if (!t.tooLargeToFollow)
                    [s showStatusNote:[NSString stringWithFormat:
                        @"%@ changed on disk. It is too large to follow live, so it is read "
                         "again when shown or when the window comes forward.",
                        path.lastPathComponent]];
                t.tooLargeToFollow = YES;
                return;
            }
            // The diff was made against the text view as it was; still so?
            const BOOL stillShown = t == s->_activeTab && s.textView.editable && !t.showingMessage;
            [s takeDiskRead:d intoCleanTab:t
                       diff:diff && stillShown && [s.textView.string isEqualToString:saved]
                                ? diff : nullptr];
        });
    });
}

- (void)takeDiskRead:(MCDiskText *)d intoCleanTab:(MCTab *)t
                diff:(std::shared_ptr<MCSourceDiff>)diff {
    NSString *fresh = d.text;
    if (!fresh) return;   // gone, or not text now: the stamp stays, so it is looked at again
    t.diskReadGeneration++;   // any read still out is older than this
    t.bom = d.bom;
    t.encoding = d.encoding;
    if ([fresh isEqualToString:t.savedText ?: @""] && !t.gone) {   // touched, not changed
        [self recordStamp:d.stamp ofTab:t];
        return;
    }
    t.savedText = fresh;   // so the edit below reads as clean
    t.gone = NO;
    t.folds = nil;
    [t.latex forgetEdits];
    if (t == _activeTab && self.textView.editable && !t.showingMessage) {
        if (!diff || ![self replaceSourceWithDiff:diff actionName:@"Reload from Disk"])
            [self replaceSourceWithText:fresh actionName:@"Reload from Disk"];
        self.sourceText = self.textView.string;
    } else {
        // A tab in the background (or a preview): its text now, the old
        // text a Cmd+Z away once it is shown.
        NSString *old = t.sourceText ?: @"";
        t.sourceText = fresh;
        if (![old isEqualToString:fresh]) [self registerReloadUndoForTab:t text:old];
        if (t == _activeTab) [self refreshDisplay];
        else [self.lsp tabsChanged];   // the server gets the disk's text for it too
    }
    t.dirty = NO;
    t.pendingSort = nil;
    [self recordStamp:d.stamp ofTab:t];
    [self updateTitle];
}

// Undoing a reload in a tab that was not on show as text: the old text
// comes back, unsaved; redo takes the disk's again.
- (void)registerReloadUndoForTab:(MCTab *)t text:(NSString *)old {
    __weak MCTab *weakTab = t;
    [t.undoManager registerUndoWithTarget:self handler:^(EditorController *s) {
        MCTab *tab = weakTab;
        if (tab) [s putBackText:old inTab:tab];
    }];
    [t.undoManager setActionName:@"Reload from Disk"];
}

- (void)putBackText:(NSString *)text inTab:(MCTab *)t {
    NSString *now = t == _activeTab && self.textView.editable && !t.showingMessage
                        ? [self.textView.string copy] : (t.sourceText ?: @"");
    // Its redo (or undo again) is this same step the other way. The text
    // view's own registration stays out of it: made during an undo, it
    // redid onto the wrong range.
    [self registerReloadUndoForTab:t text:now];
    if (t == _activeTab && self.textView.editable && !t.showingMessage) {
        NSUndoManager *um = self.textView.undoManager;
        [um disableUndoRegistration];
        [self replaceSourceWithText:text actionName:@"Reload from Disk"];
        [um enableUndoRegistration];
        self.sourceText = self.textView.string;
        [self noteEdited];
        return;
    }
    t.sourceText = text;
    t.dirty = t.gone || ![text isEqualToString:t.savedText ?: @""];
    if (t == _activeTab) [self refreshDisplay];
    else [self.lsp tabsChanged];
    [self updateTitle];
    [self scheduleBackups];
    [self scheduleAutoSave];
}

// Called when the window regains focus: reconcile with on-disk changes.
- (void)checkExternalChange {
    if (!self.currentPath || !self.fileModDate) return;
    MCTab *t = _activeTab;
    MCDiskStamp *disk = MCStampOfFile(self.currentPath);
    if (!disk) {                             // moved or deleted
        [self tabWentMissing:t];
        return;
    }
    if (t.gone) [self tabCameBack:t];
    if (![self tab:t changedOnDisk:disk]) return;   // unchanged

    // Pictures, PDFs, players and viewers show the file itself: shown again.
    if (self.isImage || self.isPDF || self.isMedia || MCViewerKindWithoutText(self.viewerKind)) {
        [self recordStamp:disk ofTab:t];
        if (self.isImage) [self showImageAtPath:self.currentPath];
        else if (self.isPDF) [self showPDFAtPath:self.currentPath];
        else if (self.isMedia) [self showMediaAtPath:self.currentPath];
        else [self showViewer:self.viewerKind];
        return;
    }
    // The tab's date moves only once the new text is in: a read that fails
    // (caught mid-character, or in another encoding now) leaves the change
    // to be seen again, never the old text marked as the disk's.
    if (!self.dirty) {                       // no local edits: reload quietly
        [self reloadCleanTab:t];
        return;
    }
    MCDiskText *fresh = MCReadDiskText(self.currentPath, ULLONG_MAX);
    NSAlert *a = [[NSAlert alloc] init];     // local edits AND disk changed
    a.messageText = [NSString stringWithFormat:
        @"“%@” changed on disk.", self.currentPath.lastPathComponent];
    a.informativeText = @"You have unsaved changes here. Keep your version, "
                         "or reload the file from disk and lose them?";
    [a addButtonWithTitle:@"Keep Mine"];     // 1000
    [a addButtonWithTitle:@"Reload"];        // 1001
    if ([a runModal] == NSAlertSecondButtonReturn) {
        if (fresh.text) [self takeDiskText:fresh forTab:t];
    } else {
        // Kept: what counts as unchanged from now on is the file's new
        // text, and the file as it is now is what a save is checked against.
        if (fresh.text) {
            self.savedText = fresh.text;
            t.bom = fresh.bom;
            t.encoding = fresh.encoding;
        }
        [self recordStamp:fresh.stamp ?: disk ofTab:t];
        [self noteEdited];
    }
}

// Put the file's text from disk in a tab, as if it had just been opened:
// clean, with nothing to undo (the user chose to let their changes go). A
// background tab shows it when shown.
- (void)takeDiskText:(MCDiskText *)d forTab:(MCTab *)t {
    if (!t || !d.text) return;
    t.diskReadGeneration++;
    t.sourceText = d.text;
    t.savedText = d.text;
    t.bom = d.bom;
    t.encoding = d.encoding;
    t.dirty = NO;
    t.gone = NO;
    t.pendingSort = nil;
    [self recordStamp:d.stamp ofTab:t];
    [self forgetHistoryOf:t];
    if (t == _activeTab) [self refreshDisplay];
    else [self.lsp tabsChanged];   // the server gets the disk's text for it too
    [self updateTitle];
}

// ------------------------------------------- files moved or deleted outside
// A tab's file is no longer at its path. A rename within its folder (mv, git
// mv) is followed when a file there has the same inode and modification
// date, which a rename keeps and nothing else produces; otherwise the tab is
// marked: "(deleted)" in its title, unsaved, and Cmd+S asks first.
- (void)tabWentMissing:(MCTab *)t {
    if (!t.currentPath || !t.fileModDate || t.gone) return;
    if ([[NSFileManager defaultManager] fileExistsAtPath:t.currentPath]) return;
    NSString *moved = [self renamedFileFor:t];
    if (moved) {
        [self pathMovedFrom:t.currentPath to:moved];
        if (t == _activeTab) [self reselectCurrentFileInTree];
        return;
    }
    t.gone = YES;
    // Only text has anything to save; a picture or a video is just marked.
    if (!t.isImage && !t.isPDF && !t.isMedia && !t.isDiff && !t.showingMessage && t.loaded)
        t.dirty = YES;
    [self updateTitle];
    [self saveSession];
}

- (void)tabCameBack:(MCTab *)t {
    if (!t.gone) return;
    t.gone = NO;
    NSString *now = t.sourceText ?: @"", *saved = t.savedText ?: @"";
    t.dirty = t.loaded && ![now isEqualToString:saved];
    [self updateTitle];
}

- (NSString *)renamedFileFor:(MCTab *)t {
    if (!t.fileID) return nil;
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *dir = t.currentPath.stringByDeletingLastPathComponent;
    for (NSString *name in [fm contentsOfDirectoryAtPath:dir error:nil]) {
        NSString *p = [dir stringByAppendingPathComponent:name];
        NSDictionary *a = [fm attributesOfItemAtPath:p error:nil];
        if (![a.fileType isEqualToString:NSFileTypeRegular]) continue;   // not a link to it
        if ([a[NSFileSystemFileNumber] unsignedLongLongValue] != t.fileID) continue;
        if (![MCStampOfFile(p).modDate isEqualToDate:t.fileModDate]) continue;
        if ([self tabForPath:p]) return nil;   // already open in another tab
        return p;
    }
    return nil;
}

// After a change in the folder (FSEvents): every tab whose file went away
// or came back, and every text tab with nothing unsaved whose file changed
// (Claude Code in the terminal, git checkout), which takes the new text
// quietly, keeping its place. A tab with unsaved changes is left as it is
// until it is saved (Cmd+S asks, an auto-save skips it), shown again, or
// the window comes forward.
- (void)checkTabsOnDisk {
    for (MCTab *t in [_tabs copy]) {
        if (!t.currentPath || !t.fileModDate) continue;
        MCDiskStamp *disk = MCStampOfFile(t.currentPath);
        if (!disk) {
            [self tabWentMissing:t];
            continue;
        }
        // Deleted and made again (a branch switch, some formatters): back,
        // and read in the same pass, so the tab never shows the old text as
        // clean meanwhile.
        if (t.gone) {
            if (t == _activeTab) {
                [self checkExternalChange];   // back, and read if clean
                continue;
            }
            [self tabCameBack:t];
        }
        // Read and diffed off the main thread.
        if (!t.dirty && t.loaded && [self tabTakesBackup:t] && [self tab:t changedOnDisk:disk])
            [self followCleanTabInBackground:t];
    }
}

// Before writing a tab's file: NO when the save must not go ahead. A file
// changed on disk since it was read asks Keep Mine (write over it), Reload
// (take the disk's text, nothing is written) or Cancel; a file no longer
// there asks before it is written again. Save All asks the same, file by
// file, so nothing outside is overwritten unasked, but asks all of them
// before it writes any (confirmCloseAllTabs).
- (BOOL)mayWriteTab:(MCTab *)t {
    if ([_approvedWrites containsObject:t]) return YES;   // Save All asked already
    MCDiskDecision d = [self diskDecisionForTab:t];
    if (d == MCDiskReload) [self reloadTabFromDisk:t];
    return d == MCDiskWrite;
}

- (void)reloadTabFromDisk:(MCTab *)t {
    [self takeDiskText:MCReadDiskText(t.currentPath, ULLONG_MAX) forTab:t];
}

// The question mayWriteTab: asks, without acting on the answer.
- (MCDiskDecision)diskDecisionForTab:(MCTab *)t {
    if (!t.fileModDate) return MCDiskWrite;   // never on disk as far as we know
    MCDiskStamp *disk = MCStampOfFile(t.currentPath);
    if (disk && ![self tab:t changedOnDisk:disk] && !t.gone) return MCDiskWrite;
    NSString *name = t.currentPath.lastPathComponent;
    NSAlert *a = [[NSAlert alloc] init];
    if (!disk) {
        a.messageText = [NSString stringWithFormat:@"“%@” is no longer on disk.", name];
        a.informativeText = @"It was moved or deleted after it was opened here. "
                             "Save your version at its old place?";
        [a addButtonWithTitle:@"Save"];      // 1000
        [a addButtonWithTitle:@"Cancel"];    // 1001
        return [a runModal] == NSAlertFirstButtonReturn ? MCDiskWrite : MCDiskCancel;
    }
    if (t.gone) {   // it came back: compare with what is there now
        [self tabCameBack:t];
        if (![self tab:t changedOnDisk:disk]) return MCDiskWrite;
    }
    a.messageText = [NSString stringWithFormat:@"“%@” changed on disk.", name];
    a.informativeText = @"It changed after it was opened here. Keep your version "
                         "and write over it, or reload the file from disk and lose "
                         "your changes?";
    [a addButtonWithTitle:@"Keep Mine"];     // 1000
    [a addButtonWithTitle:@"Reload"];        // 1001
    [a addButtonWithTitle:@"Cancel"];        // 1002
    NSModalResponse r = [a runModal];
    return r == NSAlertFirstButtonReturn ? MCDiskWrite
         : r == NSAlertSecondButtonReturn ? MCDiskReload : MCDiskCancel;
}

@end
