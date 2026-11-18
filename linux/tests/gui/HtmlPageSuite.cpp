// HtmlPageSuite.cpp — an .html file as its page (`./build/mutible-guitest
// html_page`; the Mac's html_page suite, HtmlView.h, the core's HtmlPage.h):
// it opens as its page with the text readable and its stylesheet applied; with
// html.scripts unset a script that writes into the page has not run, under
// the bar's question, and Run Scripts (not remembered) runs it for that file;
// the page never takes the keyboard by itself; the toggle shows the source
// and back; a save and a write from outside load the page again with its
// scroll kept; a link to another local file opens it in a tab of its own; a
// remembered answer writes html.scripts as the Mac does and later pages run
// unasked, and `off` never runs or asks.
//
// Only local pages (file://), nothing off this machine. Page state is read in
// the view's own script world.
#include "Harness.h"

#include "AppSettings.h"
#include "DocumentView.h"
#include "Editor.h"
#include "WindowLayout.h"
#include "HtmlView.h"

#include <string>

#ifdef MUTIBLE_ENABLE_BROWSER

namespace {

Editor* ed() { return MCGuiApp().editor; }
HtmlView* page() { return ed() ? ed()->htmlView() : nullptr; }
std::string F(const std::string& name) { return MCGuiProjectFile("html-page/" + name); }
bool has(const std::string& s, const std::string& w) { return s.find(w) != std::string::npos; }

std::string settingsText() {
    const char* p = g_getenv("MUTIBLE_SETTINGS");
    return p ? MCGuiReadFile(p) : "";
}

std::string pageSource(const std::string& marker) {
    return "<!doctype html><html><head><title>Index</title><style>body { color: rgb(1, 2, 3); }</style>"
           "</head><body><input id=field autofocus>"
           "<p id=marker>" + marker + "</p><div id=out>not run</div>"
           "<p><a id=link href='other.html'>other</a> <a id=top href='#end'>end</a></p>"
           "<div style='height:4000px'></div><p id=end>end</p>"
           "<script>document.getElementById('out').textContent = 'script ran';"
           "document.getElementById('field').focus();</script></body></html>";
}

// The state of the page, in the view's world: the output's text, the
// marker's, the scroll and the body's color. "" until it answers.
std::string gSeen;
void read(const char* what) {
    gSeen.clear();
    if (!page()) return;
    page()->evaluate(what, [](const std::string& v) { gSeen = v.empty() ? "<none>" : v; });
}
const char* const kState =
    "[document.getElementById('out').textContent, document.getElementById('marker').textContent,"
    " Math.round(window.scrollY), getComputedStyle(document.body).color].join('|')";

bool loaded(unsigned loads, const std::string& name) {
    HtmlView* p = page();
    if (!p || p->loads() < loads || !p->settled()) return false;
    const char* uri = webkit_web_view_get_uri(p->webView());
    return uri && has(uri, name);
}

bool focusInPage() {
    HtmlView* p = page();
    GtkWidget* f = p && p->webView() ? gtk_window_get_focus(GTK_WINDOW(MCGuiApp().window)) : nullptr;
    return f && (f == GTK_WIDGET(p->webView()) || gtk_widget_is_ancestor(f, GTK_WIDGET(p->webView())));
}

unsigned gLoads = 0;
size_t gDocs = 0;
bool gBrowserBefore = false;   // another suite may have left the pane shown
std::string gWasSettings;

}  // namespace

