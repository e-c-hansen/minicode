// MermaidSuite.cpp — Mermaid diagrams in the Markdown preview and Export as
// HTML (`./build/minicode-guitest mermaid`, linux/PARITY.md item 4.3, first
// part). mmdc is a stand-in script ($MINICODE_MMDC) that records its
// arguments and writes a fixed PNG (or, with a marker file, complains and
// fails); no real mmdc and no network. Checked: code before the picture, the
// picture after the run in both previews, the cache answering a reopen with
// no run, a failure's complaint as the code's tooltip, the dark and light
// theme reaching mmdc, markdown.mermaid = false and a page with no diagram
// running nothing, and the exported page's key lines.
#include "Harness.h"

#include "AppSeam.h"
#include "Document.h"
#include "Editor.h"
#include "Markdown.h"
#include "MarkdownSide.h"
#include "Mermaid.h"

#include <glib/gstdio.h>
#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string P(const std::string& n) { return MCGuiProjectFile(n); }
std::string Log() { return MCGuiReadFile(P("mmdc.log")); }

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

int offsetOf(const std::string& t, const std::string& needle) {
    const size_t at = t.find(needle);
    return at == std::string::npos ? -1 : (int)g_utf8_pointer_to_offset(t.c_str(), t.c_str() + at);
}

std::string diagram(const char* name, const char* body) {
    return std::string("# ") + name + "\n\nBefore.\n\n```mermaid\n" + body + "\n```\n\nAfter.\n";
}

void openPage(const char* file) {
    MCGuiStep(std::string("open ") + file, [=] { MCGuiOpenFileKept(P(file)); });
    MCGuiWait("the page", 20, [=] {
        return ed()->active() && ed()->active()->path() == P(file) && ed()->inPreview();
    });
}

bool picture() { return ed()->active()->pageInfo().embeds.size() == 1; }

const char* kPng =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";

}  // namespace

