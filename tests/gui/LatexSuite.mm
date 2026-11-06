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
void WriteTextPDF(NSString *path, NSString *word) {
    NSTextView *tv = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 300, 80)];
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
