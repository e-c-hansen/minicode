// EditorController+Groups.mm — EditorController: two editor groups side by
// side, VS Code's Split Editor (View > Split Editor, Cmd+\ in the editor).
//
// Each group has its own row of tabs. A file open in both is one document
// (one MCTab: one text, one undo history, one backup, one server copy)
// shown twice. The window's one editor, with everything it carries (the
// highlighter, the language servers, folds, carets, the find bar, notes,
// the viewers), is always in the focused group; the other group's tab is
// shown by an MCEditorMirror (EditorMirror.h): the editor's own text
// storage through a second layout manager when it is the same tab, so
// typing shows in both at once, else a copy of that tab's text, or a
// picture of what the editor showed for it. A click in the other group, a
// click on its strip, Cmd+1 or Cmd+2 moves the editor there (focusEditorGroup:),
// at the place the group showed, and a click goes on to the same character.
//
// The groups themselves are the core's Model::Window (_win, EditorTabs.h):
// each group's tabs, the tab each shows, the focused group and the share.
// Every open document (tabs) is a list of its own, so backups, auto-save,
// sessions, the disk checks and the servers' open files need nothing of
// groups; the strip, the tab keys and closing go by the focused group's
// list (groupTabs). Closing a tab that the other group also holds asks nothing
// (the document stays); closing a group's last tab closes the group.
#import "EditorController+Private.h"
#import "EditorController+GroupsIvars.h"
#include "SyntaxHighlighter.h"

// Neither group narrower than this while there is room for both.
static const CGFloat kMinGroupWidth = 150;
// A tab's text past this is copied into the other group plain, uncolored.
static const NSUInteger kMirrorColorLimit = 2u << 20;

@implementation EditorController (Groups)

// ------------------------------------------------------------ the lists
// The window's tabs are the core's Model::Window (_win): each group's tabs
// in order, the tab each shows, the focused group, the preview tabs, the
// closed tabs. It names documents by id; _docs gives each id's MCTab.
- (MCTab *)tabWithId:(Model::DocId)i { return i ? _docs[@(i)] : nil; }

- (NSArray<MCTab *> *)tabsWithIds:(const std::vector<Model::DocId> &)ids {
    NSMutableArray<MCTab *> *out = [NSMutableArray arrayWithCapacity:ids.size()];
    for (Model::DocId i : ids)
        if (MCTab *t = _docs[@(i)]) [out addObject:t];
    return out;
}

- (BOOL)editorSplit { return _win->split(); }
- (BOOL)isSplit { return _win->split(); }
- (NSInteger)focusedGroup { return _win->focused(); }

- (NSArray<MCTab *> *)tabs { return [self tabsWithIds:_win->documents()]; }
- (BOOL)holdsTab:(MCTab *)t { return t && _win->isOpen(t.docId) && _docs[@(t.docId)] == t; }
- (NSArray<MCTab *> *)groupTabs { return [self tabsWithIds:_win->tabs()]; }
- (NSArray<MCTab *> *)parkedTabs {
    return _win->split() ? [self tabsWithIds:_win->tabs(_win->other())] : nil;
}
- (MCTab *)parkedActive {
    return _win->split() ? [self tabWithId:_win->active(_win->other())] : nil;
}

- (BOOL)otherGroupHoldsTab:(MCTab *)t {
    return t && _win->split() && _win->inGroup(t.docId, _win->other());
}

// A tab the window takes: its id is known by _docs, and the tab asks the
// window what it is (MCTabOwner).
- (void)adoptTab:(MCTab *)t {
    _docs[@(t.docId)] = t;
    t.owner = self;
}

// The tabs neither group holds any more are let go.
- (void)forgetClosedTabs {
    for (NSNumber *i in _docs.allKeys) {
        if (_win->isOpen(i.unsignedLongLongValue)) continue;
        MCTab *t = _docs[i];
        if (t.owner == self) t.owner = nil;
        [_docs removeObjectForKey:i];
    }
}

- (void)groupInsertTab:(MCTab *)t atIndex:(NSUInteger)i {
    if (!t || _win->isOpen(t.docId)) return;
    [self adoptTab:t];
    _win->insert(t.docId, t.currentPath ? Utf8String(t.currentPath) : "",
                 (int)MIN(i, (NSUInteger)_win->count()));
}

