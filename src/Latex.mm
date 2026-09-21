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
#import "Latex.h"
#import "AppSettings.h"
#import <Quartz/Quartz.h>
#include "LatexDoc.h"
#include "SyncTex.h"
#include <zlib.h>
#include <string>

// tectonic is pinned so the download is reproducible; any tectonic already on
// the machine is preferred over ours.
static NSString *const kTectonicVersion = @"0.17.0";
#if defined(__aarch64__)
static NSString *const kTectonicArch = @"aarch64-apple-darwin";
#else
static NSString *const kTectonicArch = @"x86_64-apple-darwin";
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

@interface LatexView () <NSPopoverDelegate>
@property(nonatomic, strong) MCPdfView *pdf;
@property(nonatomic, strong) NSView *statusBar;
@property(nonatomic, strong) NSTextField *statusLabel;
@property(nonatomic, strong) NSButton *actionButton;
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
    self.logView.font = [NSFont monospacedSystemFontOfSize:12
                                                    weight:NSFontWeightRegular];
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

    [self applySettings];
    [self relayout];
    [self compileNow];
    return self;
}

- (void)dealloc {
    [_task terminate];
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
    self.spinner.frame = NSMakeRect(pad, 6, 14, 14);
    CGFloat labelX = pad + (self.spinner.isHidden ? 0 : 20);
    self.statusLabel.frame =
        NSMakeRect(labelX, 5, MAX(0, b.size.width - labelX - 112), 16);
}

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

- (NSString *)workDir {
    NSString *dir = [NSTemporaryDirectory()
        stringByAppendingPathComponent:@"MiniCode-latex"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    return dir;
}

// The hidden sibling we actually hand to tectonic, so relative paths in the
// document still resolve while the user's own file stays untouched.
- (NSString *)scratchTexPath {
    NSString *dir = self.texPath.stringByDeletingLastPathComponent;
    NSString *base = self.texPath.lastPathComponent.stringByDeletingPathExtension;
    if (!dir.length || !base.length) return [[self workDir]
        stringByAppendingPathComponent:@"untitled.minicode.tex"];
    return [dir stringByAppendingPathComponent:
        [NSString stringWithFormat:@".%@.minicode.tex", base]];
}

- (void)compileNow {
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(compileNow)
                                               object:nil];
    NSString *tool = MCTectonicPath();
    if (!tool) { [self showTectonicMissing]; return; }
    if (self.compiling) { self.compileQueued = YES; return; }

    NSString *scratch = [self scratchTexPath];
    NSString *outDir = [self workDir];
    NSString *stem = scratch.lastPathComponent.stringByDeletingPathExtension;
    NSError *err = nil;
    if (![NsStr(_src) writeToFile:scratch atomically:YES
                         encoding:NSUTF8StringEncoding error:&err]) {
        [self showFailure:[NSString stringWithFormat:@"Cannot write the preview "
                           @"copy next to your file:\n%@", err.localizedDescription]];
        return;
    }

    self.compiling = YES;
    self.compileQueued = NO;
    NSUInteger gen = ++self.generation;
    [self setStatus:@"Typesetting…" busy:YES];

    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:tool];
    task.arguments = @[@"--synctex", @"--chatter", @"minimal", @"--color", @"never",
                       @"--outdir", outDir, scratch];
    NSString *cwd = self.texPath.stringByDeletingLastPathComponent;
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
        if ([task launchAndReturnError:&runErr]) {
            output = [pipe.fileHandleForReading readDataToEndOfFile];
            [task waitUntilExit];
            status = task.terminationStatus;
        }
        NSString *log = output.length
            ? [[NSString alloc] initWithData:output encoding:NSUTF8StringEncoding]
            : (runErr.localizedDescription ?: @"");
        NSString *pdfPath = [outDir stringByAppendingPathComponent:
            [stem stringByAppendingPathExtension:@"pdf"]];
        NSString *syncPath = [outDir stringByAppendingPathComponent:
            [stem stringByAppendingString:@".synctex.gz"]];
        PDFDocument *doc = (status == 0)
            ? [[PDFDocument alloc] initWithURL:[NSURL fileURLWithPath:pdfPath]]
            : nil;
        std::string sync = (status == 0) ? ReadGzip(syncPath) : std::string();
        [[NSFileManager defaultManager] removeItemAtPath:scratch error:nil];

        dispatch_async(dispatch_get_main_queue(), ^{
            LatexView *me = weakSelf;
            if (!me || gen != me.generation) return;
            me.compiling = NO;
            me.task = nil;
            if (status == 0 && doc) [me showDocument:doc syncTex:sync];
            else [me showFailure:log.length ? log : @"tectonic did not run."];
            if (me.compileQueued) [me compileNow];
        });
    });
}

