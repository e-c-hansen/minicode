// BackupStore.h — the Mac's side of Backups.h: where backups live, the
// background queue that writes them, and which processes still hold theirs.
// EditorController decides what to back up and when (see "Backups and hot
// exit" in docs/notes/sessions.md).
#import <Foundation/Foundation.h>
#include "Backups.h"

// When a process started (Unix seconds), or 0 when there is no such process.
// With its pid, what tells a running process from one gone whose pid was
// reused (the backups' owners, the windows record's).
double MCProcessStart(pid_t pid);

@interface MCBackupStore : NSObject
// nil while backups are off: under MUTIBLE_DEMO and MUTIBLE_BENCH, which
// must start the same way every time, as the tab sessions are.
+ (instancetype)shared;
// $MUTIBLE_BACKUP_DIR, else ~/Library/Application Support/Mutible/Backups
// (NSHomeDirectory, so CFFIXED_USER_HOME moves it in tests).
@property(nonatomic, readonly) NSString *directory;
- (NSString *)newID;
// The buffer of one tab, written in the background, after any earlier write
// or removal for that id; a write overtaken by a newer one for the same id
// is skipped. `diskDate` nil: the file was never on disk as far as the tab knew.
// `fileEncoding` is how the file is written (Backups::Entry::fileEncoding).
// `window` is the writing window's key (Backups::Entry::window).
- (void)writeID:(NSString *)bid path:(NSString *)path root:(NSString *)root
         window:(NSString *)window diskDate:(NSDate *)diskDate inode:(uint64_t)inode
   fileEncoding:(NSString *)fileEncoding text:(NSString *)text;
// The same, now, on the calling thread; NO when it could not be written.
- (BOOL)writeNowID:(NSString *)bid path:(NSString *)path root:(NSString *)root
            window:(NSString *)window diskDate:(NSDate *)diskDate inode:(uint64_t)inode
      fileEncoding:(NSString *)fileEncoding text:(NSString *)text;
- (void)removeID:(NSString *)bid;
// Waits for every queued write and removal.
- (void)flush;
// The keys of the windows this launch brings back (windowsToRestore), so
// a window's backups wait for it, not the first window on its folder.
- (void)reserveWindowKeys:(NSArray<NSString *> *)keys;
// What a window opening `root` brings back (see Backups::plan): `window`
// is its key, no longer reserved from then on. Nothing is deleted: the
// conflicts are the window's to show (moveID:).
- (Backups::Restore)planForRoot:(NSString *)root
                   sessionFiles:(const std::vector<std::string> &)files
                         window:(NSString *)window;
// A conflicting backup given a path of its own (Backups::draftPath), as a
// buffer that has no file yet, held by this process for `window`:
// rewritten now, in place, so it is never without a copy on disk.
- (BOOL)moveID:(NSString *)bid toPath:(NSString *)path window:(NSString *)window;
- (BOOL)loadID:(NSString *)bid entry:(Backups::Entry *)entry text:(NSString **)text;
// Folders with backups no running window holds, newest first, but those in
// `open` (resolved paths).
- (NSArray<NSString *> *)foldersToReopenExcept:(NSArray<NSString *> *)open;
// Orphans older than 30 days and debris from crashes mid-write; once per
// process is enough, and later calls do nothing.
- (void)cleanUpOnce;
@end
