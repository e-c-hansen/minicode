// SplitEditorSuite.cpp — `./build/minicode-guitest split_editor`: the split
// editor (linux/PARITY.md 4.6, Groups.cpp), VS Code's two editor groups as
// the Mac's suite of the same name checks them, by real keys, clicks and a
// drag. Ctrl+\ opens the file on show in a second group on the right, one
// document shown by two views on one buffer: what is typed in one shows in
// the other and marks both tabs unsaved, and undo is one history. Each
// group keeps its own caret and scroll; Ctrl+1 and Ctrl+2 and a click in
// the other group move the keyboard; Ctrl+Alt+Shift+Page Down moves the tab
// on show to the next group; the tab keys act on the focused group;
// closing a group's last tab closes the group; the bar between the groups
// drags, within each side's minimum; and a new window on the folder comes
// back split.
#include "WindowLayout.h"
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "Persistence.h"
#include "TabSessions.h"
#include "TabStrip.h"

#include <cmath>
#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }
std::string Name(Document* d) {
    if (!d) return "-";
    const size_t s = d->path().rfind('/');
    return d->path().substr(s + 1);
}

// A group's tabs, its tab on show marked: "s-a.txt >s-b.txt".
std::string Group(int g) {
    std::string out;
    for (Document* d : ed()->groupDocuments(g))
        out += (out.empty() ? "" : " ") + std::string(d == ed()->groupActive(g) ? ">" : "") + Name(d);
    return out;
}

std::string Groups() {
    return ed()->isSplit() ? Group(0) + " | " + Group(1) + (ed()->focusedGroup() ? " (right)" : " (left)")
                           : Group(0);
}

GtkTextBuffer* Buf() { return gtk_text_view_get_buffer(GTK_TEXT_VIEW(ed()->textView())); }

int Caret() {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(Buf(), &it, gtk_text_buffer_get_insert(Buf()));
    return gtk_text_iter_get_offset(&it);
}

int TopLine(GtkWidget* view) {
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(view), &vis);
    GtkTextIter it;
    gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(view), &it, vis.y, nullptr);
    return gtk_text_iter_get_line(&it);
}

std::string Text(GtkTextBuffer* b) {
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* t = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    std::string out = t;
    g_free(t);
    return out;
}

bool Keyboard() { return gtk_widget_has_focus(ed()->textView()); }

TabSessions::Session SavedFor(const std::string& folder) {
    const TabSessions::Sessions all = TabSessions::decode(
        MCGuiReadFile(MCGuiScratch() + "/state/minicode/sessions.json"));
    const auto it = all.find(Persistence::keyFor(folder));
    return it == all.end() ? TabSessions::Session() : it->second;
}

// The bar dragged `dx` pixels, by real pointer events, in the groups'
// coordinates (the harness takes a negative coordinate for the middle).
void DragBar(double dx) {
    GtkWidget* bar = ed()->groupBar();
    graphene_point_t in = GRAPHENE_POINT_INIT(gtk_widget_get_width(bar) / 2.0f,
                                              gtk_widget_get_height(bar) / 2.0f), at;
    GCHECK(gtk_widget_compute_point(bar, ed()->widget(), &in, &at));
    MCGuiDrag(ed()->widget(), at.x, at.y, at.x + dx, at.y);
}

std::string kA;
int gMirrorTop = -1;
double gLeft = 0;
size_t gWindows = 0;

