// EditorController+LineHistory.mm — EditorController: Show Line History
// (EditorLineHistory.h), whose host it is: who last changed each line, a
// commit or a Claude Code session, in a box from the gutter or beside
// every line (View > Show Line History).
#import "EditorController+Private.h"
#import "EditorController+LineHistoryIvars.h"
#import "AgentWindow.h"
#import "EditorLineHistory.h"
#import "Lsp.h"

@implementation EditorController (LineHistory)

- (MCLineHistory *)ensureLineHistory {
    if (!_lineHistory && self.textView && self.gutter)
        _lineHistory = [[MCLineHistory alloc] initWithHost:self];
    return _lineHistory;
}

- (MCLineHistory *)lineHistory { return _lineHistory; }   // tests, by KVC

- (void)stopLineHistory {
    [_lineHistory stop];
}

// ---------------------------------------------------------------- the host
- (CodeTextView *)lineHistoryTextView { return self.textView; }
- (MCLineGutter *)lineHistoryGutter { return self.gutter; }
- (NSString *)lineHistoryPath {
    if (!self.currentPath || !self.textView.editable || self.previewMode || self.isDiff)
        return nil;
    return self.currentPath;
}
- (MCAgentModel *)lineHistoryAgentModel { return _agents.model; }

- (void)lineHistoryShowCommit:(NSString *)hash title:(NSString *)title {
    [self showCommitReview:hash title:title];
}

- (void)lineHistoryShowSession:(NSString *)transcript name:(NSString *)name file:(NSString *)path {
    [_agents showChangesToFile:path transcript:transcript name:name];
}

// ------------------------------------------------- View > Show Line History
- (void)toggleLineHistory:(id)sender {
    (void)sender;
    [self ensureLineHistory];
    const BOOL on = !MCLineHistoryInline();
    MCSetLineHistoryInline(on);
    [self showStatusNote:on ? @"Line history shows beside every line"
                            : @"Line history shows when you rest the pointer on a line number"];
}

- (BOOL)validateLineHistoryItem:(NSMenuItem *)item enabled:(BOOL *)enabled {
    if (item.action != @selector(toggleLineHistory:)) return NO;
    item.state = MCLineHistoryInline() ? NSControlStateValueOn : NSControlStateValueOff;
    *enabled = YES;
    return YES;
}

@end
