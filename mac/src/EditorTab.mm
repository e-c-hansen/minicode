// EditorTab.mm — see EditorTab.h.
#import "EditorTab.h"
#import "SessionLink.h"
#import "SourceStorage.h"
#include "FolderSearch.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

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

// ---- large files: read into pages of their own
// A file from 4 MB on is read into anonymous pages (mmap), not malloc's: on
// current macOS a freed malloc block that large stays in the process's
// footprint (a 256 MB block freed left all 256 MB counted), and an ASCII
// text is then the NSString itself, its bytes taken without a copy and the
// pages let go with the string. Opening a 259 MB log made the bytes, the
// string, and kept both: 520 MB before anything was shown.
namespace {

const uint64_t kPagedReadBytes = 4u << 20;

// No byte past ASCII: eight at a time.
BOOL IsAscii(const unsigned char *p, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        if (w & 0x8080808080808080ull) return NO;
    }
    for (; i < n; i++)
        if (p[i] & 0x80) return NO;
    return YES;
}

// FolderSearch::readFileStable into pages: stat'ed before, read, stat'ed
// again and read again (twice at most) when the two differ. nil when the
// ordinary read should do it: the file is not what it was when asked
// about, grew while read, is not UTF-8, or anything failed.
MCDiskText *ReadLargeText(NSString *path, unsigned long long maxBytes) {
    const char *cpath = path.fileSystemRepresentation;
    FolderSearch::FileStamp before = FolderSearch::stampOf(cpath);
    for (int attempt = 0;; attempt++) {
        if (!before.exists || !before.regular || before.size > maxBytes ||
            before.size < kPagedReadBytes)
            return nil;
        const size_t mapped = (size_t)before.size + 1;   // a byte more, to notice it grew
        void *pages = mmap(nullptr, mapped, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (pages == MAP_FAILED) return nil;
        unsigned char *bytes = (unsigned char *)pages;
        const int fd = open(cpath, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        size_t got = 0;
        bool ok = fd >= 0;
        while (ok && got < mapped) {
            const ssize_t n = read(fd, bytes + got, mapped - got);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                ok = n == 0;
                break;
            }
            got += (size_t)n;
        }
        if (fd >= 0) close(fd);
        const FolderSearch::FileStamp after = FolderSearch::stampOf(cpath);
        if (ok && after != before && attempt < 2) {   // written meanwhile: again
            munmap(pages, mapped);
            before = after;
            continue;
        }
        if (!ok || got == mapped || got > maxBytes) {   // failed, or grew past what was stat'ed
            munmap(pages, mapped);
            return nil;
        }
        const size_t skip = got >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF ? 3 : 0;
        NSString *text = nil;
        if (got == skip) {
            text = @"";
            munmap(pages, mapped);
        } else if (IsAscii(bytes + skip, got - skip)) {
            text = [[NSString alloc] initWithBytesNoCopy:bytes + skip length:got - skip
                                                encoding:NSASCIIStringEncoding
                                             deallocator:^(void *b, NSUInteger l) {
                (void)b;
                (void)l;
                munmap(pages, mapped);
            }];
            if (!text) munmap(pages, mapped);
        } else {
            // UTF-16 in the string (twice the bytes), the pages let go; not
            // UTF-8 at all (UTF-16 with its mark, Windows-1252): the
            // ordinary read decodes it.
            text = [[NSString alloc] initWithBytes:bytes + skip length:got - skip
                                          encoding:NSUTF8StringEncoding];
            munmap(pages, mapped);
        }
        if (!text) return nil;
        MCDiskText *d = [MCDiskText new];
        d.stamp = MCStampFrom(before);
        d.text = text;
        d.bom = skip != 0;
        d.encoding = MCTextEncodingUtf8;
        return d;
    }
}

}  // namespace

MCDiskText *MCReadDiskText(NSString *path, unsigned long long maxBytes) {
    MCDiskText *d = [MCDiskText new];
    if (!path.length) return d;
    if (MCDiskText *large = ReadLargeText(path, maxBytes)) return large;
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
                  (int)MCTextEncodingUtf16BE == (int)FolderSearch::TextEncoding::Utf16BE &&
                  (int)MCTextEncodingWindows1252 == (int)FolderSearch::TextEncoding::Windows1252,
              "MCTextEncoding mirrors FolderSearch::TextEncoding");

BOOL MCUtf8Bytes(NSString *text, std::string *out) {
    out->clear();
    const NSUInteger n = text.length;
    out->reserve(n);   // ASCII: exactly
    const NSUInteger block = 1 << 16;
    std::vector<char> buf(block * 3);
    for (NSUInteger at = 0; at < n;) {
        NSUInteger k = MIN(block, n - at);
        // Never a surrogate pair cut in two by the block's end.
        if (at + k < n && CFStringIsSurrogateHighCharacter([text characterAtIndex:at + k - 1])) k--;
        NSUInteger used = 0;
        NSRange rest = NSMakeRange(0, 0);
        if (![text getBytes:buf.data() maxLength:buf.size() usedLength:&used
                   encoding:NSUTF8StringEncoding options:0 range:NSMakeRange(at, k)
             remainingRange:&rest] ||
            rest.length)
            return NO;   // a lone surrogate
        out->append(buf.data(), used);
        at += k;
    }
    return YES;
}

