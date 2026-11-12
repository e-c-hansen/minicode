// MarkdownSideSuite.cpp — the Markdown page right of its source, rendered in
// pieces (`./build/minicode-guitest markdown_side`, linux/PARITY.md item 4.2,
// first part): the key opens and closes the pane, the page matches the
// source, an edit builds only its own piece and leaves every table widget
// where it was, the full preview hides the pane and brings it back, the
// state is per tab, a broken link-free context change builds whole, and the
// main thread's CPU for a one-letter re-render at 1,200 and 20,000 lines.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "MarkdownSide.h"

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }
MarkdownSide* side() { return ed()->sidePane(); }

void key(const char* chord) {
    if (ed()->textView()) gtk_widget_grab_focus(ed()->textView());
    MCGuiKey(chord);
}

bool settled() {
    return ed()->showsSide() && side() && side()->idle() && side()->stats().renders > 0;
}

std::string bufferText() {
    GtkTextBuffer* b = ed()->active()->buffer();
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* raw = gtk_text_buffer_get_text(b, &s, &e, TRUE);
    std::string t = raw;
    g_free(raw);
    return t;
}

void insertBefore(const std::string& needle, const std::string& s) {
    const std::string t = bufferText();
    const size_t at = t.find(needle);
    GtkTextBuffer* b = ed()->active()->buffer();
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(b, &it, (int)g_utf8_pointer_to_offset(t.c_str(), t.c_str() + at));
    gtk_text_buffer_insert(b, &it, s.c_str(), -1);
}

// Paragraph k of n, a table after every 100th.
std::string bigDoc(int paragraphs) {
    std::string s = "# Big page\n\n";
    for (int k = 0; k < paragraphs; ++k) {
        s += "Paragraph " + std::to_string(k) + " has some words in it, and *emphasis* too.\n\n";
        if (k % 100 == 99)
            s += "| name | value |\n|------|-------|\n| t" + std::to_string(k) + " | 1 |\n\n";
    }
    return s;
}

int g_rendersBefore = 0;
long g_builtBefore = 0;
std::vector<GtkWidget*> g_embeds;

void measure(const char* file, int paragraphs, double boundMs) {
    MCGuiStep(std::string("open ") + file, [=] {
        ed()->closeAllTabs();
        MCGuiWriteFile(F(file), bigDoc(paragraphs));
        MCGuiOpenFileKept(F(file));
    });
    MCGuiWait("the page, which Markdown opens as", 20, [] { return ed()->active() && ed()->inPreview(); });
    MCGuiStep("its source", [] { key("ctrl+shift+v"); });
    MCGuiWait("source on show", 20, [] { return ed()->active() && ed()->isSource(); });
    MCGuiStep("open the pane", [] { key("ctrl+alt+v"); });
    MCGuiWait("the whole page", 60, [] { return settled(); });
    MCGuiStep("one letter in the middle", [=] {
        GCHECK(side()->stats().pieces > 10);
        g_rendersBefore = side()->stats().renders;
        g_embeds = side()->embedWidgets();
        insertBefore("Paragraph " + std::to_string(paragraphs / 2) + " has", "x");
    });
    MCGuiWait("the re-render", 60, [] {
        return side()->idle() && side()->stats().renders > g_rendersBefore;
    });
    MCGuiStep(std::string("only its pieces built, at ") + file, [=] {
        const MarkdownSide::Stats& st = side()->stats();
        MCGuiNote("%s: %d pieces, built %d, kept %d, main %.2f ms, worker %.2f ms", file,
                  st.pieces, st.built, st.kept, st.mainCpuMs, st.workerCpuMs);
        GCHECK(st.built >= 1 && st.built <= 2);
        GCHECK_EQ_STR(std::to_string(st.built + st.kept), std::to_string(st.pieces));
        GCHECK(side()->embedWidgets() == g_embeds);
        GCHECK_TIME(st.mainCpuMs, boundMs, std::string("a one-letter re-render at ") + file);
        GCHECK(side()->pageText().find("xParagraph") != std::string::npos);
    });
}

}  // namespace

