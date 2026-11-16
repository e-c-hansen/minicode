// Latex.mm — LaTeX preview: tectonic typesets it, PDFKit shows it, and a
// double-click on the page opens the source that produced that text.
//
// The parts worth knowing:
//   * The buffer is typeset from a hidden sibling file (".<name>.minicode.tex")
//     so \input and \includegraphics still resolve, and so the preview never
//     saves over the user's file behind their back.
//   * SyncTeX maps a point on the page back to a source line; LatexDoc turns
//     that line, plus the word under the pointer, into a byte range.
//   * An edit replaces exactly that range. The document is never regenerated
//     from a model, so nothing the parser missed can be damaged.
//   * Forward search runs the other way: SyncTeX's records for a source line
//     give the lines of text on the page it reached (SyncTexIndex::
//     boxesForLine), lit up for a moment.
//   * A typeset that fails lists tectonic's problems (LatexLog.h) above its
//     log, each a link to its line; one that works lists its warnings
//     (TeX's transcript, kept with --keep-logs) behind the Warnings button.
//     The controller also dots those lines in the gutter while the source
//     shows (problemsForPath:source:).
//   * Each preview typesets from a sibling and into a folder of its own
//     (LatexScratch::Owner), so two windows on one file never collide.
//   * A click on text from an \input or \include file edits that file: it
//     is parsed on its own (its tab's buffer, else the disk), and the edit
//     goes into its tab through the controller (inputText, onInputEdited),
//     unsaved. The typeset reads such files from disk, so the bar says
//     which have unsaved changes until they are saved.
#import "Latex.h"
#import "EditorFont.h"
#import "AppSettings.h"
#import "Lsp.h"   // MCDiagnosticColor
#import "MarkdownMath.h"   // MCMathWarmUp
#import <Quartz/Quartz.h>
#import <CommonCrypto/CommonDigest.h>
#include "LatexDoc.h"
#include "MarkdownEdit.h"   // sourceOffsetFor
#include "LatexLog.h"
#include "LatexScratch.h"
#include "SyncTex.h"
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <zlib.h>
#include <map>
#include <string>

// tectonic is pinned so the download is reproducible; any tectonic already on
// the machine is preferred over ours. The checksums are the ones GitHub
// publishes for the release's assets (as the Linux port's), checked against
// a download of each; an archive that does not match is never unpacked.
static NSString *const kTectonicVersion = @"0.17.0";
#if defined(__aarch64__)
static NSString *const kTectonicArch = @"aarch64-apple-darwin";
static NSString *const kTectonicSha256 =
    @"a3f1cac7c5678f01661a92212f58480ae3b0634115d880dbc59e2953ded45667";
#else
static NSString *const kTectonicArch = @"x86_64-apple-darwin";
static NSString *const kTectonicSha256 =
    @"7c90ef5b6ddb1eb1937e4337add5237b79338e4b9676459fa91187d24d6cdf80";
#endif


// ---------------------------------------------------------------- tectonic

static NSString *MCSupportDir(void) {
    NSString *base = NSSearchPathForDirectoriesInDomains(
        NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
    return [base stringByAppendingPathComponent:@"MiniCode"];
}

static NSString *MCManagedTectonic(void) {
    return [MCSupportDir() stringByAppendingPathComponent:@"bin/tectonic"];
}

NSString *MCTectonicPath(void) {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSMutableArray<NSString *> *candidates = [NSMutableArray array];
    NSString *override = NSProcessInfo.processInfo.environment[@"MINICODE_TECTONIC"];
    if (override.length) [candidates addObject:override];
    [candidates addObject:MCManagedTectonic()];
    NSString *path = NSProcessInfo.processInfo.environment[@"PATH"] ?: @"";
    for (NSString *dir in [path componentsSeparatedByString:@":"])
        if (dir.length) [candidates addObject:[dir stringByAppendingPathComponent:@"tectonic"]];
    // A GUI app launched from Finder inherits a short PATH, so look in the
    // usual places for a Homebrew install too.
    for (NSString *dir in @[@"/opt/homebrew/bin", @"/usr/local/bin", @"/usr/bin"])
        [candidates addObject:[dir stringByAppendingPathComponent:@"tectonic"]];

    for (NSString *c in candidates)
        if ([fm isExecutableFileAtPath:c]) return c;
    return nil;
}

// ------------------------------------------------------------ small helpers

// Read a gzipped file (the .synctex.gz tectonic writes) into memory.
static std::string ReadGzip(NSString *path) {
    std::string out;
    gzFile f = gzopen(path.fileSystemRepresentation, "rb");
    if (!f) return out;
    char buf[64 * 1024];
    int got;
    while ((got = gzread(f, buf, sizeof(buf))) > 0) out.append(buf, (size_t)got);
    gzclose(f);
    return out;
}

static std::string Utf8(NSString *s) {
    const char *c = s.UTF8String;
    return c ? std::string(c) : std::string();
}

static NSString *NsStr(const std::string &s) {
    NSString *out = [[NSString alloc] initWithBytes:s.data()
                                             length:s.size()
                                           encoding:NSUTF8StringEncoding];
    return out ?: @"";
}

static NSString *HexDigest(const unsigned char *digest) {
    NSMutableString *hex = [NSMutableString stringWithCapacity:2 * CC_SHA256_DIGEST_LENGTH];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [hex appendFormat:@"%02x", digest[i]];
    return hex;
}

static NSString *Sha256Hex(const void *bytes, size_t length) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes, (CC_LONG)length, digest);
    return HexDigest(digest);
}

// A file's SHA-256 and size, read in pieces (the tectonic archive is 21 MB);
// nil when it cannot be read. A symlink is not followed.
static NSString *FileSha256Hex(NSString *path, unsigned long long *sizeOut) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return nil;
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    unsigned long long total = 0;
    char buf[64 * 1024];
    ssize_t got;
    while ((got = read(fd, buf, sizeof buf)) > 0) {
        CC_SHA256_Update(&ctx, buf, (CC_LONG)got);
        total += (unsigned long long)got;
    }
    close(fd);
    if (got < 0) return nil;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &ctx);
    if (sizeOut) *sizeOut = total;
    return HexDigest(digest);
}

// When a process started (Unix seconds), or 0 when there is no such process
// (as BackupStore.mm's, which keys backups the same way).
static double ProcessStart(pid_t pid) {
    struct kinfo_proc info;
    size_t size = sizeof info;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    if (sysctl(mib, 4, &info, &size, NULL, 0) != 0 || size == 0) return 0;
    const struct timeval tv = info.kp_proc.p_starttime;
    return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

// ------------------------------------------------------- the output folders

// Every preview's output goes in a folder of its own under this one:
// MiniCode-latex in the user's temporary folder ($MINICODE_LATEX_WORK in
// the GUI checks, since NSTemporaryDirectory() ignores TMPDIR).
static NSString *const kSiblingRecordName = @"sibling.record";

NSString *MCLatexWorkRoot(void) {
    NSString *override = NSProcessInfo.processInfo.environment[@"MINICODE_LATEX_WORK"];
    if (override.length) return override;
    return [NSTemporaryDirectory() stringByAppendingPathComponent:@"MiniCode-latex"];
}

// Preview `view` of this process, as its folder and sibling are named.
static LatexScratch::Owner MCLatexOwner(NSUInteger view) {
    static long long start = (long long)floor(ProcessStart(getpid()));
    LatexScratch::Owner o;
    o.pid = getpid();
    o.start = start;
    o.view = view;
    return o;
}

NSString *MCLatexWorkFolder(NSString *texPath, NSUInteger view) {
    std::string hash;
    if (texPath.length) {
        char resolved[PATH_MAX];
        NSString *path = realpath(texPath.fileSystemRepresentation, resolved)
            ? @(resolved) : texPath.stringByStandardizingPath;
        hash = Utf8(Sha256Hex(path.UTF8String, strlen(path.UTF8String)));
    }
    return [MCLatexWorkRoot() stringByAppendingPathComponent:
        NsStr(LatexScratch::folderName(hash, MCLatexOwner(view)))];
}

// A path as the kernel spells it (/tmp is /private/tmp), or as given.
static std::string MCRealPath(const std::string &path) {
    char resolved[PATH_MAX];
    return realpath(path.c_str(), resolved) ? std::string(resolved) : path;
}

// The folders the live previews of this process use, counted, so one closing
// never removes a folder another preview of the same file still has.
static NSCountedSet<NSString *> *gLatexFoldersInUse;

// What to do with one sibling record: a crash's copy is removed, a record
// whose writer is gone is dropped (with its folder, when no preview here
// uses it). Returns whether a sibling was removed. Any thread.
static BOOL SweepSiblingRecord(NSString *recordPath, BOOL removeFolderWhenGone) {
    NSData *data = [NSData dataWithContentsOfFile:recordPath];
    if (!data) return NO;
    LatexScratch::Record r;
    NSFileManager *fm = [NSFileManager defaultManager];
    if (!LatexScratch::parse(std::string((const char *)data.bytes, data.length), &r)) {
        [fm removeItemAtPath:recordPath error:nil];
        return NO;
    }
    const double start = ProcessStart((pid_t)r.pid);
    const bool alive = r.pid > 0 && start > 0 && fabs(start - r.start) < 1.0;
    NSString *sibling = NsStr(r.sibling);
    unsigned long long size = 0;
    NSString *sha = alive ? nil : FileSha256Hex(sibling, &size);
    const LatexScratch::Action act =
        LatexScratch::decide(r, alive, sha != nil, size, Utf8(sha ?: @""));
    if (act == LatexScratch::Action::Keep) return NO;
    BOOL removed = NO;
    if (act == LatexScratch::Action::RemoveSibling)
        removed = [fm removeItemAtPath:sibling error:nil];
    [fm removeItemAtPath:recordPath error:nil];
    if (removeFolderWhenGone) {
        NSString *folder = recordPath.stringByDeletingLastPathComponent;
        dispatch_sync(dispatch_get_main_queue(), ^{
            if ([gLatexFoldersInUse countForObject:folder] == 0)
                [fm removeItemAtPath:folder error:nil];
        });
    }
    return removed;
}

void MCLatexRemoveStaleSiblings(void (^done)(NSUInteger removed)) {
    NSString *root = MCLatexWorkRoot();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSUInteger removed = 0;
        NSFileManager *fm = [NSFileManager defaultManager];
        for (NSString *name in [fm contentsOfDirectoryAtPath:root error:nil] ?: @[]) {
            NSString *folder = [root stringByAppendingPathComponent:name];
            NSString *record = [folder stringByAppendingPathComponent:kSiblingRecordName];
            if (SweepSiblingRecord(record, YES)) removed++;
            // A preview's folder outlives a crash of its process: it goes
            // once that process has (a folder of a live one, or one named
            // as an older minicode names them, stays).
            LatexScratch::Owner owner;
            if (!LatexScratch::parseFolderName(Utf8(name), &owner) || owner.pid == getpid())
                continue;
            if (LatexScratch::ownerGone(owner, ProcessStart((pid_t)owner.pid)) &&
                ![fm fileExistsAtPath:record])
                [fm removeItemAtPath:folder error:nil];
        }
        if (done) dispatch_async(dispatch_get_main_queue(), ^{ done(removed); });
    });
}

