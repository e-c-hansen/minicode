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
// cache) adds both on real typesets.
#import "Harness.h"
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

NSString *const kStale = @"stale/.old.minicode.tex";    // a dead process's copy
NSString *const kMine = @"stale/.mine.minicode.tex";    // the user's, changed since
NSString *const kLive = @"stale/.live.minicode.tex";    // a running process's

}  // namespace

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
    });

    MCGuiStep(@"the first paper.tex opens in the preview", ^{
        GCHECK([MCGuiController() showFile:a]);
        tabA = Active(MCGuiController());
        GCHECK(tabA.isLatex && tabA.previewMode && tabA.latex != nil);
    });
    MCGuiWait(@"it shows its own PDF", 20, ^BOOL {
        return [[tabA.latex latexPDFView].document.string containsString:@"ALPHA"];
    });

    MCGuiStep(@"tectonic ran untrusted, in this file's own folder", ^{
        NSString *log = ReadText(Log());
        GCHECK([log containsString:@"[--untrusted]"]);
        NSString *folderA = MCLatexWorkFolder(a);
        NSString *outArg = [NSString stringWithFormat:@"[--outdir] [%@]", folderA];
        GCHECK([log containsString:outArg]);
        GCHECK([folderA hasPrefix:Scratch(@"latex/work/")]);
        GCHECK(![folderA isEqualToString:MCLatexWorkFolder(b)]);
        // The sibling and its record were there while it ran, and are gone.
        GCHECK([log containsString:@"SIBLING-THERE"] && [log containsString:@"RECORD-THERE"]);
        GCHECK(!Exists(Project(@"latex-a/.paper.minicode.tex")));
        GCHECK(!Exists([folderA stringByAppendingPathComponent:@"sibling.record"]));
        GCHECK(Exists([folderA stringByAppendingPathComponent:@".paper.minicode.pdf"]));
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
        GCHECK(Exists([MCLatexWorkFolder(b) stringByAppendingPathComponent:@".paper.minicode.pdf"]));
        GCHECK(!Exists(Project(@"latex-b/.paper.minicode.tex")));
    });

    MCGuiStep(@"closing the second preview", ^{
        GCHECK([MCGuiController() closeTab:tabB prompt:NO]);
        tabB = nil;
    });
    MCGuiWait(@"takes its output folder with it", 10, ^BOOL {
        return !Exists(MCLatexWorkFolder(b));
    });
    MCGuiStep(@"and leaves the first one's", ^{
        GCHECK(Exists(MCLatexWorkFolder(a)));
        GCHECK([MCGuiController() closeTab:tabA prompt:NO]);
        tabA = nil;
    });
    MCGuiWait(@"until it closes too", 10, ^BOOL { return !Exists(MCLatexWorkFolder(a)); });

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
