// MarkdownTablesSuite.cpp — what opening a table-heavy page costs the main
// thread (`./build/minicode-guitest markdown_tables`, linux/PARITY.md item
// 4.2, R1): a copy of the repo's README.md in the full preview and in the
// side pane, the longest stall (main-thread CPU between two fires of a 5 ms
// timer) and the total CPU of the open.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "MarkdownSide.h"

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& n) { return MCGuiProjectFile(n); }
void key(const char* chord) {
    if (ed()->textView()) gtk_widget_grab_focus(ed()->textView());
    MCGuiKey(chord);
}

struct Meter {
    guint id = 0;
    double lastCpu = 0, worst = 0, start = 0;
    static gboolean tick(gpointer p) {
        auto* m = static_cast<Meter*>(p);
        const double c = MCGuiThreadCpuMs();
        m->worst = std::max(m->worst, c - m->lastCpu);
        m->lastCpu = c;
        return G_SOURCE_CONTINUE;
    }
    void begin() {
        lastCpu = start = MCGuiThreadCpuMs();
        worst = 0;
        if (!id) id = g_timeout_add_full(G_PRIORITY_HIGH, 5, tick, this, nullptr);
    }
    double total() { return MCGuiThreadCpuMs() - start; }
    void end() {
        if (id) g_source_remove(id);
        id = 0;
    }
} gMeter;

// The work was 439 to 617 ms of CPU before (300 to 800 ms in R1's runs),
// 82 to 99 ms after on a quiet machine, and 180 to 208 ms in a full run
// with a build beside it: a bound well under the old cost with room for a
// loaded machine or CI's.
constexpr double kStallBoundMs = 300;

bool settled() {
    return ed()->showsSide() && ed()->sidePane() && ed()->sidePane()->idle() &&
           ed()->sidePane()->stats().renders > 0;
}

}  // namespace

MC_GUI_SUITE(markdown_tables) {
    MCGuiStep("README.md copied in", [] {
        ed()->closeAllTabs();
        // The tables are what is measured: README's $x^2$ would start a typeset
        // (and count against markdown_math's runs).
        MCGuiSetSettings("lsp.enabled = false\ntheme = dark-plus\nmarkdown.math = false\n");
        const char* root = g_getenv("MC_REPO_ROOT");
        std::string src = std::string(root ? root : "..") + "/README.md";
        std::string text = MCGuiReadFile(src);
        if (text.empty()) text = MCGuiReadFile(MCGuiBuildDir() + "/../../README.md");
        GCHECK(!text.empty());
        MCGuiWriteFile(F("readme.md"), text);
    });
    MCGuiStep("open it, as the preview", [] {
        gMeter.begin();
        MCGuiOpenFileKept(F("readme.md"));
    });
    MCGuiWait("the page", 30, [] { return ed()->active() && ed()->inPreview(); });
    MCGuiPause(0.5);   // time is the condition: late layout passes land after the open
    MCGuiStep("the full preview's stall", [] {
        gMeter.end();
        MCGuiNote("readme full preview: longest stall %.0f ms, total %.0f ms of CPU", gMeter.worst,
                  gMeter.total());
        GCHECK_TIME(gMeter.worst, kStallBoundMs, "the longest stall opening README's preview");
        key("ctrl+shift+v");
    });
    MCGuiWait("source", 20, [] { return ed()->active() && ed()->isSource(); });
    MCGuiStep("the side pane", [] {
        gMeter.begin();
        key("ctrl+alt+v");
    });
    MCGuiWait("the whole page", 60, [] { return settled(); });
    MCGuiPause(0.5);   // as above
    MCGuiStep("the side pane's stall", [] {
        gMeter.end();
        MCGuiNote("readme side pane: longest stall %.0f ms, total %.0f ms of CPU", gMeter.worst,
                  gMeter.total());
        GCHECK_TIME(gMeter.worst, kStallBoundMs, "the longest stall opening README in the side pane");
    });
    MCGuiStep("close", [] {
        ed()->closeAllTabs();
        MCGuiSetSettings("lsp.enabled = false\ntheme = dark-plus\n");
    });
}
