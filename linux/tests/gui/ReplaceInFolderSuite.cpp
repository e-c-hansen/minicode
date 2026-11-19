// ReplaceInFolderSuite.cpp — Replace in Folder (Ctrl+Alt+Shift+F;
// `./build/mutible-guitest replace_in_folder`, linux/PARITY.md item 1.6),
// the counterpart of the Mac's tests/gui/ReplaceInFolderSuite.mm, by real
// keys and clicks: Replace All counts first and asks; a file open in a
// clean tab is changed in the tab, unsaved and undoable there; a file
// whose tab has unsaved changes is left alone and named; the rest are
// written to disk; Ctrl+Shift+1 on a row replaces only its match, and only
// while its line still reads as searched; a file under two names (a
// symlink) is counted and replaced once; files to include and exclude
// narrow the search; and Alt+C works in the window. From the Mac's #210:
// Enter in the replacement never replaces (it goes into the results, no
// question, no file written); the results take the keyboard (the arrows
// move between rows, a click selects its row and previews it with the
// search window still the active one, and a search run from the list, or
// Alt+C there, gives the keyboard to the new first row); a row's ×, its
// right click menu's Dismiss File and Delete dismiss a line, a file and a
// line, and Replace
// All (Ctrl+Alt+Enter) then counts them out and leaves them unchanged; and
// Ctrl+` in the search window brings the editor window up with the
// keyboard in its terminal, and never hides it from there.
#include "Harness.h"

#include "DocumentView.h"
#include "Editor.h"
#include "Search.h"
#include "WindowLayout.h"

#ifdef MUTIBLE_ENABLE_TERMINAL
#include "Terminal.h"
#endif

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
std::string dir3() { return MCGuiProjectFile("rif3"); }

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

std::string dir4() { return MCGuiProjectFile("rif4"); }
std::string d4(const char* name) { return dir4() + "/" + name; }

// The results' selected row, or -1.
int selected() {
    GtkSelectionModel* m = gtk_list_view_get_model(GTK_LIST_VIEW(panel()->listView()));
    const guint pos = gtk_single_selection_get_selected(GTK_SINGLE_SELECTION(m));
    return pos == GTK_INVALID_LIST_POSITION ? -1 : (int)pos;
}

// A row's text as the list shows it ("a.txt:2  cat cat two"), on screen.
GtkWidget* rowLabel(const std::string& text) { return MCGuiFindLabel(panel()->listView(), text); }

// That row's ×.
GtkWidget* dismissOf(const std::string& text) {
    GtkWidget* label = rowLabel(text);
    return label ? gtk_widget_get_last_child(gtk_widget_get_parent(label)) : nullptr;
}

bool keyboardInResults() { return focusIn(panel()->listView()); }

