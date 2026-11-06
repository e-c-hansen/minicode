// FileViewers.mm — see FileViewers.h.
#import "FileViewers.h"
#import "QuickLook.h"
#include "FileSniff.h"
#include <string>

static std::string Utf8(NSString *s) {
    const char *c = s.fileSystemRepresentation;
    return c ? c : "";
}

MCViewerKind MCViewerKindForFile(NSString *path) {
    if (!path) return MCViewerNone;
    BOOL dir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&dir]) return MCViewerNone;
    if (dir) return MCViewerQuickLook;   // a package (.pages, .rtfd) opened as a file
    NSFileHandle *fh = [NSFileHandle fileHandleForReadingAtPath:path];
    NSData *head = [fh readDataUpToLength:FileSniff::kHeadBytes error:nil];
    [fh closeFile];
    if (!fh) return MCViewerNone;   // unreadable: the text path says why
    std::string bytes((const char *)head.bytes, head.length);
    switch (FileSniff::classify(Utf8(path), bytes)) {
    case FileSniff::Kind::QuickLook: return MCViewerQuickLook;
    default: return MCViewerNone;
    }
}

BOOL MCViewerKindWithoutText(MCViewerKind kind) {
    return kind == MCViewerQuickLook;
}

@implementation MCFileViewers {
    __weak NSView *_container;
    __weak NSView *_sibling;
    MCViewerKind _kind;
    MCQuickLookSlot *_quickLook;
}

- (instancetype)initWithContainer:(NSView *)container below:(NSView *)sibling {
    if ((self = [super init])) {
        _container = container;
        _sibling = sibling;
    }
    return self;
}

- (MCViewerKind)shownKind { return _kind; }
- (MCQuickLookSlot *)quickLook { return _quickLook; }

- (void)addView:(NSView *)v {
    v.hidden = YES;
    if (_sibling && _sibling.superview == _container)
        [_container addSubview:v positioned:NSWindowBelow relativeTo:_sibling];
    else
        [_container addSubview:v];
}

- (BOOL)showWithoutText:(MCViewerKind)kind path:(NSString *)path {
    [self clear];
    if (kind == MCViewerQuickLook) {
        if (!_quickLook) {
            _quickLook = [[MCQuickLookSlot alloc] initWithFrame:NSZeroRect];
            [self addView:_quickLook];
        }
        [_quickLook showPath:path];
        _kind = kind;
        return YES;
    }
    return NO;
}

- (void)clear {
    [_quickLook clear];
    _kind = MCViewerNone;
}

- (NSRect)layoutInRect:(NSRect)rect editorShown:(BOOL)shown coversText:(BOOL *)coversText {
    BOOL covers = NO;
    if (_quickLook) {
        const BOOL on = shown && _kind == MCViewerQuickLook;
        _quickLook.frame = rect;
        _quickLook.hidden = !on;
        covers = covers || on;
    }
    if (coversText) *coversText = covers;
    return rect;
}

- (NSView *)focusView {
    if (_kind == MCViewerQuickLook) return _quickLook.preview;
    return nil;
}
@end
