// SessionsSuite.cpp — a folder reopens its tabs (`./build/mutible-guitest
// sessions`, linux/PARITY.md item 1.2), the counterpart of the Mac's
// MCTabSessions checks (tests/gui/PreviewTabsSuite.mm, SplitEditorSuite.mm).
// A second app opens a folder, opens its files in tabs (a Markdown page, a
// named pipe, a file over 16 MB, a file deleted later, the preview tab),
// moves the caret by key presses and closes its window with Ctrl+Shift+W;
// a third app opening the folder gets the tabs back in their order without
// the file gone, the preview tab as one, the caret where it was and on
// screen, and every tab but the one on show not read until it is shown. In
// this app, two windows on one folder share its session, and the one that
// closes while the other stays drops out of it, as on the Mac. Saves made
// at the same moment keep each other's folders.
//
// Windows (TabSessions::Window, kept in the same file under "windows"): a
// fourth app opens two windows on two folders, gives one a size and two
// tabs with the caret placed, hides the other's sidebar, and quits; a fifth
// app launched with no path from the home folder, as the dock launches it,
// gets both windows back with their own tabs, caret, size and sidebar.
//
// Pages (TabSessions::PageTab): a sixth app opens a git folder, opens a
// file, a kept commit page, a second file and the Uncommitted Changes page
// (on show), and closes its window; a seventh gets the pages back among the
// files in the same order, the review still on show, and a page the record
// holds twice is made once.
#include "Harness.h"

#include "ChangeReviewPage.h"
#include "DocumentView.h"
#include "Editor.h"
#include "GitChecks.h"
#include "GitGraph.h"
#include "GitPanel.h"
#include "Persistence.h"
#include "TabSessions.h"
#include "TabStrip.h"
#include "WindowLayout.h"

#include <glib/gstdio.h>
#include <sys/stat.h>

#include <string>
#include <thread>
#include <vector>

namespace {

std::string Folder() { return MCGuiScratch() + "/ses"; }
std::string In(const std::string& name) { return Folder() + "/" + name; }
std::string SessionsFile() { return MCGuiScratch() + "/state/mutible/sessions.json"; }

TabSessions::Session SavedFor(const std::string& folder) {
    const TabSessions::Sessions all = TabSessions::decode(MCGuiReadFile(SessionsFile()));
    const auto it = all.find(Persistence::keyFor(folder));
    return it == all.end() ? TabSessions::Session() : it->second;
}

std::string Names(const TabSessions::Session& s) {
    std::string out;
    for (size_t i = 0; i < s.tabs.size(); i++) {
        const std::string& p = s.tabs[i].path;
        std::string n = p.substr(p.find_last_of('/') + 1);
        if ((int)i == s.preview) n = "[" + n + "]";
        if ((int)i == s.active) n = ">" + n;
        out += (out.empty() ? "" : " ") + n;
    }
    return out;
}

Editor* ed() { return MCGuiApp().editor; }

// The row as the strip shows it: the preview tab's title in brackets, ">"
// before the one on show.
std::string Row() {
    std::string out;
    TabStrip* strip = ed()->strip();
    for (int i = 0; i < strip->count(); i++) {
        const TabStrip::Item& it = strip->item(i);
        std::string t = it.preview ? "[" + it.title + "]" : it.title;
        if (i == strip->activeIndex()) t = ">" + t;
        out += (out.empty() ? "" : " ") + t;
    }
    return out;
}

long Caret() {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(ed()->buffer(), &it, gtk_text_buffer_get_insert(ed()->buffer()));
    return gtk_text_iter_get_offset(&it);
}

double ScrollY() {
    return gtk_adjustment_get_value(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(ed()->textView())));
}

bool CaretOnScreen() {
    GtkTextView* tv = GTK_TEXT_VIEW(ed()->textView());
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(ed()->buffer(), &it, gtk_text_buffer_get_insert(ed()->buffer()));
    GdkRectangle vis, at;
    gtk_text_view_get_visible_rect(tv, &vis);
    gtk_text_view_get_iter_location(tv, &it, &at);
    return vis.height > 0 && vis.y > 0 && at.y < vis.y + vis.height && at.y + at.height > vis.y;
}

