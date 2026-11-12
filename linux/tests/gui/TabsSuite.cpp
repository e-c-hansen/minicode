// TabsSuite.cpp — the editor's tabs (`./build/minicode-guitest tabs`,
// linux/PARITY.md item 1.1), by real input: files opened from the tree with
// the mouse, tabs chosen by clicks on the strip and by their keys, each tab
// keeping its own text, caret, scroll and undo; the dot for unsaved changes,
// gone again when undo goes back to the file's text; Close Tab on a tab
// with unsaved changes and each of the question's three answers; Reopen
// Closed Tab with its place, passing over a file gone; Save All; folder
// names on tabs whose files share a name; the Markdown preview toggled
// without touching the source's undo (D3); a tab's right click menu; the
// close button and a middle click; and closing a window with unsaved tabs
// asking once for them all. The Mac has these in EditorController+Tabs.mm.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "FileTree.h"
#include "TabStrip.h"

#include <glib/gstdio.h>

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
TabStrip* strip() { return ed()->strip(); }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

std::string numbered(const std::string& word, int lines) {
    std::string s;
    for (int i = 1; i <= lines; i++) s += word + " line " + std::to_string(i) + "\n";
    return s;
}

const std::string kA = numbered("alpha", 300);
const std::string kB = "def beta():\n    return 1\n" + numbered("# beta", 40);
const std::string kC = "gamma\n";
const std::string kNotes = "# Notes\n\nSome text.\n\n- [ ] a task\n";

// The row as the strip shows it: each title, the preview tab's in brackets,
// a star after one with unsaved changes, ">" before the active one.
std::string row() {
    std::string out;
    for (int i = 0; i < strip()->count(); i++) {
        const TabStrip::Item& it = strip()->item(i);
        std::string t = it.preview ? "[" + it.title + "]" : it.title;
        if (it.dirty) t += "*";
        if (i == strip()->activeIndex()) t = ">" + t;
        out += (out.empty() ? "" : " ") + t;
    }
    return out;
}

// The strip agrees with the editor's own tabs.
bool stripAgrees() {
    if (strip()->count() != ed()->tabCount() || strip()->activeIndex() != ed()->activeIndex())
        return false;
    for (int i = 0; i < ed()->tabCount(); i++) {
        const Document* d = ed()->document(i);
        const TabStrip::Item& it = strip()->item(i);
        if (it.dirty != d->dirty() || it.preview != d->isPreviewTab()) return false;
        const char* mark = gtk_label_get_text(GTK_LABEL(strip()->closeWidget(i)));
        if ((std::string(mark) == "●") != d->dirty()) return false;
    }
    return true;
}

bool tabsOnScreen() {
    for (int i = 0; i < strip()->count(); i++)
        if (!MCGuiOnScreen(strip()->titleWidget(i))) return false;
    return strip()->count() > 0;
}

GtkWidget* treeRow(const std::string& name) {
    return MCGuiFindLabel(MCGuiApp().tree->widget(), name);
}

bool editorHasKeyboard() { return gtk_widget_has_focus(ed()->textView()); }

long caret() {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(ed()->buffer(), &it, gtk_text_buffer_get_insert(ed()->buffer()));
    return gtk_text_iter_get_offset(&it);
}

double scrollY() {
    return gtk_adjustment_get_value(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(ed()->textView())));
}

bool caretOnScreen() {
    GtkTextView* tv = GTK_TEXT_VIEW(ed()->textView());
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(ed()->buffer(), &it, gtk_text_buffer_get_insert(ed()->buffer()));
    GdkRectangle vis, at;
    gtk_text_view_get_visible_rect(tv, &vis);
    gtk_text_view_get_iter_location(tv, &it, &at);
    // At least in part: the last line may sit under the bottom edge.
    const bool on = vis.height > 0 && at.y < vis.y + vis.height && at.y + at.height > vis.y;
    return on;
}

std::string title() { return gtk_window_get_title(MCGuiApp().window); }

// A Markdown page's task box, on screen in the view.
GtkWidget* taskBox(GtkWidget* root) {
    if (!root || !gtk_widget_get_mapped(root)) return nullptr;
    if (GTK_IS_CHECK_BUTTON(root) && gtk_widget_has_css_class(root, "minicode-md-task"))
        return MCGuiOnScreen(root) ? root : nullptr;
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = taskBox(c)) return hit;
    return nullptr;
}