// A new tab by the window's rules (Model::Window::add): where it goes, and
// which preview tab of its kind it replaces. An old preview with unsaved
// changes (from a backup) is kept instead; `passOver`, the tab the user is
// in while they type, is left alone. The replaced tab is returned for
// letGoOfReplacedTab: once the new one is on show.
- (MCTab *)groupAddTab:(MCTab *)t preview:(Model::Preview)kind passOver:(MCTab *)passOver {
    if (!t || _win->isOpen(t.docId)) return nil;
    [self adoptTab:t];
    const BOOL wasSplit = _win->split();
    MCTab *otherShown = [self parkedActive];
    const Model::DocId skip = passOver ? passOver.docId : 0;
    __weak EditorController *weakSelf = self;
    const Model::DocId old = _win->add(
        t.docId, t.currentPath ? Utf8String(t.currentPath) : "", kind,
        [weakSelf](Model::DocId id) { return (bool)[weakSelf tabWithId:id].dirty; },
        [skip](Model::DocId id) { return skip && id == skip; });
    // The agents' replaced tab leaves the other group too.
    if (wasSplit && !_win->split()) {
        [self editorGroupsJoined];
    } else if (_win->split()) {
        if (otherShown != [self parkedActive]) {
            _mirrorTab = nil;   // shown afresh
            [self syncEditorMirror];
        }
        [self reloadParkedStrip];
    }
    return [self tabWithId:old];
}

// The preview tab a newer one replaced (groupAddTab:): no question (it has
// no unsaved changes, since an edit keeps it) and no place in Reopen Closed
// Tab, as VS Code leaves a replaced preview editor out of its recently
// closed list. A user's preview tab the other group still shows stays
// there.
- (void)letGoOfReplacedTab:(MCTab *)t {
    if (!t) return;
    if (![self holdsTab:t] && t != _activeTab) {
        [self discardBackupOf:t];
        [t.latex removeFromSuperview];
        // A page in the preview tab (a review or a commit from Source
        // Control) goes as a closed tab's does: its view and its reads.
        if (t.page) [self closePageOf:t];
        [self forgetClosedTabs];
    }
    [self.lsp tabsChanged];
    [self reloadTabStrip];
    [self saveSession];
}

- (void)groupAppendTab:(MCTab *)t preview:(Model::Preview)kind {
    if (!t || _win->isOpen(t.docId)) return;
    [self adoptTab:t];
    _win->append(t.docId, t.currentPath ? Utf8String(t.currentPath) : "", kind);
}

// The other group's tab `t` goes (closed there, or moved): the group shows
// the tab it used last, and with none left the editor is one group again.
- (void)removeTabFromOtherGroup:(MCTab *)t {
    if (![self otherGroupHoldsTab:t]) return;
    const BOOL wasShown = t == [self parkedActive];
    const Model::Window::Closed c = _win->close(t.docId, _win->other(), nullptr);
    [self forgetClosedTabs];
    if (c.groupClosed) {
        [self editorGroupsJoined];
        return;
    }
    if (wasShown) {
        _mirrorTab = nil;   // shown afresh
        [self syncEditorMirror];
    }
    [self reloadParkedStrip];
}

// ---------------------------------------------- what MCTab asks (MCTabOwner)
- (BOOL)tabIsPreview:(MCTab *)t { return _win->preview(t.docId) == Model::Preview::User; }
- (BOOL)tabIsAgentPreview:(MCTab *)t { return Model::isAgentPreview(_win->preview(t.docId)); }
- (uint64_t)tabLastUsed:(MCTab *)t { return _win->lastUsed(t.docId); }
- (void)tabPathChanged:(MCTab *)t {
    if ([self holdsTab:t]) _win->setPath(t.docId, t.currentPath ? Utf8String(t.currentPath) : "");
}

// ------------------------------------------------------------ the views
- (void)makeEditorGroupViews {
    if (_mirror) return;
    _mirror = [[MCEditorMirror alloc] initInView:self.rightArea];
    __weak EditorController *weakSelf = self;
    _mirror.onPress = ^(NSEvent *e, NSUInteger c) { [weakSelf otherGroupPressed:e character:c]; };
    MCTabStrip *strip = [[MCTabStrip alloc] initWithFrame:NSZeroRect];
    strip.hidden = YES;
    strip.surface = @"tabs";
    [self wireTabStrip:strip];
    [self.rightArea addSubview:strip];
    _parkedStrip = strip;
    _groupDivider = [[DragBar alloc] initWithFrame:NSZeroRect];
    _groupDivider.vertical = YES;
    _groupDivider.hidden = YES;
    _groupDivider.onDrag = ^(CGFloat x) { [weakSelf groupBarDraggedTo:x]; };
    [self.rightArea addSubview:_groupDivider];
    // The other group, while it lays out the editor's own storage, lets go
    // before the whole text is replaced (another tab, a preview).
    if ([self.textView.textStorage isKindOfClass:MCSourceStorage.class])
        ((MCSourceStorage *)self.textView.textStorage).willReplaceAll = ^{
            [weakSelf editorTextWillBeReplaced];
        };
    [self applyGroupSettings];
}

