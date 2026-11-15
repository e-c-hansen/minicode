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
// MCSessionFilePage, what one session changed in one file (A1b), opened by
// a click on a file under a session in the Agents panel and from the edits
// the Evidence view lists: each Edit, MultiEdit part, Write and notebook
// edit in order, as a small diff (old lines red, new green, changed words
// marked) with its time, refused ones and ones no longer in the file said
// so (AgentEdits.h decides). When the edits undo cleanly from the file as
// it is, the top says how the file differs from before the session and
// Compare Before and Now shows the two side by side (CompareView.h). Read
// on a queue, again whenever the transcript grows.
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
// A file the session changed, listed among the commands: its page.
@property(nonatomic, copy) void (^onShowFileChanges)(NSString *path);
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
@property(nonatomic, readonly) NSUInteger editCount;     // the file edits listed
- (NSString *)editText:(NSUInteger)index;                 // one edit's line, as shown
- (void)openEdit:(NSUInteger)index;                       // its file's page
@end

@interface MCSessionFilePage : NSObject <MCEditorPage>
- (instancetype)initWithTranscript:(NSString *)transcript
                       sessionName:(NSString *)name
                              file:(NSString *)path
                             model:(MCAgentModel *)model;
@property(nonatomic, readonly, copy) NSString *transcript, *path;
@property(nonatomic, copy) void (^onShowComparison)(MCComparison *comparison);
@property(nonatomic, copy) void (^onOpenFile)(NSString *path);
- (void)update;                      // read the transcript and the file again

// For the GUI checks.
@property(nonatomic, readonly) BOOL loaded;
@property(nonatomic, readonly) NSUInteger changeCount;
@property(nonatomic, readonly) BOOL rebuilt;              // the file before the session is known
@property(nonatomic, readonly, copy) NSString *summaryText;   // the lines at the top, as text
@property(nonatomic, readonly) NSTextView *textView;
@property(nonatomic, copy) void (^onUpdate)(void);
- (NSString *)changeText:(NSUInteger)index;               // one change's block, as shown
- (MCComparison *)beforeAndNow;      // nil unless rebuilt
- (void)compareBeforeAndNow;         // the link's action
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
