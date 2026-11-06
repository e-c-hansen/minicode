// LspWatcher.h — files changed on disk, for the language servers'
// workspace/didChangeWatchedFiles (one per LspSession, made once a server
// registers a watcher). Its own FSEvents stream with file events (the
// tree's names only folders, and is the tree's) on the session's folder and
// the folders of the servers' RelativePatterns outside it (basedpyright's
// site-packages), 0.3 s of coalescing. Each event's path is looked at with
// lstat on the stream's queue, so a rename reads as a deletion and a
// creation (and a path already reported as there as a change, whatever
// FSEvents' flags still say: Lsp::FileEventClassifier); events that only
// touch metadata, .git and the like are left out (Lsp::watchExcluded); a batch
// is coalesced (Lsp::coalesceFileChanges) and handed to the main thread.
// Paths come as the kernel spells them, resolved (/private/tmp), which is
// how the session spells the folder to its servers.
#pragma once
#import <Foundation/Foundation.h>
#include "LspClient.h"
#include <vector>

typedef void (^MCLspWatcherHandler)(const std::vector<Lsp::FileChange> &changes);

@interface MCLspWatcher : NSObject
// Watches `paths` (folders, resolved; ones that do not exist are left
// out) until -stop; the handler runs on the main thread. Nil when none of
// the folders exists.
- (instancetype)initWithPaths:(NSArray<NSString *> *)paths handler:(MCLspWatcherHandler)handler;
@property(nonatomic, readonly) NSArray<NSString *> *paths;
// Batches handed over so far (for checks).
@property(nonatomic, readonly) NSUInteger batches;
- (void)stop;
@end
