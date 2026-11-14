// AgentModel.h — Objective-C++. One window's view of the agents that worked
// in its folder: the sessions Claude Code's transcripts record (through the
// core's AgentIndex), the worktrees under the folder (through git), and
// where their scratchpads are. The Agents panel, the Worktrees node in the
// file tree, terminal links and the Evidence view all read it.
//
// Read-only toward everything it reads: the transcripts, the worktrees and
// the scratchpads are never written, moved or deleted, and the only git
// commands run are ones that read (worktree list, rev-list, status,
// merge-base, diff, ls-files, show), through MCGitRunSync with its time
// limits.
//
// Threads: the index lives on one serial queue (it is not thread-safe, and
// its rows point into it); git runs on another. What the main thread sees
// is copied into the immutable objects below, published on the main queue
// with a notification. FSEvents on the transcripts folder and the
// scratchpads' folder trigger a refresh (coalesced, about 0.5 s after a
// write, so the panel follows within the 2 s the spec allows).
//
// Where Claude Code's files are: $MINICODE_CLAUDE_PROJECTS, else
// ~/.claude/projects; $MINICODE_CLAUDE_TMP, else /private/tmp/claude-<uid>
// (the GUI checks point both at scratch copies of tests/fixtures/claude).
#import <Cocoa/Cocoa.h>

@class MCComparison;

// A file a session wrote or edited.
@interface MCAgentFile : NSObject
@property(nonatomic, readonly, copy) NSString *path;      // absolute, as the tool call gave it
@property(nonatomic, readonly) NSInteger place;           // AgentIndex::Place
@property(nonatomic, readonly) NSInteger writes, edits, failed;
@property(nonatomic, readonly) BOOL exists;               // still on disk when last looked
@property(nonatomic, readonly, copy) NSString *transcript;   // the session's key
// Its folder as the row shows it: from the window's folder ("src"), from
// its worktree, "scratchpad/..." in the session's scratchpad, else with ~.
@property(nonatomic, readonly, copy) NSString *folderText;
@end

// One of the three groups of a session's files (R2.6).
@interface MCAgentFileGroup : NSObject
@property(nonatomic, readonly) NSInteger place;
@property(nonatomic, readonly, copy) NSString *title;     // "In the folder", ...
@property(nonatomic, readonly, copy) NSArray<MCAgentFile *> *files;
@property(nonatomic, readonly, copy) NSString *transcript;
@end

// A session or a subagent, as the panel shows it.
@interface MCAgentWork : NSObject
@property(nonatomic, readonly, copy) NSString *transcript;   // the key
@property(nonatomic, readonly, copy) NSString *sessionId, *agentId, *parent;
@property(nonatomic, readonly, copy) NSString *name;         // meta description, else first prompt
@property(nonatomic, readonly, copy) NSString *title, *agentType;
@property(nonatomic, readonly, copy) NSString *cwd, *gitBranch;
@property(nonatomic, readonly, copy) NSString *worktreePath, *worktreeBranch;
@property(nonatomic, readonly, copy) NSString *scratchpad;
@property(nonatomic, readonly, copy) NSArray<NSString *> *versions;
@property(nonatomic, readonly) NSTimeInterval started, lastActivity, written;   // Unix seconds
@property(nonatomic, readonly) BOOL finished, hasMeta;
@property(nonatomic, readonly) NSInteger badLines, filesTouched, bashCommands, depth;
@property(nonatomic, readonly, copy) NSArray<MCAgentFileGroup *> *groups;
@property(nonatomic, readonly, copy) NSArray<MCAgentWork *> *subagents;
// Written in the last two minutes and not marked finished (R2.5).
- (BOOL)runningAt:(NSDate *)now;
// Where it worked, for the row: its worktree's branch, else its own branch.
@property(nonatomic, readonly, copy) NSString *whereText;
@end

// A worktree under the folder (R2.8).
@interface MCAgentWorktree : NSObject
@property(nonatomic, readonly, copy) NSString *path;      // spelled from the window's folder
@property(nonatomic, readonly, copy) NSString *branch, *head, *agentId;
@property(nonatomic, readonly, copy) NSString *label;     // the agent's description, else the folder name
@property(nonatomic, readonly, copy) NSString *transcript;   // the subagent's, or nil
@property(nonatomic, readonly) BOOL running;
@property(nonatomic, readonly) NSInteger ahead;           // commits ahead of the main branch; -1 unknown
@property(nonatomic, readonly) BOOL dirty;                // uncommitted changes
@property(nonatomic, readonly, copy) NSString *badge;     // "3 ahead · modified"
@end

// A file a worktree changed against its main branch.
@interface MCWorktreeChange : NSObject
@property(nonatomic, readonly) unichar status;            // M A D R C T U, '?' untracked
@property(nonatomic, readonly, copy) NSString *path, *from;   // relative to the worktree
@end

extern NSNotificationName const MCAgentModelDidChangeNotification;            // sessions
extern NSNotificationName const MCAgentModelWorktreesDidChangeNotification;   // worktrees

@interface MCAgentModel : NSObject
- (instancetype)initWithFolder:(NSString *)folder;
// The window's folder; a new one starts the index afresh.
@property(nonatomic, copy) NSString *folder;
@property(nonatomic, readonly, copy) NSString *projectsDir, *tmpRoot;
@property(nonatomic, readonly) BOOL loaded;            // the first refresh has finished
@property(nonatomic, readonly) BOOL projectsMissing;   // the transcripts folder is not there
// Top-level sessions newest first, their subagents nested (R2.5, R2.7).
@property(nonatomic, readonly, copy) NSArray<MCAgentWork *> *sessions;
@property(nonatomic, readonly) NSUInteger sessionCount;   // with the subagents
@property(nonatomic, readonly, copy) NSArray<NSString *> *scratchpads;
@property(nonatomic, readonly, copy) NSArray<MCAgentWorktree *> *worktrees;
@property(nonatomic, readonly, copy) NSString *mainBranch;   // "main", or nil outside a repository
@property(nonatomic, readonly) BOOL worktreesLoaded;          // git has answered once
@property(nonatomic, readonly) NSUInteger generation;         // grows with every publish
- (MCAgentWork *)sessionForTranscript:(NSString *)transcript;
- (MCAgentWorktree *)worktreeAtPath:(NSString *)path;   // any spelling of its folder
// Where a relative path in terminal output is looked for after the shell's
// folder and the window's (R2.10): the worktrees under the folder, then the
// indexed sessions' scratchpads.
- (NSArray<NSString *> *)worktreeRoots;
// Reread what changed (coalesced; the FSEvents callback calls it too).
- (void)refresh;
// Ask git again for the worktrees and their ahead and modified state
// (coalesced, about a second).
- (void)refreshWorktrees;
// A worktree's changed files against the main branch: committed since it
// left it, staged, unstaged and untracked (R2.9). On the main thread.
- (void)changesInWorktree:(MCAgentWorktree *)worktree
               completion:(void (^)(NSArray<MCWorktreeChange *> *changes, NSString *error))done;
// The main branch's version of a changed file beside the worktree's.
- (void)comparisonForChange:(MCWorktreeChange *)change
                 inWorktree:(MCAgentWorktree *)worktree
                 completion:(void (^)(MCComparison *comparison))done;
- (void)stop;   // no more events or refreshes (the window closed)

+ (NSString *)defaultProjectsDir;
+ (NSString *)defaultTmpRoot;
@end