// ------------------------------------------------------------ the problems

@implementation MCLatexProblem
@end

static NSString *const kProblemLink = @"minicode-latex-problem:";

// tectonic's output and TeX's transcript as problems in the user's files:
// the hidden sibling's lines are the document's. Any thread (it asks the
// disk what exists).
static NSArray<MCLatexProblem *> *MCLatexProblemsFromLog(NSString *log, NSString *transcript,
                                                         NSString *texPath, NSString *sibling) {
    NSMutableArray<MCLatexProblem *> *out = [NSMutableArray array];
    NSFileManager *fm = [NSFileManager defaultManager];
    auto exists = [fm](const std::string &p) {
        BOOL dir = NO;
        return [fm fileExistsAtPath:NsStr(p) isDirectory:&dir] && !dir;
    };
    for (const LatexLog::Problem &p :
         LatexLog::collect(Utf8(log ?: @""), Utf8(transcript ?: @""))) {
        MCLatexProblem *m = [MCLatexProblem new];
        const std::string path = LatexLog::sourcePath(p.file, Utf8(texPath ?: @""),
                                                      Utf8(sibling ?: @""), exists);
        m.path = path.empty() ? nil : NsStr(path);
        m.line = m.path ? p.line : 0;
        m.severity = (NSInteger)p.severity;
        m.message = NsStr(p.message);
        [out addObject:m];
    }
    return out;
}

// "Line 5: Undefined control sequence", naming the file when it is not the
// document itself; `place` gets the part before the colon.
static NSString *MCDescribeProblem(MCLatexProblem *p, NSString *texPath, NSString **place) {
    NSString *where = @"";
    if (p.path && ![p.path isEqualToString:texPath])
        where = p.line > 0 ? [NSString stringWithFormat:@"%@, line %ld",
                                 p.path.lastPathComponent, (long)p.line]
                           : p.path.lastPathComponent;
    else if (p.line > 0)
        where = [NSString stringWithFormat:@"Line %ld", (long)p.line];
    if (place) *place = where;
    return where.length ? [NSString stringWithFormat:@"%@: %@", where, p.message] : p.message;
}

// ------------------------------------------------------------ the PDF view

@interface MCPdfView : PDFView
@property(nonatomic, copy) void (^onActivate)(PDFPage *page, NSPoint pt);
@end

@implementation MCPdfView
- (void)mouseDown:(NSEvent *)event {
    [super mouseDown:event];
    if (event.clickCount != 2 || !self.onActivate) return;
    NSPoint inView = [self convertPoint:event.locationInWindow fromView:nil];
    PDFPage *page = [self pageForPoint:inView nearest:YES];
    if (!page) return;
    self.onActivate(page, [self convertPoint:inView toPage:page]);
}
// PDFView keeps its own menu; ours would only get in the way.
- (NSMenu *)menuForEvent:(NSEvent *)event { return [super menuForEvent:event]; }
@end

// A text view that commits on Return and cancels on Escape, so a quick field
// edit needs no mousing. Shift+Return still types a newline.
@interface MCEditText : NSTextView
@property(nonatomic, copy) void (^onCommit)(void);
@property(nonatomic, copy) void (^onCancel)(void);
@end

@implementation MCEditText
- (void)insertNewline:(id)sender {
    if (NSEvent.modifierFlags & NSEventModifierFlagShift) {
        [super insertNewline:sender];
        return;
    }
    if (self.onCommit) self.onCommit();
}
- (void)cancelOperation:(id)sender {
    if (self.onCancel) self.onCancel();
}
@end

// ----------------------------------------------------------------- the panel

@interface LatexView () <NSPopoverDelegate, NSTextViewDelegate>
@property(nonatomic, strong) MCPdfView *pdf;
@property(nonatomic, strong) NSView *statusBar;
@property(nonatomic, strong) NSTextField *statusLabel;
@property(nonatomic, strong) NSButton *actionButton;
@property(nonatomic, strong) NSButton *warningsButton;
@property(nonatomic, strong) NSScrollView *logScroll;
@property(nonatomic, strong) NSTextView *logView;
@property(nonatomic, strong) NSProgressIndicator *spinner;
@property(nonatomic, strong) NSPopover *popover;
@property(nonatomic, strong) MCEditText *editText;
@property(nonatomic, strong) NSButton *addItemButton;
@property(nonatomic, copy)   NSString *texPath;
@property(nonatomic, assign) BOOL compiling;
@property(nonatomic, assign) BOOL compileQueued;
@property(nonatomic, assign) NSUInteger generation;
@property(nonatomic, strong) NSTask *task;
@property(nonatomic, strong, readwrite) NSArray<MCLatexProblem *> *problems;
@end

@implementation LatexView {
    std::string _src;            // the buffer being previewed
    SyncTexIndex _synctex;
    LatexDoc _doc;
    LatexSpan _editing;          // the span the popover is editing
    BOOL _editingValid;
    BOOL _addingItem;            // the popover is composing a new \item
    LatexList _editingList;
    int _editingItemIndex;
    std::vector<std::string> _undo;   // source before each preview edit
    std::vector<std::string> _redo;
    NSData *_pdfData;            // the PDF on screen, as tectonic wrote it
    std::string _pdfSrc;         // the source that PDF was typeset from
    NSMutableArray *_pdfWaiters; // pdfForPath: calls waiting on a typeset
    NSString *_folder;           // the output folder this view counts as using
    std::string _problemsSrc;    // the source `problems` came from
    NSInteger _forwardLine;      // a forward search waiting for its typeset
    NSString *_forwardFile;      // and the \input file it is from (nil: the document)
    BOOL _showingWarnings;       // the warnings list holds the page's place
    BOOL _stopped;               // its window closed (stopPreview)
    LatexScratch::Owner _owner;  // this preview's own sibling and folder
    NSArray<PDFAnnotation *> *_forwardMarks;   // what it lit up, for a moment
    PDFPage *_forwardPage;
    // The \input files' sources, parsed, by path, with the text each was
    // parsed from (a click parses again only when the text changed).
    std::map<std::string, std::pair<std::string, LatexDoc>> _inputDocs;
    NSString *_editingFile;      // the \input file the popover edits (nil: the document)
    std::string _editingSrc;     // and its text when the box opened
}

// Enough history for a working session without holding a whole file's worth of
// copies for a long-lived window.
static const size_t kMaxUndo = 50;

static const CGFloat kStatusHeight = 26;

- (instancetype)initWithPath:(NSString *)texPath source:(NSString *)source {
    if (!(self = [super initWithFrame:NSMakeRect(0, 0, 600, 400)])) return nil;
    self.wantsLayer = YES;
    _texPath = [texPath copy];
    _src = Utf8(source ?: @"");
    _doc = LatexDoc::parse(_src);
    _editingItemIndex = -1;
    static NSUInteger views = 0;
    _owner = MCLatexOwner(++views);

    self.pdf = [[MCPdfView alloc] initWithFrame:self.bounds];
    self.pdf.autoScales = YES;
    self.pdf.displayMode = kPDFDisplaySinglePageContinuous;
    self.pdf.displaysPageBreaks = YES;
    __weak LatexView *weakSelf = self;
    self.pdf.onActivate = ^(PDFPage *page, NSPoint pt) {
        [weakSelf openEditorForPage:page point:pt];
    };
    [self addSubview:self.pdf];

    self.logScroll = [[NSScrollView alloc] initWithFrame:self.bounds];
    self.logScroll.hasVerticalScroller = YES;
    self.logScroll.drawsBackground = YES;
    self.logView = [[NSTextView alloc] initWithFrame:self.bounds];
    self.logView.editable = NO;
    self.logView.delegate = self;   // the problems' links
    self.logView.font = MCMonoFont(12, NSFontWeightRegular);
    self.logView.textContainerInset = NSMakeSize(12, 12);
    self.logScroll.documentView = self.logView;
    self.logScroll.hidden = YES;
    [self addSubview:self.logScroll];

    self.statusBar = [[NSView alloc] initWithFrame:NSZeroRect];
    self.statusBar.wantsLayer = YES;
    [self addSubview:self.statusBar];

    self.statusLabel = [NSTextField labelWithString:@""];
    self.statusLabel.font = [NSFont systemFontOfSize:11];
    self.statusLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [self.statusBar addSubview:self.statusLabel];

    self.spinner = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    self.spinner.style = NSProgressIndicatorStyleSpinning;
    self.spinner.controlSize = NSControlSizeSmall;
    self.spinner.displayedWhenStopped = NO;
    [self.statusBar addSubview:self.spinner];

    self.actionButton = [NSButton buttonWithTitle:@"Recompile"
                                           target:self
                                           action:@selector(actionPressed:)];
    self.actionButton.bezelStyle = NSBezelStyleRounded;
    self.actionButton.font = [NSFont systemFontOfSize:11];
    self.actionButton.controlSize = NSControlSizeSmall;
    [self.statusBar addSubview:self.actionButton];

    // A typeset that worked with warnings: they are listed behind this.
    self.warningsButton = [NSButton buttonWithTitle:@"Warnings"
                                             target:self
                                             action:@selector(toggleWarnings:)];
    self.warningsButton.bezelStyle = NSBezelStyleRounded;
    self.warningsButton.font = [NSFont systemFontOfSize:11];
    self.warningsButton.controlSize = NSControlSizeSmall;
    self.warningsButton.hidden = YES;
    [self.statusBar addSubview:self.warningsButton];

    [self applySettings];
    [self relayout];
    [self compileNow];
    return self;
}