MC_GUI_SUITE(markdown_side) {
    MCGuiStep("two Markdown files, one with a table", [] {
        ed()->closeAllTabs();
        MCGuiWriteFile(F("a.md"),
                       "# Title\n\nFirst paragraph.\n\n| h1 | h2 |\n|----|----|\n| c1 | c2 |\n\n"
                       "Middle paragraph.\n\n> a quote\n\nLast paragraph.\n");
        MCGuiWriteFile(F("b.md"), "# Other\n\nOnly this.\n");
        MCGuiWriteFile(F("c.txt"), "plain\n");
        MCGuiOpenFileKept(F("a.md"));
    });
    MCGuiWait("the page, which Markdown opens as", 10, [] { return ed()->active() && ed()->inPreview(); });
    MCGuiStep("its source", [] { key("ctrl+shift+v"); });
    MCGuiWait("the source on show", 10, [] { return ed()->active() && ed()->isSource(); });
    MCGuiStep("no pane yet", [] { GCHECK(!ed()->showsSide()); });
    MCGuiStep("the key opens it", [] { key("ctrl+alt+v"); });
    MCGuiWait("the page", 20, [] { return settled(); });
    MCGuiStep("which shows the page beside the source, in pieces", [] {
        GCHECK(side() != nullptr);
        if (!side()) return;
        GCHECK(gtk_widget_get_mapped(side()->widget()));
        GCHECK(gtk_widget_get_mapped(ed()->textView()));
        GCHECK(ed()->isSource());
        const std::string t = side()->pageText();
        GCHECK(t.find("Title") != std::string::npos);
        GCHECK(t.find("Last paragraph.") != std::string::npos);
        GCHECK(t.find("# Title") == std::string::npos);   // rendered, not source
        GCHECK(side()->stats().pieces >= 4);
        GCHECK_EQ_STR(std::to_string(side()->embedCount()), "1");   // the table
        GCHECK(!gtk_text_view_get_editable(GTK_TEXT_VIEW(side()->textView())));
    });
    MCGuiStep("an edit in the middle paragraph", [] {
        g_rendersBefore = side()->stats().renders;
        g_builtBefore = side()->stats().builtTotal;
        g_embeds = side()->embedWidgets();
        insertBefore("Middle", "Very ");
    });
    MCGuiWait("the re-render", 10, [] {
        return side()->idle() && side()->stats().renders > g_rendersBefore;
    });
    MCGuiStep("builds only that piece and keeps the table's widget", [] {
        const MarkdownSide::Stats& st = side()->stats();
        GCHECK_EQ_STR(std::to_string(st.built), "1");
        GCHECK_EQ_STR(std::to_string(st.builtTotal - g_builtBefore), "1");
        GCHECK_EQ_STR(std::to_string(st.kept), std::to_string(st.pieces - 1));
        GCHECK(side()->embedWidgets() == g_embeds);
        GCHECK(side()->pageText().find("Very Middle paragraph.") != std::string::npos);
    });
    MCGuiStep("a line added above moves nothing that was built", [] {
        g_rendersBefore = side()->stats().renders;
        insertBefore("First", "Zero paragraph.\n\n");
    });
    MCGuiWait("the second re-render", 10, [] {
        return side()->idle() && side()->stats().renders > g_rendersBefore;
    });
    MCGuiStep("only the new paragraph is built", [] {
        GCHECK_EQ_STR(std::to_string(side()->stats().built), "1");
        GCHECK(side()->embedWidgets() == g_embeds);
        const std::string t = side()->pageText();
        GCHECK(t.find("Zero paragraph.") < t.find("First paragraph."));
    });
    MCGuiStep("an edit inside the table builds the table again", [] {
        g_rendersBefore = side()->stats().renders;
        insertBefore("c1", "new ");
    });
    MCGuiWait("the third re-render", 10, [] {
        return side()->idle() && side()->stats().renders > g_rendersBefore;
    });
    MCGuiStep("with a new widget", [] {
        GCHECK_EQ_STR(std::to_string(side()->stats().built), "1");
        GCHECK_EQ_STR(std::to_string(side()->embedCount()), "1");
        GCHECK(side()->embedWidgets() != g_embeds);
    });

    MCGuiStep("the full preview hides the pane", [] { key("ctrl+shift+v"); });
    MCGuiWait("the full page", 10, [] { return ed()->inPreview(); });
    MCGuiStep("which is alone", [] {
        GCHECK(!ed()->showsSide());
        GCHECK(!gtk_widget_get_visible(side()->widget()));
        GCHECK_EQ_STR(std::to_string(side()->embedCount()), "0");   // nothing kept behind it
        key("ctrl+shift+v");
    });
    MCGuiWait("the source and the pane again", 20, [] { return ed()->isSource() && settled(); });
    MCGuiStep("with the page of the source as it is", [] {
        GCHECK(gtk_widget_get_visible(side()->widget()));
        GCHECK(side()->pageText().find("Very Middle paragraph.") != std::string::npos);
        GCHECK_EQ_STR(std::to_string(side()->embedCount()), "1");   // the table, with its edit
    });

    MCGuiStep("another tab has its own state", [] {
        MCGuiOpenFileKept(F("b.md"));
    });
    MCGuiWait("b on show", 10, [] { return ed()->active() && ed()->active()->path() == F("b.md"); });
    MCGuiStep("with no pane", [] { GCHECK(!ed()->showsSide()); key("alt+1"); });
    MCGuiWait("a again, with its pane", 20, [] {
        return ed()->active() && ed()->active()->path() == F("a.md") && settled();
    });
    MCGuiStep("rendered anew from its own text", [] {
        GCHECK(side()->pageText().find("Very Middle paragraph.") != std::string::npos);
        MCGuiOpenFileKept(F("c.txt"));
    });
    MCGuiWait("the text file", 10, [] { return ed()->active() && ed()->active()->path() == F("c.txt"); });
    MCGuiStep("a file that is not Markdown has none, and the key does nothing", [] {
        GCHECK(!ed()->showsSide());
        key("ctrl+alt+v");
        GCHECK(!ed()->showsSide());
        key("alt+1");
    });
    MCGuiWait("a on show", 10, [] { return ed()->active() && ed()->active()->path() == F("a.md") && settled(); });
    MCGuiStep("the key closes the pane", [] { key("ctrl+alt+v"); });
    MCGuiWait("it is gone", 10, [] { return !ed()->showsSide(); });
    MCGuiStep("and hides", [] { GCHECK(!gtk_widget_get_visible(side()->widget())); });

    measure("m1200.md", 600, 15);
    measure("m20000.md", 10000, 40);
    MCGuiStep("close", [] { ed()->closeAllTabs(); });
}
