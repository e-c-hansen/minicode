// LatexForwardSuite.cpp — LaTeX forward search (`./build/minicode-guitest
// latex_forward`, linux/PARITY.md item 4.4, HANDOFF.md item 55): Ctrl+Alt+
// Shift+J lights the lines of text the caret's line set on the PDF, scrolls
// the page there, and clears the light after 1.5 s. The stand-in tectonic
// copies the committed typeset of tests/latex/fixtures/main.tex (its PDF and
// SyncTeX, the Input paths pointed at this run's files) and counts its runs.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "Latex.h"
#include "PdfView.h"

#include <glib/gstdio.h>

#include <string>

#ifdef MINICODE_ENABLE_PDF
namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& name) { return MCGuiProjectFile("fw/" + name); }
std::string D() { return MCGuiProjectFile("fw-stub"); }
std::string fixture(const std::string& name) {
    return MCGuiBuildDir() + "/../../tests/latex/fixtures/" + name;
}
std::string slurp(const std::string& path) {
    gchar* data = nullptr;
    gsize n = 0;
    std::string out;
    if (g_file_get_contents(path.c_str(), &data, &n, nullptr)) out.assign(data, n);
    g_free(data);
    return out;
}
int runs() { return (int)MCGuiReadFile(D() + "/runs.txt").size(); }   // one byte a run

const std::vector<PdfView::Highlight>& lit() { return ed()->latex()->pdfView()->highlights(); }
bool isLit() { return ed()->latex() && !lit().empty(); }

void say(const char* what) {
    std::string s = what;
    if (ed()->latex() && !lit().empty())
        s += " page=" + std::to_string(lit()[0].page) + " y=" + std::to_string(lit()[0].y) +
             " found=" + std::to_string(ed()->latex()->forwardFoundLine()) +
             " boxes=" + std::to_string(lit().size());
    g_print("%s\n", s.c_str());
}

}  // namespace