- (void)dealloc {
    [_task terminate];
    [self letGoOfFolder];
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self relayout];
}

- (void)relayout {
    NSRect b = self.bounds;
    CGFloat top = MAX(0, b.size.height - kStatusHeight);
    self.statusBar.frame = NSMakeRect(0, top, b.size.width, MIN(kStatusHeight, b.size.height));
    NSRect content = NSMakeRect(0, 0, b.size.width, top);
    self.pdf.frame = content;
    self.logScroll.frame = content;

    CGFloat pad = 8;
    self.actionButton.frame = NSMakeRect(b.size.width - 96 - pad, 3, 96, 20);
    const CGFloat warningsWidth = self.warningsButton.isHidden ? 0 : 112;
    self.warningsButton.frame = NSMakeRect(b.size.width - 96 - pad - 4 - warningsWidth, 3,
                                           warningsWidth, 20);
    self.spinner.frame = NSMakeRect(pad, 6, 14, 14);
    CGFloat labelX = pad + (self.spinner.isHidden ? 0 : 20);
    self.statusLabel.frame =
        NSMakeRect(labelX, 5, MAX(0, b.size.width - labelX - 112 - warningsWidth), 16);
}

- (PDFView *)latexPDFView { return self.pdf; }

- (void)applySettings {
    AppSettings *cfg = [AppSettings shared];
    NSColor *bg = [cfg background:Surface::Editor];
    self.layer.backgroundColor = bg.CGColor;
    self.pdf.backgroundColor = bg;
    self.logScroll.backgroundColor = bg;
    self.logView.backgroundColor = bg;
    self.logView.textColor = [cfg text:Surface::Editor];
    self.statusBar.layer.backgroundColor = [cfg background:Surface::Statusbar].CGColor;
    self.statusLabel.textColor = [cfg text:Surface::Statusbar];
}

// --------------------------------------------------------------- the source

- (void)setPath:(NSString *)texPath source:(NSString *)source {
    BOOL samePath = (texPath == self.texPath) || [texPath isEqualToString:self.texPath];
    std::string incoming = Utf8(source ?: @"");
    if (samePath && incoming == _src) return;
    self.texPath = [texPath copy];
    _src = incoming;
    _doc = LatexDoc::parse(_src);
    [self scheduleCompile];
}

- (void)scheduleCompile {
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(compileNow)
                                               object:nil];
    [self performSelector:@selector(compileNow) withObject:nil afterDelay:0.6];
}

// Apply an edit produced by LatexDoc and tell the window about it.
- (void)applyEdit:(const LatexDoc::Edit &)edit {
    _undo.push_back(_src);
    if (_undo.size() > kMaxUndo) _undo.erase(_undo.begin());
    _redo.clear();
    _src = edit.source;
    _doc = LatexDoc::parse(_src);
    if (self.onSourceEdited) self.onSourceEdited(NsStr(_src));
    [self compileNow];
}

// ------------------------------------------------------------------- undo

- (BOOL)canUndoEdit { return !_undo.empty(); }
- (BOOL)canRedoEdit { return !_redo.empty(); }

- (void)stepFrom:(std::vector<std::string> &)from
              to:(std::vector<std::string> &)to
           saying:(NSString *)what {
    if (from.empty()) { NSBeep(); return; }
    to.push_back(_src);
    _src = from.back();
    from.pop_back();
    _doc = LatexDoc::parse(_src);
    [self.popover performClose:nil];
    if (self.onSourceEdited) self.onSourceEdited(NsStr(_src));
    [self setStatus:what busy:YES];
    [self compileNow];
}

- (void)undoEdit { [self stepFrom:_undo to:_redo saying:@"Undid a preview edit"]; }
- (void)redoEdit { [self stepFrom:_redo to:_undo saying:@"Redid a preview edit"]; }
- (void)forgetEdits {
    _undo.clear();
    _redo.clear();
}

// Cmd+Z reaches here through the responder chain when the page has focus; the
// window controller forwards it when something else does.
- (void)undo:(id)sender { [self undoEdit]; }
- (void)redo:(id)sender { [self redoEdit]; }

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(undo:)) return [self canUndoEdit];
    if (item.action == @selector(redo:)) return [self canRedoEdit];
    return YES;
}

// -------------------------------------------------------------- compiling

// This document's output folder, made if need be (0700, as the temporary
// folder it is in), and counted as in use while this view has it.
- (NSString *)workFolder { return MCLatexWorkFolder(self.texPath, (NSUInteger)_owner.view); }
- (NSString *)siblingPath { return [self scratchTexPath]; }

- (NSString *)workDir {
    NSString *dir = self.workFolder;
    if (![dir isEqualToString:_folder]) {
        [self letGoOfFolder];
        if (!gLatexFoldersInUse) gLatexFoldersInUse = [NSCountedSet set];
        [gLatexFoldersInUse addObject:dir];
        _folder = [dir copy];
    }
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                              withIntermediateDirectories:YES
                                               attributes:@{NSFilePosixPermissions: @0700}
                                                    error:nil];
    return dir;
}

// The output folder goes with the last view using it, unless a typeset is
// still writing there (then the end of that typeset removes it).
- (void)letGoOfFolder {
    if (!_folder) return;
    [gLatexFoldersInUse removeObject:_folder];
    if ([gLatexFoldersInUse countForObject:_folder] == 0 && !self.compiling)
        [[NSFileManager defaultManager] removeItemAtPath:_folder error:nil];
    _folder = nil;
}

// The hidden sibling we actually hand to tectonic, so relative paths in the
// document still resolve while the user's own file stays untouched.
// Named for this preview (".paper.<pid>-<view>.minicode.tex"), so another
// window's preview of the same file has a copy of its own.
- (NSString *)scratchTexPath {
    NSString *dir = self.texPath.stringByDeletingLastPathComponent;
    NSString *base = self.texPath.lastPathComponent.stringByDeletingPathExtension;
    if (!dir.length || !base.length) return [self.workFolder
        stringByAppendingPathComponent:@"untitled.minicode.tex"];
    return [dir stringByAppendingPathComponent:NsStr(LatexScratch::siblingName(Utf8(base), _owner))];
}

// Before the sibling is written, what it will hold and who wrote it, so a
// crash during the typeset leaves something the next launch can clean up
// (MCLatexRemoveStaleSiblings). A copy an earlier, crashed process left for
// this document is dealt with first.
- (void)recordSibling:(NSString *)sibling bytes:(NSData *)bytes inFolder:(NSString *)folder {
    NSString *record = [folder stringByAppendingPathComponent:kSiblingRecordName];
    SweepSiblingRecord(record, NO);
    LatexScratch::Record r;
    r.sibling = Utf8(sibling);
    r.pid = getpid();
    r.start = ProcessStart(getpid());
    r.size = bytes.length;
    r.sha256 = Utf8(Sha256Hex(bytes.bytes, bytes.length));
    const std::string text = LatexScratch::format(r);
    [[NSData dataWithBytes:text.data() length:text.size()] writeToFile:record
                                                              atomically:YES];
}

- (void)stopPreview {
    _stopped = YES;
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
    [self.task terminate];
    [self.popover performClose:nil];
    [self letGoOfFolder];
}