- (NSView *)groupDivider { return _win->split() ? _groupDivider : nil; }

- (void)groupBarDraggedTo:(CGFloat)x {
    const CGFloat W = NSWidth(_groupSlot);
    if (W <= 0) return;
    const CGFloat left = MIN(MAX(x - NSMinX(_groupSlot), kMinGroupWidth), W - kMinGroupWidth);
    _win->setFraction(MIN(MAX(left / W, 0.05), 0.95));
    [self relayoutRightArea];
    [self saveSession];
}

// Called by relayoutRightArea with the editor's slot: the other group, its
// strip and the bar between them laid out; returns the focused group's part
// of the slot, for the editor and everything that shares its place.
- (NSRect)layoutEditorGroupsIn:(NSRect)slot editorShown:(BOOL)shown {
    if (!_win->split() || !_mirror) {
        if (_mirror) {
            [_mirror layoutInRect:NSZeroRect hidden:YES];
            _parkedStrip.hidden = YES;
            _groupDivider.hidden = YES;
        }
        _liveSlot = slot;
        return slot;
    }
    _groupSlot = slot;
    const CGFloat W = NSWidth(slot);
    const CGFloat lw = EditorTabs::leftGroupWidth(W, _win->fraction(), kMinGroupWidth);
    const NSRect left = NSMakeRect(NSMinX(slot), NSMinY(slot), lw, NSHeight(slot));
    const NSRect right = NSMakeRect(NSMinX(slot) + lw, NSMinY(slot), W - lw, NSHeight(slot));
    const NSRect live = _win->focused() == 0 ? left : right;
    const NSRect other = _win->focused() == 0 ? right : left;
    const CGFloat h = MIN(MCTabStripHeight(), NSHeight(other));
    _parkedStrip.hidden = !shown;
    _parkedStrip.frame = NSMakeRect(NSMinX(other), NSMaxY(other) - h, NSWidth(other), h);
    [_mirror layoutInRect:NSMakeRect(NSMinX(other), NSMinY(other), NSWidth(other),
                                     NSHeight(other) - h)
                   hidden:!shown];
    _groupDivider.frame = NSMakeRect(NSMinX(right) - kDragBarHeight / 2, NSMinY(slot),
                                     kDragBarHeight, NSHeight(slot));
    _groupDivider.hidden = !shown;
    _liveSlot = NSMakeRect(NSMinX(live), NSMinY(live), NSWidth(live),
                           MAX(0, NSHeight(live) - h));
    return live;
}

// The group without the keyboard: its tab on show not lit, as in VS Code.
- (void)colorOtherGroupStrip {
    if (!_parkedStrip) return;
    AppSettings *cfg = [AppSettings shared];
    _parkedStrip.stripColor = [cfg background:Surface::Sidebar];
    _parkedStrip.activeColor = [cfg background:Surface::Editor];
    _parkedStrip.activeText = [[cfg text:Surface::Editor] colorWithAlphaComponent:0.75];
    _parkedStrip.inactiveText = [[cfg text:Surface::Sidebar] colorWithAlphaComponent:0.6];
    _parkedStrip.needsDisplay = YES;
}

- (void)applyGroupSettings {
    if (!_mirror) return;
    AppSettings *cfg = [AppSettings shared];
    [self colorOtherGroupStrip];
    MCLineGutter *g = self.gutter;
    [_mirror applyBackground:[cfg background:Surface::Editor]
            gutterBackground:g.backgroundColor ?: [cfg background:Surface::Editor]
                 numberColor:g.numberColor
                        font:g.font ?: MCEditorFont()
                 lineNumbers:g.mode != MCLineNumbersOff
                   laneWidth:g.laneWidth];
    // A copy keeps the colors and font it was taken with: taken again.
    if (_win->split() && !_mirror.sharedStorage) {
        _mirrorTab = nil;
        [self syncEditorMirror];
    }
}

