// MarkdownSideSyncSuite.cpp — scroll sync between a Markdown source and its
// page to the side (`./build/minicode-guitest markdown_side_sync`,
// linux/PARITY.md item 4.2, second part): scrolling the source scrolls the
// page to the same place, scrolling the page scrolls the source, a caret
// moved far down brings its block into view, the pane's own re-render does
// not move the source, and the main thread's CPU for one sync at 20,000
// lines.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "MarkdownSide.h"

#include <cmath>
#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }
MarkdownSide* side() { return ed()->sidePane(); }
GtkTextView* src() { return GTK_TEXT_VIEW(ed()->textView()); }
GtkAdjustment* srcAdj() { return gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(src())); }
GtkAdjustment* paneAdj() {
    return gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(side()->textView()));
}
GtkTextView* paneView() { return GTK_TEXT_VIEW(side()->textView()); }

void key(const char* chord) {
    if (ed()->textView()) gtk_widget_grab_focus(ed()->textView());
    MCGuiKey(chord);
}

bool settled() {
    return ed()->showsSide() && side() && side()->idle() && side()->stats().renders > 0;
}

// Section k's heading line (0-based) in the document built.
std::vector<int> g_head;

// `sections` sections: a heading, `paras` paragraphs, a code block in every
// fifth and a table in every seventh. No paragraph says "Section".
std::string syncDoc(int sections, int paras) {
    std::string s = "# Title\n\n";
    int line = 2;
    auto add = [&](const std::string& t) {
        s += t;
        for (char c : t) if (c == '\n') line++;
    };
    g_head.clear();
    for (int k = 0; k < sections; ++k) {
        g_head.push_back(line);
        add("## Section " + std::to_string(k) + "\n\n");
        for (int p = 0; p < paras; ++p)
            add("Paragraph " + std::to_string(k) + "." + std::to_string(p) +
                " has some words in it, and *emphasis* too, enough to be a line.\n\n");
        if (k % 5 == 4) add("```\ncode " + std::to_string(k) + "\nmore code\n```\n\n");
        if (k % 7 == 6) add("| name | value |\n|------|-------|\n| t" + std::to_string(k) + " | 1 |\n\n");
    }
    return s;
}

// The pane's y for the heading "Section k" (a line of its own).
double paneHeadingY(int k) {
    GtkTextIter from, ms, me;
    gtk_text_buffer_get_start_iter(side()->buffer(), &from);
    const std::string needle = "Section " + std::to_string(k) + "\n";
    if (!gtk_text_iter_forward_search(&from, needle.c_str(), GTK_TEXT_SEARCH_TEXT_ONLY, &ms, &me, nullptr))
        return -1;
    GdkRectangle r;
    gtk_text_view_get_iter_location(paneView(), &ms, &r);
    return r.y;
}

double paneTop() {
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(paneView(), &vis);
    return vis.y;
}

double sourceLineY(int line) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line(ed()->active()->buffer(), &it, line);
    int y = 0, h = 0;
    gtk_text_view_get_line_yrange(src(), &it, &y, &h);
    return y;
}

long layoutSum() {
    return (long)(gtk_adjustment_get_upper(srcAdj()) + gtk_adjustment_get_upper(paneAdj()));
}

void openDoc(const char* file, int sections, int paras) {
    MCGuiStep(std::string("open ") + file, [=] {
        ed()->closeAllTabs();
        MCGuiWriteFile(F(file), syncDoc(sections, paras));
        MCGuiOpenFileKept(F(file));
    });
    MCGuiWait("the page, which Markdown opens as", 20, [] { return ed()->active() && ed()->inPreview(); });
    MCGuiStep("its source", [] { key("ctrl+shift+v"); });
    MCGuiWait("source on show", 20, [] { return ed()->active() && ed()->isSource(); });
    MCGuiStep("open the pane", [] { key("ctrl+alt+v"); });
    MCGuiWait("the whole page", 90, [] { return settled(); });
    MCGuiWaitQuiet("the layout", 1.0, 60, [] { return layoutSum(); });
}

}  // namespace

