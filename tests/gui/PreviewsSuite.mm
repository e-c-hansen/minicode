// PreviewsSuite.mm — `make guitest SUITES=previews`: previews and editors
// for more kinds of file (FileViewers.h): Quick Look in the editor's slot
// for what minicode has no viewer for, Space in the tree for the Quick Look
// panel, binary files never opened as text, and the acceptance checks of a
// PDF, a .mov, a .docx and a .heic opened from Finder. Every fixture is
// made here, in the scratch project: a PDF with PDFKit, PNG and HEIC with
// ImageIO, a QuickTime movie with AVAssetWriter, a .docx zipped by hand.
#import "Harness.h"
#import "EditorController.h"
#import "FileViewers.h"
#import "QuickLook.h"
#import "CsvTable.h"
#import <AVFoundation/AVFoundation.h>
#import <ImageIO/ImageIO.h>
#import <Quartz/Quartz.h>
#import <objc/runtime.h>
#include <zlib.h>
#include <string>

namespace {

NSString *Fixture(NSString *name) {
    return [MCGuiProject() stringByAppendingPathComponent:[@"previews" stringByAppendingPathComponent:name]];
}

NSDictionary *ActiveTab(EditorController *c) {
    for (NSDictionary *t in [c controlTabs])
        if ([t[@"active"] boolValue]) return t;
    return nil;
}

NSString *ActiveKind(EditorController *c) { return ActiveTab(c)[@"kind"] ?: @"(none)"; }
NSString *ActiveName(EditorController *c) {
    id p = ActiveTab(c)[@"path"];
    return [p isKindOfClass:NSString.class] ? [p lastPathComponent] : @"";
}

MCFileViewers *Viewers(EditorController *c) { return [c valueForKey:@"viewers"]; }
NSTextView *EditorText(EditorController *c) { return [c valueForKey:@"textView"]; }
NSScrollView *EditorScroll(EditorController *c) { return [c valueForKey:@"editorScroll"]; }
NSOutlineView *Tree(EditorController *c) { return [c valueForKey:@"outline"]; }

CGImageRef MakePicture(int w, int h) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(NULL, w, h, 8, 0, cs, kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    CGContextSetRGBFillColor(ctx, 0.2, 0.5, 0.9, 1);
    CGContextFillRect(ctx, CGRectMake(0, 0, w, h));
    CGContextSetRGBFillColor(ctx, 0.95, 0.8, 0.2, 1);
    CGContextFillEllipseInRect(ctx, CGRectMake(w / 4.0, h / 4.0, w / 2.0, h / 2.0));
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    return img;
}

BOOL WriteImage(NSString *path, CFStringRef type) {
    CGImageRef img = MakePicture(64, 48);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)[NSURL fileURLWithPath:path], type, 1, NULL);
    BOOL ok = NO;
    if (dest) {
        CGImageDestinationAddImage(dest, img, NULL);
        ok = CGImageDestinationFinalize(dest);
        CFRelease(dest);
    }
    CGImageRelease(img);
    return ok;
}

BOOL WritePDF(NSString *path) {
    CGImageRef cg = MakePicture(200, 100);
    NSImage *img = [[NSImage alloc] initWithCGImage:cg size:NSMakeSize(200, 100)];
    CGImageRelease(cg);
    PDFDocument *doc = [PDFDocument new];
    [doc insertPage:[[PDFPage alloc] initWithImage:img] atIndex:0];
    [doc insertPage:[[PDFPage alloc] initWithImage:img] atIndex:1];
    return [doc writeToFile:path];
}

// A zip with stored (uncompressed) entries, enough for a .docx.
void PutLE(NSMutableData *d, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) {
        uint8_t b = (uint8_t)(v >> (8 * i));
        [d appendBytes:&b length:1];
    }
}