- (void)compileNow {
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(compileNow)
                                               object:nil];
    if (_stopped) return;
    NSString *tool = MCTectonicPath();
    if (!tool) { [self showTectonicMissing]; return; }
    if (self.compiling) { self.compileQueued = YES; return; }

    static dispatch_once_t sweepOnce;
    dispatch_once(&sweepOnce, ^{ MCLatexRemoveStaleSiblings(nil); });

    NSString *scratch = [self scratchTexPath];
    NSString *outDir = [self workDir];
    NSString *stem = scratch.lastPathComponent.stringByDeletingPathExtension;
    NSString *record = [outDir stringByAppendingPathComponent:kSiblingRecordName];
    NSError *err = nil;
    // The buffer's own bytes, which the record's hash is of.
    NSData *bytes = [NSData dataWithBytes:_src.data() length:_src.size()];
    [self recordSibling:scratch bytes:bytes inFolder:outDir];
    if (![bytes writeToFile:scratch options:NSDataWritingAtomic error:&err]) {
        [[NSFileManager defaultManager] removeItemAtPath:record error:nil];
        [self showFailure:[NSString stringWithFormat:@"Cannot write the preview "
                           @"copy next to your file:\n%@", err.localizedDescription]];
        [self answerPdfWaiters:@"The preview copy could not be written next to "
                                "the file, so the document was not typeset."];
        return;
    }

    self.compiling = YES;
    self.compileQueued = NO;
    NSUInteger gen = ++self.generation;
    std::string compiledSrc = _src;
    [self setStatus:@"Typesetting…" busy:YES];

    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:tool];
    // --untrusted keeps shell escape off whatever the document asks for, as
    // for the Markdown preview's math.
    // --keep-logs leaves TeX's transcript, whose warnings are listed.
    task.arguments = @[@"--synctex", @"--untrusted", @"--keep-logs", @"--chatter", @"minimal",
                       @"--color", @"never", @"--outdir", outDir, scratch];
    NSString *texPath = self.texPath;
    NSString *cwd = texPath.stringByDeletingLastPathComponent;
    if (cwd.length) task.currentDirectoryURL = [NSURL fileURLWithPath:cwd];
    NSPipe *pipe = [NSPipe pipe];
    task.standardOutput = pipe;
    task.standardError = pipe;
    self.task = task;

    __weak LatexView *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *runErr = nil;
        NSData *output = nil;
        int status = -1;
        NSString *stopped = nil;
        if ([task launchAndReturnError:&runErr]) {
            // A document that loops (\def\x{\x}\x) never finishes, and every
            // later edit would queue behind it, so the run has a limit: long
            // enough for a first run's downloads on a slow network.
            const int64_t limitMinutes = 5;
            dispatch_queue_t utility = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, limitMinutes * 60 * NSEC_PER_SEC),
                           utility, ^{
                if (!task.running) return;
                [task terminate];
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), utility, ^{
                    if (task.running) kill(task.processIdentifier, SIGKILL);
                });
            });
            output = [pipe.fileHandleForReading readDataToEndOfFile];
            [task waitUntilExit];
            status = task.terminationStatus;
            if (task.terminationReason == NSTaskTerminationReasonUncaughtSignal) {
                status = 128 + status;
                stopped = [NSString stringWithFormat:@"tectonic did not finish within %lld "
                                                      "minutes, so it was stopped.\n\n",
                                                     (long long)limitMinutes];
            }
        }
        NSString *log = output.length
            ? [[NSString alloc] initWithData:output encoding:NSUTF8StringEncoding]
            : (runErr.localizedDescription ?: @"");
        NSString *transcriptPath = [outDir stringByAppendingPathComponent:
            [stem stringByAppendingPathExtension:@"log"]];
        NSData *transcriptData = [NSData dataWithContentsOfFile:transcriptPath];
        [[NSFileManager defaultManager] removeItemAtPath:transcriptPath error:nil];
        // TeX writes its transcript in the document's encoding; Latin-1 reads
        // any bytes, and the warnings' words are ASCII.
        NSString *transcript = transcriptData.length
            ? ([[NSString alloc] initWithData:transcriptData encoding:NSUTF8StringEncoding]
                   ?: [[NSString alloc] initWithData:transcriptData
                                            encoding:NSISOLatin1StringEncoding])
            : @"";
        NSArray<MCLatexProblem *> *problems =
            stopped ? @[] : MCLatexProblemsFromLog(log, transcript, texPath, scratch);
        if (stopped) log = [stopped stringByAppendingString:log ?: @""];
        NSString *pdfPath = [outDir stringByAppendingPathComponent:
            [stem stringByAppendingPathExtension:@"pdf"]];
        NSString *syncPath = [outDir stringByAppendingPathComponent:
            [stem stringByAppendingString:@".synctex.gz"]];
        // The bytes are kept so an export is exactly what tectonic wrote, not
        // PDFKit's re-save of it.
        NSData *pdfData = (status == 0) ? [NSData dataWithContentsOfFile:pdfPath] : nil;
        PDFDocument *doc = pdfData ? [[PDFDocument alloc] initWithData:pdfData] : nil;
        std::string sync = (status == 0) ? ReadGzip(syncPath) : std::string();
        [[NSFileManager defaultManager] removeItemAtPath:scratch error:nil];
        [[NSFileManager defaultManager] removeItemAtPath:record error:nil];

        dispatch_async(dispatch_get_main_queue(), ^{
            LatexView *me = weakSelf;
            // The view went away (or its window closed) while this ran: its
            // folder goes now, unless a preview still has it.
            const BOOL gone = !me || me->_stopped;
            if (gone) me.compiling = NO;
            if (gone && [gLatexFoldersInUse countForObject:outDir] == 0)
                [[NSFileManager defaultManager] removeItemAtPath:outDir error:nil];
            if (gone || gen != me.generation) return;
            me.compiling = NO;
            me.task = nil;
            BOOL ok = status == 0 && doc;
            [me setProblems:problems fromSource:compiledSrc];
            if (ok) {
                [me keepPdf:pdfData source:compiledSrc];
                [me showDocument:doc syncTex:sync];
            } else {
                me->_forwardLine = 0;   // no page to show it on
                [me showFailure:log.length ? log : @"tectonic did not run." problems:problems];
            }
            if (me.compileQueued) [me compileNow];
            else [me answerPdfWaiters:ok ? nil : @"The document did not typeset. "
                                             "The preview shows tectonic's log."];
        });
    });
}

- (void)showDocument:(PDFDocument *)doc syncTex:(const std::string &)sync {
    _synctex = SyncTexIndex::parse(sync);

    // Keep the reader where they were: same page, same scroll offset.
    PDFDestination *was = self.pdf.currentDestination;
    NSUInteger pageIndex = was ? [self.pdf.document indexForPage:was.page] : 0;
    CGFloat scale = self.pdf.scaleFactor;
    // Fit to the width (the default, and Zoom to Fit Width) stays so: a
    // scale set by hand would stop following the pane's width.
    BOOL autoScales = self.pdf.autoScales;
    BOOL hadDoc = self.pdf.document != nil;

    self.pdf.document = doc;
    if (!hadDoc) {
        // The first document opens at the top of its first page, margin and
        // all, as Preview opens a PDF. PDFView left it where its own scroll
        // view's insets and the first rescale to the pane's width put it,
        // which cut the title off under the bar. Once now and once after
        // the layout this turn brings; later typesets keep the reader's place.
        [self showFirstPageTop];
        __weak LatexView *weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            LatexView *me = weakSelf;
            // A forward search waiting for this typeset has put its line
            // in view since: that place wins.
            if (me && me.pdf.document == doc && !me->_forwardMarks) [me showFirstPageTop];
        });
    }
    if (hadDoc && pageIndex < doc.pageCount) {
        PDFPage *page = [doc pageAtIndex:pageIndex];
        PDFDestination *dest = [[PDFDestination alloc] initWithPage:page
                                                            atPoint:was.point];
        if (autoScales) self.pdf.autoScales = YES;
        else self.pdf.scaleFactor = scale;
        [self.pdf goToDestination:dest];
    }
    if (_showingWarnings && self.problems.count) {
        [self showWarningsList];
    } else {
        _showingWarnings = NO;
        self.logScroll.hidden = YES;
        self.pdf.hidden = NO;
    }
    [self showPagesStatus];
    self.actionButton.title = @"Recompile";
    if (_forwardLine > 0 && !self.compiling && _pdfSrc == _src) [self runForwardSearch];
}

// Scrolls the PDF to the top of its document (the first page's top edge with
// the page-break margin above it), with no automatic content insets: the
// view is never under the title bar, and an inset made the top unreachable.
- (void)showFirstPageTop {
    MCPdfView *v = self.pdf;
    if (!v.document.pageCount) return;
    [v layoutDocumentView];
    NSView *docView = v.documentView;
    NSScrollView *sv = docView.enclosingScrollView;
    if (!sv) {
        [v goToPage:[v.document pageAtIndex:0]];
        return;
    }
    sv.automaticallyAdjustsContentInsets = NO;
    sv.contentInsets = NSEdgeInsetsZero;
    NSClipView *clip = sv.contentView;
    const NSRect doc = [clip convertRect:docView.bounds fromView:docView];
    NSRect want = clip.bounds;
    want.origin.y = clip.isFlipped ? NSMinY(doc) : NSMaxY(doc) - NSHeight(want);
    [clip scrollToPoint:[clip constrainBoundsRect:want].origin];
    [sv reflectScrolledClipView:clip];
}

// "2 pages · 3 warnings · double-click text to edit it", and the Warnings
// button when there are any.
- (void)showPagesStatus {
    PDFDocument *doc = self.pdf.document;
    NSString *pages = doc.pageCount == 1 ? @"1 page"
        : [NSString stringWithFormat:@"%lu pages", (unsigned long)doc.pageCount];
    const NSUInteger n = self.problems.count;
    NSString *warnings = n == 0 ? @""
        : [NSString stringWithFormat:n == 1 ? @" · %lu warning" : @" · %lu warnings",
                                     (unsigned long)n];
    // Files the typeset reads from disk with unsaved changes in their
    // tabs: the page shows them only once they are saved.
    NSMutableArray<NSString *> *unsaved = [NSMutableArray array];
    if (self.inputUnsaved)
        for (NSString *f in [self inputFiles])
            if (self.inputUnsaved(f)) [unsaved addObject:f.lastPathComponent];
    NSString *tail = @"double-click text to edit it";
    if (unsaved.count == 1)
        tail = [NSString stringWithFormat:@"%@ has unsaved changes: save to see them in the PDF",
                                          unsaved[0]];
    else if (unsaved.count > 1)
        tail = [NSString stringWithFormat:@"%lu files it reads have unsaved changes: save to "
                                          @"see them in the PDF", (unsigned long)unsaved.count];
    [self setStatus:[NSString stringWithFormat:@"%@%@ · %@", pages, warnings, tail] busy:NO];
    self.warningsButton.hidden = n == 0;
    self.warningsButton.title = _showingWarnings ? @"Show Page"
        : [NSString stringWithFormat:@"%lu Warning%@", (unsigned long)n, n == 1 ? @"" : @"s"];
    [self relayout];
}

