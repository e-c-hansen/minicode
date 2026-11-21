// TreeDragsSuite.cpp — tree_drags: a row dragged from the file tree onto the
// terminal and onto the editor (linux/HANDOFF.md item 119; the Mac's #212,
// checked there by terminal_paste). Real XTest drags in the harness's Xvfb,
// so GTK's own drag source and the drop targets' XDND exchange both run:
//
//   - a file onto the terminal types its path at the prompt, quoted as the
//     Mac quotes a dropped path (TermDrop::paths: backslashes, a space
//     after), as a paste, and the terminal takes the keyboard;
//   - a file onto the editor opens it in a tab of its own, as VS Code does,
//     the text under the pointer left alone (GtkTextView's own handling
//     typed the file's path into it);
//   - a folder onto the editor is shown in the tree, nothing opened.
//
// A row dragged onto a folder row of the tree itself (HANDOFF item 13) is
// not checked here: this drag, two rows up the same list, never landed,
// before this change as after it (HANDOFF item 119).
#include "Harness.h"

#include "Editor.h"
#include "FileTree.h"
#include "TermDrop.h"
#include "WindowLayout.h"
#ifdef MUTIBLE_ENABLE_TERMINAL
#include "Terminal.h"
#include "TerminalDock.h"
#endif

#include <string>
#include <utility>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& name) { return MCGuiProjectFile("drags/" + name); }
const std::string kHost = "host text\nsecond line\n";

std::string bufferOf(const std::string& path) {
    Document* d = ed()->documentFor(path);
    if (!d) return "<no tab>";
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(d->buffer(), &a, &b);
    char* t = gtk_text_buffer_get_text(d->buffer(), &a, &b, TRUE);
    std::string s = t ? t : "";
    g_free(t);
    return s;
}

// A drag from the middle of `path`'s tree row to the middle of `target`,
// lingering over the target so the XDND exchange has turns to answer
// before the button goes up, as a hand's does.
void dragRowTo(const std::string& path, std::function<GtkWidget*()> target) {
    MCGuiBringTreeRow(path);
    MCGuiWait("the row and the target on screen", 10, [path, target] {
        GtkWidget* row = MCGuiTreeRow(path);
        GtkWidget* to = target();
        return row && to && MCGuiOnScreen(row) && MCGuiOnScreen(to);
    });
    MCGuiStep("the row dragged onto the target", [path, target] {
        GtkWidget* row = MCGuiTreeRow(path);
        GtkWidget* to = target();
        if (!row || !to) return;
        graphene_point_t mid = GRAPHENE_POINT_INIT((float)gtk_widget_get_width(to) / 2,
                                                   (float)gtk_widget_get_height(to) / 2);
        graphene_point_t at;
        if (!gtk_widget_compute_point(to, row, &mid, &at)) {
            GCHECK(!"the target's middle in the row's coordinates");
            return;
        }
        const double x0 = 12, y0 = gtk_widget_get_height(row) / 2.0;
        MCGuiDragThrough(row, {{x0, y0}, {x0 + 12, y0 + 2}, {(x0 + at.x) / 2, (y0 + at.y) / 2},
                               {at.x, at.y}, {at.x + 3, at.y + 2}, {at.x, at.y}});
    });
    MCGuiWait("the drag played", 10, [] { return !MCGuiInputPending(); });
}

#ifdef MUTIBLE_ENABLE_TERMINAL
Terminal* term() { return MCGuiApp().terminal; }
bool ready(Terminal* t) {
    return t && t->shellPid() > 0 && t->shellState().known && t->shellState().atPrompt;
}
#endif

}  // namespace

MC_GUI_SUITE(tree_drags) {
    MCGuiHideBrowserPane();
    MCGuiStep("files to drag, a tab on show", [] {
        MCGuiSetKeyWindow(nullptr);
        ed()->closeAllTabs();
        MCGuiWriteFile(F("a b's.txt"), "dragged\n");
        MCGuiWriteFile(F("plain.txt"), "plain\n");
        MCGuiWriteFile(F("folder/inside.txt"), "inside\n");
        MCGuiWriteFile(F("host.txt"), kHost);
        MCGuiOpenFileKept(F("host.txt"));
    });
    MCGuiWait("host.txt on show", 10, [] { return ed()->currentPath() == F("host.txt"); });

#ifdef MUTIBLE_ENABLE_TERMINAL
    MCGuiStep("the terminal shown", [] {
        if (!MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
    });
    MCGuiWait("its shell at a prompt", 30, [] { return ready(term()); });
    dragRowTo(F("a b's.txt"), [] { return term() ? term()->vteWidget() : nullptr; });
    MCGuiWait("the quoted path typed at the prompt", 10, [] {
        return term() && term()->recentText(4).find(TermDrop::quotePath(F("a b's.txt"))) !=
                             std::string::npos;
    });
    MCGuiStep("as one word, a space after it; the terminal has the keyboard; nothing opened", [] {
        const std::string text = term()->recentText(4);
        MCGuiNote("terminal: %s", text.c_str());
        GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(MCGuiApp().window));
        GCHECK(term()->owns(focus));
        GCHECK_EQ_STR(ed()->currentPath(), F("host.txt"));
        GCHECK(ed()->documentFor(F("a b's.txt")) == nullptr);
        MCGuiType("X");   // the space after the path, which the screen's text trims
    });
    MCGuiWait("the path, a space, then what is typed next", 10, [] {
        return term()->recentText(4).find(TermDrop::paths({F("a b's.txt")}) + "X") !=
               std::string::npos;
    });
    MCGuiClearTerminalLine("a\\ b");
#endif

    MCGuiStep("the editor has the keyboard", [] {
        MCGuiFocusKeyWindow();
        ed()->focusActiveDocument();
    });
    dragRowTo(F("plain.txt"), [] { return ed()->textView(); });
    MCGuiWait("plain.txt opens in the editor", 10, [] {
        return ed()->currentPath() == F("plain.txt");
    });
    MCGuiStep("in a tab of its own; host.txt's text untouched", [] {
        MCGuiNote("host.txt: %s", bufferOf(F("host.txt")).c_str());
        GCHECK_EQ_STR(bufferOf(F("host.txt")), kHost);
        GCHECK(ed()->documentFor(F("host.txt")) != nullptr);
        Document* d = ed()->documentFor(F("plain.txt"));
        GCHECK(d && !d->isPreviewTab());
        MCGuiOpenFileKept(F("host.txt"));
    });
    MCGuiWait("host.txt on show again", 10, [] { return ed()->currentPath() == F("host.txt"); });
    dragRowTo(F("folder"), [] { return ed()->textView(); });
    MCGuiWait("the folder selected in the tree", 10, [] {
        return MCGuiApp().tree->selectedPath() == F("folder");
    });
    MCGuiStep("nothing opened, host.txt's text untouched", [] {
        GCHECK_EQ_STR(ed()->currentPath(), F("host.txt"));
        GCHECK_EQ_STR(bufferOf(F("host.txt")), kHost);
        GCHECK(ed()->documentFor(F("folder")) == nullptr);
    });

    // Each drop above came from the app's own drag, whose XdndDrop carries
    // no real time with GTK 4.22 (Input.cpp): GDK's user time may be left
    // days ahead of the X server's, and every window presented after it
    // came up without the keyboard (preview_tabs' and replace_in_folder's).
    MCGuiStep("the tabs closed, and GDK's user time put back", [] {
        ed()->closeAllTabs();
        MCGuiPutBackUserTime();
        GCHECK(!MCGuiUserTimeAhead());
    });
}