BOOL WriteZip(NSString *path, NSArray<NSArray<NSString *> *> *entries) {
    NSMutableData *zip = [NSMutableData data], *central = [NSMutableData data];
    for (NSArray<NSString *> *e : entries) {
        NSData *name = [e[0] dataUsingEncoding:NSUTF8StringEncoding];
        NSData *body = [e[1] dataUsingEncoding:NSUTF8StringEncoding];
        const uint32_t crc = (uint32_t)crc32(0, (const Bytef *)body.bytes, (uInt)body.length);
        const uint32_t offset = (uint32_t)zip.length;
        PutLE(zip, 0x04034b50, 4); PutLE(zip, 20, 2); PutLE(zip, 0, 2); PutLE(zip, 0, 2);
        PutLE(zip, 0, 2); PutLE(zip, 0x21, 2); PutLE(zip, crc, 4);
        PutLE(zip, (uint32_t)body.length, 4); PutLE(zip, (uint32_t)body.length, 4);
        PutLE(zip, (uint32_t)name.length, 2); PutLE(zip, 0, 2);
        [zip appendData:name];
        [zip appendData:body];
        PutLE(central, 0x02014b50, 4); PutLE(central, 20, 2); PutLE(central, 20, 2);
        PutLE(central, 0, 2); PutLE(central, 0, 2); PutLE(central, 0, 2); PutLE(central, 0x21, 2);
        PutLE(central, crc, 4); PutLE(central, (uint32_t)body.length, 4);
        PutLE(central, (uint32_t)body.length, 4); PutLE(central, (uint32_t)name.length, 2);
        PutLE(central, 0, 2); PutLE(central, 0, 2); PutLE(central, 0, 2); PutLE(central, 0, 2);
        PutLE(central, 0, 4); PutLE(central, offset, 4);
        [central appendData:name];
    }
    const uint32_t centralAt = (uint32_t)zip.length;
    [zip appendData:central];
    PutLE(zip, 0x06054b50, 4); PutLE(zip, 0, 2); PutLE(zip, 0, 2);
    PutLE(zip, (uint32_t)entries.count, 2); PutLE(zip, (uint32_t)entries.count, 2);
    PutLE(zip, (uint32_t)central.length, 4); PutLE(zip, centralAt, 4); PutLE(zip, 0, 2);
    return [zip writeToFile:path atomically:YES];
}

BOOL WriteDocx(NSString *path) {
    NSString *types =
        @"<?xml version=\"1.0\" encoding=\"UTF-8\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        @"<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        @"<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        @"<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/></Types>";
    NSString *rels =
        @"<?xml version=\"1.0\" encoding=\"UTF-8\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        @"<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/></Relationships>";
    NSString *body =
        @"<?xml version=\"1.0\" encoding=\"UTF-8\"?><w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        @"<w:body><w:p><w:r><w:t>A document from the previews suite.</w:t></w:r></w:p></w:body></w:document>";
    return WriteZip(path, @[@[@"[Content_Types].xml", types], @[@"_rels/.rels", rels],
                            @[@"word/document.xml", body]]);
}

// A short QuickTime movie (H.264, 16 frames), written in the background.
void WriteMovie(NSString *path, void (^done)(BOOL ok)) {
    [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    NSError *err = nil;
    AVAssetWriter *w = [AVAssetWriter assetWriterWithURL:[NSURL fileURLWithPath:path]
                                                fileType:AVFileTypeQuickTimeMovie error:&err];
    if (!w) { done(NO); return; }
    AVAssetWriterInput *in = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
        outputSettings:@{AVVideoCodecKey: AVVideoCodecTypeH264, AVVideoWidthKey: @160,
                         AVVideoHeightKey: @96}];
    AVAssetWriterInputPixelBufferAdaptor *ad = [AVAssetWriterInputPixelBufferAdaptor
        assetWriterInputPixelBufferAdaptorWithAssetWriterInput:in
        sourcePixelBufferAttributes:@{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                                      (id)kCVPixelBufferWidthKey: @160, (id)kCVPixelBufferHeightKey: @96}];
    [w addInput:in];
    if (![w startWriting]) { done(NO); return; }
    [w startSessionAtSourceTime:kCMTimeZero];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        for (int f = 0; f < 16; f++) {
            while (!in.readyForMoreMediaData) usleep(2000);
            CVPixelBufferRef pb = NULL;
            CVPixelBufferPoolCreatePixelBuffer(NULL, ad.pixelBufferPool, &pb);
            if (!pb) break;
            CVPixelBufferLockBaseAddress(pb, 0);
            uint8_t *base = (uint8_t *)CVPixelBufferGetBaseAddress(pb);
            const size_t row = CVPixelBufferGetBytesPerRow(pb);
            for (size_t y = 0; y < 96; y++)
                memset(base + y * row, (int)(f * 15 + y), row);
            CVPixelBufferUnlockBaseAddress(pb, 0);
            [ad appendPixelBuffer:pb withPresentationTime:CMTimeMake(f, 8)];
            CVPixelBufferRelease(pb);
        }
        [in markAsFinished];
        [w finishWritingWithCompletionHandler:^{
            dispatch_async(dispatch_get_main_queue(), ^{ done(w.status == AVAssetWriterStatusCompleted); });
        }];
    });
}

