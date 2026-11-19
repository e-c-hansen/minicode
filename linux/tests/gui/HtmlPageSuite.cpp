// HtmlPageSuite.cpp — an .html file as its page (`./build/mutible-guitest
// html_page`; the Mac's html_page suite, HtmlView.h, the core's HtmlPage.h):
// it opens as its page with the text readable and its stylesheet applied; with
// html.scripts unset a script that writes into the page has not run, under
// the bar's question, and Run Scripts (not remembered) runs it for that file;
// the page never takes the keyboard by itself; the toggle shows the source
// and back; a save and a write from outside load the page again with its
// scroll kept; a link to another local file opens it in a tab of its own; a
// remembered answer writes html.scripts as the Mac does and later pages run
// unasked, and `off` never runs or asks. A page that sends itself elsewhere
// (round 10's documents defect 2, the Mac's html_page): a meta refresh to
// zoommtg://, a script setting location to smb:// or to a local file, and
// window.open to zoommtg:// without a click open nothing and leave the page
// as it is, the refused ones named in the status bar; a meta refresh to a
// web address is followed only while scripts run; a real click on a link,
// and on a target=_blank one, still opens its file. A link to another app
// (the Mac's #254, HtmlPage::follow): a page's script clicking its own
// smb:// link, and sending its own form to mailto:, is refused with no
// question and named in the status bar; a real click on a zoommtg:// link
// asks, naming the app and the address in the Mac's words, Cancel opens
// nothing and Open hands it to the desktop's handler; a real click on an
// https link asks nothing; a Markdown preview's zoommtg:// link goes to the
// desktop's handler unasked, as before.
//
// Only local pages (file://), nothing off this machine: every address the
// page hands the link handler is recorded (HtmlView::linkSpy) and only a
// file or an app's address reaches it, the desktop's handler is the
// harness's recorder (MCGuiLaunchedUris, so no app starts), and the web
// address is https://127.0.0.1:2/, where nothing listens (not port 9:
// WebKit never navigates to a port on the fetch standard's bad-ports list,
// so a refresh there does nothing at all). The app the question names is a
// desktop file the suite writes in the scratch world's data folder, with
// its own default in the scratch config's mimeapps.list. Page state is read
// in the view's own script world.
#include "Harness.h"

#include "AppSettings.h"
#include "DocumentView.h"
#include "Editor.h"
#include "WindowLayout.h"
#include "HtmlView.h"

#include <glib/gstdio.h>

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

// Every address the page handed the link handler (HtmlView::linkSpy); a web
// address goes no further, so nothing leaves this machine, while a local
// file opens and an app's address reaches the desktop's handler, which is
// the harness's recorder. And how many there were, and how many the
// desktop's handler got, before a step.
std::vector<std::string> gOpened;
size_t gOpenedBefore = 0, gLaunchedBefore = 0;
bool spy(const std::string& url) {
    gOpened.push_back(url);
    return url.rfind("http:", 0) != 0 && url.rfind("https:", 0) != 0;
}
void countBefore() {
    gOpenedBefore = gOpened.size();
    gLaunchedBefore = MCGuiLaunchedUris().size();
}

std::string statusText() { return gtk_label_get_text(GTK_LABEL(MCGuiApp().statusLabel)); }
std::string pageUri() {
    const char* uri = page() && page()->webView() ? webkit_web_view_get_uri(page()->webView()) : nullptr;
    return uri ? uri : "";
}
// Nothing reached the link handler or the desktop's since countBefore, and
// the tab and its page are still `name`.
void nothingOpened(const std::string& name) {
    MCGuiNote("opened %zu -> %zu, launched %zu -> %zu; page %s; status \"%s\"", gOpenedBefore,
              gOpened.size(), gLaunchedBefore, MCGuiLaunchedUris().size(), pageUri().c_str(),
              statusText().c_str());
    GCHECK(gOpened.size() == gOpenedBefore);
    GCHECK(MCGuiLaunchedUris().size() == gLaunchedBefore);
    GCHECK(has(ed()->active()->path(), "/" + name) && ed()->active()->showsPage());
    GCHECK(has(pageUri(), name));
    GCHECK(MCGuiApp().layout->browserShown() == gBrowserBefore);
}
// The links page laid out: its second link 60 pixels down.
bool laidOut() {
    if (gSeen.empty()) read("Math.round(document.getElementById('blank').getBoundingClientRect().top)");
    else if (gSeen == "60") return true;
    else gSeen.clear();
    return false;
}
size_t gLaunchedAtStart = 0;
const char* const kNotOpened = " by itself; mutible did not open it";
const char* const kNotFollowed = " by itself; with its scripts off, it was not followed";

