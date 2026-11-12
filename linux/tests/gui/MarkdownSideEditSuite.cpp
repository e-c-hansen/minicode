// MarkdownSideEditSuite.cpp — editing from the Markdown page to the side
// (`./build/minicode-guitest markdown_side_edit`, linux/PARITY.md item 4.2):
// a real double-click on a paragraph, a list item and a table cell of the
// pane opens the block editor over the pane with that block's Markdown; an
// edit lands in the source as one undo step, the pane shows it and the
// caret keeps its line; a double-click right after typing edits nothing (the
// page renders first); the full preview's double-click still works.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "MarkdownSide.h"

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }
MarkdownSide* side() { return ed()->sidePane(); }

const char* kDoc =
    "# Title\n\nAlpha paragraph words.\n\n- item one\n- item two\n\n"
    "| h1 | h2 |\n|----|----|\n| c1 | c2 |\n\nOmega.\n";

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

// A double-click on `needle` in a text view (a little in from the pane's edge,
// where the paned's handle takes the pointer) (the pane's, or the full page's).
void doubleClickText(GtkWidget* view, const std::string& needle) {
    GtkTextView* tv = GTK_TEXT_VIEW(view);
    GtkTextBuffer* b = gtk_text_view_get_buffer(tv);
    GtkTextIter s, e, m;
    gtk_text_buffer_get_start_iter(b, &s);
    GCHECK(gtk_text_iter_forward_search(&s, needle.c_str(), GTK_TEXT_SEARCH_TEXT_ONLY, &m, &e,
                                        nullptr));
    GdkRectangle r;
    gtk_text_view_get_iter_location(tv, &m, &r);
    int wx, wy;
    gtk_text_view_buffer_to_window_coords(tv, GTK_TEXT_WINDOW_WIDGET, r.x + 16, r.y + r.height / 2,
                                          &wx, &wy);
    MCGuiDoubleClick(view, wx, wy);
}

GtkWidget* labelWith(GtkWidget* w, const std::string& text) {
    if (GTK_IS_LABEL(w) && text == gtk_label_get_text(GTK_LABEL(w))) return w;
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* f = labelWith(c, text)) return f;
    return nullptr;
}

int caretLine() {
    GtkTextBuffer* b = ed()->active()->buffer();
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(b, &it, gtk_text_buffer_get_insert(b));
    return gtk_text_iter_get_line(&it);
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Edit the open editor's text and press Enter in it (the key is the next step's, with the popover holding the keyboard).
void commit(const std::string& text) { ed()->setBlockEditorText(text); }

}  // namespace

