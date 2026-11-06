// EditorTab.mm — see EditorTab.h.
#import "EditorTab.h"

@implementation MCTab
- (instancetype)init {
    if ((self = [super init])) {
        _undoManager = [NSUndoManager new];
        _mediaSeconds = -1;
        _pdfAutoScales = YES;
    }
    return self;
}
@end