- (void)reloadParkedStrip {
    if (!_win->split() || !_parkedStrip) return;
    const int other = _win->other();
    _parkedStrip.items = [self stripItemsForGroup:other];
    _parkedStrip.activeIndex = _win->indexOf(_win->active(other), other);
}

// A press on a strip: the other group's takes the keyboard there first.
- (void)focusGroupOfStrip:(MCTabStrip *)strip {
    if (_win->split() && strip && strip == _parkedStrip) [self focusEditorGroup:1 - _win->focused()];
}

// ------------------------------------------------------- what it shows
- (BOOL)editorShowsEditableText {
    return self.textView.editable && _activeTab && !_activeTab.showingMessage;
}

- (BOOL)editorWraps { return self.textView.textContainer.widthTracksTextView; }

// Whether a tab not on show in the editor can be shown as text from its
// own copy (sourceText): read, and shown as source, not in a viewer.
- (BOOL)tabShowsAsText:(MCTab *)t {
    if (!t.loaded || !t.sourceText || t.showingMessage || t.isImage || t.isPDF ||
        t.isMedia || t.isDiff || MCViewerKindWithoutText(t.viewerKind))
        return NO;
    if (t.previewMode && (t.isMarkdown || t.isLatex || MCViewerKindOverText(t.viewerKind) ||
                          MCViewerKindRendersMarkdown(t.viewerKind)))
        return NO;
    return YES;
}

// A tab's text in the editor's font and the syntax colors, for the other
// group (a long text, or one in large-file mode, plain).
- (NSAttributedString *)colorTextOfTab:(MCTab *)t {
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineSpacing = 2.0;   // displaySourceEditable's
    AppSettings *cfg = [AppSettings shared];
    NSString *text = t.sourceText ?: @"";
    NSMutableAttributedString *a = [[NSMutableAttributedString alloc]
        initWithString:text
            attributes:@{NSFontAttributeName: MCEditorFont(),
                         NSForegroundColorAttributeName: [cfg text:Surface::Editor],
                         NSParagraphStyleAttributeName: ps}];
    std::string ext = t.currentExt.UTF8String ?: "";
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    const char *utf8 = text.length <= kMirrorColorLimit && !t.largeFile ? text.UTF8String : nullptr;
    if (!utf8 || !SyntaxHighlighter::supports(ext)) return a;
    const std::string s = utf8;
    size_t byte = 0, unit = 0;
    auto toUnits = [&](size_t target) {
        if (target < byte) byte = unit = 0;
        while (byte < target && byte < s.size()) {
            const unsigned char c = (unsigned char)s[byte];
            if ((c & 0xC0) != 0x80) unit += c >= 0xF0 ? 2 : 1;
            byte++;
        }
        return unit;
    };
    [a beginEditing];
    for (const Token &tok : SyntaxHighlighter::highlight(s, ext)) {
        if (tok.style == TokenStyle::Plain) continue;
        const size_t from = toUnits(tok.start), to = toUnits(tok.start + tok.length);
        if (to <= from || to > a.length) continue;
        [a addAttribute:NSForegroundColorAttributeName value:[cfg syntax:tok.style]
                  range:NSMakeRange(from, to - from)];
    }
    [a endEditing];
    return a;
}

// What the editor shows below its strip, as a picture.
- (NSImage *)pictureOfEditor {
    const NSRect r = NSIntersectionRect(_liveSlot, self.rightArea.bounds);
    if (NSIsEmptyRect(r)) return nil;
    NSBitmapImageRep *rep = [self.rightArea bitmapImageRepForCachingDisplayInRect:r];
    if (!rep) return nil;
    [self.rightArea cacheDisplayInRect:r toBitmapImageRep:rep];
    NSImage *img = [[NSImage alloc] initWithSize:r.size];
    [img addRepresentation:rep];
    return img;
}

// A tab with nothing to copy (never shown yet, or a viewer's): its name.
- (NSImage *)placeholderPictureOfTab:(MCTab *)t {
    NSString *name = t.currentPath.lastPathComponent ?: t.diffName ?: @"";
    NSString *line = [NSString stringWithFormat:@"%@: click to show", name];
    NSDictionary *a = @{NSFontAttributeName: [NSFont systemFontOfSize:13],
                        NSForegroundColorAttributeName: Hex(0x9D9D9D)};
    const NSSize s = [line sizeWithAttributes:a];
    return [NSImage imageWithSize:NSMakeSize(ceil(s.width) + 32, ceil(s.height) + 32)
                          flipped:YES
                   drawingHandler:^BOOL(NSRect) {
                       [line drawAtPoint:NSMakePoint(16, 16) withAttributes:a];
                       return YES;
                   }];
}

