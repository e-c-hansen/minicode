// Lsp+SyncIvars.h — the instance variables only Lsp+Sync.mm uses, which
// imports it, as does Lsp.mm, which lays them out. See the rule at the top
// of Lsp+Private.h.
#pragma once
#import "Lsp+Private.h"
#import "LspWatcher.h"

@interface LspSession () {
    NSString *_languageId;    // the file on show's, as didOpen names it
    // The edits to the text on show since it was last sent, for servers
    // that take ranged changes (incremental didChange).
    Lsp::ChangeTracker _changes;

    // Files changed on disk, for the servers that registered watchers
    // (workspace/didChangeWatchedFiles): made with the first registration,
    // remade when the folders wanted change, gone with the last.
    MCLspWatcher *_watcher;
    NSUInteger _watchedBatches;
    double _watchedMainCpuMs;        // the last batch's main-thread CPU time
    NSUInteger _watchedMatchedHere;  // servers matched on main, not the queue
    BOOL _stoppingAll;   // stopAllServers is going: the watcher just goes
}
@end
