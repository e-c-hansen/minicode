// RemoteWorkspace.h — Objective-C++. A window over SSH's folder, read
// through the remote host (stage 8, docs/notes/session-host.md, "The
// window's tree, Quick Open and Find in Folder over SSH"), never through
// this Mac's disk:
//   - the tree's listings (MCTreeSource: FsRead "list", waited for, as the
//     Mac's own tree lists a folder as it opens), with the host's marks
//     (what the ignore files ignore, a link out of the folder);
//   - the folders the tree has listed, told to the host (FsRead "watch"),
//     whose listings come back by themselves when they change there
//     (FsChanged, onListings): the remote disk's notices in place of
//     FSEvents;
//   - Quick Open's list of the folder's files (FsRead "files") and Find in
//     Folder (FsRead "search"), both on the host's workers, answered as
//     they go (SessionSearch.h);
//   - the files the window opened that are not there yet (the address's
//     new file, New File's), which their first save makes.
// Its link is the window's, moved by Reconnect (attachLink:). Main thread.
#pragma once
#import <Cocoa/Cocoa.h>

#include <vector>

#include "Json.h"
#import "FileTree.h"

@class MCSessionLinkRef;

@interface MCRemoteWorkspace : NSObject <MCTreeSource>
- (instancetype)initWithFolder:(NSString *)folder;
@property(nonatomic, readonly, copy) NSString *folder;
// The window's link, now or again after Reconnect: its disk notices heard,
// and the folders open in the tree told to the new host.
- (void)attachLink:(MCSessionLinkRef *)link;
// The window closes: nothing more is heard or asked.
- (void)detach;

// Each change the host saw: the listings of the folders it named, by path
// (each MCTreeList-shaped), on the main queue's next turn.
@property(nonatomic, copy) void (^onListings)(NSDictionary<NSString *, NSArray<MCTreeEntry *> *> *listings);
// The folders the tree has listed (absolute, under the folder), with
// dotfiles when `hidden`: the host watches them. Sent when it differs from
// what was sent last (and again on a new link).
- (void)watchFolders:(NSArray<NSString *> *)paths hidden:(BOOL)hidden;
// The window's: a watch is to be sent on the main queue's next turn.
@property(nonatomic, assign) BOOL watchQueued;

// Quick Open's list: the folder's files from it ('/'-separated), as the
// Mac's own walk lists them, dotfiles too. `done` on the main thread once
// (an empty list when the host cannot be asked).
- (void)listFiles:(void (^)(NSArray<NSString *> *files, BOOL truncated))done;
// Find in Folder on the host (a "search" request, SessionSearch.h): each
// batch of answers as it comes, `done` with the last. 0, calling nothing,
// when the host cannot be asked.
- (uint64_t)search:(Json)request answers:(void (^)(const std::vector<Json> &answers, BOOL done))answers;
- (void)cancelSearch:(uint64_t)req;

// Files opened here that are not on the host yet: shown empty, made by
// their first save.
- (void)markNew:(NSString *)path;
- (BOOL)isNew:(NSString *)path;

// For tests: folders listed through the host, its notices taken, Quick
// Open lists and searches asked for.
@property(nonatomic, readonly) NSUInteger listings, notices, fileLists, searches;
@end