void WriteBytes(NSString *path, const void *bytes, size_t n) {
    [[NSData dataWithBytes:bytes length:n] writeToFile:path atomically:YES];
}

// The Quick Look panel is a window: whatever ordering QLPreviewPanel (or
// NSPanel) does of its own is swizzled away too, beside the harness's
// NSWindow swizzles, and recorded.
int gPanelOrders = 0;
void QuietQuickLookPanel(void) {
    static BOOL done = NO;
    if (done) return;
    done = YES;
    SEL sels[] = {@selector(makeKeyAndOrderFront:), @selector(orderFront:),
                  @selector(orderFrontRegardless), @selector(orderWindow:relativeTo:),
                  @selector(makeKeyWindow)};
    Class classes[] = {QLPreviewPanel.class, NSPanel.class};
    for (Class c : classes) {
        for (SEL sel : sels) {
            Method m = class_getInstanceMethod(c, sel);
            Method base = class_getInstanceMethod(NSWindow.class, sel);
            if (!m || m == base) continue;   // inherited: the harness has it
            if (sel == @selector(orderWindow:relativeTo:))
                method_setImplementation(m, imp_implementationWithBlock(
                    ^(id, NSWindowOrderingMode, NSInteger) { gPanelOrders++; }));
            else if (sel == @selector(orderFrontRegardless) || sel == @selector(makeKeyWindow))
                method_setImplementation(m, imp_implementationWithBlock(^(id) { gPanelOrders++; }));
            else
                method_setImplementation(m, imp_implementationWithBlock(^(id, id) { gPanelOrders++; }));
        }
    }
}

// Selects a file's row in the tree and gives the tree the keyboard.
BOOL SelectInTree(EditorController *c, NSString *path) {
    [c revealPath:path andOpen:NO];
    NSOutlineView *tree = Tree(c);
    [c.window makeFirstResponder:tree];
    id item = [tree itemAtRow:tree.selectedRow];
    return [[item valueForKey:@"path"] isEqualToString:path];
}

// A key as the hardware makes it (CGEvent), sent to the window, which hands
// it to its first responder.
void PressKey(NSWindow *window, unsigned short keyCode, NSString *chars) {
    CGEventRef cg = CGEventCreateKeyboardEvent(NULL, keyCode, true);
    CGEventSetFlags(cg, (CGEventFlags)0);
    unichar buf[4];
    NSUInteger n = MIN(chars.length, (NSUInteger)4);
    [chars getCharacters:buf range:NSMakeRange(0, n)];
    CGEventKeyboardSetUnicodeString(cg, n, buf);
    NSEvent *e = [NSEvent eventWithCGEvent:cg];
    CFRelease(cg);
    NSEvent *forWindow = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
                                     modifierFlags:e.modifierFlags timestamp:e.timestamp
                                      windowNumber:window.windowNumber context:nil
                                        characters:e.characters
                       charactersIgnoringModifiers:e.charactersIgnoringModifiers
                                         isARepeat:NO keyCode:keyCode];
    [window sendEvent:forWindow];
}

void PressInTree(EditorController *c, unsigned short keyCode, NSString *chars) {
    PressKey(c.window, keyCode, chars);
}

}  // namespace

static void CsvSteps(void);

