// EditorTab.mm — see EditorTab.h.
#import "EditorTab.h"
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

@implementation MCTab {
    NSString *_sourceText;
    MCSourceStorage *_live;   // set while the text is the storage's, not yet copied
}
@synthesize textGeneration = _textGeneration;

- (instancetype)init {
    if ((self = [super init])) {
        _undoManager = [NSUndoManager new];
        _mediaSeconds = -1;
        _pdfAutoScales = YES;
        _fileSize = -1;
        _textGeneration = 1;
    }
    return self;
}

- (NSString *)sourceText {
    if (_live) {
        _sourceText = [_live.string copy];
        _live = nil;
    }
    return _sourceText;
}

- (void)setSourceText:(NSString *)text {
    NSString *copy = [text copy];
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