MC_GUI_SUITE(html_page) {
    MCGuiStep("a folder with pages; html.scripts unset", [] {
        ed()->closeAllTabs();
        gWasSettings = settingsText();
        gBrowserBefore = MCGuiApp().layout->browserShown();
        MCGuiSetSettings("lsp.enabled = false\n");
        MCGuiWriteFile(F("index.html"), pageSource("First"));
        MCGuiWriteFile(F("second.html"), pageSource("Two"));
        MCGuiWriteFile(F("third.html"), pageSource("Three"));
        MCGuiWriteFile(F("other.html"), "<html><head><title>Other</title></head>"
                                        "<body><p>plain, no scripts</p></body></html>");
        MCGuiOpenFileKept(F("index.html"));
    });
    MCGuiWait("index.html shows as its page", 20, [] {
        Document* d = ed()->active();
        return d && d->isHtml() && d->showsPage() && page() && loaded(1, "index.html");
    });
    MCGuiStep("its text is readable, its stylesheet applies, its script has not run", [] {
        GCHECK(gtk_widget_get_visible(page()->bar()));
        GCHECK(page()->asking() && !page()->runsScripts());
        GCHECK_EQ_STR(page()->barText(), "This page has scripts. Run them?");
        GCHECK(MCGuiFindButton(page()->bar(), "Run Scripts") != nullptr);
        GCHECK(MCGuiFindButton(page()->bar(), "Don't Run") != nullptr);
        GCHECK(page()->rememberBox() && gtk_widget_get_visible(page()->rememberBox()));
        read(kState);
    });
    MCGuiWait("the page answers", 10, [] { return !gSeen.empty(); });
    MCGuiStep("the page as asked about", [] {
        MCGuiNote("before: %s", gSeen.c_str());
        GCHECK_EQ_STR(gSeen, "not run|First|0|rgb(1, 2, 3)");
        MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "html-page-asking");
    });

    MCGuiStep("Run Scripts, pressed: the page loads again with them", [] {
        gLoads = page()->loads();
        MCGuiClick(MCGuiFindButton(page()->bar(), "Run Scripts"));
    });
    MCGuiWait("the page loads with scripts and the bar says what was chosen", 20, [] {
        return loaded(gLoads + 1, "index.html") && !page()->asking() && page()->noting();
    });
    MCGuiStep("the line keeps the bar's place, nothing is written to the settings", [] {
        GCHECK(page()->runsScripts());
        GCHECK(gtk_widget_get_visible(page()->bar()));
        GCHECK_EQ_STR(page()->barText(), "This page's scripts run, until mutible quits.");
        GCHECK(!MCGuiFindButton(page()->bar(), "Run Scripts") ||
               !gtk_widget_get_visible(MCGuiFindButton(page()->bar(), "Run Scripts")));
        GCHECK(!has(settingsText(), "\nhtml.scripts"));
        read(kState);
    });
    MCGuiWait("the page answers", 10, [] { return !gSeen.empty(); });
    MCGuiStep("the script ran", [] { GCHECK_EQ_STR(gSeen, "script ran|First|0|rgb(1, 2, 3)"); });
    // Time is the condition: the autofocus field and the script's focus()
    // have had their chance to take the keyboard.
    MCGuiPause(0.5);
    MCGuiStep("the page never took the keyboard by itself", [] { GCHECK(!focusInPage()); });

    // ------------------------------------------- the source, and the place
    MCGuiStep("scrolled down", [] { read("window.scrollTo(0, 600); Math.round(window.scrollY)"); });
    MCGuiWait("scrolled", 10, [] { return gSeen == "600"; });
    MCGuiStep("the toggle shows the source", [] {
        MCGuiKey("ctrl+shift+v");
    });
    MCGuiWait("the source is on show", 10, [] { return ed()->active() && !ed()->active()->showsPage(); });
    MCGuiStep("it is the file's text, in the editor", [] {
        GCHECK(has(MCGuiEditorText(), "<p id=marker>First</p>"));
        GtkWidget* slot = gtk_widget_get_parent(gtk_widget_get_parent(ed()->textView()));
        GCHECK(gtk_stack_get_visible_child(GTK_STACK(slot)) != page()->widget());
        gLoads = page()->loads();
        MCGuiKey("ctrl+shift+v");
    });
    MCGuiWait("the page is back", 10, [] { return ed()->active() && ed()->active()->showsPage(); });
    MCGuiWait("the toggle gave the keyboard to the page: asked for, given", 10, [] { return focusInPage(); });
    MCGuiStep("it was not loaded again for an unchanged file", [] {
        GCHECK(page()->loads() == gLoads);
        GCHECK(page()->settled());
    });

    MCGuiStep("a write from outside", [] {
        gLoads = page()->loads();
        MCGuiWriteFile(F("index.html"), pageSource("Outside"));
    });
    MCGuiWait("the page follows the file and puts its place back", 20, [] {
        return loaded(gLoads + 1, "index.html");
    });
    MCGuiStep("read the page", [] { read(kState); });
    MCGuiWait("the page answers", 10, [] { return !gSeen.empty(); });
    MCGuiStep("the new text, the scroll kept, the answer held for the file", [] {
        MCGuiNote("after the write: %s", gSeen.c_str());
        GCHECK_EQ_STR(gSeen, "script ran|Outside|600|rgb(1, 2, 3)");
    });

    // ------------------------------------------------------------ links
    MCGuiStep("a link to a place in the page scrolls there", [] {
        read("document.getElementById('top').click(); 'clicked'");
    });
    MCGuiWait("clicked", 10, [] { return gSeen == "clicked"; });
    MCGuiWait("scrolled to the end, on the same page", 10, [] {
        if (gSeen.empty()) read("Math.round(window.scrollY)");   // one question at a time
        else if (atoi(gSeen.c_str()) > 2000) return has(ed()->active()->path(), "/index.html");
        else gSeen.clear();
        return false;
    });
    MCGuiStep("a link to another page", [] { read("document.getElementById('link').click(); 'clicked'"); });
    MCGuiWait("opens that file in a tab of its own, as a page", 20, [] {
        Document* d = ed()->active();
        return d && has(d->path(), "/other.html") && d->showsPage() && loaded(1, "other.html");
    });
    MCGuiStep("a page with no scripts is not asked about; the answered line went with the open", [] {
        GCHECK(!page()->asking() && !page()->noting());
        GCHECK(!gtk_widget_get_visible(page()->bar()));
        GCHECK(ed()->documents().size() == 2);
    });

    // ------------------------------------------------- remembered answers
    MCGuiStep("another scripted page asks; Run Scripts remembered writes the setting", [] {
        MCGuiOpenFileKept(F("second.html"));
    });
    MCGuiWait("second.html asks", 20, [] { return page() && page()->asking() && loaded(1, "second.html"); });
    MCGuiStep("the box is ticked and Run Scripts pressed", [] {
        GtkWidget* box = page()->rememberBox();
        GCHECK(box && !page()->rememberChecked());
        MCGuiClick(box);
    });
    MCGuiWait("it is ticked", 5, [] { return page()->rememberChecked(); });
    MCGuiStep("press", [] {
        gLoads = page()->loads();
        MCGuiClick(MCGuiFindButton(page()->bar(), "Run Scripts"));
    });
    MCGuiWait("the setting is written and the page runs", 20, [] {
        return has(settingsText(), "html.scripts = on") && loaded(gLoads + 1, "second.html");
    });
    MCGuiStep("it is the setting the Mac writes, and the line's close box takes the bar away", [] {
        GCHECK(has(settingsText(), "\nhtml.scripts = on\n"));
        GCHECK(page()->runsScripts() && !page()->asking());
        GCHECK_EQ_STR(page()->barText(), "This page's scripts run.");
        MCGuiClick(MCGuiFindButton(page()->bar(), "\xC3\x97"));
    });
    MCGuiWait("the line is gone", 5, [] { return !gtk_widget_get_visible(page()->bar()); });
    MCGuiStep("from then on pages run their scripts unasked", [] { MCGuiOpenFileKept(F("third.html")); });
    MCGuiWait("third.html loads", 20, [] { return page() && loaded(1, "third.html"); });
    MCGuiStep("not asked, and its script runs", [] {
        GCHECK(!page()->asking() && page()->runsScripts());
        read(kState);
    });
    MCGuiWait("the page answers", 10, [] { return !gSeen.empty(); });
    MCGuiStep("ran", [] { GCHECK_EQ_STR(gSeen, "script ran|Three|0|rgb(1, 2, 3)"); });

    MCGuiStep("html.scripts = off: never run, never asked", [] {
        gLoads = page()->loads();
        MCGuiSetSettings("lsp.enabled = false\nhtml.scripts = off\n");
    });
    MCGuiWait("the page loads again without scripts", 20, [] {
        return !page()->runsScripts() && loaded(gLoads + 1, "third.html");
    });
    MCGuiStep("it did not run, and nothing asks", [] {
        GCHECK(!page()->asking());
        read(kState);
    });
    MCGuiWait("the page answers", 10, [] { return !gSeen.empty(); });
    MCGuiStep("not run", [] {
        GCHECK_EQ_STR(gSeen, "not run|Three|0|rgb(1, 2, 3)");
    });

    // ----------------------------------- frames, and a script's navigation
    MCGuiStep("scripts on; a page with an unnamed iframe to a local file", [] {
        MCGuiSetSettings("lsp.enabled = false\nhtml.scripts = on\n");
        MCGuiWriteFile(F("frame.html"),
                       "<p>inside the frame</p>"
                       "<script>parent.postMessage(document.body.innerText, '*');</script>");
        MCGuiWriteFile(F("framed.html"),
                       "<div id=out>none</div><iframe src='frame.html'></iframe>"
                       "<script>addEventListener('message', function (e) {"
                       " document.getElementById('out').textContent = e.data; });</script>");
        gDocs = ed()->documents().size();
        MCGuiOpenFileKept(F("framed.html"));
    });
    MCGuiWait("the frame's text arrives in the page", 20, [] {
        if (!page() || !loaded(1, "framed.html")) return false;
        if (gSeen.empty()) read("document.getElementById('out').textContent");
        else if (has(gSeen, "inside the frame")) return true;
        else gSeen.clear();
        return false;
    });
    MCGuiStep("the frame opened nothing: no new tab, no browser pane, still its own page", [] {
        GCHECK(ed()->documents().size() == gDocs + 1);   // framed.html only
        GCHECK(has(ed()->active()->path(), "/framed.html"));
        GCHECK(MCGuiApp().layout->browserShown() == gBrowserBefore);
        const char* uri = webkit_web_view_get_uri(page()->webView());
        GCHECK(uri && has(uri, "framed.html"));
        MCGuiWriteFile(F("target.html"), "<p>the target</p>");
        MCGuiWriteFile(F("jump.html"),
                       "<p>jumping</p><script>if (window.name !== 'jumped') {"
                       " window.name = 'jumped'; location = 'target.html'; }</script>");
        gDocs = ed()->documents().size();
        MCGuiOpenFileKept(F("jump.html"));
    });
    MCGuiWait("the script's navigation away opens that file in a tab of its own", 20, [] {
        Document* d = ed()->active();
        return d && has(d->path(), "/target.html") && d->showsPage() && loaded(1, "target.html");
    });
    MCGuiStep("the HTML tab is still there, and shows its own page", [] {
        GCHECK(ed()->documents().size() == gDocs + 2);   // jump.html and target.html
        gLoads = page()->loads();
        MCGuiOpenFileKept(F("jump.html"));
    });
    MCGuiWait("jump.html shows", 20, [] {
        Document* d = ed()->active();
        return d && has(d->path(), "/jump.html") && loaded(1, "jump.html");
    });
    MCGuiStep("its own page, not the target, and the frame opened no browser", [] {
        const char* uri = webkit_web_view_get_uri(page()->webView());
        GCHECK(uri && has(uri, "jump.html") && !has(uri, "target.html"));
        GCHECK(MCGuiApp().layout->browserShown() == gBrowserBefore);
        ed()->closeAllTabs();
        MCGuiSetSettings(gWasSettings);
    });
}

#else
MC_GUI_SUITE(html_page) {}
#endif