MC_GUI_SUITE(previews) {
    QuietQuickLookPanel();
    NSFileManager *fm = [NSFileManager defaultManager];
    [fm createDirectoryAtPath:Fixture(@"") withIntermediateDirectories:YES attributes:nil error:nil];
    __block BOOL haveHeic = NO, haveMovie = NO;

    MCGuiStep(@"fixtures are made", ^{
        GCHECK(WriteImage(Fixture(@"picture.png"), CFSTR("public.png")));
        haveHeic = WriteImage(Fixture(@"photo.heic"), CFSTR("public.heic"));
        if (!haveHeic) MCGuiNote(@"no HEIC encoder here: the .heic check is skipped");
        GCHECK(WritePDF(Fixture(@"paper.pdf")));
        GCHECK(WriteDocx(Fixture(@"letter.docx")));
        const char elf[] = "\x7f" "ELF\x02\x01\x01\0\0\0\0\0int main() { return 0; }\n";
        WriteBytes(Fixture(@"program.c"), elf, sizeof elf - 1);
        const char latin1[] = "caf\xE9 cr\xE8me br\xFBl\xE9" "e\n";
        WriteBytes(Fixture(@"latin1.txt"), latin1, sizeof latin1 - 1);
        MCGuiWriteFile(Fixture(@"plain.txt"), @"just text\n");
        // A window of its own on the fixtures, whatever other suites left
        // open: files from Finder go to the window whose folder holds them.
        id app = NSApp.delegate;
        EditorController *mine = [app performSelector:@selector(openWindowAtPath:)
                                           withObject:Fixture(@"").stringByStandardizingPath];
        MCGuiSetKeyWindow(nil);
        GCHECK(mine != nil && MCGuiController() == mine);
    });
    MCGuiAsync(@"a movie is written", 60, ^(MCGuiDone done) {
        WriteMovie(Fixture(@"clip.mov"), ^(BOOL ok) {
            haveMovie = ok;
            if (!ok) MCGuiNote(@"AVAssetWriter failed here: the .mov check is skipped");
            done();
        });
    });

    // ---- R4.3: binary by content, never as text
    MCGuiStep(@"a binary file named .c opens in Quick Look, not as text", ^{
        EditorController *c = MCGuiController();
        NSData *before = [NSData dataWithContentsOfFile:Fixture(@"program.c")];
        GCHECK([c showFile:Fixture(@"program.c")]);
        GCHECK_EQ_STR(ActiveName(c), @"program.c");
        GCHECK_EQ_STR(ActiveKind(c), @"quicklook");
        GCHECK(!EditorText(c).editable);
        GCHECK(EditorScroll(c).hidden);
        GCHECK(Viewers(c).shownKind == MCViewerQuickLook);
        MCQuickLookSlot *slot = Viewers(c).quickLook;
        GCHECK(slot != nil && !slot.hidden);
        GCHECK([slot.preview.previewItem.previewItemURL.path isEqualToString:Fixture(@"program.c")]);
        GCHECK(NSEqualRects(slot.frame, EditorScroll(c).frame));
        // Cmd+S writes nothing over it.
        [c saveCurrentFile:nil];
        GCHECK([[NSData dataWithContentsOfFile:Fixture(@"program.c")] isEqualToData:before]);
    });
    MCGuiStep(@"text in another encoding goes to Quick Look too", ^{
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"latin1.txt")]);
        GCHECK_EQ_STR(ActiveKind(c), @"quicklook");
    });
    MCGuiStep(@"a text file still opens as text, and the slot is given back", ^{
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"plain.txt")]);
        GCHECK_EQ_STR(ActiveKind(c), @"text");
        GCHECK(EditorText(c).editable);
        GCHECK(!EditorScroll(c).hidden);
        GCHECK(Viewers(c).quickLook.hidden);
        GCHECK(Viewers(c).quickLook.preview.previewItem == nil);
    });
    MCGuiStep(@"back to the binary tab: its preview comes back", ^{
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"program.c")]);
        GCHECK_EQ_STR(ActiveKind(c), @"quicklook");
        GCHECK(!Viewers(c).quickLook.hidden);
        GCHECK([Viewers(c).quickLook.preview.previewItem.previewItemURL.path
                   isEqualToString:Fixture(@"program.c")]);
    });

    // ---- R4.1 and the acceptance check: from Finder
    MCGuiStep(@"a .docx from Finder opens as a Quick Look preview tab", ^{
        EditorController *c = MCGuiController();
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:Fixture(@"letter.docx")]]];
        GCHECK_EQ_STR(ActiveName(c), @"letter.docx");
        GCHECK_EQ_STR(ActiveKind(c), @"quicklook");
        GCHECK(!Viewers(c).quickLook.hidden);
        MCGuiSnapshot(c.window.contentView, @"previews-docx");
    });
    MCGuiStep(@"a PDF from Finder opens as a preview tab", ^{
        EditorController *c = MCGuiController();
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:Fixture(@"paper.pdf")]]];
        GCHECK_EQ_STR(ActiveName(c), @"paper.pdf");
        GCHECK_EQ_STR(ActiveKind(c), @"pdf");
        GCHECK(Viewers(c).quickLook.hidden);
    });
    MCGuiStep(@"a .heic from Finder opens as a preview tab", ^{
        if (!haveHeic) return;
        EditorController *c = MCGuiController();
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:Fixture(@"photo.heic")]]];
        GCHECK_EQ_STR(ActiveName(c), @"photo.heic");
        GCHECK_EQ_STR(ActiveKind(c), @"image");
    });
    MCGuiStep(@"a .mov from Finder opens as a preview tab", ^{
        if (!haveMovie) return;
        EditorController *c = MCGuiController();
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:Fixture(@"clip.mov")]]];
        GCHECK_EQ_STR(ActiveName(c), @"clip.mov");
        GCHECK_EQ_STR(ActiveKind(c), @"media");
    });

    // ---- R4.2: Space in the tree
    MCGuiStep(@"Space on a PNG in the tree opens the Quick Look panel on it", ^{
        EditorController *c = MCGuiController();
        MCQuickLookPanel *ql = [MCQuickLookPanel shared];
        GCHECK(!ql.shown);
        GCHECK(SelectInTree(c, Fixture(@"picture.png")));
        GCHECK(c.window.firstResponder == Tree(c));
        PressInTree(c, 49, @" ");
        GCHECK(ql.shown);
        GCHECK(ql.items.count == 1 &&
               [ql.items.firstObject.path isEqualToString:Fixture(@"picture.png")]);
        // The panel is driven through the tree, as Quick Look asks.
        QLPreviewPanel *panel = [QLPreviewPanel sharedPreviewPanel];
        GCHECK([Tree(c) acceptsPreviewPanelControl:panel]);
        [Tree(c) beginPreviewPanelControl:panel];
        GCHECK(panel.dataSource == (id)ql);
        GCHECK([ql numberOfPreviewItemsInPreviewPanel:panel] == 1);
        GCHECK([[ql previewPanel:panel previewItemAtIndex:0].previewItemURL.path
                   isEqualToString:Fixture(@"picture.png")]);
        // Nothing opened in the editor: the tree still has the keyboard.
        GCHECK(c.window.firstResponder == Tree(c));
    });
    MCGuiStep(@"Space again closes it", ^{
        EditorController *c = MCGuiController();
        PressInTree(c, 49, @" ");
        GCHECK(![MCQuickLookPanel shared].shown);
        GCHECK(![Tree(c) acceptsPreviewPanelControl:[QLPreviewPanel sharedPreviewPanel]]);
        [Tree(c) endPreviewPanelControl:[QLPreviewPanel sharedPreviewPanel]];
        GCHECK([QLPreviewPanel sharedPreviewPanel].dataSource == nil);
    });
    MCGuiStep(@"Space opens it and Esc closes it", ^{
        EditorController *c = MCGuiController();
        PressInTree(c, 49, @" ");
        GCHECK([MCQuickLookPanel shared].shown);
        PressInTree(c, 53, @"\x1b");
        GCHECK(![MCQuickLookPanel shared].shown);
        // Esc with the panel closed is the tree's as before (nothing happens).
        PressInTree(c, 53, @"\x1b");
        GCHECK(![MCQuickLookPanel shared].shown);
    });
    MCGuiStep(@"while open, the panel follows the selection", ^{
        EditorController *c = MCGuiController();
        GCHECK(SelectInTree(c, Fixture(@"paper.pdf")));
        PressInTree(c, 49, @" ");
        GCHECK([MCQuickLookPanel shared].shown);
        NSOutlineView *tree = Tree(c);
        const NSInteger row = tree.selectedRow;
        PressInTree(c, 125, [NSString stringWithFormat:@"%C", (unichar)NSDownArrowFunctionKey]);
        GCHECK(tree.selectedRow == row + 1);
        NSString *now = [[tree itemAtRow:tree.selectedRow] valueForKey:@"path"];
        GCHECK([[MCQuickLookPanel shared].items.firstObject.path isEqualToString:now]);
        [[MCQuickLookPanel shared] close];
        GCHECK(![MCQuickLookPanel shared].shown);
    });
    CsvSteps();
}