std::string Numbered(const char* word, int lines) {
    std::string s;
    for (int i = 1; i <= lines; i++) s += std::string(word) + " line " + std::to_string(i) + "\n";
    return s;
}

// ---- pages
std::string Pages() { return MCGuiScratch() + "/sespg"; }
std::string PagesHash() { return MCGuiScratch() + "/sespg-hash"; }
std::string FirstCommit() {
    std::string h = MCGuiReadFile(PagesHash());
    while (!h.empty() && (h.back() == '\n' || h.back() == ' ')) h.pop_back();
    return h;
}

const std::string kThree = Numbered("three", 400);
// The start of its line 391, ten lines up from its end.
const long kCaret = (long)kThree.size() - 10 * (long)std::string("three line 400\n").size();

Document* TabFor(const std::string& path) { return ed()->documentFor(path); }

size_t gWindows = 0;

// ---- windows
std::string WinA() { return MCGuiScratch() + "/win-a"; }
std::string WinB() { return MCGuiScratch() + "/win-b"; }
// The size the first app's window had as it quit, as it read it (GTK may
// or may not resize a mapped window to a new default size without a window
// manager), for the checks after it.
std::string SizeFile() { return MCGuiScratch() + "/win-a-size"; }
std::string SizeOf(GtkWindow* w) {
    int width = 0, height = 0;
    gtk_window_get_default_size(w, &width, &height);
    return std::to_string(width) + "x" + std::to_string(height);
}
const std::string kA2 = "first line\nsecond line\nthird line\nfourth line\n";
const long kA2Caret = (long)std::string("first line\nsecond line\n").size();

const TabSessions::Window* WindowOn(const std::vector<TabSessions::Window>& list,
                                    const std::string& folder) {
    for (const TabSessions::Window& w : list)
        if (w.root == Persistence::keyFor(folder)) return &w;
    return nullptr;
}

const MCGuiAppWindow* AppOn(const std::vector<MCGuiAppWindow>& list, const std::string& folder) {
    for (const MCGuiAppWindow& w : list)
        if (Persistence::keyFor(w.rootDir) == Persistence::keyFor(folder)) return &w;
    return nullptr;
}

std::string RowOf(Editor* e) {
    std::string out;
    TabStrip* strip = e->strip();
    for (int i = 0; i < strip->count(); i++) {
        std::string t = strip->item(i).title;
        if (i == strip->activeIndex()) t = ">" + t;
        out += (out.empty() ? "" : " ") + t;
    }
    return out;
}

long CaretOf(Editor* e) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(e->buffer(), &it, gtk_text_buffer_get_insert(e->buffer()));
    return gtk_text_iter_get_offset(&it);
}

}  // namespace

