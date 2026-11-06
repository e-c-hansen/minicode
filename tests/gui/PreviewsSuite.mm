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
#import "JsonOutlineView.h"
#import "AppSettings.h"
#import "SqliteView.h"
#include <sqlite3.h>
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
static void JsonSteps(void);
static void NotebookSteps(void);
static void SqliteSteps(void);

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
    MCGuiStep(@"while open, the panel follows a click too", ^{
        EditorController *c = MCGuiController();
        NSOutlineView *tree = Tree(c);
        GCHECK(SelectInTree(c, Fixture(@"paper.pdf")));
        PressInTree(c, 49, @" ");
        GCHECK([MCQuickLookPanel shared].shown);
        GCHECK(SelectInTree(c, Fixture(@"picture.png")));   // reveal it, then click it
        const NSInteger row = tree.selectedRow;
        GCHECK(SelectInTree(c, Fixture(@"paper.pdf")));
        const NSRect r = [tree rectOfRow:row];
        const NSPoint at = [tree convertPoint:NSMakePoint(NSMidX(r), NSMidY(r)) toView:nil];
        NSEvent *down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:at modifierFlags:0
                                          timestamp:NSProcessInfo.processInfo.systemUptime
                                       windowNumber:c.window.windowNumber context:nil
                                        eventNumber:0 clickCount:1 pressure:1];
        NSEvent *up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:at modifierFlags:0
                                        timestamp:NSProcessInfo.processInfo.systemUptime
                                     windowNumber:c.window.windowNumber context:nil
                                      eventNumber:0 clickCount:1 pressure:0];
        [NSApp postEvent:up atStart:NO];
        [tree mouseDown:down];
        GCHECK(tree.selectedRow == row);
        GCHECK([[MCQuickLookPanel shared].items.firstObject.path isEqualToString:Fixture(@"picture.png")]);
        [[MCQuickLookPanel shared] close];
    });
    MCGuiStep(@"open for one window's tree, Space in another's shows its file", ^{
        MCQuickLookPanel *ql = [MCQuickLookPanel shared];
        NSObject *treeA = [NSObject new], *treeB = [NSObject new];
        NSURL *a = [NSURL fileURLWithPath:Fixture(@"paper.pdf")];
        NSURL *b = [NSURL fileURLWithPath:Fixture(@"picture.png")];
        [ql toggleWithURLs:@[a] from:treeA];
        GCHECK(ql.shown);
        [ql toggleWithURLs:@[b] from:treeB];
        GCHECK(ql.shown);
        GCHECK([ql.items.firstObject isEqual:b]);
        [ql toggleWithURLs:@[b] from:treeB];   // its own Space closes it
        GCHECK(!ql.shown);
        // Left with no view driving it (the keyboard went to the editor),
        // the next Space in the tree shows the selection again.
        QLPreviewPanel *panel = [QLPreviewPanel sharedPreviewPanel];
        [ql toggleWithURLs:@[a] from:treeA];
        [ql beginControl:panel];
        [ql endControl:panel];
        [ql toggleWithURLs:@[b] from:treeA];
        GCHECK(ql.shown);
        GCHECK([ql.items.firstObject isEqual:b]);
        [ql toggleWithURLs:@[b] from:treeA];
        GCHECK(!ql.shown);
    });
    MCGuiStep(@"a space in a name being typed to find a row is type-select's", ^{
        EditorController *c = MCGuiController();
        GCHECK(SelectInTree(c, Fixture(@"plain.txt")));
        PressInTree(c, 35, @"p");
        PressInTree(c, 49, @" ");
        GCHECK(![MCQuickLookPanel shared].shown);
    });
    MCGuiPause(1.2);
    MCGuiStep(@"and a while later Space is Quick Look again", ^{
        EditorController *c = MCGuiController();
        PressInTree(c, 49, @" ");
        GCHECK([MCQuickLookPanel shared].shown);
        [[MCQuickLookPanel shared] close];
    });
    CsvSteps();
    JsonSteps();
    NotebookSteps();
    SqliteSteps();
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
    MCGuiStep(@"a cell being typed into lands in its own file when the tab changes", ^{
        EditorController *c = MCGuiController();
        MCCsvTableView *t = Csv(c);
        GCHECK(!Dirty(c));
        GCHECK_EQ_STR([t cellAtRow:2 column:1], @"Doe");
        [c.window makeFirstResponder:t.table];
        [t.table editColumn:2 row:2 withEvent:nil select:YES];   // # A B: B is "name"
        GCHECK(t.table.currentEditor != nil);
        t.table.currentEditor.string = @"TYPED";
        NSData *crlfBefore = Bytes(Fixture(@"crlf.csv"));
        OwnUndoGroup([c valueForKeyPath:@"textView.undoManager"], ^{
            GCHECK([c showFile:Fixture(@"crlf.csv")]);   // a tab switch, as Ctrl+Tab makes
        });
        // The other CSV is as it was: not edited, not dirty, not written.
        GCHECK_EQ_STR(ActiveName(c), @"crlf.csv");
        GCHECK_EQ_STR([Csv(c) cellAtRow:1 column:0], @"x; y");
        GCHECK_EQ_STR([Csv(c) cellAtRow:1 column:1], @"3");
        GCHECK(!Dirty(c));
        GCHECK([Bytes(Fixture(@"crlf.csv")) isEqualToData:crlfBefore]);
        // The value went into people.csv, where it was typed.
        GCHECK([c showFile:Fixture(@"people.csv")]);
        GCHECK_EQ_STR([Csv(c) cellAtRow:2 column:1], @"TYPED");
        GCHECK(Dirty(c));
        [c saveCurrentFile:nil];
    });
    MCGuiStep(@"a cell being typed into when the file changes on disk is dropped, not misplaced", ^{
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"crlf.csv")]);
        MCCsvTableView *t = Csv(c);
        [c.window makeFirstResponder:t.table];
        [t.table editColumn:2 row:1 withEvent:nil select:YES];
        GCHECK(t.table.currentEditor != nil);
        t.table.currentEditor.string = @"TYPED";
        // A script sorts the rows: another record now sits on row 1.
        NSString *path = Fixture(@"crlf.csv");
        MCGuiWriteFile(path, @"a;b\r\nnew;row\r\n\"x; y\";3\r\n");
        [[NSFileManager defaultManager] setAttributes:@{NSFileModificationDate:
                                                            [NSDate dateWithTimeIntervalSinceNow:5]}
                                         ofItemAtPath:path error:nil];
        [c performSelector:@selector(checkExternalChange)];
        GCHECK(t.table.currentEditor == nil);
        GCHECK_EQ_STR([t cellAtRow:1 column:1], @"row");
        GCHECK_EQ_STR([t cellAtRow:2 column:1], @"3");
        GCHECK(!Dirty(c));
        GCHECK_EQ_STR(Text(path), @"a;b\r\nnew;row\r\n\"x; y\";3\r\n");
    });
    MCGuiStep(@"typing in a CSV's text, then Cmd+Z back in the table undoes the typing", ^{
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"people.csv")]);
        NSUndoManager *u = [c valueForKeyPath:@"textView.undoManager"];
        OwnUndoGroup(u, ^{ [c togglePreview:nil]; });
        NSTextView *tv = EditorText(c);
        GCHECK(tv.editable);
        OwnUndoGroup(u, ^{ [tv insertText:@"Z" replacementRange:NSMakeRange(0, 0)]; });
        OwnUndoGroup(u, ^{ [tv insertText:@"Y" replacementRange:NSMakeRange(1, 0)]; });
        OwnUndoGroup(u, ^{ [c togglePreview:nil]; });
        MCCsvTableView *t = Csv(c);
        GCHECK(!t.hidden);
        GCHECK_EQ_STR([t cellAtRow:0 column:0], @"ZYkey");
        GCHECK(Dirty(c));
        // One step for what was typed, taken in the table: the text comes
        // back, not an exception in the hidden, empty text view.
        GCHECK(u.canUndo);
        GCHECK_EQ_STR(u.undoActionName, @"Typing");
        [(id)t.table undo:nil];
        GCHECK_EQ_STR([t cellAtRow:0 column:0], @"key");
        GCHECK([[c valueForKey:@"sourceText"] hasPrefix:@"key,name,notes\n"]);
        GCHECK(!Dirty(c));
        [(id)t.table redo:nil];
        GCHECK_EQ_STR([t cellAtRow:0 column:0], @"ZYkey");
        [(id)t.table undo:nil];
        GCHECK(!Dirty(c));
    });
    MCGuiStep(@"undo keeps only what changed, and never splits a character", ^{
        EditorController *c = MCGuiController();
        MCCsvTableView *t = Csv(c);
        NSUndoManager *u = [c valueForKeyPath:@"textView.undoManager"];
        OwnUndoGroup(u, ^{ GCHECK([t setCell:@"\U0001F600" row:2 column:2]); });
        OwnUndoGroup(u, ^{ GCHECK([t setCell:@"\U0001F601" row:2 column:2]); });
        [(id)t.table undo:nil];
        GCHECK_EQ_STR([t cellAtRow:2 column:2], @"\U0001F600");
        [(id)t.table undo:nil];
        GCHECK_EQ_STR([t cellAtRow:2 column:2], @"edited");
        GCHECK(!Dirty(c));
        [(id)t.table redo:nil];
        [(id)t.table redo:nil];
        GCHECK_EQ_STR([t cellAtRow:2 column:2], @"\U0001F601");
        [(id)t.table undo:nil];
        [(id)t.table undo:nil];
        GCHECK(!Dirty(c));
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

// ---------------------------------------------------------------- JSON
namespace {

NSString *const kJsonText =
    @"{\n"
    @"  \"name\": \"minicode\",\n"
    @"  \"tags\": [\"editor\", \"mac\"],\n"
    @"  \"nested\": {\"deep\": {\"k\": 1}}\n"
    @"}\n";

MCJsonOutlinePane *Outline(EditorController *c) { return Viewers(c).json; }

NSArray<NSString *> *OutlineRows(MCJsonOutlinePane *p) {
    NSMutableArray *rows = [NSMutableArray array];
    for (NSInteger r = 0; r < p.outline.numberOfRows; r++) [rows addObject:[p rowText:r]];
    return rows;
}

NSInteger RowStarting(MCJsonOutlinePane *p, NSString *prefix) {
    for (NSInteger r = 0; r < p.outline.numberOfRows; r++)
        if ([[p rowText:r] hasPrefix:prefix]) return r;
    return -1;
}

}  // namespace

static void JsonSteps(void) {
    MCGuiStep(@"a JSON file opens as text with its outline beside it", ^{
        MCGuiWriteFile(Fixture(@"data.json"), kJsonText);
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"data.json")]);
        GCHECK_EQ_STR(ActiveKind(c), @"text");
        GCHECK(EditorText(c).editable);
        GCHECK(!EditorScroll(c).hidden);
        MCJsonOutlinePane *p = Outline(c);
        GCHECK(p != nil && !p.hidden);
        // Side by side: the text ends where the outline starts.
        GCHECK(fabs(NSMaxX(EditorScroll(c).frame) - NSMinX(p.frame)) < 0.5);
        GCHECK(NSWidth(p.frame) >= 220);
    });
    MCGuiWait(@"the outline is read", 5, ^BOOL {
        MCJsonOutlinePane *p = Outline(MCGuiController());
        return !p.parsing && p.outline.numberOfRows > 1;
    });
    MCGuiStep(@"the outline shows the tree", ^{
        MCJsonOutlinePane *p = Outline(MCGuiController());
        NSArray *rows = OutlineRows(p);
        GCHECK(rows.count == 4);
        GCHECK_EQ_STR(rows.count ? rows[0] : nil, @"root {3}");
        GCHECK_EQ_STR(rows.count > 1 ? rows[1] : nil, @"name \"minicode\"");
        GCHECK_EQ_STR(rows.count > 2 ? rows[2] : nil, @"tags [2]");
        GCHECK_EQ_STR(rows.count > 3 ? rows[3] : nil, @"nested {1}");
        GCHECK_EQ_STR(p.statusText, @"8 values");
        MCGuiSnapshot(MCGuiController().window.contentView, @"previews-json");
    });
    MCGuiStep(@"selecting a node selects its text, the keyboard staying in the outline", ^{
        EditorController *c = MCGuiController();
        MCJsonOutlinePane *p = Outline(c);
        [c.window makeFirstResponder:p.outline];
        [p selectRow:1];
        NSTextView *tv = EditorText(c);
        GCHECK_EQ_STR([tv.string substringWithRange:tv.selectedRange], @"\"name\": \"minicode\"");
        GCHECK(c.window.firstResponder == p.outline);
        [p selectRow:2];
        GCHECK_EQ_STR([tv.string substringWithRange:tv.selectedRange],
                      @"\"tags\": [\"editor\", \"mac\"]");
    });
    MCGuiStep(@"the caret in the text selects its node, opening what holds it", ^{
        EditorController *c = MCGuiController();
        MCJsonOutlinePane *p = Outline(c);
        NSTextView *tv = EditorText(c);
        tv.selectedRange = NSMakeRange([tv.string rangeOfString:@"\"k\""].location + 1, 0);
        const NSInteger row = p.outline.selectedRow;
        GCHECK_EQ_STR(row >= 0 ? [p rowText:row] : nil, @"k 1");
        GCHECK(RowStarting(p, @"deep {1}") >= 0);
        // The text's caret was left where it was put.
        GCHECK(tv.selectedRange.length == 0);
    });
    MCGuiStep(@"text that stops being JSON keeps the last outline, and says where", ^{
        EditorController *c = MCGuiController();
        NSTextView *tv = EditorText(c);
        const NSUInteger at = [tv.string rangeOfString:@"\"tags\":"].location + 6;
        [tv insertText:@"" replacementRange:NSMakeRange(at, 1)];   // drop the colon
    });
    MCGuiWait(@"the error is shown", 5, ^BOOL {
        return [Outline(MCGuiController()).statusText hasPrefix:@"Not valid JSON"];
    });
    MCGuiStep(@"the error's place", ^{
        MCJsonOutlinePane *p = Outline(MCGuiController());
        GCHECK_EQ_STR(p.statusText, @"Not valid JSON: Expected ':' at line 3, column 10");
        GCHECK(RowStarting(p, @"deep {1}") >= 0);   // the last good outline
        // Its places in the text are out of date: it is dimmed, and a click
        // in it selects nothing (the next keystroke would replace it).
        GCHECK(p.stale);
        GCHECK(p.outline.alphaValue < 1);
        NSTextView *tv = EditorText(MCGuiController());
        tv.selectedRange = NSMakeRange(0, 0);
        [p selectRow:RowStarting(p, @"nested {1}")];
        GCHECK(tv.selectedRange.location == 0 && tv.selectedRange.length == 0);
    });
    MCGuiStep(@"fixed and grown, the outline follows and keeps what was open", ^{
        EditorController *c = MCGuiController();
        NSTextView *tv = EditorText(c);
        const NSUInteger at = [tv.string rangeOfString:@"\"tags\""].location + 6;
        [tv insertText:@": " replacementRange:NSMakeRange(at, 1)];
        const NSUInteger end = [tv.string rangeOfString:@"}}\n}"].location + 2;
        [tv insertText:@",\n  \"added\": null" replacementRange:NSMakeRange(end, 0)];
    });
    MCGuiWait(@"the new outline", 5, ^BOOL {
        return RowStarting(Outline(MCGuiController()), @"added null") >= 0;
    });
    MCGuiStep(@"what was open stays open", ^{
        MCJsonOutlinePane *p = Outline(MCGuiController());
        GCHECK_EQ_STR(p.statusText, @"9 values");
        GCHECK(!p.stale && p.outline.alphaValue == 1);
        GCHECK(RowStarting(p, @"deep {1}") >= 0);
        GCHECK(RowStarting(p, @"k 1") >= 0);
    });
    MCGuiStep(@"Shift+Cmd+V hides the outline and shows it again", ^{
        EditorController *c = MCGuiController();
        GCHECK(c.canTogglePreview);
        [c togglePreview:nil];
        GCHECK(Outline(c).hidden);
        GCHECK(EditorText(c).editable);
        const CGFloat full = NSWidth(EditorScroll(c).frame);
        [c togglePreview:nil];
        GCHECK(!Outline(c).hidden);
        GCHECK(NSWidth(EditorScroll(c).frame) < full);
    });
    MCGuiStep(@"another tab lets the outline go, and coming back brings it", ^{
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"plain.txt")]);
        GCHECK(Outline(c).hidden);
        GCHECK(Outline(c).outline.numberOfRows == 0);
        GCHECK([c showFile:Fixture(@"data.json")]);
        GCHECK(!Outline(c).hidden);
        GCHECK([c valueForKey:@"dirty"] != nil && [[c valueForKey:@"dirty"] boolValue]);
    });
    MCGuiWait(@"the outline is read again", 5, ^BOOL {
        return RowStarting(Outline(MCGuiController()), @"added null") >= 0;
    });
    MCGuiStep(@"a .jsonc file, comments and trailing commas, has its outline", ^{
        MCGuiWriteFile(Fixture(@"config.jsonc"),
                       @"// settings\n{\n  /* how wide */ \"editor.ruler\": 100,\n"
                       @"  \"files\": [\"a\", \"b\",],  // trailing\n}\n");
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"config.jsonc")]);
        GCHECK(Outline(c).allowsComments);
    });
    MCGuiWait(@"the .jsonc outline", 5, ^BOOL {
        MCJsonOutlinePane *p = Outline(MCGuiController());
        return !p.parsing && p.outline.numberOfRows > 1;
    });
    MCGuiStep(@"read as JSON with comments", ^{
        MCJsonOutlinePane *p = Outline(MCGuiController());
        GCHECK_EQ_STR(p.statusText, @"5 values");
        GCHECK(RowStarting(p, @"editor.ruler 100") >= 0);
        GCHECK(RowStarting(p, @"files [2]") >= 0);
        // A plain .json file is still strict.
        MCGuiWriteFile(Fixture(@"strict.json"), @"{\"a\": 1, // no\n}\n");
        GCHECK([MCGuiController() showFile:Fixture(@"strict.json")]);
        GCHECK(!Outline(MCGuiController()).allowsComments);
    });
    MCGuiWait(@"the strict file's verdict", 5, ^BOOL {
        return [Outline(MCGuiController()).statusText hasPrefix:@"Not valid JSON"];
    });
}