// ---------------------------------------------------------------- CSV
namespace {

NSString *const kCsvText =
    @"id,name,notes\n"
    @"1,\"Smith, J\",\"line one\nline two\"\n"
    @"2,Doe,plain\n"
    @"3,\"Roe, R\",\"a, b, c\"\n";

NSData *Bytes(NSString *path) { return [NSData dataWithContentsOfFile:path]; }
NSString *Text(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}

// How many lines differ between two texts, line by line.
NSUInteger ChangedLines(NSString *a, NSString *b) {
    NSArray *x = [a componentsSeparatedByString:@"\n"], *y = [b componentsSeparatedByString:@"\n"];
    NSUInteger n = 0;
    for (NSUInteger i = 0; i < MAX(x.count, y.count); i++)
        if (i >= x.count || i >= y.count || ![x[i] isEqualToString:y[i]]) n++;
    return n;
}

MCCsvTableView *Csv(EditorController *c) { return Viewers(c).csv; }

// An edit as one undo step of its own. Steps run from timers, where the
// group the undo manager opens by itself is never closed (Harness.h), so
// it is closed first, or every edit since would undo as one.
void OwnUndoGroup(NSUndoManager *u, void (^block)(void)) {
    if (u.groupsByEvent && u.groupingLevel > 0) [u endUndoGrouping];
    MCGuiUndoGroup(u, block);
}
BOOL Dirty(EditorController *c) { return [ActiveTab(c)[@"dirty"] boolValue]; }

}  // namespace

