// CsvHeaderSuite.cpp — the CSV table's header row
// (`./build/mutible-guitest csv_header`): a text first row over numbers
// shown as the columns' names, a table of text alone lettered A, B, C, the
// right click menu's First Row Is Header turning it on and off both ways, the
// choice coming back when the file is opened again, and an empty header
// field keeping its letter. Rows and columns of the table stay the file's.
#include "Harness.h"

#include "CsvTable.h"
#include "DocumentView.h"
#include "Editor.h"

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }
CsvTable* tbl() { return ed()->csvTable(); }

// The titles of the first `n` columns, joined by "|".
std::string titles(size_t n) {
    std::string s;
    for (size_t c = 0; c < n; c++) s += (c ? "|" : "") + tbl()->columnTitle(c);
    return s;
}

bool shows(const std::string& name) {
    return ed()->active() && ed()->active()->path() == F(name) && ed()->showsTable();
}

// The menu item chosen: its check set to `on`, as a click on it does.
void choose(bool on) { gtk_check_button_set_active(GTK_CHECK_BUTTON(tbl()->headerMenuItem()), on); }

}  // namespace

MC_GUI_SUITE(csv_header) {
    MCGuiStep("a numeric table under names, a table of text, one with an empty name", [] {
        ed()->closeAllTabs();
        MCGuiWriteFile(F("h1.csv"), "id,name,score\n1,Ann,90\n2,Bob,85\n");
        MCGuiWriteFile(F("h2.csv"), "a,b\nc,d\n");
        MCGuiWriteFile(F("h3.csv"), "id,,score\n1,x,90\n2,y,85\n");
        MCGuiOpenFileKept(F("h1.csv"));
    });
    MCGuiWait("h1 as a table", 10, [] { return shows("h1.csv"); });
    MCGuiStep("the names are the column titles, the rows the file's", [] {
        MCGuiNote("titles: %s", titles(3).c_str());
        GCHECK_EQ_STR(titles(3), "id|name|score");
        GCHECK(tbl()->firstRowIsHeader());
        GCHECK_EQ_STR(std::to_string(tbl()->rows()), "3");
        GCHECK_EQ_STR(tbl()->cell(0, 1), "name");
        GCHECK(!ed()->dirty());
    });
    MCGuiStep("First Row Is Header off: the letters", [] { choose(false); });
    MCGuiWait("the letters", 10, [] { return titles(3) == "A|B|C"; });
    MCGuiStep("the file untouched, the choice off", [] {
        GCHECK(!tbl()->firstRowIsHeader());
        GCHECK_EQ_STR(std::to_string(tbl()->rows()), "3");
        GCHECK(!ed()->dirty());
        ed()->closeAllTabs();
        MCGuiOpenFileKept(F("h2.csv"));
    });
    MCGuiWait("h2 as a table", 10, [] { return shows("h2.csv"); });
    MCGuiStep("text alone is lettered", [] {
        MCGuiNote("titles: %s", titles(2).c_str());
        GCHECK_EQ_STR(titles(2), "A|B");
        GCHECK(!tbl()->firstRowIsHeader());
    });
    MCGuiStep("First Row Is Header on: its names", [] { choose(true); });
    MCGuiWait("the names", 10, [] { return titles(2) == "a|b"; });
    MCGuiStep("h2 closed and h1 opened again", [] {
        ed()->closeAllTabs();
        MCGuiOpenFileKept(F("h1.csv"));
    });
    MCGuiWait("h1 as a table again", 10, [] { return shows("h1.csv"); });
    MCGuiStep("h1 remembered as not a header", [] {
        GCHECK_EQ_STR(titles(3), "A|B|C");
        GCHECK(!tbl()->firstRowIsHeader());
        ed()->closeAllTabs();
        MCGuiOpenFileKept(F("h2.csv"));
    });
    MCGuiWait("h2 as a table again", 10, [] { return shows("h2.csv"); });
    MCGuiStep("h2 remembered as a header, and turned off again", [] {
        GCHECK_EQ_STR(titles(2), "a|b");
        GCHECK(tbl()->firstRowIsHeader());
        choose(false);
    });
    MCGuiWait("h2's letters", 10, [] { return titles(2) == "A|B"; });
    MCGuiStep("h3 opened", [] {
        ed()->closeAllTabs();
        MCGuiOpenFileKept(F("h3.csv"));
    });
    MCGuiWait("h3 as a table", 10, [] { return shows("h3.csv"); });
    // The guess leaves a first row with an empty field alone; the reader
    // turns it on.
    MCGuiStep("not guessed as a header, then chosen", [] {
        GCHECK(!tbl()->firstRowIsHeader());
        choose(true);
    });
    MCGuiWait("the names", 10, [] { return titles(1) == "id"; });
    MCGuiStep("an empty header field keeps its letter", [] {
        MCGuiNote("titles: %s", titles(3).c_str());
        GCHECK_EQ_STR(titles(3), "id|B|score");
        ed()->closeAllTabs();
    });
}