// ------------------------------------------------ the parent's part
MC_GUI_SUITE(sessions) {
    MCGuiStep("a folder of files: text, a Markdown page, a pipe, one over 16 MB", [] {
        MCGuiWriteFile(In("one.txt"), "one\n");
        MCGuiWriteFile(In("two.md"), "# Two\n\nSome text.\n");
        mkfifo(In("pipe").c_str(), 0600);
        MCGuiWriteFile(In("huge.log"), std::string(17 * 1024 * 1024, 'h'));
        MCGuiWriteFile(In("three.txt"), kThree);
        MCGuiWriteFile(In("gone.txt"), "gone\n");
        MCGuiWriteFile(In("four.txt"), "four\n");
        MCGuiWriteFile(In("extra.txt"), "extra\n");
    });
    MCGuiAsync("a second app opens them and closes its window", 90, [](MCGuiDone done) {
        MCGuiRunChild("_sessions_first", Folder(), [done](int status, std::string out) {
            GCHECK(status == 0);
            if (status != 0) MCGuiNote("first (%d):\n%s", status, out.c_str());
            done();
        });
    });
    MCGuiStep("the folder's session, under $XDG_STATE_HOME, by the folder's real path", [] {
        const TabSessions::Session s = SavedFor(Folder());
        MCGuiNote("saved: %s", Names(s).c_str());
        GCHECK_EQ_STR(Names(s), "one.txt two.md pipe >huge.log three.txt gone.txt [four.txt]");
        struct stat st;
        GCHECK(stat(SessionsFile().c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
        // Then gone.txt goes from disk.
        g_remove(In("gone.txt").c_str());
    });
    MCGuiAsync("a third app opens the folder again", 90, [](MCGuiDone done) {
        MCGuiRunChild("_sessions_again", Folder(), [done](int status, std::string out) {
            GCHECK(status == 0);
            if (status != 0) MCGuiNote("again (%d):\n%s", status, out.c_str());
            done();
        });
    });

    // ---- every window back on a launch that opens nothing
    MCGuiStep("two folders, for two windows", [] {
        MCGuiWriteFile(WinA() + "/a1.txt", "a1\n");
        MCGuiWriteFile(WinA() + "/a2.txt", kA2);
        MCGuiWriteFile(WinB() + "/b1.txt", "b1\n");
    });
    MCGuiAsync("a fourth app opens and arranges two windows, and quits", 90, [](MCGuiDone done) {
        MCGuiRunChild("_windows_first", WinA(), [done](int status, std::string out) {
            GCHECK(status == 0);
            if (status != 0) MCGuiNote("windows (%d):\n%s", status, out.c_str());
            done();
        });
    });
    MCGuiStep("both windows are in the file, beside the folders' sessions", [] {
        const std::string text = MCGuiReadFile(SessionsFile());
        const TabSessions::Store st = TabSessions::decodeStore(text);
        MCGuiNote("%zu windows kept", st.windows.size());
        GCHECK(st.windows.size() == 2);
        const TabSessions::Window* a = WindowOn(st.windows, WinA());
        const TabSessions::Window* b = WindowOn(st.windows, WinB());
        GCHECK(a && b);
        if (!a || !b) return;
        GCHECK_EQ_STR(Names(a->tabs), "a1.txt >a2.txt");
        GCHECK_EQ_STR(Names(b->tabs), ">b1.txt");
        GCHECK(a->tabs.tabs.size() == 2 && a->tabs.tabs[1].selStart == kA2Caret);
        const std::string size = MCGuiReadFile(SizeFile());
        MCGuiNote("A's size as it quit: %s", size.c_str());
        GCHECK(a->haveFrame);
        GCHECK_EQ_STR(std::to_string((int)a->frame.w) + "x" + std::to_string((int)a->frame.h), size);
        GCHECK(b->sidebarHidden && !a->sidebarHidden);
        // The folders' sessions still read as before, by the old reader too.
        GCHECK_EQ_STR(Names(SavedFor(WinA())), "a1.txt >a2.txt");
        GCHECK(TabSessions::decode(text).count(TabSessions::kWindowsKey) == 0);
    });
    MCGuiAsync("a fifth app, launched with no path from the home folder", 90, [](MCGuiDone done) {
        MCGuiRunChild("_windows_again", "", [done](int status, std::string out) {
            GCHECK(status == 0);
            if (status != 0) MCGuiNote("windows again (%d):\n%s", status, out.c_str());
            done();
        });
    });

    // ---- two windows on one folder, in this app
    MCGuiStep("two files in this window", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiWriteFile(MCGuiProjectFile("ses-p1.txt"), "p1\n");
        MCGuiWriteFile(MCGuiProjectFile("ses-p2.txt"), "p2\n");
        MCGuiWriteFile(MCGuiProjectFile("ses-p3.txt"), "p3\n");
        ed()->closeAllTabs();
        MCGuiOpenFileKept(MCGuiProjectFile("ses-p1.txt"));
        MCGuiOpenFileKept(MCGuiProjectFile("ses-p2.txt"));
        gWindows = MCGuiAppWindows().size();
    });
    MCGuiWait("they are the folder's session", 10, [] {
        return Names(SavedFor(MCGuiProject())) == "ses-p1.txt >ses-p2.txt";
    });
    MCGuiStep("a new window on the folder", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+shift+n");
    });
    MCGuiWait("it opens with the folder's tabs", 10, [] {
        return MCGuiAppWindows().size() == gWindows + 1 && ed()->tabCount() == 2;
    });
    MCGuiStep("the same tabs, the same on show", [] {
        GCHECK_EQ_STR(Row(), "ses-p1.txt >ses-p2.txt");
        Document* p1 = TabFor(MCGuiProjectFile("ses-p1.txt"));
        GCHECK(p1 && p1->unread());
        MCGuiOpenFileKept(MCGuiProjectFile("ses-p3.txt"));
    });
    MCGuiWait("a file only the new window has joins the session", 10, [] {
        return Names(SavedFor(MCGuiProject())) == "ses-p1.txt ses-p2.txt >ses-p3.txt" ||
               Names(SavedFor(MCGuiProject())) == "ses-p1.txt >ses-p2.txt ses-p3.txt";
    });
    MCGuiStep("the new window closes, the first staying", [] { MCGuiKey("ctrl+shift+w"); });
    MCGuiWait("one window again", 10, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("the session is the window that stayed: the other's file is out", [] {
        MCGuiFocusKeyWindow();   // as a desktop gives the window left the X focus
        const TabSessions::Session s = SavedFor(MCGuiProject());
        MCGuiNote("saved: %s", Names(s).c_str());
        GCHECK_EQ_STR(Names(s), "ses-p1.txt >ses-p2.txt");
        GCHECK_EQ_STR(Row(), "ses-p1.txt >ses-p2.txt");
    });

    // ---- several processes saving at once
    // Every mutible saves into the one file, reading it and writing it
    // back. Threads stand in for processes here (flock tells each open of
    // the lock file apart, within one process too): each saves its own
    // folder over and over, and no save may write over another's.
    MCGuiStep("saves made at the same moment keep each other's folders", [] {
        constexpr int kSavers = 6, kSaves = 15;
        auto key = [](int i) { return "/saver-" + std::to_string(i); };
        std::vector<std::thread> savers;
        for (int i = 0; i < kSavers; i++)
            savers.emplace_back([i, key] {
                TabSessions::Session s;
                TabSessions::Tab t;
                t.path = key(i) + "/file.txt";
                s.tabs.push_back(t);
                for (int n = 0; n < kSaves; n++) Persistence::putSession(key(i), s);
            });
        for (std::thread& t : savers) t.join();
        const TabSessions::Sessions all = TabSessions::decode(MCGuiReadFile(SessionsFile()));
        int kept = 0;
        for (int i = 0; i < kSavers; i++) kept += (int)all.count(key(i));
        GCHECK_EQ_STR(std::to_string(kept), std::to_string(kSavers));
        GCHECK(all.count(Persistence::keyFor(MCGuiProject())) == 1);
        for (int i = 0; i < kSavers; i++) Persistence::putSession(key(i), TabSessions::Session());
    });

    // ---- the review and a commit's page
    gitchecks::Shell("the repository",
                     "set -e; mkdir -p '" + Pages() + "'; cd '" + Pages() + "'; " + gitchecks::G() +
                         " init -q -b main; printf 'a\\n' > a.txt; printf 'b\\n' > b.txt; " +
                         gitchecks::G() + " add -A; " + gitchecks::G() + " commit -qm first; " +
                         gitchecks::G() + " rev-list --max-parents=0 HEAD > '" + PagesHash() +
                         "'; printf 'changed\\n' >> a.txt");
    MCGuiAsync("a sixth app opens the pages and closes its window", 90, [](MCGuiDone done) {
        MCGuiRunChild("_pages_first", Pages(), [done](int status, std::string out) {
            GCHECK(status == 0);
            if (status != 0) MCGuiNote("pages (%d):\n%s", status, out.c_str());
            done();
        });
    });
    MCGuiStep("the session keeps both pages, the files between them, and the one on show", [] {
        const TabSessions::Session s = SavedFor(Pages());
        GCHECK(s.tabs.size() == 2 && s.pages.size() == 2);
        if (s.pages.size() != 2) return;
        GCHECK_EQ_STR(s.pages[0].kind + " at " + std::to_string(s.pages[0].at), "commit at 1");
        GCHECK_EQ_STR(s.pages[0].title, Git::shortHash(FirstCommit()) + " first");
        GCHECK_EQ_STR(s.pages[1].kind + " at " + std::to_string(s.pages[1].at), "review at 3");
        GCHECK_EQ_STR(std::to_string(s.activePage), "1");
        // A second record of the same commit and of the review, as a
        // crashed save or an older copy may leave.
        TabSessions::Session twice = s;
        twice.pages.push_back(s.pages[0]);
        twice.pages.push_back(s.pages[1]);
        Persistence::putSession(Persistence::keyFor(Pages()), twice);
    });
    MCGuiAsync("a seventh app opens the folder again", 90, [](MCGuiDone done) {
        MCGuiRunChild("_pages_again", Pages(), [done](int status, std::string out) {
            GCHECK(status == 0);
            if (status != 0) MCGuiNote("pages again (%d):\n%s", status, out.c_str());
            done();
        });
    });
}

// ----------------------------------- the first child: open, move, close
MC_GUI_SUITE(_sessions_first) {
    MCGuiStep("the files in tabs, the last in the preview tab", [] {
        GCHECK(MCGuiApp().rootDir == Folder());
        GCHECK(ed()->tabCount() == 0);
        for (const char* f : {"one.txt", "two.md", "pipe", "huge.log", "three.txt", "gone.txt"})
            MCGuiOpenFileKept(In(f));
        MCGuiOpenFile(In("four.txt"));
    });
    MCGuiWait("all seven", 10, [] {
        return Row() == "one.txt two.md pipe huge.log three.txt gone.txt >[four.txt]";
    });
    MCGuiStep("three.txt, the caret to its end by key presses", [] {
        MCGuiKey("alt+5");
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+End");
    });
    MCGuiWait("there", 10, [] {
        return ed()->currentPath() == In("three.txt") && Caret() == (long)kThree.size();
    });
    // GtkTextView scrolls to the end of a file it has not laid out yet by
    // guessed line heights; once it has, a step up and back down brings the
    // caret on screen, as it would for a person.
    MCGuiWaitQuiet("the view settles", 0.4, 5, [] { return (long)ScrollY(); });
    MCGuiStep("up and down again, then ten lines up", [] {
        MCGuiKey("Up");
        MCGuiKey("Down");
        for (int i = 0; i < 10; i++) MCGuiKey("Up");
    });
    MCGuiWait("the caret on line 391, on screen", 10, [] {
        return Caret() == kCaret && CaretOnScreen();
    });
    // Over 16 MB: read in the background (item 1.12), and never at launch.
    MCGuiStep("huge.log on show, a file read in the background", [] { MCGuiKey("alt+4"); });
    MCGuiWait("on show, and in", 30, [] {
        return ed()->currentPath() == In("huge.log") && ed()->active() &&
               !ed()->active()->loading() && ed()->isSource();
    });
    MCGuiStep("Ctrl+Shift+W", [] {
        GCHECK(ed()->active() && ed()->active()->kind() == Document::Kind::Text);
        MCGuiKey("ctrl+shift+w");
    });
    MCGuiWait("the window closes", 10, [] { return MCGuiAppWindows().empty(); });
}

// ------------------------------------------ the second child: the tabs back
MC_GUI_SUITE(_sessions_again) {
    MCGuiWait("the window has the tabs back", 10, [] { return ed() && ed()->tabCount() == 6; });
    MCGuiStep("in their order, the file gone left out, the preview tab as one", [] {
        MCGuiNote("row: %s", Row().c_str());
        // huge.log was on show; a file over 16 MB is never read at launch,
        // and the pipe beside it is no plain file, so three.txt is shown.
        GCHECK_EQ_STR(Row(), "one.txt two.md pipe huge.log >three.txt [four.txt]");
        GCHECK(TabFor(In("gone.txt")) == nullptr);
        // Only the tab on show was read.
        for (const char* f : {"one.txt", "two.md", "pipe", "huge.log", "four.txt"})
            GCHECK(TabFor(In(f)) && TabFor(In(f))->unread());
        GCHECK(TabFor(In("three.txt")) && !TabFor(In("three.txt"))->unread());
        GCHECK(!ed()->dirty());
    });
    MCGuiWait("three.txt with the caret where it was, on screen", 10, [] {
        return Caret() == kCaret && CaretOnScreen();
    });
    MCGuiStep("two.md", [] { MCGuiKey("alt+2"); });
    MCGuiWait("read as it is shown, its page as it was", 10, [] {
        return ed()->currentPath() == In("two.md") && TabFor(In("two.md")) &&
               !TabFor(In("two.md"))->unread() &&
               ed()->inPreview();
    });
    MCGuiStep("the pipe", [] { MCGuiKey("alt+3"); });
    MCGuiWait("shown without opening it: a message", 10, [] {
        return ed()->currentPath() == In("pipe") && ed()->active() &&
               ed()->active()->kind() == Document::Kind::Message &&
               !ed()->active()->unread() &&
               MCGuiEditorText().find("Not a regular file") != std::string::npos;
    });
    MCGuiStep("a file opened in preview", [] { MCGuiOpenFile(In("extra.txt")); });
    MCGuiWait("replaces the preview tab that came back", 10, [] {
        return Row() == "one.txt two.md pipe >[extra.txt] huge.log three.txt";
    });
}

// -------------------------------- the pages' first app: open them, a quit
MC_GUI_SUITE(_pages_first) {
    MCGuiStep("a file, a kept commit page, a file, then the review page", [] {
        GCHECK(MCGuiApp().rootDir == Pages());
        GCHECK(ed()->tabCount() == 0);
        MCGuiOpenFileKept(Pages() + "/a.txt");
        MCGuiApp().git->onReviewCommit(FirstCommit(), "first", false);
        MCGuiOpenFileKept(Pages() + "/b.txt");
        MCGuiApp().git->onReview("", false, false);
    });
    MCGuiWait("all four, the review on show", 15, [] {
        return Row() == "a.txt " + Git::shortHash(FirstCommit()) + " first b.txt >Uncommitted Changes";
    });
    MCGuiStep("Ctrl+Shift+W", [] { MCGuiKey("ctrl+shift+w"); });
    MCGuiWait("the window closes", 10, [] { return MCGuiAppWindows().empty(); });
}

// ------------------------------------ the pages' second app: pages back
MC_GUI_SUITE(_pages_again) {
    MCGuiWait("the window has the four tabs back", 15, [] { return ed() && ed()->tabCount() == 4; });
    MCGuiStep("in their order, the review on show, no page made twice", [] {
        MCGuiNote("row: %s", Row().c_str());
        GCHECK_EQ_STR(Row(), "a.txt " + Git::shortHash(FirstCommit()) + " first b.txt >Uncommitted Changes");
        GCHECK(ed()->currentPage() && dynamic_cast<ChangeReviewPage*>(ed()->currentPage()));
        GCHECK(ed()->active() && ed()->active()->editorPage());
    });
    MCGuiWait("the review read git", 15, [] {
        auto* p = dynamic_cast<ChangeReviewPage*>(ed()->currentPage());
        return p && p->loaded() && p->fileCount() == 1;
    });
    MCGuiStep("alt+2 shows the commit's page, read as well", [] { MCGuiKey("alt+2"); });
    MCGuiWait("its two files", 15, [] {
        auto* p = dynamic_cast<ChangeReviewPage*>(ed()->currentPage());
        return p && p->isCommit() && p->loaded() && p->fileCount() == 2;
    });
}

// ------------------- the windows' first app: two windows arranged, a quit
MC_GUI_SUITE(_windows_first) {
    MCGuiStep("two files here", [] {
        GCHECK(MCGuiApp().rootDir == WinA());
        MCGuiOpenFileKept(WinA() + "/a1.txt");
        MCGuiOpenFileKept(WinA() + "/a2.txt");
    });
    MCGuiWait("a2.txt on show", 10, [] { return ed()->currentPath() == WinA() + "/a2.txt"; });
    MCGuiStep("the caret to its third line, and the window 900 by 640", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+Home");
        MCGuiKey("Down");
        MCGuiKey("Down");
        gtk_window_set_default_size(MCGuiApp().window, 900, 640);
    });
    MCGuiWait("the caret there", 10, [] { return Caret() == kA2Caret; });
    MCGuiStep("a second window, on the other folder", [] { MCGuiOpenWindow(WinB()); });
    MCGuiWait("it is up", 10, [] {
        return MCGuiAppWindows().size() == 2 && MCGuiApp().rootDir == WinB() &&
               gtk_widget_get_mapped(GTK_WIDGET(MCGuiApp().window));
    });
    MCGuiStep("b1.txt in it, and its sidebar hidden", [] {
        MCGuiSetKeyWindow(MCGuiApp().window);
        MCGuiOpenFileKept(WinB() + "/b1.txt");
        if (MCGuiApp().layout->sidebarShown()) MCGuiApp().layout->toggleSidebar();
    });
    MCGuiWait("so", 10, [] {
        return ed()->currentPath() == WinB() + "/b1.txt" && !MCGuiApp().layout->sidebarShown();
    });
    MCGuiStep("A's size noted, then quit", [] {
        const std::vector<MCGuiAppWindow> all = MCGuiAppWindows();
        const MCGuiAppWindow* a = AppOn(all, WinA());
        GCHECK(a != nullptr);
        if (a) MCGuiWriteFile(SizeFile(), SizeOf(a->window));
        MCGuiKey("ctrl+q");
    });
    MCGuiWait("every window closes", 15, [] { return MCGuiAppWindows().empty(); });
}

