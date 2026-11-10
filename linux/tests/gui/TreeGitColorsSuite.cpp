// TreeGitColorsSuite.cpp — `./build/minicode-guitest tree_git_colors`: the
// file tree's git colors (TreeGitColors.h, the core's Git::Decorations),
// PARITY.md item 2.10, the git half of the Mac's tests/gui/TreeSuite.mm,
// on a scratch repository with real git, every change made on the disk as
// a terminal or a build would.
//
// A file changed after the window opened takes the modified color, and its
// folder with it; clean rows keep the tree's text color; the tooltip names
// the status; an untracked file, a staged new file and an ignored folder
// take theirs; a commit made outside, which writes only in .git, takes the
// colors away again while the ignored folder keeps its own; a change only
// inside the ignored folder runs no status; the colors are the tree.git-*
// keys and VS Code's light defaults under a light theme.
#include "GitChecks.h"

#include "FileTree.h"
#include "Settings.h"
#include "TreeGitColors.h"

#include <cmath>

using namespace gitchecks;

namespace {

TreeGitColors* Colors() { return RepoWindow().treeColors; }
GtkWidget* TreeWidget() { return RepoWindow().tree->widget(); }
GtkWidget* Row(const std::string& name) { return MCGuiFindLabel(TreeWidget(), name); }

std::string gSettings = "lsp.enabled = false\n";

Rgba Expected(TreeGitColor c) { return Settings::parse(gSettings).treeGitColor(c); }
Rgba SidebarText() { return Settings::parse(gSettings).text(Surface::Sidebar); }

// The color a row's name is drawn in, from the stylesheet.
bool Colored(const std::string& name, const Rgba& want) {
    GtkWidget* l = Row(name);
    if (!l) return false;
    GdkRGBA c;
    gtk_widget_get_color(l, &c);
    return std::lround(c.red * 255) == want.r && std::lround(c.green * 255) == want.g &&
           std::lround(c.blue * 255) == want.b;
}
bool Colored(const std::string& name, TreeGitColor c) { return Colored(name, Expected(c)); }
bool Plain(const std::string& name) { return Colored(name, SidebarText()); }

std::string Tip(const std::string& name) {
    GtkWidget* l = Row(name);
    const char* t = l ? gtk_widget_get_tooltip_text(l) : nullptr;
    return t ? t : "";
}

unsigned gRuns = 0;

}  // namespace