// ---------------------------------------------------------------- notebooks
namespace {

NSString *PngBase64(void) {
    NSString *path = Fixture(@"picture.png");
    return [[NSData dataWithContentsOfFile:path] base64EncodedStringWithOptions:0];
}

NSString *NotebookText(void) {
    NSDictionary *nb = @{
        @"nbformat": @4, @"nbformat_minor": @5,
        @"metadata": @{@"kernelspec": @{@"language": @"python", @"name": @"python3"},
                       @"language_info": @{@"name": @"python"}},
        @"cells": @[
            @{@"cell_type": @"markdown", @"metadata": @{},
              @"source": @[@"# Analysis\n", @"Some **bold** words."]},
            @{@"cell_type": @"code", @"execution_count": @1, @"metadata": @{},
              @"source": @[@"def double(x):\n", @"    return x * 2\n", @"print(double(21))"],
              @"outputs": @[
                  @{@"output_type": @"stream", @"name": @"stdout", @"text": @[@"42\n"]},
                  @{@"output_type": @"display_data", @"metadata": @{},
                    @"data": @{@"image/png": PngBase64(), @"text/plain": @[@"<Figure>"]}},
                  @{@"output_type": @"display_data", @"metadata": @{},
                    @"data": @{@"text/html": @[@"<table><tr><td>cell one</td></tr></table>"]}},
                  @{@"output_type": @"error", @"ename": @"KeyError", @"evalue": @"'k'",
                    @"traceback": @[@"\x1b[0;31mKeyError\x1b[0m: 'k'"]},
              ]},
        ],
    };
    NSData *d = [NSJSONSerialization dataWithJSONObject:nb options:NSJSONWritingPrettyPrinted error:nil];
    return [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
}

// The color the text view draws the first `word` of its text in.
NSColor *ColorOf(NSTextView *tv, NSString *word, NSUInteger from) {
    NSRange r = [tv.string rangeOfString:word options:0 range:NSMakeRange(from, tv.string.length - from)];
    if (r.location == NSNotFound) return nil;
    return [tv.textStorage attribute:NSForegroundColorAttributeName atIndex:r.location effectiveRange:nil];
}

BOOL SameColor(NSColor *a, NSColor *b) {
    if (!a || !b) return NO;
    NSColor *x = [a colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    NSColor *y = [b colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    return fabs(x.redComponent - y.redComponent) < 0.01 && fabs(x.greenComponent - y.greenComponent) < 0.01 &&
           fabs(x.blueComponent - y.blueComponent) < 0.01;
}

NSUInteger Attachments(NSTextView *tv) {
    __block NSUInteger n = 0;
    [tv.textStorage enumerateAttribute:NSAttachmentAttributeName
                               inRange:NSMakeRange(0, tv.textStorage.length) options:0
                            usingBlock:^(id value, NSRange, BOOL *) { if (value) n++; }];
    return n;
}

}  // namespace

static void NotebookSteps(void) {
    MCGuiStep(@"a notebook opens rendered, read-only", ^{
        MCGuiWriteFile(Fixture(@"analysis.ipynb"), NotebookText());
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"analysis.ipynb")]);
        GCHECK_EQ_STR(ActiveKind(c), @"notebook");
        NSTextView *tv = EditorText(c);
        GCHECK(!tv.editable);
        GCHECK(!EditorScroll(c).hidden);
        NSString *s = tv.string;
        GCHECK([s containsString:@"Analysis\n"]);
        GCHECK(![s containsString:@"# Analysis"]);              // a heading, not text
        GCHECK([s containsString:@"def double(x):"]);
        GCHECK([s containsString:@"42"]);
        GCHECK([s containsString:@"cell one"]);                  // the HTML's text
        GCHECK([s containsString:@"KeyError: 'k'"]);
        GCHECK(![s containsString:@"\x1b"]);                     // colors stripped
        GCHECK(![s containsString:@"\"cell_type\""]);            // no JSON on show
        GCHECK(Attachments(tv) == 1);                            // the plot
        MCGuiSnapshot(c.window.contentView, @"previews-notebook");
    });
    MCGuiStep(@"its code is colored by the editor's highlighter", ^{
        EditorController *c = MCGuiController();
        NSTextView *tv = EditorText(c);
        AppSettings *cfg = [AppSettings shared];
        GCHECK(SameColor(ColorOf(tv, @"def", 0), [cfg syntax:TokenStyle::Keyword]));
        GCHECK(SameColor(ColorOf(tv, @"return", 0), [cfg syntax:TokenStyle::Keyword]));
        GCHECK(SameColor(ColorOf(tv, @"2", [tv.string rangeOfString:@"x * "].location),
                         [cfg syntax:TokenStyle::Number]));
        // Output is not code: "42" keeps the code block's own color.
        const NSUInteger out = [tv.string rangeOfString:@"\n42"].location + 1;
        GCHECK(!SameColor(ColorOf(tv, @"42", out), [cfg syntax:TokenStyle::Number]));
    });
    MCGuiStep(@"saved unedited it is untouched; Shift+Cmd+V shows its JSON", ^{
        EditorController *c = MCGuiController();
        NSData *before = [NSData dataWithContentsOfFile:Fixture(@"analysis.ipynb")];
        [c saveCurrentFile:nil];
        GCHECK([[NSData dataWithContentsOfFile:Fixture(@"analysis.ipynb")] isEqualToData:before]);
        GCHECK(c.canTogglePreview);
        [c togglePreview:nil];
        NSTextView *tv = EditorText(c);
        GCHECK(tv.editable);
        GCHECK([tv.string containsString:@"\"cell_type\""]);
        [c togglePreview:nil];
        GCHECK(!tv.editable);
        GCHECK([tv.string containsString:@"def double(x):"]);
    });
    MCGuiStep(@"a file that is not a notebook says so", ^{
        MCGuiWriteFile(Fixture(@"broken.ipynb"), @"{\"cells\": [");
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"broken.ipynb")]);
        GCHECK([EditorText(c).string containsString:@"This notebook cannot be read"]);
    });
    MCGuiStep(@"a Markdown file's fenced code is colored too", ^{
        MCGuiWriteFile(Fixture(@"notes.md"), @"# Notes\n\n```python\nimport os\nx = 10\n```\n\n"
                                             @"```\nimport plain\n```\n");
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"notes.md")]);
        NSTextView *tv = EditorText(c);
        AppSettings *cfg = [AppSettings shared];
        GCHECK(SameColor(ColorOf(tv, @"import", 0), [cfg syntax:TokenStyle::Keyword]));
        GCHECK(SameColor(ColorOf(tv, @"10", 0), [cfg syntax:TokenStyle::Number]));
        // A block with no language stays plain.
        const NSUInteger plain = [tv.string rangeOfString:@"import plain"].location;
        GCHECK(!SameColor(ColorOf(tv, @"import", plain), [cfg syntax:TokenStyle::Keyword]));
    });
    MCGuiStep(@"headings get the core's anchors, as on Linux and Android", ^{
        MCGuiWriteFile(Fixture(@"anchors.md"), @"# Résumé & Notes\n\ntext\n\n## Résumé & Notes\n\n"
                                               @"## Plan: v2 (draft)\n");
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"anchors.md")]);
        NSDictionary *anchors = [c valueForKey:@"markdownAnchors"];
        GCHECK(anchors[@"résumé--notes"] != nil);
        GCHECK(anchors[@"résumé--notes-1"] != nil);   // a repeat is numbered
        GCHECK(anchors[@"plan-v2-draft"] != nil);
        GCHECK(anchors.count == 3);
    });
}

