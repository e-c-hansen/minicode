// LatexInputsSuite.cpp — `./build/minicode-guitest latex_inputs`: a
// double-click in the LaTeX preview on text that an \input file put on the
// page edits that file (HANDOFF.md item 80), the counterpart of the Mac's
// latex suite step of the same name. The stand-in tectonic copies the
// committed typeset of tests/latex/fixtures/main.tex (its PDF and SyncTeX,
// whose boxes name chapter.tex and appendix.tex), as latex_forward's does:
//   - the click on the chapter's "lemur" opens a box on the chapter's own
//     source, titled "Editing text in chapter.tex", with no tab for the
//     chapter open;
//   - saving the box puts the edit in a new tab of chapter.tex beside the
//     document's, not shown, unsaved, one "Edit in Preview" step, and
//     nothing is written to disk;
//   - the bar says "chapter.tex has unsaved changes: save to see them in
//     the PDF";
//   - Ctrl+S with the preview on show saves the chapter too, and the
//     document is typeset again;
//   - a click on the document's own text still edits the document, in a box
//     with no file named;
//   - with the chapter open in a tab and changed there, the box opens on the
//     tab's text, and the edit goes into that tab.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "Latex.h"
#include "LatexClick.h"
#include "PdfView.h"

#include <glib/gstdio.h>

#include <string>

#ifdef MINICODE_ENABLE_PDF
namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& name) { return MCGuiProjectFile("li/" + name); }
std::string D() { return MCGuiProjectFile("li-stub"); }
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

LatexPreview* latex() { return ed()->latex(); }
bool typeset(int pages) {
    return latex() && !latex()->busy() && latex()->pdfView()->pageCount() == pages &&
           latex()->status().find("pages") != std::string::npos;
}

// The point (page 0, points from the top-left) in the middle of `word` on
// the page as poppler lays it out, found in the page's own text.
bool pointOf(const std::string& word, double* x, double* y) {
    PopplerPage* pp = poppler_document_get_page(latex()->pdfView()->document(), 0);
    if (!pp) return false;
    const PageText text = pageTextOf(pp);
    g_object_unref(pp);
    for (const auto& w : text.words()) {
        if (text.slice(w.first, w.second) != word) continue;
        const PageBox& b = text.box(w.first + (w.second - w.first) / 2);
        *x = (b.x1 + b.x2) / 2;
        *y = (b.y1 + b.y2) / 2;
        return true;
    }
    return false;
}

std::string gChapter;
Document* chapterTab() { return ed()->documentFor(F("chapter.tex")); }

std::string replaced(std::string s, const std::string& a, const std::string& b) {
    const size_t at = s.find(a);
    if (at != std::string::npos) s.replace(at, a.size(), b);
    return s;
}

}  // namespace