MC_GUI_SUITE(markdown_side_edit) {
    MCGuiStep("a Markdown file, in its source with the pane", [] {
        ed()->closeAllTabs();
        MCGuiWriteFile(F("e.md"), kDoc);
        MCGuiOpenFileKept(F("e.md"));
    });
    MCGuiWait("the page", 10, [] { return ed()->active() && ed()->inPreview(); });
    MCGuiStep("its source", [] { key("ctrl+shift+v"); });
    MCGuiWait("the source", 10, [] { return ed()->active() && ed()->isSource(); });
    MCGuiStep("the pane", [] { key("ctrl+alt+v"); });
    MCGuiWait("its page", 20, [] { return settled(); });
    MCGuiStep("the caret on the last paragraph; a double-click on the first", [] {
        GtkTextBuffer* b = ed()->active()->buffer();
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(b, &it, 12);
        gtk_text_buffer_place_cursor(b, &it);
        GCHECK_EQ_STR(std::to_string(caretLine()), "12");
        doubleClickText(side()->textView(), "Alpha");
    });
    MCGuiWait("the editor", 10, [] { return ed()->blockEditorOpen(); });
    MCGuiStep("holds that paragraph, over the pane", [] {
        GCHECK(ed()->blockEditorInPane());
        GCHECK_EQ_STR(ed()->blockEditorText(), "Alpha paragraph words.");
        commit("Alpha CHANGED words.");
    });
    MCGuiStep("Enter in the editor", [] { MCGuiKey("enter"); });
    MCGuiWait("the editor closed and the pane rendered", 10, [] {
        return !ed()->blockEditorOpen() && settled() && has(side()->pageText(), "CHANGED");
    });
    MCGuiStep("the source has the edit, the caret its line", [] {
        const std::string t = bufferText();
        GCHECK(has(t, "# Title\n\nAlpha CHANGED words.\n\n- item one\n"));
        GCHECK_EQ_STR(std::to_string(caretLine()), "12");
        GCHECK(!has(side()->pageText(), "Alpha paragraph"));
        key("ctrl+z");   // one step takes it back
    });
    MCGuiWait("the page again", 10, [] { return settled() && has(side()->pageText(), "Alpha paragraph"); });
    MCGuiStep("the source is as it was", [] { GCHECK_EQ_STR(bufferText(), kDoc); });

    MCGuiStep("typing, and a double-click at once", [] {
        GtkTextBuffer* b = ed()->active()->buffer();
        GtkTextIter it;
        gtk_text_buffer_get_end_iter(b, &it);
        gtk_text_buffer_insert(b, &it, "tail\n", -1);
        GCHECK(!side()->idle());
        doubleClickText(side()->textView(), "Alpha");
    });
    MCGuiWait("the page rendered", 10, [] { return settled() && has(side()->pageText(), "tail"); });
    MCGuiWaitQuiet("the page still", 0.6, 10, [] { return (long)side()->stats().renders; });
    MCGuiStep("edited nothing and opened nothing", [] {
        GCHECK(!ed()->blockEditorOpen());
        GCHECK_EQ_STR(bufferText(), std::string(kDoc) + "tail\n");
        doubleClickText(side()->textView(), "item two");
    });
    MCGuiWait("the editor on the list item", 10, [] { return ed()->blockEditorOpen(); });
    MCGuiStep("which holds it", [] {
        GCHECK(has(ed()->blockEditorText(), "item two"));
        GCHECK(!has(ed()->blockEditorText(), "item one"));
        std::string t = ed()->blockEditorText();
        t.replace(t.find("two"), 3, "2");
        commit(t);
    });
    MCGuiStep("Enter in the editor", [] { MCGuiKey("enter"); });
    MCGuiWait("the item edited", 10, [] { return !ed()->blockEditorOpen() && settled() && has(side()->pageText(), "item 2"); });
    MCGuiStep("in the source, between its neighbours", [] {
        GCHECK(has(bufferText(), "- item one\n- item 2\n\n| h1"));
        key("ctrl+z");
    });
    MCGuiWait("undone", 10, [] { return settled() && has(side()->pageText(), "item two"); });
    MCGuiStep("one step", [] {
        GCHECK(has(bufferText(), "- item one\n- item two\n"));
        GCHECK(has(bufferText(), "tail\n"));   // the typing is still there
        GtkWidget* table = side()->embedWidgets().at(0);
        GtkWidget* cell = labelWith(table, "c1");
        GCHECK(cell != nullptr);
        if (cell) MCGuiDoubleClick(cell);
    });
    MCGuiWait("the editor on the cell", 10, [] { return ed()->blockEditorOpen(); });
    MCGuiStep("which holds it", [] {
        GCHECK(ed()->blockEditorInPane());
        GCHECK_EQ_STR(ed()->blockEditorText(), "c1");
        commit("z1");
    });
    MCGuiStep("Enter in the editor", [] { MCGuiKey("enter"); });
    MCGuiWait("the cell edited", 10, [] { return !ed()->blockEditorOpen() && settled() && has(bufferText(), "| z1 | c2 |"); });
    MCGuiStep("the page has it", [] {
        GtkWidget* cell = labelWith(side()->embedWidgets().at(0), "z1");
        GCHECK(cell != nullptr);
        key("ctrl+z");
    });
    MCGuiWait("undone", 10, [] { return settled() && has(bufferText(), "| c1 | c2 |"); });

    MCGuiStep("the full preview", [] { key("ctrl+shift+v"); });
    MCGuiWait("the full page", 10, [] { return ed()->inPreview(); });
    MCGuiStep("a double-click on its paragraph", [] {
        doubleClickText(ed()->textView(), "Alpha");
    });
    MCGuiWait("its editor", 10, [] { return ed()->blockEditorOpen(); });
    MCGuiStep("holds the paragraph, over the page", [] {
        GCHECK(!ed()->blockEditorInPane());
        GCHECK_EQ_STR(ed()->blockEditorText(), "Alpha paragraph words.");
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return !ed()->blockEditorOpen(); });
    MCGuiStep("back to the source", [] { key("ctrl+shift+v"); });
    MCGuiWait("the source", 10, [] { return ed()->isSource(); });
}
