// TerminalSplitsSuite.cpp — terminal_splits: panes within a terminal tab
// (PARITY.md 4.7, second part; the Mac's terminal_splits). A real bash in
// each pane, started as the app starts it; a shell is "ready" when the shell
// integration's marks say it is at a prompt, never text read off the screen.
//   - Split Terminal (Ctrl+Shift+5) halves the active pane side by side,
//     Split Terminal Down stacks; each pane has a shell of its own and the
//     true size (columns) its frame holds;
//   - splitting until a half would be under 20 columns evens the panes, and
//     the next split is refused;
//   - dragging a bar changes the shares, the keyboard resize moves a
//     boundary 4 cells, closing a pane gives its share to its neighbour;
//   - a click in a pane makes it the active terminal;
//   - `minicode split --right term --run`, `--below term`, `--right browse`,
//     and `open` refused.
#include "Harness.h"
#include "AppSeam.h"
#include "ShellIntegration.h"
#include "TabStrip.h"
#include "Terminal.h"
#include "TerminalDock.h"
#include "WindowLayout.h"

#include <gio/gio.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace {

TerminalDock* dock() { return MCGuiApp().termDock; }
TerminalGroup* grp() { return dock() ? dock()->activeGroup() : nullptr; }
bool ready(Terminal* t) { return t && t->shellPid() > 0 && t->shellState().known && t->shellState().atPrompt; }
bool allReady() {
    if (!grp()) return false;
    for (Terminal* t : grp()->panes())
        if (!ready(t)) return false;
    return true;
}

std::string gHome, gOldShell;
bool gHadShell = false;
Terminal *gA = nullptr, *gB = nullptr, *gC = nullptr;
std::vector<double> gCols;
std::vector<double> gRows;
int gPanes = 0;
std::function<void(Terminal*, std::function<void()>)> gOldConfirm;

void action(const char* name) {
    GAction* a = g_action_map_lookup_action(G_ACTION_MAP(MCGuiApp().window), name);
    GCHECK(a != nullptr);
    if (a) g_action_activate(a, nullptr);
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

struct CliRun {
    bool done = false;
    int exitCode = -1;
    std::string out, err;
};
std::shared_ptr<CliRun> gCli;

void runCli(const std::vector<std::string>& args) {
    gCli = std::make_shared<CliRun>();
    const std::string exe = MCGuiBuildDir() + "/minicode";
    std::vector<const char*> argv = {exe.c_str()};
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    GError* error = nullptr;
    GSubprocess* p = g_subprocess_newv(
        argv.data(), (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE), &error);
    if (!p) {
        gCli->done = true;
        g_clear_error(&error);
        return;
    }
    g_subprocess_communicate_utf8_async(
        p, nullptr, nullptr,
        +[](GObject* src, GAsyncResult* res, gpointer data) {
            auto* holder = static_cast<std::shared_ptr<CliRun>*>(data);
            char *out = nullptr, *err = nullptr;
            g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, &err, nullptr);
            (*holder)->out = out ? out : "";
            (*holder)->err = err ? err : "";
            (*holder)->exitCode = g_subprocess_get_if_exited(G_SUBPROCESS(src))
                                      ? g_subprocess_get_exit_status(G_SUBPROCESS(src)) : -1;
            g_free(out);
            g_free(err);
            (*holder)->done = true;
            delete holder;
            g_object_unref(src);
        },
        new std::shared_ptr<CliRun>(gCli));
}

}  // namespace