bool terminalShownBefore = false;

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
        MCGuiKey("ctrl+alt+enter");   // Replace All (Enter there only searches)
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
        MCGuiClick(panel()->replaceAllButton());
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

    // ---- a file over 1 MB: found, never rewritten, named as too large
    MCGuiStep("big.txt just over 1 MB with a match at its end; \"cat\" searched", [] {
        std::string big(1024 * 1024 + 100, 'x');
        big += "\ncat\n";
        MCGuiWriteFile(dir3() + "/big.txt", big);
        panel()->setScope(dir3());
        MCGuiSetKeyWindow(panelWindow());
        typeInto(panel()->queryEntry(), "cat");
    });
    MCGuiWait("Find in Folder lists its match", 10, [] {
        return idle() && panel()->matches().size() == 1 && panel()->matches()[0].path == dir3() + "/big.txt";
    });
    MCGuiStep("\"dog\"; Replace All", [] {
        typeInto(panel()->replaceEntry(), "dog");
        MCGuiClick(panel()->replaceAllButton());
    });
    MCGuiWait("nothing to replace, and it says why", 10, [] {
        return idle() && panel()->statusText().rfind("Nothing replaced: the matches are in files", 0) == 0;
    });
    MCGuiStep("worded as the Mac's, and the file untouched", [] {
        GCHECK_EQ_STR(panel()->statusText(),
                      "Nothing replaced: the matches are in files larger than 1 MB, which Replace "
                      "in Folder leaves alone (big.txt)");
        GCHECK(MCGuiReadFile(dir3() + "/big.txt").size() == 1024 * 1024 + 100 + 5);
        GCHECK(MCGuiReadFile(dir3() + "/big.txt").find("dog") == std::string::npos);
        MCGuiWriteFile(dir3() + "/small.txt", "cat\n");
        panel()->runSearch();
    });
    MCGuiWait("both found", 10, [] { return idle() && panel()->matches().size() == 2; });
    MCGuiStep("Replace All again", [] { MCGuiClick(panel()->replaceAllButton()); });
    MCGuiWait("it asks about the small file", 10, [] { return alertAsks("Replace 1 match in 1 file?"); });
    MCGuiStep("the large file is named in the question; Replace", [] {
        GCHECK(MCGuiFindLabel(GTK_WIDGET(alert()),
                              "Each becomes “dog”. Files open in a tab are changed there, unsaved, "
                              "and can be undone there; the others are written to disk. Left "
                              "alone, since they are larger than 1 MB: big.txt.") != nullptr);
        MCGuiSetKeyWindow(alert());
        MCGuiClick(MCGuiFindButton(GTK_WIDGET(alert()), "Replace"));
    });
    MCGuiWait("small.txt replaced", 10, [] { return idle() && MCGuiReadFile(dir3() + "/small.txt") == "dog\n"; });
    MCGuiStep("big.txt not modified", [] {
        MCGuiSetKeyWindow(panelWindow());
        GCHECK(MCGuiReadFile(dir3() + "/big.txt").find("dog") == std::string::npos);
        GCHECK(MCGuiReadFile(dir3() + "/big.txt").size() == 1024 * 1024 + 100 + 5);
        panel()->setScope(dir());
    });

    // ---- Enter in the replacement never replaces (#210)
    MCGuiStep("rif4: four files, seven matches on six lines; \"cat\" searched", [] {
        MCGuiWriteFile(d4("a.txt"), "cat one\ncat cat two\ncat three\n");
        MCGuiWriteFile(d4("b.txt"), "a cat\n");
        MCGuiWriteFile(d4("c.txt"), "cat c\n");
        MCGuiWriteFile(d4("d.txt"), "cat d\n");
        // The query emptied first, so that setScope searches nothing: with
        // "cat" still in it from above, its search listed these six rows at
        // once, the wait below passed on them, and the typed "cat" was
        // searched 0.35 s later, after Enter had gone into the results.
        gtk_editable_set_text(GTK_EDITABLE(panel()->queryEntry()), "");
        panel()->setScope(dir4());
        MCGuiSetKeyWindow(panelWindow());
        typeInto(panel()->queryEntry(), "cat");
    });
    // The typed query's own search ("ca", searched in a pause between
    // keys, lists the same six rows).
    MCGuiWait("six rows", 10, [] {
        return idle() && panel()->matches().size() == 6 && panel()->searchedQuery() == "cat";
    });
    MCGuiStep("\"dog\" typed in the replacement, then Enter", [] {
        GCHECK_EQ_STR(panel()->statusText(), "7 matches in 4 files");
        typeInto(panel()->replaceEntry(), "dog");
        MCGuiKey("enter");
    });
    // The list already answers the query, so Enter goes into it, as Enter
    // in the query does.
    MCGuiWait("the keyboard in the results, on the first row", 10, [] {
        return keyboardInResults() && selected() == 0;
    });
    MCGuiPause(1.0);   // a negative check: no question comes, nothing is written
    MCGuiStep("no question, every file as it was", [] {
        GCHECK(alert() == nullptr);
        GCHECK(idle());
        GCHECK_EQ_STR(MCGuiReadFile(d4("a.txt")), "cat one\ncat cat two\ncat three\n");
        GCHECK_EQ_STR(MCGuiReadFile(d4("b.txt")), "a cat\n");
        GCHECK_EQ_STR(MCGuiReadFile(d4("c.txt")), "cat c\n");
        GCHECK_EQ_STR(MCGuiReadFile(d4("d.txt")), "cat d\n");
        GCHECK_EQ_STR(panel()->statusText(), "7 matches in 4 files");
    });

    // ---- the results take the keyboard: arrows, and a click
    MCGuiStep("Down", [] { MCGuiKey("down"); });
    MCGuiWait("the second row", 10, [] { return keyboardInResults() && selected() == 1; });
    MCGuiStep("Down again", [] { MCGuiKey("down"); });
    MCGuiWait("the third row", 10, [] { return keyboardInResults() && selected() == 2; });

    // ---- a search while the keyboard is in the results keeps it there
    MCGuiStep("the search run again from the third row", [] {
        panel()->runSearch();
        // Held by the window while the list is empty, never by the removed
        // row (whose window had no focus at all after its next frame).
        GtkWidget* f = gtk_root_get_focus(GTK_ROOT(panelWindow()));
        GCHECK(f && gtk_widget_get_root(f) == GTK_ROOT(panelWindow()));
        GCHECK(panel()->searching());
    });
    MCGuiWait("the keyboard on the first row of the new results", 10, [] {
        return idle() && panel()->matches().size() == 6 && keyboardInResults() && selected() == 0;
    });
    MCGuiStep("Down", [] { MCGuiKey("down"); });
    MCGuiWait("the second row, not the first again", 10, [] {
        return keyboardInResults() && selected() == 1;
    });
    MCGuiStep("Alt+C from the results: Match Case, a search", [] { MCGuiKey("alt+c"); });
    MCGuiWait("the keyboard on the first row again", 10, [] {
        return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(panel()->caseButton())) && idle() &&
               panel()->matches().size() == 6 && keyboardInResults() && selected() == 0;
    });
    MCGuiStep("Alt+C again", [] { MCGuiKey("alt+c"); });
    MCGuiWait("Match Case off, the keyboard on the first row", 10, [] {
        return !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(panel()->caseButton())) && idle() &&
               panel()->matches().size() == 6 && keyboardInResults() && selected() == 0;
    });
    MCGuiStep("Down", [] { MCGuiKey("down"); });
    MCGuiWait("the second row", 10, [] { return keyboardInResults() && selected() == 1; });
    MCGuiStep("the keyboard back in the replacement; a click on b.txt's row", [] {
        MCGuiClick(panel()->replaceEntry());
    });
    MCGuiWait("in the replacement", 10, [] { return focusIn(panel()->replaceEntry()); });
    MCGuiStep("the click", [] { MCGuiClick(rowLabel("b.txt:1  a cat")); });
    MCGuiWait("b.txt in the preview tab, its row selected, the keyboard in the results", 10, [] {
        Document* d = ed()->documentFor(d4("b.txt"));
        return d && d->isPreviewTab() && ed()->currentPath() == d4("b.txt") &&
               selected() == rowOf(d4("b.txt"), 1) && keyboardInResults();
    });
    MCGuiPause(0.5);   // a negative check: the editor window never came up over the results
    MCGuiStep("the search window still the active one; Up", [] {
        GCHECK(gtk_window_is_active(panelWindow()));
        GCHECK(!gtk_window_is_active(MCGuiApp().window));
        MCGuiKey("up");
    });
    MCGuiWait("the row above b.txt's: a.txt's last", 10, [] {
        return keyboardInResults() && selected() == rowOf(d4("a.txt"), 3);
    });

    // ---- dismissed rows leave the results, and Replace All leaves them alone
    MCGuiStep("a.txt:2's ×", [] { MCGuiClick(dismissOf("a.txt:2  cat cat two")); });
    MCGuiWait("its row gone, its two matches out of the count", 10, [] {
        return rowOf(d4("a.txt"), 2) < 0 && panel()->matches().size() == 5;
    });
    MCGuiStep("said so", [] {
        GCHECK_EQ_STR(panel()->statusText(), "5 matches in 4 files (dismissed rows are left alone)");
        GCHECK(panel()->dismissed().lines.count(d4("a.txt")) == 1);
    });
    MCGuiStep("b.txt's right click menu", [] { MCGuiClick(rowLabel("b.txt:1  a cat"), -1, -1, 3); });
    MCGuiClickWhenReady("Dismiss File", [] {
        return panel()->rowMenu() ? MCGuiFindButton(panel()->rowMenu(), "Dismiss File") : nullptr;
    });
    MCGuiWait("b.txt's rows gone, the file dismissed", 10, [] {
        return rowOf(d4("b.txt"), 1) < 0 && panel()->dismissed().excludesFile(d4("b.txt")) &&
               !panel()->rowMenu();
    });
    MCGuiStep("d.txt's row selected, the keyboard in the results; Delete", [] {
        GCHECK_EQ_STR(panel()->statusText(), "4 matches in 3 files (dismissed rows are left alone)");
        selectRow(rowOf(d4("d.txt"), 1));
        MCGuiKey("delete");
    });
    MCGuiWait("d.txt's row gone, the keyboard still in the results on the last row", 10, [] {
        return rowOf(d4("d.txt"), 1) < 0 && panel()->matches().size() == 3 &&
               keyboardInResults() && selected() == 2;
    });
    MCGuiStep("Up from there", [] {
        GCHECK_EQ_STR(panel()->statusText(), "3 matches in 2 files (dismissed rows are left alone)");
        MCGuiKey("up");
    });
    MCGuiWait("the row above", 10, [] { return keyboardInResults() && selected() == 1; });
    MCGuiStep("Ctrl+Alt+Enter: Replace All", [] { MCGuiKey("ctrl+alt+enter"); });
    MCGuiWait("it asks about what is left", 10, [] { return alertAsks("Replace 3 matches in 2 files?"); });
    MCGuiStep("the dismissed matches counted out in the question; Replace", [] {
        GCHECK(MCGuiFindLabel(GTK_WIDGET(alert()),
                              "Each becomes “dog”. Files open in a tab are changed there, unsaved, "
                              "and can be undone there; the others are written to disk. 4 matches "
                              "in dismissed rows are left alone.") != nullptr);
        MCGuiSetKeyWindow(alert());
        MCGuiClick(MCGuiFindButton(GTK_WIDGET(alert()), "Replace"));
    });
    MCGuiWait("replaced", 10, [] { return idle() && MCGuiReadFile(d4("c.txt")) == "dog c\n"; });
    MCGuiStep("the dismissed line, file and row untouched, the rest replaced", [] {
        MCGuiSetKeyWindow(panelWindow());
        GCHECK_EQ_STR(MCGuiReadFile(d4("a.txt")), "dog one\ncat cat two\ndog three\n");
        GCHECK_EQ_STR(MCGuiReadFile(d4("b.txt")), "a cat\n");
        GCHECK_EQ_STR(MCGuiReadFile(d4("d.txt")), "cat d\n");
        // b.txt's preview tab, clean, left as it was too.
        GCHECK_EQ_STR(tabText(d4("b.txt")), "a cat\n");
        GCHECK(!tabDirty(d4("b.txt")));
        MCGuiNote("status: %s", panel()->statusText().c_str());
        GCHECK(panel()->statusText().rfind("Replaced 3 matches in 2 files. Now ", 0) == 0);
        // The search after it shows every row again.
        GCHECK(panel()->dismissed().empty());
    });
    MCGuiWait("the rows still matching, all shown", 10, [] {
        return idle() && panel()->matches().size() == 3 && rowOf(d4("a.txt"), 2) >= 0 &&
               rowOf(d4("b.txt"), 1) >= 0 && rowOf(d4("d.txt"), 1) >= 0;
    });

