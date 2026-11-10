// LargeFileSuite.cpp — `./build/minicode-guitest large_file`: big files and
// long lines (linux/PARITY.md item 1.12, LargeFileView.h), the counterpart of
// the Mac's tests/gui/LargeFileSuite.mm.
//
// A 25 MB Python file opens in large-file mode (no colors, no server, the
// status bar's note), read in the background and put in a piece at a time,
// so the main loop is never held long, and typing in it stays cheap; leaving
// its tab and coming back stalls nothing and keeps its place; its unsaved
// text gets a backup, and Reopen Closed Tab brings it back with its
// selection. A 300,001-line file counts as large too, and a real click on
// the note turns everything on for it, a server included; the setting turned
// off opens it as any file. A file over 50 MB asks Open Anyway or Cancel,
// and past VS Code's 50 MB sync limit no server gets it even with the mode
// off. A long line is shown 10,000 characters at a time, read at once or in
// the background: opening a 5 MB one and typing in it never hold the main
// loop, the part shown follows the caret to the line's end and to a find
// bar match, the lines around it stay colored, and Select All and Copy
// take all of it.
//
// The longest stall is the main thread's CPU between two fires of a timer
// asking for every 5 ms (R1's tree_stall suite does the same), and the open's
// cost is the main thread's CPU from the open to the file being in.
#include "Harness.h"

#include "BufferLines.h"
#include "CodeView.h"
#include "Document.h"
#include "Editor.h"
#include "FakeLsp.h"
#include "FindBar.h"
#include "Gutter.h"
#include "LargeFileView.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
Document* doc() { return ed()->active(); }
GtkTextView* tv() { return GTK_TEXT_VIEW(ed()->textView()); }
GtkTextBuffer* buf() { return ed()->buffer(); }

std::string dir() { return MCGuiScratch() + "/large-files"; }
std::string F(const std::string& name) { return dir() + "/" + name; }

// ---- the longest stall, as tree_stall times it
struct StallMeter {
    guint id = 0;
    double lastWall = 0, lastCpu = 0;
    double worstCpu = 0, worstWall = 0;
    static gboolean tick(gpointer p) {
        static_cast<StallMeter*>(p)->sample();
        return G_SOURCE_CONTINUE;
    }
    void sample() {
        const double w = MCGuiNowMs(), c = MCGuiThreadCpuMs();
        if (c - lastCpu > worstCpu) {
            worstCpu = c - lastCpu;
            worstWall = w - lastWall;
        }
        lastWall = w;
        lastCpu = c;
    }
    void start() {
        if (!id) id = g_timeout_add_full(G_PRIORITY_HIGH, 5, tick, this, nullptr);
        reset();
    }
    void stop() {
        if (id) g_source_remove(id);
        id = 0;
    }
    void reset() {
        lastWall = MCGuiNowMs();
        lastCpu = MCGuiThreadCpuMs();
        worstCpu = worstWall = 0;
    }
};
StallMeter gMeter;

// Far below GNOME's five seconds and the seconds the old code took (6.7 s of
// CPU for the 5 MB line's first layout, 3 to 9 s a key in it), and well
// above what each phase needs here (tens of milliseconds), for a loaded
// machine.
constexpr double kStallBoundMs = 250;

void checkStall(const std::string& what) {
    gMeter.sample();
    MCGuiNote("%s: longest main-thread stall %.0f ms of CPU (%.0f ms long)", what.c_str(),
              gMeter.worstCpu, gMeter.worstWall);
    GCHECK_TIME(gMeter.worstCpu, kStallBoundMs, "the longest stall " + what);
    gMeter.reset();
}

// Any tag but the long lines' hidden one toggles in [from, to).
bool tagsIn(int from, int to) {
    GtkTextBuffer* b = buf();
    LongLines* lines = LongLines::of(b);
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(b, &it, from);
    for (int guard = 0; guard < 100000; guard++) {
        GSList* tags = gtk_text_iter_get_toggled_tags(&it, TRUE);
        GSList* off = gtk_text_iter_get_toggled_tags(&it, FALSE);
        bool other = false;
        for (GSList* l = tags; l; l = l->next)
            if (!lines || l->data != lines->hiddenTag()) other = true;
        for (GSList* l = off; l; l = l->next)
            if (!lines || l->data != lines->hiddenTag()) other = true;
        g_slist_free(tags);
        g_slist_free(off);
        if (other) return true;
        if (!gtk_text_iter_forward_to_tag_toggle(&it, nullptr) || gtk_text_iter_get_offset(&it) >= to)
            return false;
    }
    return true;
}