// ---------------------------------------------------------------- SQLite
namespace {

MCSqliteView *Sqlite(EditorController *c) { return Viewers(c).sqlite; }

BOOL MakeDatabase(NSString *path) {
    [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    sqlite3 *db = nullptr;
    if (sqlite3_open(path.fileSystemRepresentation, &db) != SQLITE_OK) return NO;
    BOOL ok = sqlite3_exec(db,
        "CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT, age INTEGER, photo BLOB);"
        "CREATE VIEW adults AS SELECT name, age FROM people WHERE age >= 18;"
        "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 1500) "
        "INSERT INTO people (id, name, age) SELECT i, 'person ' || i, i % 90 FROM n;"
        "UPDATE people SET name = NULL WHERE id = 2;"
        "UPDATE people SET name = 'NULL' WHERE id = 3;"
        "UPDATE people SET photo = x'00010203' WHERE id = 1;",
        nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(db);
    return ok;
}

long CountPeople(NSString *path) {
    sqlite3 *db = nullptr;
    long n = -1;
    if (sqlite3_open_v2(path.fileSystemRepresentation, &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT count(*) FROM people", -1, &st, nullptr) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW)
            n = (long)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return n;
}

void WaitForQuery(NSString *what) {
    MCGuiWait(what, 10, ^BOOL {
        MCSqliteView *s = Sqlite(MCGuiController());
        return s && !s.busy;
    });
}

}  // namespace

static void SqliteSteps(void) {
    __block NSData *dbBytes = nil;
    MCGuiStep(@"a database is made", ^{
        GCHECK(MakeDatabase(Fixture(@"shop.db")));
        dbBytes = [NSData dataWithContentsOfFile:Fixture(@"shop.db")];
        [dbBytes writeToFile:Fixture(@"unnamed.bin") atomically:YES];
        GCHECK(dbBytes.length > 0);
    });
    MCGuiStep(@"a .db opens read-only, in the background", ^{
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"shop.db")]);
        GCHECK_EQ_STR(ActiveKind(c), @"sqlite");
        MCSqliteView *s = Sqlite(c);
        GCHECK(s != nil && !s.hidden);
        GCHECK(EditorScroll(c).hidden);
        GCHECK(!c.canTogglePreview);
        GCHECK(s.busy);   // opening is not done on the main thread
    });
    WaitForQuery(@"the first table's rows");
    MCGuiStep(@"its tables and views", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        GCHECK([s.objectNames isEqualToArray:(@[@"people", @"adults"])]);
        GCHECK([s.schemaText hasPrefix:@"CREATE TABLE people"]);
    });
    MCGuiStep(@"the first 1,000 rows of the table", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        GCHECK(s.rows.count == MCSqliteMaxRows);
        GCHECK([s.columns isEqualToArray:(@[@"id", @"name", @"age", @"photo"])]);
        GCHECK_EQ_STR(s.statusText, @"The first 1,000 rows");
        GCHECK(s.rowsTable.numberOfRows == 1000 && s.rowsTable.numberOfColumns == 4);
        GCHECK_EQ_STR(s.rows.count > 1 ? s.rows[1][1] : nil, @"NULL");
        GCHECK_EQ_STR(s.rows.count ? s.rows[0][3] : nil, @"<4 bytes>");
        // SQL NULL and the text "NULL" are told apart.
        GCHECK([s isNullAtRow:1 column:1]);
        GCHECK_EQ_STR(s.rows.count > 2 ? s.rows[2][1] : nil, @"NULL");
        GCHECK(![s isNullAtRow:2 column:1]);
        MCGuiSnapshot(MCGuiController().window.contentView, @"previews-sqlite");
    });
    MCGuiStep(@"a query typed in the box runs", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        s.queryField.stringValue = @"SELECT count(*) AS n, max(age) FROM people";
        [s.queryField sendAction:s.queryField.action to:s.queryField.target];
    });
    WaitForQuery(@"the count");
    MCGuiStep(@"its answer", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        GCHECK(s.rows.count == 1);
        GCHECK_EQ_STR(s.rows.count ? s.rows[0][0] : nil, @"1500");
        GCHECK_EQ_STR(s.columns.firstObject, @"n");
        GCHECK([s.statusText hasPrefix:@"1 row, "]);
        [s runQuery:@"DELETE FROM people"];
    });
    WaitForQuery(@"a delete");
    MCGuiStep(@"a statement that writes is refused, and the file is unchanged", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        GCHECK([s.statusText hasPrefix:@"Not run: the database is open read-only"]);
        GCHECK(CountPeople(Fixture(@"shop.db")) == 1500);
        [s runQuery:@"CREATE TABLE sneaky (x)"];
    });
    WaitForQuery(@"a create");
    MCGuiStep(@"so is a CREATE", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        GCHECK([s.statusText hasPrefix:@"Not run"]);
        s.timeLimit = 0.3;
        [s runQuery:@"WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) "
                    @"SELECT count(*) FROM c"];
    });
    WaitForQuery(@"a query that never ends");
    MCGuiStep(@"a query past its time limit is stopped", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        GCHECK_EQ_STR(s.statusText, @"Stopped: the query ran longer than 0.3 seconds.");
        s.timeLimit = 5;
        [s runQuery:@"SELEC nonsense"];
    });
    WaitForQuery(@"a syntax error");
    MCGuiStep(@"SQLite's own complaint for a mistake", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        GCHECK([s.statusText containsString:@"syntax error"]);
        [s runQuery:@"BEGIN"];
    });
    // What changes no bytes but would reach past the viewer is refused too:
    // a BEGIN would hold a lock that other writers of the file wait on.
    WaitForQuery(@"a BEGIN");
    MCGuiStep(@"a BEGIN is refused", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        GCHECK([s.statusText hasPrefix:@"Not run: the database is open read-only"]);
        [s runQuery:@"SAVEPOINT here"];
    });
    WaitForQuery(@"a SAVEPOINT");
    MCGuiStep(@"so is a SAVEPOINT", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        GCHECK([s.statusText hasPrefix:@"Not run"]);
        [s runQuery:@"ATTACH ':memory:' AS other"];
    });
    WaitForQuery(@"an ATTACH");
    MCGuiStep(@"so is an ATTACH", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        GCHECK([s.statusText hasPrefix:@"Not run"]);
        [s runQuery:@"PRAGMA query_only = OFF"];
    });
    WaitForQuery(@"a pragma that sets something");
    MCGuiStep(@"so is a pragma that sets something", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        GCHECK([s.statusText hasPrefix:@"Not run"]);
        [s runQuery:@"PRAGMA table_info(people)"];
    });
    WaitForQuery(@"a pragma that reads");
    MCGuiStep(@"a pragma that only reads runs", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        GCHECK(s.rows.count == 4);
        GCHECK_EQ_STR(s.rows.count > 1 ? s.rows[1][1] : nil, @"name");
        // Another connection can still take the write lock: nothing is held.
        sqlite3 *db = nullptr;
        GCHECK(sqlite3_open(Fixture(@"shop.db").fileSystemRepresentation, &db) == SQLITE_OK);
        sqlite3_busy_timeout(db, 0);
        GCHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        [s.objectsTable selectRowIndexes:[NSIndexSet indexSetWithIndex:1] byExtendingSelection:NO];
    });
    WaitForQuery(@"the view");
    MCGuiStep(@"a click on the view shows it", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        GCHECK([s.schemaText hasPrefix:@"CREATE VIEW adults"]);
        GCHECK([s.columns isEqualToArray:(@[@"name", @"age"])]);
        GCHECK(s.rows.count == MCSqliteMaxRows);
    });
    MCGuiStep(@"found by its header whatever its name, and Cmd+S writes nothing", ^{
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"unnamed.bin")]);
        GCHECK_EQ_STR(ActiveKind(c), @"sqlite");
        // Another database: the query box is not the last one's.
        GCHECK_EQ_STR(Sqlite(c).queryField.stringValue, @"");
        [c saveCurrentFile:nil];
    });
    WaitForQuery(@"the other database");
    MCGuiStep(@"back to the first database", ^{
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"shop.db")]);   // back: the database again
        GCHECK_EQ_STR(ActiveKind(c), @"sqlite");
        [c saveCurrentFile:nil];
    });
    WaitForQuery(@"the table again");
    MCGuiStep(@"it comes back to the view on show, and the query typed", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        GCHECK([s.objectNames containsObject:@"people"]);
        GCHECK(s.objectsTable.selectedRow == 1);
        GCHECK([s.schemaText hasPrefix:@"CREATE VIEW adults"]);
        GCHECK([s.columns isEqualToArray:(@[@"name", @"age"])]);
        GCHECK_EQ_STR(s.queryField.stringValue, @"SELECT count(*) AS n, max(age) FROM people");
    });
    MCGuiStep(@"the database file is byte for byte as made", ^{
        GCHECK([[NSData dataWithContentsOfFile:Fixture(@"shop.db")] isEqualToData:dbBytes]);
        GCHECK([[NSData dataWithContentsOfFile:Fixture(@"unnamed.bin")] isEqualToData:dbBytes]);
        NSFileManager *fm = [NSFileManager defaultManager];
        GCHECK(![fm fileExistsAtPath:Fixture(@"shop.db-journal")]);
        GCHECK(![fm fileExistsAtPath:Fixture(@"shop.db-wal")]);
    });

    // ---- a database that changes on disk
    MCGuiStep(@"a database another program writes", ^{
        GCHECK(MakeDatabase(Fixture(@"live.db")));
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"live.db")]);
    });
    WaitForQuery(@"the live database");
    MCGuiStep(@"a query on it", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        s.queryField.stringValue = @"SELECT count(*) AS n FROM people";
        [s.queryField sendAction:s.queryField.action to:s.queryField.target];
    });
    WaitForQuery(@"the live count");
    MCGuiStep(@"the program adds a row; the tab reads the file again", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        GCHECK_EQ_STR(s.rows.count ? s.rows[0][0] : nil, @"1500");
        NSString *path = Fixture(@"live.db");
        sqlite3 *db = nullptr;
        GCHECK(sqlite3_open(path.fileSystemRepresentation, &db) == SQLITE_OK);
        GCHECK(sqlite3_exec(db, "INSERT INTO people (id, name, age) VALUES (2000, 'new', 30)",
                            nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
        [[NSFileManager defaultManager] setAttributes:@{NSFileModificationDate:
                                                            [NSDate dateWithTimeIntervalSinceNow:5]}
                                         ofItemAtPath:path error:nil];
        [MCGuiController() performSelector:@selector(checkExternalChange)];
    });
    WaitForQuery(@"the count again");
    MCGuiStep(@"the query on show is run again on the new file", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        GCHECK_EQ_STR(s.rows.count ? s.rows[0][0] : nil, @"1501");
        GCHECK_EQ_STR(s.queryField.stringValue, @"SELECT count(*) AS n FROM people");
        GCHECK_EQ_STR(s.columns.firstObject, @"n");
    });

    // ---- WAL, and a file SQLite cannot open
    __block NSData *walBytes = nil;
    MCGuiStep(@"a WAL database whose writer has closed", ^{
        NSString *path = Fixture(@"wal.db");
        NSFileManager *fm = [NSFileManager defaultManager];
        for (NSString *x in @[@"", @"-wal", @"-shm"])
            [fm removeItemAtPath:[path stringByAppendingString:x] error:nil];
        sqlite3 *db = nullptr;
        GCHECK(sqlite3_open(path.fileSystemRepresentation, &db) == SQLITE_OK);
        int keep = 0;   // the last connection removes -wal and -shm, as most builds do
        sqlite3_file_control(db, "main", SQLITE_FCNTL_PERSIST_WAL, &keep);
        GCHECK(sqlite3_exec(db, "PRAGMA journal_mode = WAL; CREATE TABLE notes (t TEXT);"
                                "INSERT INTO notes VALUES ('kept in WAL mode');",
                            nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
        // Should this SQLite keep them anyway, the close checkpointed: they go.
        [fm removeItemAtPath:[path stringByAppendingString:@"-wal"] error:nil];
        [fm removeItemAtPath:[path stringByAppendingString:@"-shm"] error:nil];
        walBytes = [NSData dataWithContentsOfFile:path];
        const unsigned char *b = (const unsigned char *)walBytes.bytes;
        GCHECK(walBytes.length > 20 && b[18] == 2 && b[19] == 2);
        GCHECK([MCGuiController() showFile:path]);
        GCHECK_EQ_STR(ActiveKind(MCGuiController()), @"sqlite");
    });
    WaitForQuery(@"the WAL database");
    MCGuiStep(@"it opens, read-only, and leaves no file beside it", ^{
        MCSqliteView *s = Sqlite(MCGuiController());
        GCHECK([s.objectNames isEqualToArray:@[@"notes"]]);
        GCHECK_EQ_STR(s.rows.count ? s.rows[0][0] : nil, @"kept in WAL mode");
        NSFileManager *fm = [NSFileManager defaultManager];
        NSString *path = Fixture(@"wal.db");
        GCHECK(![fm fileExistsAtPath:[path stringByAppendingString:@"-wal"]]);
        GCHECK(![fm fileExistsAtPath:[path stringByAppendingString:@"-shm"]]);
        GCHECK([[NSData dataWithContentsOfFile:path] isEqualToData:walBytes]);
    });
    MCGuiStep(@"a file with a SQLite header that is not a database", ^{
        std::string bytes("SQLite format 3\0", 16);
        bytes += std::string(200, 'x');
        WriteBytes(Fixture(@"broken.db"), bytes.data(), bytes.size());
        EditorController *c = MCGuiController();
        GCHECK([c showFile:Fixture(@"broken.db")]);
        GCHECK_EQ_STR(ActiveKind(c), @"sqlite");
    });
    WaitForQuery(@"the broken database");
    MCGuiStep(@"says why in the tab", ^{
        EditorController *c = MCGuiController();
        GCHECK_EQ_STR(ActiveKind(c), @"sqlite");
        GCHECK([Sqlite(c).statusText hasPrefix:@"This database cannot be opened: "]);
        GCHECK(!Sqlite(c).hidden);
    });
}