// The other group shows its tab: the editor's storage when it is the same
// tab shown as text there, else what it had while still current, else a
// copy made now. Cheap when nothing changed; run after every change of
// what the editor shows.
- (void)syncEditorMirror {
    if (!_win->split() || !_mirror) return;
    MCTab *t = [self parkedActive];
    if (!t) {
        [_mirror clear];
        _mirrorTab = nil;
        return;
    }
    NSTextStorage *storage = self.textView.textStorage;
    if (t == _activeTab && [self editorShowsEditableText]) {
        if (_mirror.sharedStorage != storage) {
            CGFloat offset = 0;
            const BOOL same = _mirrorTab == t && !_mirror.showsPicture;
            const NSUInteger top = same ? [_mirror topCharacter:&offset] : t.topCharacter;
            [_mirror shareStorage:storage wraps:[self editorWraps]];
            [_mirror scrollToCharacter:top offset:same ? offset : t.topOffset];
        }
        _mirrorTab = t;
        return;
    }
    if (_mirror.sharedStorage) {
        // Shared, but the editor no longer shows it as text: from its own copy.
        _mirrorTab = nil;
    }
    if (_mirrorTab == t && (_mirror.showsPicture || _mirrorText == t.sourceText)) return;
    // The same text in a new string (stashActiveTab copies it): still current.
    if (_mirrorTab == t && _mirrorText && t.sourceText &&
        _mirrorText.length == t.sourceText.length && [_mirrorText isEqualToString:t.sourceText]) {
        _mirrorText = t.sourceText;
        return;
    }
    if ([self tabShowsAsText:t]) {
        [_mirror showText:[self colorTextOfTab:t] wraps:[self editorWraps]];
        [_mirror scrollToCharacter:t.topCharacter offset:t.topOffset];
    } else if (t == _activeTab) {
        [_mirror showPicture:[self pictureOfEditor]];
    } else {
        [_mirror showPicture:[self placeholderPictureOfTab:t]];
    }
    _mirrorTab = t;
    _mirrorText = t.sourceText;
}

- (void)syncEditorMirrorSoon {
    if (_mirrorSyncQueued) return;
    _mirrorSyncQueued = YES;
    __weak EditorController *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        EditorController *s = weakSelf;
        if (!s) return;
        s->_mirrorSyncQueued = NO;
        [s syncEditorMirror];
    });
}

// The editor's storage is about to take a whole new text (MCSourceStorage
// willReplaceAll): the other group keeps the text as it is, in a copy.
- (void)editorTextWillBeReplaced {
    if (!_mirror.sharedStorage) return;
    [_mirror keepCopyOfSharedText];
    _mirrorText = [self parkedActive].sourceText;
    [self syncEditorMirrorSoon];   // shared again if it is still the same tab's text
}

// The editor is about to show another tab (activateTab:): the other group
// stops sharing a text that is about to go.
- (void)editorWillShowTab:(MCTab *)tab {
    if (_win->split() && _mirror.sharedStorage && tab != _activeTab) [self editorTextWillBeReplaced];
}

// The character at the top of the editor's pane and its line's distance
// below the pane's top, as stashActiveTab keeps a tab's place.
- (NSUInteger)editorTopCharacter:(CGFloat *)offset {
    *offset = 0;
    NSTextView *tv = self.textView;
    const NSUInteger n = tv.string.length;
    if (n == 0) return 0;
    const NSRect visible = tv.visibleRect;
    const NSUInteger top = MIN([tv characterIndexForInsertionAtPoint:
                                   NSMakePoint(NSMinX(visible) + 20, NSMinY(visible) + 1)],
                               n - 1);
    const NSRect r = [self textRectForCharacter:top];
    if (!NSIsEmptyRect(r)) *offset = NSMinY(r) - NSMinY(visible);
    return top;
}