BOOL MCEncodeFileText(NSString *text, MCTextEncoding encoding, BOOL bom, std::string *bytes,
                      std::string *why) {
    std::string utf8;
    if (!MCUtf8Bytes(text ?: @"", &utf8)) {
        if (why) *why = "the text holds a character UTF-8 cannot store";
        return NO;
    }
    if (encoding == MCTextEncodingUtf8) {   // as encodeFileText, without a second copy
        if (bom) utf8.insert(0, "\xEF\xBB\xBF");
        *bytes = std::move(utf8);
        return YES;
    }
    *bytes = FolderSearch::encodeFileText(utf8, (FolderSearch::TextEncoding)encoding, bom);
    return YES;
}

MCTextEncoding MCEncodingToWrite(NSString *text, MCTextEncoding encoding, NSString **unheld) {
    if (encoding != MCTextEncodingWindows1252) return encoding;
    NSData *utf8 = [text ?: @"" dataUsingEncoding:NSUTF8StringEncoding];
    if (!utf8) return encoding;   // a lone surrogate: MCEncodeFileText says so
    std::string first;
    if (FolderSearch::canEncode(std::string((const char *)utf8.bytes, utf8.length),
                                FolderSearch::TextEncoding::Windows1252, &first))
        return encoding;
    if (unheld) *unheld = [NSString stringWithUTF8String:first.c_str()] ?: @"?";
    return MCTextEncodingUtf8;
}

NSString *MCEncodingStatusLabel(MCTextEncoding encoding, BOOL bom) {
    if (encoding == MCTextEncodingUtf8 && !bom) return nil;
    return @(FolderSearch::encodingLabel((FolderSearch::TextEncoding)encoding, bom));
}

// A tab's undo manager. While the session's host owns the tab's history
// (MCTab session, editor.session-host) it is the host's front: undo, redo,
// undoNestedGroup and what they would do are the host's, and a name given
// to a step (Toggle Comment, Change Color, ...) names the host's step too.
// Everything that undoes through the tab's manager (the menu, a viewer's
// table, the workspace edit applier's put-back, the GUI checks) then undoes
// on the host. What NSTextView and the editor register still goes on
// NSUndoManager's own stacks, which are never undone then: NSTextView keeps
// deciding by the manager it gets (an undoManager answered while allowsUndo
// was off made it register twice), so it is left exactly as it was. Without
// a session it is NSUndoManager, untouched.
@interface MCTabUndoManager : NSUndoManager
@property(nonatomic, weak) MCTab *tab;
@end

@implementation MCTabUndoManager {
    NSInteger _ownGroups;   // depth of groups begun outside event grouping
}
- (MCSessionDoc *)host { return self.tab.session; }

- (void)setActionName:(NSString *)name {
    [super setActionName:name];
    [self.host nameStep:name];
}

// A group begun with event grouping off (the GUI checks' group around one
// simulated key press) is an undo action of its own: a step of its own on
// the host, its typing joining nothing before it. Groups NSTextView and
// NSUndoManager open by event are left alone.
- (void)beginUndoGrouping {
    MCSessionDoc *h = self.host;
    if (h && (_ownGroups > 0 || !self.groupsByEvent) && _ownGroups++ == 0) {
        [h closeStep];
        [h breakCoalescing];
    }
    [super beginUndoGrouping];
}

- (void)endUndoGrouping {
    [super endUndoGrouping];
    MCSessionDoc *h = self.host;
    if (h && _ownGroups > 0 && --_ownGroups == 0) [h closeStep];
}

// The steps NSUndoManager had go, and so does the host's history.
- (void)removeAllActions {
    [super removeAllActions];
    [self.host forgetHistory];
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
    [_session pathChanged];   // the host finds it by its path (Save As, a rename)
    [_owner tabPathChanged:self];
}

- (BOOL)attachHeld:(uint64_t)docId link:(MCSessionLinkRef *)link {
    if (_session || !link) return NO;
    MCSessionDoc *d = [[MCSessionDoc alloc] initWithTab:self held:docId link:link];
    if (!d.heldText) {
        [d detach];   // the host did not answer: nothing of it is closed there
        return NO;
    }
    _session = d;
    _live = nil;
    _sourceText = [d.heldText copy];
    _textGeneration++;
    _heldFromHost = YES;
    return YES;
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

- (void)takeTextFromStorage:(NSString *)text {
    if (!_session) {
        self.sourceText = text;
        return;
    }
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
