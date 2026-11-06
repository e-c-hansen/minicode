// AgentPages.h — Objective-C++. Two pages for the editor's slot
// (EditorPage.h), opened from the Agents panel and the file tree:
//
// MCEvidencePage, a session's Evidence view (R3.1-R3.5): its final message
// at the top beside the result of its last test command, then every shell
// command it ran, in order, with the description the agent gave, the
// command, whether its result was an error, a mark and the runner's own
// summary for test commands, and the first and last 20 lines of the output
// with the rest a click away. Nothing is concluded from any of it. Outputs
// are read lazily from the transcript on a background queue (the core reads
// only the first and last 64 KB of an output, however large), so a 1 MB
// output never holds up the window. "Open in Terminal" types a command into
// a new terminal at the session's folder without running it.
//
// MCWorktreeChangesPage, "Compare with Main" for a worktree (R2.9): the
// files it changed against the main branch; Return or a click on one shows
// the main branch's version beside the worktree's (CompareView.h).
#import <Cocoa/Cocoa.h>
#import "EditorPage.h"

@class MCAgentModel, MCAgentWorktree, MCComparison;

@interface MCEvidencePage : NSObject <MCEditorPage>
- (instancetype)initWithTranscript:(NSString *)transcript
                              name:(NSString *)name
                             model:(MCAgentModel *)model;
@property(nonatomic, readonly, copy) NSString *transcript;
// "Open in Terminal": the command and the folder it ran in.
@property(nonatomic, copy) void (^onOpenInTerminal)(NSString *command, NSString *cwd);
// Read the transcript again (the model calls it when it grows).
- (void)update;

// For the GUI checks.
@property(nonatomic, readonly) BOOL loaded;              // the first read is shown
@property(nonatomic, readonly) BOOL outputsLoaded;       // and every output's ends
@property(nonatomic, readonly) NSUInteger commandCount;
@property(nonatomic, readonly, copy) NSString *finalMessageText;
@property(nonatomic, readonly, copy) NSString *lastTestText;   // the box beside it, as text
@property(nonatomic, readonly) NSTextView *commandsView;
@property(nonatomic, copy) void (^onUpdate)(void);       // after each change on screen
- (NSString *)commandText:(NSUInteger)index;             // one command's block, as shown
- (void)expandCommand:(NSUInteger)index;                 // its whole output
- (BOOL)isExpanded:(NSUInteger)index;
- (void)openCommandInTerminal:(NSUInteger)index;
@end

@interface MCWorktreeChangesPage : NSObject <MCEditorPage>
- (instancetype)initWithWorktree:(MCAgentWorktree *)worktree model:(MCAgentModel *)model;
@property(nonatomic, readonly, strong) MCAgentWorktree *worktree;
@property(nonatomic, copy) void (^onShowComparison)(MCComparison *comparison);

// For the GUI checks.
@property(nonatomic, readonly) BOOL loaded;
@property(nonatomic, readonly, copy) NSArray<NSString *> *rowDescriptions;   // "M src/a.cpp"
@property(nonatomic, readonly, copy) NSString *errorText;
@property(nonatomic, readonly) NSTableView *list;
@property(nonatomic, copy) void (^onLoaded)(void);
- (void)openChangeAtRow:(NSInteger)row;
@end