// The Warnings button: the list in the page's place, or the page again.
- (void)toggleWarnings:(id)sender {
    (void)sender;
    _showingWarnings = !_showingWarnings && self.problems.count > 0;
    if (_showingWarnings) {
        [self showWarningsList];
    } else {
        self.logScroll.hidden = YES;
        self.pdf.hidden = NO;
    }
    [self showPagesStatus];
}

- (void)showWarningsList {
    [self.logView.textStorage setAttributedString:[self problemRows:self.problems]];
    self.logScroll.hidden = NO;
    self.pdf.hidden = YES;
}

// ---------------------------------------------------------- forward search

- (void)revealSourceLine:(NSInteger)line {
    [self revealSourceLine:line inFile:nil];
}

- (void)revealSourceLine:(NSInteger)line inFile:(NSString *)path {
    _forwardLine = MAX(1, line);
    _forwardFile = [path isEqualToString:self.texPath] ? nil : [path copy];
    if (_showingWarnings) [self toggleWarnings:nil];
    // The page on show is the text's: go now. Otherwise the typeset under
    // way, or the one the last edit scheduled, ends in showDocument.
    if (!self.compiling && _pdfSrc == _src && self.pdf.document && !self.pdf.isHidden)
        [self runForwardSearch];
    else if (!self.compiling)
        [self scheduleCompile];
}

- (void)clearForwardMarks {
    for (PDFAnnotation *a in _forwardMarks) [_forwardPage removeAnnotation:a];
    if (_forwardMarks.count) [self.pdf annotationsChangedOnPage:_forwardPage];
    _forwardMarks = nil;
    _forwardPage = nil;
}

- (void)runForwardSearch {
    const NSInteger line = _forwardLine;
    _forwardLine = 0;
    if (line <= 0) return;
    [self clearForwardMarks];
    if (!_synctex.valid()) {
        [self setStatus:@"No SyncTeX data, so the page for that line is not known" busy:NO];
        return;
    }
    NSString *file = _forwardFile;
    _forwardFile = nil;
    // The file's name before its lines, when it is not the document itself.
    NSString *named = file ? [file.lastPathComponent stringByAppendingString:@", line"] : @"Line";
    const int tag = file ? [self inputTagForFile:file] : _synctex.tagForPath(Utf8([self scratchTexPath]));
    if (tag == 0) {
        [self setStatus:[NSString stringWithFormat:@"%@ is not part of %@'s last typeset",
                                                   file.lastPathComponent ?: @"That file",
                                                   self.texPath.lastPathComponent]
                   busy:NO];
        return;
    }
    int found = 0;
    const std::vector<SyncTexBox> boxes = _synctex.boxesForLine(tag, (int)line, &found);
    PDFDocument *doc = self.pdf.document;
    if (boxes.empty() || boxes[0].page < 1 || (NSUInteger)boxes[0].page > doc.pageCount) {
        [self setStatus:[NSString stringWithFormat:@"%@ %ld put nothing on the page",
                                                   named, (long)line] busy:NO];
        return;
    }
    PDFPage *page = [doc pageAtIndex:(NSUInteger)boxes[0].page - 1];
    const NSRect media = [page boundsForBox:kPDFDisplayBoxMediaBox];
    NSColor *tint = [NSColor colorWithSRGBRed:0x4E / 255.0 green:0xA1 / 255.0
                                         blue:0xF7 / 255.0 alpha:0.3];
    NSMutableArray<PDFAnnotation *> *marks = [NSMutableArray array];
    NSRect all = NSZeroRect;
    for (const SyncTexBox &b : boxes) {
        // SyncTeX measures down from the top, PDF up from the bottom.
        NSRect r = NSInsetRect(NSMakeRect(b.x, media.size.height - b.y - b.height,
                                          b.width, b.height), -1.5, -1.5);
        all = NSIsEmptyRect(all) ? r : NSUnionRect(all, r);
        PDFAnnotation *a = [[PDFAnnotation alloc] initWithBounds:r
                                                         forType:PDFAnnotationSubtypeSquare
                                                  withProperties:nil];
        PDFBorder *border = [PDFBorder new];
        border.lineWidth = 0;
        a.border = border;
        a.color = tint;
        a.interiorColor = tint;
        [page addAnnotation:a];
        [marks addObject:a];
    }
    _forwardMarks = marks;
    _forwardPage = page;
    [self.pdf annotationsChangedOnPage:page];
    // A little of what is above it shows too, for context.
    [self.pdf goToRect:NSInsetRect(all, 0, -24) onPage:page];
    NSString *what = found == line
        ? [NSString stringWithFormat:@"%@ %ld is on page %d", named, (long)line, boxes[0].page]
        : [NSString stringWithFormat:@"%@ %ld put nothing on the page; line %d, on page %d, "
                                     @"is the nearest that did", named, (long)line, found,
                                     boxes[0].page];
    [self setStatus:what busy:NO];
    __weak LatexView *weakSelf = self;
    NSArray *shown = marks;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        LatexView *me = weakSelf;
        if (me && me->_forwardMarks == shown) [me clearForwardMarks];
    });
}

// SyncTeX's input tag for an \input file of the document, read against
// the document's folder (where tectonic ran), 0 for none or the document.
- (int)inputTagForFile:(NSString *)path {
    if (!path.length || !_synctex.valid()) return 0;
    const int tag = _synctex.tagForFile(Utf8(path),
                                        Utf8(self.texPath.stringByDeletingLastPathComponent),
                                        MCRealPath);
    return tag == _synctex.tagForPath(Utf8([self scratchTexPath])) ? 0 : tag;
}

- (BOOL)typesetReadFile:(NSString *)path {
    return ![path isEqualToString:self.texPath] && [self inputTagForFile:path] != 0;
}

// The user's file behind SyncTeX input `tag`: a file the typeset read
// (an \input or \include, or a .bib), as the document's folder spells it,
// or nil for the document itself, the engine's own files (no path), and
// anything not a plain file on disk.
- (NSString *)inputFileForTag:(int)tag {
    if (tag == 0 || !_synctex.valid()) return nil;
    const std::string written = _synctex.pathForTag(tag);
    if (written.empty()) return nil;
    NSString *dir = self.texPath.stringByDeletingLastPathComponent;
    NSString *path = NsStr(written);
    if (!path.isAbsolutePath) path = [dir stringByAppendingPathComponent:path];
    path = path.stringByStandardizingPath;
    BOOL isDir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&isDir] || isDir)
        return nil;
    const std::string real = MCRealPath(Utf8(path));
    if (real == MCRealPath(Utf8(self.texPath)) || real == MCRealPath(Utf8([self scratchTexPath])))
        return nil;
    // tectonic writes the kernel's spelling (/private/tmp); the tab and the
    // tree have the document folder's (/tmp).
    const std::string realDir = MCRealPath(Utf8(dir)) + "/";
    if (dir.length && real.compare(0, realDir.size(), realDir) == 0)
        return [dir stringByAppendingPathComponent:NsStr(real.substr(realDir.size()))];
    return path;
}

- (NSArray<NSString *> *)inputFiles {
    NSMutableOrderedSet<NSString *> *files = [NSMutableOrderedSet orderedSet];
    if (!_synctex.valid()) return @[];
    for (const auto &kv : _synctex.inputs())
        if (NSString *f = [self inputFileForTag:kv.first]) [files addObject:f];
    return files.array;
}

// The \input file's source, parsed, from its tab's buffer or the disk;
// nullptr when it cannot be read. Parsed again only when its text changed.
// An \input file's text as the user has it: its tab's buffer, else the disk.
- (NSString *)textOfInputFile:(NSString *)path {
    NSString *text = self.inputText ? self.inputText(path) : nil;
    if (!text) text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding
                                                   error:nil];
    return text;
}

- (const LatexDoc *)docOfInputFile:(NSString *)path source:(std::string *)srcOut {
    NSString *text = [self textOfInputFile:path];
    if (!text) return nullptr;
    const std::string src = Utf8(text);
    auto &slot = _inputDocs[Utf8(path)];
    if (slot.first != src || slot.second.spans.empty()) {
        slot.first = src;
        slot.second = LatexDoc::parseInput(src);
    }
    if (srcOut) *srcOut = src;
    return &slot.second;
}

- (void)refreshStatus {
    if (self.pdf.document && !self.pdf.isHidden && !self.compiling) [self showPagesStatus];
}

// --------------------------------------------------------------- problems

- (void)setProblems:(NSArray<MCLatexProblem *> *)problems fromSource:(const std::string &)src {
    const BOOL changed = !(problems.count == 0 && self.problems.count == 0);
    self.problems = problems ?: @[];
    _problemsSrc = src;
    if (changed && self.onProblemsChanged) self.onProblemsChanged();
}

- (NSArray<MCLatexProblem *> *)problemsForPath:(NSString *)path source:(NSString *)source {
    if (!path.length || self.problems.count == 0 || Utf8(source ?: @"") != _problemsSrc) return @[];
    NSMutableArray *out = [NSMutableArray array];
    for (MCLatexProblem *p in self.problems)
        if (p.line > 0 && [p.path isEqualToString:path]) [out addObject:p];
    return out;
}

