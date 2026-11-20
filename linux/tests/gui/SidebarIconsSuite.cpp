// SidebarIconsSuite.cpp — `./build/mutible-guitest sidebar_icons`: the
// sidebar's row of panel icons (SidebarIcons.h, the core's SidebarPanels.h,
// workbench.sidebar-icons; the Mac's sidebar_icons suite, #225), in a window
// of its own on a scratch repository with real git:
//   - four icons above the panels, File Tree, Source Control, Agents and
//     Processes, each from the icon theme, never the keyboard's; each
//     tooltip names its panel and its key as the registry and keys.conf
//     spell it ("Source Control (Ctrl+Shift+G)"); the tree's marked;
//   - Source Control's badge follows a new file, then a second, with no git
//     of its own (the tree's status);
//   - a real click on each icon shows its panel, marked; a click on the
//     icon on show gives that panel the keyboard; the tree's icon gives the
//     tree back;
//   - Agents' dot while a terminal of this window waits, not the first
//     window's, and gone once a key is typed there;
//   - Processes' badge counts a server listening in the folder at the
//     monitor's next look, and goes when the server stops;
//   - Toggle Sidebar (its key) hides the row with the sidebar, and
//     workbench.sidebar-icons = off hides it alone, the panels taking the
//     sidebar's whole height.
#include "GitChecks.h"

#include "FileTree.h"
#include "Json.h"
#include "Keys.h"
#include "ProcessMonitor.h"
#include "SidebarIcons.h"
#include "SidebarPanels.h"
#include "Terminal.h"
#include "TerminalAttention.h"
#include "TreeGitColors.h"
#include "WindowLayout.h"

#include <csignal>
#include <cstdlib>
#include <unistd.h>
#include <string>

using namespace gitchecks;

namespace {

using P = SidebarPanels::Panel;   // the namespace above has a Panel() of its own

const std::string kSettings = "lsp.enabled = false\n";

SidebarIcons* Icons() { return RepoWindow().sidebarIcons; }
GtkWidget* Button(P p) { return Icons() ? Icons()->button(p) : nullptr; }
std::string Badge(P p) { return Icons() ? Icons()->badge(p) : std::string("(no row)"); }

// What the stack of panels shows: "tree", "git", "agents", "processes".
std::string StackShows() {
    GtkWidget* stack = RepoWindow().sidebarStack;
    const char* n = stack ? gtk_stack_get_visible_child_name(GTK_STACK(stack)) : nullptr;
    return n ? n : "";
}

bool Shows(P p, const char* child) {
    const MCGuiAppWindow w = RepoWindow();
    return Icons() && Icons()->current() == p && StackShows() == child &&
           w.gitShown == (p == P::SourceControl) && w.agentsShown == (p == P::Agents) &&
           w.procsShown == (p == P::Processes);
}

// Only the icon on show has the accent line.
bool MarkedAlone(P p) {
    if (!Icons()) return false;
    for (int i = 0; i < SidebarPanels::kCount; i++)
        if ((bool)gtk_widget_get_visible(Icons()->markWidget((P)i)) != (i == (int)p)) return false;
    return gtk_widget_has_css_class(Button(p), "current");
}

bool FocusIn(GtkWidget* w) {
    GtkWidget* f = gtk_root_get_focus(GTK_ROOT(RepoWindow().window));
    return w && f && (f == w || gtk_widget_is_ancestor(f, w));
}

// The window's terminal's id (the board marks terminals by it).
int TerminalId() {
    Terminal* t = RepoWindow().terminal;
    return t ? t->controlId() : 0;
}

// A widget's top in the window.
double TopOf(GtkWidget* w) {
    graphene_point_t in = GRAPHENE_POINT_INIT(0, 0), out;
    if (!w || !gtk_widget_compute_point(w, GTK_WIDGET(RepoWindow().window), &in, &out)) return -1;
    return out.y;
}

int gServerPid = 0;
bool gKeysBefore = false;   // keys.conf as the suite found it
std::string gKeysText;

void KillServer() {
    if (gServerPid > 0) kill(gServerPid, SIGTERM);   // the suite's own server, by its pid
}

}  // namespace

