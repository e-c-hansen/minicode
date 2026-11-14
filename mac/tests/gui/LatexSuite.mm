// LatexSuite.mm — `make guitest SUITES=latex`: the LaTeX preview's safety
// (Latex.mm), with a stand-in tectonic that records how it was run and
// copies a fixture PDF to --outdir. Checked: --untrusted is passed; two
// files named paper.tex in different folders get output folders of their
// own and each shows its own PDF; the hidden sibling and its record exist
// only while tectonic runs; a closed preview's folder goes; a sibling a
// dead process left is removed at the first typeset of a launch, while a
// user's own file of that name and a live process's copy are not; and a
// tectonic download whose bytes do not match the pinned checksum is
// refused with nothing installed. MC_GUI_TECTONIC_ARCHIVE=<the real
// tectonic-0.17.0 archive> adds the install itself.
//
// Forward search and problems on lines, with a stand-in that writes a
// two-page PDF and SyncTeX for it, or fails as tectonic does: Option+Cmd+J
// from line 5 shows page 2 with the line lit up, then not; from the
// preamble, page 1; a failed typeset lists its problem on the user's file,
// the failure panel's link opens the source at the line, the gutter dot
// follows an edit above it, and another tab, or the edited text later, has
// none. MC_GUI_REAL_TECTONIC=<a tectonic> (TECTONIC_CACHE_DIR at a scratch
// cache) adds both on real typesets, and the warnings of one that works.
//
// Two windows previewing one file with different unsaved text each typeset
// from a sibling and into a folder of their own, and each shows its own PDF.
// A typeset that works lists its warnings (tests/latex/logs/warnings.*, as
// tectonic printed and kept them) behind the Warnings button, yellow rows
// whose links open the line, with yellow dots in the gutter. On the
// committed fixture (tests/latex/fixtures: a document with two \input
// files, its tectonic PDF and SyncTeX), a double-click on a word opens its
// source and an edit there is one splice of the buffer, which turns dirty;
// and Option+Cmd+J in an \input file shows its line in the main
// document's preview, found by a "% !TEX root" comment or by the open
// preview whose typeset read the file.
#import "Harness.h"
#import "Lsp.h"   // MCDiagnosticColor
#import "EditorController.h"
#import "EditorController+Private.h"
#import "EditorTab.h"
#import "Latex.h"
#import <Quartz/Quartz.h>
#include "LatexScratch.h"
#include <CommonCrypto/CommonDigest.h>
#include <string>
#include <sys/sysctl.h>
#include <unistd.h>

namespace {

NSString *Scratch(NSString *rel) { return [MCGuiScratch() stringByAppendingPathComponent:rel]; }
NSString *Project(NSString *rel) { return [MCGuiProject() stringByAppendingPathComponent:rel]; }
NSString *Log(void) { return Scratch(@"latex/tectonic.log"); }
NSString *ReadText(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] ?: @"";
}
BOOL Exists(NSString *path) { return [[NSFileManager defaultManager] fileExistsAtPath:path]; }
MCTab *Active(EditorController *c) { return [c valueForKey:@"activeTab"]; }

// A one-page PDF whose text is `word`, as PDFKit reads text back.
void WriteTextPDF(NSString *path, NSString *word, CGFloat height = 80) {
    NSTextView *tv = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 300, height)];
    tv.string = word;
    tv.font = [NSFont systemFontOfSize:24];
    [[tv dataWithPDFInsideRect:tv.bounds] writeToFile:path atomically:YES];
}

std::string Sha256(NSData *d) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(d.bytes, (CC_LONG)d.length, digest);
    char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return hex;
}

double ProcessStart(pid_t pid) {
    struct kinfo_proc info;
    size_t size = sizeof info;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    if (sysctl(mib, 4, &info, &size, NULL, 0) != 0 || size == 0) return 0;
    return (double)info.kp_proc.p_starttime.tv_sec + info.kp_proc.p_starttime.tv_usec / 1e6;
}

// A record as a preview of process `pid` would leave it, in a folder of the
// work root, for `sibling` holding `bytes` when it was written.
void WriteRecord(NSString *folderName, NSString *sibling, NSData *bytes, pid_t pid, double start) {
    LatexScratch::Record r;
    r.sibling = sibling.UTF8String;
    r.pid = pid;
    r.start = start;
    r.size = bytes.length;
    r.sha256 = Sha256(bytes);
    const std::string text = LatexScratch::format(r);
    NSString *dir = [MCLatexWorkRoot() stringByAppendingPathComponent:folderName];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                               attributes:nil error:nil];
    [[NSData dataWithBytes:text.data() length:text.size()]
        writeToFile:[dir stringByAppendingPathComponent:@"sibling.record"] atomically:YES];
}

// The repository's tests/latex folder.
NSString *Fixtures(NSString *rel) {
    return [[MCGuiBuildDir().stringByDeletingLastPathComponent
        stringByAppendingPathComponent:@"tests/latex"] stringByAppendingPathComponent:rel];
}

// The text view showing the preview's log or problem list, when on show.
NSTextView *ShownLog(LatexView *v) {
    for (NSView *sub in v.subviews)
        if ([sub isKindOfClass:[NSScrollView class]] && !sub.isHidden &&
            [((NSScrollView *)sub).documentView isKindOfClass:[NSTextView class]])
            return (NSTextView *)((NSScrollView *)sub).documentView;
    return nil;
}

// The PDF's name for a preview: its sibling's, as tectonic names it.
NSString *PdfName(LatexView *v) {
    return [v.siblingPath.lastPathComponent.stringByDeletingPathExtension
        stringByAppendingPathExtension:@"pdf"];
}

NSString *const kStale = @"stale/.old.minicode.tex";    // a dead process's copy
NSString *const kMine = @"stale/.mine.minicode.tex";    // the user's, changed since
NSString *const kLive = @"stale/.live.minicode.tex";    // a running process's

}  // namespace

// The preview's own steps, driven as a double-click and the popover's Save
// would drive them (CLAUDE.md, "Headless").
@interface LatexView (LatexSuite)
- (void)openEditorForPage:(PDFPage *)page point:(NSPoint)pagePoint;
- (void)commitEdit:(id)sender;
- (void)toggleWarnings:(id)sender;
@end

@interface NSObject (LatexSuiteWindows)
- (EditorController *)openWindowAtPath:(NSString *)path;
@end