- (BOOL)textView:(NSTextView *)textView clickedOnLink:(id)link atIndex:(NSUInteger)charIndex {
    NSString *text = [link isKindOfClass:[NSURL class]] ? [link absoluteString] : link;
    if (![text isKindOfClass:[NSString class]] || ![text hasPrefix:kProblemLink]) return NO;
    const NSInteger k = [text substringFromIndex:kProblemLink.length].integerValue;
    if (k < 0 || (NSUInteger)k >= self.problems.count) return YES;
    MCLatexProblem *p = self.problems[(NSUInteger)k];
    if (p.path && p.line > 0 && self.onOpenProblem) self.onOpenProblem(p.path, p.line);
    return YES;
}

// ------------------------------------------------------------------ export

- (void)keepPdf:(NSData *)data source:(const std::string &)src {
    _pdfData = data;
    _pdfSrc = src;
}

- (void)pdfForPath:(NSString *)texPath source:(NSString *)source
        completion:(void (^)(NSData *pdf, NSString *error))done {
    [self setPath:texPath source:source];   // no-op when nothing changed
    if (_pdfData && _pdfSrc == _src && !self.compiling) {
        done(_pdfData, nil);
        return;
    }
    if (!MCTectonicPath()) {
        done(nil, @"tectonic is not installed. The LaTeX preview offers to "
                  "download it.");
        return;
    }
    if (!_pdfWaiters) _pdfWaiters = [NSMutableArray array];
    [_pdfWaiters addObject:[done copy]];
    [self compileNow];   // skip the typing debounce; queues if one is running
}

// Called when a typeset finishes with nothing queued after it. The source
// can still have moved on (typing during the run), in which case the waiters
// wait for the typeset that change scheduled.
- (void)answerPdfWaiters:(NSString *)error {
    if (_pdfWaiters.count == 0) return;
    if (!error && _pdfSrc != _src) { [self compileNow]; return; }
    NSArray *waiters = _pdfWaiters;
    _pdfWaiters = nil;
    for (void (^done)(NSData *, NSString *) in waiters)
        done(error ? nil : _pdfData, error);
}

- (void)showFailure:(NSString *)log {
    [self showFailure:log problems:nil];
}

// The log, under the problems it holds: a row each, its place a link to the
// line (onOpenProblem) when it has one.
// A row per problem, in its severity's color (errors red, warnings
// yellow), its place a link to the line (onOpenProblem) when it has one.
// The link's number is the problem's index in `problems`, which must be
// self.problems.
- (NSMutableAttributedString *)problemRows:(NSArray<MCLatexProblem *> *)problems {
    NSMutableAttributedString *page = [NSMutableAttributedString new];
    NSString *texPath = self.texPath;
    [problems enumerateObjectsUsingBlock:^(MCLatexProblem *p, NSUInteger i, BOOL *) {
        NSString *place = nil;
        NSString *row = MCDescribeProblem(p, texPath, &place);
        NSDictionary *attrs = @{NSFontAttributeName: [NSFont systemFontOfSize:13],
                                NSForegroundColorAttributeName: MCDiagnosticColor(p.severity)};
        NSMutableAttributedString *r =
            [[NSMutableAttributedString alloc] initWithString:[row stringByAppendingString:@"\n"]
                                                   attributes:attrs];
        if (p.path && p.line > 0 && place.length)
            [r addAttribute:NSLinkAttributeName
                      value:[kProblemLink stringByAppendingFormat:@"%lu", (unsigned long)i]
                      range:NSMakeRange(0, place.length)];
        [page appendAttributedString:r];
    }];
    return page;
}

- (void)showFailure:(NSString *)log problems:(NSArray<MCLatexProblem *> *)problems {
    AppSettings *cfg = [AppSettings shared];
    NSColor *ink = [cfg text:Surface::Editor];
    NSDictionary *plain = @{NSFontAttributeName: [NSFont systemFontOfSize:13],
                            NSForegroundColorAttributeName: ink};
    NSString *texPath = self.texPath;
    NSMutableAttributedString *page = [self problemRows:problems ?: @[]];
    _showingWarnings = NO;
    self.warningsButton.hidden = YES;
    if (problems.count)
        [page appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"
                                                                     attributes:plain]];
    [page appendAttributedString:[[NSAttributedString alloc] initWithString:log ?: @""
        attributes:@{NSFontAttributeName: MCMonoFont(12, NSFontWeightRegular),
                     NSForegroundColorAttributeName: ink}]];
    [self.logView.textStorage setAttributedString:page];
    self.logScroll.hidden = NO;
    self.pdf.hidden = YES;
    MCLatexProblem *first = problems.firstObject;
    if (first.severity != 1) first = nil;   // a warning does not say why it failed
    [self setStatus:first ? [@"Did not typeset: " stringByAppendingString:
                                 MCDescribeProblem(first, texPath, nullptr)]
                          : @"Did not typeset — see the log"
               busy:NO];
    self.actionButton.title = @"Recompile";
}

- (void)showTectonicMissing {
    _showingWarnings = NO;
    self.warningsButton.hidden = YES;
    self.logView.string =
        @"The LaTeX preview needs tectonic, a single-binary TeX engine.\n\n"
        @"MiniCode uses whichever tectonic is already on your machine "
        @"(brew install tectonic), or it can download the official build for "
        @"you, about 22 MB, into ~/Library/Application Support/MiniCode.\n\n"
        @"The first document you typeset also downloads the LaTeX packages it "
        @"uses, roughly 40 MB, which tectonic caches for later.";
    self.logScroll.hidden = NO;
    self.pdf.hidden = YES;
    [self setStatus:@"tectonic was not found" busy:NO];
    self.actionButton.title = @"Download…";
}

- (void)setStatus:(NSString *)text busy:(BOOL)busy {
    self.statusLabel.stringValue = text ?: @"";
    if (busy) [self.spinner startAnimation:nil];
    else [self.spinner stopAnimation:nil];
    self.spinner.hidden = !busy;
    [self relayout];
}

- (void)actionPressed:(id)sender {
    if (MCTectonicPath()) [self compileNow];
    else [self downloadTectonic];
}

// ------------------------------------------------------------- downloading

- (void)downloadTectonic {
    [self setStatus:@"Downloading tectonic…" busy:YES];
    self.actionButton.enabled = NO;
    __weak LatexView *weakSelf = self;
    MCDownloadTectonic(^(NSString *problem) {
        LatexView *me = weakSelf;
        if (!me) return;
        me.actionButton.enabled = YES;
        if (problem) {
            [me showFailure:[NSString stringWithFormat:
                @"Could not install tectonic.\n\n%@\n\nYou can also install "
                @"it yourself with:\n\n    brew install tectonic", problem]];
            me.actionButton.title = @"Download…";
        } else {
            [me compileNow];
        }
    });
}

// ------------------------------------------------------- click -> source

// TeX hyphenates at the end of a line, so the PDF holds "counterrevolution-"
// and "aries" as two words where the source has one. When the clicked word
// sits against a line-end hyphen, take the other half from the text around it
// (and out of that text), so the whole word is what gets matched.
static void MCJoinHyphenation(NSString **word, NSString **before, NSString **after) {
    NSCharacterSet *breaks = [NSCharacterSet newlineCharacterSet];
    NSCharacterSet *letters = [NSCharacterSet letterCharacterSet];
    NSArray<NSString *> *hyphens = @[@"-", @"\u2010", @"\u00AD"];
    // "-\n" right after the word: the rest of it starts the next line.
    NSString *a = *after;
    for (NSString *h in hyphens) {
        if (![a hasPrefix:h] || a.length <= h.length) continue;
        NSUInteger k = h.length;
        if (![breaks characterIsMember:[a characterAtIndex:k]]) continue;
        while (k < a.length && [breaks characterIsMember:[a characterAtIndex:k]]) k++;
        NSUInteger e = k;
        while (e < a.length && [letters characterIsMember:[a characterAtIndex:e]]) e++;
        if (e > k) {
            *word = [*word stringByAppendingString:[a substringWithRange:NSMakeRange(k, e - k)]];
            *after = [a substringFromIndex:e];
        }
        break;
    }
    // "-\n" right before it: the word began at the end of the previous line.
    NSString *b = *before;
    NSUInteger k = b.length;
    while (k > 0 && [breaks characterIsMember:[b characterAtIndex:k - 1]]) k--;
    if (k == b.length || k == 0) return;
    for (NSString *h in hyphens) {
        if (k < h.length || ![[b substringWithRange:NSMakeRange(k - h.length, h.length)]
                                 isEqualToString:h])
            continue;
        NSUInteger e = k - h.length, s = e;
        while (s > 0 && [letters characterIsMember:[b characterAtIndex:s - 1]]) s--;
        if (e > s) {
            *word = [[b substringWithRange:NSMakeRange(s, e - s)] stringByAppendingString:*word];
            *before = [b substringToIndex:s];
        }
        break;
    }
}

// The word under `pagePoint` and the page's text on either side of it (40
// characters, a hyphenated word joined), or nil when the PDF gives no word
// there that has letters or digits.
static NSString *MCLatexClickedWord(PDFPage *page, NSPoint pagePoint, NSString **before,
                                    NSString **after) {
    PDFSelection *word = [page selectionForWordAtPoint:pagePoint];
    NSString *clicked = word.string ?: @"";
    *before = @"";
    *after = @"";
    if (LatexDoc::matchKey(Utf8(clicked)).empty()) return nil;
    // The text on either side tells a common word's occurrences apart: which
    // "the" was clicked is settled by the words around it.
    PDFSelection *wide = [word copy];
    [wide extendSelectionAtStart:40];
    if ([wide.string hasSuffix:clicked])
        *before = [wide.string substringToIndex:wide.string.length - clicked.length];
    wide = [word copy];
    [wide extendSelectionAtEnd:40];
    if ([wide.string hasPrefix:clicked])
        *after = [wide.string substringFromIndex:clicked.length];
    MCJoinHyphenation(&clicked, before, after);
    return clicked;
}