// The app the question names: a desktop file in the scratch world's data
// folder for zoommtg:, the default in the scratch config's mimeapps.list
// (put back as it was at the end). Nothing runs it: the desktop's handler
// is the harness's recorder.
const char* const kApp = "Zoom Check";
const char* const kAppLink = "zoommtg://example";
std::string appFile() { return std::string(g_get_user_data_dir()) + "/applications/mutible-check-zoom.desktop"; }
std::string mimeApps() { return std::string(g_get_user_config_dir()) + "/mimeapps.list"; }
bool gHadMimeApps = false;
std::string gWasMimeApps;

// The question over the window, when it is up: the newest toplevel that is
// not the app's window.
GtkWidget* question() {
    GtkWindow* top = MCGuiTopWindow();
    return top && top != GTK_WINDOW(MCGuiApp().window) ? GTK_WIDGET(top) : nullptr;
}
bool noQuestion() { return !page()->askingToOpen() && !question(); }
double leftOf(GtkWidget* w, GtkWidget* in) {
    graphene_rect_t r;
    return w && gtk_widget_compute_bounds(w, in, &r) ? r.origin.x : -1;
}

// Where a Markdown page's text `word` is, in its view's coordinates.
double gWordX = 0, gWordY = 0;
bool wordAt(const std::string& word, double* x, double* y) {
    GtkTextBuffer* b = ed()->active() ? ed()->active()->page() : nullptr;
    if (!b) return false;
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* raw = gtk_text_buffer_get_text(b, &s, &e, TRUE);
    const std::string t = raw ? raw : "";
    g_free(raw);
    const size_t at = t.find(word);
    if (at == std::string::npos) return false;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(b, &it, (int)g_utf8_pointer_to_offset(t.c_str(), t.c_str() + at));
    GdkRectangle r;
    GtkTextView* tv = GTK_TEXT_VIEW(ed()->textView());
    gtk_text_view_get_iter_location(tv, &it, &r);
    if (r.height <= 0) return false;
    int wx = 0, wy = 0;
    gtk_text_view_buffer_to_window_coords(tv, GTK_TEXT_WINDOW_WIDGET, r.x, r.y, &wx, &wy);
    *x = wx + 3;
    *y = wy + r.height / 2.0;
    return true;
}

}  // namespace

