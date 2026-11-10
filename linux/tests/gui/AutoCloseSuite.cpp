// AutoCloseSuite.cpp — `./build/minicode-guitest auto_close`: brackets and
// quotes closing as they are typed (editor.auto-close, linux/PARITY.md item
// 1.8, AutoCloseInput.h), the counterpart of the Mac's AutoCloseSuite, with
// every key real (XTest through the view's own key handling): print( gives
// print() with the caret inside, Ctrl+Z takes the pair back in one step and
// Ctrl+Shift+Z puts it back, ) types over the closer, Backspace takes the
// pair, a pair typed by hand or let go stays; the closers are let go when
// the caret leaves the pair or the pair spans lines; quotes by Python's
// rules (a word's apostrophe, f', the docstring's third quote); a selection
// wrapped; Markdown closing brackets and not quotes, and wrapping in * and
// _; nothing while a dead key composes, and a quote an input method commits
// types as it is; the setting off types plainly but still wraps; the typed
// character goes into the buffer last with the caret after it (what the
// language server's trigger check reads); and the CPU of a ( typed in a
// 100,000-line file, against the setting off.
#include "Harness.h"

#include "AutoCloseInput.h"
#include "Editor.h"

#include <string>
#include <utility>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
AutoCloseInput* ac() { return ed()->autoClose(); }
GtkTextBuffer* buf() { return ed()->buffer(); }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

const char* kBase = "lsp.enabled = false\neditor.bracket-pair-colorization = off\n";

std::string text() {
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buf(), &a, &b);
    char* t = gtk_text_buffer_get_text(buf(), &a, &b, TRUE);
    std::string s = t ? t : "";
    g_free(t);
    return s;
}

// The last line of the text with "|" where the caret is, or a selection
// between "«" and "»" whichever way it was made.
std::string lastLine() {
    GtkTextIter a, b;
    gtk_text_buffer_get_selection_bounds(buf(), &a, &b);
    const std::string s = text();
    const char* base = s.c_str();
    const size_t lo = (size_t)(g_utf8_offset_to_pointer(base, gtk_text_iter_get_offset(&a)) - base);
    const size_t hi = (size_t)(g_utf8_offset_to_pointer(base, gtk_text_iter_get_offset(&b)) - base);
    std::string marked = s;
    if (hi != lo) {
        marked.insert(hi, "»");
        marked.insert(lo, "«");
    } else {
        marked.insert(lo, "|");
    }
    const size_t nl = marked.rfind('\n');
    return nl == std::string::npos ? marked : marked.substr(nl + 1);
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// Every insertion into the buffer on show, with where the caret was once it
// was in: what Lsp.cpp's textEdited sees.
std::vector<std::pair<std::string, bool>> gInserts;   // text, caret just after it
gulong gInsertHandler = 0;
GtkTextBuffer* gWatched = nullptr;
void watchInserts() {
    gInserts.clear();
    if (gWatched == buf()) return;
    if (gWatched) g_signal_handler_disconnect(gWatched, gInsertHandler);
    gWatched = buf();
    gInsertHandler = g_signal_connect_after(
        gWatched, "insert-text",
        G_CALLBACK(+[](GtkTextBuffer* b, GtkTextIter* at, char* t, int len, gpointer) {
            GtkTextIter caret;
            gtk_text_buffer_get_iter_at_mark(b, &caret, gtk_text_buffer_get_insert(b));
            gInserts.push_back({std::string(t, (size_t)len),
                                gtk_text_iter_get_offset(&caret) == gtk_text_iter_get_offset(at)});
        }),
        nullptr);
}
void unwatchInserts() {
    if (gWatched) g_signal_handler_disconnect(gWatched, gInsertHandler);
    gWatched = nullptr;
}

double gCpu0 = 0, gKeyMs[2] = {0, 0};
size_t gBigChars = 0;
int gFrames = 0;
guint gTick = 0;
void countFrames() {
    gFrames = 0;
    if (gTick) return;
    gTick = gtk_widget_add_tick_callback(
        ed()->textView(),
        [](GtkWidget*, GdkFrameClock*, gpointer) -> gboolean {
            gFrames++;
            return G_SOURCE_CONTINUE;
        },
        nullptr, nullptr);
}
bool framesDrawn() {
    if (gFrames < 2) return false;
    if (gTick) gtk_widget_remove_tick_callback(ed()->textView(), gTick);
    gTick = 0;
    return true;
}

}  // namespace