MC_GUI_SUITE(tree_git_colors) {
    if (GitPath().empty()) {
        MCGuiStep("no git", [] { MCGuiNote("no git here: the tree's color checks are skipped"); });
        return;
    }
    const std::string repo = MCGuiScratch() + "/treecolors";
    MCGuiStep("the settings", [] { MCGuiSetSettings(gSettings); });
    Shell("a repository: src (a.txt, b.txt, deep/d.txt), docs, small.txt, build/ ignored",
          "set -e; mkdir -p '" + repo + "/src/deep' '" + repo + "/docs'; cd '" + repo +
              "'; echo alpha > src/a.txt; echo bravo > src/b.txt; echo delta > src/deep/d.txt; "
              "echo guide > docs/guide.txt; echo s > small.txt; echo 'build/' > .gitignore; " +
              G() + " init -q -b main; " + G() + " add -A; " + G() + " commit -qm first");
    OpenRepoWindow(repo, false);
    MCGuiWait("the tree's rows, and a first look", 15, [] {
        return Row("src") && Row("small.txt") && Colors() && Colors()->runs() > 0 &&
               Colors()->inRepository();
    });
    MCGuiStep("a click opens src", [] { MCGuiClick(Row("src")); });
    MCGuiWait("its files", 10, [] { return Row("a.txt") && Row("b.txt") && Row("deep"); });
    MCGuiStep("a clean repository: every row in the tree's text color", [] {
        for (const char* n : {"src", "a.txt", "b.txt", "deep", "docs", "small.txt"})
            GCHECK(Plain(n));
        GCHECK_EQ_STR(Tip("a.txt"), "");
        gRuns = Colors()->runs();
        MCGuiWriteFile(In("src/a.txt"), "ALPHA\n");   // as an editor elsewhere would
    });

    // ---- following the disk
    MCGuiWait("the changed file and its folder in the modified color", 15, [] {
        return Colored("a.txt", TreeGitColor::Modified) && Colored("src", TreeGitColor::Modified);
    });
    MCGuiStep("clean rows keep the tree's text color; the tooltip names the status", [] {
        GCHECK(Colors()->runs() > gRuns);
        GCHECK(Plain("b.txt") && Plain("deep") && Plain("docs") && Plain("small.txt"));
        GCHECK_EQ_STR(Tip("a.txt"), "Modified (git)");
        GCHECK_EQ_STR(Tip("src"), "Modified (git)");
        // An untracked file, and output in an ignored folder.
        MCGuiWriteFile(In("notes.txt"), "n\n");
        MCGuiWriteFile(In("build/out.o"), "o\n");
    });
    MCGuiWait("the untracked file and the ignored folder", 15, [] {
        return Colored("notes.txt", TreeGitColor::Untracked) &&
               Colored("build", TreeGitColor::Ignored);
    });
    MCGuiStep("their tooltips", [] {
        GCHECK_EQ_STR(Tip("notes.txt"), "Untracked (git)");
        GCHECK_EQ_STR(Tip("build"), "Ignored (git)");
        MCGuiWriteFile(In("added.txt"), "new\n");
    });
    Shell("a new file staged, as a terminal would", "cd '" + repo + "' && " + G() + " add added.txt");
    MCGuiWait("the staged new file in the added color", 15, [] {
        return Colored("added.txt", TreeGitColor::Added);
    });
    // The tree watches the folders it has listed (TreeGitColors.h): deep
    // is opened first.
    MCGuiStep("a click opens deep", [] {
        GCHECK_EQ_STR(Tip("added.txt"), "Added (git)");
        MCGuiClick(Row("deep"));
    });
    MCGuiWait("its file", 10, [] { return Row("d.txt") != nullptr; });
    MCGuiStep("a file changed two folders down", [] {
        GCHECK(Plain("deep") && Plain("d.txt"));
        MCGuiWriteFile(In("src/deep/d.txt"), "DELTA\n");
    });
    MCGuiWait("it and both folders above in the modified color", 15, [] {
        return Colored("d.txt", TreeGitColor::Modified) &&
               Colored("deep", TreeGitColor::Modified) && Colored("src", TreeGitColor::Modified);
    });

    // ---- the editor's own save
    MCGuiStep("guide.txt opened, a line typed, Ctrl+S", [] {
        GCHECK(Plain("docs"));
        MCGuiOpenFileKept(In("docs/guide.txt"));
        GCHECK(RepoWindow().editor->currentPath() == In("docs/guide.txt"));
        gtk_widget_grab_focus(RepoWindow().editor->textView());
        MCGuiType("more\n");
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("saved", 10, [] { return MCGuiReadFile(In("docs/guide.txt")) == "more\nguide\n"; });
    MCGuiWait("the file and its folder in the modified color", 15, [] {
        return Colored("guide.txt", TreeGitColor::Modified) &&
               Colored("docs", TreeGitColor::Modified);
    });

    // ---- the theme's defaults, and the keys
    MCGuiStep("a light theme", [] {
        gSettings = "lsp.enabled = false\ntheme = light-modern\n";
        MCGuiSetSettings(gSettings);
    });
    MCGuiWait("VS Code's light colors", 10, [] {
        return Colored("a.txt", TreeGitColor::Modified) &&
               Colored("notes.txt", TreeGitColor::Untracked) && Plain("b.txt");
    });
    MCGuiStep("light: the modified color is VS Code's #895503", [] {
        GCHECK(Expected(TreeGitColor::Modified).rgb() == 0x895503);
        gSettings = "lsp.enabled = false\ntree.git-modified = #FF00FF\n";
        MCGuiSetSettings(gSettings);
    });
    MCGuiWait("tree.git-modified's color", 10, [] {
        return Colored("a.txt", Rgba::hex(0xFF00FF)) && Colored("src", Rgba::hex(0xFF00FF));
    });
    MCGuiStep("the settings back", [] {
        gSettings = "lsp.enabled = false\n";
        MCGuiSetSettings(gSettings);
    });
    MCGuiWait("dark again: VS Code's #E2C08D", 10, [] {
        return Expected(TreeGitColor::Modified).rgb() == 0xE2C08D &&
               Colored("a.txt", TreeGitColor::Modified);
    });

    // ---- a commit made outside writes only in .git
    Shell("everything committed, as a terminal would",
          "set -e; cd '" + repo + "'; " + G() + " add -A; " + G() + " commit -qm second");
    MCGuiWait("committed rows lose their color; the ignored folder keeps its own", 15, [] {
        return Plain("a.txt") && Plain("src") && Plain("deep") && Plain("notes.txt") &&
               Plain("added.txt") && Colored("build", TreeGitColor::Ignored);
    });
    MCGuiWaitQuiet("the commit's last status runs out first", 1.0, 20, [] {
        return (long)Colors()->runs();
    });
    // build is opened, so its folder is watched and the change is reported:
    // it is the colors that let it go.
    MCGuiStep("a click opens build", [] { MCGuiClick(Row("build")); });
    MCGuiWait("its file", 10, [] { return Row("out.o") != nullptr; });
    MCGuiWaitQuiet("the statuses run out", 1.0, 20, [] { return (long)Colors()->runs(); });
    MCGuiStep("a change only inside the ignored folder", [] {
        gRuns = Colors()->runs();
        MCGuiWriteFile(In("build/more.o"), "o\n");
    });
    MCGuiWait("reported: the tree lists it", 10, [] { return Row("more.o") != nullptr; });
    MCGuiPause(1.0);   // before a negative check: no status is to run (one waits 0.5 s)
    MCGuiStep("runs no status", [] {
        MCGuiNote("statuses since: %u", Colors()->runs() - gRuns);
        GCHECK(Colors()->runs() == gRuns);
    });

    CloseRepoWindow();
}
