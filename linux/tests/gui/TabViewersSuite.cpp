// TabViewersSuite.cpp — what takes the text view's place, in tabs
// (`./build/minicode-guitest tab_viewers`, linux/PARITY.md item 1.1): a
// picture and a PDF each in a tab of its own, shown again when their tab
// is, with nothing to save; a LaTeX file's preview its tab's own, kept while
// another tab shows and taken off the slot with its tab; and diffs and
// commits sharing one tab, as on the Mac.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "GitPanel.h"
#include "Latex.h"
#include "MediaView.h"
#include "TabStrip.h"

#include <cairo.h>
#ifdef MINICODE_ENABLE_PDF
#include <cairo-pdf.h>
#endif

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

// The stack the text view and its stand-ins share.
GtkWidget* slot() { return gtk_widget_get_parent(gtk_widget_get_parent(ed()->textView())); }
GtkWidget* onShow() { return gtk_stack_get_visible_child(GTK_STACK(slot())); }
int slotChildren() {
    int n = 0;
    for (GtkWidget* c = gtk_widget_get_first_child(slot()); c; c = gtk_widget_get_next_sibling(c)) n++;
    return n;
}

std::string titles() {
    std::string out;
    TabStrip* s = ed()->strip();
    for (int i = 0; i < s->count(); i++) out += (out.empty() ? "" : " ") + s->item(i).title;
    return out;
}

std::string title() { return gtk_window_get_title(MCGuiApp().window); }

#ifdef MINICODE_ENABLE_PDF
LatexPreview* gLatex = nullptr;
#endif
int gSlotChildren = 0;
size_t gClosed = 0;

}  // namespace

MC_GUI_SUITE(tab_viewers) {
    MCGuiStep("a text file, a picture, a PDF and a LaTeX file, and no tab open", [] {
        ed()->closeAllTabs();
        MCGuiWriteFile(F("v-text.txt"), "plain text\n");
        cairo_surface_t* png = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 40, 30);
        cairo_t* cr = cairo_create(png);
        cairo_set_source_rgb(cr, 0.2, 0.4, 0.8);
        cairo_paint(cr);
        cairo_destroy(cr);
        cairo_surface_write_to_png(png, F("v-pic.png").c_str());
        cairo_surface_destroy(png);
#ifdef MINICODE_ENABLE_PDF
        cairo_surface_t* pdf = cairo_pdf_surface_create(F("v-doc.pdf").c_str(), 300, 400);
        cairo_t* pc = cairo_create(pdf);
        cairo_show_page(pc);
        cairo_show_page(pc);
        cairo_destroy(pc);
        cairo_surface_destroy(pdf);
        MCGuiWriteFile(F("v-paper.tex"),
                       "\\documentclass{article}\n\\begin{document}\nHello.\n\\end{document}\n");
#endif
    });
    MCGuiStep("the text file and the picture, each in a tab of its own", [] {
        MCGuiOpenFileKept(F("v-text.txt"));
        MCGuiOpenFileKept(F("v-pic.png"));
    });
    MCGuiWait("the picture on show in the slot", 10, [] {
        return ed()->tabCount() == 2 && ed()->isMedia() && onShow() == ed()->media()->widget();
    });
    MCGuiStep("with its size in the title, and nothing to save", [] {
        GCHECK_EQ_STR(titles(), "v-text.txt v-pic.png");
        GCHECK(ed()->media()->kind() == MediaView::Kind::Image);
        GCHECK(title().find("v-pic.png  40 × 30") != std::string::npos);
        GCHECK(!ed()->canSave() && !ed()->dirty() && !ed()->isSource());
        GCHECK(ed()->active()->kind() == Document::Kind::Media);
    });
#ifdef MINICODE_ENABLE_PDF
    MCGuiStep("the PDF", [] { MCGuiOpenFileKept(F("v-doc.pdf")); });
    MCGuiWait("its pages on show", 10, [] {
        return ed()->media()->kind() == MediaView::Kind::Pdf && ed()->showingPdf();
    });
    MCGuiStep("two pages, as the title says", [] {
        GCHECK(title().find("v-doc.pdf  2 pages") != std::string::npos);
    });
