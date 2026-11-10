// MultiCaretPerfSuite.cpp — `./build/minicode-guitest multicaret_perf`:
// typing at 1,000 carets spread over a 100,000-line Python file (linux/
// PARITY.md item 1.9 and D7), within a bound of the main thread's CPU time,
// the colors right afterwards. The counterpart of the Mac's MultiEditSuite's
// steps over 100,000 lines.
//
// Ctrl+Shift+L selects the 1,000 places "needle" occurs, one line in a
// hundred; five letters are typed over them through XTest, then a quote,
// which opens a string at every caret and recolors the rest of each line.
// Every key is one MultiEdit: its own cost (Carets::lastEditMs, the edit
// with every buffer signal and the highlighter's batch, EditBatch.h) and the
// main thread's CPU until it is idle again (GTK laying the lines out again,
// the lines off screen retagged in idle time) are bounded. Before the batch
// a key there took 115 to 140 ms in the edit and a second of CPU in all,
// the highlighter re-lexing and retagging every line between the first
// caret and the last. Then the lines' colors are held to a fresh lex of the
// whole text, and one Ctrl+Z takes the quote back from every line. What the
// timed waits look at costs nothing (the buffer's count of characters):
// reading the text back costs as much as a key (3.5 MB a poll).
#include "CaretSuites.h"

#include "Document.h"
#include "Palette.h"
#include "SyntaxHighlighter.h"

#include <algorithm>

using namespace caretsuite;

namespace {

std::string F(const std::string& name) { return MCGuiProjectFile(name); }

const int kLines = 100000;
const char kTyped[] = "abcde";

double gCpu0 = 0, gEditMax = 0, gEditSum = 0;
int gChars = 0;
gboolean gBlink = TRUE;

std::string bigText() {
    std::string s;
    s.reserve((size_t)kLines * 36);
    for (int i = 0; i < kLines; i++)
        s += i % 100 == 0 ? "    total = needle(i) + 'x'  # hit\n" : "    value = compute(i) + 1  # note\n";
    return s;
}

// Lines whose every character carries the highlight tag of its token's
// style in a full lex of the text, and no other (none outside tokens).
bool colorsRight(const std::vector<int>& lines, bool say) {
    const std::string t = text();
    const std::vector<Token> tokens = SyntaxHighlighter::highlight(t, "py");
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buf());
    std::vector<GtkTextTag*> tags;
    for (int i = 0; i < kTokenStyleCount; i++)
        tags.push_back(gtk_text_tag_table_lookup(table, TagNameForStyle((TokenStyle)i)));
    for (int line : lines) {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(buf(), &it, line);
        // Bytes are characters: the text is ASCII.
        const size_t start = (size_t)gtk_text_iter_get_offset(&it);
        size_t end = t.find('\n', start);
        if (end == std::string::npos) end = t.size();
        auto tok = std::lower_bound(tokens.begin(), tokens.end(), start,
                                    [](const Token& a, size_t at) { return a.start + a.length <= at; });
        for (size_t p = start; p < end; p++) {
            while (tok != tokens.end() && tok->start + tok->length <= p) ++tok;
            const int want = tok != tokens.end() && tok->start <= p ? (int)tok->style : -1;
            GtkTextIter at;
            gtk_text_buffer_get_iter_at_offset(buf(), &at, (int)p);
            for (int s = 0; s < kTokenStyleCount; s++) {
                const bool has = tags[(size_t)s] && gtk_text_iter_has_tag(&at, tags[(size_t)s]);
                if (has != (s == want)) {
                    if (say)
                        MCGuiNote("line %d, column %zu: style %d tagged %d, a full lex says %d", line,
                                  p - start, s, has, want);
                    return false;
                }
            }
        }
    }
    return true;
}

std::vector<int> sampled() {
    std::vector<int> out;
    for (int i = 0; i < kLines; i += 2500) out.push_back(i);   // every 25th caret's line
    out.push_back(1);
    out.push_back(99900);
    out.push_back(kLines - 1);
    return out;
}

size_t count(const std::string& t, const std::string& what) {
    size_t n = 0;
    for (size_t at = t.find(what); at != std::string::npos; at = t.find(what, at + 1)) n++;
    return n;
}

// The lines off screen are retagged in idle time, and a look at the colors
// reads the whole text back and lexes it (well over 50 ms here): polled
// every 50 ms it starved the retag it waited for, which at load 28 had
// reached line 22,500 in 30 s. So the main thread goes quiet first.
void waitForColors(const std::string& what) {
    MCGuiWaitQuiet(what + ": the main thread idle", 0.5, 120,
                   [] { return (long)(MCGuiThreadCpuMs() / 10); });
    MCGuiWait(what, 30, [] { return colorsRight(sampled(), false); });
    MCGuiStep(what + " (the first line that differs, if one does)",
              [] { GCHECK(colorsRight(sampled(), true)); });
}

}  // namespace

