// EditorController+Session.mm — EditorController: the session's host
// (SessionLink.h, editor.session-host). The window's hooks only: the
// editor's storage reports each edit of the tab on show to the tab's
// document on the host, the text view asks which document it shows (for
// Undo and Redo), and the host's edits to a tab not in the storage reach
// its title and backups. The rest is MCSessionDoc's.
#import "EditorController+Private.h"
#import "SessionLink.h"
#import "SourceStorage.h"

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

@end