// ------------- the windows' second app: launched from home with no path
MC_GUI_SUITE(_windows_again) {
    MCGuiWait("both windows are back", 20, [] {
        const std::vector<MCGuiAppWindow> all = MCGuiAppWindows();
        return all.size() == 2 && AppOn(all, WinA()) && AppOn(all, WinB());
    });
    MCGuiWait("each with its own tabs, and the caret where it was", 10, [] {
        const std::vector<MCGuiAppWindow> all = MCGuiAppWindows();
        const MCGuiAppWindow* a = AppOn(all, WinA());
        const MCGuiAppWindow* b = AppOn(all, WinB());
        return a && b && RowOf(a->editor) == "a1.txt >a2.txt" && RowOf(b->editor) == ">b1.txt" &&
               CaretOf(a->editor) == kA2Caret;
    });
    MCGuiStep("so, and at the size it had", [] {
        const std::vector<MCGuiAppWindow> all = MCGuiAppWindows();
        const MCGuiAppWindow* a = AppOn(all, WinA());
        const MCGuiAppWindow* b = AppOn(all, WinB());
        GCHECK(a && b);
        if (!a || !b) return;
        MCGuiNote("A: %s; B: %s", RowOf(a->editor).c_str(), RowOf(b->editor).c_str());
        GCHECK_EQ_STR(RowOf(a->editor), "a1.txt >a2.txt");
        GCHECK_EQ_STR(RowOf(b->editor), ">b1.txt");
        GCHECK(CaretOf(a->editor) == kA2Caret);
        MCGuiNote("A's size: %s", SizeOf(a->window).c_str());
        GCHECK_EQ_STR(SizeOf(a->window), MCGuiReadFile(SizeFile()));
        // Not the home folder this app was started in.
        for (const MCGuiAppWindow& x : all) GCHECK(x.rootDir != std::string(g_get_home_dir()));
    });
    MCGuiWait("B's sidebar hidden, A's shown", 10, [] {
        const std::vector<MCGuiAppWindow> all = MCGuiAppWindows();
        const MCGuiAppWindow* a = AppOn(all, WinA());
        const MCGuiAppWindow* b = AppOn(all, WinB());
        return a && b && a->layout->sidebarShown() && !b->layout->sidebarShown();
    });
}
