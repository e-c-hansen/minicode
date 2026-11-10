// ReplaceInFolderSuite.cpp — Replace in Folder (Ctrl+Alt+Shift+F;
// `./build/minicode-guitest replace_in_folder`, linux/PARITY.md item 1.6),
// the counterpart of the Mac's tests/gui/ReplaceInFolderSuite.mm, by real
// keys and clicks: Replace All counts first and asks; a file open in a
// clean tab is changed in the tab, unsaved and undoable there; a file
// whose tab has unsaved changes is left alone and named; the rest are
// written to disk; Ctrl+Shift+1 on a row replaces only its match, and only
// while its line still reads as searched; a file under two names (a
// symlink) is counted and replaced once; files to include and exclude
// narrow the search; and Alt+C works in the window.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "Search.h"

#include <sys/stat.h>
#include <unistd.h>

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
SearchPanel* panel() { return MCGuiApp().search; }
GtkWindow* panelWindow() { return panel() ? GTK_WINDOW(panel()->window()) : nullptr; }

std::string dir() { return MCGuiProjectFile("rif"); }
std::string one() { return dir() + "/one.txt"; }
std::string two() { return dir() + "/two.txt"; }
std::string three() { return dir() + "/three.txt"; }
std::string four() { return dir() + "/sub/four.txt"; }
std::string dir2() { return MCGuiProjectFile("rif2"); }

bool idle() { return panel() && !panel()->searching() && !panel()->replacing(); }

bool focusIn(GtkWidget* w) {
    GtkWindow* win = GTK_WINDOW(gtk_widget_get_root(w));
    GtkWidget* f = win ? gtk_root_get_focus(GTK_ROOT(win)) : nullptr;
    return f && (f == w || gtk_widget_is_ancestor(f, w));
}

std::string tabText(const std::string& path) {
    Document* d = ed()->documentFor(path);
    return d ? d->text() : "(no tab)";
}

bool tabDirty(const std::string& path) {
    Document* d = ed()->documentFor(path);
    return d && d->dirty();
}

// The row of `path`'s line `line`, or -1.
int rowOf(const std::string& path, int line) {
    const std::vector<FolderSearchMatch>& hits = panel()->matches();
    for (size_t i = 0; i < hits.size(); i++)
        if (hits[i].path == path && hits[i].line == line) return (int)i;
    return -1;
}

void selectRow(int row) {
    GtkSelectionModel* m = gtk_list_view_get_model(GTK_LIST_VIEW(panel()->listView()));
    gtk_single_selection_set_selected(GTK_SINGLE_SELECTION(m), (guint)row);
    gtk_widget_grab_focus(panel()->listView());
}

GtkWindow* alert() {
    GtkWindow* top = MCGuiTopWindow();
    return top && top != panelWindow() && top != MCGuiApp().window ? top : nullptr;
}

// The question up and laid out: no window manager places it, and a click
// before its layout lands elsewhere (TabsSuite's questionUp).
bool alertAsks(const std::string& question) {
    GtkWindow* a = alert();
    if (!a || !MCGuiFindLabel(GTK_WIDGET(a), question)) return false;
    GtkWidget* replace = MCGuiFindButton(GTK_WIDGET(a), "Replace");
    return replace && gtk_widget_get_width(replace) > 0 && gtk_widget_get_height(replace) > 0;
}

void typeInto(GtkWidget* entry, const std::string& text) {
    MCGuiClick(entry);
    MCGuiKey("ctrl+a");
    MCGuiType(text);
}

std::string gStatus;

}  // namespace

