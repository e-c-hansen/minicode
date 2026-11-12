// LatexProblemsSuite.cpp — the LaTeX preview's problems and warnings
// (`./build/minicode-guitest latex_problems`, linux/PARITY.md item 4.4,
// HANDOFF.md item 51), against a stand-in tectonic (a shell script given
// canned output: the lines are tectonic 0.17.0's, from tests/latex/logs).
// A failed typeset lists its problems as rows that open their file at the
// line, marks the lines in the gutter until an edit, and a good one counts
// its warnings (tectonic's own and TeX's transcript) behind the bar's
// button, with the transcript, the record and the hidden copy gone after.
// The first typeset of a launch also removes what a crashed process left
// (LatexScratch), and only a copy still exactly the bytes its record names.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "Latex.h"
#include "LatexScratch.h"

#include <cairo.h>
#ifdef MINICODE_ENABLE_PDF
#include <cairo-pdf.h>
#endif

#include <glib/gstdio.h>

#include <string>

#ifdef MINICODE_ENABLE_PDF
namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }
std::string D() { return F("lp-stub"); }

LatexPreview* gPreview = nullptr;
std::string gOut, gStaleGone, gStaleKept, gStaleDirGone, gStaleDirKept;

int caretLine() {
    GtkTextIter it;
    GtkTextBuffer* b = ed()->buffer();
    gtk_text_buffer_get_iter_at_mark(b, &it, gtk_text_buffer_get_insert(b));
    return gtk_text_iter_get_line(&it) + 1;
}

bool lineHasTag(int line, const char* tag) {
    GtkTextBuffer* b = ed()->buffer();
    GtkTextTag* t = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(b), tag);
    if (!t) return false;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line(b, &it, line - 1);
    return gtk_text_iter_has_tag(&it, t);
}

std::string sha(const std::string& s) {
    char* h = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                          reinterpret_cast<const guchar*>(s.data()), s.size());
    std::string out = h;
    g_free(h);
    return out;
}

std::string doc(const char* marker) {   // 20 lines
    std::string t = std::string("\\documentclass{article}\n\\begin{document}\n") + marker + "\n";
    for (int i = 4; i < 19; ++i) t += "Line " + std::to_string(i) + " of the text.\n";
    return t + "\\end{document}\n";
}

}  // namespace