MC_GUI_SUITE(auto_close) {
    MCGuiStep("a Python file, the caret on a new last line", [] {
        ed()->closeAllTabs();
        MCGuiSetSettings(kBase);
        MCGuiWriteFile(F("auto_close.py"), "def f():\n    pass\n");
        MCGuiWriteFile(F("auto_close.md"), "");   // empty: opens in the source
        MCGuiOpenFileKept(F("auto_close.py"));
        gtk_widget_grab_focus(ed()->textView());
        MCGuiKey("ctrl+end");
    });
    MCGuiWait("there", 5, [] { return lastLine() == "|"; });

    MCGuiStep("print( typed", [] {
        watchInserts();
        MCGuiType("print(");
    });
    MCGuiWait("print() with the caret inside", 5, [] { return lastLine() == "print(|)"; });
    MCGuiStep("one keystroke remembered; ( went in last, the caret after it", [] {
        GCHECK(ac()->rememberedActions() == 1);
        GCHECK(gInserts.size() >= 2);
        if (gInserts.size() >= 2) {
            GCHECK_EQ_STR(gInserts[gInserts.size() - 2].first, ")");
            GCHECK_EQ_STR(gInserts.back().first, "(");
            GCHECK(gInserts.back().second);
        }
        unwatchInserts();
    });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("the pair taken back in one step", 5, [] { return endsWith(text(), "\nprint"); });
    MCGuiStep("Ctrl+Shift+Z", [] { MCGuiKey("ctrl+shift+z"); });
    MCGuiWait("both back", 5, [] { return endsWith(text(), "\nprint()"); });

    MCGuiStep(") types over the closer", [] {
        // Undo and redo let the closer go (the caret left the pair), as in
        // VS Code: a new pair, then its closer typed.
        MCGuiKey("ctrl+end");
        MCGuiType("\nprint(");
    });
    MCGuiWait("closed", 5, [] { return lastLine() == "print(|)"; });
    MCGuiStep("type )", [] { MCGuiType(")"); });
    MCGuiWait("over the closer: no second )", 5, [] {
        return lastLine() == "print()|" && endsWith(text(), "\nprint()");
    });
    MCGuiStep("and let go", [] { GCHECK(ac()->rememberedActions() == 0); });

    MCGuiStep("Backspace between an empty pair it put in", [] { MCGuiType("\nlen("); });
    MCGuiWait("closed", 5, [] { return lastLine() == "len(|)"; });
    MCGuiStep("Backspace", [] { MCGuiKey("backspace"); });
    MCGuiWait("deletes both", 5, [] { return lastLine() == "len|"; });
    MCGuiStep("a pair typed by hand, the caret back between it", [] {
        MCGuiType("()");
        MCGuiKey("left");
    });
    MCGuiWait("between", 5, [] { return lastLine() == "len(|)"; });
    MCGuiStep("Backspace", [] { MCGuiKey("backspace"); });
    MCGuiWait("only its opener goes", 5, [] { return lastLine() == "len|)"; });

    MCGuiStep("the caret leaving a pair lets it go", [] {
        MCGuiKey("end");
        MCGuiType("\nfoo(");
    });
    MCGuiWait("closed", 5, [] { return lastLine() == "foo(|)" && ac()->rememberedActions() == 1; });
    MCGuiStep("left, out of the pair, and right, back in", [] {
        MCGuiKey("left");
        MCGuiKey("right");
    });
    MCGuiWait("let go", 5, [] { return lastLine() == "foo(|)" && ac()->rememberedActions() == 0; });
    MCGuiStep("type )", [] { MCGuiType(")"); });
    MCGuiWait("a ) of its own, as in VS Code", 5, [] { return lastLine() == "foo()|)"; });
    MCGuiStep("a pair spanning lines lets go too", [] {
        MCGuiKey("end");
        MCGuiType("\nbar(");
    });
    MCGuiWait("closed", 5, [] { return lastLine() == "bar(|)" && ac()->rememberedActions() == 1; });
    MCGuiStep("Return inside it", [] { MCGuiType("\n"); });
    MCGuiWait("the closer on a line of its own, let go", 5, [] {
        return endsWith(text(), "bar(\n    \n)") && ac()->rememberedActions() == 0;
    });

    MCGuiStep("quotes by Python's rules: a string", [] {
        MCGuiKey("ctrl+end");
        MCGuiType("\nx = \"hi\"");
    });
    MCGuiWait("its closing quote typed over", 5, [] { return lastLine() == "x = \"hi\"|"; });
    MCGuiStep("an apostrophe in a word", [] { MCGuiType("\n# don't"); });
    MCGuiWait("types as it is", 5, [] { return lastLine() == "# don't|"; });
    MCGuiStep("f'", [] { MCGuiType("\ny = f'"); });
    MCGuiWait("closes", 5, [] { return lastLine() == "y = f'|'"; });
    MCGuiStep("a docstring's three quotes", [] {
        MCGuiKey("end");
        MCGuiType("\n\"\"\"");
    });
    MCGuiWait("the third closes the triple quote", 5, [] {
        return lastLine() == "\"\"\"|\"\"\"";
    });
    MCGuiStep("a word, selected", [] {
        MCGuiKey("end");
        MCGuiType("\nword");
        MCGuiKey("shift+home");
    });
    MCGuiWait("selected", 5, [] { return lastLine() == "«word»"; });
    MCGuiStep("[ over it", [] { MCGuiType("["); });
    MCGuiWait("wrapped, still selected", 5, [] { return lastLine() == "[«word»]"; });
    // The selection must stay: emptied for a moment it once went a moment
    // later (AutoCloseInput::apply), which a check made at once would miss.
    MCGuiPause(0.5);
    MCGuiStep("and stays so", [] { GCHECK_EQ_STR(lastLine(), "[«word»]"); });

    // ---- input methods
    MCGuiStep("a dead key composing, then (", [] {
        MCGuiKey("end");
        MCGuiType("\nz = ");
        MCGuiKey("dead_acute");
    });
    MCGuiWait("composing", 5, [] { return ed()->composing(); });
    MCGuiStep("(", [] { MCGuiType("("); });
    // A negative check: the input method had the key, and its time.
    MCGuiPause(0.4);
    MCGuiStep("the input method had it: nothing closed", [] {
        const std::string l = lastLine();
        MCGuiNote("after the dead key and (: %s (%s)", l.c_str(),
                  ed()->composing() ? "still composing" : "composed");
        GCHECK(l.rfind("z = ", 0) == 0);
        GCHECK(l.find(')') == std::string::npos);
        GCHECK(ac()->rememberedActions() == 0);
        // GTK's simple input method keeps the dead key after a key it
        // cannot compose with (Pango warns of a cursor index as it does,
        // with auto-closing or without); Escape ends it.
        if (ed()->composing()) MCGuiKey("escape");
    });
    MCGuiWait("nothing composing", 5, [] { return !ed()->composing(); });
    MCGuiStep("a quote the input method commits (dead acute, space)", [] {
        MCGuiType("\nq = ");
        MCGuiKey("dead_acute");
        MCGuiKey("space");
    });
    MCGuiWait("types as it is, unpaired", 5, [] {
        const std::string l = lastLine();
        return !ed()->composing() && (l == "q = '|" || l == "q = \xC2\xB4|");
    });
    MCGuiStep("what it committed", [] { MCGuiNote("after the dead key and space: %s", lastLine().c_str()); });

    // ---- Markdown
    MCGuiStep("Markdown", [] {
        MCGuiOpenFileKept(F("auto_close.md"));
        gtk_widget_grab_focus(ed()->textView());
        MCGuiKey("ctrl+end");
    });
    MCGuiWait("its source", 5, [] { return ed()->currentPath() == F("auto_close.md") && ed()->isSource(); });
    MCGuiStep("see (", [] { MCGuiType("see ("); });
    MCGuiWait("closes brackets", 5, [] { return lastLine() == "see (|)"; });
    MCGuiStep("a quote", [] {
        MCGuiKey("end");
        MCGuiType(" it's \"");
    });
    MCGuiWait("not quotes", 5, [] { return lastLine() == "see () it's \"|"; });
    MCGuiStep("bold, selected", [] {
        MCGuiType("\nbold");
        MCGuiKey("shift+home");
    });
    MCGuiWait("selected", 5, [] { return lastLine() == "«bold»"; });
    MCGuiStep("* twice", [] { MCGuiType("**"); });
    MCGuiWait("wrapped twice, still selected", 5, [] { return lastLine() == "**«bold»**"; });
    MCGuiStep("_ over it", [] { MCGuiType("_"); });
    MCGuiWait("wrapped in _", 5, [] { return lastLine() == "**_«bold»_**"; });

    // ---- off
    MCGuiStep("editor.auto-close = off, back in Python", [] {
        MCGuiSetSettings(std::string(kBase) + "editor.auto-close = off\n");
        MCGuiOpenFileKept(F("auto_close.py"));
        gtk_widget_grab_focus(ed()->textView());
        MCGuiKey("ctrl+end");
    });
    MCGuiWait("there", 5, [] { return ed()->currentPath() == F("auto_close.py"); });
    MCGuiStep("q(", [] { MCGuiType("\nq("); });
    MCGuiWait("types plainly", 5, [] { return lastLine() == "q(|"; });
    MCGuiStep("over a selection, on a line of its own", [] {
        MCGuiType(")\nw");   // the ( closed by hand, so the next line is not indented in it
        MCGuiKey("shift+home");
        MCGuiType("(");
    });
    MCGuiWait("still wraps", 5, [] { return lastLine() == "(«w»)"; });

    // ---- what it costs
    MCGuiStep("100,000 lines, the caret at line 50,000", [] {
        MCGuiSetSettings(kBase);
        std::string big;
        for (int k = 0; k < 100000; k++)
            big += k % 7 == 0 ? "def f(a, b):  # (\n" : "    x = g(h[1], {'k': (2)})\n";
        gBigChars = big.size();
        MCGuiWriteFile(F("big_close.py"), big);
        MCGuiOpenFileKept(F("big_close.py"));
        gtk_widget_grab_focus(ed()->textView());
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(buf(), &it, 50000);
        gtk_text_buffer_place_cursor(buf(), &it);
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(ed()->textView()),
                                     gtk_text_buffer_get_insert(buf()), 0.0, TRUE, 0.0, 0.3);
    });
    // With and without, twice each in turn, and the sums compared: measured
    // once each a second apart, a load that changed between them made the
    // plain typing 1.9 ms a key against its usual 2.5 to 4, and the bound
    // from it failed.
    for (int round = 0; round < 4; round++) {
        const int on = round % 2 == 0;
        MCGuiStep(on ? "( and a space, five times" : "again with the setting off", [on] {
            MCGuiSetSettings(std::string(kBase) + (on ? "" : "editor.auto-close = off\n"));
            gCpu0 = MCGuiThreadCpuMs();
            MCGuiType("( ( ( ( ( ");
        });
        MCGuiWait("they arrive", 10, [on, round] {
            // Closed: 15 characters; plainly: 10.
            const size_t want = gBigChars + 15 * (size_t)((round + 2) / 2) + 10 * (size_t)((round + 1) / 2);
            const bool there = (size_t)gtk_text_buffer_get_char_count(buf()) == want;
            if (there) gKeyMs[on] += MCGuiThreadCpuMs() - gCpu0;
            return there;
        });
    }
    MCGuiStep("about the CPU of typing them plainly", [] {
        MCGuiNote("a ( in 100,000 lines: %.2f ms of main-thread CPU a key with auto-closing, "
                  "%.2f without",
                  gKeyMs[1] / 20, gKeyMs[0] / 20);
        GCHECK_TIME(gKeyMs[1], 1.5 * gKeyMs[0] + 5 * 20, "typing ( with auto-closing");
        countFrames();
    });
    MCGuiWait("the view has drawn it", 5, framesDrawn);
    MCGuiStep("put back", [] {
        ed()->closeAllTabs();
        MCGuiSetSettings("lsp.enabled = false\n");
    });
}