MC_GUI_SUITE(replace_in_folder) {
    MCGuiStep("four files; three.txt open with unsaved changes, two.txt open and clean", [] {
        if (ed()->dirty()) ed()->save();
        MCGuiWriteFile(one(), "cat and cat\n");
        MCGuiWriteFile(two(), "a cat\n");
        MCGuiWriteFile(three(), "cat three\n");
        MCGuiWriteFile(four(), "no cats here? cat\n");
        MCGuiOpenFileKept(three());
    });
    MCGuiWait("three.txt shows", 10, [] { return ed()->currentPath() == three(); });
    MCGuiStep("typed into it", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+home");
        MCGuiType("edited ");
    });
    MCGuiWait("unsaved", 10, [] { return tabText(three()) == "edited cat three\n" && tabDirty(three()); });
    MCGuiStep("two.txt in a tab of its own", [] { MCGuiOpenFileKept(two()); });
    MCGuiWait("it shows", 10, [] { return ed()->currentPath() == two() && !tabDirty(two()); });
    MCGuiStep("Ctrl+Alt+Shift+F: Replace in Folder", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+alt+shift+f");
    });
    // The query laid out too, since the next step clicks it: a window maps
    // its widgets at once and places them at its first frame, and a click
    // in between found nothing to point at (a loaded full run, after which
    // every later step of the suite failed).
    MCGuiWait("its window, the replace row shown, the keyboard in the replacement", 10, [] {
        return panel() && gtk_widget_get_mapped(panel()->window()) && panel()->replaceShown() &&
               focusIn(panel()->replaceEntry()) && MCGuiOnScreen(panel()->queryEntry());
    });
    MCGuiStep("the folder; \"cat\" typed in the query", [] {
        panel()->setScope(dir());
        MCGuiSetKeyWindow(panelWindow());
        typeInto(panel()->queryEntry(), "cat");
    });
    MCGuiWait("the results", 10, [] { return idle() && panel()->matches().size() == 4; });
    MCGuiStep("every match counted, though a row is a line; \"dog\" for the replacement", [] {
        MCGuiNote("status: %s", panel()->statusText().c_str());
        GCHECK_EQ_STR(panel()->statusText(), "6 matches in 4 files");
        typeInto(panel()->replaceEntry(), "dog");
        MCGuiKey("enter");   // Replace All, from the replacement
    });
    MCGuiWait("it asks first, three.txt left out of the count", 10, [] {
        return alertAsks("Replace 5 matches in 3 files?");
    });
    MCGuiStep("the files changed in tabs and left alone are named; Replace", [] {
        GCHECK(MCGuiFindLabel(GTK_WIDGET(alert()),
                              "Each becomes “dog”. Files open in a tab are changed there, unsaved, "
                              "and can be undone there; the others are written to disk. Left "
                              "alone, since their tabs have unsaved changes: three.txt.") != nullptr);
        MCGuiSetKeyWindow(alert());
        MCGuiClick(MCGuiFindButton(GTK_WIDGET(alert()), "Replace"));
    });
    MCGuiWait("replaced", 10, [] { return idle() && MCGuiReadFile(one()) == "dog and dog\n"; });
    MCGuiStep("written, changed in the clean tab, three.txt left alone and named", [] {
        MCGuiSetKeyWindow(panelWindow());
        GCHECK_EQ_STR(MCGuiReadFile(four()), "no dogs here? dog\n");
        // two.txt in its tab, unsaved; the disk as it was.
        GCHECK_EQ_STR(tabText(two()), "a dog\n");
        GCHECK(tabDirty(two()));
        GCHECK_EQ_STR(MCGuiReadFile(two()), "a cat\n");
        // three.txt: neither its tab nor its file.
        GCHECK_EQ_STR(MCGuiReadFile(three()), "cat three\n");
        GCHECK_EQ_STR(tabText(three()), "edited cat three\n");
        gStatus = panel()->statusText();
        MCGuiNote("status: %s", gStatus.c_str());
        GCHECK(gStatus.rfind("Replaced 5 matches in 3 files (1 open in tabs, unsaved). Left alone, "
                             "with unsaved changes: three.txt. Now ", 0) == 0);
    });
    MCGuiStep("the tab's change undoes there", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
        MCGuiOpenFileKept(two());
    });
    MCGuiWait("two.txt shows", 10, [] { return ed()->currentPath() == two(); });
    MCGuiStep("Ctrl+Z", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("as on disk, and clean", 10, [] { return tabText(two()) == "a cat\n" && !tabDirty(two()); });

    // ---- one row's match alone
    MCGuiStep("one.txt with three matches on two lines; searched again", [] {
        MCGuiWriteFile(one(), "cat cat\ncat\n");
        MCGuiSetKeyWindow(panelWindow());
        panel()->runSearch();
    });
    MCGuiWait("its second line is a row", 10, [] { return idle() && rowOf(one(), 2) >= 0; });
    MCGuiStep("that row selected; Ctrl+Shift+1", [] {
        selectRow(rowOf(one(), 2));
        MCGuiKey("ctrl+shift+1");
    });
    MCGuiWait("that one replaced", 10, [] { return idle() && MCGuiReadFile(one()) == "cat cat\ndog\n"; });
    MCGuiStep("searched again; then the file changes under its first row", [] {
        panel()->runSearch();
    });
    MCGuiWait("its first line is a row", 10, [] { return idle() && rowOf(one(), 1) >= 0; });
    MCGuiStep("line 1 changed on disk; Ctrl+Shift+1 on its old row", [] {
        MCGuiWriteFile(one(), "cat cat changed\ndog\n");
        selectRow(rowOf(one(), 1));
        MCGuiKey("ctrl+shift+1");
    });
    MCGuiWait("nothing replaced, and it says so", 10, [] {
        return idle() && panel()->statusText().rfind("That match is no longer there", 0) == 0;
    });
    MCGuiStep("the file as it was changed", [] {
        GCHECK_EQ_STR(MCGuiReadFile(one()), "cat cat changed\ndog\n");
    });

    // ---- a file under two names: counted and replaced once
    MCGuiStep("real.txt, and alias.txt a symlink to it", [] {
        MCGuiWriteFile(dir2() + "/real.txt", "kit\n");
        GCHECK(symlink("real.txt", (dir2() + "/alias.txt").c_str()) == 0);
        panel()->setScope(dir2());
        typeInto(panel()->queryEntry(), "kit");
    });
    // A search shows the file under each name, as the Mac's does.
    MCGuiWait("a row under each name", 10, [] { return idle() && panel()->matches().size() == 2; });
    MCGuiStep("\"kitten\", whose \"kit\" would show a second pass; Replace All", [] {
        typeInto(panel()->replaceEntry(), "kitten");
        MCGuiKey("enter");
    });
    MCGuiWait("it asks about one match in one file", 10, [] { return alertAsks("Replace 1 match in 1 file?"); });
    MCGuiStep("Replace", [] {
        MCGuiSetKeyWindow(alert());
        MCGuiClick(MCGuiFindButton(GTK_WIDGET(alert()), "Replace"));
    });
    MCGuiWait("replaced", 10, [] { return idle() && MCGuiReadFile(dir2() + "/real.txt") != "kit\n"; });
    MCGuiStep("once, through the link, which stays a link", [] {
        MCGuiSetKeyWindow(panelWindow());
        GCHECK_EQ_STR(MCGuiReadFile(dir2() + "/real.txt"), "kitten\n");
        struct stat st;
        GCHECK(lstat((dir2() + "/alias.txt").c_str(), &st) == 0 && S_ISLNK(st.st_mode));
        GCHECK(panel()->statusText().rfind("Replaced 1 match in 1 file", 0) == 0);
    });

    // ---- files to include and exclude, and Alt+C in the window
    MCGuiStep("back to rif; \"cat\"; the details", [] {
        panel()->setScope(dir());
        typeInto(panel()->queryEntry(), "cat");
        MCGuiClick(panel()->detailsButton());
    });
    MCGuiWait("the include and exclude boxes, and the results", 10, [] {
        return MCGuiOnScreen(panel()->excludeEntry()) && MCGuiOnScreen(panel()->includeEntry()) &&
               idle() && panel()->matches().size() == 3;
    });
    MCGuiStep("files to exclude: *.txt", [] { typeInto(panel()->excludeEntry(), "*.txt"); });
    MCGuiWait("nothing", 10, [] { return idle() && panel()->matches().empty() &&
                                         panel()->statusText().rfind("0 matches", 0) == 0; });
    MCGuiStep("none excluded; files to include: three.txt", [] {
        MCGuiKey("ctrl+a");
        MCGuiKey("backspace");
        typeInto(panel()->includeEntry(), "three.txt");
    });
    MCGuiWait("three.txt alone", 10, [] {
        return idle() && panel()->matches().size() == 1 && panel()->matches()[0].path == three();
    });
    MCGuiStep("Alt+C in the window: Match Case; \"CAT\"", [] {
        MCGuiKey("alt+c");
        typeInto(panel()->queryEntry(), "CAT");
    });
    MCGuiWait("no CAT in three.txt", 10, [] {
        return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(panel()->caseButton())) && idle() &&
               panel()->matches().empty();
    });
    MCGuiStep("Alt+C again", [] { MCGuiKey("alt+c"); });
    MCGuiWait("found again", 10, [] { return idle() && panel()->matches().size() == 1; });
    MCGuiStep("put away: the window closed, the unsaved tab dropped", [] {
        MCGuiKey("escape");
        if (Document* d = ed()->documentFor(three())) ed()->closeTab(d);
        MCGuiSetKeyWindow(nullptr);
    });
    MCGuiWait("closed", 10, [] { return !gtk_widget_get_mapped(panel()->window()); });
}
