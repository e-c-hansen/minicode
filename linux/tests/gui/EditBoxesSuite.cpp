// EditBoxesSuite.cpp — edit_boxes: what is typed in an edit box stays
// through an app switch (linux/HANDOFF.md item 119; the Mac's #223, checked
// there by markdown_page and lsp_fixes; the rename field is lsp_popups').
//
//   - The Markdown page's block editor (a double-click on a paragraph) says
//     "Enter applies" and has an Apply button, as the Mac's does since #223
//     (it puts the edit in the buffer, never on disk). Typing in it, then
//     another program taking the keyboard and giving it back, leaves it
//     open with what was typed, and the keyboard in it again.
//   - Only Escape and Cancel drop the draft: a click elsewhere in the
//     window applies it, as one undo step; so do Ctrl+S (which then saves)
//     and showing another tab.
//   - The tree's name box (Rename, F2 in the tree) keeps its typing through
//     the same switch.
#include "Harness.h"

#include "Editor.h"
#include "FileTree.h"

#include <functional>
#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
FileTree* tree() { return MCGuiApp().tree; }
std::string F(const std::string& name) { return MCGuiProjectFile("boxes/" + name); }
const std::string kPage = "# Boxes\n\nFirst paragraph words.\n\nSecond paragraph words.\n";

std::string bufferText() {
    Document* d = ed()->documentFor(F("page.md"));
    if (!d) return "<no tab>";
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(d->buffer(), &a, &b);
    char* t = gtk_text_buffer_get_text(d->buffer(), &a, &b, TRUE);
    std::string s = t ? t : "";
    g_free(t);
    return s;
}

bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

void doubleClickText(const std::string& needle) {
    GtkTextView* tv = GTK_TEXT_VIEW(ed()->textView());
    GtkTextBuffer* b = gtk_text_view_get_buffer(tv);
    GtkTextIter s, e, m;
    gtk_text_buffer_get_start_iter(b, &s);
    GCHECK(gtk_text_iter_forward_search(&s, needle.c_str(), GTK_TEXT_SEARCH_TEXT_ONLY, &m, &e, nullptr));
    GdkRectangle r;
    gtk_text_view_get_iter_location(tv, &m, &r);
    int wx, wy;
    gtk_text_view_buffer_to_window_coords(tv, GTK_TEXT_WINDOW_WIDGET, r.x + 16, r.y + r.height / 2, &wx, &wy);
    MCGuiDoubleClick(GTK_WIDGET(tv), wx, wy);
}

GtkWidget* firstOf(GtkWidget* w, GType type) {
    if (!w) return nullptr;
    if (G_TYPE_CHECK_INSTANCE_TYPE(w, type)) return w;
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = firstOf(c, type)) return hit;
    return nullptr;
}
// The block editor's popover (mapped, under the editor's view) and its text.
GtkWidget* boxPopover() {
    std::function<GtkWidget*(GtkWidget*)> find = [&](GtkWidget* w) -> GtkWidget* {
        if (GTK_IS_POPOVER(w) && gtk_widget_get_mapped(w) && firstOf(w, GTK_TYPE_TEXT_VIEW)) return w;
        for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
            if (GtkWidget* hit = find(c)) return hit;
        return nullptr;
    };
    return find(ed()->textView());
}
GtkWidget* boxText() { return firstOf(boxPopover(), GTK_TYPE_TEXT_VIEW); }
bool boxHasKeyboard() {
    GtkWidget* t = boxText();
    return t && gtk_widget_has_focus(t) && gtk_window_is_active(MCGuiApp().window);
}

// Opens the block editor on the first paragraph and types over the word.
void openAndType(const std::string& what, const std::string& typed) {
    MCGuiStep(what + ": a double-click on the first paragraph", [] { doubleClickText("First"); });
    MCGuiWait("the block editor", 10, [] { return ed()->blockEditorOpen(); });
    MCGuiWait("with the keyboard", 5, [] { return boxHasKeyboard(); });
    MCGuiStep("typed over the word", [typed] { MCGuiType(typed); });
    MCGuiWait("in the box", 5, [typed] { return has(ed()->blockEditorText(), typed); });
}

// Another program takes the keyboard for a moment and gives it back.
void appSwitch(std::function<bool()> stillOpen) {
    MCGuiStep("another program takes the keyboard", [] { MCGuiFocusElsewhere(); });
    MCGuiWait("the window has lost it", 5, [] { return !gtk_window_is_active(MCGuiApp().window); });
    MCGuiPause(0.5);   // a negative check: nothing may close the box meanwhile
    MCGuiStep("the box still up; the window given back", [stillOpen] {
        GCHECK(stillOpen());
        MCGuiFocusKeyWindow();
    });
    MCGuiWait("the window has the keyboard", 5, [] { return gtk_window_is_active(MCGuiApp().window); });
}

GtkWidget* nameEntry() { return firstOf(tree()->namePopover(), GTK_TYPE_ENTRY); }
std::string nameTyped() {
    GtkWidget* e = nameEntry();
    return e ? gtk_editable_get_text(GTK_EDITABLE(e)) : "<no box>";
}
bool nameHasKeyboard() {
    GtkWidget* e = nameEntry();
    return e && gtk_widget_is_visible(e) && gtk_widget_has_focus(firstOf(e, GTK_TYPE_TEXT)) &&
           gtk_window_is_active(MCGuiApp().window);
}

}  // namespace

