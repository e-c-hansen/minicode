// CsvTableSuite.cpp — a CSV or TSV file as a table, and files that are not
// text (`./build/minicode-guitest csv_table`, linux/PARITY.md item 4.1):
// the cells as the core's Csv reads them, one cell edit changing one line of
// the text and the file, undo, the toggle to the text and back, an unedited
// file saved byte for byte, a cell being typed committed before the tab goes,
// and a binary file not read as text and a database in its viewer.
#include "Harness.h"

#include "CsvTable.h"
#include "Document.h"
#include "Editor.h"

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

std::string bufferText() {
    GtkTextBuffer* b = ed()->active()->buffer();
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* raw = gtk_text_buffer_get_text(b, &s, &e, TRUE);
    std::string t = raw;
    g_free(raw);
    return t;
}

GtkWidget* onShow() {
    GtkWidget* slot = gtk_widget_get_parent(gtk_widget_get_parent(ed()->textView()));
    return gtk_stack_get_visible_child(GTK_STACK(slot));
}

GtkWidget* labelAt(GtkWidget* w, gsize row, gsize col) {
    if (GTK_IS_EDITABLE_LABEL(w) && (gsize)g_object_get_data(G_OBJECT(w), "row") == row + 1 &&
        (gsize)g_object_get_data(G_OBJECT(w), "col") == col + 1)
        return w;
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* r = labelAt(c, row, col)) return r;
    return nullptr;
}

Document* gTsv = nullptr;

// A BOM, quoted fields, a CRLF row and a last line with no ending.
const char* kOdd = "\xEF\xBB\xBF" "name,note\r\n\"Ann\",\"a, b\"\r\nBob,\"say \"\"hi\"\"\"\ncy,z";

}  // namespace