static void CsvSteps(void) {
    MCGuiStep(@"CSV fixtures", ^{
        MCGuiWriteFile(Fixture(@"people.csv"), kCsvText);
        MCGuiWriteFile(Fixture(@"crlf.csv"), @"a;b\r\n\"x; y\";2\r\n");
        MCGuiWriteFile(Fixture(@"cols.tsv"), @"k\tv\nalpha\tone, two\n");
    });
    MCGuiStep(@"a CSV opens as a table, the text view hidden", ^{
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"people.csv")]);
        GCHECK_EQ_STR(ActiveKind(c), @"table");
        MCCsvTableView *t = Csv(c);
        GCHECK(t != nil && !t.hidden);
        GCHECK(EditorScroll(c).hidden);
        GCHECK(t.editable);
        GCHECK(t.rowCount == 4 && t.columnCount == 3);
        GCHECK_EQ_STR([t cellAtRow:1 column:1], @"Smith, J");
        GCHECK_EQ_STR([t cellAtRow:1 column:2], @"line one\nline two");
        GCHECK(t.table.numberOfRows == 4 && t.table.numberOfColumns == 4);   // # and A..C
        GCHECK_EQ_STR(t.table.tableColumns[1].title, @"A");
        GCHECK([t.infoText hasPrefix:@"4 rows, 3 columns, comma separated"]);
        GCHECK(c.canTogglePreview);
        GCHECK(!Dirty(c));
        MCGuiSnapshot(c.window.contentView, @"previews-csv");
    });
    MCGuiStep(@"saved with no edits it comes back byte-identical", ^{
        EditorController *c = MCGuiController();
        NSData *before = Bytes(Fixture(@"people.csv"));
        [c saveCurrentFile:nil];
        GCHECK([Bytes(Fixture(@"people.csv")) isEqualToData:before]);
    });
    MCGuiStep(@"one cell edit changes one line, unsaved until Cmd+S", ^{
        EditorController *c = MCGuiController();
        MCCsvTableView *t = Csv(c);
        GCHECK([t setCell:@"edited" row:2 column:2]);
        GCHECK(Dirty(c));
        GCHECK_EQ_STR(Text(Fixture(@"people.csv")), kCsvText);   // the disk is untouched
        [c saveCurrentFile:nil];
        NSString *now = Text(Fixture(@"people.csv"));
        GCHECK(ChangedLines(kCsvText, now) == 1);
        GCHECK([now containsString:@"\n2,Doe,edited\n"]);
        GCHECK([now containsString:@"\"line one\nline two\""]);
        GCHECK(!Dirty(c));
    });
    MCGuiStep(@"undo and redo go through the tab's undo manager", ^{
        EditorController *c = MCGuiController();
        MCCsvTableView *t = Csv(c);
        GCHECK([(id<NSMenuItemValidation>)t.table validateMenuItem:[[NSMenuItem alloc] initWithTitle:@"Undo"
                                                                     action:@selector(undo:)
                                                              keyEquivalent:@""]]);
        [(id)t.table undo:nil];
        GCHECK_EQ_STR([t cellAtRow:2 column:2], @"plain");
        GCHECK(Dirty(c));   // differs from the saved file
        [(id)t.table redo:nil];
        GCHECK_EQ_STR([t cellAtRow:2 column:2], @"edited");
        GCHECK(!Dirty(c));
    });
    MCGuiStep(@"a cell typed in the table's own editor", ^{
        EditorController *c = MCGuiController();
        MCCsvTableView *t = Csv(c);
        [c.window makeFirstResponder:t.table];
        [t.table editColumn:1 row:3 withEvent:nil select:YES];
        NSText *editor = t.table.currentEditor;
        GCHECK(editor != nil);
        editor.string = @"0";
        OwnUndoGroup([c valueForKeyPath:@"textView.undoManager"], ^{
            [c.window makeFirstResponder:t.table];   // ends editing, as a click away does
        });
        GCHECK_EQ_STR([t cellAtRow:3 column:0], @"0");
        GCHECK(Dirty(c));
    });
    MCGuiStep(@"rows and columns come and go from the right click menu", ^{
        EditorController *c = MCGuiController();
        MCCsvTableView *t = Csv(c);
        NSMenu *m = [t menuForRow:1 column:2];   // row 1, column B
        NSMutableArray *titles = [NSMutableArray array];
        for (NSMenuItem *i in m.itemArray) if (i.title.length) [titles addObject:i.title];
        GCHECK([titles containsObject:@"Insert Row Below"]);
        GCHECK([titles containsObject:@"Delete Column"]);
        GCHECK([titles containsObject:@"Adjust Settings…"]);
        // Each edit its own undo group, as each click would be.
        NSUndoManager *u = [c valueForKeyPath:@"textView.undoManager"];
        NSMenuItem *below = [m itemWithTitle:@"Insert Row Below"];
        OwnUndoGroup(u, ^{ [NSApp sendAction:below.action to:below.target from:below]; });
        GCHECK(t.rowCount == 5);
        GCHECK_EQ_STR([t cellAtRow:2 column:0], @"");
        NSMenuItem *delCol = [m itemWithTitle:@"Delete Column"];
        OwnUndoGroup(u, ^{ [NSApp sendAction:delCol.action to:delCol.target from:delCol]; });
        GCHECK(t.columnCount == 2);
        GCHECK_EQ_STR([t cellAtRow:0 column:1], @"notes");
        OwnUndoGroup(u, ^{ GCHECK([t insertColumnAt:0]); });
        GCHECK(t.columnCount == 3);
        OwnUndoGroup(u, ^{ GCHECK([t deleteRowAt:2]); });
        GCHECK(t.rowCount == 4);
    });
    MCGuiStep(@"undo walks back through them, one step each", ^{
        EditorController *c = MCGuiController();
        MCCsvTableView *t = Csv(c);
        for (int i = 0; i < 4; i++) [(id)t.table undo:nil];
        GCHECK_EQ_STR([t cellAtRow:0 column:1], @"name");
        GCHECK(t.rowCount == 4 && t.columnCount == 3);
        GCHECK_EQ_STR([t cellAtRow:3 column:0], @"0");   // the typed cell stays
    });
    MCGuiStep(@"Shift+Cmd+V shows the text and back, edits carried both ways", ^{
        EditorController *c = MCGuiController();
        [c togglePreview:nil];
        GCHECK(!EditorScroll(c).hidden);
        GCHECK(Csv(c).hidden);
        NSTextView *tv = EditorText(c);
        GCHECK(tv.editable);
        GCHECK([tv.string hasPrefix:@"id,name,notes\n"]);
        [tv insertText:@"key" replacementRange:NSMakeRange(0, 2)];
        [c togglePreview:nil];
        GCHECK(!Csv(c).hidden);
        GCHECK(EditorScroll(c).hidden);
        GCHECK_EQ_STR([Csv(c) cellAtRow:0 column:0], @"key");
        GCHECK(c.window.firstResponder == Csv(c).table);
    });
    MCGuiStep(@"semicolons and CRLF are kept, a TSV gets tabs", ^{
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"crlf.csv")]);
        MCCsvTableView *t = Csv(c);
        GCHECK_EQ_STR([t cellAtRow:1 column:0], @"x; y");
        GCHECK([t.infoText containsString:@"semicolon"]);
        [t setCell:@"3" row:1 column:1];
        [c saveCurrentFile:nil];
        GCHECK_EQ_STR(Text(Fixture(@"crlf.csv")), @"a;b\r\n\"x; y\";3\r\n");
        GCHECK([c showFile:Fixture(@"cols.tsv")]);
        GCHECK_EQ_STR(ActiveKind(c), @"table");
        GCHECK_EQ_STR([Csv(c) cellAtRow:1 column:1], @"one, two");
        GCHECK([Csv(c).infoText containsString:@"tab separated"]);
    });
    MCGuiStep(@"a tab switch keeps the table's unsaved edits", ^{
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"people.csv")]);
        GCHECK_EQ_STR(ActiveKind(c), @"table");
        GCHECK_EQ_STR([Csv(c) cellAtRow:0 column:0], @"key");
        GCHECK(Dirty(c));
        [c saveCurrentFile:nil];
        GCHECK([Text(Fixture(@"people.csv")) hasPrefix:@"key,name,notes\n"]);
    });
    MCGuiStep(@"a CSV over 50 MB opens read-only with its first 100,000 rows", ^{
        EditorController *c = MCGuiController();
        std::string big = "n,name,value\n";
        big.reserve(52 * 1000 * 1000);
        char line[64];
        for (int i = 0; big.size() <= MCCsvEditableBytes; i++) {
            int n = snprintf(line, sizeof line, "%d,\"row %d, quoted\",%d.5\n", i, i, i * 3);
            big.append(line, (size_t)n);
        }
        NSString *path = Fixture(@"huge.csv");
        [[NSData dataWithBytes:big.data() length:big.size()] writeToFile:path atomically:NO];
        NSDate *mtime = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil]
                            .fileModificationDate;
        GCHECK([c showFile:path]);
        GCHECK_EQ_STR(ActiveKind(c), @"table");
        MCCsvTableView *t = Csv(c);
        GCHECK(!t.editable);
        GCHECK(t.rowCount == MCCsvLargeRows);
        GCHECK(t.truncated);
        GCHECK_EQ_STR([t cellAtRow:1 column:1], @"row 0, quoted");
        GCHECK([t.infoText hasPrefix:@"Read-only"]);
        GCHECK(![t setCell:@"x" row:1 column:1]);
        GCHECK(!c.canTogglePreview);
        [c saveCurrentFile:nil];
        NSDictionary *after = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
        GCHECK([after.fileModificationDate isEqualToDate:mtime]);
        GCHECK(after.fileSize == big.size());
        [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    });
}