- (void)showDocument:(PDFDocument *)doc syncTex:(const std::string &)sync {
    _synctex = SyncTexIndex::parse(sync);

    // Keep the reader where they were: same page, same scroll offset.
    PDFDestination *was = self.pdf.currentDestination;
    NSUInteger pageIndex = was ? [self.pdf.document indexForPage:was.page] : 0;
    CGFloat scale = self.pdf.scaleFactor;
    BOOL hadDoc = self.pdf.document != nil;

    self.pdf.document = doc;
    if (hadDoc && pageIndex < doc.pageCount) {
        PDFPage *page = [doc pageAtIndex:pageIndex];
        PDFDestination *dest = [[PDFDestination alloc] initWithPage:page
                                                            atPoint:was.point];
        [self.pdf goToDestination:dest];
        self.pdf.scaleFactor = scale;
    }
    self.logScroll.hidden = YES;
    self.pdf.hidden = NO;
    NSString *pages = doc.pageCount == 1 ? @"1 page"
        : [NSString stringWithFormat:@"%lu pages", (unsigned long)doc.pageCount];
    [self setStatus:[NSString stringWithFormat:
        @"%@ · double-click text to edit it", pages] busy:NO];
    self.actionButton.title = @"Recompile";
}

- (void)showFailure:(NSString *)log {
    self.logView.string = log;
    self.logScroll.hidden = NO;
    self.pdf.hidden = YES;
    [self setStatus:@"Did not typeset — see the log" busy:NO];
    self.actionButton.title = @"Recompile";
}