const LatexSpan *MCLatexSpanAtPoint(const LatexDoc &doc, const SyncTexIndex &sync,
                                    int tag, PDFPage *page, int pageNumber,
                                    NSPoint pagePoint,
                                    std::vector<SyncTexHit> *hitsOut) {
    NSRect bounds = [page boundsForBox:kPDFDisplayBoxMediaBox];
    double x = pagePoint.x;
    double y = bounds.size.height - pagePoint.y;   // SyncTeX measures from the top

    std::vector<SyncTexHit> hits = sync.textHitsAtPoint(pageNumber, x, y, 8);
    std::vector<int> lines;
    for (const SyncTexHit &h : hits)
        if (tag == 0 || h.tag == tag) lines.push_back(h.line);
    if (hitsOut) *hitsOut = hits;

    // The word under the pointer names what was clicked. When the PDF cannot
    // give one (a glyph from a picture font, a logo, a bullet), fall back to
    // the whole line, which the matcher can still place.
    NSString *before = @"", *after = @"";
    NSString *clicked = MCLatexClickedWord(page, pagePoint, &before, &after);
    if (!clicked) {
        clicked = [page selectionForLineAtPoint:pagePoint].string ?: @"";
        return doc.spanForClick(lines, Utf8(clicked));
    }
    return doc.spanForClick(lines, Utf8(clicked), Utf8(before), Utf8(after));
}

const LatexSpan *MCLatexSpanAtPointInFiles(
    const SyncTexIndex &sync, int mainTag, PDFPage *page, int pageNumber, NSPoint pagePoint,
    const std::function<const LatexDoc *(int tag)> &docFor, int *tagOut,
    std::vector<SyncTexHit> *hitsOut) {
    const NSRect bounds = [page boundsForBox:kPDFDisplayBoxMediaBox];
    const std::vector<int> tags =
        sync.tagsAtPoint(pageNumber, pagePoint.x, bounds.size.height - pagePoint.y, mainTag);
    bool first = true;
    for (int tag : tags) {
        const LatexDoc *doc = docFor(tag);
        if (!doc) continue;
        std::vector<SyncTexHit> hits;
        const LatexSpan *span =
            MCLatexSpanAtPoint(*doc, sync, tag, page, pageNumber, pagePoint, &hits);
        if (hitsOut && (first || span)) *hitsOut = hits;
        first = false;
        if (span) {
            if (tagOut) *tagOut = tag;
            return span;
        }
    }
    if (tagOut) *tagOut = 0;
    return nullptr;
}

- (void)openEditorForPage:(PDFPage *)page point:(NSPoint)pagePoint {
    if (!_synctex.valid()) {
        [self setStatus:@"No SyncTeX data, so the preview cannot be edited"
                   busy:NO];
        return;
    }
    NSRect bounds = [page boundsForBox:kPDFDisplayBoxMediaBox];
    int pageNumber = (int)[self.pdf.document indexForPage:page] + 1;
    const int mainTag = _synctex.tagForPath(Utf8([self scratchTexPath]));
    std::vector<SyncTexHit> hits;
    // The document's own text, or an \input file's: whichever the boxes
    // under the point name first that can place the click.
    std::map<int, std::pair<NSString *, std::string>> files;
    int tag = 0;
    const LatexSpan *span = MCLatexSpanAtPointInFiles(
        _synctex, mainTag, page, pageNumber, pagePoint,
        [&](int t) -> const LatexDoc * {
            if (t == mainTag) return &_doc;
            NSString *file = [self inputFileForTag:t];
            if (!file) return nullptr;
            std::string src;
            const LatexDoc *d = [self docOfInputFile:file source:&src];
            if (d) files[t] = {file, src};
            return d;
        },
        &tag, &hits);
    if (span && tag != mainTag) {
        auto it = files.find(tag);
        if (it == files.end()) span = nullptr;
        else {
            _editingFile = it->second.first;
            _editingSrc = it->second.second;
        }
    } else {
        _editingFile = nil;
        _editingSrc.clear();
    }
    if (!span) {
        [self setStatus:@"That is not text MiniCode can trace back to the source"
                   busy:NO];
        return;
    }

    // Anchor the popover on the box the click landed in.
    NSRect anchor = NSMakeRect(pagePoint.x - 2, pagePoint.y - 2, 4, 4);
    if (!hits.empty()) {
        const SyncTexHit &h = hits.front();
        anchor = NSMakeRect(h.x, bounds.size.height - h.y - h.height,
                            MAX(h.width, 4), MAX(h.height, 4));
    }
    NSRect inView = [self.pdf convertRect:[self.pdf convertRect:anchor fromPage:page]
                                   toView:self];
    // The box opens on the word that was clicked, found in the span's
    // LaTeX by the text around it, never the whole span preselected. The
    // span's offsets are in its own file's text: an \input file's, or the
    // document's.
    NSString *before = @"", *after = @"";
    NSString *clicked = MCLatexClickedWord(page, pagePoint, &before, &after);
    NSRange selection = NSMakeRange(NSNotFound, 0);
    const std::string &spanSrc = _editingFile ? _editingSrc : _src;
    if (clicked && span->start <= span->end && span->end <= spanSrc.size()) {
        const std::string body = spanSrc.substr(span->start, span->end - span->start);
        const auto at = MarkdownEdit::sourceOffsetFor(body, Utf8(clicked), Utf8(before), Utf8(after));
        if (at.first != std::string::npos) {
            NSString *head = [[NSString alloc] initWithBytes:body.data() length:at.first
                                                    encoding:NSUTF8StringEncoding];
            NSString *hit = [[NSString alloc] initWithBytes:body.data() + at.first
                                                     length:at.second - at.first
                                                   encoding:NSUTF8StringEncoding];
            if (head && hit) selection = NSMakeRange(head.length, hit.length);
        }
    }
    [self showPopoverForSpan:*span anchor:inView selection:selection];
}

// ------------------------------------------------------------ editing popover

- (NSString *)describeSpan:(const LatexSpan &)span {
    switch (span.kind) {
        case LatexSpanKind::Field:
            return [NSString stringWithFormat:@"\\%@", NsStr(span.command)];
        case LatexSpanKind::Math:
            return @"math";
        case LatexSpanKind::Text:
            return span.itemIndex >= 0 ? @"list item" : @"text";
    }
    return @"text";
}

- (void)showPopoverForSpan:(const LatexSpan &)span anchor:(NSRect)rect {
    [self showPopoverForSpan:span anchor:rect selection:NSMakeRange(NSNotFound, 0)];
}

- (void)showPopoverForSpan:(const LatexSpan &)span anchor:(NSRect)rect selection:(NSRange)selection {
    _editing = span;
    _editingValid = YES;
    _addingItem = NO;
    _editingItemIndex = span.itemIndex;
    // An \input file's span is in that file's text, and its lists.
    const std::string &src = _editingFile ? _editingSrc : _src;
    const LatexDoc *doc = _editingFile ? &_inputDocs[Utf8(_editingFile)].second : &_doc;
    BOOL inList = span.listIndex >= 0 &&
                  (size_t)span.listIndex < doc->lists.size();
    if (inList) _editingList = doc->lists[(size_t)span.listIndex];

    const size_t a = std::min(span.start, src.size());
    NSString *body = NsStr(src.substr(a, std::min(span.end, src.size()) - a));
    NSString *title = [NSString stringWithFormat:@"Editing %@", [self describeSpan:span]];
    if (_editingFile)
        title = [title stringByAppendingFormat:@" in %@", _editingFile.lastPathComponent];
    [self presentPopoverWithTitle:title
                             text:body
                       allowsItem:inList
                           anchor:rect
                        selection:selection];
}

- (void)presentPopoverWithTitle:(NSString *)title
                           text:(NSString *)text
                     allowsItem:(BOOL)allowsItem
                         anchor:(NSRect)rect
                      selection:(NSRange)selection {
    NSView *content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 360, 150)];

    NSTextField *label = [NSTextField labelWithString:title];
    label.font = [NSFont systemFontOfSize:11];
    label.textColor = [NSColor secondaryLabelColor];
    label.frame = NSMakeRect(12, 124, 336, 14);
    [content addSubview:label];

    NSScrollView *scroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(12, 44, 336, 74)];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    MCEditText *editor = [[MCEditText alloc]
        initWithFrame:NSMakeRect(0, 0, 336, 74)];
    editor.font = MCMonoFont(MCZoomed(12), NSFontWeightRegular);
    editor.string = text ?: @"";
    editor.automaticQuoteSubstitutionEnabled = NO;
    editor.automaticDashSubstitutionEnabled = NO;
    editor.automaticTextReplacementEnabled = NO;
    editor.allowsUndo = YES;
    scroll.documentView = editor;
    [content addSubview:scroll];
    self.editText = editor;

    __weak LatexView *weakSelf = self;
    editor.onCommit = ^{ [weakSelf commitEdit:nil]; };
    editor.onCancel = ^{ [weakSelf.popover performClose:nil]; };

    NSButton *save = [NSButton buttonWithTitle:@"Save"
                                        target:self
                                        action:@selector(commitEdit:)];
    save.bezelStyle = NSBezelStyleRounded;
    save.keyEquivalent = @"\r";
    save.frame = NSMakeRect(268, 10, 80, 26);
    [content addSubview:save];

    NSButton *cancel = [NSButton buttonWithTitle:@"Cancel"
                                          target:self
                                          action:@selector(cancelEdit:)];
    cancel.bezelStyle = NSBezelStyleRounded;
    cancel.frame = NSMakeRect(184, 10, 80, 26);
    [content addSubview:cancel];

    if (allowsItem) {
        NSButton *add = [NSButton buttonWithTitle:@"Add item"
                                           target:self
                                           action:@selector(startAddItem:)];
        add.bezelStyle = NSBezelStyleRounded;
        add.font = [NSFont systemFontOfSize:11];
        add.frame = NSMakeRect(12, 10, 96, 26);
        add.toolTip = @"Add a new entry to this list, below the one you clicked";
        [content addSubview:add];
        self.addItemButton = add;
    } else {
        self.addItemButton = nil;
    }

    NSViewController *vc = [[NSViewController alloc] init];
    vc.view = content;

    [self.popover performClose:nil];
    self.popover = [[NSPopover alloc] init];
    self.popover.contentViewController = vc;
    self.popover.behavior = NSPopoverBehaviorTransient;
    self.popover.delegate = self;
    [self.popover showRelativeToRect:rect ofView:self
                       preferredEdge:NSRectEdgeMaxY];
    [self.window makeFirstResponder:editor];
    // The clicked word, or the caret at the end: never the whole span,
    // which one key would replace.
    if (selection.location != NSNotFound && NSMaxRange(selection) <= editor.string.length)
        [editor setSelectedRange:selection];
    else
        [editor setSelectedRange:NSMakeRange(editor.string.length, 0)];
    [editor scrollRangeToVisible:editor.selectedRange];
}