std::string sourceText() {
    GtkTextBuffer* b = ed()->sourceBuffer();
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* t = gtk_text_buffer_get_text(b, &s, &e, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

// A question up, laid out (no window manager places it: it opens at the
// window's corner, and a click before its layout lands elsewhere).
bool questionUp() {
    GtkWindow* top = MCGuiTopWindow();
    if (top == MCGuiApp().window) return false;
    GtkWidget* cancel = MCGuiFindButton(GTK_WIDGET(top), "Cancel");
    return cancel && gtk_widget_get_width(cancel) > 0 && gtk_widget_get_height(cancel) > 0;
}

std::string questionText() {
    GtkWindow* top = MCGuiTopWindow();
    if (top == MCGuiApp().window) return "";
    std::string all;
    // Every label in it, joined: the message and its detail.
    std::vector<GtkWidget*> todo = {GTK_WIDGET(top)};
    while (!todo.empty()) {
        GtkWidget* w = todo.back();
        todo.pop_back();
        if (GTK_IS_LABEL(w)) all += std::string(gtk_label_get_text(GTK_LABEL(w))) + "\n";
        for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
            todo.push_back(c);
    }
    return all;
}

void answer(const char* button) {
    MCGuiClick(MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), button));
}

double gScrollA = 0;
long gCaretA = 0;
long gCaretB = 0;
size_t gWindows = 0;

// a on show with its caret and its view where they were.
bool aBack() {
    return caret() == gCaretA && caretOnScreen() && scrollY() > gScrollA - 20 &&
           scrollY() < gScrollA + 20;
}

}  // namespace

