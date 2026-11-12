// MathSuite.cpp — typeset math in the Markdown preview
// (`./build/minicode-guitest markdown_math`, linux/PARITY.md item 4.3, second
// part, HANDOFF.md). tectonic is a stand-in script ($MINICODE_TECTONIC): it
// reads the batch's .tex, takes the run's nonce and each formula's number
// from it, writes the log lines the core's MathTex::readLog reads (a formula
// holding FAILME fails with TeX's complaint), and copies a PDF this suite made
// with cairo (one all-black page per good formula, every page 32 by 15 pt).
// No real tectonic and no download. Checked: the TeX and the "Typesetting"
// line while the run is out, pictures after it in the text's color, inline
// ones inline and display ones centered on a line of their own, a failed
// formula keeping its TeX with TeX's complaint as the tooltip while the
// others typeset, one run for a page's formulas with the arguments the Mac
// uses, a reopen answered by the cache, the side pane, a page with no math
// and markdown.math = false running nothing, and the offer to download
// tectonic when there is none.
#include "Harness.h"

#include "AppSeam.h"
#include "Document.h"
#include "Editor.h"
#include "Markdown.h"
#include "MarkdownSide.h"
#include "Math.h"

#ifdef MINICODE_ENABLE_PDF
#include <cairo-pdf.h>
#include <glib/gstdio.h>
#include <cmath>
#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string P(const std::string& n) { return MCGuiProjectFile(n); }
std::string Log() { return MCGuiReadFile(P("tectonic.log")); }

int lines(const std::string& s) {
    int n = 0;
    for (char c : s) n += c == '\n';
    return n;
}

std::string pageText() {
    GtkTextBuffer* b = ed()->active()->page();
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* raw = gtk_text_buffer_get_text(b, &s, &e, TRUE);
    std::string t = raw;
    g_free(raw);
    return t;
}

// The page with a U+FFFC where a widget is anchored (get_text leaves them out).
std::string sliceText() {
    GtkTextBuffer* b = ed()->active()->page();
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* raw = gtk_text_buffer_get_slice(b, &s, &e, TRUE);
    std::string t = raw;
    g_free(raw);
    return t;
}

int offsetOf(const std::string& t, const std::string& needle) {
    const size_t at = t.find(needle);
    return at == std::string::npos ? -1 : (int)g_utf8_pointer_to_offset(t.c_str(), t.c_str() + at);
}

void openPage(const char* file) {
    MCGuiStep(std::string("open ") + file, [=] { MCGuiOpenFileKept(P(file)); });
    MCGuiWait("the page", 20, [=] {
        return ed()->active() && ed()->active()->path() == P(file) && ed()->inPreview();
    });
}

size_t embeds() { return ed()->active()->pageInfo().embeds.size(); }

const char* kObject = "\xEF\xBF\xBC";   // where a widget is anchored

}  // namespace

