// EditorTab.mm — see EditorTab.h.
#import "EditorTab.h"
#include "FolderSearch.h"

@implementation MCDiskStamp
@end
@implementation MCDiskText
@end

static MCDiskStamp *MCStampFrom(const FolderSearch::FileStamp &s) {
    if (!s.exists) return nil;
    MCDiskStamp *d = [MCDiskStamp new];
    d.modDate = [NSDate dateWithTimeIntervalSince1970:(double)s.mtimeSec + s.mtimeNsec / 1e9];
    d.fileID = s.inode;
    d.size = s.size;
    d.regular = s.regular;
    return d;
}

MCDiskStamp *MCStampOfFile(NSString *path) {
    if (!path.length) return nil;
    return MCStampFrom(FolderSearch::stampOf(path.fileSystemRepresentation));
}

MCDiskStamp *MCDiskStampFromFileStamp(const FolderSearch::FileStamp &s) { return MCStampFrom(s); }

MCDiskText *MCReadDiskText(NSString *path, unsigned long long maxBytes) {
    MCDiskText *d = [MCDiskText new];
    if (!path.length) return d;
    std::string bytes;
    FolderSearch::FileStamp st;
    const FolderSearch::ReadStatus r =
        FolderSearch::readFileStable(path.fileSystemRepresentation, maxBytes, &bytes, &st);
    d.stamp = MCStampFrom(st);
    d.tooLarge = r == FolderSearch::ReadStatus::TooLarge;
    if (r != FolderSearch::ReadStatus::Ok) return d;
    d.bom = FolderSearch::stripUtf8Bom(&bytes);
    d.text = [[NSString alloc] initWithBytes:bytes.data() length:bytes.size()
                                    encoding:NSUTF8StringEncoding];
    return d;
}

@implementation MCTab
- (instancetype)init {
    if ((self = [super init])) {
        _undoManager = [NSUndoManager new];
        _mediaSeconds = -1;
        _pdfAutoScales = YES;
        _fileSize = -1;
    }
    return self;
}
@end
