// SessionsSuite.cpp — a folder reopens its tabs (`./build/minicode-guitest
// sessions`, linux/PARITY.md item 1.2), the counterpart of the Mac's
// MCTabSessions checks (tests/gui/PreviewTabsSuite.mm, SplitEditorSuite.mm).
// A second app opens a folder, opens its files in tabs (a Markdown page, a
// named pipe, a file over 16 MB, a file deleted later, the preview tab),
// moves the caret by key presses and closes its window with Ctrl+Shift+W;
// a third app opening the folder gets the tabs back in their order without
// the file gone, the preview tab as one, the caret where it was and on
// screen, and every tab but the one on show not read until it is shown. In
// this app, two windows on one folder share its session, and the one that
// closes while the other stays drops out of it, as on the Mac.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "Persistence.h"
#include "TabSessions.h"
#include "TabStrip.h"

#include <glib/gstdio.h>
#include <sys/stat.h>

#include <string>

namespace {

std::string Folder() { return MCGuiScratch() + "/ses"; }
std::string In(const std::string& name) { return Folder() + "/" + name; }
std::string SessionsFile() { return MCGuiScratch() + "/state/minicode/sessions.json"; }

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

const std::string kThree = Numbered("three", 400);
// The start of its line 391, ten lines up from its end.
const long kCaret = (long)kThree.size() - 10 * (long)std::string("three line 400\n").size();

Document* TabFor(const std::string& path) { return ed()->documentFor(path); }

size_t gWindows = 0;

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
        const TabSessions::Session s = SavedFor(MCGuiProject());
        MCGuiNote("saved: %s", Names(s).c_str());
        GCHECK_EQ_STR(Names(s), "ses-p1.txt >ses-p2.txt");
        GCHECK_EQ_STR(Row(), "ses-p1.txt >ses-p2.txt");
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