MC_GUI_SUITE(edit_boxes) {
    MCGuiStep("a Markdown page", [] {
        MCGuiSetKeyWindow(nullptr);
        ed()->closeAllTabs();
        MCGuiWriteFile(F("page.md"), kPage);
        MCGuiWriteFile(F("other.txt"), "other\n");
        MCGuiOpenFileKept(F("page.md"));
    });
    MCGuiWait("the page", 10, [] { return ed()->active() && ed()->inPreview(); });
    MCGuiStep("the editor has the keyboard", [] {
        MCGuiFocusKeyWindow();
        ed()->focusActiveDocument();
    });
    openAndType("the wording", "Changed ");
    MCGuiStep("\"Enter applies\" and Apply, as the Mac's", [] {
        GtkWidget* pop = boxPopover();
        GtkWidget* label = firstOf(pop, GTK_TYPE_LABEL);
        const std::string title = label ? gtk_label_get_text(GTK_LABEL(label)) : "";
        MCGuiNote("title: %s", title.c_str());
        GCHECK(has(title, "Enter applies, Shift+Enter for a new line"));
        GCHECK(MCGuiFindButton(pop, "Apply") != nullptr);
        GCHECK(MCGuiFindButton(pop, "Save") == nullptr);
    });
    appSwitch([] { return ed()->blockEditorOpen(); });
    MCGuiWait("the box has the keyboard again", 5, [] { return ed()->blockEditorOpen() && boxHasKeyboard(); });
    MCGuiStep("what was typed kept; typing goes on in it", [] {
        GCHECK(has(ed()->blockEditorText(), "Changed "));
        MCGuiType("more ");
    });
    MCGuiWait("typed on", 5, [] { return has(ed()->blockEditorText(), "Changed more "); });
    MCGuiStep("a click elsewhere in the window: the status bar", [] {
        MCGuiClick(GTK_WIDGET(MCGuiApp().statusBar));
    });
    MCGuiWait("the box closed, the draft applied", 5, [] {
        return !ed()->blockEditorOpen() && has(bufferText(), "Changed more ");
    });
    MCGuiStep("not saved; one Ctrl+Z takes it back", [] {
        GCHECK_EQ_STR(MCGuiReadFile(F("page.md")), kPage);
        MCGuiFocusKeyWindow();
        ed()->focusActiveDocument();
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("taken back", 5, [] { return bufferText() == kPage; });

    openAndType("Escape", "Dropped ");
    MCGuiStep("Escape", [] { MCGuiKey("Escape"); });
    MCGuiWait("closed, nothing applied", 5, [] { return !ed()->blockEditorOpen(); });
    MCGuiStep("the text as it was", [] { GCHECK_EQ_STR(bufferText(), kPage); });

    openAndType("Ctrl+S", "Saved ");
    MCGuiStep("Ctrl+S", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("applied and written", 5, [] {
        return !ed()->blockEditorOpen() && has(MCGuiReadFile(F("page.md")), "Saved ");
    });
    MCGuiStep("the buffer the same as the file", [] {
        GCHECK_EQ_STR(bufferText(), MCGuiReadFile(F("page.md")));
    });

    openAndType("another tab", "Tabbed ");
    MCGuiStep("another tab shown", [] { MCGuiOpenFileKept(F("other.txt")); });
    MCGuiWait("other.txt on show", 5, [] { return ed()->currentPath() == F("other.txt"); });
    MCGuiStep("the draft applied to the page's buffer", [] {
        GCHECK(!ed()->blockEditorOpen());
        GCHECK(has(bufferText(), "Tabbed "));
    });

    // ---- the tree's name box
    MCGuiBringTreeRow(F("other.txt"));
    MCGuiStep("a click on the row", [] { MCGuiClick(MCGuiTreeRow(F("other.txt"))); });
    MCGuiWait("the row selected", 5, [] { return tree()->selectedPath() == F("other.txt"); });
    MCGuiStep("F2", [] { MCGuiKey("F2"); });
    MCGuiWait("the name box, with the keyboard", 5, [] { return nameHasKeyboard(); });
    MCGuiStep("typed", [] { MCGuiType("renamed"); });
    MCGuiWait("in the box", 5, [] { return has(nameTyped(), "renamed"); });
    appSwitch([] { return tree()->namePopover() && gtk_widget_get_mapped(tree()->namePopover()); });
    MCGuiWait("the box has the keyboard again", 5, [] { return nameHasKeyboard(); });
    MCGuiStep("the typing kept; Escape", [] {
        GCHECK(has(nameTyped(), "renamed"));
        MCGuiKey("Escape");
    });
    MCGuiWait("closed, nothing renamed", 5, [] {
        return !tree()->namePopover() && MCGuiFileExists(F("other.txt"));
    });
    MCGuiStep("the page put back; the tabs closed", [] {
        Document* d = ed()->documentFor(F("page.md"));
        if (d) gtk_text_buffer_set_modified(d->buffer(), FALSE);
        MCGuiWriteFile(F("page.md"), kPage);
        ed()->closeAllTabs();
    });
}