- (void)showTectonicMissing {
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
    NSString *file = [NSString stringWithFormat:@"tectonic-%@-%@.tar.gz",
                      kTectonicVersion, kTectonicArch];
    NSString *urlText = [NSString stringWithFormat:
        @"https://github.com/tectonic-typesetting/tectonic/releases/download/"
        @"tectonic%%40%@/%@", kTectonicVersion, file];
    NSURL *url = [NSURL URLWithString:urlText];
    if (!url) return;

    [self setStatus:@"Downloading tectonic…" busy:YES];
    self.actionButton.enabled = NO;
    __weak LatexView *weakSelf = self;
    NSURLSessionDownloadTask *task = [NSURLSession.sharedSession
        downloadTaskWithURL:url
          completionHandler:^(NSURL *tmp, NSURLResponse *response, NSError *error) {
        NSString *problem = error.localizedDescription;
        NSString *dest = [MCSupportDir() stringByAppendingPathComponent:@"bin"];
        if (!problem) {
            NSHTTPURLResponse *http = (NSHTTPURLResponse *)response;
            if ([http isKindOfClass:[NSHTTPURLResponse class]] &&
                http.statusCode != 200)
                problem = [NSString stringWithFormat:@"The download failed (HTTP %ld).",
                           (long)http.statusCode];
        }
        if (!problem) {
            NSFileManager *fm = [NSFileManager defaultManager];
            [fm createDirectoryAtPath:dest withIntermediateDirectories:YES
                           attributes:nil error:nil];
            NSString *archive = [dest stringByAppendingPathComponent:file];
            [fm removeItemAtPath:archive error:nil];
            NSError *moveErr = nil;
            if (![fm moveItemAtURL:tmp toURL:[NSURL fileURLWithPath:archive]
                             error:&moveErr]) {
                problem = moveErr.localizedDescription;
            } else {
                NSTask *untar = [[NSTask alloc] init];
                untar.executableURL = [NSURL fileURLWithPath:@"/usr/bin/tar"];
                untar.arguments = @[@"-xzf", archive, @"-C", dest];
                NSError *tarErr = nil;
                if ([untar launchAndReturnError:&tarErr]) [untar waitUntilExit];
                else problem = tarErr.localizedDescription;
                [fm removeItemAtPath:archive error:nil];
                if (!problem) {
                    [fm setAttributes:@{NSFilePosixPermissions: @(0755)}
                         ofItemAtPath:MCManagedTectonic() error:nil];
                    if (![fm isExecutableFileAtPath:MCManagedTectonic()])
                        problem = @"The download did not contain tectonic.";
                }
            }
        }
        dispatch_async(dispatch_get_main_queue(), ^{
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
    }];
    [task resume];
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
    PDFSelection *word = [page selectionForWordAtPoint:pagePoint];
    NSString *clicked = word.string ?: @"";
    if (LatexDoc::matchKey(Utf8(clicked)).empty()) {
        clicked = [page selectionForLineAtPoint:pagePoint].string ?: clicked;
        return doc.spanForClick(lines, Utf8(clicked));
    }
    // The text on either side tells a common word's occurrences apart: which
    // "the" was clicked is settled by the words around it.
    NSString *before = @"", *after = @"";
    PDFSelection *wide = [word copy];
    [wide extendSelectionAtStart:40];
    if ([wide.string hasSuffix:clicked])
        before = [wide.string substringToIndex:wide.string.length - clicked.length];
    wide = [word copy];
    [wide extendSelectionAtEnd:40];
    if ([wide.string hasPrefix:clicked])
        after = [wide.string substringFromIndex:clicked.length];
    MCJoinHyphenation(&clicked, &before, &after);
    return doc.spanForClick(lines, Utf8(clicked), Utf8(before), Utf8(after));
}

- (void)openEditorForPage:(PDFPage *)page point:(NSPoint)pagePoint {
    if (!_synctex.valid()) {
        [self setStatus:@"No SyncTeX data, so the preview cannot be edited"
                   busy:NO];
        return;
    }
    NSRect bounds = [page boundsForBox:kPDFDisplayBoxMediaBox];
    int pageNumber = (int)[self.pdf.document indexForPage:page] + 1;
    int tag = _synctex.tagForPath(Utf8([self scratchTexPath]));
    std::vector<SyncTexHit> hits;
    const LatexSpan *span = MCLatexSpanAtPoint(_doc, _synctex, tag, page,
                                               pageNumber, pagePoint, &hits);
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
    [self showPopoverForSpan:*span anchor:inView];
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
    _editing = span;
    _editingValid = YES;
    _addingItem = NO;
    _editingItemIndex = span.itemIndex;
    BOOL inList = span.listIndex >= 0 &&
                  (size_t)span.listIndex < _doc.lists.size();
    if (inList) _editingList = _doc.lists[(size_t)span.listIndex];

    NSString *body = NsStr(_src.substr(span.start, span.end - span.start));
    [self presentPopoverWithTitle:[NSString stringWithFormat:@"Editing %@",
                                   [self describeSpan:span]]
                             text:body
                       allowsItem:inList
                           anchor:rect];
}

- (void)presentPopoverWithTitle:(NSString *)title
                           text:(NSString *)text
                     allowsItem:(BOOL)allowsItem
                         anchor:(NSRect)rect {
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
    editor.font = [NSFont monospacedSystemFontOfSize:12
                                              weight:NSFontWeightRegular];
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
    [editor setSelectedRange:NSMakeRange(0, editor.string.length)];
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
                           anchor:rect];
}

- (void)commitEdit:(id)sender {
    if (!_editingValid || !self.editText) return;
    std::string text = Utf8(self.editText.string ?: @"");
    BOOL adding = _addingItem;
    [self.popover performClose:nil];
    _editingValid = NO;

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

- (void)popoverDidClose:(NSNotification *)note {
    self.editText = nil;
}

@end