#ifdef MUTIBLE_ENABLE_TERMINAL
    // ---- a pane's key in the search window: the editor window takes the keyboard
    MCGuiStep("Ctrl+` in the search window", [] {
        terminalShownBefore = MCGuiApp().layout->terminalShown();
        MCGuiSetKeyWindow(panelWindow());
        MCGuiClick(panel()->replaceEntry());
        MCGuiKey("ctrl+`");
    });
    MCGuiWait("the editor window active, its terminal shown with the keyboard", 10, [] {
        MCGuiAppWindow a = MCGuiApp();
        return a.terminal && gtk_window_is_active(a.window) && a.layout->terminalShown() &&
               focusIn(a.terminal->widget());
    });
    MCGuiStep("the replacement as it was; Ctrl+` there again shows it still", [] {
        GCHECK_EQ_STR(gtk_editable_get_text(GTK_EDITABLE(panel()->replaceEntry())), "dog");
        MCGuiSetKeyWindow(panelWindow());
        MCGuiKey("ctrl+`");
    });
    MCGuiPause(0.5);   // a negative check: from the search window it never hides the terminal
    MCGuiStep("shown, and the keyboard in it", [] {
        MCGuiAppWindow a = MCGuiApp();
        GCHECK(a.layout->terminalShown());
        GCHECK(focusIn(a.terminal->widget()));
        GCHECK(gtk_window_is_active(a.window));
        if (!terminalShownBefore) a.layout->toggleTerminal();
        // The search window over the editor's again, as a window manager
        // keeps a transient one.
        MCGuiSetKeyWindow(panelWindow());
        MCGuiFocusKeyWindow();
    });
#endif

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
    // The X focus back on the app's window, not on the one just closed.
    MCGuiStep("the keyboard back in the app's window", [] { MCGuiFocusKeyWindow(); });
    MCGuiWait("active", 10, [] { return gtk_window_is_active(MCGuiApp().window); });
}