MC_GUI_SUITE(latex) {
    __block MCTab *tabA = nil, *tabB = nil;
    NSString *a = Project(@"latex-a/paper.tex");
    NSString *b = Project(@"latex-b/paper.tex");

    MCGuiStep(@"a stand-in tectonic, two paper.tex files and a crash's leftovers", ^{
        setenv("MINICODE_LATEX_WORK", Scratch(@"latex/work").UTF8String, 1);
        NSString *pdfs = Scratch(@"latex/pdfs");
        [[NSFileManager defaultManager] createDirectoryAtPath:pdfs withIntermediateDirectories:YES
                                                   attributes:nil error:nil];
        WriteTextPDF([pdfs stringByAppendingPathComponent:@"alpha.pdf"], @"ALPHA");
        WriteTextPDF([pdfs stringByAppendingPathComponent:@"beta.pdf"], @"BETA");
        // It logs its arguments, whether the sibling and its record were
        // there while it ran, then "typesets" by copying a PDF.
        NSString *script = [NSString stringWithFormat:
            @"#!/bin/sh\n"
            @"out=''; prev=''; input=''\n"
            @"for a in \"$@\"; do [ \"$prev\" = --outdir ] && out=\"$a\"; prev=\"$a\"; input=\"$a\"; done\n"
            @"{ printf 'ARGS'; for a in \"$@\"; do printf ' [%%s]' \"$a\"; done; printf '\\n';\n"
            @"  [ -f \"$input\" ] && echo SIBLING-THERE; [ -f \"$out/sibling.record\" ] && echo RECORD-THERE;\n"
            @"} >> '%@'\n"
            @"stem=$(basename \"$input\" .tex)\n"
            @"if grep -q ALPHA \"$input\"; then cp '%@/alpha.pdf' \"$out/$stem.pdf\";\n"
            @"else cp '%@/beta.pdf' \"$out/$stem.pdf\"; fi\n",
            Log(), pdfs, pdfs];
        NSString *tool = Scratch(@"latex/tectonic");
        MCGuiWriteFile(tool, script);
        chmod(tool.fileSystemRepresentation, 0755);
        setenv("MINICODE_TECTONIC", tool.UTF8String, 1);
        GCHECK_EQ_STR(MCTectonicPath(), tool);

        MCGuiWriteFile(a, @"\\documentclass{article}\\begin{document}ALPHA\\end{document}\n");
        MCGuiWriteFile(b, @"\\documentclass{article}\\begin{document}BETA\\end{document}\n");

        // What crashed previews left: our copy (removed), one the user has
        // changed since (kept), and a live process's (kept).
        pid_t dead = fork();
        if (dead == 0) _exit(0);
        waitpid(dead, nullptr, 0);
        NSData *old = [@"\\documentclass{article}old" dataUsingEncoding:NSUTF8StringEncoding];
        MCGuiWriteFile(Project(kStale), @"\\documentclass{article}old");
        WriteRecord(@"aaaaaaaaaaaaaaaa", Project(kStale), old, dead, 1000);
        MCGuiWriteFile(Project(kMine), @"\\documentclass{article}the user's own");
        WriteRecord(@"bbbbbbbbbbbbbbbb", Project(kMine), old, dead, 1000);
        MCGuiWriteFile(Project(kLive), @"\\documentclass{article}old");
        WriteRecord(@"cccccccccccccccc", Project(kLive), old, getpid(), ProcessStart(getpid()));
        // Output folders a preview of the dead process left (no record: it
        // crashed between typesets), and one of a live process (this one's
        // parent: never this process's own, whose previews are counted).
        NSFileManager *fm = [NSFileManager defaultManager];
        NSString *root = MCLatexWorkRoot();
        [fm createDirectoryAtPath:[root stringByAppendingPathComponent:
                [NSString stringWithFormat:@"dddddddddddddddd-%d-1000-1", dead]]
            withIntermediateDirectories:YES attributes:nil error:nil];
        const pid_t parent = getppid();
        [fm createDirectoryAtPath:[root stringByAppendingPathComponent:
                [NSString stringWithFormat:@"eeeeeeeeeeeeeeee-%d-%lld-1", parent,
                                           (long long)floor(ProcessStart(parent))]]
            withIntermediateDirectories:YES attributes:nil error:nil];
    });

    MCGuiStep(@"the first paper.tex opens in the preview", ^{
        GCHECK([MCGuiController() showFile:a]);
        tabA = Active(MCGuiController());
        GCHECK(tabA.isLatex && tabA.previewMode && tabA.latex != nil);
    });
    MCGuiWait(@"it shows its own PDF", 20, ^BOOL {
        return [[tabA.latex latexPDFView].document.string containsString:@"ALPHA"];
    });

    __block NSString *folderA = nil, *folderB = nil;
    MCGuiStep(@"tectonic ran untrusted, in this preview's own folder", ^{
        NSString *log = ReadText(Log());
        GCHECK([log containsString:@"[--untrusted]"]);
        GCHECK([log containsString:@"[--keep-logs]"]);
        folderA = tabA.latex.workFolder;
        NSString *outArg = [NSString stringWithFormat:@"[--outdir] [%@]", folderA];
        GCHECK([log containsString:outArg]);
        GCHECK([folderA hasPrefix:Scratch(@"latex/work/")]);
        // Named for the file and the preview: ".paper.<pid>-<n>.minicode.tex".
        NSString *sibling = tabA.latex.siblingPath;
        GCHECK_EQ_STR(sibling.stringByDeletingLastPathComponent, Project(@"latex-a"));
        NSString *prefix = [NSString stringWithFormat:@".paper.%d-", getpid()];
        GCHECK([sibling.lastPathComponent hasPrefix:prefix] && [sibling hasSuffix:@".minicode.tex"]);
        NSString *siblingArg = [NSString stringWithFormat:@"[%@]", sibling];
        GCHECK([log containsString:siblingArg]);
        // The sibling and its record were there while it ran, and are gone.
        GCHECK([log containsString:@"SIBLING-THERE"] && [log containsString:@"RECORD-THERE"]);
        GCHECK(!Exists(sibling));
        GCHECK(!Exists([folderA stringByAppendingPathComponent:@"sibling.record"]));
        GCHECK(Exists([folderA stringByAppendingPathComponent:PdfName(tabA.latex)]));
        if (![log containsString:@"[--untrusted]"]) MCGuiNote(@"tectonic's log: %@", log);
    });

    MCGuiWait(@"the first typeset removed the dead process's copy", 10, ^BOOL {
        return !Exists(Project(kStale));
    });
    MCGuiStep(@"but not the user's own file or a live process's copy", ^{
        GCHECK(Exists(Project(kMine)));
        GCHECK_EQ_STR(ReadText(Project(kMine)), @"\\documentclass{article}the user's own");
        GCHECK(Exists(Project(kLive)));
        NSString *root = MCLatexWorkRoot();
        // The dead process's records (and their folders) are gone, the live
        // one's stays.
        GCHECK(!Exists([root stringByAppendingPathComponent:@"aaaaaaaaaaaaaaaa"]));
        GCHECK(!Exists([root stringByAppendingPathComponent:@"bbbbbbbbbbbbbbbb"]));
        GCHECK(Exists([root stringByAppendingPathComponent:@"cccccccccccccccc/sibling.record"]));
        // The dead process's preview folder is gone, the live one's stays.
        NSArray *names = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:root error:nil];
        BOOL deadLeft = NO, liveLeft = NO;
        for (NSString *n in names) {
            deadLeft |= [n hasPrefix:@"dddddddddddddddd-"];
            liveLeft |= [n hasPrefix:@"eeeeeeeeeeeeeeee-"];
        }
        GCHECK(!deadLeft);
        GCHECK(liveLeft);
        if (deadLeft || !liveLeft) MCGuiNote(@"work root holds %@", names);
    });

    MCGuiStep(@"the other paper.tex opens in a tab of its own", ^{
        GCHECK([MCGuiController() showFile:b]);
        tabB = Active(MCGuiController());
        GCHECK(tabB != tabA && tabB.isLatex && tabB.latex != nil && tabB.latex != tabA.latex);
    });
    MCGuiWait(@"it shows its own PDF", 20, ^BOOL {
        return [[tabB.latex latexPDFView].document.string containsString:@"BETA"];
    });
    MCGuiStep(@"and the first still shows its own", ^{
        GCHECK([[tabA.latex latexPDFView].document.string containsString:@"ALPHA"]);
        GCHECK(![[tabA.latex latexPDFView].document.string containsString:@"BETA"]);
        folderB = tabB.latex.workFolder;
        GCHECK(![folderA isEqualToString:folderB]);
        GCHECK(Exists([folderB stringByAppendingPathComponent:PdfName(tabB.latex)]));
        GCHECK(!Exists(tabB.latex.siblingPath));
    });

    // Two windows on one file, the second's buffer holding other, unsaved
    // text: each typesets its own text from its own sibling into its own
    // folder, so neither shows the other's PDF.
    __block EditorController *first = nil, *second = nil;
    __block MCTab *tabA2 = nil;
    MCGuiStep(@"a second window shows the first paper.tex, with other unsaved text", ^{
        first = MCGuiController();
        second = [(NSObject *)NSApp.delegate openWindowAtPath:MCGuiProject()];
        GCHECK(second != nil && second != first);
        GCHECK([second showFile:a]);
        tabA2 = Active(second);
        GCHECK(tabA2.isLatex && tabA2.latex != nil && tabA2.latex != tabA.latex);
        [tabA2.latex setPath:a source:@"\\documentclass{article}\\begin{document}BETA, "
                                      @"unsaved\\end{document}\n"];
    });
    MCGuiWait(@"it shows its own text's PDF", 20, ^BOOL {
        return [[tabA2.latex latexPDFView].document.string containsString:@"BETA"];
    });
    MCGuiStep(@"from a sibling and a folder of its own, the first window's untouched", ^{
        GCHECK(![tabA2.latex.workFolder isEqualToString:folderA]);
        GCHECK(![tabA2.latex.siblingPath isEqualToString:tabA.latex.siblingPath]);
        GCHECK_EQ_STR(tabA2.latex.siblingPath.stringByDeletingLastPathComponent,
                      tabA.latex.siblingPath.stringByDeletingLastPathComponent);
        NSString *log = ReadText(Log());
        NSString *siblingArg = [NSString stringWithFormat:@"[%@]", tabA2.latex.siblingPath];
        GCHECK([log containsString:siblingArg]);
        GCHECK(Exists([tabA2.latex.workFolder stringByAppendingPathComponent:PdfName(tabA2.latex)]));
        GCHECK(Exists([folderA stringByAppendingPathComponent:PdfName(tabA.latex)]));
        GCHECK(!Exists(tabA2.latex.siblingPath) && !Exists(tabA.latex.siblingPath));
        GCHECK([[tabA.latex latexPDFView].document.string containsString:@"ALPHA"]);
        [tabA.latex compileNow];   // the first typesets again meanwhile: still its own
    });
    MCGuiWait(@"the first window's preview typesets its own text again", 20, ^BOOL {
        return ![[tabA.latex valueForKey:@"compiling"] boolValue] &&
               [[tabA.latex latexPDFView].document.string containsString:@"ALPHA"];
    });
    __block NSString *folderA2 = nil;
    MCGuiStep(@"the second window closes", ^{
        GCHECK([[tabA2.latex latexPDFView].document.string containsString:@"BETA"]);
        folderA2 = tabA2.latex.workFolder;
        tabA2 = nil;
        [second.window close];
        second = nil;
        MCGuiSetKeyWindow(first.window);
        GCHECK(MCGuiController() == first);
    });
    MCGuiWait(@"taking its preview's folder, and leaving the first's", 10, ^BOOL {
        return !Exists(folderA2) && Exists(folderA);
    });

    MCGuiStep(@"closing the second preview", ^{
        GCHECK([MCGuiController() closeTab:tabB prompt:NO]);
        tabB = nil;
    });
    MCGuiWait(@"takes its output folder with it", 10, ^BOOL {
        return !Exists(folderB);
    });
    MCGuiStep(@"and leaves the first one's", ^{
        GCHECK(Exists(folderA));
        GCHECK([MCGuiController() closeTab:tabA prompt:NO]);
        tabA = nil;
    });
    MCGuiWait(@"until it closes too", 10, ^BOOL { return !Exists(folderA); });

    // ---- forward search and problems on lines (W22), with a second
    // stand-in: a document holding FORWARD typesets to two pages with a
    // SyncTeX file for them, one holding BROKEN fails as tectonic does.
    __block MCTab *tabF = nil, *tabE = nil;
    NSString *fwd = Project(@"latex-c/fwd.tex");
    NSString *broken = Project(@"latex-c/broken.tex");
    MCGuiStep(@"a stand-in that writes SyncTeX, and one that fails", ^{
        NSString *dir = Scratch(@"latex");
        NSString *p1 = [dir stringByAppendingPathComponent:@"p1.pdf"];
        NSString *p2 = [dir stringByAppendingPathComponent:@"p2.pdf"];
        // Pages taller than the pane, so the one on show is the one in view.
        WriteTextPDF(p1, @"PAGEONE", 1200);
        WriteTextPDF(p2, @"PAGETWO", 1200);
        PDFDocument *two = [[PDFDocument alloc] initWithURL:[NSURL fileURLWithPath:p1]];
        PDFDocument *second = [[PDFDocument alloc] initWithURL:[NSURL fileURLWithPath:p2]];
        [two insertPage:[second pageAtIndex:0] atIndex:1];
        GCHECK([two writeToFile:[dir stringByAppendingPathComponent:@"two.pdf"]]);
        // Line 3 on page 1; lines 5 and 6 one line of text on page 2, whose
        // box carries line 6. Boxes 10 pt in, baselines 40 pt down, 200 by
        // 12 pt.
        NSString *script =
            @"#!/bin/sh\n"
            @"out=''; prev=''; input=''\n"
            @"for a in \"$@\"; do [ \"$prev\" = --outdir ] && out=\"$a\"; prev=\"$a\"; input=\"$a\"; done\n"
            @"dir=$(dirname \"$0\"); stem=$(basename \"$input\" .tex)\n"
            @"if grep -q BROKEN \"$input\"; then\n"
            @"  echo \"error: $(basename \"$input\"):4: Undefined control sequence\"\n"
            @"  echo 'error: something bad happened inside XeTeX; its output follows:'\n"
            @"  echo '====='; echo \"($(basename \"$input\")\"; echo '! Undefined control sequence.'\n"
            @"  printf '%s\\n' 'l.4 \\foo' '====='\n"
            @"  echo 'error: the XeTeX engine had an unrecoverable error'\n"
            @"  exit 1\n"
            @"fi\n"
            @"cp \"$dir/two.pdf\" \"$out/$stem.pdf\"\n"
            @"printf 'SyncTeX Version:1\\nInput:1:%s\\nOutput:pdf\\nMagnification:1000\\nUnit:1\\n"
            @"X Offset:0\\nY Offset:0\\nContent:\\n{1\\n(1,4:655360,2621440:13107200,655360,131072\\n"
            @"g1,3:3000000,2621440\\n)\\n}1\\n{2\\n(1,6:655360,2621440:13107200,655360,131072\\n"
            @"g1,5:3000000,2621440\\ng1,6:9000000,2621440\\n)\\n}2\\n' \"$input\" | gzip > \"$out/$stem.synctex.gz\"\n";
        NSString *tool = Scratch(@"latex/tectonic2");
        MCGuiWriteFile(tool, script);
        chmod(tool.fileSystemRepresentation, 0755);
        setenv("MINICODE_TECTONIC", tool.UTF8String, 1);
        MCGuiWriteFile(fwd, @"\\documentclass{article}\n\\begin{document}\nFORWARD on page one\n"
                            @"\\newpage\nPage two,\nin one line.\n\\end{document}\n");
        MCGuiWriteFile(broken, @"\\documentclass{article}\n\\begin{document}\nBROKEN here\n"
                               @"\\foo{} and more\n\\end{document}\n");
    });
    MCGuiStep(@"the two-page document opens", ^{
        GCHECK([MCGuiController() showFile:fwd]);
        tabF = Active(MCGuiController());
        GCHECK(tabF.isLatex && tabF.latex != nil);
    });
    MCGuiWait(@"and typesets", 20, ^BOOL {
        return [tabF.latex latexPDFView].document.pageCount == 2;
    });
    MCGuiStep(@"to the source, the caret on line 5", ^{
        EditorController *c = MCGuiController();
        [c goToLine:5 column:0];
        GCHECK(!c.previewMode);
        GCHECK([c.textView.string lineRangeForRange:c.textView.selectedRange].location ==
               [c.textView.string rangeOfString:@"Page two"].location);
    });
    MCGuiStep(@"Option+Cmd+J", ^{
        EditorController *c = MCGuiController();
        GCHECK(MCGuiPerformMenuKey(c.window, 38 /* J */,
                                   NSEventModifierFlagOption | NSEventModifierFlagCommand, nil, nil));
    });
    MCGuiWait(@"shows page 2 with the line lit up", 10, ^BOOL {
        PDFView *pdf = [tabF.latex latexPDFView];
        return MCGuiController().previewMode && pdf.document &&
               [pdf.document indexForPage:pdf.currentPage] == 1 &&
               [pdf.document pageAtIndex:1].annotations.count == 1;
    });
    MCGuiStep(@"the light covers the line of text", ^{
        PDFView *pdf = [tabF.latex latexPDFView];
        MCGuiNote(@"preview %d, page %lu, annotations %lu, status: %@", MCGuiController().previewMode,
                  (unsigned long)[pdf.document indexForPage:pdf.currentPage],
                  (unsigned long)[pdf.document pageAtIndex:1].annotations.count,
                  [tabF.latex valueForKeyPath:@"statusLabel.stringValue"]);
        PDFPage *page = [[tabF.latex latexPDFView].document pageAtIndex:1];
        PDFAnnotation *a = page.annotations.firstObject;
        const NSRect media = [page boundsForBox:kPDFDisplayBoxMediaBox];
        // x 10 pt, top 30 pt down (40 less the 10 pt height), 12 pt tall.
        GCHECK(fabs(NSMinX(a.bounds) - 8.5) < 0.1);
        GCHECK(fabs(NSMaxY(a.bounds) - (media.size.height - 30 + 1.5)) < 0.1);
        GCHECK(fabs(NSHeight(a.bounds) - 15) < 0.1);
        GCHECK([page annotations].count == 1 &&
               [[tabF.latex latexPDFView].document pageAtIndex:0].annotations.count == 0);
        if (a) MCGuiNote(@"lit up %@ on a page of %@", NSStringFromRect(a.bounds),
                         NSStringFromRect(media));
    });
    MCGuiWait(@"for a moment", 5, ^BOOL {
        return [[tabF.latex latexPDFView].document pageAtIndex:1].annotations.count == 0;
    });
    MCGuiStep(@"from the preamble, from the preview itself", ^{
        EditorController *c = MCGuiController();
        [c goToLine:1 column:0];
        GCHECK(!c.previewMode);
        [c togglePreview:nil];
        GCHECK(c.previewMode);
        [c showLineInPDF:nil];
    });
    MCGuiWait(@"page 1, the first line that typeset something", 10, ^BOOL {
        PDFView *pdf = [tabF.latex latexPDFView];
        return [pdf.document indexForPage:pdf.currentPage] == 0 &&
               [pdf.document pageAtIndex:0].annotations.count == 1;
    });

    MCGuiStep(@"a document that does not typeset", ^{
        GCHECK([MCGuiController() showFile:broken]);
        tabE = Active(MCGuiController());
        GCHECK(tabE.isLatex && tabE.latex != nil && tabE != tabF);
    });
    MCGuiWait(@"lists its problem", 20, ^BOOL { return tabE.latex.problems.count == 1; });
    MCGuiStep(@"placed on the user's file, linked from the failure panel", ^{
        MCLatexProblem *p = tabE.latex.problems.firstObject;
        GCHECK_EQ_STR(p.path, broken);
        GCHECK(p.line == 4);
        GCHECK_EQ_STR(p.message, @"Undefined control sequence");
        GCHECK(p.severity == 1);
        GCHECK([tabE.latex latexPDFView].isHidden);
        NSTextView *log = nil;
        for (NSView *v in tabE.latex.subviews)
            if ([v isKindOfClass:[NSScrollView class]] && !v.isHidden)
                log = (NSTextView *)((NSScrollView *)v).documentView;
        GCHECK([log isKindOfClass:[NSTextView class]]);
        GCHECK([log.string hasPrefix:@"Line 4: Undefined control sequence\n"]);
        GCHECK([log.string containsString:@"l.4 \\foo"]);   // the log follows
        NSRange linked = NSMakeRange(NSNotFound, 0);
        id link = [log.textStorage attribute:NSLinkAttributeName atIndex:0
                               effectiveRange:&linked];
        GCHECK(link != nil && linked.location == 0 && linked.length == 6);   // "Line 4"
        // A click on it, as NSTextView reports one.
        GCHECK([log.delegate textView:log clickedOnLink:link atIndex:1]);
    });
    MCGuiStep(@"opens the source at that line, dotted in the gutter", ^{
        EditorController *c = MCGuiController();
        GCHECK(Active(c) == tabE && !c.previewMode && c.textView.editable);
        NSString *text = c.textView.string;
        const NSUInteger line4 = [text rangeOfString:@"\\foo"].location;
        GCHECK(c.textView.selectedRange.location == line4);
        GCHECK(c.textView.buildMarks.count == 1);
        MCDiagnosticMark *m = c.textView.buildMarks.firstObject;
        GCHECK(m.range.location == line4 && m.severity == 1);
        GCHECK(c.textView.diagnostics.count == 0);   // not the language server's
        // A line typed above moves the dot down with its text.
        MCGuiUndoGroup(c.textView.undoManager, ^{
            [c.textView setSelectedRange:NSMakeRange(0, 0)];
            [c.textView insertText:@"% a comment\n" replacementRange:NSMakeRange(0, 0)];
        });
        GCHECK(c.textView.buildMarks.count == 1 &&
               c.textView.buildMarks.firstObject.range.location ==
                   [c.textView.string rangeOfString:@"\\foo"].location);
    });
    MCGuiStep(@"another tab's source has no dots, and coming back to edited text has none", ^{
        EditorController *c = MCGuiController();
        [c showFile:fwd];
        GCHECK(Active(c) == tabF);
        if (c.previewMode) [c togglePreview:nil];
        GCHECK(c.textView.buildMarks.count == 0);
        [c showFile:broken];
        GCHECK(Active(c) == tabE);
        if (c.previewMode) [c togglePreview:nil];
        GCHECK(c.textView.buildMarks.count == 0);   // the text is not what failed
    });
    MCGuiStep(@"close them", ^{
        EditorController *c = MCGuiController();
        GCHECK([c closeTab:tabE prompt:NO]);
        GCHECK([c closeTab:tabF prompt:NO]);
        tabE = tabF = nil;
    });

    // ---- warnings, click-to-source and \input files on the committed
    // fixtures, with a third stand-in: a document with mypkg's warning
    // "typesets" printing and keeping tests/latex/logs/warnings.*, named for
    // its sibling; a document of its own gets tests/latex/fixtures/main.pdf
    // and its SyncTeX, placed in the document's folder; a part (no
    // \documentclass) fails as it would.
    __block MCTab *tabW = nil, *tabM = nil;
    NSString *warned = Project(@"latex-w/warned.tex");
    NSString *mainTex = Project(@"latex-fx/main.tex");
    NSString *chapter = Project(@"latex-fx/chapter.tex");
    NSString *appendix = Project(@"latex-fx/appendix.tex");
    MCGuiStep(@"a stand-in for the committed fixtures", ^{
        NSString *script = [NSString stringWithFormat:
            @"#!/bin/sh\n"
            @"out=''; prev=''; input=''\n"
            @"for a in \"$@\"; do [ \"$prev\" = --outdir ] && out=\"$a\"; prev=\"$a\"; input=\"$a\"; done\n"
            @"dir=$(dirname \"$input\"); stem=$(basename \"$input\" .tex); fx='%@'\n"
            @"if grep -q mypkg \"$input\"; then\n"
            @"  sed \"s/\\.warnings\\.minicode/$stem/g\" \"$fx/logs/warnings.txt\"\n"
            @"  sed \"s/\\.warnings\\.minicode/$stem/g\" \"$fx/logs/warnings.log\" > \"$out/$stem.log\"\n"
            @"  cp \"$fx/fixtures/main.pdf\" \"$out/$stem.pdf\"; exit 0\n"
            @"fi\n"
            @"if ! grep -q documentclass \"$input\"; then\n"
            @"  echo \"error: $(basename \"$input\"):1: Undefined control sequence\"; exit 1\n"
            @"fi\n"
            @"cp \"$fx/fixtures/main.pdf\" \"$out/$stem.pdf\"\n"
            @"sed -e \"s#@DIR@/.main.minicode.tex#$input#\" -e \"s#@DIR@#$dir#g\" "
            @"\"$fx/fixtures/main.synctex\" | gzip > \"$out/$stem.synctex.gz\"\n",
            Fixtures(@"")];
        NSString *tool = Scratch(@"latex/tectonic3");
        MCGuiWriteFile(tool, script);
        chmod(tool.fileSystemRepresentation, 0755);
        setenv("MINICODE_TECTONIC", tool.UTF8String, 1);
        for (NSString *rel in @[@"logs/warnings.tex", @"logs/parts/chapter.tex",
                                @"fixtures/main.tex", @"fixtures/chapter.tex",
                                @"fixtures/appendix.tex"])
            GCHECK(ReadText(Fixtures(rel)).length > 0);
        MCGuiWriteFile(warned, ReadText(Fixtures(@"logs/warnings.tex")));
        MCGuiWriteFile(Project(@"latex-w/parts/chapter.tex"),
                       ReadText(Fixtures(@"logs/parts/chapter.tex")));
        MCGuiWriteFile(mainTex, ReadText(Fixtures(@"fixtures/main.tex")));
        MCGuiWriteFile(chapter, ReadText(Fixtures(@"fixtures/chapter.tex")));
        MCGuiWriteFile(appendix, ReadText(Fixtures(@"fixtures/appendix.tex")));
    });

    MCGuiStep(@"a document that typesets with warnings", ^{
        GCHECK([MCGuiController() showFile:warned]);
        tabW = Active(MCGuiController());
        GCHECK(tabW.isLatex && tabW.latex != nil);
    });
    MCGuiWait(@"shows its page and lists its nine warnings", 20, ^BOOL {
        return tabW.latex.problems.count == 9 && [tabW.latex latexPDFView].document != nil;
    });
    MCGuiStep(@"each a warning, placed on the user's files", ^{
        NSArray<MCLatexProblem *> *ps = tabW.latex.problems;
        for (MCLatexProblem *p in ps) GCHECK(p.severity == 2);
        NSString *part = Project(@"latex-w/parts/chapter.tex");
        if (ps.count == 9) {
            GCHECK_EQ_STR(ps[0].path, part);   // tectonic's boxes first
            GCHECK(ps[0].line == 3);
            GCHECK_EQ_STR(ps[2].path, warned);
            GCHECK(ps[2].line == 13);          // the paragraph's first line
            GCHECK_EQ_STR(ps[3].path, warned); // the label defined twice: no line
            GCHECK(ps[3].line == 0);
            GCHECK_EQ_STR(ps[4].path, warned);
            GCHECK(ps[4].line == 4);
            GCHECK_EQ_STR(ps[4].message, @"LaTeX Warning: Reference `sec:missing' on page 1 undefined");
            GCHECK_EQ_STR(ps[7].path, part);
            GCHECK(ps[7].line == 1);
            GCHECK(ps[8].line == 16);
        }
        LatexView *v = tabW.latex;
        GCHECK(![v latexPDFView].isHidden);
        GCHECK([[v valueForKeyPath:@"statusLabel.stringValue"] containsString:@"9 warnings"]);
        NSButton *button = [v valueForKey:@"warningsButton"];
        GCHECK(!button.isHidden && [button.title isEqualToString:@"9 Warnings"]);
        MCGuiNote(@"status: %@", [v valueForKeyPath:@"statusLabel.stringValue"]);
    });
    MCGuiStep(@"the Warnings button lists them, yellow, each place a link", ^{
        LatexView *v = tabW.latex;
        [v toggleWarnings:nil];
        GCHECK([v latexPDFView].isHidden);
        NSTextView *log = ShownLog(v);
        GCHECK(log != nil);
        NSArray<NSString *> *rows = [log.string componentsSeparatedByString:@"\n"];
        GCHECK(rows.count >= 9);
        if (rows.count >= 9) {
            GCHECK_EQ_STR(rows[0], @"chapter.tex, line 3: Overfull \\hbox (86.74pt too wide) "
                                   @"detected at line 3");
            GCHECK_EQ_STR(rows[3], @"LaTeX Warning: Label `sec:intro' multiply defined");
            GCHECK([rows[4] hasPrefix:@"Line 4: LaTeX Warning: Reference `sec:missing'"]);
        }
        NSColor *ink = [log.textStorage attribute:NSForegroundColorAttributeName atIndex:0
                                   effectiveRange:nullptr];
        GCHECK([ink isEqual:MCDiagnosticColor(2)]);
        GCHECK(![ink isEqual:MCDiagnosticColor(1)]);
        GCHECK([[[v valueForKey:@"warningsButton"] title] isEqualToString:@"Show Page"]);
        // Row 5's place, "Line 4", clicked as NSTextView reports a click.
        const NSUInteger at = [log.string rangeOfString:@"Line 4:"].location;
        GCHECK(at != NSNotFound);
        if (at == NSNotFound) return;
        NSRange linked;
        id link = [log.textStorage attribute:NSLinkAttributeName atIndex:at effectiveRange:&linked];
        GCHECK(link != nil && linked.location == at && linked.length == 6);
        GCHECK([log.delegate textView:log clickedOnLink:link atIndex:at]);
    });
    MCGuiStep(@"opens the source at line 4, warnings dotted yellow in the gutter", ^{
        EditorController *c = MCGuiController();
        GCHECK(Active(c) == tabW && !c.previewMode);
        NSString *text = c.textView.string;
        GCHECK(c.textView.selectedRange.location == [text rangeOfString:@"See section"].location);
        // Lines 4 (twice), 7, 11, 13 and 16 of this file; the label's has no
        // line, the part's are the part's.
        NSArray<MCDiagnosticMark *> *marks = c.textView.buildMarks;
        GCHECK(marks.count == 6);
        NSMutableIndexSet *lines = [NSMutableIndexSet indexSet];
        for (MCDiagnosticMark *m in marks) {
            GCHECK(m.severity == 2);
            [lines addIndex:[[text substringToIndex:m.range.location]
                                componentsSeparatedByString:@"\n"].count];
        }
        NSMutableIndexSet *want = [NSMutableIndexSet indexSet];
        for (NSNumber *n in @[@4, @7, @11, @13, @16]) [want addIndex:n.unsignedIntegerValue];
        GCHECK([lines isEqualToIndexSet:want]);
        if (![lines isEqualToIndexSet:want]) MCGuiNote(@"dotted lines %@", lines);
    });
    MCGuiStep(@"close it", ^{
        GCHECK([MCGuiController() closeTab:tabW prompt:NO]);
        tabW = nil;
    });

    MCGuiStep(@"the fixture document opens", ^{
        GCHECK([MCGuiController() showFile:mainTex]);
        tabM = Active(MCGuiController());
        GCHECK(tabM.isLatex && tabM.latex != nil);
    });
    MCGuiWait(@"and shows its two pages", 20, ^BOOL {
        return [tabM.latex latexPDFView].document.pageCount == 2;
    });
    __block NSString *before = nil, *spanText = nil;
    MCGuiStep(@"a double-click on \"walrus\" opens its source", ^{
        LatexView *v = tabM.latex;
        PDFDocument *doc = [v latexPDFView].document;
        NSArray<PDFSelection *> *hits = [doc findString:@"walrus" withOptions:0];
        GCHECK(hits.count == 1);
        if (hits.count != 1) return;
        PDFPage *page = hits[0].pages.firstObject;
        const NSRect box = [hits[0] boundsForPage:page];
        before = tabM.sourceText;
        GCHECK(!tabM.dirty);
        [v openEditorForPage:page point:NSMakePoint(NSMidX(box), NSMidY(box))];
        NSTextView *edit = [v valueForKey:@"editText"];
        GCHECK(edit != nil);
        spanText = [edit.string copy];
        GCHECK([spanText containsString:@"walrus"]);
        GCHECK([before rangeOfString:spanText].location != NSNotFound);
        GCHECK(![spanText containsString:@"heron"]);   // its own paragraph only
        MCGuiNote(@"the span: \"%@\"", spanText);
        // The edit, saved as the popover's Return would.
        edit.string = [spanText stringByReplacingOccurrencesOfString:@"walrus"
                                                          withString:@"narwhal"];
        [v commitEdit:nil];
    });
    MCGuiStep(@"the edit is one splice of the buffer, which is now dirty", ^{
        GCHECK(spanText.length > 0);
        if (!spanText.length) return;
        const NSRange r = [before rangeOfString:spanText];
        NSString *want = [before stringByReplacingCharactersInRange:r
            withString:[spanText stringByReplacingOccurrencesOfString:@"walrus" withString:@"narwhal"]];
        GCHECK_EQ_STR(tabM.sourceText, want);
        GCHECK(tabM.dirty);
        GCHECK_EQ_STR(ReadText(mainTex), before);   // nothing saved
        GCHECK([tabM.latex canUndoEdit]);
    });
    MCGuiWait(@"and typesets again", 20, ^BOOL {
        return ![[tabM.latex valueForKey:@"compiling"] boolValue] &&
               [tabM.latex latexPDFView].document.pageCount == 2;
    });

    // Forward search from the \input files. The chapter names its main
    // document in a "% !TEX root" comment; the appendix does not, and is
    // found through the open preview whose typeset read it.
    __block MCTab *tabC = nil, *tabX = nil;
    MCGuiStep(@"the chapter, in its source, the caret on line 5", ^{
        EditorController *c = MCGuiController();
        GCHECK([c showFile:chapter]);
        tabC = Active(c);
        GCHECK(tabC != tabM);
        if (c.previewMode) [c togglePreview:nil];
        [c goToLine:5 column:0];
        GCHECK(!c.previewMode && Active(c) == tabC);
    });
    MCGuiStep(@"Option+Cmd+J", ^{
        EditorController *c = MCGuiController();
        GCHECK(MCGuiPerformMenuKey(c.window, 38 /* J */,
                                   NSEventModifierFlagOption | NSEventModifierFlagCommand, nil, nil));
    });
    MCGuiWait(@"shows the main document's preview with the chapter's line lit", 10, ^BOOL {
        EditorController *c = MCGuiController();
        PDFDocument *doc = [tabM.latex latexPDFView].document;
        return Active(c) == tabM && c.previewMode && [doc pageAtIndex:0].annotations.count == 1;
    });
    MCGuiStep(@"the light covers the chapter's line 5", ^{
        PDFPage *page = [[tabM.latex latexPDFView].document pageAtIndex:0];
        PDFAnnotation *a = page.annotations.firstObject;
        NSString *lit = [page selectionForRect:a.bounds].string ?: @"";
        GCHECK([lit containsString:@"lemur"]);
        GCHECK(![lit containsString:@"marmot"]);
        GCHECK([[tabM.latex valueForKeyPath:@"statusLabel.stringValue"]
                   hasPrefix:@"chapter.tex, line 5 is on page 1"]);
        MCGuiNote(@"lit \"%@\"; status %@", lit, [tabM.latex valueForKeyPath:@"statusLabel.stringValue"]);
    });
    MCGuiWait(@"for a moment", 5, ^BOOL {
        return [[tabM.latex latexPDFView].document pageAtIndex:0].annotations.count == 0;
    });
    MCGuiStep(@"the appendix, with no root comment, the caret on line 2", ^{
        EditorController *c = MCGuiController();
        GCHECK([c showFile:appendix]);
        tabX = Active(c);
        if (c.previewMode) [c togglePreview:nil];
        [c goToLine:2 column:0];
        GCHECK(!c.previewMode && Active(c) == tabX);
        [c showLineInPDF:nil];
    });
    MCGuiWait(@"lights its line on page 2 of the main document's preview", 10, ^BOOL {
        EditorController *c = MCGuiController();
        PDFDocument *doc = [tabM.latex latexPDFView].document;
        return Active(c) == tabM && c.previewMode && [doc pageAtIndex:1].annotations.count == 1;
    });
    MCGuiStep(@"on the appendix's kestrel", ^{
        PDFView *pdf = [tabM.latex latexPDFView];
        GCHECK([pdf.document indexForPage:pdf.currentPage] == 1);
        PDFPage *page = [pdf.document pageAtIndex:1];
        NSString *lit = [page selectionForRect:page.annotations.firstObject.bounds].string ?: @"";
        GCHECK([lit containsString:@"kestrel"]);
        MCGuiNote(@"lit \"%@\"", lit);
    });
    MCGuiStep(@"close the fixture's tabs", ^{
        EditorController *c = MCGuiController();
        GCHECK([c closeTab:tabX prompt:NO]);
        GCHECK([c closeTab:tabC prompt:NO]);
        GCHECK([c closeTab:tabM prompt:NO]);   // the edit is not saved
        tabX = tabC = tabM = nil;
        GCHECK_EQ_STR(ReadText(mainTex), ReadText(Fixtures(@"fixtures/main.tex")));
    });

    // MC_GUI_REAL_TECTONIC=<a tectonic> (with TECTONIC_CACHE_DIR at a scratch
    // cache) runs both on a real typeset: tests/latex/forward.tex's line 8
    // lights up "Page two text." on page 2, and an \input file's error is a
    // row naming that file, which opens it at the line.
    NSString *realTool = NSProcessInfo.processInfo.environment[@"MC_GUI_REAL_TECTONIC"];
    if (realTool.length) {
        __block MCTab *tabR = nil;
        NSString *real = Project(@"latex-real/forward.tex");
        NSString *main = Project(@"latex-real/main.tex");
        MCGuiStep(@"real tectonic: the forward search fixture", ^{
            setenv("MINICODE_TECTONIC", realTool.UTF8String, 1);
            GCHECK_EQ_STR(MCTectonicPath(), realTool);
            NSString *src = [NSString stringWithContentsOfFile:
                [MCGuiBuildDir().stringByDeletingLastPathComponent
                    stringByAppendingPathComponent:@"tests/latex/forward.tex"]
                encoding:NSUTF8StringEncoding error:nil];
            GCHECK(src.length > 0);
            MCGuiWriteFile(real, src ?: @"");
            MCGuiWriteFile(Project(@"latex-real/child.tex"), @"Child line one.\n\\badcommand\n");
            MCGuiWriteFile(main, @"\\documentclass{article}\n\\begin{document}\nIntro.\n"
                                 @"\\input{child}\n\\end{document}\n");
            GCHECK([MCGuiController() showFile:real]);
            tabR = Active(MCGuiController());
        });
        MCGuiWait(@"typesets two pages", 120, ^BOOL {
            return [tabR.latex latexPDFView].document.pageCount == 2;
        });
        MCGuiStep(@"line 8 is on page 2", ^{
            EditorController *c = MCGuiController();
            if ([tabR.latex latexPDFView].document.pageCount != 2)
                MCGuiNote(@"status: %@\nlog: %@", [tabR.latex valueForKeyPath:@"statusLabel.stringValue"],
                          [tabR.latex valueForKeyPath:@"logView.string"]);
            [c goToLine:8 column:0];
            [c showLineInPDF:nil];
            PDFView *pdf = [tabR.latex latexPDFView];
            GCHECK([pdf.document indexForPage:pdf.currentPage] == 1);
            PDFPage *page = [pdf.document pageAtIndex:1];
            GCHECK(page.annotations.count == 1);
            PDFAnnotation *a = page.annotations.firstObject;
            NSString *lit = [page selectionForRect:a.bounds].string;
            GCHECK([lit containsString:@"Page two text"]);
            MCGuiNote(@"line 8 lit up %@, holding \"%@\"", NSStringFromRect(a.bounds), lit);
            [c goToLine:5 column:0];
            [c showLineInPDF:nil];
            page = [pdf.document pageAtIndex:0];
            a = page.annotations.firstObject;
            lit = [page selectionForRect:a.bounds].string;
            GCHECK([lit containsString:@"Second paragraph"] && ![lit containsString:@"First"]);
            MCGuiNote(@"line 5 lit up \"%@\"", lit);
        });
        MCGuiStep(@"real tectonic: an error in an \\input file", ^{
            GCHECK([MCGuiController() closeTab:tabR prompt:NO]);
            GCHECK([MCGuiController() showFile:main]);
            tabR = Active(MCGuiController());
        });
        MCGuiWait(@"is a problem", 120, ^BOOL { return tabR.latex.problems.count > 0; });
        MCGuiStep(@"naming the child at its line, and opening it there", ^{
            MCLatexProblem *p = tabR.latex.problems.firstObject;
            GCHECK_EQ_STR(p.path, Project(@"latex-real/child.tex"));
            GCHECK(p.line == 2);
            GCHECK_EQ_STR(p.message, @"Undefined control sequence");
            tabR.latex.onOpenProblem(p.path, p.line);
            EditorController *c = MCGuiController();
            GCHECK_EQ_STR(c.currentPath, Project(@"latex-real/child.tex"));
            GCHECK(!c.previewMode);
            GCHECK(c.textView.selectedRange.location ==
                   [c.textView.string rangeOfString:@"\\badcommand"].location);
        });
        MCGuiStep(@"close the real ones", ^{
            EditorController *c = MCGuiController();
            [c closeTab:Active(c) prompt:NO];
            [c closeTab:tabR prompt:NO];
            tabR = nil;
        });
        // The warnings fixture's own source, typeset for real: the same
        // nine warnings the committed logs give, from TeX's kept transcript.
        NSString *realWarned = Project(@"latex-real-w/warned.tex");
        MCGuiStep(@"real tectonic: a document that typesets with warnings", ^{
            MCGuiWriteFile(realWarned, ReadText(Fixtures(@"logs/warnings.tex")));
            MCGuiWriteFile(Project(@"latex-real-w/parts/chapter.tex"),
                           ReadText(Fixtures(@"logs/parts/chapter.tex")));
            GCHECK([MCGuiController() showFile:realWarned]);
            tabR = Active(MCGuiController());
        });
        MCGuiWait(@"lists its nine warnings", 120, ^BOOL {
            return tabR.latex.problems.count == 9 && [tabR.latex latexPDFView].document != nil;
        });
        MCGuiStep(@"all warnings, on its lines and its part's", ^{
            NSUInteger inPart = 0;
            for (MCLatexProblem *p in tabR.latex.problems) {
                GCHECK(p.severity == 2);
                if ([p.path isEqualToString:Project(@"latex-real-w/parts/chapter.tex")]) inPart++;
            }
            GCHECK(inPart == 2);
            if (tabR.latex.problems.count != 9 || inPart != 2)
                for (MCLatexProblem *p in tabR.latex.problems)
                    MCGuiNote(@"%@:%ld %@", p.path, (long)p.line, p.message);
            GCHECK([MCGuiController() closeTab:tabR prompt:NO]);
            tabR = nil;
        });
    }

    // The download. The managed copy goes in Application Support under the
    // scratch home; never run it anywhere else.
    NSString *bin = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,
        NSUserDomainMask, YES).firstObject stringByAppendingPathComponent:@"MiniCode/bin"];
    const BOOL safe = [bin hasPrefix:[MCGuiScratch() stringByAppendingString:@"/"]] ||
                      [bin hasPrefix:[@"/private" stringByAppendingString:MCGuiScratch()]];
    MCGuiAsync(@"a download that does not match the checksum is refused", 20, ^(MCGuiDone done) {
        GCHECK(safe);
        if (!safe) { MCGuiNote(@"Application Support is %@, not in the scratch world", bin); done(); return; }
        NSString *fake = Scratch(@"latex/fake.tar.gz");
        MCGuiWriteFile(fake, @"not the tectonic archive");
        setenv("MINICODE_TECTONIC_DOWNLOAD", [NSURL fileURLWithPath:fake].absoluteString.UTF8String, 1);
        MCDownloadTectonic(^(NSString *problem) {
            GCHECK([problem containsString:@"checksum"]);
            if (![problem containsString:@"checksum"]) MCGuiNote(@"the download said: %@", problem);
            GCHECK(!Exists([bin stringByAppendingPathComponent:@"tectonic"]));
            NSArray *left = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:bin error:nil];
            GCHECK(left.count == 0);
            done();
        });
    });

    NSString *real = NSProcessInfo.processInfo.environment[@"MC_GUI_TECTONIC_ARCHIVE"];
    if (real.length) {
        MCGuiAsync(@"the real archive installs tectonic", 60, ^(MCGuiDone done) {
            if (!safe) { done(); return; }
            setenv("MINICODE_TECTONIC_DOWNLOAD", [NSURL fileURLWithPath:real].absoluteString.UTF8String, 1);
            MCDownloadTectonic(^(NSString *problem) {
                GCHECK(problem == nil);
                if (problem) MCGuiNote(@"the download said: %@", problem);
                NSString *tool = [bin stringByAppendingPathComponent:@"tectonic"];
                GCHECK([[NSFileManager defaultManager] isExecutableFileAtPath:tool]);
                NSArray *left = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:bin error:nil];
                GCHECK_EQ_STR([left componentsJoinedByString:@","], @"tectonic");
                done();
            });
        });
    }
    MCGuiStep(@"clean up", ^{
        unsetenv("MINICODE_TECTONIC_DOWNLOAD");
        unsetenv("MINICODE_TECTONIC");
    });
}