#endif
    MCGuiStep("tab 1, the text", [] { MCGuiKey("alt+1"); });
    MCGuiWait("the text view back in the slot, with its file", 10, [] {
        return !ed()->isMedia() && onShow() == gtk_widget_get_parent(ed()->textView()) &&
               MCGuiEditorText() == "plain text\n" && ed()->isSource();
    });
    MCGuiStep("the picture let go", [] {
        GCHECK(ed()->media()->kind() == MediaView::Kind::None);
        GCHECK(title().find("40 × 30") == std::string::npos);
    });
    MCGuiStep("tab 2, the picture again", [] { MCGuiKey("alt+2"); });
    MCGuiWait("shown again", 10, [] {
        return ed()->isMedia() && ed()->media()->kind() == MediaView::Kind::Image &&
               title().find("v-pic.png  40 × 30") != std::string::npos;
    });

#ifdef MINICODE_ENABLE_PDF
    // ---- the LaTeX preview is its tab's own
    MCGuiStep("the LaTeX file", [] {
        gSlotChildren = slotChildren();
        MCGuiOpenFileKept(F("v-paper.tex"));
    });
    MCGuiWait("its preview in the slot, the source in its buffer", 10, [] {
        return ed()->isLatex() && ed()->inPreview() && ed()->latex() &&
               onShow() == ed()->latex()->widget() &&
               ed()->text().find("Hello.") != std::string::npos;
    });
    MCGuiStep("one more child of the slot: this tab's preview", [] {
        gLatex = ed()->latex();
        GCHECK(slotChildren() == gSlotChildren + 1);
        // Right of the tab on show, the picture's.
        GCHECK_EQ_STR(titles(), "v-text.txt v-pic.png v-paper.tex v-doc.pdf");
        MCGuiKey("alt+1");
    });
    MCGuiWait("the text again, no preview on show", 10, [] {
        return !ed()->isLatex() && ed()->latex() == nullptr &&
               onShow() == gtk_widget_get_parent(ed()->textView());
    });
    MCGuiStep("back to the LaTeX tab", [] { MCGuiKey("alt+3"); });
    MCGuiWait("the same preview shows again", 10, [] {
        return ed()->latex() == gLatex && onShow() == gLatex->widget();
    });
    MCGuiStep("Ctrl+Shift+V: the source, in its own buffer", [] {
        MCGuiClick(GTK_WIDGET(gLatex->widget()));
        MCGuiKey("ctrl+shift+v");
    });
    MCGuiWait("the source", 10, [] {
        return !ed()->inPreview() && onShow() == gtk_widget_get_parent(ed()->textView()) &&
               MCGuiEditorText().find("Hello.") != std::string::npos && ed()->isSource();
    });
    MCGuiStep("and the preview again", [] { MCGuiKey("ctrl+shift+v"); });
    MCGuiWait("the preview", 10, [] { return ed()->inPreview() && onShow() == gLatex->widget(); });
    MCGuiStep("Ctrl+W closes the LaTeX tab", [] {
        GCHECK(!ed()->dirty());
        MCGuiKey("ctrl+w");
    });
    MCGuiWait("its preview goes from the slot with it", 10, [] {
        return ed()->documentFor(F("v-paper.tex")) == nullptr && slotChildren() == gSlotChildren;
    });
#endif

    // ---- diffs and commits share one tab
    MCGuiStep("a diff from Source Control", [] {
        gClosed = ed()->closedTabCount();
        ed()->showDiff("v-text.txt (diff)",
                       "diff --git a/v-text.txt b/v-text.txt\n--- a/v-text.txt\n+++ b/v-text.txt\n"
                       "@@ -1 +1 @@\n-old\n+plain text\n",
                       false);
    });
    MCGuiWait("in a tab of its own, read-only in the slot", 10, [] {
        return ed()->isDiff() && onShow() == gtk_widget_get_parent(ed()->diffView()->textView());
    });
    MCGuiStep("titled as the Mac titles it", [] {
        GCHECK(titles().find("v-text.txt (diff)") != std::string::npos);
        GCHECK(title().find("v-text.txt (diff)") != std::string::npos);
        GCHECK(!ed()->canSave());
    });
    MCGuiStep("a commit takes the same tab", [] {
        const int before = ed()->tabCount();
        ed()->showDiff("abc1234 A commit", "commit abc1234\nAuthor: x\n\n    A commit\n", true);
        GCHECK(ed()->tabCount() == before);
        GCHECK(titles().find("abc1234 A commit") != std::string::npos);
        GCHECK(titles().find("(diff)") == std::string::npos);
        GCHECK(ed()->isDiff());
    });
    MCGuiStep("closed, it is not a tab to reopen", [] {
        ed()->closeTab(ed()->active());
        GCHECK(ed()->closedTabCount() == gClosed);
        GCHECK(titles().find("abc1234") == std::string::npos);
    });
}