MC_GUI_SUITE(terminal_splits) {
    MCGuiStep("a bash, one tab, the dock shown", [] {
        GCHECK(dock() != nullptr);
        if (!dock()) return;
        const char* old = g_getenv("SHELL");
        gHadShell = old != nullptr;
        gOldShell = old ? old : "";
        gHome = g_get_home_dir();
        GCHECK(gHome.find(MCGuiScratch()) == 0);   // never the user's own
        g_setenv("SHELL", "/bin/bash", TRUE);
        MCGuiWriteFile(gHome + "/.bashrc", "\n");
        gOldConfirm = dock()->confirmClose;
        dock()->confirmClose = nullptr;
        // Earlier suites leave tabs and panes: start from one fresh terminal.
        dock()->newTerminal();
        while (dock()->count() > 1) dock()->close(0, false);
        GCHECK(dock()->count() == 1);
        if (!MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        gA = dock()->at(0);
        GCHECK(grp() && grp()->count() == 1);
    });
    MCGuiWait("the first shell marks its prompt", 30, [] { return ready(gA); });

    // ---- side by side
    MCGuiStep("Split Terminal by its key: two panes, halves, the new one active", [] {
        MCGuiKey("ctrl+shift+5");
    });
    MCGuiWait("both shells ready", 30, [] { return grp()->count() == 2 && allReady(); });
    MCGuiStep("each pane has a shell of its own and half the width", [] {
        const auto p = grp()->panes();
        GCHECK(p.size() == 2);
        gB = p[1];
        GCHECK(p[0] == gA);
        GCHECK(gA->shellPid() != gB->shellPid());
        GCHECK(MCGuiApp().terminal == gB);
        GCHECK(dock()->count() == 1);   // the same tab
        const auto& l = grp()->layout();
        GCHECK(l.columns.size() == 2 && near(l.columns[0], 0.5, 1e-9) && near(l.columns[1], 0.5, 1e-9));
        TerminalLayout::Rect fa, fb;
        GCHECK(grp()->frameOf(gA, &fa) && grp()->frameOf(gB, &fb));
        const double W = gtk_widget_get_width(grp()->widget());
        const double H = gtk_widget_get_height(grp()->widget());
        MCGuiNote("group %gx%g, A %g,%g %gx%g, B %g,%g %gx%g, cols %d %d", W, H, fa.x, fa.y, fa.w, fa.h,
                  fb.x, fb.y, fb.w, fb.h, gA->columns(), gB->columns());
        GCHECK(near(fa.w + fb.w + TerminalGroup::kGap, W, 2));
        GCHECK(near(fa.h, H, 1) && near(fb.h, H, 1));
        GCHECK(near(fb.x, fa.w + TerminalGroup::kGap, 1));
        // The pty size is the frame's: columns of the pane's cell width.
        double cw, ch;
        gA->cellSize(&cw, &ch);
        GCHECK(gA->columns() > 20 && std::abs(gA->columns() - (int)(fa.w / cw)) <= 3);
        GCHECK(std::abs(gB->columns() - (int)(fb.w / cw)) <= 3);
        GCHECK(grp()->columnBars() == 1 && grp()->rowBars() == 0);
    });

    // ---- one down
    MCGuiStep("Split Terminal Down stacks the active pane", [] { action("splitterminaldown"); });
    MCGuiWait("three shells ready", 30, [] { return grp()->count() == 3 && allReady(); });
    MCGuiStep("a column of two, each its own shell, halves of the height", [] {
        const auto p = grp()->panes();   // column by column
        GCHECK(p.size() == 3 && p[0] == gA && p[1] == gB);
        gC = p[2];
        GCHECK(gC != gB && gC->shellPid() != gB->shellPid() && gC->shellPid() != gA->shellPid());
        GCHECK(MCGuiApp().terminal == gC);
        const auto& l = grp()->layout();
        GCHECK(l.rows.size() == 2 && l.rows[0].size() == 1 && l.rows[1].size() == 2);
        GCHECK(near(l.rows[1][0], 0.5, 1e-9));
        TerminalLayout::Rect fb, fc;
        GCHECK(grp()->frameOf(gB, &fb) && grp()->frameOf(gC, &fc));
        const double H = gtk_widget_get_height(grp()->widget());
        GCHECK(near(fb.h + fc.h + TerminalGroup::kGap, H, 2));
        GCHECK(near(fb.x, fc.x, 1) && near(fb.w, fc.w, 1));
        GCHECK(grp()->rowBars() == 1);
        // A third row would leave a pane under 5 rows: the core says so.
        double cw, ch;
        gC->cellSize(&cw, &ch);
        GCHECK(std::abs(gC->rows() - (int)(fc.h / ch)) <= 2);
    });

    // ---- the active terminal follows the keyboard
    MCGuiStep("a click in the first pane makes it the active terminal", [] {
        MCGuiClick(gA->widget());
    });
    MCGuiWait("app->terminal follows the click", 10, [] { return MCGuiApp().terminal == gA; });
    MCGuiStep("a click in the lower pane", [] { MCGuiClick(gC->widget()); });
    MCGuiWait("and back", 10, [] { return MCGuiApp().terminal == gC && grp()->active() == gC; });

    // ---- a drag of the bar
    MCGuiStep("dragging the column bar right changes the shares", [] {
        gCols = grp()->layout().columns;
        double x, y;
        GCHECK(grp()->barCenter(true, 0, &x, &y));
        MCGuiDrag(grp()->widget(), x, y, x + 80, y);
    });
    MCGuiWait("the drag is over", 10, [] { return !MCGuiInputPending(); });
    MCGuiStep("the first column grew by about the drag, the sum is still 1", [] {
        const auto& c = grp()->layout().columns;
        const double W = gtk_widget_get_width(grp()->widget());
        MCGuiNote("columns %g %g (was %g %g), W %g", c[0], c[1], gCols[0], gCols[1], W);
        GCHECK(c[0] > gCols[0] + 40.0 / W);
        GCHECK(near(c[0] + c[1], 1.0, 1e-9));
    });
    MCGuiStep("dragging the row bar changes the lower column's shares", [] {
        gRows = grp()->layout().rows[1];
        double x, y;
        GCHECK(grp()->barCenter(false, 0, &x, &y));
        MCGuiDrag(grp()->widget(), x, y, x, y + 10);
    });
    MCGuiWait("the second drag is over", 10, [] { return !MCGuiInputPending(); });
    MCGuiStep("the top row grew", [] {
        const auto& r = grp()->layout().rows[1];
        MCGuiNote("rows %g %g (was %g %g)", r[0], r[1], gRows[0], gRows[1]);
        GCHECK(r[0] > gRows[0] && near(r[0] + r[1], 1.0, 1e-9));
    });

    // ---- keyboard resize
    MCGuiStep("Resize Terminal Pane moves the active pane's boundary 4 cells", [] {
        gCols = grp()->layout().columns;
        action("resizeterminalleft");   // the active pane is the lower one of the last column
    });
    MCGuiStep("the last column grew by 4 cells, then Right gives them back", [] {
        const auto& c = grp()->layout().columns;
        const double W = gtk_widget_get_width(grp()->widget());
        double cw, ch;
        gC->cellSize(&cw, &ch);
        MCGuiNote("columns %g %g (was %g %g), 4 cells = %g", c[0], c[1], gCols[0], gCols[1], 4 * cw / W);
        GCHECK(near(gCols[0] - c[0], 4 * cw / W, 0.004));
        action("resizeterminalright");
        const auto& d = grp()->layout().columns;
        GCHECK(near(d[0], gCols[0], 0.004));
    });

    // ---- closing a pane
    MCGuiStep("Close Terminal closes the active pane only; its share goes to its neighbour", [] {
        gRows = grp()->layout().rows[1];
        action("closeterminal");
        GCHECK(grp()->count() == 2);
        GCHECK(dock()->count() == 1);
        const auto& l = grp()->layout();
        GCHECK(l.rows[1].size() == 1 && near(l.rows[1][0], 1.0, 1e-9));
        GCHECK(grp()->rowBars() == 0);
        GCHECK(MCGuiApp().terminal == gB);
    });
    MCGuiStep("closing the first column gives its width to the next", [] {
        dock()->closePane(gA, false);
        GCHECK(grp()->count() == 1);
        const auto& l = grp()->layout();
        GCHECK(l.columns.size() == 1 && near(l.columns[0], 1.0, 1e-9));
        GCHECK(grp()->columnBars() == 0);
        GCHECK(MCGuiApp().terminal == gB);
        gA = gB;
    });
    MCGuiWait("the pane takes the whole width", 10, [] {
        TerminalLayout::Rect f;
        return grp()->frameOf(gA, &f) && near(f.w, gtk_widget_get_width(grp()->widget()), 1);
    });

    // ---- half, even, refuse
    MCGuiStep("splitting on halves, then evens, then is refused under 20 columns", [] {
        std::vector<int> counts;
        bool evened = false, refused = false;
        for (int i = 0; i < 12 && !refused; i++) {
            const size_t before = grp()->layout().columns.size();
            const std::vector<double> was = grp()->layout().columns;
            Terminal* t = dock()->split(false, false);
            if (!t) {
                refused = true;
                GCHECK(grp()->layout().columns == was);   // nothing changed
                break;
            }
            const auto& c = grp()->layout().columns;
            GCHECK(c.size() == before + 1);
            bool equal = true;
            for (double v : c) equal = equal && near(v, c[0], 1e-9);
            if (equal && c.size() > 2) evened = true;
            double sum = 0;
            for (double v : c) sum += v;
            GCHECK(near(sum, 1.0, 1e-9));
        }
        GCHECK(evened);
        GCHECK(refused);
        const int n = (int)grp()->layout().columns.size();
        MCGuiNote("refused with %d columns", n);
        const double W = gtk_widget_get_width(grp()->widget());
        double cw, ch;
        grp()->active()->cellSize(&cw, &ch);
        GCHECK(W / n >= 20 * cw && W / (n + 1) < 20 * cw + 4 * n);
    });
    MCGuiStep("close the extra panes", [] {
        while (grp()->count() > 1) dock()->closePane(grp()->panes().back(), false);
        GCHECK(grp()->count() == 1 && near(grp()->layout().columns[0], 1.0, 1e-9));
        gA = grp()->panes()[0];
    });
    MCGuiWait("the shell is ready", 30, [] { return ready(gA); });

    // ---- the control channel
    MCGuiStep("minicode split --right term --run: a new pane, the command ran there", [] {
        gPanes = grp()->count();
        runCli({"split", "--right", "term", "--run", "echo ran-in-$$ > \"$HOME/sp.out\""});
    });
    MCGuiWait("split answered", 30, [] { return gCli->done; });
    MCGuiStep("a second pane, active", [] {
        if (gCli->exitCode != 0) MCGuiNote("split said: %s", gCli->err.c_str());
        GCHECK(gCli->exitCode == 0);
        GCHECK(grp()->count() == gPanes + 1);
        GCHECK(dock()->count() == 1);
        GCHECK(MCGuiApp().terminal == grp()->panes().back());
    });
    MCGuiWait("the command ran", 20, [] { return MCGuiFileExists(gHome + "/sp.out"); });
    MCGuiStep("in the new pane's shell, not the first", [] {
        const std::string got = MCGuiReadFile(gHome + "/sp.out");
        const int pid = grp()->panes().back()->shellPid();
        MCGuiNote("sp.out %s new shell %d first %d", got.c_str(), pid, gA->shellPid());
        GCHECK(got == "ran-in-" + std::to_string(pid) + "\n");
        GCHECK(gA->shellPid() != pid);
    });
    MCGuiStep("split --below term opens a tab in the dock", [] {
        gPanes = dock()->count();
        runCli({"split", "--below", "term", "--title", "below"});
    });
    MCGuiWait("--below answered", 30, [] { return gCli->done; });
    MCGuiStep("a new tab called below", [] {
        GCHECK(gCli->exitCode == 0);
        GCHECK(dock()->count() == gPanes + 1);
        GCHECK_EQ_STR(dock()->strip()->item(dock()->activeIndex()).title, "below");
        dock()->close(dock()->activeIndex(), false);
        GCHECK(dock()->count() == gPanes);
    });
    MCGuiStep("split --right browse shows the browser and browses there", [] {
        runCli({"split", "--right", "browse", "about:blank"});
    });
    MCGuiWait("browse answered", 30, [] { return gCli->done; });
    MCGuiStep("the browser pane is shown", [] {
        if (MCGuiApp().browser) {
            if (gCli->exitCode != 0) MCGuiNote("browse said: %s", gCli->err.c_str());
            GCHECK(gCli->exitCode == 0);
        } else {
            MCGuiNote("no browser in this build; browse not checked");
        }
        runCli({"split", "--right", "open", "/etc/hostname"});
    });
    MCGuiWait("open answered", 30, [] { return gCli->done; });
    MCGuiStep("split open is refused with the core's message", [] {
        GCHECK(gCli->exitCode != 0);
        GCHECK(gCli->err.find("the editor has one pane") != std::string::npos);
    });

    // ---- clean up
    MCGuiStep("one pane again; the settings are put back", [] {
        while (grp()->count() > 1) dock()->closePane(grp()->panes().back(), false);
        dock()->confirmClose = gOldConfirm;
        if (gHadShell) g_setenv("SHELL", gOldShell.c_str(), TRUE);
        else g_unsetenv("SHELL");
    });
}