MC_GUI_SUITE(split_editor) {
    MCGuiStep("three files in tabs, the first on show", [] {
        MCGuiSetKeyWindow(nullptr);
        // The bar's drags need the editor's full width: with a browser pane
        // an earlier suite left open the editor is half as wide and the
        // groups' minimum stops the drag short (as it should).
        if (MCGuiApp().layout->browserShown()) MCGuiApp().layout->hideBrowser();
        if (MCGuiApp().layout->terminalShown()) MCGuiApp().layout->hideTerminal();
        for (int i = 1; i <= 120; i++) kA += "line " + std::to_string(i) + "\n";
        MCGuiWriteFile(F("s-a.txt"), kA);
        MCGuiWriteFile(F("s-b.txt"), "bee\n");
        MCGuiWriteFile(F("s-c.txt"), "sea\n");
        ed()->closeAllTabs();
        MCGuiOpenFileKept(F("s-a.txt"));
    });
    // Laid out before it leaves the screen, so the place it keeps is its top.
    MCGuiWait("s-a.txt laid out", 10, [] {
        return ed()->currentPath() == F("s-a.txt") && MCGuiTextLaidOut(ed()->textView());
    });
    MCGuiStep("then the others, and back to the first", [] {
        MCGuiOpenFileKept(F("s-b.txt"));
        MCGuiOpenFileKept(F("s-c.txt"));
        ed()->selectTab(0);
    });
    MCGuiWait("s-a.txt on show with the keyboard, laid out", 10, [] {
        return ed()->currentPath() == F("s-a.txt") && Keyboard() && MCGuiTextLaidOut(ed()->textView());
    });
    MCGuiStep("its caret and view at the top", [] { MCGuiKey("ctrl+home"); });
    MCGuiWait("at the top", 5, [] { return Caret() == 0 && TopLine(ed()->textView()) == 0; });
    MCGuiStep("Ctrl+\\ splits the editor", [] {
        GCHECK(!ed()->isSplit());
        GCHECK(!gtk_widget_get_visible(ed()->groupBar()));
        MCGuiKey("ctrl+\\");
    });
    MCGuiWait("two groups, the new one on the right with the keyboard", 10, [] {
        return ed()->isSplit() && ed()->focusedGroup() == 1 && Keyboard() &&
               MCGuiOnScreen(ed()->mirrorView());
    });
    MCGuiStep("one document in both: one buffer, two views", [] {
        GCHECK_EQ_STR(Groups(), ">s-a.txt s-b.txt s-c.txt | >s-a.txt (right)");
        Document* left = ed()->groupActive(0);
        Document* right = ed()->groupActive(1);
        GCHECK(left == right);
        GCHECK(gtk_text_view_get_buffer(GTK_TEXT_VIEW(ed()->mirrorView())) == Buf());
        GCHECK(ed()->mirrorView() != ed()->textView());
        // Every document once, for the backups, the session and the servers.
        GCHECK(ed()->documents().size() == 3);
        GCHECK(ed()->otherStrip()->count() == 3 && ed()->strip()->count() == 1);
        // The right group is right of the left one, the bar between them.
        graphene_rect_t l, r;
        GCHECK(gtk_widget_compute_bounds(ed()->groupWidget(0), ed()->widget(), &l));
        GCHECK(gtk_widget_compute_bounds(ed()->groupWidget(1), ed()->widget(), &r));
        GCHECK(l.origin.x < r.origin.x && l.origin.x + l.size.width <= r.origin.x);
        MCGuiType("XY");
    });
    MCGuiWait("what is typed in the right group shows in the left", 5, [] {
        return Text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(ed()->mirrorView()))) == "XY" + kA;
    });
    MCGuiStep("both tabs are unsaved", [] {
        GCHECK(ed()->strip()->item(0).dirty);
        GCHECK(ed()->otherStrip()->item(0).dirty);
        // The right group's caret goes to the end, the view following it.
        MCGuiKey("ctrl+end");
    });
    MCGuiWait("the right view scrolled down", 5, [] { return TopLine(ed()->textView()) > 20; });
    MCGuiStep("Ctrl+1: the left group has the keyboard", [] { MCGuiKey("ctrl+1"); });
    MCGuiWait("the left group focused", 5, [] { return ed()->focusedGroup() == 0 && Keyboard(); });
    MCGuiWait("its caret and scroll its own: at the top, where Ctrl+\\ left them", 5, [] {
        return Caret() == 0 && TopLine(ed()->textView()) == 0 && TopLine(ed()->mirrorView()) > 20;
    });
    MCGuiStep("undo there undoes what the right group typed", [] {
        GCHECK_EQ_STR(Groups(), ">s-a.txt s-b.txt s-c.txt | >s-a.txt (left)");
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("one history: the text as it was, in both views", 5, [] {
        return Text(Buf()) == kA && Text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(ed()->mirrorView()))) == kA;
    });
    MCGuiStep("Ctrl+2: the right group back, at its own place", [] { MCGuiKey("ctrl+2"); });
    MCGuiWait("the right group focused, its caret at the end", 5, [] {
        return ed()->focusedGroup() == 1 && Keyboard() &&
               Caret() == gtk_text_buffer_get_char_count(Buf()) && TopLine(ed()->textView()) > 20;
    });
    MCGuiWait("and the left view at the top", 5, [] { return TopLine(ed()->mirrorView()) == 0; });

    // ---- a click in the other group focuses it, the caret where it landed
    MCGuiStep("a click on the left group's text", [] {
        gMirrorTop = TopLine(ed()->mirrorView());
        MCGuiClick(ed()->mirrorView(), 80, 40);
    });
    MCGuiWait("the left group has the keyboard", 5, [] { return ed()->focusedGroup() == 0 && Keyboard(); });
    MCGuiStep("the caret on the line clicked", [] {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_mark(Buf(), &it, gtk_text_buffer_get_insert(Buf()));
        MCGuiNote("caret line %d, the left view's top line was %d", gtk_text_iter_get_line(&it), gMirrorTop);
        GCHECK(std::abs(gtk_text_iter_get_line(&it) - gMirrorTop) <= 2);
    });

    // ---- moving a tab, and the tab keys in a group
    MCGuiStep("the left group shows s-c.txt, then s-b.txt", [] { MCGuiKey("alt+3"); });
    MCGuiWait("s-c.txt", 5, [] { return ed()->currentPath() == F("s-c.txt"); });
    MCGuiStep("then s-b.txt", [] { MCGuiKey("alt+2"); });
    MCGuiWait("s-b.txt", 5, [] { return ed()->currentPath() == F("s-b.txt"); });
    MCGuiStep("Ctrl+Alt+Shift+Page Down moves it to the next group", [] {
        MCGuiKey("ctrl+alt+shift+pagedown");
    });
    MCGuiWait("s-b.txt in the right group, which has the keyboard", 5, [] {
        return Groups() == "s-a.txt >s-c.txt | s-a.txt >s-b.txt (right)" && Keyboard();
    });
    MCGuiStep("Ctrl+Page Up steps through the right group's tabs only", [] {
        GCHECK_EQ_STR(Groups(), "s-a.txt >s-c.txt | s-a.txt >s-b.txt (right)");
        MCGuiKey("ctrl+pageup");
    });
    MCGuiWait("s-a.txt on show on the right, the left untouched", 5, [] {
        return Groups() == "s-a.txt >s-c.txt | >s-a.txt s-b.txt (right)";
    });
    MCGuiStep("Alt+2 is the right group's second tab", [] { MCGuiKey("alt+2"); });
    MCGuiWait("s-b.txt", 5, [] { return Groups() == "s-a.txt >s-c.txt | s-a.txt >s-b.txt (right)"; });
    MCGuiStep("Move Editor into Previous Group puts it back", [] {
        MCGuiKey("ctrl+alt+shift+pageup");
    });
    MCGuiWait("s-b.txt on the left again", 5, [] {
        return Groups() == "s-a.txt s-c.txt >s-b.txt | >s-a.txt (left)" && Keyboard();
    });

    // ---- the bar between the groups
    MCGuiStep("the bar drags 100 px right", [] {
        gLeft = gtk_widget_get_width(ed()->groupWidget(0));
        MCGuiNote("groups %d and %d px, the editor %d px wide", gtk_widget_get_width(ed()->groupWidget(0)),
                  gtk_widget_get_width(ed()->groupWidget(1)), gtk_widget_get_width(ed()->widget()));
        DragBar(100);
    });
    MCGuiWait("the left group is 100 px wider", 5, [] {
        return std::fabs(gtk_widget_get_width(ed()->groupWidget(0)) - (gLeft + 100)) <= 3;
    });
    MCGuiStep("dragged 300 px right, past it, the right group keeps its minimum", [] {
        DragBar(300);
    });
    MCGuiWait("the right group 150 px wide", 5, [] {
        return std::abs(gtk_widget_get_width(ed()->groupWidget(1)) - 150) <= 2;
    });
    MCGuiStep("dragged well left, the left group keeps its minimum", [] {
        DragBar(-600);
    });
    MCGuiWait("the left group 150 px wide", 5, [] {
        return std::abs(gtk_widget_get_width(ed()->groupWidget(0)) - 150) <= 2;
    });
    MCGuiStep("back to halves", [] { ed()->setGroupFraction(0.5); });

    // ---- the session: a new window on the folder comes back split
    MCGuiWait("the split is the folder's session", 10, [] {
        const TabSessions::Session s = SavedFor(MCGuiProject());
        return s.split && s.groups.focused == 0 && s.groups.groups[1].size() == 1;
    });
    MCGuiStep("a new window on the folder", [] {
        gWindows = MCGuiAppWindows().size();
        MCGuiKey("ctrl+shift+n");
    });
    MCGuiWait("it opens split", 10, [] {
        return MCGuiAppWindows().size() == gWindows + 1 && ed()->isSplit();
    });
    MCGuiStep("its groups as they were", [] {
        GCHECK_EQ_STR(Groups(), "s-a.txt s-c.txt >s-b.txt | >s-a.txt (left)");
        GCHECK(ed()->groupActive(1) && !ed()->groupActive(1)->unread());
        MCGuiKey("ctrl+shift+w");
    });
    MCGuiWait("one window again", 10, [] { return MCGuiAppWindows().size() == gWindows; });

    // ---- closing a group's last tab closes the group
    MCGuiStep("Ctrl+2, then Ctrl+W on the right group's only tab", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiKey("ctrl+2");
    });
    MCGuiWait("the right group focused", 5, [] { return ed()->focusedGroup() == 1 && Keyboard(); });
    MCGuiStep("Ctrl+W", [] { MCGuiKey("ctrl+w"); });
    MCGuiWait("one group, with the left group's tabs", 5, [] {
        return !ed()->isSplit() && Groups() == "s-a.txt s-c.txt >s-b.txt";
    });
    MCGuiStep("the other group's widgets are gone from the screen, the document kept", [] {
        GCHECK(!gtk_widget_get_visible(ed()->groupBar()));
        GCHECK(!MCGuiOnScreen(ed()->mirrorView()));
        GCHECK(ed()->documentFor(F("s-a.txt")) != nullptr);
        GCHECK(Keyboard());
        ed()->closeAllTabs();
    });
}

}  // namespace