MC_GUI_SUITE(mermaid) {
    MCGuiStep("a stand-in mmdc and a cache of its own", [] {
        MCGuiSetSettings("lsp.enabled = false\ntheme = dark-plus\n");
        const std::string script =
            "#!/bin/sh\n"
            "echo \"$@\" >> '" + P("mmdc.log") + "'\n"
            "while [ $# -gt 0 ]; do [ \"$1\" = -o ] && out=\"$2\"; shift; done\n"
            "sleep 0.4\n"
            "if [ -e '" + P("mmdc.fail") + "' ]; then\n"
            "  echo 'Error: Parse error on line 2: unexpected token' >&2\n"
            "  echo '    at somewhere (stack.js:1:1)' >&2\n"
            "  exit 1\n"
            "fi\n"
            "echo '" + kPng + "' | base64 -d > \"$out\"\n";
        MCGuiWriteFile(P("fake-mmdc"), script);
        g_chmod(P("fake-mmdc").c_str(), 0755);
        g_setenv("MINICODE_MMDC", P("fake-mmdc").c_str(), TRUE);
        g_setenv("MINICODE_MERMAID_CACHE", P("mermaid-cache").c_str(), TRUE);
        MCGuiWriteFile(P("plain.md"), "# Plain\n\nNo diagram here.\n");
        MCGuiWriteFile(P("one.md"), diagram("One", "graph TD\n  A-->B"));
        MCGuiWriteFile(P("bad.md"), diagram("Bad", "graph LR\n  X-->"));
        MCGuiWriteFile(P("off.md"), diagram("Off", "graph BT\n  C-->D"));
        MCGuiWriteFile(P("side.md"), diagram("Side", "graph RL\n  E-->F"));
        MCGuiWriteFile(P("light.md"), diagram("Light", "graph TD\n  G-->H"));
        MCGuiWriteFile(P("mmdc.log"), "");
    });

    openPage("plain.md");
    MCGuiStep("a page with no diagram runs nothing", [] {
        GCHECK_EQ_STR(std::to_string(Mermaid::runs()), "0");
        MCGuiOpenFileKept(P("one.md"));
    });
    MCGuiWait("one.md", 20, [] { return ed()->active() && ed()->active()->path() == P("one.md") && ed()->inPreview(); });
    MCGuiStep("code first, the run asked once", [] {
        GCHECK(pageText().find("graph TD") != std::string::npos);
        GCHECK(!picture());
        GCHECK_EQ_STR(std::to_string(ed()->active()->pageInfo().waiting), "1");
    });
    MCGuiWait("the picture", 30, [] { return picture(); });
    MCGuiStep("the picture stands for the code, mmdc got the dark theme at scale 2", [] {
        GCHECK(pageText().find("graph TD") == std::string::npos);
        GCHECK(pageText().find("After.") != std::string::npos);
        GCHECK_EQ_STR(std::to_string(ed()->active()->pageInfo().waiting), "0");
        const std::string log = Log();
        GCHECK_EQ_STR(std::to_string(lines(log)), "1");
        GCHECK(log.find("-t dark") != std::string::npos);
        GCHECK(log.find("-s 2") != std::string::npos);
        GCHECK(log.find("-b transparent") != std::string::npos);
        GCHECK_EQ_STR(std::to_string(Mermaid::runs()), "1");
        ed()->closeAllTabs();
        Mermaid::forgetMemory();   // the disk cache stays
    });
    openPage("one.md");
    MCGuiStep("a reopen is answered by the cache with no run", [] {
        GCHECK(picture());
        GCHECK_EQ_STR(std::to_string(Mermaid::runs()), "1");
        GCHECK_EQ_STR(std::to_string(lines(Log())), "1");
        MCGuiSetSettings("lsp.enabled = false\ntheme = light-plus\n");
    });
    MCGuiWait("the light theme asks again, code meanwhile", 30, [] {
        return Mermaid::runs() == 2 && picture();
    });
    MCGuiStep("with the default theme", [] {
        const std::string log = Log();
        GCHECK_EQ_STR(std::to_string(lines(log)), "2");
        GCHECK(log.rfind("-t default") != std::string::npos);
        MCGuiSetSettings("lsp.enabled = false\ntheme = dark-plus\n");
    });
    MCGuiWait("dark again from the cache", 20, [] { return picture() && Mermaid::runs() == 2; });

    MCGuiStep("a failing mmdc", [] { MCGuiWriteFile(P("mmdc.fail"), "x"); });
    openPage("bad.md");
    MCGuiWait("the failure", 30, [] { return Mermaid::runs() == 3 && ed()->active()->pageInfo().waiting == 0; });
    MCGuiStep("the code stays, its tooltip is the complaint without the stack", [] {
        GCHECK(!picture());
        const std::string t = pageText();
        const int at = offsetOf(t, "graph LR");
        GCHECK(at >= 0);
        const std::string tip = Markdown::tooltipAt(ed()->active()->page(), at);
        GCHECK(tip.find("Parse error on line 2") != std::string::npos);
        GCHECK(tip.find("stack.js") == std::string::npos);
        GCHECK_EQ_STR(Markdown::tooltipAt(ed()->active()->page(), offsetOf(t, "After.")), "");
        g_unlink(P("mmdc.fail").c_str());
        MCGuiSetSettings("lsp.enabled = false\ntheme = dark-plus\nmarkdown.mermaid = false\n");
    });
    openPage("off.md");
    MCGuiStep("markdown.mermaid = false runs nothing and shows code", [] {
        MCGuiPause(0.8);   // time is the condition: a run would start by now
        GCHECK_EQ_STR(std::to_string(Mermaid::runs()), "3");
        GCHECK(pageText().find("graph BT") != std::string::npos);
        GCHECK(!picture());
        ed()->closeAllTabs();   // so turning the setting on draws nothing here
        MCGuiSetSettings("lsp.enabled = false\ntheme = dark-plus\n");
    });

    MCGuiStep("the side pane", [] { MCGuiOpenFileKept(P("side.md")); });
    MCGuiWait("side.md", 20, [] { return ed()->active() && ed()->active()->path() == P("side.md") && ed()->inPreview(); });
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
    MCGuiStep("code while the run is out", [] {
        GCHECK(ed()->sidePane()->pageText().find("graph RL") != std::string::npos ||
               ed()->sidePane()->embedCount() == 1);
    });
    MCGuiWait("the picture in the pane", 30, [] {
        return ed()->sidePane()->idle() && ed()->sidePane()->embedCount() == 1 &&
               ed()->sidePane()->pageText().find("graph RL") == std::string::npos;
    });
    MCGuiStep("one run for it", [] {
        GCHECK_EQ_STR(std::to_string(Mermaid::runs()), "4");
        GCHECK(ed()->sidePane()->pageText().find("After.") != std::string::npos);
        MCGuiKey("ctrl+alt+v");
        ed()->closeAllTabs();
    });
}