MC_GUI_SUITE(sidebar_icons) {
    if (GitPath().empty()) {
        MCGuiStep("no git", [] { MCGuiNote("no git here: the sidebar icon checks are skipped"); });
        return;
    }
    const std::string repo = MCGuiScratch() + "/sidebaricons";
    MCGuiStep("the settings", [] { MCGuiSetSettings(kSettings); });
    Shell("a repository with one commit",
          "set -e; rm -rf '" + repo + "'; mkdir -p '" + repo + "'; cd '" + repo +
              "'; echo alpha > a.txt; " + G() + " init -q -b main; " + G() + " add -A; " + G() +
              " commit -qm first");
    OpenRepoWindow(repo, false);
    MCGuiWait("the row on screen, and the tree's first look", 15, [] {
        TreeGitColors* c = RepoWindow().treeColors;
        return Icons() && MCGuiOnScreen(Button(P::Processes)) && c && c->runs() > 0 &&
               c->inRepository();
    });

    // ---- what the row shows
    MCGuiStep("four icons over the panels, the tree's marked, each tip its panel and key", [] {
        const MCGuiAppWindow w = RepoWindow();
        for (int i = 0; i < SidebarPanels::kCount; i++) {
            const P p = (P)i;
            GtkWidget* b = Button(p);
            GCHECK(MCGuiOnScreen(b));
            GCHECK(!gtk_widget_get_focusable(b) && !gtk_widget_get_focus_on_click(b));
            // A name the icon theme has, not GTK's missing picture.
            GCHECK(Icons()->iconFound(p));
            MCGuiNote("%s: %s, tip \"%s\"", SidebarPanels::info(p).name, Icons()->iconName(p),
                      Icons()->tip(p).c_str());
            const char* gtkTip = gtk_widget_get_tooltip_text(b);
            GCHECK_EQ_STR(std::string(gtkTip ? gtkTip : ""), Icons()->tip(p));
        }
        // Left to right in the panels' order.
        double x = -1;
        for (int i = 0; i < SidebarPanels::kCount; i++) {
            graphene_point_t in = GRAPHENE_POINT_INIT(0, 0), out;
            GCHECK(gtk_widget_compute_point(Button((P)i), GTK_WIDGET(w.window), &in, &out));
            GCHECK(out.x > x);
            x = out.x;
        }
        GCHECK_EQ_STR(Icons()->tip(P::Tree), "File Tree");
        // The keys as the registry gives them on Linux, keys.conf applied.
        const std::string scm = Keys::label("workbench.view.scm", "+");
        GCHECK_EQ_STR(scm, "Ctrl+Shift+G");
        GCHECK_EQ_STR(Icons()->tip(P::SourceControl), "Source Control (" + scm + ")");
        const std::string agents = Keys::label("mutible.toggleAgents", "+");
        GCHECK_EQ_STR(agents, "Ctrl+Shift+A");
        GCHECK_EQ_STR(Icons()->tip(P::Agents), "Agents (" + agents + ")");
        // Toggle Processes has no key on Linux (the Mac's Shift+Cmd+J is
        // not carried over), so its tip is the name alone, as the hints
        // and the palette show it with none.
        const std::string procs = Keys::label("mutible.toggleProcesses", "+");
        GCHECK_EQ_STR(Icons()->tip(P::Processes),
                      procs.empty() ? std::string("Processes") : "Processes (" + procs + ")");
        // The tree on show, marked alone; nothing to count yet.
        GCHECK(Shows(P::Tree, "tree"));
        GCHECK(MarkedAlone(P::Tree));
        GCHECK_EQ_STR(Badge(P::SourceControl), "");
        GCHECK(!Icons()->dot(P::Agents));
        // The row above the panels, inside the sidebar, 28 px tall.
        GCHECK(TopOf(w.sidebarIconsRow) >= 0 &&
               TopOf(w.sidebarIconsRow) < TopOf(w.sidebarStack));
        GCHECK(gtk_widget_is_ancestor(w.sidebarIconsRow, w.layout->sidebar()));
        const int h = gtk_widget_get_height(w.sidebarIconsRow);
        MCGuiNote("row %d px tall, stack at %.0f", h, TopOf(w.sidebarStack));
        GCHECK(h >= 26 && h <= 34);
    });

    // ---- the tips follow keys.conf
    MCGuiStep("keys.conf gives Toggle Processes a key and takes Source Control's", [] {
        gKeysBefore = MCGuiFileExists(Keys::path());
        gKeysText = MCGuiReadFile(Keys::path());
        MCGuiSetKeys("ctrl+alt+j = mutible.toggleProcesses\nctrl+shift+g = -workbench.view.scm\n");
    });
    MCGuiWait("the tips say so", 10, [] {
        return Icons()->tip(P::Processes) == "Processes (Ctrl+Alt+J)" &&
               Icons()->tip(P::SourceControl) == "Source Control";
    });
    // As it was: no file where there was none (keys counts on writing the
    // first, and an empty file reads the same as none to the harness).
    MCGuiStep("keys.conf as it was", [] {
        if (gKeysBefore) {
            MCGuiSetKeys(gKeysText);
        } else {
            unlink(Keys::path().c_str());
            MCGuiReloadKeys();
        }
    });
    MCGuiWait("the tips as they were", 10, [] {
        return Icons()->tip(P::SourceControl) == "Source Control (Ctrl+Shift+G)" &&
               Icons()->tip(P::Processes) ==
                   SidebarPanels::tip(P::Processes, Keys::label("mutible.toggleProcesses", "+"), false);
    });

    // ---- Source Control's count: the tree's git status
    MCGuiStep("a new file in the repository, as a terminal would make it", [] {
        MCGuiWriteFile(In("new.txt"), "n\n");
    });
    MCGuiWait("Source Control's badge says 1", 15, [] { return Badge(P::SourceControl) == "1"; });
    MCGuiStep("its pill shows the count", [] {
        GtkWidget* pill = Icons()->badgeWidget(P::SourceControl);
        GCHECK(gtk_widget_get_visible(pill) && gtk_widget_get_mapped(pill));
        GCHECK_EQ_STR(gtk_label_get_text(GTK_LABEL(pill)), "1");
        GCHECK(RepoWindow().treeColors->changeCount() == 1);
        MCGuiWriteFile(In("second.txt"), "s\n");
    });
    MCGuiWait("a second: 2", 15, [] { return Badge(P::SourceControl) == "2"; });

    // ---- each icon's click, for real
    MCGuiStep("a click on Source Control", [] { MCGuiClick(Button(P::SourceControl)); });
    MCGuiWait("the Source Control panel shows, its icon marked", 10, [] {
        return Shows(P::SourceControl, "git") && MarkedAlone(P::SourceControl);
    });
    MCGuiStep("the keyboard leaves the panel for a file's text", [] {
        GCHECK(FocusIn(RepoWindow().git->widget()));   // the toggle gave it the panel
        MCGuiOpenFileKept(In("a.txt"));
        gtk_widget_grab_focus(RepoWindow().editor->textView());
        GCHECK(!FocusIn(RepoWindow().git->widget()));
    });
    MCGuiStep("a click on the icon on show", [] { MCGuiClick(Button(P::SourceControl)); });
    MCGuiWait("the panel stays and has the keyboard", 10, [] {
        return Shows(P::SourceControl, "git") && FocusIn(RepoWindow().git->widget());
    });
    MCGuiStep("a click on Agents", [] { MCGuiClick(Button(P::Agents)); });
    MCGuiWait("the Agents panel, Source Control closed", 10, [] {
        return Shows(P::Agents, "agents") && MarkedAlone(P::Agents);
    });
    MCGuiStep("a click on Processes", [] { MCGuiClick(Button(P::Processes)); });
    MCGuiWait("the Processes panel", 10, [] {
        return Shows(P::Processes, "processes") && MarkedAlone(P::Processes);
    });
    MCGuiStep("a click on File Tree", [] { MCGuiClick(Button(P::Tree)); });
    MCGuiWait("the tree back, marked", 10, [] {
        return Shows(P::Tree, "tree") && MarkedAlone(P::Tree);
    });
    // A key toggle marks its icon too (the panel's own toggle, no click).
    MCGuiStep("Ctrl+Shift+G", [] { MCGuiKey("ctrl+shift+g"); });
    MCGuiWait("Source Control marked", 10, [] {
        return Shows(P::SourceControl, "git") && MarkedAlone(P::SourceControl);
    });
    MCGuiStep("Ctrl+Shift+G again", [] { MCGuiKey("ctrl+shift+g"); });
    MCGuiWait("the tree marked", 10, [] { return Shows(P::Tree, "tree") && MarkedAlone(P::Tree); });

    // ---- Agents' dot: a terminal of this window waiting
    MCGuiStep("a notify marks this window's terminal waiting", [] {
        GCHECK(TerminalId() != 0);
        Json args;
        GCHECK(Json::parse("{\"state\":\"waiting\",\"message\":\"Claude needs your permission\"}", args));
        std::string error;
        GCHECK(TerminalAttention::apply(TerminalId(), args, &error));
        if (!error.empty()) MCGuiNote("notify: %s", error.c_str());
    });
    MCGuiWait("the orange dot on Agents", 10, [] {
        return Icons()->dot(P::Agents) && gtk_widget_get_mapped(Icons()->dotWidget(P::Agents));
    });
    MCGuiStep("the first window's Agents icon has none", [] {
        // A picture to look at (MC_GUI_SNAPSHOTS): the count and the dot.
        MCGuiSnapshot(RepoWindow().layout->sidebar(), "sidebar-icons");
        const std::vector<MCGuiAppWindow> all = MCGuiAppWindows();
        GCHECK(all.size() >= 2);
        if (all.size() >= 2 && all[0].sidebarIcons) {
            GCHECK(all[0].rootDir != Repo());
            GCHECK(!all[0].sidebarIcons->dot(P::Agents));
        }
        // A key typed in the terminal clears the mark.
        TerminalAttention::typed(TerminalId());
    });
    MCGuiWait("the dot goes", 10, [] {
        return !Icons()->dot(P::Agents) && !gtk_widget_get_visible(Icons()->dotWidget(P::Agents));
    });

    // ---- Processes' count: a server listening in the folder
    MCGuiStep("a server started in the folder, outside mutible", [repo] {
        GCHECK_EQ_STR(Badge(P::Processes), "");
        // The shell exits at once, so the server's parent is gone: the
        // harness's subreaper holds it, outside the app's own tree.
        const std::string script =
            "cd \"$1\" || exit; python3 -m http.server 0 --bind 127.0.0.1 >/dev/null 2>&1 & "
            "echo $! > \"$2\"";
        const std::string pidFile = MCGuiScratch() + "/sidebaricons-server.pid";
        const char* argv[] = {"/bin/sh", "-c", script.c_str(), "sh", repo.c_str(), pidFile.c_str(),
                              nullptr};
        GError* error = nullptr;
        GSubprocess* p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE, &error);
        GCHECK(p != nullptr);
        if (p) g_object_unref(p);
        g_clear_error(&error);
    });
    MCGuiWait("its pid", 10, [] {
        const std::string s = MCGuiReadFile(MCGuiScratch() + "/sidebaricons-server.pid");
        gServerPid = s.empty() ? 0 : atoi(s.c_str());
        return gServerPid > 0;
    });
    // The monitor looks every 10 s; a look asked for now comes sooner, and
    // the badge follows each look (the server takes a moment to listen).
    MCGuiWait("Processes' badge says 1 after a look", 30, [] {
        ProcessMonitor::shared().sampleNow(nullptr);
        return Badge(P::Processes) == "1";
    });
    MCGuiStep("its pill shows it; the server stops", [] {
        GtkWidget* pill = Icons()->badgeWidget(P::Processes);
        GCHECK(gtk_widget_get_mapped(pill));
        GCHECK_EQ_STR(gtk_label_get_text(GTK_LABEL(pill)), "1");
        KillServer();
    });
    MCGuiWait("the badge goes at a later look", 30, [] {
        ProcessMonitor::shared().sampleNow(nullptr);
        return Badge(P::Processes) == "";
    });
    MCGuiStep("server gone", [] { gServerPid = 0; });

    // ---- the row hides with the sidebar, and by the setting
    MCGuiStep("Toggle Sidebar's key", [] {
        RepoWindow().tree->focus();
        MCGuiKey("ctrl+b");
    });
    MCGuiWait("the sidebar and its row hidden", 10, [] {
        return !RepoWindow().layout->sidebarShown() && !gtk_widget_get_mapped(RepoWindow().sidebarIconsRow);
    });
    MCGuiStep("the key again", [] { MCGuiKey("ctrl+b"); });
    MCGuiWait("both back", 10, [] {
        return RepoWindow().layout->sidebarShown() && MCGuiOnScreen(Button(P::Tree));
    });
    MCGuiStep("workbench.sidebar-icons = off", [] {
        MCGuiSetSettings(kSettings + "workbench.sidebar-icons = off\n");
    });
    MCGuiWait("the row hidden, the panels from the sidebar's top", 10, [] {
        const MCGuiAppWindow w = RepoWindow();
        return !gtk_widget_get_visible(w.sidebarIconsRow) && w.layout->sidebarShown() &&
               std::abs(TopOf(w.sidebarStack) - TopOf(w.layout->sidebar())) < 1;
    });
    MCGuiStep("on again", [] { MCGuiSetSettings(kSettings); });
    MCGuiWait("the row back, its marks and counts as they were", 10, [] {
        return MCGuiOnScreen(Button(P::Tree)) && MarkedAlone(P::Tree) &&
               Badge(P::SourceControl) == "2";
    });

    MCGuiStep("a server left by a failed wait stops", [] { KillServer(); });
    CloseRepoWindow();
}