MC_GUI_SUITE(csv_table) {
    MCGuiStep("a CSV, an odd CSV, a TSV, a text file, a database and a binary file", [] {
        ed()->closeAllTabs();
        MCGuiWriteFile(F("t.csv"), "id,name,score\n1,Ann,90\n2,Bob,85\n3,Cy,70\n");
        MCGuiWriteFile(F("odd.csv"), kOdd);
        MCGuiWriteFile(F("t.tsv"), "a\tb\n1\t2\n");
        MCGuiWriteFile(F("plain.txt"), "just text\n");
        MCGuiWriteFile(F("d.sqlite"), std::string("SQLite format 3\0rest of it", 26));
        MCGuiWriteFile(F("b.bin"), std::string("ab\0cd\1\2\3", 8));
        MCGuiOpenFileKept(F("t.csv"));
    });
    MCGuiWait("the table on show", 10, [] {
        return ed()->active() && ed()->showsTable() && onShow() == ed()->csvTable()->widget();
    });
    MCGuiStep("with the file's cells, and nothing changed", [] {
        CsvTable* t = ed()->csvTable();
        GCHECK_EQ_STR(std::to_string(t->rows()), "4");
        GCHECK_EQ_STR(std::to_string(t->columns()), "3");
        GCHECK_EQ_STR(t->cell(0, 2), "score");
        GCHECK_EQ_STR(t->cell(2, 1), "Bob");
        GCHECK(t->delimiter() == ',');
        GCHECK(!ed()->dirty());
    });
    MCGuiStep("one cell edited: one line of the text changes, in one undo step", [] {
        GCHECK(ed()->csvTable()->setCell(2, 2, "99"));
        GCHECK_EQ_STR(bufferText(), "id,name,score\n1,Ann,90\n2,Bob,99\n3,Cy,70\n");
        GCHECK(ed()->dirty());
    });
    MCGuiStep("which Ctrl+Z takes back, the table following", [] {
        ed()->csvTable()->onUndo(false);
        GCHECK_EQ_STR(bufferText(), "id,name,score\n1,Ann,90\n2,Bob,85\n3,Cy,70\n");
        GCHECK_EQ_STR(ed()->csvTable()->cell(2, 2), "85");
        GCHECK(!ed()->dirty());
        ed()->csvTable()->onUndo(true);
        GCHECK_EQ_STR(ed()->csvTable()->cell(2, 2), "99");
    });
    MCGuiStep("saved, only that line differs on disk", [] {
        std::string err;
        GCHECK(ed()->save(&err));
        GCHECK_EQ_STR(MCGuiReadFile(F("t.csv")), "id,name,score\n1,Ann,90\n2,Bob,99\n3,Cy,70\n");
        GCHECK(!ed()->dirty());
    });
    MCGuiStep("a value needing quotes is quoted, in the same line", [] {
        GCHECK(ed()->csvTable()->setCell(1, 1, "Ann, Jr"));
        GCHECK_EQ_STR(bufferText(), "id,name,score\n1,\"Ann, Jr\",90\n2,Bob,99\n3,Cy,70\n");
    });
    MCGuiStep("the preview key shows the text, and again the table", [] {
        ed()->togglePreview();
        GCHECK(!ed()->showsTable());
        GCHECK(onShow() != ed()->csvTable()->widget());
        GCHECK(ed()->isSource());
        ed()->togglePreview();
        GCHECK(ed()->showsTable());
        GCHECK_EQ_STR(ed()->csvTable()->cell(1, 1), "Ann, Jr");
    });
    MCGuiStep("the text edited by hand shows in the table", [] {
        ed()->togglePreview();
        GtkTextBuffer* b = ed()->active()->buffer();
        GtkTextIter s;
        gtk_text_buffer_get_start_iter(b, &s);
        gtk_text_buffer_insert(b, &s, "x,y,z\n", -1);
        ed()->togglePreview();
        GCHECK_EQ_STR(std::to_string(ed()->csvTable()->rows()), "5");
        GCHECK_EQ_STR(ed()->csvTable()->cell(0, 0), "x");
    });
    MCGuiStep("an unedited odd file (BOM, CRLF, quotes, no last newline) saved is the same bytes", [] {
        MCGuiOpenFileKept(F("odd.csv"));
    });
    MCGuiWait("its table", 10, [] { return ed()->showsTable() && ed()->active()->path() == F("odd.csv"); });
    MCGuiStep("with the cells decoded", [] {
        CsvTable* t = ed()->csvTable();
        GCHECK_EQ_STR(t->cell(1, 1), "a, b");
        GCHECK_EQ_STR(t->cell(2, 1), "say \"hi\"");
        GCHECK_EQ_STR(t->cell(3, 1), "z");
        GCHECK(!ed()->csvTable()->setCell(1, 0, "Ann"));   // the same text: no change
        GCHECK(!ed()->dirty());
    });
    MCGuiStep("one cell of it changes one line", [] {
        GCHECK(ed()->csvTable()->setCell(3, 1, "w"));
        GCHECK_EQ_STR(bufferText(), std::string(kOdd).substr(0, std::string(kOdd).size() - 1) + "w");
        std::string err;
        GCHECK(ed()->save(&err));
        GCHECK_EQ_STR(MCGuiReadFile(F("odd.csv")),
                      std::string(kOdd).substr(0, std::string(kOdd).size() - 1) + "w");
    });
    MCGuiStep("a TSV splits on tabs", [] { MCGuiOpenFileKept(F("t.tsv")); });
    MCGuiWait("its table", 10, [] { return ed()->showsTable() && ed()->active()->path() == F("t.tsv"); });
    MCGuiStep("with its cells", [] {
        GCHECK(ed()->csvTable()->delimiter() == '\t');
        GCHECK_EQ_STR(ed()->csvTable()->cell(1, 1), "2");
        GCHECK(ed()->csvTable()->setCell(1, 1, "5"));
        GCHECK_EQ_STR(bufferText(), "a\tb\n1\t5\n");
    });
    MCGuiWait("the cell on screen", 10, [] {
        return labelAt(ed()->csvTable()->widget(), 1, 1) != nullptr;
    });
    MCGuiStep("a cell being typed is committed into its own tab when another shows", [] {
        GtkWidget* l = labelAt(ed()->csvTable()->widget(), 1, 1);
        gtk_editable_label_start_editing(GTK_EDITABLE_LABEL(l));
        gtk_editable_set_text(GTK_EDITABLE(l), "7");
        GCHECK(ed()->csvTable()->editing());
        gTsv = ed()->active();
        MCGuiOpenFileKept(F("plain.txt"));
    });
    MCGuiWait("the text file", 10, [] { return !ed()->showsTable() && ed()->active()->path() == F("plain.txt"); });
    MCGuiStep("the tab switch left the TSV as it was and the text file alone", [] {
        GCHECK_EQ_STR(bufferText(), "just text\n");
        GCHECK(!ed()->dirty());
        GCHECK(gTsv->dirty());
        GtkTextIter a, b;
        gtk_text_buffer_get_bounds(gTsv->buffer(), &a, &b);
        char* raw = gtk_text_buffer_get_text(gTsv->buffer(), &a, &b, TRUE);
        GCHECK_EQ_STR(std::string(raw), "a\tb\n1\t7\n");
        g_free(raw);
        GCHECK(onShow() != ed()->csvTable()->widget());
    });
    MCGuiStep("a database and a binary file are not opened as text", [] {
        MCGuiOpenFileKept(F("d.sqlite"));
    });
    MCGuiWait("the database's viewer", 10, [] { return ed()->active() && ed()->active()->path() == F("d.sqlite"); });
    MCGuiStep("shown by the database viewer (sqlite_view checks it), not as text", [] {
        GCHECK(ed()->active()->kind() == Document::Kind::Database);
        GCHECK(bufferText().empty());
        MCGuiOpenFileKept(F("b.bin"));
    });
    MCGuiWait("the binary file's message", 10, [] { return ed()->active() && ed()->active()->path() == F("b.bin"); });
    MCGuiStep("with no text of the file in the buffer", [] {
        GCHECK(ed()->active()->kind() == Document::Kind::Message);
        GCHECK(bufferText().find("ab") == std::string::npos);
        ed()->closeAllTabs();
    });
}