MC_GUI_SUITE(export_html) {
    MCGuiStep("a small document", [] {
        MCGuiSetSettings("lsp.enabled = false\n");
        MCGuiWriteFile(P("doc.md"),
                       "# Hello World\n\nSome *text* with [a link](https://example.com) and "
                       "[bad](javascript:alert(1)).\n\n- one\n- [x] two\n\n```python\nprint('<hi>')\n```\n");
        g_setenv("MINICODE_HTML_EXPORT_PATH", P("doc-out.html").c_str(), TRUE);
        MCGuiOpenFileKept(P("doc.md"));
    });
    MCGuiWait("the page", 20, [] { return ed()->active() && ed()->active()->path() == P("doc.md") && ed()->inPreview(); });
    MCGuiStep("Export as HTML is on for Markdown, and writes the page", [] {
        GAction* a = g_action_map_lookup_action(G_ACTION_MAP(MCGuiApp().window), "exporthtml");
        GCHECK(a != nullptr);
        if (a) GCHECK(g_action_get_enabled(a));
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApp().window), "exporthtml", nullptr);
    });
    MCGuiWait("the file", 10, [] { return MCGuiFileExists(P("doc-out.html")); });
    MCGuiStep("its key lines", [] {
        const std::string h = MCGuiReadFile(P("doc-out.html"));
        GCHECK(h.find("<title>doc.md</title>") != std::string::npos || h.find("<title>Hello World</title>") != std::string::npos);
        GCHECK(h.find("Hello World</h1>") != std::string::npos);
        GCHECK(h.find("href=\"https://example.com\"") != std::string::npos);
        GCHECK(h.find("javascript:") == std::string::npos);
        GCHECK(h.find("language-python") != std::string::npos);
        GCHECK(h.find("&lt;hi&gt;") != std::string::npos);
        GCHECK(h.find("checkbox") != std::string::npos);
        GCHECK(!MCGuiFileExists(P("doc-out.html.tmp")));
    });
    MCGuiStep("unsaved changes are exported, a text file is not", [] {
        GtkTextBuffer* b = ed()->active()->buffer();
        GtkTextIter it;
        gtk_text_buffer_get_start_iter(b, &it);
        gtk_text_buffer_insert(b, &it, "Unsaved line.\n\n", -1);
        g_unlink(P("doc-out.html").c_str());
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApp().window), "exporthtml", nullptr);
    });
    MCGuiWait("the second file", 10, [] { return MCGuiFileExists(P("doc-out.html")); });
    MCGuiStep("with the unsaved line", [] {
        GCHECK(MCGuiReadFile(P("doc-out.html")).find("Unsaved line.") != std::string::npos);
        MCGuiWriteFile(P("t.txt"), "plain\n");
        MCGuiOpenFileKept(P("t.txt"));
    });
    MCGuiWait("t.txt", 10, [] { return ed()->active() && ed()->active()->path() == P("t.txt"); });
    MCGuiStep("off for a text file", [] {
        MCGuiPause(0.3);   // the title update runs from an idle
        GAction* a = g_action_map_lookup_action(G_ACTION_MAP(MCGuiApp().window), "exporthtml");
        GCHECK(a && !g_action_get_enabled(a));
        g_unsetenv("MINICODE_HTML_EXPORT_PATH");
        ed()->closeAllTabs();
    });
}
