// BackupStore.h — the Mac's side of Backups.h: where backups live, the
// background queue that writes them, and which processes still hold theirs.
// EditorController decides what to back up and when (see "Backups and hot
// exit" in CLAUDE.md).
#import <Foundation/Foundation.h>
#include "Backups.h"

@interface MCBackupStore : NSObject
// nil while backups are off: under MINICODE_DEMO and MINICODE_BENCH, which
// must start the same way every time, as the tab sessions are.
+ (instancetype)shared;
// $MINICODE_BACKUP_DIR, else ~/Library/Application Support/MiniCode/Backups
// (NSHomeDirectory, so CFFIXED_USER_HOME moves it in tests).
@property(nonatomic, readonly) NSString *directory;
- (NSString *)newID;
// The buffer of one tab, written in the background, after any earlier write
// or removal for that id; a write overtaken by a newer one for the same id
// is skipped. `diskDate` nil: the file was never on disk as far as the tab knew.
- (void)writeID:(NSString *)bid path:(NSString *)path root:(NSString *)root
       diskDate:(NSDate *)diskDate inode:(uint64_t)inode text:(NSString *)text;
// The same, now, on the calling thread; NO when it could not be written.
- (BOOL)writeNowID:(NSString *)bid path:(NSString *)path root:(NSString *)root
          diskDate:(NSDate *)diskDate inode:(uint64_t)inode text:(NSString *)text;
- (void)removeID:(NSString *)bid;
// Waits for every queued write and removal.
- (void)flush;
// What a window opening `root` brings back (see Backups::plan); older
// duplicates are removed on the way.
- (Backups::Restore)planForRoot:(NSString *)root
                   sessionFiles:(const std::vector<std::string> &)files;
- (BOOL)loadID:(NSString *)bid entry:(Backups::Entry *)entry text:(NSString **)text;
// Folders with backups no running window holds, newest first, but those in
// `open` (resolved paths).
- (NSArray<NSString *> *)foldersToReopenExcept:(NSArray<NSString *> *)open;
// Orphans older than 30 days and debris from crashes mid-write; once per
// process is enough, and later calls do nothing.
- (void)cleanUpOnce;
@end