MC_GUI_SUITE(latex_inputs) {
    MCGuiStep("a stand-in tectonic that copies the committed typeset of main.tex", [] {
        ed()->closeAllTabs();
        for (const char* n : {"main.tex", "chapter.tex", "appendix.tex"})
            MCGuiWriteFile(F(n), slurp(fixture(n)));
        gChapter = slurp(fixture("chapter.tex"));
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
    MCGuiStep("no tab for the chapter; the preview", [] {
        GCHECK(chapterTab() == nullptr);
        if (!ed()->active()->showsPreview()) ed()->togglePreview();   // a .tex opens on its page
        GCHECK(ed()->active()->showsPreview());
    });
    MCGuiWait("it typesets, two pages", 30, [] { return typeset(2); });

    // ---- the chapter's text, with no tab for it
    MCGuiStep("a double-click on the chapter's lemur opens the chapter's source", [] {
        double x = 0, y = 0;
        GCHECK(pointOf("lemur", &x, &y));
        latex()->doubleClick(0, x, y);
        GCHECK(latex()->editing());
        const std::string span = latex()->editText();
        g_print("the span: \"%s\"\nthe box: %s\n", span.c_str(), latex()->editTitle().c_str());
        GCHECK(span.find("lemur in the snow") != std::string::npos);
        GCHECK(gChapter.find(span) != std::string::npos);
        GCHECK_EQ_STR(latex()->editTitle(), "Editing text in chapter.tex");
        latex()->setEditText(replaced(span, "lemur", "ocelot"));
        latex()->commitEdit();
    });
    MCGuiStep("the edit is in the chapter's tab, opened behind the preview, unsaved", [] {
        Document* t = chapterTab();
        GCHECK(t != nullptr);
        GCHECK(ed()->active() && ed()->currentPath() == F("main.tex"));
        GCHECK(t && t != ed()->active() && t->dirty() && !ed()->active()->dirty());
        GCHECK_EQ_STR(t ? t->text() : "", replaced(gChapter, "lemur", "ocelot"));
        GCHECK_EQ_STR(slurp(F("chapter.tex")), gChapter);   // nothing saved
        GCHECK_EQ_STR(t ? t->undoName() : "", "Edit in Preview");
        const std::string status = latex()->status();
        g_print("status: %s\n", status.c_str());
        GCHECK(status.find("chapter.tex has unsaved changes: save to see them in the PDF") !=
               std::string::npos);
        bool reads = false;
        for (const std::string& f : latex()->inputFiles()) reads |= f == F("chapter.tex");
        GCHECK(reads);
        GCHECK_EQ_STR(std::to_string(runs()), "1");
    });

    // ---- the document's own text is still the document's
    MCGuiStep("a click on the document's own walrus edits main.tex, no file named", [] {
        double x = 0, y = 0;
        GCHECK(pointOf("walrus", &x, &y));
        latex()->doubleClick(0, x, y);
        GCHECK(latex()->editing());
        GCHECK(latex()->editText().find("walrus") != std::string::npos);
        GCHECK_EQ_STR(latex()->editTitle(), "Editing text");
        latex()->cancelEdit();
    });

    // ---- the box opens on the tab's own text once the chapter is changed there
    MCGuiStep("a second click on the chapter reads the tab's text and edits that tab", [] {
        double x = 0, y = 0;
        GCHECK(pointOf("marmot", &x, &y));
        latex()->doubleClick(0, x, y);
        GCHECK(latex()->editing());
        const std::string span = latex()->editText();
        GCHECK(span.find("marmot") != std::string::npos);
        // The page still says lemur, and the tab's text has ocelot: the
        // second paragraph's box would show the tab's.
        GCHECK_EQ_STR(latex()->editTitle(), "Editing text in chapter.tex");
        latex()->setEditText(replaced(span, "marmot", "badger"));
        latex()->commitEdit();
    });
    MCGuiStep("one tab, both edits in it, two steps to take back", [] {
        Document* t = chapterTab();
        GCHECK(t != nullptr);
        GCHECK_EQ_STR(t ? t->text() : "",
                      replaced(replaced(gChapter, "lemur", "ocelot"), "marmot", "badger"));
        GCHECK(latex()->status().find("chapter.tex has unsaved changes") != std::string::npos);
    });

    // ---- Ctrl+S on the preview saves the chapter too, and the document is typeset again
    MCGuiStep("Ctrl+S with the preview on show, the keyboard in it", [] {
        gtk_widget_grab_focus(latex()->widget());
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("the chapter is written and the document typeset again", 30, [] {
        Document* t = chapterTab();
        return t && !t->dirty() && runs() >= 2 && typeset(2) &&
               latex()->status().find("unsaved") == std::string::npos;
    });
    MCGuiStep("the chapter on disk has both edits; main.tex as it was", [] {
        GCHECK_EQ_STR(slurp(F("chapter.tex")),
                      replaced(replaced(gChapter, "lemur", "ocelot"), "marmot", "badger"));
        GCHECK_EQ_STR(slurp(F("main.tex")), slurp(fixture("main.tex")));
    });
    MCGuiStep("closing the tabs, the chapter as it was", [] {
        if (Document* t = chapterTab()) ed()->closeTab(t);
        ed()->closeAllTabs();
        MCGuiWriteFile(F("chapter.tex"), gChapter);
    });
}
#endif