- (void)cancelEdit:(id)sender {
    [self.popover performClose:nil];
}

- (void)startAddItem:(id)sender {
    if (!_editingValid) return;
    _addingItem = YES;
    NSRect rect = self.popover.positioningRect;
    [self presentPopoverWithTitle:@"New list entry"
                             text:@""
                       allowsItem:NO
                           anchor:rect
                        selection:NSMakeRange(NSNotFound, 0)];
}

- (void)commitEdit:(id)sender {
    if (!_editingValid || !self.editText) return;
    std::string text = Utf8(self.editText.string ?: @"");
    BOOL adding = _addingItem;
    [self.popover performClose:nil];
    _editingValid = NO;

    if (_editingFile) {
        [self commitInputEdit:text adding:adding];
        return;
    }
    if (adding) {
        if (text.empty()) return;
        LatexDoc::Edit edit =
            LatexDoc::addItem(_src, _editingList, _editingItemIndex, text);
        [self applyEdit:edit];
        [self setStatus:@"Added a list entry" busy:YES];
        return;
    }
    std::string current = _src.substr(_editing.start, _editing.end - _editing.start);
    if (text == current) return;
    LatexDoc::Edit edit = LatexDoc::replaceSpan(_src, _editing, text);
    [self applyEdit:edit];
}

// The popover's edit of an \input file: a splice of that file's text, put
// into its tab by the controller (opened in the background when it was not
// open), unsaved, one undo step there. Refused when the file's text
// changed while the box was open.
- (void)commitInputEdit:(const std::string &)text adding:(BOOL)adding {
    NSString *file = _editingFile;
    NSString *name = file.lastPathComponent;
    NSString *nowText = [self textOfInputFile:file];
    const std::string now = Utf8(nowText ?: @"");
    if (!nowText || now != _editingSrc) {
        [self setStatus:[NSString stringWithFormat:@"%@ changed while the box was open, "
                                                   @"so the edit was not made", name]
                   busy:NO];
        return;
    }
    LatexDoc::Edit edit;
    if (adding) {
        if (text.empty()) return;
        edit = LatexDoc::addItem(now, _editingList, _editingItemIndex, text);
    } else {
        const size_t a = std::min(_editing.start, now.size());
        if (now.substr(a, std::min(_editing.end, now.size()) - a) == text) return;
        edit = LatexDoc::replaceSpan(now, _editing, text);
    }
    // The splice as a range of the old text (UTF-16) and what replaces it:
    // the common ends of the two texts, kept off a character's middle.
    const std::string &after = edit.source;
    size_t head = 0;
    while (head < now.size() && head < after.size() && now[head] == after[head]) head++;
    size_t tail = 0;
    while (tail < now.size() - head && tail < after.size() - head &&
           now[now.size() - 1 - tail] == after[after.size() - 1 - tail])
        tail++;
    auto continuation = [](char c) { return ((unsigned char)c & 0xC0) == 0x80; };
    while (head > 0 && head < now.size() && continuation(now[head])) head--;
    while (tail > 0 && continuation(now[now.size() - tail])) tail--;
    NSString *before = NsStr(now.substr(0, head));
    NSString *removed = NsStr(now.substr(head, now.size() - tail - head));
    NSString *inserted = NsStr(after.substr(head, after.size() - tail - head));
    const NSRange range = NSMakeRange(before.length, removed.length);
    if (!self.onInputEdited || !self.onInputEdited(file, range, inserted)) {
        [self setStatus:[NSString stringWithFormat:@"%@ could not be changed", name] busy:NO];
        return;
    }
    [self showPagesStatus];
}

- (void)popoverDidClose:(NSNotification *)note {
    self.editText = nil;
}

@end

// The download, shared by the LaTeX preview and the Markdown preview's math.
static NSMutableArray<void (^)(NSString *)> *gTectonicWaiters;

BOOL MCTectonicDownloading(void) {
    return gTectonicWaiters != nil;
}

// Puts the pinned archive at `archive` in place: checked against its
// published SHA-256 first, unpacked into a folder of its own beside the
// destination, and only its one `tectonic` file, a plain file, moved into
// place. Returns why it failed, or nil. Any thread.
static NSString *InstallTectonicArchive(NSString *archive, NSString *dest) {
    NSString *sha = FileSha256Hex(archive, nullptr);
    if (!sha) return @"The download could not be read.";
    if (![sha isEqualToString:kTectonicSha256])
        return [NSString stringWithFormat:@"The download did not match tectonic %@'s "
                                          @"published checksum, so it was not installed.",
                                          kTectonicVersion];
    NSFileManager *fm = [NSFileManager defaultManager];
    [fm createDirectoryAtPath:dest withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *tmpl = [dest stringByAppendingPathComponent:@".tectonic-install-XXXXXX"];
    char buf[PATH_MAX];
    strlcpy(buf, tmpl.fileSystemRepresentation, sizeof buf);
    if (!mkdtemp(buf)) return @"The download could not be unpacked.";
    NSString *staging = @(buf);
    NSString *problem = nil;
    NSTask *untar = [[NSTask alloc] init];
    untar.executableURL = [NSURL fileURLWithPath:@"/usr/bin/tar"];
    untar.arguments = @[@"-xzf", archive, @"-C", staging, @"tectonic"];
    untar.standardOutput = [NSFileHandle fileHandleWithNullDevice];
    untar.standardError = [NSFileHandle fileHandleWithNullDevice];
    NSError *tarErr = nil;
    if ([untar launchAndReturnError:&tarErr]) [untar waitUntilExit];
    else problem = tarErr.localizedDescription;
    NSString *unpacked = [staging stringByAppendingPathComponent:@"tectonic"];
    struct stat st;
    if (!problem && (untar.terminationStatus != 0 ||
                     lstat(unpacked.fileSystemRepresentation, &st) != 0 || !S_ISREG(st.st_mode)))
        problem = @"The download did not contain tectonic.";
    if (!problem) {
        chmod(unpacked.fileSystemRepresentation, 0755);
        if (rename(unpacked.fileSystemRepresentation,
                   MCManagedTectonic().fileSystemRepresentation) != 0)
            problem = @(strerror(errno));
    }
    [fm removeItemAtPath:staging error:nil];
    if (!problem && ![fm isExecutableFileAtPath:MCManagedTectonic()])
        problem = @"The download did not contain tectonic.";
    return problem;
}

void MCDownloadTectonic(void (^done)(NSString *problem)) {
    if (gTectonicWaiters) {   // one is under way: wait for it
        [gTectonicWaiters addObject:[done copy]];
        return;
    }
    NSString *file = [NSString stringWithFormat:@"tectonic-%@-%@.tar.gz",
                      kTectonicVersion, kTectonicArch];
    NSString *urlText = [NSString stringWithFormat:
        @"https://github.com/tectonic-typesetting/tectonic/releases/download/"
        @"tectonic%%40%@/%@", kTectonicVersion, file];
    // The GUI checks fetch a file:// stand-in, which is held to the same
    // checksum.
    NSString *override = NSProcessInfo.processInfo.environment[@"MINICODE_TECTONIC_DOWNLOAD"];
    if (override.length) urlText = override;
    NSURL *url = [NSURL URLWithString:urlText];
    if (!url) {
        done(@"The download address is not valid.");
        return;
    }
    gTectonicWaiters = [NSMutableArray arrayWithObject:[done copy]];
    NSURLSessionDownloadTask *task = [NSURLSession.sharedSession
        downloadTaskWithURL:url
          completionHandler:^(NSURL *tmp, NSURLResponse *response, NSError *error) {
        NSString *problem = error.localizedDescription;
        if (!problem) {
            NSHTTPURLResponse *http = (NSHTTPURLResponse *)response;
            if ([http isKindOfClass:[NSHTTPURLResponse class]] &&
                http.statusCode != 200)
                problem = [NSString stringWithFormat:@"The download failed (HTTP %ld).",
                           (long)http.statusCode];
        }
        if (!problem && !tmp.path) problem = @"The download did not arrive.";
        // The session removes `tmp` when this returns, so it is read here.
        if (!problem)
            problem = InstallTectonicArchive(tmp.path,
                [MCSupportDir() stringByAppendingPathComponent:@"bin"]);
        dispatch_async(dispatch_get_main_queue(), ^{
            NSArray<void (^)(NSString *)> *waiters = gTectonicWaiters;
            gTectonicWaiters = nil;
            // Its LaTeX files fetched now, not when a page first needs them.
            if (!problem) MCMathWarmUp();
            for (void (^w)(NSString *) in waiters) w(problem);
        });
    }];
    [task resume];
}