MC_GUI_SUITE(latex_forward) {
    MCGuiStep("a stand-in tectonic that copies the committed typeset of main.tex", [] {
        ed()->closeAllTabs();
        for (const char* n : {"main.tex", "chapter.tex", "appendix.tex"})
            MCGuiWriteFile(F(n), slurp(fixture(n)));
        MCGuiWriteFile(D() + "/main.pdf", slurp(fixture("main.pdf")));
        MCGuiWriteFile(D() + "/main.synctex", slurp(fixture("main.synctex")));
        MCGuiWriteFile(D() + "/tectonic",
            "#!/bin/sh\n"
            "while [ $# -gt 1 ]; do case \"$1\" in --outdir) out=$2; shift;; esac; shift; done\n"
            "file=$1; case $file in /*) ;; *) file=$PWD/$file;; esac\n"
            "sib=$(basename \"$file\"); stem=${sib%.tex}\n"
            "d=$(dirname \"$0\")\n"
            "printf x >> \"$d/runs.txt\"\n"
            "cp \"$d/main.pdf\" \"$out/$stem.pdf\"\n"
            "sed -e \"s|@DIR@/.main.minicode.tex|$file|\" -e \"s|@DIR@|$(dirname \"$file\")|\" "
            "\"$d/main.synctex\" | gzip -c > \"$out/$stem.synctex.gz\"\n");
        g_chmod((D() + "/tectonic").c_str(), 0755);
        g_setenv("MINICODE_TECTONIC", (D() + "/tectonic").c_str(), TRUE);
        MCGuiOpenFileKept(F("main.tex"));
    });
    MCGuiWait("main.tex shows", 10, [] { return ed()->currentPath() == F("main.tex"); });
    MCGuiStep("the caret on line 3, the key", [] {
        GCHECK(ed()->revealLine(3, 0, 0));
        MCGuiKey("ctrl+alt+shift+j");
    });
    MCGuiWait("it typesets and lights the line", 30, [] { return isLit(); });
    MCGuiStep("line 3 is on page 1, near the top, after one typeset", [] {
        say("line 3:");
        GCHECK_EQ_STR(std::to_string(runs()), "1");
        GCHECK_EQ_STR(std::to_string(ed()->latex()->forwardFoundLine()), "3");
        GCHECK_EQ_STR(std::to_string(lit()[0].page), "0");
        GCHECK(lit()[0].y > 100 && lit()[0].y < 160);
        GCHECK(lit()[0].w > 100 && lit()[0].h > 3);
    });
    MCGuiWait("the light goes after about 1.5 s", 10, [] { return !isLit(); });
    MCGuiStep("a line that put nothing (2) finds the nearest below", [] {
        GCHECK(ed()->revealLine(2, 0, 0));
        MCGuiKey("ctrl+alt+shift+j");
    });
    MCGuiWait("lit", 10, [] { return isLit(); });
    MCGuiStep("the PDF on screen is used, not typeset again", [] {
        say("line 2:");
        GCHECK(ed()->latex()->forwardFoundLine() > 2);
        GCHECK_EQ_STR(std::to_string(lit()[0].page), "0");
        GCHECK_EQ_STR(std::to_string(runs()), "1");
    });
    MCGuiWait("cleared", 10, [] { return !isLit(); });
    MCGuiStep("an edit makes the PDF stale; the key typesets first", [] {
        GCHECK(ed()->revealLine(5, 0, 0));
        gtk_text_buffer_insert_at_cursor(ed()->buffer(), "x", 1);
        MCGuiKey("ctrl+alt+shift+j");
    });
    MCGuiWait("lit again after a second typeset", 30, [] { return isLit() && runs() >= 2; });
    MCGuiStep("it lit a line of the second paragraph", [] {
        say("after edit:");
        GCHECK_EQ_STR(std::to_string(lit()[0].page), "0");
        GCHECK(lit()[0].y > 140);
    });
    MCGuiWait("cleared", 10, [] { return !isLit(); });

    MCGuiStep("chapter.tex has a root comment: its line 5 shows main.tex's page", [] {
        MCGuiOpenFileKept(F("chapter.tex"));
    });
    MCGuiWait("chapter.tex shows", 10, [] { return ed()->currentPath() == F("chapter.tex"); });
    MCGuiStep("the key from it", [] {
        GCHECK(ed()->revealLine(5, 0, 0));
        MCGuiKey("ctrl+alt+shift+j");
    });
    MCGuiWait("main.tex's tab shows with the line lit", 20, [] {
        return ed()->currentPath() == F("main.tex") && isLit();
    });
    MCGuiStep("chapter line 5, page 1", [] {
        say("chapter 5:");
        GCHECK_EQ_STR(std::to_string(ed()->latex()->forwardFoundLine()), "5");
        GCHECK_EQ_STR(std::to_string(lit()[0].page), "0");
    });
    MCGuiWait("cleared", 10, [] { return !isLit(); });
    MCGuiStep("appendix.tex has no root comment and no \\documentclass: the tab that read it", [] {
        MCGuiOpenFileKept(F("appendix.tex"));
    });
    MCGuiWait("appendix.tex shows", 10, [] { return ed()->currentPath() == F("appendix.tex"); });
    MCGuiStep("line 1 is on page 2", [] {
        GCHECK(ed()->revealLine(1, 0, 0));
        MCGuiKey("ctrl+alt+shift+j");
    });
    MCGuiWait("main.tex shows, lit", 20, [] {
        return ed()->currentPath() == F("main.tex") && isLit();
    });
    MCGuiStep("page 2, and scrolled there", [] {
        say("appendix 1:");
        GCHECK_EQ_STR(std::to_string(lit()[0].page), "1");
        GCHECK_EQ_STR(std::to_string(ed()->latex()->pdfView()->anchor().page), "1");
    });
    MCGuiStep("closing the tabs", [] { ed()->closeAllTabs(); });
}
#endif