MC_GUI_SUITE(html_page) {
    MCGuiStep("a folder with pages; html.scripts unset", [] {
        ed()->closeAllTabs();
        gWasSettings = settingsText();
        gLaunchedAtStart = MCGuiLaunchedUris().size();
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
        gOpened.clear();
        page()->linkSpy = spy;
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
        GCHECK(gOpened.size() == 1 && has(gOpened[0], "/other.html"));   // the link, through the handler
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

    // ------------------------------- a page that goes somewhere by itself
    // Round 10's documents defect 2: a meta refresh to another app's scheme
    // opened that app with no click. Only a link the user clicks, or a form
    // they send, is followed; a web address the page goes to by itself only
    // while its scripts run (HtmlPage::mayFollow). Scripts are off here.
    MCGuiStep("a page whose meta refresh goes to zoommtg://", [] {
        MCGuiWriteFile(F("refresh-app.html"),
                       "<html><head><title>Refresh</title>"
                       "<meta http-equiv=\"refresh\" content=\"0; url=zoommtg://example\">"
                       "</head><body><p id=marker>refreshing</p></body></html>");
        countBefore();
        MCGuiOpenFileKept(F("refresh-app.html"));
    });
    MCGuiWait("it loads, and its refresh is refused, named in the status bar", 20, [] {
        return loaded(1, "refresh-app.html") && has(statusText(), "zoommtg://example");
    });
    MCGuiStep("nothing opened, the page still the file, the Mac's words", [] {
        nothingOpened("refresh-app.html");
        GCHECK_EQ_STR(statusText(), std::string("The page tried to open zoommtg://example") + kNotOpened);
    });
    MCGuiStep("one to a web address, with no scripts running", [] {
        MCGuiWriteFile(F("refresh-web.html"),
                       "<html><head><title>Web</title>"
                       "<meta http-equiv=\"refresh\" content=\"0; url=https://127.0.0.1:2/refused\">"
                       "</head><body><p>refreshing</p></body></html>");
        countBefore();
        MCGuiOpenFileKept(F("refresh-web.html"));
    });
    MCGuiWait("is not followed either", 20, [] {
        return loaded(1, "refresh-web.html") && has(statusText(), "127.0.0.1:2/refused");
    });
    MCGuiStep("(nothing opened, the page still the file)", [] {
        nothingOpened("refresh-web.html");
        GCHECK_EQ_STR(statusText(),
                      std::string("The page tried to open https://127.0.0.1:2/refused") + kNotFollowed);
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
        countBefore();
        MCGuiOpenFileKept(F("jump.html"));
    });
    // A file, even with scripts on, is not a web address: the Mac's rule.
    // The note is cut at 80 characters (HtmlPage::refusedNote, the Mac's
    // wording) and flashed: under CI's long scratch root the file's name
    // falls past the cut, so the wait looks for the note's start while it
    // shows, and the next step does not read the status again.
    MCGuiWait("the script's navigation to a local file is refused, named in the status bar", 20, [] {
        return loaded(1, "jump.html") && has(statusText(), "The page tried to open file://");
    });
    MCGuiStep("no tab for it, and the page is still its own", [] {
        nothingOpened("jump.html");
        GCHECK(ed()->documents().size() == gDocs + 1);   // jump.html only
        GCHECK(!has(pageUri(), "target.html"));
    });

    MCGuiStep("a meta refresh to a web address, with scripts on", [] {
        MCGuiWriteFile(F("refresh-on.html"),
                       "<html><head><title>Web</title>"
                       "<meta http-equiv=\"refresh\" content=\"0; url=https://127.0.0.1:2/followed\">"
                       "</head><body><p>refreshing</p></body></html>");
        countBefore();
        MCGuiOpenFileKept(F("refresh-on.html"));
    });
    MCGuiWait("is followed: the address goes to the link handler", 20, [] {
        return loaded(1, "refresh-on.html") && gOpened.size() > gOpenedBefore;
    });
    MCGuiStep("that address and nothing else, the page still the file", [] {
        MCGuiNote("opened: %s", gOpened.back().c_str());
        GCHECK(gOpened.size() == gOpenedBefore + 1);
        GCHECK_EQ_STR(gOpened.back(), "https://127.0.0.1:2/followed");
        gOpenedBefore = gOpened.size();   // the rest as for a refused one
        nothingOpened("refresh-on.html");
        GCHECK(!has(statusText(), "127.0.0.1:2/followed"));
    });

    MCGuiStep("a script setting location to smb://", [] {
        MCGuiWriteFile(F("script-smb.html"),
                       "<p id=out>not run</p>"
                       "<script>location = 'smb://x/share';"
                       "document.getElementById('out').textContent = 'ran';</script>");
        countBefore();
        MCGuiOpenFileKept(F("script-smb.html"));
    });
    MCGuiWait("is refused, named in the status bar", 20, [] {
        return loaded(1, "script-smb.html") && has(statusText(), "smb://x/share");
    });
    MCGuiStep("nothing opened, though its scripts run", [] {
        GCHECK(page()->runsScripts());
        nothingOpened("script-smb.html");
        GCHECK_EQ_STR(statusText(), std::string("The page tried to open smb://x/share") + kNotOpened);
    });

    MCGuiStep("a window.open to zoommtg:// without a click", [] {
        MCGuiWriteFile(F("popup.html"),
                       "<p id=out>not run</p>"
                       "<script>var w = window.open('zoommtg://y/meeting');"
                       "document.getElementById('out').textContent = w ? 'a window' : 'no window';</script>");
        countBefore();
        gSeen.clear();
        MCGuiOpenFileKept(F("popup.html"));
    });
    MCGuiWait("its script has run", 20, [] {
        if (!loaded(1, "popup.html")) return false;
        if (gSeen.empty()) read("document.getElementById('out').textContent");
        else if (gSeen == "a window" || gSeen == "no window") return true;
        else gSeen.clear();
        return false;
    });
    // Time is the condition: a refusal or an open, on its way, has had its
    // chance to arrive.
    MCGuiPause(0.5);
    MCGuiStep("nothing opened (refused, or no window given at all)", [] {
        MCGuiNote("window.open gave %s", gSeen.c_str());
        nothingOpened("popup.html");
        GCHECK(!has(statusText(), "zoommtg://y") || has(statusText(), kNotOpened));
    });

    // ------------------------- a script's own click, to another app (#254)
    // A page whose scripts run can click its own link (a.click()) and send
    // its own form (form.submit()), and WebKit reports those as a link
    // clicked and a form sent, like the user's: the Mac asks about every
    // one, GTK tells them apart (is_user_gesture) and refuses the script's
    // with no question (HtmlPage::follow).
    MCGuiStep("a page whose script clicks its own smb:// link at load", [] {
        MCGuiWriteFile(F("script-click.html"),
                       "<a href='smb://x'>share</a><p id=out>not run</p>"
                       "<script>document.querySelector('a').click();"
                       "document.getElementById('out').textContent = 'clicked';</script>");
        countBefore();
        gSeen.clear();
        MCGuiOpenFileKept(F("script-click.html"));
    });
    MCGuiWait("is refused, named in the status bar", 20, [] {
        return loaded(1, "script-click.html") && has(statusText(), "smb://x");
    });
    MCGuiStep("no question, nothing opened, though the script ran", [] {
        GCHECK(page()->runsScripts());
        GCHECK(noQuestion());
        nothingOpened("script-click.html");
        GCHECK_EQ_STR(statusText(), std::string("The page tried to open smb://x") + kNotOpened);
        read("document.getElementById('out').textContent");
    });
    MCGuiWait("the page answers", 10, [] { return !gSeen.empty(); });
    MCGuiStep("(its script clicked)", [] { GCHECK_EQ_STR(gSeen, "clicked"); });

    MCGuiStep("a page whose script sends its own form to mailto:", [] {
        MCGuiWriteFile(F("script-form.html"),
                       "<form action='mailto:someone@example.com' method=get>"
                       "<input name=subject value=hello></form>"
                       "<script>document.forms[0].submit();</script>");
        countBefore();
        MCGuiOpenFileKept(F("script-form.html"));
    });
    MCGuiWait("is refused the same way", 20, [] {
        return loaded(1, "script-form.html") && has(statusText(), "mailto:someone@example.com");
    });
    MCGuiStep("(no question, nothing opened)", [] {
        GCHECK(noQuestion());
        nothingOpened("script-form.html");
        GCHECK(statusText().rfind("The page tried to open mailto:someone@example.com", 0) == 0);
        GCHECK(has(statusText(), kNotOpened));
    });

    // ----------------------------------------------- the user's own click
    MCGuiStep("a page of links, clicked for real", [] {
        MCGuiWriteFile(F("clicked.html"), "<p>the clicked link's page</p>");
        MCGuiWriteFile(F("blank-target.html"), "<p>the target=_blank link's page</p>");
        MCGuiWriteFile(F("links.html"),
                       "<body style='margin:0'>"
                       "<a id=plain href='clicked.html' style='display:block;width:400px;height:60px'>plain</a>"
                       "<a id=blank href='blank-target.html' target=_blank"
                       " style='display:block;width:400px;height:60px'>blank</a></body>");
        gDocs = ed()->documents().size();
        MCGuiOpenFileKept(F("links.html"));
        gSeen.clear();
    });
    MCGuiWait("laid out", 20, [] {
        if (!loaded(1, "links.html")) return false;
        return laidOut();
    });
    MCGuiStep("a click on the plain link", [] {
        countBefore();
        MCGuiClick(GTK_WIDGET(page()->webView()), 60, 30);
    });
    MCGuiWait("opens its file in a tab of its own, as a page", 20, [] {
        Document* d = ed()->active();
        return d && has(d->path(), "/clicked.html") && d->showsPage() && loaded(1, "clicked.html");
    });
    MCGuiStep("through the link handler, with no question", [] {
        GCHECK(noQuestion());
        GCHECK(gOpened.size() == gOpenedBefore + 1 && has(gOpened.back(), "/clicked.html"));
        GCHECK(ed()->documents().size() == gDocs + 2);
        MCGuiOpenFileKept(F("links.html"));
        gSeen.clear();
    });
    MCGuiWait("the links again", 20, [] {
        if (!loaded(1, "links.html")) return false;
        return laidOut();
    });
    MCGuiStep("a click on the target=_blank link", [] {
        countBefore();
        MCGuiClick(GTK_WIDGET(page()->webView()), 60, 90);
    });
    MCGuiWait("opens its file in a tab of its own too", 20, [] {
        Document* d = ed()->active();
        return d && has(d->path(), "/blank-target.html") && d->showsPage() &&
               loaded(1, "blank-target.html");
    });
    MCGuiStep("through the link handler, and the desktop's handler never got anything", [] {
        GCHECK(noQuestion());
        GCHECK(gOpened.size() == gOpenedBefore + 1 && has(gOpened.back(), "/blank-target.html"));
        GCHECK(MCGuiLaunchedUris().size() == gLaunchedAtStart);
        GCHECK(MCGuiApp().layout->browserShown() == gBrowserBefore);
    });

    // ------------------------------- the user's own click, to another app
    MCGuiStep("an app for zoommtg: in the scratch world", [] {
        gHadMimeApps = MCGuiFileExists(mimeApps());
        gWasMimeApps = gHadMimeApps ? MCGuiReadFile(mimeApps()) : "";
        MCGuiWriteFile(appFile(), std::string("[Desktop Entry]\nType=Application\nName=") + kApp +
                                      "\nExec=/bin/false %u\nNoDisplay=true\n"
                                      "MimeType=x-scheme-handler/zoommtg;\n");
        MCGuiWriteFile(mimeApps(), gWasMimeApps + "\n[Default Applications]\n"
                                                  "x-scheme-handler/zoommtg=mutible-check-zoom.desktop\n");
    });
    MCGuiWait("the desktop knows it", 20, [] { return HtmlView::appFor(kAppLink) == kApp; });
    MCGuiStep("a page with a zoommtg:// link and an https one, clicked for real", [] {
        MCGuiWriteFile(F("app-link.html"),
                       std::string("<body style='margin:0'>"
                                   "<a id=app href='") + kAppLink +
                           "' style='display:block;width:400px;height:60px'>join</a>"
                           "<a id=blank href='https://127.0.0.1:2/clicked'"
                           " style='display:block;width:400px;height:60px'>web</a></body>");
        MCGuiOpenFileKept(F("app-link.html"));
        gSeen.clear();
    });
    MCGuiWait("laid out", 20, [] {
        if (!loaded(1, "app-link.html")) return false;
        return laidOut();
    });
    MCGuiStep("a click on the zoommtg:// link", [] {
        countBefore();
        MCGuiClick(GTK_WIDGET(page()->webView()), 60, 30);
    });
    MCGuiWait("asks first, naming the app and the address", 10, [] {
        return page()->askingToOpen() && question() &&
               MCGuiFindLabel(question(), HtmlPage::openAppQuestion(kApp));
    });
    MCGuiStep("the Mac's words and buttons, Open then Cancel; nothing opened yet", [] {
        GtkWidget* q = question();
        GCHECK_EQ_STR(HtmlPage::openAppQuestion(kApp), "Open in Zoom Check?");
        GCHECK(MCGuiFindLabel(q, HtmlPage::openAppDetail(kAppLink)) != nullptr);
        GCHECK_EQ_STR(HtmlPage::openAppDetail(kAppLink), "The link goes to zoommtg://example, outside mutible.");
        GtkWidget* open = MCGuiFindButton(q, HtmlPage::kOpenApp);
        GtkWidget* cancel = MCGuiFindButton(q, "Cancel");
        GCHECK(open && cancel);
        GCHECK(leftOf(open, q) >= 0 && leftOf(open, q) < leftOf(cancel, q));
        GCHECK(gtk_window_get_transient_for(GTK_WINDOW(q)) == GTK_WINDOW(MCGuiApp().window));
        nothingOpened("app-link.html");
    });
    MCGuiClickWhenReady("Cancel", [] { return question() ? MCGuiFindButton(question(), "Cancel") : nullptr; });
    MCGuiWait("the question is gone", 10, noQuestion);
    // Time is the condition: an open on its way after Cancel has had its turn.
    MCGuiPause(0.3);
    MCGuiStep("Cancel opened nothing", [] {
        nothingOpened("app-link.html");
        // No window manager: the X focus stays on the question that closed.
        MCGuiFocusKeyWindow();
    });
    MCGuiStep("clicked again", [] {
        countBefore();
        MCGuiClick(GTK_WIDGET(page()->webView()), 60, 30);
    });
    MCGuiWait("asks again", 10, [] {
        return page()->askingToOpen() && question() &&
               MCGuiFindLabel(question(), HtmlPage::openAppQuestion(kApp));
    });
    MCGuiClickWhenReady("Open", [] { return question() ? MCGuiFindButton(question(), HtmlPage::kOpenApp) : nullptr; });
    MCGuiWait("Open hands the address to the desktop's handler (the recorder)", 10, [] {
        return MCGuiLaunchedUris().size() > gLaunchedBefore;
    });
    MCGuiStep("that address once, through the link handler, the page still the file", [] {
        MCGuiNote("launched %s", MCGuiLaunchedUris().back().c_str());
        GCHECK(noQuestion());
        GCHECK(MCGuiLaunchedUris().size() == gLaunchedBefore + 1);
        GCHECK_EQ_STR(MCGuiLaunchedUris().back(), kAppLink);
        GCHECK(gOpened.size() == gOpenedBefore + 1);
        GCHECK_EQ_STR(gOpened.back(), kAppLink);
        GCHECK(has(ed()->active()->path(), "/app-link.html") && ed()->active()->showsPage());
        GCHECK(has(pageUri(), "app-link.html"));
        MCGuiFocusKeyWindow();
    });
    MCGuiStep("a click on the https link", [] {
        countBefore();
        MCGuiClick(GTK_WIDGET(page()->webView()), 60, 90);
    });
    MCGuiWait("goes to the link handler, as before", 10, [] { return gOpened.size() > gOpenedBefore; });
    MCGuiStep("with no question, and nothing to the desktop's handler", [] {
        GCHECK(noQuestion());
        GCHECK(gOpened.size() == gOpenedBefore + 1);
        GCHECK_EQ_STR(gOpened.back(), "https://127.0.0.1:2/clicked");
        GCHECK(MCGuiLaunchedUris().size() == gLaunchedBefore);
        page()->linkSpy = nullptr;
    });

    // A Markdown preview's link is not the HTML page's: one to zoommtg://
    // still goes to the desktop's handler with no question (unchanged).
    MCGuiStep("a Markdown file with a zoommtg:// link", [] {
        MCGuiWriteFile(F("app-link.md"), "Before.\n\n[join](zoommtg://example/md)\n");
        MCGuiOpenFileKept(F("app-link.md"));
    });
    MCGuiWait("its preview, laid out", 20, [] {
        return ed()->active() && ed()->active()->path() == F("app-link.md") && ed()->inPreview() &&
               wordAt("join", &gWordX, &gWordY);
    });
    MCGuiStep("a click on the link", [] {
        countBefore();
        MCGuiClick(ed()->textView(), gWordX, gWordY);
    });
    MCGuiWait("goes to the desktop's handler", 10, [] { return MCGuiLaunchedUris().size() > gLaunchedBefore; });
    MCGuiStep("unasked, as before", [] {
        GCHECK(!question());
        GCHECK_EQ_STR(MCGuiLaunchedUris().back(), "zoommtg://example/md");
        GCHECK(MCGuiLaunchedUris().size() == gLaunchedBefore + 1);
    });

    MCGuiStep("(the scratch world's app and settings put back)", [] {
        if (gHadMimeApps) MCGuiWriteFile(mimeApps(), gWasMimeApps);
        else g_remove(mimeApps().c_str());
        g_remove(appFile().c_str());
        ed()->closeAllTabs();
        MCGuiSetSettings(gWasSettings);
    });
}

#else
MC_GUI_SUITE(html_page) {}
#endif