MC_GUI_SUITE(markdown_math) {
    MCGuiStep("a stand-in tectonic, PDFs of its own, a cache of its own", [] {
        MCGuiSetSettings("lsp.enabled = false\ntheme = dark-plus\n");
        const double w = 32 * 72.0 / 72.27, h = 15 * 72.0 / 72.27;
        for (int n = 1; n <= 4; n++) {
            cairo_surface_t* pdf = cairo_pdf_surface_create(P("pdf-" + std::to_string(n) + ".pdf").c_str(), w, h);
            cairo_t* cr = cairo_create(pdf);
            for (int k = 0; k < n; k++) {
                cairo_set_source_rgb(cr, 0, 0, 0);
                cairo_rectangle(cr, 0, 0, w, h);
                cairo_fill(cr);
                cairo_show_page(cr);
            }
            cairo_destroy(cr);
            cairo_surface_destroy(pdf);
        }
        const std::string script =
            "#!/bin/sh\n"
            "echo \"$@\" >> '" + P("tectonic.log") + "'\n"
            "while [ $# -gt 0 ]; do [ \"$1\" = --outdir ] && out=\"$2\"; tex=\"$1\"; shift; done\n"
            "sleep 0.8\n"
            "mark=$(grep -o 'MC:[0-9a-f]*:' \"$tex\" | head -1)\n" +
            R"AWK(awk -v mark="$mark" -v lg="$out/math.log" '
 /BEGIN:[0-9]+/ { match($0, /BEGIN:[0-9]+/); k = substr($0, RSTART + 6, RLENGTH - 6); print mark "BEGIN:" k >> lg; bad = 0; next }
 /FAILME/ { bad = 1 }
 /\\mcship\{[0-9]+\}/ { match($0, /\{[0-9]+\}/); k = substr($0, RSTART + 1, RLENGTH - 2)
   if (bad) { print "! Undefined control sequence." >> lg; print "l.9 \\failme" >> lg }
   else { good++; print mark k ":" good ":30pt:10pt:3pt" >> lg } }
 END { print good + 0 > (lg ".good") }' "$tex"
)AWK"
            "n=$(cat \"$out/math.log.good\"); rm -f \"$out/math.log.good\"\n"
            "[ \"$n\" -gt 0 ] && cp '" + P("pdf-") + "'\"$n\".pdf \"$out/math.pdf\"\n"
            "cat \"$out/math.log\"\n"
            // How many formulas each run was given, so a check can tell a
            // cache hit from a batch that typeset everything again.
            "grep -o '\\\\mcship{[0-9]*}' \"$tex\" | wc -l | tr -d ' ' >> '" + P("batches.log") + "'\n";
        MCGuiWriteFile(P("fake-tectonic"), script);
        g_chmod(P("fake-tectonic").c_str(), 0755);
        g_setenv("MINICODE_TECTONIC", P("fake-tectonic").c_str(), TRUE);
        g_setenv("MINICODE_MATH_CACHE", P("math-cache").c_str(), TRUE);
        MCGuiWriteFile(P("tectonic.log"), "");
        MCGuiWriteFile(P("batches.log"), "");
        MCGuiWriteFile(P("plain.md"), "# Plain\n\nNo formula here, only a price of 5 and 10 dollars.\n");
        MCGuiWriteFile(P("math.md"),
                       "# Math\n\nInline $a+b$ and then\n\n$$\nx^2\n$$\n\nAnd $\\failme FAILME$ bad.\n");
        MCGuiWriteFile(P("off.md"), "# Off\n\nSome $c+d$ here.\n");
        MCGuiWriteFile(P("side.md"), "# Side\n\nIn the pane $q^2$ stands.\n");
        MCGuiWriteFile(P("none.md"), "# None\n\nMissing $e+f$ tool.\n");
    });

    openPage("plain.md");
    MCGuiStep("a page with no math runs nothing", [] {
        GCHECK_EQ_STR(std::to_string(Math::runs()), "0");
        GCHECK_EQ_STR(ed()->mathNote(), "");
        GCHECK_EQ_STR(std::to_string(lines(Log())), "0");
    });

    openPage("math.md");
    MCGuiStep("TeX first, the line says what is out", [] {
        GCHECK(pageText().find("a+b") != std::string::npos);
        GCHECK_EQ_STR(std::to_string(ed()->active()->pageInfo().mathWaiting), "3");
        GCHECK_EQ_STR(ed()->mathNote(), "Typesetting 3 formulas…");
    });
    MCGuiWait("the pictures", 30, [] { return embeds() == 2 && ed()->active()->pageInfo().waiting == 0; });
    MCGuiStep("pictures for the good ones, TeX and a tooltip for the bad one, one run", [] {
        const std::string t = pageText();
        GCHECK(t.find("a+b") == std::string::npos);
        GCHECK(t.find("x^2") == std::string::npos);
        GCHECK(t.find("FAILME") != std::string::npos);
        const std::string sl = sliceText();
        GCHECK(sl.find(std::string("Inline ") + kObject + " and then") != std::string::npos);
        GCHECK(sl.find(std::string("\n") + kObject + "\n") != std::string::npos);   // display: a line of its own
        const int at = offsetOf(t, "FAILME");
        GCHECK(at >= 0);
        const std::string tip = Markdown::tooltipAt(ed()->active()->page(), at);
        GCHECK(tip.find("Undefined control sequence") != std::string::npos);
        GCHECK_EQ_STR(Markdown::tooltipAt(ed()->active()->page(), offsetOf(t, "And ")), "");
        GCHECK_EQ_STR(ed()->mathNote(), "");
        // 32 by 15 pt pages, TeX points to PDF points to 1.4 pixels each; the
        // depth is 3 pt plus the pad.
        const Markdown::Embed& e = ed()->active()->pageInfo().embeds[0];
        GCHECK_EQ_STR(std::to_string(e.width), "45");
        GCHECK_EQ_STR(std::to_string(e.height), "21");
        GCHECK_EQ_STR(std::to_string(e.depth), "6");
        MCGuiSnapshot(GTK_WIDGET(MCGuiTopWindow()), "math-page");
        GCHECK_EQ_STR(std::to_string(Math::runs()), "1");
        const std::string log = Log();
        GCHECK_EQ_STR(std::to_string(lines(log)), "1");
        GCHECK(log.find("-Z continue-on-errors") != std::string::npos);
        GCHECK(log.find("--untrusted") != std::string::npos);
        GCHECK(log.find("--keep-logs") != std::string::npos);
        GCHECK(log.find("--reruns 0") != std::string::npos);
        GCHECK(ed()->active()->page() != nullptr);
    });
    MCGuiStep("the ink takes the text's color", [] {
        const Math::State st = Math::want("a+b", false, ed()->mathColor());
        GCHECK(st.kind == Math::State::Ready);
        if (st.kind != Math::State::Ready) return;
        const int w = gdk_texture_get_width(st.texture), h = gdk_texture_get_height(st.texture);
        const int stride = w * 4;
        std::vector<guchar> px((size_t)stride * (size_t)h);
        gdk_texture_download(st.texture, px.data(), (gsize)stride);
        const guchar* c = &px[(size_t)(h / 2) * (size_t)stride + (size_t)(w / 2) * 4];
        GdkRGBA want;
        gdk_rgba_parse(&want, ed()->mathColor().c_str());
        GCHECK_EQ_STR(std::to_string((int)c[2]), std::to_string((int)std::lround(want.red * 255)));
        GCHECK_EQ_STR(std::to_string((int)c[1]), std::to_string((int)std::lround(want.green * 255)));
        GCHECK_EQ_STR(std::to_string((int)c[0]), std::to_string((int)std::lround(want.blue * 255)));
        GCHECK_EQ_STR(std::to_string((int)c[3]), "255");
        ed()->closeAllTabs();
        Math::forgetMemory();   // the disk cache stays
    });

    openPage("math.md");
    MCGuiStep("a reopen is answered by the cache", [] {
        GCHECK_EQ_STR(std::to_string(embeds()), "2");
        // The failed formula was not cached: it asks again 50 ms after the
        // page shows, so on a loaded machine its run (0.8 s in the stand-in)
        // may have started before this step; the batch sizes below are what
        // tell the cache's answer from a run that typeset everything again.
        GCHECK(Math::runs() <= 2);
        if (Math::runs() == 1) {
            GCHECK_EQ_STR(ed()->mathNote(), "Typesetting 1 formula…");
            GCHECK_EQ_STR(std::to_string(ed()->active()->pageInfo().mathWaiting), "1");
        }
    });
    MCGuiWait("the failure asked again and settled", 30, [] { return Math::runs() == 2 && ed()->active()->pageInfo().waiting == 0; });
    MCGuiStep("the second run was given the failed formula alone", [] {
        const std::string b = MCGuiReadFile(P("batches.log"));
        MCGuiNote("formulas per run: %s", b.c_str());
        GCHECK_EQ_STR(std::to_string(lines(b)), "2");
        GCHECK(b.size() >= 2 && b.substr(b.size() - 2) == "1\n");
        GCHECK_EQ_STR(std::to_string(lines(Log())), "2");
    });
    MCGuiStep("markdown.math = false", [] {
        ed()->closeAllTabs();
        MCGuiSetSettings("lsp.enabled = false\ntheme = dark-plus\nmarkdown.math = false\n");
    });
    openPage("off.md");
    MCGuiStep("runs nothing and shows the TeX", [] {
        MCGuiPause(1.2);   // time is the condition: a run would have started by now
        GCHECK_EQ_STR(std::to_string(Math::runs()), "2");
        GCHECK(pageText().find("c+d") != std::string::npos);
        GCHECK_EQ_STR(std::to_string(embeds()), "0");
        GCHECK_EQ_STR(ed()->mathNote(), "");
        ed()->closeAllTabs();
        MCGuiSetSettings("lsp.enabled = false\ntheme = dark-plus\n");
    });

    openPage("side.md");
    MCGuiStep("source, then the pane", [] {
        if (ed()->textView()) gtk_widget_grab_focus(ed()->textView());
        MCGuiKey("ctrl+shift+v");
    });
    MCGuiWait("source", 20, [] { return ed()->active() && ed()->isSource(); });
    MCGuiStep("pane", [] { MCGuiKey("ctrl+alt+v"); });
    MCGuiWait("the pane built", 30, [] {
        return ed()->showsSide() && ed()->sidePane() && ed()->sidePane()->idle() &&
               ed()->sidePane()->stats().renders > 0;
    });
    MCGuiStep("TeX while the run is out", [] {
        GCHECK(ed()->sidePane()->pageText().find("q^2") != std::string::npos ||
               ed()->sidePane()->embedCount() == 1);
    });
    MCGuiWait("the picture in the pane", 30, [] {
        return ed()->sidePane()->idle() && ed()->sidePane()->embedCount() == 1 &&
               ed()->sidePane()->pageText().find("q^2") == std::string::npos;
    });
    MCGuiStep("one run for it", [] {
        GCHECK_EQ_STR(std::to_string(Math::runs()), "3");
        GCHECK(ed()->sidePane()->pageText().find("stands.") != std::string::npos);
        MCGuiKey("ctrl+alt+v");
        ed()->closeAllTabs();
        g_setenv("MINICODE_TECTONIC", P("no-such-tectonic").c_str(), TRUE);
    });

    openPage("none.md");
    MCGuiStep("without tectonic: the TeX and the offer to download", [] {
        GCHECK(pageText().find("e+f") != std::string::npos);
        GCHECK_EQ_STR(std::to_string(Math::runs()), "3");
        const std::string note = ed()->mathNote();
        GCHECK(note.find("tectonic is not installed") != std::string::npos);
        GCHECK(note.find("Download tectonic") != std::string::npos);
        ed()->closeAllTabs();
    });
}
#endif