MC_GUI_SUITE(markdown_side_sync) {
    openDoc("sync.md", 60, 8);

    MCGuiStep("scroll the source to a heading", [] {
        GCHECK(side()->stats().pieces > 50);
        gtk_adjustment_set_value(srcAdj(), sourceLineY(g_head[30]));
    });
    MCGuiWait("the page follows", 10, [] {
        const double y = paneHeadingY(30);
        return y >= 0 && std::fabs(y - paneTop()) < 24 && !side()->syncing();
    });
    MCGuiStep("with that heading at its top", [] {
        MCGuiNote("heading 30 at %.1f of the pane's top %.1f", paneHeadingY(30), paneTop());
        GCHECK(std::fabs(paneHeadingY(30) - paneTop()) < 24);
        GCHECK(!side()->pageLeads());
    });

    MCGuiStep("scroll the page to another heading", [] {
        gtk_adjustment_set_value(paneAdj(), paneHeadingY(10));
    });
    MCGuiWait("the source follows", 10, [] {
        return std::fabs(side()->sourceTopLine() - g_head[10]) < 1.5 && !side()->syncing();
    });
    MCGuiStep("to that heading's line", [] {
        MCGuiNote("source top line %.2f, heading at %d", side()->sourceTopLine(), g_head[10]);
        GCHECK(std::fabs(side()->sourceTopLine() - g_head[10]) < 1.5);
        GCHECK(side()->pageLeads());
        GCHECK(std::fabs(paneHeadingY(10) - paneTop()) < 24);   // and the page stayed
    });

    MCGuiStep("back to the top, then the caret far down", [] {
        gtk_adjustment_set_value(srcAdj(), 0);
    });
    MCGuiWait("the page at its top", 10, [] { return paneTop() < 5 && !side()->syncing(); });
    MCGuiStep("move the caret", [] {
        GtkTextBuffer* b = ed()->active()->buffer();
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(b, &it, g_head[50] + 2);
        gtk_text_buffer_place_cursor(b, &it);
    });
    MCGuiWait("its block in view", 10, [] {
        const double y = paneHeadingY(50);
        GdkRectangle vis;
        gtk_text_view_get_visible_rect(paneView(), &vis);
        return y >= vis.y && y < vis.y + vis.height && !side()->syncing();
    });
    MCGuiStep("shown", [] {
        MCGuiNote("heading 50 at %.1f, pane top %.1f", paneHeadingY(50), paneTop());
        GCHECK(paneHeadingY(50) >= paneTop());
    });

    static double s_src = 0;
    static int s_renders = 0;
    MCGuiStep("scroll the source to the middle, then edit in view", [] {
        gtk_adjustment_set_value(srcAdj(), sourceLineY(g_head[20]));
    });
    MCGuiWait("the page there", 10, [] {
        return std::fabs(paneHeadingY(20) - paneTop()) < 24 && !side()->syncing();
    });
    MCGuiStep("one letter in the paragraph below", [] {
        s_src = gtk_adjustment_get_value(srcAdj());
        s_renders = side()->stats().renders;
        GtkTextBuffer* b = ed()->active()->buffer();
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(b, &it, g_head[20] + 2);
        gtk_text_buffer_place_cursor(b, &it);
        gtk_text_buffer_insert(b, &it, "x", 1);
    });
    MCGuiWait("the re-render", 20, [] { return side()->idle() && side()->stats().renders > s_renders; });
    MCGuiWait("settled", 10, [] { return !side()->syncing(); });
    MCGuiStep("which did not move the source, and the page still follows it", [] {
        MCGuiNote("source %.1f -> %.1f", s_src, gtk_adjustment_get_value(srcAdj()));
        GCHECK(std::fabs(gtk_adjustment_get_value(srcAdj()) - s_src) < 1.0);
        GCHECK(std::fabs(paneHeadingY(20) - paneTop()) < 24);
        GCHECK(!side()->pageLeads());
    });

    // The cost of one sync at 20,000 lines.
    openDoc("sync20000.md", 400, 24);
    static double s_cpu = 0;
    MCGuiStep("sync the page to the source's middle, in one go", [] {
        GCHECK(ed()->active()->buffer() != nullptr);
        GCHECK(gtk_text_buffer_get_line_count(ed()->active()->buffer()) > 19000);
        const double c0 = MCGuiThreadCpuMs();
        gtk_adjustment_set_value(srcAdj(), sourceLineY(g_head[200]));
        s_cpu = MCGuiThreadCpuMs() - c0;
        MCGuiNote("20,000 lines: one sync, main %.2f ms", s_cpu);
        GCHECK_TIME(s_cpu, 15, "one sync at 20,000 lines");
    });
    MCGuiWait("the page there", 40, [] {
        const double y = paneHeadingY(200);
        return y >= 0 && std::fabs(y - paneTop()) < 24 && !side()->syncing();
    });
    MCGuiStep("close", [] { ed()->closeAllTabs(); });
}