MC_GUI_SUITE(tabs) {
    MCGuiStep("files in the project, and no tab open", [] {
        // An earlier suite's tabs go (their edits were saved).
        ed()->closeAllTabs();
        MCGuiWriteFile(F("tab-a.txt"), kA);
        MCGuiWriteFile(F("tab-b.py"), kB);
        MCGuiWriteFile(F("tab-c.txt"), kC);
        MCGuiWriteFile(F("tab-notes.md"), kNotes);
        MCGuiWriteFile(F("one/README.md"), "# one\n");
        MCGuiWriteFile(F("two/README.md"), "# two\n");
    });
    MCGuiWait("no tab, no strip, and the tree lists the files", 10, [] {
        return ed()->tabCount() == 0 && !gtk_widget_get_visible(strip()->widget()) &&
               treeRow("tab-a.txt") && treeRow("tab-b.py") && treeRow("tab-c.txt");
    });
    MCGuiStep("with no tab, an empty editor, and nothing to save", [] {
        GCHECK(!ed()->canSave() && ed()->currentPath().empty());
        GCHECK(MCGuiEditorText().empty());
        GCHECK_EQ_STR(title(), "MiniCode");
    });

    // ---- opening from the tree
    MCGuiStep("a double-click on a file in the tree opens it in a tab of its own", [] {
        MCGuiDoubleClick(treeRow("tab-a.txt"));
    });
    MCGuiWait("one tab, kept, on show", 10, [] { return row() == ">tab-a.txt"; });
    // A pause where time is the condition: GTK's double-click timer from
    // the last double-click would otherwise count this one's second click
    // as a first (it is not stopped by a click elsewhere).
    MCGuiPause(0.5);
    MCGuiStep("and b", [] { MCGuiDoubleClick(treeRow("tab-b.py")); });
    MCGuiWait("b right of a, on show", 10, [] { return row() == "tab-a.txt >tab-b.py"; });
    MCGuiPause(0.5);   // the double-click interval again
    MCGuiStep("and c", [] { MCGuiDoubleClick(treeRow("tab-c.txt")); });
    MCGuiWait("three tabs, the newest on show, the strip drawn", 10, [] {
        return row() == "tab-a.txt tab-b.py >tab-c.txt" && tabsOnScreen();
    });
    MCGuiStep("each tab holds its own file in a buffer of its own, over one tag table", [] {
        GCHECK(stripAgrees());
        GCHECK(gtk_widget_get_visible(strip()->widget()));
        GCHECK_EQ_STR(MCGuiEditorText(), kC);
        GCHECK_EQ_STR(ed()->document(0)->text(), kA);
        GCHECK_EQ_STR(ed()->document(1)->text(), kB);
        GCHECK(ed()->document(0)->buffer() != ed()->document(1)->buffer());
        GCHECK(gtk_text_buffer_get_tag_table(ed()->document(0)->buffer()) ==
               gtk_text_buffer_get_tag_table(ed()->document(2)->buffer()));
        GCHECK(ed()->buffer() == ed()->document(2)->buffer());
        GCHECK(title().find("tab-c.txt") != std::string::npos);
    });

    // ---- typing in each, chosen by a click and by keys
    MCGuiStep("a click in c's text", [] { MCGuiClick(ed()->textView()); });
    MCGuiWait("the editor has the keyboard", 5, editorHasKeyboard);
    MCGuiStep("typing at c's end", [] {
        MCGuiKey("ctrl+end");
        MCGuiType("C typed");
    });
    MCGuiWait("c has it, and its tab has the dot", 10, [] {
        return MCGuiEditorText() == kC + "C typed" && row() == "tab-a.txt tab-b.py >tab-c.txt*";
    });
    MCGuiStep("the strip and the title say so", [] {
        GCHECK(stripAgrees());
        GCHECK(title().find("tab-c.txt *") != std::string::npos);
    });
    MCGuiStep("a click on a's tab", [] { MCGuiClick(strip()->titleWidget(0)); });
    MCGuiWait("a on show, as read, with the keyboard", 10, [] {
        return row() == ">tab-a.txt tab-b.py tab-c.txt*" && MCGuiEditorText() == kA &&
               editorHasKeyboard();
    });
    MCGuiStep("typing at a's end, far down", [] {
        GCHECK(caret() == 0);   // a fresh tab starts at the top
        MCGuiKey("ctrl+end");
        MCGuiType("A typed");
    });
    MCGuiWait("a has it", 10, [] { return MCGuiEditorText() == kA + "A typed"; });
    // Each of these waits reads what the checks after it read: a quiet view
    // alone, under load, was a view GTK had not moved yet.
    MCGuiWait("a's view at its end", 10, [] {
        return scrollY() > 0 && caretOnScreen() && caret() == g_utf8_strlen((kA + "A typed").c_str(), -1);
    });
    MCGuiWaitQuiet("and still", 0.4, 5, [] { return (long)scrollY(); });
    MCGuiStep("a's caret and scroll", [] {
        gScrollA = scrollY();
        gCaretA = caret();
        GCHECK(gScrollA > 0);
        GCHECK(caretOnScreen());
        GCHECK(gCaretA == g_utf8_strlen((kA + "A typed").c_str(), -1));
    });
    MCGuiStep("Ctrl+PageDown goes to b", [] { MCGuiKey("ctrl+pagedown"); });
    MCGuiWait("b on show, with the keyboard", 10, [] {
        return row() == "tab-a.txt* >tab-b.py tab-c.txt*" && MCGuiEditorText() == kB &&
               editorHasKeyboard();
    });
    MCGuiStep("typing at b's start", [] {
        MCGuiKey("ctrl+home");
        MCGuiType("B typed ");
    });
    MCGuiWait("b has it", 10, [] { return MCGuiEditorText() == "B typed " + kB; });
    MCGuiStep("b's caret", [] { gCaretB = caret(); });
    MCGuiStep("Ctrl+PageDown goes to c", [] { MCGuiKey("ctrl+pagedown"); });
    MCGuiWait("c, its typing and its caret as left", 10, [] {
        return row() == "tab-a.txt* tab-b.py* >tab-c.txt*" &&
               MCGuiEditorText() == kC + "C typed" &&
               caret() == g_utf8_strlen((kC + "C typed").c_str(), -1);
    });
    MCGuiStep("Ctrl+PageDown wraps round to a", [] { MCGuiKey("ctrl+pagedown"); });
    MCGuiWait("a, with its typing", 10, [] {
        return row() == ">tab-a.txt* tab-b.py* tab-c.txt*" && MCGuiEditorText() == kA + "A typed";
    });
    MCGuiWait("its view back", 10, aBack);
    MCGuiStep("a's caret where it was, and its view back where it was", [] {
        GCHECK(caret() == gCaretA);
        GCHECK(caretOnScreen());
        MCGuiNote("a's scroll: %.0f, was %.0f", scrollY(), gScrollA);
        GCHECK(scrollY() > gScrollA - 20 && scrollY() < gScrollA + 20);
    });
    MCGuiStep("Ctrl+PageUp wraps the other way, to c", [] { MCGuiKey("ctrl+pageup"); });
    MCGuiWait("c", 10, [] { return ed()->activeIndex() == 2 && MCGuiEditorText() == kC + "C typed"; });
    MCGuiStep("Ctrl+Tab, the next", [] { MCGuiKey("ctrl+tab"); });
    MCGuiWait("a", 10, [] { return ed()->activeIndex() == 0; });
    MCGuiStep("Ctrl+Shift+Tab, the previous", [] { MCGuiKey("ctrl+shift+tab"); });
    MCGuiWait("c again", 10, [] { return ed()->activeIndex() == 2; });
    MCGuiStep("Alt+2, tab 2", [] { MCGuiKey("alt+2"); });
    MCGuiWait("b, its caret as left", 10, [] {
        return ed()->activeIndex() == 1 && MCGuiEditorText() == "B typed " + kB && caret() == gCaretB;
    });
    MCGuiStep("Alt+1, tab 1", [] { MCGuiKey("alt+1"); });
    MCGuiWait("a", 10, [] { return ed()->activeIndex() == 0; });
    MCGuiStep("Alt+9, the last", [] { MCGuiKey("alt+9"); });
    MCGuiWait("c", 10, [] { return ed()->activeIndex() == 2; });
    MCGuiStep("Alt+1, then Alt+0, the last too", [] {
        MCGuiKey("alt+1");
        MCGuiKey("alt+0");
    });
    MCGuiWait("c", 10, [] { return ed()->activeIndex() == 2 && editorHasKeyboard(); });
    MCGuiStep("Alt+4: no tab 4, so nothing changes", [] { MCGuiKey("alt+4"); });
    MCGuiPause(0.3);   // a negative check: nothing may happen in this time
    MCGuiStep("still c", [] {
        GCHECK(ed()->activeIndex() == 2);
        GCHECK(stripAgrees());
    });
    MCGuiStep("flicking through every tab twice, in one go", [] {
        for (int i = 0; i < 6; i++) MCGuiKey("ctrl+pagedown");
    });
    MCGuiWait("back on c", 10, [] { return ed()->activeIndex() == 2; });
    // A tab left while its place was still coming back once recorded GTK's
    // half-done scroll as its place, and its place drifted to the top (one
    // run in four of this suite, by these keys and "Alt+1, then Alt+0").
    MCGuiPause(0.3);   // a negative check: nothing may move in this time
    MCGuiStep("then a", [] { MCGuiKey("alt+1"); });
    MCGuiWait("a on show", 10, [] { return ed()->activeIndex() == 0; });
    MCGuiWait("its view back", 10, aBack);
    MCGuiStep("a's place was kept through it, its caret too", [] {
        MCGuiNote("a's scroll: %.0f, was %.0f", scrollY(), gScrollA);
        GCHECK(scrollY() > gScrollA - 20 && scrollY() < gScrollA + 20);
        GCHECK(caret() == gCaretA);
        MCGuiKey("alt+9");
    });
    MCGuiWait("c again", 10, [] { return ed()->activeIndex() == 2; });

    // ---- undo is each tab's, and undoing back to the file's text is clean
    MCGuiStep("Ctrl+Z in c, enough times to take back its typing", [] {
        for (int i = 0; i < 6; i++) MCGuiKey("ctrl+z");
    });
    MCGuiWait("c is the file's text again, and clean", 10, [] {
        return MCGuiEditorText() == kC && row() == "tab-a.txt* tab-b.py* >tab-c.txt";
    });
    MCGuiStep("no dot, no star; the other tabs keep their typing", [] {
        GCHECK(stripAgrees());
        GCHECK(!ed()->dirty());
        GCHECK(title().find(" *") == std::string::npos);
        GCHECK_EQ_STR(ed()->document(0)->text(), kA + "A typed");
        GCHECK_EQ_STR(ed()->document(1)->text(), "B typed " + kB);
    });
    MCGuiStep("one more Ctrl+Z in c: nothing left to undo, the file stays", [] { MCGuiKey("ctrl+z"); });
    MCGuiPause(0.3);   // a negative check: the load itself is never undone
    MCGuiStep("c as read", [] { GCHECK_EQ_STR(MCGuiEditorText(), kC); });
    MCGuiStep("a's tab, and Ctrl+Z there", [] {
        MCGuiClick(strip()->titleWidget(0));
    });
    MCGuiWait("a on show", 10, [] { return ed()->activeIndex() == 0 && editorHasKeyboard(); });
    MCGuiStep("its undo", [] {
        for (int i = 0; i < 6; i++) MCGuiKey("ctrl+z");
    });
    MCGuiWait("a's own typing is what goes", 10, [] {
        return MCGuiEditorText() == kA && row() == ">tab-a.txt tab-b.py* tab-c.txt";
    });
    MCGuiStep("and Ctrl+Shift+Z brings it back, unsaved again", [] {
        for (int i = 0; i < 6; i++) MCGuiKey("ctrl+shift+z");
    });
    MCGuiWait("a has its typing again", 10, [] {
        return MCGuiEditorText() == kA + "A typed" && row() == ">tab-a.txt* tab-b.py* tab-c.txt";
    });

    // ---- closing a tab with unsaved changes: each answer
    MCGuiStep("b's tab, and Ctrl+W", [] {
        MCGuiKey("alt+2");
        MCGuiKey("ctrl+w");
    });
    MCGuiWait("the question is up", 10, questionUp);
    MCGuiStep("it names the file", [] {
        MCGuiNote("%s", questionText().c_str());
        GCHECK(questionText().find("Save changes to “tab-b.py”?") != std::string::npos);
        GCHECK(MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), "Save") != nullptr);
        GCHECK(MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), "Don't Save") != nullptr);
    });
    MCGuiStep("Cancel", [] { answer("Cancel"); });
    MCGuiWait("the question goes", 10, [] { return MCGuiTopWindow() == MCGuiApp().window; });
    MCGuiStep("b stays, its typing with it", [] {
        GCHECK_EQ_STR(row(), "tab-a.txt* >tab-b.py* tab-c.txt");
        GCHECK_EQ_STR(MCGuiEditorText(), "B typed " + kB);
        GCHECK_EQ_STR(MCGuiReadFile(F("tab-b.py")), kB);
    });
    MCGuiStep("Ctrl+W again", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiKey("ctrl+w");
    });
    MCGuiWait("the question again", 10, questionUp);
    MCGuiStep("Don't Save", [] { answer("Don't Save"); });
    MCGuiWait("b's tab goes, the file untouched", 10, [] {
        return MCGuiTopWindow() == MCGuiApp().window && ed()->tabCount() == 2;
    });
    MCGuiStep("the tab shown next is the one used most recently: a", [] {
        GCHECK_EQ_STR(row(), ">tab-a.txt* tab-c.txt");
        GCHECK_EQ_STR(MCGuiReadFile(F("tab-b.py")), kB);
        GCHECK_EQ_STR(MCGuiEditorText(), kA + "A typed");
    });
    MCGuiWait("a's view back at its place", 10, [] {
        return scrollY() > gScrollA - 20 && scrollY() < gScrollA + 20;
    });
    MCGuiStep("where it was", [] {
        MCGuiNote("a's scroll: %.0f, was %.0f", scrollY(), gScrollA);
        GCHECK(scrollY() > gScrollA - 20 && scrollY() < gScrollA + 20);
    });
    MCGuiStep("Ctrl+W on a", [] { MCGuiKey("ctrl+w"); });
    MCGuiWait("the question", 10, questionUp);
    MCGuiStep("Save", [] { answer("Save"); });
    MCGuiWait("a's tab goes, its typing in the file", 10, [] {
        return MCGuiTopWindow() == MCGuiApp().window && ed()->tabCount() == 1 &&
               MCGuiReadFile(F("tab-a.txt")) == kA + "A typed";
    });
    MCGuiStep("c is left, on show", [] { GCHECK_EQ_STR(row(), ">tab-c.txt"); });

    // ---- Reopen Closed Tab
    MCGuiStep("Ctrl+Alt+Shift+T reopens the tab closed last", [] { MCGuiKey("ctrl+alt+shift+t"); });
    MCGuiWait("a, back where it was in the row, as saved", 10, [] {
        return row() == ">tab-a.txt tab-c.txt" && MCGuiEditorText() == kA + "A typed";
    });
    MCGuiWait("its view back", 10, aBack);
    MCGuiStep("with its caret and scroll, and the keyboard", [] {
        GCHECK(caret() == gCaretA);
        GCHECK(caretOnScreen());
        MCGuiNote("a's scroll: %.0f, was %.0f", scrollY(), gScrollA);
        GCHECK(scrollY() > gScrollA - 20 && scrollY() < gScrollA + 20);
        GCHECK(editorHasKeyboard());
    });

    // ---- a place waits for the view to be on screen
    // A tab shown before its view is on screen (a session's tab, in a window
    // slow to open on a busy machine) gave its place up two seconds after it
    // was asked for, before the view was ever drawn, and came back at its
    // top. A tab shown while the editor's pane is hidden is the same case,
    // by hand.
    MCGuiStep("c's tab, then the editor hidden (Ctrl+Shift+E)", [] {
        ed()->activate(ed()->document(1));
        MCGuiKey("ctrl+shift+e");
    });
    MCGuiWait("hidden, c on show", 10, [] {
        return ed()->activeIndex() == 1 && !gtk_widget_get_mapped(ed()->textView());
    });
    MCGuiStep("a's tab shown while it is hidden", [] { ed()->activate(ed()->document(0)); });
    MCGuiPause(2.5);   // time is the condition: hidden for longer than the place's two seconds
    MCGuiStep("the editor shown again", [] {
        GCHECK(ed()->activeIndex() == 0 && !gtk_widget_get_mapped(ed()->textView()));
        MCGuiKey("ctrl+shift+e");
    });
    MCGuiWait("a's view back where it was", 10, [] {
        return gtk_widget_get_mapped(ed()->textView()) && aBack();
    });
    MCGuiStep("again: b", [] { MCGuiKey("ctrl+alt+shift+t"); });
    MCGuiWait("b, from its file, where it was", 10, [] {
        return row() == "tab-a.txt >tab-b.py tab-c.txt" && MCGuiEditorText() == kB;
    });
    MCGuiStep("b's caret where it was when it closed", [] { GCHECK(caret() == gCaretB); });
    MCGuiStep("c closed by its close button, with no question (it is clean)", [] {
        MCGuiClick(strip()->closeWidget(2));
    });
    MCGuiWait("c's tab goes", 10, [] { return row() == "tab-a.txt >tab-b.py"; });
    MCGuiStep("b closed by a middle click on it", [] { MCGuiClick(strip()->titleWidget(1), -1, -1, 2); });
    MCGuiWait("b's tab goes", 10, [] { return row() == ">tab-a.txt"; });
    MCGuiStep("b's file is removed, then Reopen", [] {
        GCHECK(MCGuiFileExists(F("tab-b.py")));
        g_remove(F("tab-b.py").c_str());
        MCGuiKey("ctrl+alt+shift+t");
    });
    MCGuiWait("b is passed over: c comes back", 10, [] { return row() == "tab-a.txt >tab-c.txt"; });
    MCGuiStep("b's file again, for later", [] { MCGuiWriteFile(F("tab-b.py"), kB); });

    // ---- Save All
    MCGuiStep("typing in c", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+end");
        MCGuiType("more C");
    });
    MCGuiWait("c has it", 10, [] { return MCGuiEditorText() == kC + "more C"; });
    MCGuiStep("and in a", [] {
        MCGuiKey("alt+1");
        MCGuiKey("ctrl+home");
        MCGuiType("more A ");
    });
    MCGuiWait("a has it; both have their dots", 10, [] {
        return MCGuiEditorText() == "more A " + kA + "A typed" &&
               row() == ">tab-a.txt* tab-c.txt*";
    });
    MCGuiStep("Ctrl+Alt+S saves both", [] { MCGuiKey("ctrl+alt+s"); });
    MCGuiWait("both files have their text, and both tabs are clean", 10, [] {
        return MCGuiReadFile(F("tab-a.txt")) == "more A " + kA + "A typed" &&
               MCGuiReadFile(F("tab-c.txt")) == kC + "more C" && row() == ">tab-a.txt tab-c.txt";
    });
    MCGuiStep("the title has no star", [] {
        GCHECK(stripAgrees());
        GCHECK(title().find(" *") == std::string::npos);
    });

    // ---- names that clash
    MCGuiStep("two files called README.md, and b", [] {
        MCGuiOpenFileKept(F("one/README.md"));
        MCGuiOpenFileKept(F("two/README.md"));
        MCGuiOpenFileKept(F("tab-b.py"));
    });
    MCGuiWait("their tabs say which folder", 10, [] {
        return row() == "tab-a.txt one/README.md two/README.md >tab-b.py tab-c.txt";
    });
    MCGuiStep("with the whole path as the tooltip", [] {
        GCHECK_EQ_STR(strip()->item(1).tooltip, F("one/README.md"));
        GCHECK_EQ_STR(gtk_widget_get_tooltip_text(strip()->tabWidget(2)), F("two/README.md"));
    });
    MCGuiStep("one of them closed: the other is README.md again", [] {
        ed()->closeTab(ed()->document(1));
    });
    MCGuiWait("only the name", 10, [] { return row() == "tab-a.txt README.md >tab-b.py tab-c.txt"; });

    // ---- a tab's right click menu
    MCGuiWait("the tabs on screen", 10, tabsOnScreen);
    MCGuiStep("a right click on b's tab", [] { MCGuiClick(strip()->titleWidget(2), -1, -1, 3); });
    MCGuiWait("its menu", 10, [] {
        return strip()->menu() && gtk_widget_get_mapped(strip()->menu()) &&
               MCGuiFindLabel(strip()->menu(), "Close Tabs to the Right");
    });
    MCGuiStep("it offers what a tab's menu has, and b stays as it was", [] {
        GtkWidget* m = strip()->menu();
        for (const char* item : {"Close", "Close Other Tabs", "Close All Tabs", "Copy Path",
                                 "Open Containing Folder", "Reveal in File Tree"})
            GCHECK(MCGuiFindLabel(m, item) != nullptr);
        GCHECK(MCGuiFindLabel(m, "Keep Open") == nullptr);   // a kept tab
        GCHECK_EQ_STR(row(), "tab-a.txt README.md >tab-b.py tab-c.txt");
        MCGuiClick(MCGuiFindLabel(m, "Close Tabs to the Right"));
    });
    MCGuiWait("c's tab goes", 10, [] { return row() == "tab-a.txt README.md >tab-b.py"; });
    MCGuiStep("Copy Path from a's menu", [] { MCGuiClick(strip()->titleWidget(0), -1, -1, 3); });
    MCGuiWait("its menu", 10, [] {
        return strip()->menu() && gtk_widget_get_mapped(strip()->menu()) &&
               MCGuiFindLabel(strip()->menu(), "Copy Path");
    });
    MCGuiStep("chosen", [] { MCGuiClick(MCGuiFindLabel(strip()->menu(), "Copy Path")); });
    MCGuiAsync("the clipboard has a's path", 10, [](MCGuiDone done) {
        GdkClipboard* cb = gtk_widget_get_clipboard(GTK_WIDGET(MCGuiApp().window));
        auto* finished = new MCGuiDone(done);
        // Read a moment later, once the menu's action has run.
        g_timeout_add(300, [](gpointer p) -> gboolean {
            auto* fin = static_cast<MCGuiDone*>(p);
            GdkClipboard* clip = gtk_widget_get_clipboard(GTK_WIDGET(MCGuiApp().window));
            gdk_clipboard_read_text_async(clip, nullptr, [](GObject* src, GAsyncResult* res,
                                                            gpointer data) {
                auto* f = static_cast<MCGuiDone*>(data);
                char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, nullptr);
                GCHECK_EQ_STR(text ? text : "", F("tab-a.txt"));
                g_free(text);
                (*f)();
                delete f;
            }, fin);
            return G_SOURCE_REMOVE;
        }, finished);
        (void)cb;
    });
    MCGuiStep("Close Other Tabs from b's menu", [] { MCGuiClick(strip()->titleWidget(2), -1, -1, 3); });
    MCGuiWait("its menu", 10, [] {
        return strip()->menu() && gtk_widget_get_mapped(strip()->menu()) &&
               MCGuiFindLabel(strip()->menu(), "Close Other Tabs");
    });
    MCGuiStep("chosen", [] { MCGuiClick(MCGuiFindLabel(strip()->menu(), "Close Other Tabs")); });
    MCGuiWait("b is left", 10, [] { return row() == ">tab-b.py"; });

    // ---- the Markdown preview leaves the source alone (D3)
    MCGuiStep("the notes, in a tab of their own", [] { MCGuiOpenFileKept(F("tab-notes.md")); });
    MCGuiWait("rendered, from a buffer of its own", 10, [] {
        return ed()->currentPath() == F("tab-notes.md") && ed()->inPreview() &&
               ed()->buffer() != ed()->sourceBuffer() &&
               MCGuiEditorText().find("# Notes") == std::string::npos &&
               MCGuiEditorText().find("Notes") != std::string::npos;
    });
    MCGuiStep("a click in the page, then Ctrl+Shift+V, the source", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+shift+v");
    });
    MCGuiWait("the source", 10, [] {
        return !ed()->inPreview() && MCGuiEditorText() == kNotes && ed()->buffer() == ed()->sourceBuffer();
    });
    MCGuiStep("a click in it, and typing at the end", [] { MCGuiClick(ed()->textView()); });
    MCGuiWait("the editor has the keyboard", 5, editorHasKeyboard);
    MCGuiStep("types", [] {
        MCGuiKey("ctrl+end");
        MCGuiType("typed in the source");
    });
    MCGuiWait("it is there, unsaved", 10, [] {
        return MCGuiEditorText() == kNotes + "typed in the source" && ed()->dirty();
    });
    MCGuiStep("the page again", [] { MCGuiKey("ctrl+shift+v"); });
    MCGuiWait("the page shows the typing", 10, [] {
        return ed()->inPreview() &&
               MCGuiEditorText().find("typed in the source") != std::string::npos;
    });
    MCGuiStep("another tab and back, the page still on show", [] {
        MCGuiKey("ctrl+pageup");
        MCGuiKey("ctrl+pagedown");
    });
    MCGuiWait("the notes' page again", 10, [] {
        return ed()->currentPath() == F("tab-notes.md") && ed()->inPreview() &&
               MCGuiEditorText().find("typed in the source") != std::string::npos;
    });
    MCGuiStep("the source again", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+shift+v");
    });
    MCGuiWait("the source, with its typing", 10, [] {
        return !ed()->inPreview() && MCGuiEditorText() == kNotes + "typed in the source";
    });
    MCGuiStep("its caret where the typing left it", [] {
        GCHECK(caret() == g_utf8_strlen((kNotes + "typed in the source").c_str(), -1));
        MCGuiClick(ed()->textView());
    });
    MCGuiWait("the editor has the keyboard", 5, editorHasKeyboard);
    MCGuiStep("Ctrl+Z, its undo intact", [] {
        for (int i = 0; i < 6; i++) MCGuiKey("ctrl+z");
    });
    MCGuiWait("the typing goes, and the file is clean", 10, [] {
        return MCGuiEditorText() == kNotes && !ed()->dirty();
    });
    MCGuiStep("the page, for its task box", [] { MCGuiKey("ctrl+shift+v"); });
    MCGuiWait("the box on screen", 10, [] {
        return ed()->inPreview() && taskBox(ed()->textView()) != nullptr;
    });
    MCGuiStep("a click on the box", [] { MCGuiClick(taskBox(ed()->textView())); });
    MCGuiWait("ticks the task in the source, unsaved", 10, [] {
        return sourceText() == "# Notes\n\nSome text.\n\n- [x] a task\n" && ed()->dirty() &&
               ed()->inPreview();
    });
    MCGuiStep("Ctrl+Z in the page", [] {
        MCGuiClick(ed()->textView(), 40, 20);
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("is the source's undo: the task clear again, and the file clean", 10, [] {
        return sourceText() == kNotes && !ed()->dirty() && ed()->inPreview();
    });

    // ---- closing a window asks once for every unsaved tab
    MCGuiStep("a new window", [] {
        gWindows = MCGuiAppWindows().size();
        MCGuiKey("ctrl+shift+n");
    });
    MCGuiWait("it is up", 10, [] { return MCGuiAppWindows().size() == gWindows + 1; });
    // It has the folder's tabs (its session, as on the Mac); they go first.
    MCGuiStep("with no tab", [] { ed()->closeAllTabs(); });
    MCGuiWait("none left", 10, [] { return ed()->tabCount() == 0; });
    MCGuiStep("two files in it, each typed in", [] {
        MCGuiOpenFileKept(F("tab-a.txt"));
        MCGuiOpenFileKept(F("tab-c.txt"));
    });
    MCGuiWait("both on screen", 10, [] { return ed()->tabCount() == 2 && tabsOnScreen(); });
    MCGuiStep("typing in c", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+end");
        MCGuiType("x");
    });
    MCGuiWait("c is unsaved", 10, [] { return ed()->dirty(); });
    MCGuiStep("and in a", [] {
        MCGuiKey("alt+1");
        MCGuiKey("ctrl+end");
        MCGuiType("y");
    });
    MCGuiWait("both are unsaved", 10, [] { return ed()->dirtyDocuments().size() == 2; });
    MCGuiStep("Ctrl+Shift+W", [] { MCGuiKey("ctrl+shift+w"); });
    MCGuiWait("one question, for both", 10, questionUp);
    MCGuiStep("it names both files", [] {
        const std::string q = questionText();
        MCGuiNote("%s", q.c_str());
        GCHECK(q.find("Save changes to 2 files?") != std::string::npos);
        GCHECK(q.find("tab-a.txt") != std::string::npos && q.find("tab-c.txt") != std::string::npos);
        GCHECK(MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), "Save All") != nullptr);
        answer("Cancel");
    });
    MCGuiWait("the question goes", 10, [] { return MCGuiTopWindow() == MCGuiApp().window; });
    MCGuiStep("the window stays, both tabs unsaved", [] {
        GCHECK(MCGuiAppWindows().size() == gWindows + 1);
        GCHECK(ed()->dirtyDocuments().size() == 2);
        MCGuiSetKeyWindow(nullptr);
        MCGuiKey("ctrl+shift+w");
    });
    MCGuiWait("the question again", 10, questionUp);
    MCGuiStep("Don't Save", [] { answer("Don't Save"); });
    MCGuiWait("the window closes", 10, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("and the files are as they were", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK_EQ_STR(MCGuiReadFile(F("tab-a.txt")), "more A " + kA + "A typed");
        GCHECK_EQ_STR(MCGuiReadFile(F("tab-c.txt")), kC + "more C");
        // The first window kept its own tabs.
        GCHECK(ed()->currentPath() == F("tab-notes.md"));
    });
}