MC_GUI_SUITE(multicaret_perf) {
    MCGuiStep("a Python file of 100,000 lines, needle on one in a hundred", [] {
        // While the caret blinks GTK draws the view every frame for
        // seconds after any key (Shift too), about 100 ms of CPU a second at
        // 100,000 lines, whether there are carets or not; when the measure
        // caught it, a key cost 500 ms more. Off for the measure.
        g_object_get(gtk_settings_get_default(), "gtk-cursor-blink", &gBlink, nullptr);
        g_object_set(gtk_settings_get_default(), "gtk-cursor-blink", FALSE, nullptr);
        ed()->closeAllTabs();
        MCGuiWriteFile(F("carets-big.py"), bigText());
        MCGuiOpenFileKept(F("carets-big.py"));
    });
    MCGuiWait("open", 30, [] {
        return ed()->currentPath() == F("carets-big.py") && !ed()->active()->loading();
    });
    MCGuiWait("laid out", 10, [] { return gtk_widget_get_height(view()) > 100; });
    // Opening highlights in idle slices; the typing is measured from when
    // the main thread is idle again.
    MCGuiWaitQuiet("the main thread idle", 0.5, 60, [] { return (long)(MCGuiThreadCpuMs() / 20); });
    MCGuiStep("a click in the first needle; Ctrl+Shift+L", [] {
        clickAt(14);
        MCGuiKey("ctrl+shift+l");
    });
    MCGuiWait("1,000 selections", 20, [] { return carets()->current().size() == 1000; });
    MCGuiWaitQuiet("idle", 0.5, 30, [] { return (long)(MCGuiThreadCpuMs() / 10); });

    // Five letters, a key at a time, each its edit timed: "needle" becomes
    // "a", then a letter more at each caret.
    MCGuiStep("the count of characters", [] { gChars = gtk_text_buffer_get_char_count(buf()); });
    for (int k = 0; kTyped[k]; k++) {
        MCGuiStep(std::string("typing ") + kTyped[k], [k] {
            if (k == 0) {
                gCpu0 = MCGuiThreadCpuMs();
                gEditMax = gEditSum = 0;
            }
            MCGuiType(std::string(1, kTyped[k]));
        });
        MCGuiWait(std::string("in at every caret: ") + kTyped[k], 30, [k] {
            return gtk_text_buffer_get_char_count(buf()) == gChars - 5000 + 1000 * k;
        });
        MCGuiStep("its edit's cost", [] {
            gEditMax = std::max(gEditMax, carets()->lastEditMs());
            gEditSum += carets()->lastEditMs();
        });
    }
    MCGuiWaitQuiet("the main thread idle again", 0.5, 60,
                   [] { return (long)(MCGuiThreadCpuMs() / 10); });
    MCGuiStep("within bounds of CPU time", [] {
        const int keys = (int)sizeof(kTyped) - 1;
        const double total = MCGuiThreadCpuMs() - gCpu0;
        MCGuiNote("%d keys at 1,000 carets in 100,000 lines: the edits %.1f ms a key (%.1f at most), "
                  "%.1f ms a key of the main thread in all", keys, gEditSum / keys, gEditMax,
                  total / keys);
        // See PARITY.md's D7 for what the ThinkPad measures (35 to 55 ms a
        // key in the edit, 100 to 200 in all). Before EditBatch: 115 to 225
        // in the edit, 1,000 to 1,200 in all. The bounds leave room for a
        // slower runner and catch the second.
        GCHECK_TIME(gEditMax, 150, "the slowest key's edit at 1,000 carets");
        GCHECK_TIME(total / keys, 400, "a key at 1,000 carets, the main thread in all");
    });
    MCGuiStep("the text as typed", [] {
        const std::string t = text();
        GCHECK(count(t, "= abcde(i)") == 1000);
        GCHECK(t.find("needle") == std::string::npos);
    });
    waitForColors("the lines' colors as a fresh lex");

    MCGuiStep("a quote at every caret", [] {
        gChars = gtk_text_buffer_get_char_count(buf());
        gCpu0 = MCGuiThreadCpuMs();
        MCGuiType("\"");
    });
    MCGuiWait("in at every caret", 30,
              [] { return gtk_text_buffer_get_char_count(buf()) == gChars + 1000; });
    MCGuiWaitQuiet("idle", 0.5, 60, [] { return (long)(MCGuiThreadCpuMs() / 10); });
    MCGuiStep("a string opened on each line, within a bound", [] {
        const double total = MCGuiThreadCpuMs() - gCpu0;
        MCGuiNote("a quote at 1,000 carets: the edit %.1f ms, %.1f ms of the main thread in all",
                  carets()->lastEditMs(), total);
        GCHECK_TIME(carets()->lastEditMs(), 150, "a quote's edit at 1,000 carets");
        GCHECK_TIME(total, 600, "a quote at 1,000 carets, the main thread in all");
        GCHECK(count(text(), "abcde\"(i)") == 1000);
    });
    waitForColors("the lines' colors as a fresh lex, the strings to the lines' ends");
    MCGuiStep("one Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("takes the quote back from every line", 60,
              [] { return gtk_text_buffer_get_char_count(buf()) == gChars; });
    MCGuiStep("the text without it", [] {
        const std::string t = text();
        GCHECK(t.find("abcde\"(i)") == std::string::npos);
        GCHECK(count(t, "abcde(i)") == 1000);
    });
    waitForColors("the colors as a fresh lex again");
    MCGuiStep("the tab closes", [] {
        MCGuiKey("escape");
        ed()->closeAllTabs();
        g_object_set(gtk_settings_get_default(), "gtk-cursor-blink", gBlink, nullptr);
    });
}