// ------------------------------------------------------------ the focus
// The editor moves to group `g`: the group it leaves shows what it
// showed, and it shows that group's tab at the place the group showed it.
- (void)focusEditorGroup:(NSInteger)g {
    if (!_win->split() || g == _win->focused() || (g != 0 && g != 1)) return;
    [self makeEditorGroupViews];
    MCTab *leaving = _activeTab, *arriving = [self parkedActive];
    CGFloat arriveOffset = 0;
    const BOOL arriveHasPlace = arriving && _mirrorTab == arriving && !_mirror.showsPicture &&
                                _mirror.textView.textStorage.length > 0;
    const NSUInteger arriveTop = arriveHasPlace ? [_mirror topCharacter:&arriveOffset] : 0;
    const BOOL arriveHasSelection = _parkedSelectionTab == arriving && arriving;
    const NSRange arriveSelection = _parkedSelection;

    // What the editor shows now, for the group it leaves.
    [self flushHighlighting];
    const BOOL text = leaving && [self editorShowsEditableText];
    NSAttributedString *copy = nil;
    NSImage *picture = nil;
    // (The same tab shown as text in both is the editor's storage, shared.)
    if (leaving && (leaving != arriving || !text)) {
        if (text) copy = [[NSAttributedString alloc]
                             initWithAttributedString:self.textView.textStorage];
        else picture = [self pictureOfEditor];
    }
    CGFloat leaveOffset = 0;
    const NSUInteger leaveTop = text ? [self editorTopCharacter:&leaveOffset] : 0;
    _parkedSelectionTab = leaving;
    _parkedSelection = self.textView.selectedRange;

    // The groups change places: the group left keeps `leaving` on show.
    _win->focus((int)g);
    [self swapGroupStrips];
    [self applyTabSettings];   // the strips' colors change places too
    if (copy) {
        [_mirror showText:copy wraps:[self editorWraps]];
        _mirrorTab = leaving;
    } else if (picture) {
        [_mirror showPicture:picture];
        _mirrorTab = leaving;
    }

    if (arriving && arriving != leaving) {
        if (arriveHasPlace) {
            arriving.haveView = YES;
            arriving.topCharacter = arriveTop;
            arriving.topOffset = arriveOffset;
        }
        if (arriveHasSelection) arriving.selection = arriveSelection;
        [self activateTab:arriving];   // laid out in its new place before its text goes in
        _mirrorText = leaving.sourceText;
    } else if (arriving) {
        // One document in both: the editor takes up the place this group had.
        [self relayoutRightArea];
        if (arriveHasSelection && NSMaxRange(arriveSelection) <= self.textView.string.length)
            self.textView.selectedRange = arriveSelection;
        if (arriveHasPlace) [self scrollEditorToCharacter:arriveTop offset:arriveOffset];
        [self reloadTabStrip];
        [self updateTitle];
    }
    [self relayoutRightArea];
    [self syncEditorMirror];
    if (text && !_mirror.showsPicture) [_mirror scrollToCharacter:leaveTop offset:leaveOffset];
    [self reloadTabStrip];
    [self saveSession];
}

// A press in the other group: the editor goes there, and a left click
// puts the caret on the character it was on (two clicks select its word,
// three its line). The press itself is not handed on: the rest of it
// (drags, the release) comes to the view it began in, so a drag there
// would select from the character to wherever the other group's view
// maps the pointer.
- (void)otherGroupPressed:(NSEvent *)e character:(NSUInteger)c {
    if (!_win->split()) return;
    [self focusEditorGroup:1 - _win->focused()];
    const BOOL text = [self editorShowsEditableText] && !self.editorScroll.hidden;
    if (!text || c == NSNotFound || e.type != NSEventTypeLeftMouseDown) {
        [self focusActiveDocument];
        return;
    }
    NSTextView *tv = self.textView;
    [self.window makeFirstResponder:tv];
    const NSUInteger n = tv.string.length;
    const NSUInteger at = MIN(c, n);
    NSRange sel = NSMakeRange(at, 0);
    if (e.clickCount == 2 && n > 0)
        sel = [tv.textStorage doubleClickAtIndex:MIN(at, n - 1)];
    else if (e.clickCount >= 3)
        sel = [tv.string lineRangeForRange:NSMakeRange(at, 0)];
    else if (e.modifierFlags & NSEventModifierFlagShift)
        sel = NSUnionRange(tv.selectedRange, NSMakeRange(at, 0));
    tv.selectedRange = sel;
}