bool hasTag(int offset, const char* name) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, offset);
    GtkTextTag* t = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buf()), name);
    return t && gtk_text_iter_has_tag(&it, t);
}

// The `n` characters at `offset`, read from the buffer by iterators only.
std::string textAt(int offset, int n) {
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(buf(), &a, offset);
    gtk_text_buffer_get_iter_at_offset(buf(), &b, offset + n);
    char* t = gtk_text_buffer_get_text(buf(), &a, &b, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

int caret() {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(buf(), &it, gtk_text_buffer_get_insert(buf()));
    return gtk_text_iter_get_offset(&it);
}

// The caret is inside the view's visible rect.
bool caretOnScreen() {
    GdkRectangle vis, at;
    gtk_text_view_get_visible_rect(tv(), &vis);
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(buf(), &it, gtk_text_buffer_get_insert(buf()));
    gtk_text_view_get_iter_location(tv(), &it, &at);
    return at.y >= vis.y && at.y + at.height <= vis.y + vis.height && at.x >= vis.x &&
           at.x <= vis.x + vis.width;
}

int topLine() {
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(tv(), &vis);
    GtkTextIter it;
    gtk_text_view_get_line_at_y(tv(), &it, vis.y + 1, nullptr);
    return gtk_text_iter_get_line(&it);
}

// The caret on screen among the lines around it: GTK gives lines it has not
// laid out yet no height, so a caret can be inside the visible rect with
// line 450 at its top, and drift off as those lines get their heights.
bool caretShown() {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(buf(), &it, gtk_text_buffer_get_insert(buf()));
    const int below = gtk_text_iter_get_line(&it) - topLine();
    return caretOnScreen() && below >= 0 && below < 60;
}

bool loaded(const std::string& path) {
    return doc() && doc()->path() == path && !doc()->loading() && ed()->isSource();
}

// A question up, laid out (no window manager places it: it opens at the
// window's corner, and a click before its layout lands elsewhere), as the
// tabs suite waits for one.
bool questionUp() {
    GtkWindow* top = MCGuiTopWindow();
    if (top == MCGuiApp().window) return false;
    GtkWidget* b = MCGuiFindButton(GTK_WIDGET(top), "Open Anyway");
    return b && gtk_widget_get_width(b) > 0 && gtk_widget_get_height(b) > 0;
}

// Every label in the window on top, joined.
std::string questionText() {
    std::string all;
    std::vector<GtkWidget*> todo = {GTK_WIDGET(MCGuiTopWindow())};
    while (!todo.empty()) {
        GtkWidget* w = todo.back();
        todo.pop_back();
        if (GTK_IS_LABEL(w)) all += std::string(gtk_label_get_text(GTK_LABEL(w))) + "\n";
        for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
            todo.push_back(c);
    }
    return all;
}

GtkWidget* note() { return ed()->largeFileNote(); }
bool noteShown() { return gtk_widget_get_visible(note()) && gtk_widget_get_mapped(note()); }

// Characters typed one at a time as the keyboard types them (one user
// action each, at the caret), each one's main-thread CPU in ms.
std::vector<double> typeEach(const std::string& text) {
    std::vector<double> times;
    for (char c : text) {
        const double c0 = MCGuiThreadCpuMs();
        gtk_text_buffer_begin_user_action(buf());
        gtk_text_buffer_insert_interactive_at_cursor(buf(), &c, 1, TRUE);
        gtk_text_buffer_end_user_action(buf());
        // What the next frame does for it: the caret's line laid out again.
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_mark(buf(), &it, gtk_text_buffer_get_insert(buf()));
        GdkRectangle r;
        gtk_text_view_get_iter_location(tv(), &it, &r);
        times.push_back(MCGuiThreadCpuMs() - c0);
    }
    return times;
}

void bounds(std::vector<double> v, const std::string& what, double medianBound, double p95Bound) {
    std::sort(v.begin(), v.end());
    const double med = v.empty() ? 0 : v[v.size() / 2];
    const double p95 = v.empty() ? 0 : v[v.size() * 95 / 100];
    MCGuiNote("%s: %zu keystrokes, median %.2f ms of CPU, 95th percentile %.2f ms", what.c_str(),
              v.size(), med, p95);
    GCHECK_TIME(med, medianBound, what + ": the median");
    GCHECK_TIME(p95, p95Bound, what + ": the 95th percentile");
}

// What the open costs at the least on this machine, on this thread: the
// file read and put in a plain buffer in one go.
double baselineMs(const std::string& path) {
    const double c0 = MCGuiThreadCpuMs();
    std::string s;
    Document::readText(path, s);
    GtkTextBuffer* b = gtk_text_buffer_new(nullptr);
    gtk_text_buffer_set_text(b, s.data(), (int)s.size());
    g_object_unref(b);
    return MCGuiThreadCpuMs() - c0;
}

double gBase = 0, gOpen0 = 0, gLoadedAt = 0, gWaitFrom = 0;
int gTypedAt = 0, gTopBefore = 0, gCaretBefore = 0;
double gKeysCpu0 = 0;
size_t gTypedKeys = 0;
const char* kServer = "large";
int gLine1 = 0, gNeedle = 0, gTail = 0;   // the 5 MB line's file: where things are
std::string gClip;   // what the clipboard held

}  // namespace

MC_GUI_SUITE(large_file) {
    // ------------------------------------------------ a 25 MB file
    MCGuiStep("a 25 MB Python file opens in the background, in large-file mode", [] {
        MCGuiSetSettings("lsp.enabled = false\n");
        ed()->closeAllTabs();
        std::string s;
        s.reserve(26u << 20);
        for (int i = 0; s.size() < (25u << 20); i++)
            s += "value_" + std::to_string(i) + " = [1, 2.5, 'three', None]  # note (" +
                 std::to_string(i) + ")\n";
        MCGuiWriteFile(F("big25.py"), s);
        gBase = baselineMs(F("big25.py"));
        gMeter.start();
        gOpen0 = MCGuiThreadCpuMs();
        MCGuiOpenFileKept(F("big25.py"));
        const double call = MCGuiThreadCpuMs() - gOpen0;
        MCGuiNote("the open itself: %.1f ms of CPU (the read and the rest go on meanwhile)", call);
        GCHECK(doc() && doc()->loading());
        GCHECK(!ed()->isSource());   // "Opening…" on show, nothing to type into
        GCHECK(MCGuiEditorText().find("Opening") != std::string::npos);
        GCHECK(!doc()->canSave());
        gLoadedAt = 0;
        doc()->whenLoaded([] { gLoadedAt = MCGuiThreadCpuMs(); });
    });
    MCGuiWait("it is in", 60, [] { return gLoadedAt > 0 && loaded(F("big25.py")); });
    MCGuiStep("it took little of the main thread, never much at once", [] {
        const double open = gLoadedAt - gOpen0;
        MCGuiNote("opening 25 MB: %.0f ms of main-thread CPU in all, the baseline (read and "
                  "put in a buffer at once) %.0f ms", open, gBase);
        // The pieces cost what one set_text does, a little more: 0.4 to 0.6
        // s of CPU in all on the ThinkPad, against a baseline of 0.45 s. The
        // bound leaves room for a loaded machine, as the Mac's does.
        GCHECK_TIME(open, std::max(1500.0, 3 * gBase), "a 25 MB file opens");
        checkStall("opening 25 MB");   // 20 to 50 ms; 0.6 s held at once before
        Document* d = doc();
        GCHECK(d->isLargeFile() && d->largeFileFeaturesOff());
        GCHECK(!d->languageServerMayRead());
        GCHECK(d->canSave() && !d->dirty());
        GCHECK(gtk_text_buffer_get_char_count(buf()) > (int)(25u << 20) - 100);
        BufferLines* lines = BufferLines::of(buf());
        GCHECK(lines && lines->lineCount() > 400000);
        // Plain: no highlight tag over the first lines, numbers, strings and
        // comments included.
        GCHECK(!tagsIn(0, 4000));
        GCHECK(gtk_text_view_get_editable(tv()));
    });
    MCGuiWait("the note in the status bar, the gutter numbering six digits", 10, [] {
        Gutter* g = ed()->codeView()->gutter();
        return noteShown() && g->shown() && g->digits() == 6 && !g->drawnRows().empty();
    });
    MCGuiStep("the note says what and why", [] {
        GCHECK_EQ_STR(gtk_label_get_text(GTK_LABEL(note())), "Large file: features off");
        const char* tip = gtk_widget_get_tooltip_text(note());
        GCHECK(tip && std::string(tip).find("big25.py") != std::string::npos);
        GCHECK(tip && std::string(tip).find("300,000 lines") != std::string::npos);
    });

    MCGuiStep("the caret to line 300,000", [] {
        gWaitFrom = MCGuiNowMs();
        GCHECK(ed()->revealLine(300000, 6, 0));
    });
    // GTK lays the lines out for 10 to 20 s after the file comes on show,
    // and until then the view cannot be put far down for good (see
    // settleOnCaret): revealLine keeps at it while the height grows.
    MCGuiWait("on screen, and staying there", 180, [] {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_mark(buf(), &it, gtk_text_buffer_get_insert(buf()));
        return gtk_text_iter_get_line(&it) == 299999 && caretShown();
    });
    MCGuiStep("typing in it stays cheap", [] {
        MCGuiNote("line 300,000 on screen to stay %.1f s after Go to Line", (MCGuiNowMs() - gWaitFrom) / 1000);
        gTypedAt = caret();
        std::string text;
        while (text.size() < 200) text += "ab cd ";
        text.resize(200);
        const std::vector<double> times = typeEach(text);
        // Measured on the ThinkPad: a median of 0.1 to 0.3 ms, the 95th
        // percentile under 1 ms (the Mac's suite: about 5 ms a key, bound at
        // 20 and 60).
        bounds(times, "a 25 MB file in large-file mode", 20, 60);
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buf(), &a, gTypedAt);
        gtk_text_buffer_get_iter_at_offset(buf(), &b, gTypedAt + 6);
        char* got = gtk_text_buffer_get_text(buf(), &a, &b, TRUE);
        GCHECK_EQ_STR(got, "ab cd ");
        g_free(got);
        GCHECK(!tagsIn(gTypedAt - 100, gTypedAt + 300));   // still plain
        GCHECK(doc()->dirty());
        gMeter.reset();
        gKeysCpu0 = MCGuiThreadCpuMs();
        gtk_widget_grab_focus(ed()->textView());
        // And from the keyboard, Return and Tab included (AutoIndent).
        MCGuiType("xy z\n\tq");
        gTypedKeys = 7;
    });
    MCGuiWait("the keys arrive", 20, [] {
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buf(), &a, gTypedAt + 200);
        gtk_text_buffer_get_iter_at_offset(buf(), &b, gTypedAt + 200 + 4);
        char* got = gtk_text_buffer_get_text(buf(), &a, &b, TRUE);
        const bool ok = std::string(got) == "xy z";
        g_free(got);
        return ok && caret() >= gTypedAt + 200 + 7;
    });
    MCGuiStep("", [] {
        MCGuiNote("%zu keys from the keyboard: %.0f ms of main-thread CPU", gTypedKeys,
                  MCGuiThreadCpuMs() - gKeysCpu0);
        checkStall("typing in 25 MB, Return and Tab included");
    });
    // Its unsaved text gets a backup a moment after typing stops, as any tab's.
    MCGuiWait("a backup of it", 15, [] { return doc()->backup().written; });

    // ------------------------------------------------ switching
    MCGuiStep("switching away from it and back stalls nothing", [] {
        Document* big = doc();
        gTopBefore = topLine();
        gCaretBefore = caret();
        GCHECK(gTopBefore > 290000);
        MCGuiWriteFile(F("beside.py"), "x = 1\n");
        gMeter.reset();
        double c0 = MCGuiThreadCpuMs();
        MCGuiOpenFileKept(F("beside.py"));   // a new tab
        const double toNew = MCGuiThreadCpuMs() - c0;
        GCHECK(doc() != big && MCGuiEditorText() == "x = 1\n");
        c0 = MCGuiThreadCpuMs();
        ed()->activate(big);
        const double back = MCGuiThreadCpuMs() - c0;
        GCHECK(doc() == big && big->dirty());
        c0 = MCGuiThreadCpuMs();
        MCGuiOpenFileKept(F("beside.py"));   // a tab shown before
        const double again = MCGuiThreadCpuMs() - c0;
        c0 = MCGuiThreadCpuMs();
        ed()->activate(big);
        const double back2 = MCGuiThreadCpuMs() - c0;
        MCGuiNote("from the 25 MB tab to a new tab: %.0f ms of CPU, back %.0f, away again %.0f, "
                  "back again %.0f", toNew, back, again, back2);
        // 20 to 60 ms each on the ThinkPad. The language server session took
        // its squiggles off the whole buffer at every switch, 100 to 130 ms
        // of it.
        GCHECK_TIME(toNew, kStallBoundMs, "switching from the 25 MB tab to a new tab");
        GCHECK_TIME(back, kStallBoundMs, "switching back to the 25 MB tab");
        GCHECK_TIME(again, kStallBoundMs, "switching from the 25 MB tab to a tab shown before");
        GCHECK_TIME(back2, kStallBoundMs, "switching back to the 25 MB tab again");
        GCHECK(caret() == gCaretBefore);
        GCHECK(gtk_text_buffer_get_can_undo(buf()));
        gWaitFrom = MCGuiNowMs();
    });
    // GTK lays the 470,000 lines out for 10 to 20 s after the tab comes back
    // (see settleOnPlace): the place is right once the height stops growing.
    MCGuiWait("its place as it was", 180, [] {
        return std::abs(topLine() - gTopBefore) <= 1 && caretShown();
    });
    MCGuiStep("", [] {
        MCGuiNote("the place back %.1f s after the tab", (MCGuiNowMs() - gWaitFrom) / 1000);
        MCGuiNote("the top line %d before, %d shown again; the caret on screen: %d", gTopBefore,
                  topLine(), (int)caretOnScreen());
        checkStall("switching tabs");
    });

    // ------------------------------------------------ servers
    MCGuiStep("a server for Python files", [] {
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp(kServer, "{}") + "\n");
        MCGuiOpenFileKept(F("beside.py"));
    });
    MCGuiWait("it has the small file", 20, [] {
        return MCGuiFakeLspReceived(kServer, "textDocument/didOpen", "beside.py") > 0;
    });
    MCGuiStep("the 25 MB file shown, and typed in", [] {
        MCGuiOpenFileKept(F("big25.py"));
        GCHECK(loaded(F("big25.py")));
        gtk_widget_grab_focus(ed()->textView());
        MCGuiType("w");
    });
    MCGuiPause(1.5);   // a negative check: a didOpen or didChange would have gone by now
    MCGuiStep("no server ever had it", [] {
        GCHECK(MCGuiFakeLspReceived(kServer, "textDocument/didOpen", "big25.py") == 0);
        GCHECK(MCGuiFakeLspReceived(kServer, "textDocument/didChange", "big25.py") == 0);
        GCHECK(noteShown());
    });

    // ------------------------------------------------ 300,001 lines, and the note
    MCGuiStep("300,001 short lines count as large", [] {
        std::string s = "def f(x):\n    return (x)\n";
        for (int i = 2; i < 300001; i++) s += "x = 1\n";
        MCGuiWriteFile(F("lines300k.py"), s);   // 300,001 lines, the last one empty
        MCGuiOpenFileKept(F("lines300k.py"));
    });
    MCGuiWait("open, the note on show", 20, [] { return loaded(F("lines300k.py")) && noteShown(); });
    MCGuiStep("plain, and a click on the note", [] {
        GCHECK(doc()->isLargeFile() && doc()->largeFileFeaturesOff());
        GCHECK(!hasTag(0, "kw"));
        MCGuiClick(note());
    });
    MCGuiWait("everything on for it", 10, [] { return !doc()->largeFileFeaturesOff(); });
    MCGuiStep("colors, a server, no note", [] {
        GCHECK(doc()->isLargeFile());
        GCHECK(hasTag(0, "kw"));   // "def"
        GCHECK(doc()->languageServerMayRead());
    });
    MCGuiWait("the server has it, and the note is gone", 20, [] {
        return MCGuiFakeLspReceived(kServer, "textDocument/didOpen", "lines300k.py") > 0 &&
               !gtk_widget_get_visible(note());
    });
    MCGuiStep("editor.large-file-optimizations = off opens it as any file", [] {
        ed()->closeTab(doc());
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp(kServer, "{}") +
                         "\neditor.large-file-optimizations = off\n");
        MCGuiOpenFileKept(F("lines300k.py"));
    });
    MCGuiWait("open", 20, [] { return loaded(F("lines300k.py")); });
    MCGuiStep("colored, no note", [] {
        GCHECK(doc()->isLargeFile() && !doc()->largeFileFeaturesOff());
        GCHECK(hasTag(0, "kw"));
        GCHECK(!gtk_widget_get_visible(note()));
        ed()->closeTab(doc());
    });

    // ------------------------------------------------ over 50 MB
    static size_t asked = 0;
    MCGuiStep("a file over 50 MB asks before it is read", [] {
        // 60 MB in six lines of 10 MB: few lines for GTK to lay out.
        std::string s;
        s.reserve(61u << 20);
        while (s.size() < (60u << 20)) s += "x = '" + std::string(10000000, 'h') + "'\n";
        MCGuiWriteFile(F("huge60.py"), s);
        asked = ed()->largeFileUi()->questionsAsked();
        MCGuiOpenFileKept(F("huge60.py"));
        GCHECK(ed()->largeFileUi()->questionsAsked() == asked + 1);
        GCHECK(ed()->documentFor(F("huge60.py")) == nullptr);   // nothing read
    });
    MCGuiWait("the question is up", 10, questionUp);
    MCGuiStep("it names the file and its size; Cancel", [] {
        const std::string text = questionText();
        MCGuiNote("asked: %s", text.c_str());
        GCHECK(text.find("“huge60.py” is ") != std::string::npos);
        GCHECK(text.find("MB. Open it anyway?") != std::string::npos);   // "63.0\u00A0MB"
        GtkWidget* cancel = MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), "Cancel");
        GCHECK(cancel != nullptr);
        MCGuiSetKeyWindow(MCGuiTopWindow());
        MCGuiClick(cancel);
    });
    MCGuiWait("the question goes, nothing opens", 10, [] {
        return MCGuiTopWindow() == MCGuiApp().window;
    });
    MCGuiStep("asked again: Open Anyway", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK(ed()->documentFor(F("huge60.py")) == nullptr);
        MCGuiOpenFileKept(F("huge60.py"));
        GCHECK(ed()->largeFileUi()->questionsAsked() == asked + 2);
    });
    MCGuiWait("the question is up", 10, questionUp);
    MCGuiStep("Open Anyway", [] {
        MCGuiSetKeyWindow(MCGuiTopWindow());
        gMeter.reset();
        MCGuiClick(MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), "Open Anyway"));
    });
    MCGuiWait("it opens", 60, [] { return loaded(F("huge60.py")); });
    MCGuiStep("not large-file mode (the setting is off), but past the sync limit", [] {
        MCGuiSetKeyWindow(nullptr);
        checkStall("opening 60 MB");
        GCHECK(doc()->isLargeFile() && !doc()->largeFileFeaturesOff());
        GCHECK(!doc()->languageServerMayRead());
        // Its long lines are shown in part.
        LongLines::Part p;
        GCHECK(LongLines::of(buf())->partOf(0, &p) && p.start == 0 && p.end == 10000 &&
               p.length == 10000006);
        // A file with a tab is not asked about again.
        MCGuiOpenFileKept(F("beside.py"));
        MCGuiOpenFileKept(F("huge60.py"));
        GCHECK(ed()->largeFileUi()->questionsAsked() == asked + 2);
        GCHECK(loaded(F("huge60.py")));
    });
    MCGuiPause(1.0);   // a negative check: a didOpen would have gone by now
    MCGuiStep("no server has it", [] {
        GCHECK(MCGuiFakeLspReceived(kServer, "textDocument/didOpen", "huge60.py") == 0);
        ed()->closeTab(doc());
        unlink(F("huge60.py").c_str());
        MCGuiSetSettings("lsp.enabled = false\n");
    });

    // ------------------------------------------------ Reopen Closed Tab
    MCGuiStep("the 25 MB tab closed, and reopened", [] {
        Document* big = ed()->documentFor(F("big25.py"));
        GCHECK(big != nullptr);
        ed()->activate(big);
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buf(), &a, gTypedAt);
        gtk_text_buffer_get_iter_at_offset(buf(), &b, gTypedAt + 5);
        gtk_text_buffer_select_range(buf(), &a, &b);
        gCaretBefore = gTypedAt;
        ed()->closeTab(big);   // unsaved: the shell would ask; the check does not
        GCHECK(ed()->documentFor(F("big25.py")) == nullptr);
        GCHECK(ed()->reopenClosedTab());
        GCHECK(doc() && doc()->path() == F("big25.py") && doc()->loading());
    });
    MCGuiWait("it is in again", 60, [] { return loaded(F("big25.py")); });
    MCGuiStep("with its selection, as read from the disk", [] {
        GtkTextIter a, b;
        gtk_text_buffer_get_selection_bounds(buf(), &a, &b);
        GCHECK(gtk_text_iter_get_offset(&a) == gCaretBefore);
        GCHECK(gtk_text_iter_get_offset(&b) == gCaretBefore + 5);
        GCHECK(!doc()->dirty());
        ed()->closeAllTabs();
    });

    // ------------------------------------------------ a long line read at once
    // Under 4 MB a file is read and put in at once, as before: its long
    // line is hidden past its part in the same edit, before GTK lays it out.
    MCGuiStep("a 300,000-character line in a small file", [] {
        MCGuiWriteFile(F("one-line.json"), "[" + std::string(300000, '1') + "]\n");
        gMeter.reset();
        MCGuiOpenFileKept(F("one-line.json"));
        GCHECK(loaded(F("one-line.json")));   // no reading in the background
        LongLines::Part p;
        GCHECK(LongLines::of(buf())->partOf(0, &p) && p.start == 0 && p.end == 10000 &&
               p.length == 300002);
    });
    MCGuiWaitQuiet("drawn", 0.5, 10, [] { return (long)ed()->largeFileUi()->notesDrawn(); });
    MCGuiStep("", [] {
        checkStall("opening a file with a 300,000-character line");
        GCHECK(MCGuiEditorText() == MCGuiReadFile(F("one-line.json")));
        gClip.clear();
        gtk_widget_grab_focus(ed()->textView());
        MCGuiKey("ctrl+a");
        MCGuiKey("ctrl+c");
    });
    // The hidden part is copied too: GTK's own copy leaves invisible text
    // out of the text it hands over (10,001 characters of this line).
    MCGuiWait("something copied", 10, [] {
        GdkContentProvider* p = gdk_clipboard_get_content(gtk_widget_get_clipboard(ed()->textView()));
        if (!p) return false;
        GValue v = G_VALUE_INIT;
        g_value_init(&v, G_TYPE_STRING);
        if (gdk_content_provider_get_value(p, &v, nullptr) && g_value_get_string(&v))
            gClip = g_value_get_string(&v);
        g_value_unset(&v);
        return !gClip.empty();
    });
    MCGuiStep("all of it copied", [] {
        MCGuiNote("copied %zu bytes of %zu", gClip.size(), MCGuiReadFile(F("one-line.json")).size());
        GCHECK(gClip == MCGuiReadFile(F("one-line.json")));
        ed()->closeAllTabs();
    });

    // ------------------------------------------------ a 5 MB line
    // Offsets are worked out from the text written, never searched for in
    // the buffer: a search through the line on every poll of a wait held
    // the main thread itself, 100 ms at a time.
    MCGuiStep("a 5,000,000-character line", [] {
        const std::string first = "function f(a) { return a + 1; } // top\n";
        std::string line = "var m = [";
        while (line.size() < 4000000) line += "\"x\",1,{\"k\":true},";
        gNeedle = (int)(first.size() + line.size() + 1);   // past the quote
        line += "\"NEEDLE\",";
        while (line.size() < 5000000) line += "\"x\",1,{\"k\":true},";
        line += "0];\n";
        gLine1 = (int)first.size();
        gTail = (int)(first.size() + line.size());
        MCGuiWriteFile(F("minified.js"), first + line + "const tail = 'end'; // bottom\n");
        gMeter.reset();
        MCGuiOpenFileKept(F("minified.js"));
    });
    MCGuiWait("it is in", 30, [] { return loaded(F("minified.js")); });
    MCGuiWaitQuiet("its first screen drawn", 0.5, 20,
                   [] { return (long)ed()->largeFileUi()->notesDrawn(); });
    MCGuiStep("shown 10,000 characters at a time, its neighbours colored", [] {
        checkStall("opening a 5 MB line");   // 30 to 60 ms; 4 to 7 s before
        GCHECK(!doc()->isLargeFile());       // 5 MB, three lines: an ordinary file
        GCHECK(textAt(gNeedle, 6) == "NEEDLE" && textAt(gTail, 10) == "const tail");
        LongLines::Part p;
        GCHECK(LongLines::of(buf())->partOf(1, &p));
        GCHECK(p.start == 0 && p.end == 10000 && p.length > 5000000);
        GCHECK(!LongLines::of(buf())->partOf(0, &p));
        GCHECK(ed()->largeFileUi()->notesDrawn() > 0);   // "… 4,990,000 more characters"
        GCHECK(hasTag(0, "kw"));          // "function"
        GCHECK(hasTag(gTail, "kw"));      // "const"
        GCHECK(!tagsIn(gLine1, gTail - 1));   // the long line: plain
        // The tag is never in the text: a copy of the buffer is all of it.
        GCHECK(MCGuiEditorText() == MCGuiReadFile(F("minified.js")));
    });

    // The End key takes the caret to the line's end, in its hidden part.
    MCGuiStep("End on the long line", [] {
        gtk_widget_grab_focus(ed()->textView());
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_offset(buf(), &it, gLine1 + 5);
        gtk_text_buffer_place_cursor(buf(), &it);
        gMeter.reset();
        MCGuiKey("End");
    });
    MCGuiWait("the caret at its end, the part shown its last", 10, [] {
        LongLines::Part p;
        return caret() == gTail - 1 && LongLines::of(buf())->partOf(1, &p) &&
               p.end == p.length && p.start == p.length - 10000;
    });
    MCGuiWait("on screen", 10, [] { return caretOnScreen(); });
    MCGuiStep("", [] { checkStall("going to the end of a 5 MB line"); });

    // The find bar's match in the hidden part is shown. The bar searches on
    // a worker of its own (FindBar.h), so its search is not timed here.
    MCGuiStep("Ctrl+F", [] { MCGuiKey("ctrl+f"); });
    MCGuiWait("the find field laid out", 10, [] {
        GtkWidget* e = MCGuiApp().findBar->findField();
        return MCGuiApp().findBar->shown() && gtk_widget_get_mapped(e) &&
               gtk_widget_get_height(e) > 10;
    });
    // A click in the field: after Ctrl+F the field has the focus here, but
    // keys typed without a window manager went nowhere until it was clicked.
    // The query the window's bar still holds from an earlier suite ("needle"
    // after find_bar) selected, so that typing replaces it.
    MCGuiStep("", [] {
        MCGuiClick(MCGuiApp().findBar->findField());
        MCGuiKey("ctrl+a");
    });
    MCGuiStep("NEEDLE", [] {
        GtkTextIter start;
        gtk_text_buffer_get_start_iter(buf(), &start);
        gtk_text_buffer_place_cursor(buf(), &start);   // the search runs from here
        MCGuiType("NEEDLE");
    });
    MCGuiWait("the match selected", 30, [] {
        GtkTextIter a, b;
        return gtk_text_buffer_get_selection_bounds(buf(), &a, &b) &&
               gtk_text_iter_get_offset(&a) == gNeedle && gtk_text_iter_get_offset(&b) == gNeedle + 6;
    });
    MCGuiWait("the part shown holds it, on screen", 10, [] {
        LongLines::Part p;
        return LongLines::of(buf())->partOf(1, &p) && (int)p.start + gLine1 <= gNeedle &&
               gNeedle + 6 <= (int)p.end + gLine1 && caretOnScreen();
    });
    MCGuiStep("", [] {
        LongLines::Part p;
        LongLines::of(buf())->partOf(1, &p);
        MCGuiNote("the match at %d of the line, the part shown %zu to %zu", gNeedle - gLine1,
                  p.start, p.end);
        MCGuiKey("Escape");
    });
    MCGuiStep("the caret in the match", [] {
        gtk_widget_grab_focus(ed()->textView());
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_offset(buf(), &it, gNeedle + 3);
        gtk_text_buffer_place_cursor(buf(), &it);
    });
    MCGuiStep("typing there", [] {
        const std::vector<double> times = typeEach("abcdefghijklmnopqrst");
        // 0.1 to 0.2 ms each in the editor's handlers and 2.5 ms for GTK to
        // lay out the 10,000 characters shown: the tags are left as they are
        // while the part grows (20 ms a key when they were set again on
        // every keystroke, 3 to 9 s with the whole line shown).
        bounds(times, "a keystroke in a 5,000,000-character line", 20, 60);
        GCHECK(textAt(gNeedle, 26) == "NEEabcdefghijklmnopqrstDLE");
        LongLines::Part p;
        GCHECK(LongLines::of(buf())->partOf(1, &p));
        GCHECK((int)p.start + gLine1 <= caret() && caret() <= (int)p.end + gLine1);
        // Characters put in this way are no keys: the view does not scroll
        // to the caret for them as it does for typing.
        gtk_text_view_scroll_mark_onscreen(tv(), gtk_text_buffer_get_insert(buf()));
    });
    MCGuiWait("the caret on screen", 10, [] { return caretOnScreen(); });
    MCGuiStep("and from the keyboard", [] {
        gMeter.reset();
        gKeysCpu0 = MCGuiThreadCpuMs();
        MCGuiType("uvwxyz");
    });
    MCGuiWait("the keys arrive", 20, [] { return textAt(gNeedle, 32) == "NEEabcdefghijklmnopqrstuvwxyzDLE"; });
    MCGuiWaitQuiet("drawn", 0.3, 10, [] { return (long)ed()->largeFileUi()->notesDrawn(); });
    MCGuiStep("never held the main loop", [] {
        MCGuiNote("6 keys from the keyboard in the 5 MB line: %.0f ms of main-thread CPU",
                  MCGuiThreadCpuMs() - gKeysCpu0);
        checkStall("typing in a 5 MB line");   // 5 to 15 ms; 3 to 9 s a key before
        GCHECK(caretOnScreen());
        gMeter.stop();
        ed()->closeAllTabs();
    });
}
