// EditorTab.mm — see EditorTab.h.
#import "EditorTab.h"
#import "SessionLink.h"
#import "SourceStorage.h"
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
    FolderSearch::TextEncoding encoding = FolderSearch::TextEncoding::Utf8;
    bool bom = false;
    if (!FolderSearch::decodeFileText(&bytes, &encoding, &bom)) return d;
    d.bom = bom;
    d.encoding = (MCTextEncoding)encoding;
    d.text = [[NSString alloc] initWithBytes:bytes.data() length:bytes.size()
                                    encoding:NSUTF8StringEncoding];
    return d;
}

static_assert((int)MCTextEncodingUtf8 == (int)FolderSearch::TextEncoding::Utf8 &&
                  (int)MCTextEncodingUtf16LE == (int)FolderSearch::TextEncoding::Utf16LE &&
                  (int)MCTextEncodingUtf16BE == (int)FolderSearch::TextEncoding::Utf16BE,
              "MCTextEncoding mirrors FolderSearch::TextEncoding");

BOOL MCEncodeFileText(NSString *text, MCTextEncoding encoding, BOOL bom, std::string *bytes,
                      std::string *why) {
    NSData *utf8 = [text ?: @"" dataUsingEncoding:NSUTF8StringEncoding];
    if (!utf8) {
        if (why) *why = "the text holds a character UTF-8 cannot store";
        return NO;
    }
    *bytes = FolderSearch::encodeFileText(std::string((const char *)utf8.bytes, utf8.length),
                                          (FolderSearch::TextEncoding)encoding, bom);
    return YES;
}

// A tab's undo manager. While the session's host owns the tab's history
// (MCTab session, editor.session-host) it is the host's front: undo, redo
// and what they would do are the host's, a name given to a step (Toggle
// Comment, Change Color, ...) names the host's step, and registrations go
// nowhere, since every change of the text is a step on the host already
// (the storage's edits, a text set whole). Everything that asks the tab's
// undo manager (the menu, a viewer's table, the GUI checks) then undoes on
// the host. Without a session it is NSUndoManager, untouched.
@interface MCTabUndoManager : NSUndoManager
@property(nonatomic, weak) MCTab *tab;
@end

@implementation MCTabUndoManager
- (MCSessionDoc *)host { return self.tab.session; }

- (void)setActionName:(NSString *)name {
    [super setActionName:name];
    [self.host nameStep:name];
}

- (void)registerUndoWithTarget:(id)target selector:(SEL)selector object:(id)object {
    if (!self.host) [super registerUndoWithTarget:target selector:selector object:object];
}

- (void)registerUndoWithTarget:(id)target handler:(void (^)(id))handler {
    if (!self.host) [super registerUndoWithTarget:target handler:handler];
}

// A top-level group is a step of its own on the host, as it is an undo
// action of its own here: the turn's step closes as it opens and again as
// it ends, and its typing joins nothing before it (the workspace edit
// applier's group per file, the GUI checks' group per simulated key).
- (void)beginUndoGrouping {
    MCSessionDoc *h = self.host;
    if (h && self.groupingLevel == 0) {
        [h closeStep];
        [h breakCoalescing];
    }
    [super beginUndoGrouping];
}

- (void)endUndoGrouping {
    [super endUndoGrouping];
    MCSessionDoc *h = self.host;
    if (h && self.groupingLevel == 0) [h closeStep];
}

- (BOOL)canUndo {
    MCSessionDoc *h = self.host;
    return h ? [h canUndo:NO name:NULL] : [super canUndo];
}

- (BOOL)canRedo {
    MCSessionDoc *h = self.host;
    return h ? [h canUndo:YES name:NULL] : [super canRedo];
}

- (void)undo {
    MCSessionDoc *h = self.host;
    if (h) [h undo:NO inView:nil];
    else [super undo];
}

- (void)redo {
    MCSessionDoc *h = self.host;
    if (h) [h undo:YES inView:nil];
    else [super redo];
}

- (void)undoNestedGroup {
    MCSessionDoc *h = self.host;
    if (h) [h undo:NO inView:nil];
    else [super undoNestedGroup];
}

- (BOOL)isUndoing { return self.host.undoing == 1 || [super isUndoing]; }
- (BOOL)isRedoing { return self.host.undoing == 2 || [super isRedoing]; }

- (NSString *)undoActionName {
    MCSessionDoc *h = self.host;
    if (!h) return [super undoActionName];
    NSString *name = nil;
    return [h canUndo:NO name:&name] ? name ?: @"" : @"";
}

- (NSString *)redoActionName {
    MCSessionDoc *h = self.host;
    if (!h) return [super redoActionName];
    NSString *name = nil;
    return [h canUndo:YES name:&name] ? name ?: @"" : @"";
}

- (NSString *)undoMenuItemTitle {
    return self.host ? [self undoMenuTitleForUndoActionName:self.undoActionName] : [super undoMenuItemTitle];
}

- (NSString *)redoMenuItemTitle {
    return self.host ? [self redoMenuTitleForUndoActionName:self.redoActionName] : [super redoMenuItemTitle];
}
@end

@implementation MCTab {
    NSString *_sourceText;
    MCSourceStorage *_live;   // set while the text is the storage's, not yet copied
}
@synthesize textGeneration = _textGeneration;

- (instancetype)init {
    if ((self = [super init])) {
        static uint64_t lastId = 0;   // made on the main thread only
        _docId = ++lastId;
        MCTabUndoManager *um = [MCTabUndoManager new];
        um.tab = self;
        _undoManager = um;
        _mediaSeconds = -1;
        _pdfAutoScales = YES;
        _fileSize = -1;
        _textGeneration = 1;
    }
    return self;
}

- (void)setCurrentPath:(NSString *)path {
    _currentPath = [path copy];
    [_owner tabPathChanged:self];
}

- (BOOL)preview { return [_owner tabIsPreview:self]; }
- (BOOL)agentPreview { return [_owner tabIsAgentPreview:self]; }
- (uint64_t)lastActive { return _owner ? [_owner tabLastUsed:self] : 0; }

- (NSString *)sourceText {
    if (_live) {
        _sourceText = [_live.string copy];
        _live = nil;
    }
    return _sourceText;
}

- (void)setSourceText:(NSString *)text {
    NSString *copy = [text copy];
    // The session's host follows every change it did not make itself.
    if (_session) {
        if (!_session.applying) [_session textWillChangeFrom:self.currentText to:copy];
    } else if (copy && MCSessionHostEnabled()) {
        _session = [[MCSessionDoc alloc] initWithTab:self text:copy];
    }
    const BOOL same = !_live && copy == _sourceText;
    _live = nil;
    _sourceText = copy;
    if (!same) _textGeneration++;
}

- (void)followStorage:(NSTextStorage *)storage {
    _textGeneration++;
    if (![storage isKindOfClass:[MCSourceStorage class]]) {   // a copy now, as before
        _live = nil;
        _sourceText = [storage.string copy];
        return;
    }
    if (_live == storage) return;
    _live = (MCSourceStorage *)storage;
    _sourceText = nil;   // the old text, let go
    __weak MCTab *weakSelf = self;
    __weak MCSourceStorage *weakStorage = _live;
    _live.willReplaceAllCharacters = ^{
        MCTab *t = weakSelf;
        if (t && t->_live && t->_live == weakStorage) (void)t.sourceText;   // copied now
    };
}

- (BOOL)followsStorage { return _live != nil; }

- (NSString *)currentText { return _live ? _live.string : _sourceText; }

- (BOOL)hasSourceText { return _live || _sourceText; }
@end