MC_GUI_SUITE(latex_problems) {
    MCGuiStep("a stand-in tectonic, a failing document, a good one and an \\input part", [] {
        ed()->closeAllTabs();
        MCGuiWriteFile(D() + "/x", "");
        cairo_surface_t* pdf = cairo_pdf_surface_create((D() + "/blank.pdf").c_str(), 300, 400);
        cairo_t* pc = cairo_create(pdf);
        cairo_show_page(pc);
        cairo_destroy(pc);
        cairo_surface_destroy(pdf);
        // A failed run: tectonic's summary, then XeTeX's transcript.
        MCGuiWriteFile(D() + "/fail.out",
            "error: @SIB@:3: Undefined control sequence\n"
            "error: something bad happened inside XeTeX; its output follows:\n\n"
            "===============================================================================\n"
            "(@SIB@\nLaTeX2e <2021-11-15> patch level 1\n"
            "! Undefined control sequence.\nl.3 \\foo\n        \n"
            "No pages of output.\n"
            "===============================================================================\n"
            "error: the XeTeX engine had an unrecoverable error\n"
            "caused by: halted on potentially-recoverable error as specified\n");
        // A good run: tectonic's own warnings (an underfull box is left out) ...
        MCGuiWriteFile(D() + "/ok.out",
            "warning: parts/chapter:3: Overfull \\hbox (86.74pt too wide) detected at line 3\n"
            "warning: @SIB@:11: Overfull \\hbox (156.34001pt too wide) detected at line 11\n"
            "warning: @SIB@:17: Overfull \\hbox (198.98502pt too wide) in paragraph at lines 13--17\n"
            "warning: @SIB@:18: Underfull \\hbox (badness 10000) detected at line 18\n"
            "warning: warnings were issued by the TeX engine; use --print and/or --keep-logs for details.\n");
        // ... and TeX's transcript, with a part open when its warning was written.
        MCGuiWriteFile(D() + "/ok.log",
            "**\n(@SIB@\nLaTeX2e <2021-11-15> patch level 1\n(article.cls\n"
            "Document Class: article 2021/10/04 v1.4n Standard LaTeX document class\n)\n"
            "LaTeX Warning: Reference `sec:missing' on page 1 undefined on input line 4.\n\n"
            "(parts/chapter.tex\nPackage mypkg Warning: Something odd here on input line 2.\n\n)\n"
            "LaTeX Warning: There were undefined references.\n\n)\n");
        MCGuiWriteFile(D() + "/tectonic",
            "#!/bin/sh\n"
            "args=\"$*\"\n"
            "while [ $# -gt 1 ]; do case \"$1\" in --outdir) out=$2; shift;; esac; shift; done\n"
            "file=$1; sib=$(basename \"$file\"); stem=${sib%.tex}\n"
            "d=$(dirname \"$0\")\n"
            "echo \"$args\" > \"$d/args.txt\"\n"
            "cp \"$file\" \"$d/sibling-seen.tex\"\n"
            "[ -f \"$out/sibling.record\" ] && echo yes > \"$d/record-seen.txt\"\n"
            "if grep -q FAILME \"$file\"; then\n"
            "  sed \"s/@SIB@/$sib/g\" \"$d/fail.out\"; exit 1\n"
            "fi\n"
            "cp \"$d/blank.pdf\" \"$out/$stem.pdf\"\n"
            "sed \"s/@SIB@/$sib/g\" \"$d/ok.log\" > \"$out/$stem.log\"\n"
            "sed \"s/@SIB@/$sib/g\" \"$d/ok.out\"\n");
        g_chmod((D() + "/tectonic").c_str(), 0755);
        g_setenv("MINICODE_TECTONIC", (D() + "/tectonic").c_str(), TRUE);
        MCGuiWriteFile(F("lp-bad.tex"), doc("\\foo FAILME"));
        MCGuiWriteFile(F("lp-good.tex"), doc("See \\ref{sec:missing}."));
        MCGuiWriteFile(F("parts/chapter.tex"), "one\ntwo\nthree\nfour\n");
        // What a crashed process left: a folder of a dead pid with its
        // record and sibling, one still the bytes recorded and one changed.
        const std::string base = F("lp-xdg");
        g_setenv("XDG_RUNTIME_DIR", base.c_str(), TRUE);
        g_mkdir_with_parents(base.c_str(), 0755);
        for (int k = 0; k < 2; ++k) {
            const std::string sib = F(k ? ".keep.tex.7.minicode.tex" : ".gone.tex.7.minicode.tex");
            const std::string dir = base + "/minicode-latex/0123456789abcdef-4194000-5-" +
                                    std::to_string(k + 1);
            MCGuiWriteFile(sib, "recorded bytes\n");
            LatexScratch::Record rec;
            rec.sibling = sib;
            rec.pid = 4194000;
            rec.start = 5;
            rec.size = 15;
            rec.sha256 = sha("recorded bytes\n");
            MCGuiWriteFile(dir + "/sibling.record", LatexScratch::format(rec));
            if (k) MCGuiWriteFile(sib, "the user's own edit\n");
            (k ? gStaleKept : gStaleGone) = sib;
            (k ? gStaleDirKept : gStaleDirGone) = dir;
        }
        MCGuiOpenFileKept(F("lp-bad.tex"));
    });
    MCGuiWait("the preview shows tectonic's failure", 20, [] {
        gPreview = ed()->latex();
        return gPreview && gPreview->showingLog() && !gPreview->busy() &&
               !gPreview->problems().empty();
    });
    MCGuiStep("one error, on line 3, in the document; tectonic ran as the Mac runs it", [] {
        const auto& p = gPreview->problems();
        GCHECK_EQ_STR(std::to_string(p.size()), "1");
        GCHECK(p[0].severity == LatexLog::Severity::Error);
        GCHECK_EQ_STR(std::to_string(p[0].line), "3");
        GCHECK_EQ_STR(p[0].message, "Undefined control sequence");
        const std::string args = MCGuiReadFile(D() + "/args.txt");
        GCHECK(args.find("--keep-logs") != std::string::npos);
        GCHECK(args.find("--untrusted") != std::string::npos);
        GCHECK(MCGuiReadFile(D() + "/sibling-seen.tex").find("FAILME") != std::string::npos);
        GCHECK(MCGuiFileExists(D() + "/record-seen.txt"));
        gOut = gPreview->outDirForTest();
        GCHECK(gOut.find("/minicode-latex/") != std::string::npos);
        GCHECK(gOut.find("-" + std::to_string(getpid()) + "-") != std::string::npos);
        // Afterwards: the copy and its record are gone.
        GCHECK(!MCGuiFileExists(gPreview->siblingPathForTest()));
        GCHECK(!MCGuiFileExists(gOut + "/sibling.record"));
        // The crashed process's folders are gone, and its copy with them
        // unless it had changed.
        GCHECK(!MCGuiFileExists(gStaleGone));
        GCHECK(!MCGuiFileExists(gStaleDirGone));
        GCHECK(MCGuiFileExists(gStaleKept));
        GCHECK(!MCGuiFileExists(gStaleDirKept));
    });
    MCGuiStep("the line is marked in the gutter's tags", [] {
        GCHECK(gPreview->marksShown());
        GCHECK(lineHasTag(3, "latex-error"));
        GCHECK(!lineHasTag(2, "latex-error"));
    });
    MCGuiStep("pressing the row opens the source at the line", [] {
        GCHECK(gPreview->openProblemRow(0));
    });
    MCGuiWait("the source, the caret on line 3", 10, [] {
        return !ed()->inPreview() && caretLine() == 3;
    });
    MCGuiStep("typing takes the marks away", [] {
        GCHECK(gPreview->marksShown());
        MCGuiType("x");
    });
    MCGuiWait("no marks", 10, [] { return !gPreview->marksShown() && !lineHasTag(3, "latex-error"); });

    MCGuiStep("the good document", [] { MCGuiOpenFileKept(F("lp-good.tex")); });
    MCGuiWait("it typesets with warnings", 20, [] {
        gPreview = ed()->latex();
        return gPreview && !gPreview->busy() && gPreview->warningsShown();
    });
    MCGuiStep("tectonic's boxes and the transcript's warnings, behind a button", [] {
        std::string seen;
        for (const auto& p : gPreview->problems())
            seen += std::to_string(p.severity == LatexLog::Severity::Error) + ":" + p.file + ":" +
                    std::to_string(p.line) + ":" + p.message + "\n";
        GCHECK_EQ_STR(std::to_string(gPreview->warningCount()), "5");
        GCHECK_EQ_STR(gPreview->warningsLabel(), "5 warnings");
        GCHECK(!gPreview->showingLog());
        GCHECK(seen.find("Something odd here") != std::string::npos);
        GCHECK(seen.find("Underfull") == std::string::npos);
        // The transcript is read and removed, with the record and the copy.
        const std::string out = gPreview->outDirForTest();
        bool anyLog = false;
        if (GDir* d = g_dir_open(out.c_str(), 0, nullptr)) {
            while (const char* n = g_dir_read_name(d))
                anyLog = anyLog || g_str_has_suffix(n, ".log");
            g_dir_close(d);
        }
        GCHECK(!anyLog);
        GCHECK(!MCGuiFileExists(gPreview->siblingPathForTest()));
        GCHECK(!MCGuiFileExists(out + "/sibling.record"));
        gOut = out;
        gPreview->showWarnings();
    });
    MCGuiStep("the warnings are marked on the source lines", [] {
        GCHECK(gPreview->marksShown());
        GCHECK(lineHasTag(11, "latex-warning"));
        GCHECK(lineHasTag(13, "latex-warning"));
    });
    MCGuiStep("a warning in the \\input part opens that file at its line", [] {
        int row = -1, n = 0;
        for (const auto& p : gPreview->problems()) {
            if (p.severity != LatexLog::Severity::Warning) continue;
            if (p.file == "parts/chapter" && p.line == 3) row = n;
            ++n;
        }
        GCHECK(row >= 0);
        GCHECK(gPreview->openWarningRow(row));
    });
    MCGuiWait("parts/chapter.tex on show at line 3", 10, [] {
        return ed()->currentPath() == F("parts/chapter.tex") && caretLine() == 3;
    });
    MCGuiStep("closing the tabs", [] { ed()->closeAllTabs(); });
}
#endif