// ------------------------------------------------------------ the commands
// Split Editor (Cmd+\ in the editor, VS Code's): the tab on show in a new
// group to the right, which takes the keyboard. Already split, the tab is
// shown in the other group as well.
- (void)splitEditor:(id)sender {
    MCTab *t = _activeTab;
    if (!t) {
        NSBeep();
        return;
    }
    [self revealEditor];
    if (_win->split()) {
        _win->putInOther(t.docId);   // beside the group's tab on show, unless it has it
        _mirrorTab = nil;
        [self syncEditorMirror];
        [self focusEditorGroup:1 - _win->focused()];
        [self focusActiveDocument];
        return;
    }
    [self makeEditorGroupViews];
    // The new group starts where this one is.
    if ([self editorShowsEditableText]) {
        CGFloat offset = 0;
        t.topCharacter = [self editorTopCharacter:&offset];
        t.topOffset = offset;
        t.haveView = YES;
    }
    _win->putInOther(t.docId);   // a group on the right with the tab; the focus stays
    _mirrorTab = nil;
    _parkedSelectionTab = nil;
    [self relayoutRightArea];
    [self syncEditorMirror];
    [self focusEditorGroup:1];
    [self focusActiveDocument];
}

// The other group has closed (Model::Window::close dropped it): the views
// are one group's again.
- (void)editorGroupsJoined {
    [self hideEditorGroupViews];
    [self relayoutRightArea];
    [self reloadTabStrip];
    [self saveSession];
}

// The same, remembering nothing: every tab is about to go (dropAllTabs),
// and the folder's session stays as it was.
- (void)joinEditorGroupsQuietly {
    if (_win->split()) [self hideEditorGroupViews];
}

- (void)hideEditorGroupViews {
    _mirrorTab = nil;
    _mirrorText = nil;
    [_mirror clear];   // off the editor's storage
    [_mirror layoutInRect:NSZeroRect hidden:YES];
    _parkedStrip.hidden = YES;
    _groupDivider.hidden = YES;
}

// The strip under the window's title is always the focused group's.
- (void)swapGroupStrips {
    MCTabStrip *strip = self.tabStrip;
    self.tabStrip = _parkedStrip;
    _parkedStrip = strip;
}

// The focused group lost its last tab, and the window closed it
// (Model::Window::close): the other group, now the only one, takes the
// editor, at the place it showed its tab, and the keyboard. The tab that
// closed has left the editor already.
- (void)editorGroupClosed {
    MCTab *arriving = [self tabWithId:_win->active()];
    CGFloat arriveOffset = 0;
    const BOOL arriveHasPlace = arriving && _mirrorTab == arriving && !_mirror.showsPicture &&
                                _mirror.textView.textStorage.length > 0;
    const NSUInteger arriveTop = arriveHasPlace ? [_mirror topCharacter:&arriveOffset] : 0;
    const BOOL arriveHasSelection = _parkedSelectionTab == arriving && arriving;
    const NSRange arriveSelection = _parkedSelection;
    [self flushHighlighting];
    _parkedSelectionTab = nil;
    _parkedSelection = self.textView.selectedRange;
    [self swapGroupStrips];
    [self applyTabSettings];   // the strips' colors change places too
    if (arriving) {
        if (arriveHasPlace) {
            arriving.haveView = YES;
            arriving.topCharacter = arriveTop;
            arriving.topOffset = arriveOffset;
        }
        if (arriveHasSelection) arriving.selection = arriveSelection;
        [self activateTab:arriving];
    }
    [self editorGroupsJoined];
    [self focusActiveDocument];
}

// Cmd+1: the first (left) group, or the one editor.
- (void)focusFirstEditorGroup:(id)sender {
    [self focusEditorGroup:0];
    [self focusEditor:sender];
}

// Cmd+2: the second group, made from the tab on show when there is none,
// as VS Code does.
- (void)focusSecondEditorGroup:(id)sender {
    if (!_win->split()) {
        [self splitEditor:sender];
        return;
    }
    [self focusEditorGroup:1];
    [self focusEditor:sender];
}

// Ctrl+Cmd+Right and Left (VS Code's): the tab on show moves to the other
// group (to a new one on the right when there is one), shown there with the
// keyboard; a group left without tabs closes.
- (void)moveEditorToNextGroup:(id)sender { [self moveEditorToGroupInDirection:1]; }
- (void)moveEditorToPreviousGroup:(id)sender { [self moveEditorToGroupInDirection:-1]; }

- (void)moveEditorToGroupInDirection:(int)direction {
    MCTab *t = _activeTab;
    const int target = EditorTabs::moveTarget((int)[self focusedGroup], _win->split(), direction);
    if (!t || target < 0) {
        NSBeep();
        return;
    }
    if (!_win->split()) {
        if (_win->count() < 2) {   // the only tab: nothing to leave behind
            NSBeep();
            return;
        }
        [self splitEditor:nil];   // the tab in both, the new group focused
        [self removeTabFromOtherGroup:t];
        return;
    }
    [self splitEditor:nil];   // shown in the other group too, which takes the keyboard
    [self removeTabFromOtherGroup:t];
    [self focusActiveDocument];
}

- (BOOL)validateGroupItem:(NSMenuItem *)item enabled:(BOOL *)enabled {
    const SEL a = item.action;
    if (a == @selector(splitEditor:)) *enabled = _activeTab != nil;
    else if (a == @selector(focusSecondEditorGroup:)) *enabled = _win->split() || _activeTab != nil;
    else if (a == @selector(moveEditorToNextGroup:) || a == @selector(moveEditorToPreviousGroup:)) {
        const int d = a == @selector(moveEditorToNextGroup:) ? 1 : -1;
        const bool split = _win->split();
        *enabled = _activeTab && EditorTabs::moveTarget((int)[self focusedGroup], split, d) >= 0 &&
                   (split || _win->count() > 1);
    } else return NO;
    return YES;
}

// ------------------------------------------------------------ sessions
// The split, for the folder's session (EditorController+Persistence.mm):
// each group's files, their files on show, which group had the keyboard
// and the left group's share. nil when not split.
- (NSDictionary *)editorGroupsSessionState {
    if (!_win->split()) return nil;
    const int focused = _win->focused();
    NSArray<MCTab *> *left = [self tabsWithIds:_win->tabs(0)];
    NSArray<MCTab *> *right = [self tabsWithIds:_win->tabs(1)];
    MCTab *leftActive = focused == 0 ? _activeTab : [self parkedActive];
    MCTab *rightActive = focused == 0 ? [self parkedActive] : _activeTab;
    NSArray *(^paths)(NSArray<MCTab *> *) = ^NSArray *(NSArray<MCTab *> *tabs) {
        NSMutableArray *out = [NSMutableArray array];
        for (MCTab *t in tabs)
            if (t.currentPath && !t.agentPreview) [out addObject:t.currentPath];
        return out;
    };
    return @{@"groups": @[paths(left), paths(right)],
             @"active": @[leftActive.currentPath ?: @"", rightActive.currentPath ?: @""],
             @"focused": @(focused), @"fraction": @(_win->fraction())};
}

// After the session's tabs are back (restoreSession), the split they were
// in: the other group's tab is shown first, so the group has its text and
// colors, then the focused group's.
- (void)restoreEditorGroups:(NSDictionary *)state {
    if (_win->split() || ![state isKindOfClass:NSDictionary.class]) return;
    NSArray *groups = state[@"groups"], *active = state[@"active"];
    if (![groups isKindOfClass:NSArray.class] || groups.count != 2) return;
    EditorTabs::GroupsSession s;
    for (int g = 0; g < 2; g++) {
        if (![groups[g] isKindOfClass:NSArray.class]) return;
        for (id f in groups[g])
            if ([f isKindOfClass:NSString.class]) s.groups[g].push_back(Utf8String(f));
        if ([active isKindOfClass:NSArray.class] && active.count == 2 &&
            [active[g] isKindOfClass:NSString.class])
            s.active[g] = Utf8String(active[g]);
    }
    s.focused = [state[@"focused"] intValue];
    s.fraction = [state[@"fraction"] doubleValue];
    std::vector<std::string> open;
    for (MCTab *t in [self tabs]) open.push_back(t.currentPath ? Utf8String(t.currentPath) : "");
    const EditorTabs::GroupLayout l = EditorTabs::restoreGroups(s, open);
    if (!l.split) return;
    [self makeEditorGroupViews];
    if (!_win->restoreGroups(l)) return;
    const int first = 1 - _win->focused();
    _win->focus(first);
    _mirrorTab = nil;
    // The editor shows a tab of neither list yet, or the wrong one.
    if (!_activeTab || !_win->inGroup(_activeTab.docId, first)) {
        [self stashActiveTab];
        _activeTab = nil;
    }
    [self relayoutRightArea];
    [self activateTab:[self tabWithId:_win->active(first)]];
    [self focusEditorGroup:1 - first];
}
@end
