// FindBarSuite.cpp — the editor's find and replace bar
// (`./build/minicode-guitest find_bar`, linux/PARITY.md item 1.6), the
// counterpart of the Mac's tests/gui/FindBarSuite.mm, pressing real keys:
// Ctrl+F opens it with the keyboard in the query, typing finds as you go
// and selects the match from the caret, Enter, F3 and their Shift forms
// step on and back and wrap, Alt+C, Alt+W and Alt+R narrow the matches, a
// regex with groups goes into the replacement, Ctrl+H shows the replace
// row, Enter there replaces one and moves on, Ctrl+Alt+Enter replaces all
// as one undo step, and Escape closes the bar from the bar or the editor.
// Then Ctrl+F's seed (a one-line selection, the word at the caret, nothing
// for two lines, escaped for a regex), "No results" in red, Find in
// Selection (Alt+L) with Preserve Case (Alt+P), a pattern left to
// std::regex running out of time, the query's keystrokes over 8,000,000
// characters copying no text, Replace All of 20,000 matches in a
// 100,000-line file within a CPU bound, and with the terminal focused the
// option keys going to the program as Meta.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "FindBar.h"
#include "Keys.h"
#include "MultiEdit.h"
#include "TextFind.h"

#ifdef MINICODE_ENABLE_TERMINAL
#include "Terminal.h"
#include <vte/vte.h>
#endif

#include <algorithm>
#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
FindBar* bar() { return MCGuiApp().findBar; }
GtkTextBuffer* buf() { return ed()->buffer(); }

std::string wordsPath() { return MCGuiProjectFile("find/words.txt"); }

// Per line: "alpha" twice, "Alpha" once, "alphabet" once.
std::string wordsText() {
    std::string s;
    for (int i = 0; i < 50; i++) s += std::to_string(i) + " alpha Alpha alphabet alpha.\n";
    return s;
}
const std::string kOriginal = wordsText();

// Offsets in the ASCII text are bytes and characters alike.
int at(const std::string& needle) { return (int)MCGuiEditorText().find(needle); }

void selectRange(int a, int b) {
    GtkTextIter s, e;
    gtk_text_buffer_get_iter_at_offset(buf(), &s, a);
    gtk_text_buffer_get_iter_at_offset(buf(), &e, b);
    gtk_text_buffer_select_range(buf(), &e, &s);
}

void caretAt(int a) { selectRange(a, a); }

void selection(int* a, int* b) {
    GtkTextIter s, e;
    gtk_text_buffer_get_selection_bounds(buf(), &s, &e);
    *a = gtk_text_iter_get_offset(&s);
    *b = gtk_text_iter_get_offset(&e);
}

std::string selected() {
    GtkTextIter s, e;
    gtk_text_buffer_get_selection_bounds(buf(), &s, &e);
    char* t = gtk_text_buffer_get_text(buf(), &s, &e, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

bool settled(size_t n) { return bar()->resultsCurrent() && bar()->matches().size() == n; }

size_t count(const std::string& text, const std::string& word) {
    size_t n = 0;
    for (size_t p = text.find(word); p != std::string::npos; p = text.find(word, p + 1)) n++;
    return n;
}

bool editorHasKeyboard() { return gtk_widget_has_focus(ed()->textView()); }

// The query typed over what is in the field, as a hand would.
void retype(const std::string& text) {
    MCGuiKey("ctrl+a");
    MCGuiType(text);
}

std::string statusText() { return gtk_label_get_text(GTK_LABEL(MCGuiApp().statusLabel)); }

void setOption(GtkWidget* toggle, bool on) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), on);
}

std::string bigPath() { return MCGuiProjectFile("find/big.py"); }
std::string bigText;
std::string longPath() { return MCGuiProjectFile("find/long.txt"); }
size_t gCopies = 0;
double gCpu = 0;
double gReplaceBase = 0;

// What Replace All's edits cost at the least on this thread, now: the same
// replacements of "needle" through MultiEdit into a plain buffer of the
// same text, with no view, highlighter, find bar, or selection clipboard.
double replaceAllBaseline() {
    GtkTextBuffer* b = gtk_text_buffer_new(nullptr);
    gtk_text_buffer_set_text(b, bigText.data(), (int)bigText.size());
    std::vector<MultiEdit::Change> changes;
    // The text is ASCII, so byte offsets are character offsets.
    for (size_t p = bigText.find("needle"); p != std::string::npos; p = bigText.find("needle", p + 6))
        changes.push_back({p, 6, "pin"});
    const double c0 = MCGuiThreadCpuMs();
    MultiEdit::apply(b, changes, true);
    const double ms = MCGuiThreadCpuMs() - c0;
    g_object_unref(b);
    return ms;
}
int gChars = 0;

#ifdef MINICODE_ENABLE_TERMINAL
VteTerminal* findVte(GtkWidget* root) {
    if (!root) return nullptr;
    if (VTE_IS_TERMINAL(root)) return VTE_TERMINAL(root);
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (VteTerminal* hit = findVte(c)) return hit;
    return nullptr;
}

std::string terminalText() {
    VteTerminal* vte = findVte(MCGuiApp().terminal->widget());
    if (!vte) return "";
    char* text = vte_terminal_get_text_format(vte, VTE_FORMAT_TEXT);
    std::string out = text ? text : "";
    g_free(text);
    return out;
}

bool gTerminalWasOpen = false;

bool terminalHasKeyboard() {
    return MCGuiApp().terminal->owns(gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)));
}

std::vector<std::string> accels(const char* action) {
    std::vector<std::string> out;
    gchar** list = gtk_application_get_accels_for_action(MCGuiApplication(), action);
    for (gchar** a = list; a && *a; a++) out.push_back(*a);
    g_strfreev(list);
    return out;
}
#endif

}  // namespace

MC_GUI_SUITE(find_bar) {
    MCGuiStep("a file of words, kept in a tab; nothing unsaved, the options off", [] {
        if (ed()->dirty()) ed()->save();
        MCGuiWriteFile(wordsPath(), kOriginal);
        MCGuiOpenFileKept(wordsPath());
        // An earlier suite may have flipped an option (Alt+C outside Markdown).
        bar()->close();
        for (GtkWidget* t : {bar()->caseButton(), bar()->wordButton(), bar()->regexButton(),
                             bar()->preserveButton()})
            setOption(t, false);
    });
    MCGuiWait("it shows", 10, [] {
        return ed()->currentPath() == wordsPath() && MCGuiEditorText() == kOriginal;
    });
    MCGuiStep("the keyboard in the text", [] { MCGuiClick(ed()->textView()); });
    MCGuiWait("the editor has it", 5, editorHasKeyboard);
    MCGuiStep("the caret before \"10 alpha\"; Ctrl+F; the query typed", [] {
        caretAt(at("10 alpha"));
        GCHECK(!bar()->shown());
        MCGuiKey("ctrl+f");
    });
    MCGuiWait("the bar is up with the keyboard in its query", 5, [] {
        return bar()->shown() && gtk_widget_get_mapped(bar()->widget()) && bar()->editing() &&
               gtk_widget_has_focus(bar()->findField()) && bar()->resultsCurrent();
    });
    MCGuiStep("\"alpha\" typed over the seed", [] {
        MCGuiNote("seeded with \"%s\" (the word at the caret)", bar()->query().c_str());
        // The seed stays selected while its matches are found and one is
        // selected in the text, so typing replaces it.
        int s0 = -1, e0 = -1;
        GCHECK(gtk_editable_get_selection_bounds(GTK_EDITABLE(bar()->findField()), &s0, &e0));
        GCHECK(s0 == 0 && e0 == 2);
        GCHECK_EQ_STR(bar()->query(), "10");
        MCGuiType("alpha");
    });
    // Highlighted means the selected match carries its tag. The seed's one
    // match ("10") was tagged a moment before, so highlighted() > 0 held as
    // soon as the new matches arrived, and they are painted from an idle,
    // which a loaded run reached only after the step below had looked: its
    // two tag checks failed. (Here highlighted() > 0 held before the paint in
    // 1 run of 6.)
    MCGuiWait("200 matches, case ignored, highlighted", 5, [] {
        int a = 0, b = 0;
        selection(&a, &b);
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_offset(buf(), &it, a + 1);
        GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buf());
        return settled(200) && bar()->highlighted() > 0 &&
               gtk_text_iter_has_tag(&it, gtk_text_tag_table_lookup(table, "minicode-find-current"));
    });
    MCGuiStep("the first match after the caret is selected; the count says so", [] {
        int a = 0, b = 0;
        selection(&a, &b);
        GCHECK(a == at("10 alpha") + 3 && b == a + 5);
        GCHECK(bar()->currentIndex() == 40);
        GCHECK_EQ_STR(bar()->countText(), "41 of 200");
        // Tags on the text: the selected match its own, the next the others'.
        GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buf());
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_offset(buf(), &it, a + 1);
        GCHECK(gtk_text_iter_has_tag(&it, gtk_text_tag_table_lookup(table, "minicode-find-current")));
        gtk_text_buffer_get_iter_at_offset(buf(), &it, at("10 alpha Alpha") + 10);
        GCHECK(gtk_text_iter_has_tag(&it, gtk_text_tag_table_lookup(table, "minicode-find-match")));
        GCHECK(bar()->highlighted() <= 200);
        MCGuiKey("enter");
    });
    MCGuiWait("Enter: the next", 5, [] { return bar()->currentIndex() == 41; });
    MCGuiStep("the keyboard stays in the query; F3 steps on too", [] {
        GCHECK_EQ_STR(selected(), "Alpha");
        GCHECK(gtk_widget_has_focus(bar()->findField()));
        MCGuiKey("f3");
    });
    MCGuiWait("the one after", 5, [] { return bar()->currentIndex() == 42; });
    MCGuiStep("Shift+F3 goes back", [] {
        GCHECK_EQ_STR(selected(), "alpha");   // the start of alphabet
        MCGuiKey("shift+f3");
    });
    MCGuiWait("back one", 5, [] { return bar()->currentIndex() == 41; });
    MCGuiStep("Shift+Enter goes back", [] { MCGuiKey("shift+enter"); });
    MCGuiWait("back to the first after the caret", 5, [] { return bar()->currentIndex() == 40; });
    MCGuiStep("from the last match, Enter wraps to the first", [] {
        const TextFind::Match& last = bar()->matches().back();
        selectRange((int)last.start, (int)(last.start + last.length));
        MCGuiKey("enter");
    });
    MCGuiWait("the first", 5, [] { return bar()->currentIndex() == 0; });
    MCGuiStep("and Shift+Enter from the first to the last", [] { MCGuiKey("shift+enter"); });
    MCGuiWait("the last", 5, [] { return bar()->currentIndex() == 199; });
    MCGuiStep("Match Case (Alt+C)", [] {
        GCHECK_EQ_STR(bar()->countText(), "200 of 200");
        MCGuiKey("alt+c");
    });
    MCGuiWait("150 matches", 5, [] { return settled(150); });
    MCGuiStep("Whole Word (Alt+W)", [] {
        GCHECK(bar()->matchCase());
        MCGuiKey("alt+w");
    });
    MCGuiWait("100 matches", 5, [] { return settled(100); });
    MCGuiStep("a regular expression (Alt+R), Match Case and Whole Word off", [] {
        GCHECK(bar()->wholeWord());
        MCGuiKey("alt+c");
        MCGuiKey("alt+w");
        MCGuiKey("alt+r");
        retype("alpha(bet)?\\.");
    });
    MCGuiWait("50 matches of the regex", 5, [] {
        return bar()->regex() && !bar()->matchCase() && !bar()->wholeWord() && settled(50);
    });
    MCGuiStep("a bad regex says so and finds nothing", [] { retype("alpha("); });
    MCGuiWait("no matches, in red", 5, [] {
        return bar()->query() == "alpha(" && bar()->matches().empty() &&
               bar()->countInErrorColor();
    });
    MCGuiStep("a regex with groups, and Ctrl+H for the replace row", [] {
        MCGuiNote("a bad regex: \"%s\"", bar()->countText().c_str());
        GCHECK(bar()->queryOutlined());
        GCHECK(!gtk_widget_get_sensitive(bar()->nextButton()));
        retype("(\\d+) (alpha)\\b");
        MCGuiKey("ctrl+h");
    });
    MCGuiWait("the replace row, with the keyboard", 5, [] {
        return bar()->replaceShown() && gtk_widget_has_focus(bar()->replaceField()) && settled(50);
    });
    MCGuiStep("$2-$1 replaces all (Ctrl+Alt+Enter): the groups swapped on every line", [] {
        MCGuiType("$2-$1");
        MCGuiKey("ctrl+alt+enter");
    });
    MCGuiWait("replaced", 10, [] { return MCGuiEditorText().rfind("alpha-0 Alpha", 0) == 0; });
    MCGuiStep("every line, as one undo step", [] {
        GCHECK(count(MCGuiEditorText(), "alpha-") == 50);
        GCHECK(ed()->dirty());
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("one Ctrl+Z puts it all back", 5, [] { return MCGuiEditorText() == kOriginal; });
    MCGuiStep("not unsaved any more; back to the bar, plain text \"alphabet\"", [] {
        GCHECK(!ed()->dirty());
        MCGuiKey("ctrl+h");
    });
    MCGuiWait("the replace field has the keyboard", 5, [] {
        return gtk_widget_has_focus(bar()->replaceField());
    });
    MCGuiStep("Regular Expression off; the query; \"omega\" for the replacement", [] {
        MCGuiKey("alt+r");
        MCGuiKey("ctrl+a");
        MCGuiType("omega");
        MCGuiKey("shift+tab");   // to the query
        retype("alphabet");
    });
    MCGuiWait("50 alphabets", 5, [] { return !bar()->regex() && settled(50); });
    MCGuiStep("Enter in the replacement replaces one and moves on", [] {
        MCGuiKey("tab");
        caretAt(at("3 alpha"));
        MCGuiKey("enter");   // no match selected: to the next one first
    });
    MCGuiWait("the next selected", 5, [] { return selected() == "alphabet"; });
    MCGuiStep("again: now it is replaced", [] {
        GCHECK(gtk_widget_has_focus(bar()->replaceField()));
        GCHECK(MCGuiEditorText() == kOriginal);
        MCGuiKey("enter");
    });
    MCGuiWait("one replaced", 5, [] { return settled(49); });
    MCGuiStep("unsaved; on line 3; the next alphabet selected; Replace All", [] {
        GCHECK(ed()->dirty());
        GCHECK(count(MCGuiEditorText(), "omega") == 1);
        GCHECK(MCGuiEditorText().find("3 alpha Alpha omega alpha.") != std::string::npos);
        GCHECK_EQ_STR(selected(), "alphabet");
        int a = 0, b = 0;
        selection(&a, &b);
        GCHECK(a == at("4 alpha Alpha alphabet") + 14);
        MCGuiKey("ctrl+alt+enter");
    });
    MCGuiWait("all replaced", 10, [] { return settled(0); });
    MCGuiStep("50 omegas; the note; one undo takes back Replace All, one more the first", [] {
        GCHECK(count(MCGuiEditorText(), "omega") == 50);
        GCHECK_EQ_STR(statusText(), "Replaced 49 matches");
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("one omega left", 5, [] { return count(MCGuiEditorText(), "omega") == 1; });
    MCGuiStep("and the first Replace", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("as it was", 5, [] { return MCGuiEditorText() == kOriginal; });
    MCGuiStep("Escape in the editor closes the bar; the editor keeps the keyboard", [] {
        GCHECK(!ed()->dirty());
        GCHECK(bar()->shown() && editorHasKeyboard());
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 5, [] { return !bar()->shown() && !gtk_widget_get_visible(bar()->widget()); });
    MCGuiStep("no highlight left behind", [] {
        GCHECK(editorHasKeyboard());
        GCHECK(bar()->highlighted() == 0);
    });

    // ---- Ctrl+F's seed
    MCGuiStep("Ctrl+F with \"Alpha\" selected on one line: it is the query", [] {
        const int a = at("Alpha");
        selectRange(a, a + 5);
        MCGuiKey("ctrl+f");
    });
    MCGuiWait("the bar, seeded", 5, [] { return bar()->shown() && bar()->query() == "Alpha"; });
    MCGuiStep("the keyboard in the query; the replace row as it was left; the chevron hides it", [] {
        GCHECK(gtk_widget_has_focus(bar()->findField()));
        GCHECK(bar()->replaceShown());
        MCGuiClick(bar()->chevronButton());
    });
    MCGuiWait("the replace row hidden; 200 matches of Alpha", 5, [] {
        return !bar()->replaceShown() && settled(200);
    });
    MCGuiStep("the selected Alpha is the second; tooltips name the keys", [] {
        GCHECK_EQ_STR(bar()->countText(), "2 of 200");
        const char* t = gtk_widget_get_tooltip_text(bar()->caseButton());
        GCHECK(t && std::string(t) == "Match Case (Alt+C)");
        t = gtk_widget_get_tooltip_text(bar()->replaceAllButton());
        GCHECK(t && std::string(t) == "Replace All (Ctrl+Alt+Enter)");
        t = gtk_widget_get_tooltip_text(bar()->selectionButton());
        GCHECK(t && std::string(t) == "Find in Selection (Alt+L)");
        t = gtk_widget_get_tooltip_text(bar()->chevronButton());
        GCHECK(t && std::string(t) == "Toggle Replace (Ctrl+H)");
        // Find in Selection is offered: there is a selection (the match).
        GCHECK(gtk_widget_get_sensitive(bar()->selectionButton()));
        MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "find-bar");
        retype("zebra");
    });
    MCGuiWait("nothing found", 5, [] { return bar()->query() == "zebra" && settled(0); });
    MCGuiStep("\"No results\" in red, the query outlined, no arrows", [] {
        GCHECK_EQ_STR(bar()->countText(), "No results");
        GCHECK(bar()->countInErrorColor() && bar()->queryOutlined());
        GCHECK(!gtk_widget_get_sensitive(bar()->previousButton()) &&
               !gtk_widget_get_sensitive(bar()->nextButton()));
        MCGuiSnapshot(bar()->widget(), "find-bar-no-results");
        retype("alpha");
    });
    MCGuiWait("alpha again", 5, [] { return bar()->query() == "alpha" && settled(200); });
    MCGuiStep("found again: no red; the keyboard to the editor", [] {
        GCHECK(!bar()->countInErrorColor() && !bar()->queryOutlined());
        MCGuiClick(ed()->textView());
    });
    MCGuiWait("the editor has it", 5, editorHasKeyboard);
    MCGuiStep("a selection over two lines leaves the query alone", [] {
        const int a = at("3 alpha");
        selectRange(a, a + 40);
        MCGuiKey("ctrl+f");
    });
    MCGuiWait("the query has the keyboard", 5, [] { return gtk_widget_has_focus(bar()->findField()); });
    MCGuiStep("still \"alpha\"; back in the editor", [] {
        GCHECK_EQ_STR(bar()->query(), "alpha");
        MCGuiClick(ed()->textView());
    });
    MCGuiWait("the editor has it", 5, editorHasKeyboard);
    MCGuiStep("no selection: the word at the caret (inside \"alphabet\")", [] {
        caretAt(at("7 alpha Alpha alphabet") + 17);
        MCGuiKey("ctrl+f");
    });
    MCGuiWait("50 alphabets, the one at the caret selected", 5, [] {
        return bar()->query() == "alphabet" && settled(50) && bar()->currentIndex() == 7;
    });
    MCGuiStep("with Regular Expression on, the selection is escaped", [] {
        MCGuiKey("alt+r");
        MCGuiClick(ed()->textView());
    });
    MCGuiWait("the editor has it", 5, [] { return editorHasKeyboard() && bar()->regex(); });
    MCGuiStep("\"alpha.\" selected; Ctrl+F", [] {
        const int a = at("alpha.\n");
        selectRange(a, a + 6);
        MCGuiKey("ctrl+f");
    });
    MCGuiWait("escaped", 5, [] { return bar()->query() == "alpha\\."; });
    MCGuiStep("Regular Expression off again; \"alpha\"", [] {
        MCGuiKey("alt+r");
        retype("alpha");
    });
    MCGuiWait("alpha", 5, [] { return !bar()->regex() && settled(200); });

    // ---- Find in Selection (Alt+L) and Preserve Case (Alt+P)
    MCGuiStep("lines 5 to 9 selected; Alt+L with the keyboard in the bar", [] {
        const int a = at("5 alpha"), b = at("10 alpha");
        selectRange(a, b);   // ends at the start of line 10
        GCHECK(bar()->editing());
        MCGuiKey("alt+l");
    });
    MCGuiWait("20 matches, all in the five lines", 5, [] {
        return bar()->findInSelection() && settled(20);
    });
    MCGuiStep("inside the scope, its end before the line break; then Preserve Case and Replace All", [] {
        int s = 0, e = 0;
        GCHECK(bar()->scope(&s, &e));
        GCHECK(s == at("5 alpha") && e == at("10 alpha") - 1);
        GCHECK(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bar()->selectionButton())));
        for (const TextFind::Match& m : bar()->matches())
            GCHECK((int)m.start >= s && (int)(m.start + m.length) <= e);
        MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "find-in-selection");
        MCGuiKey("alt+p");
        MCGuiKey("ctrl+h");
    });
    MCGuiWait("Preserve Case on; the replacement has the keyboard", 5, [] {
        return bar()->preserveCase() && gtk_widget_has_focus(bar()->replaceField());
    });
    MCGuiStep("\"omega\"; Replace All", [] {
        retype("omega");
        MCGuiKey("ctrl+alt+enter");
    });
    MCGuiWait("replaced in the scope only", 10, [] {
        return MCGuiEditorText().find("5 omega Omega omegabet omega.") != std::string::npos;
    });
    MCGuiStep("five lines changed, case kept; the rest alone; one undo", [] {
        const std::string t = MCGuiEditorText();
        GCHECK(count(t, "Omega") == 5);
        GCHECK(t.find("9 omega Omega omegabet omega.\n10 alpha Alpha") != std::string::npos);
        GCHECK(t.find("4 alpha Alpha alphabet alpha.\n5 omega") != std::string::npos);
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("as it was", 5, [] { return MCGuiEditorText() == kOriginal; });
    MCGuiStep("text typed before the scope moves it; Escape ends it", [] {
        int s = 0, e = 0;
        GCHECK(bar()->scope(&s, &e));
        caretAt(0);
        MCGuiType("xx");
    });
    MCGuiWait("typed", 5, [] { return MCGuiEditorText().rfind("xx0 alpha", 0) == 0; });
    MCGuiStep("the scope two on", [] {
        int s = 0, e = 0;
        GCHECK(bar()->scope(&s, &e));
        GCHECK(s == at("5 alpha") && e == at("10 alpha") - 1);
        // GTK may take typing back a piece at a time; nothing else is left
        // to undo, so a second press is harmless.
        MCGuiKey("ctrl+z");
        MCGuiKey("ctrl+z");
        MCGuiKey("escape");
    });
    MCGuiWait("the bar closed, the scope gone", 5, [] {
        return !bar()->shown() && !bar()->findInSelection() && MCGuiEditorText() == kOriginal;
    });
    MCGuiStep("Preserve Case off", [] {
        bar()->togglePreserveCase();
        GCHECK(!bar()->preserveCase());
        GCHECK(!ed()->dirty());
    });

    // ---- a pattern left to std::regex that runs past its time
    MCGuiStep("a backreference over a 20,000-letter line", [] {
        TextFind::setSlowPatternSeconds(0.05);
        GtkTextIter s;
        gtk_text_buffer_get_start_iter(buf(), &s);
        gtk_text_buffer_begin_user_action(buf());
        gtk_text_buffer_insert(buf(), &s, std::string(20000, 'a').c_str(), -1);
        gtk_text_buffer_end_user_action(buf());
        const int a = at("1 alpha");
        selectRange(a + 2, a + 7);   // a word to seed with, not the line of a's
        MCGuiKey("ctrl+f");
    });
    MCGuiWait("the bar", 5, [] { return bar()->editing(); });
    MCGuiStep("the pattern", [] {
        MCGuiKey("alt+r");
        retype("(a)(.*)\\1z");
    });
    MCGuiWait("\"Too slow\"", 10, [] {
        return bar()->resultsCurrent() && bar()->countText() == "Too slow";
    });
    MCGuiStep("nothing found, the reason in the tooltip; the text back as it was", [] {
        GCHECK(bar()->matches().empty());
        const char* tip = gtk_widget_get_tooltip_text(bar()->countLabel());
        GCHECK(tip && std::string(tip).find("two seconds") != std::string::npos);
        TextFind::setSlowPatternSeconds(2.0);
        MCGuiKey("alt+r");
        retype("alpha");
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("as it was", 5, [] { return MCGuiEditorText() == kOriginal && !bar()->regex(); });
    MCGuiStep("closed", [] { MCGuiKey("escape"); });
    MCGuiWait("shut", 5, [] { return !bar()->shown(); });

    // ---- a long text: the query's keystrokes copy nothing on the main thread
    MCGuiStep("8,000,000 characters in 100,000 lines; a query", [] {
        std::string t;
        t.reserve(8100000);
        const std::string row = std::string(73, 'x') + " alpha\n";   // 80 characters
        for (int k = 0; k < 100000; k++) t += k % 300 == 7 ? "lorem ipsum dolor alpha\n" : row;
        MCGuiWriteFile(longPath(), t);
        MCGuiOpenFileKept(longPath());
    });
    MCGuiWait("it shows", 20, [] { return ed()->currentPath() == longPath(); });
    MCGuiStep("Ctrl+F; \"alph\"", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+f");
    });
    MCGuiWait("the bar", 5, [] { return bar()->editing(); });
    MCGuiStep("typed", [] { retype("alph"); });
    MCGuiWait("found in the long text (the limit), highlighted", 20, [] {
        return bar()->query() == "alph" && bar()->resultsCurrent() &&
               bar()->matches().size() == FindRules::kMatchLimit && bar()->highlighted() > 0;
    });
    MCGuiStep("only the lines near the screen carry the tag", [] {
        MCGuiNote("%d of 20,000 matches tagged", bar()->highlighted());
        GCHECK(bar()->highlighted() < 1000);
        GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buf());
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line_offset(buf(), &it, 99990, 75);
        GCHECK(!gtk_text_iter_has_tag(&it, gtk_text_tag_table_lookup(table, "minicode-find-match")));
    });
    MCGuiStep("eight more queries: no copy of the text, little CPU on the main thread", [] {
        GCHECK_EQ_STR(bar()->countText().substr(bar()->countText().size() - 9), "of 20000+");
        gCopies = bar()->textCopies();
        const char* queries[] = {"alpha", "alph", "alpha", "lorem", "alpha", "lpha", "ipsum", "dolor"};
        const double c0 = MCGuiThreadCpuMs();
        for (const char* q : queries) gtk_editable_set_text(GTK_EDITABLE(bar()->findField()), q);
        const double ms = MCGuiThreadCpuMs() - c0;
        MCGuiNote("eight queries over 8,000,000 characters: %.2f ms of main-thread CPU, %zu copies",
                  ms, bar()->textCopies() - gCopies);
        GCHECK(bar()->textCopies() == gCopies);
        GCHECK_TIME(ms, 25, "eight queries over 8,000,000 characters");
    });
    MCGuiWait("current again", 20, [] { return bar()->resultsCurrent() && settled(334); });
    MCGuiStep("an edit of the text: one copy for the next search", [] {
        GtkTextIter s;
        gtk_text_buffer_get_start_iter(buf(), &s);
        gtk_text_buffer_insert(buf(), &s, "dolor ", -1);
    });
    MCGuiWait("the new match found", 20, [] { return settled(335); });
    MCGuiStep("one copy; put back; closed", [] {
        GCHECK(bar()->textCopies() == gCopies + 1);
        GtkTextIter s, e;
        gtk_text_buffer_get_start_iter(buf(), &s);
        gtk_text_buffer_get_iter_at_offset(buf(), &e, 6);
        gtk_text_buffer_delete(buf(), &s, &e);
        bar()->close();
    });

    // ---- Replace All of 20,000 matches in 100,000 lines, highlighted
    MCGuiStep("a 100,000-line Python file, every fifth line holding \"needle\"", [] {
        bigText.clear();
        bigText.reserve(2600000);
        for (int k = 0; k < 100000; k++)
            bigText += k % 5 == 0 ? "x = needle + " + std::to_string(k) + "  # one\n"
                                  : "y = " + std::to_string(k) + "\n";
        MCGuiWriteFile(bigPath(), bigText);
        MCGuiOpenFileKept(bigPath());
    });
    MCGuiWait("it shows", 20, [] { return ed()->currentPath() == bigPath() && MCGuiEditorText() == bigText; });
    MCGuiStep("Ctrl+H; \"needle\"", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+h");
    });
    MCGuiWait("the replace row", 5, [] { return bar()->replaceShown() && bar()->editing(); });
    MCGuiStep("the query and its replacement", [] {
        MCGuiKey("shift+tab");
        retype("needle");
        MCGuiKey("tab");
        retype("pin");
    });
    MCGuiWait("20,000 needles", 20, [] {
        return bar()->query() == "needle" && bar()->resultsCurrent() &&
               bar()->matches().size() == FindRules::kMatchLimit;
    });
    MCGuiStep("Replace All", [] {
        gReplaceBase = replaceAllBaseline();
        MCGuiKey("ctrl+alt+enter");
    });
    MCGuiWait("every needle replaced", 30, [] { return MCGuiEditorText().find("needle") == std::string::npos; });
    MCGuiStep("all 20,000, each its own edit, within a CPU bound", [] {
        const std::string t = MCGuiEditorText();
        GCHECK(count(t, "x = pin + ") == 20000);
        MCGuiNote("Replace All of 20,000 matches in 100,000 lines: %.0f ms of main-thread CPU "
                  "for the edits, %.0f ms on the worker to find them",
                  bar()->lastReplaceAllMs(), bar()->lastReplaceAllWorkerMs());
        // About 170 and 25 ms on the ThinkPad. While the selection stayed
        // during the edits it was close to a minute for twice as many
        // (MultiEdit.cpp), an X round trip a delete.
        //
        // On a loaded machine this thread is several times slower: 0.5 to
        // 0.9 s at a load of 5, and 1.5 and 1.6 s, over the old bound of
        // 1.5 s, in loaded runs. Not from MALLOC_PERTURB_'s filling of every
        // block, which meson and CI set: with and without it, 496 and 526 ms,
        // 586 and 532. So the
        // bound grows with the same edits put through MultiEdit into a plain
        // buffer just before, on this thread (the edits cost about 10 times
        // that, the view and every listener of the buffer included). With
        // the selection kept through them (MultiEdit's place_cursor taken out
        // to see) they took 9.3 s, 166 times it.
        const double ra = bar()->lastReplaceAllMs();
        MCGuiNote("the same edits in a plain buffer just before: %.0f ms (%.1f times)", gReplaceBase,
                  gReplaceBase > 0 ? ra / gReplaceBase : 0.0);
        GCHECK_TIME(ra, std::max(1500.0, 30 * gReplaceBase), "Replace All of 20,000 matches (main thread)");
        GCHECK_TIME(bar()->lastReplaceAllWorkerMs(), 1000, "Replace All of 20,000 matches (worker)");
        GCHECK_EQ_STR(statusText(), "Replaced 20000 matches");
        MCGuiClick(ed()->textView());
    });
    MCGuiWait("the editor has the keyboard", 5, editorHasKeyboard);
    MCGuiStep("one Ctrl+Z", [] {
        gCpu = MCGuiThreadCpuMs();
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("puts all 20,000 back", 30, [] {
        return gtk_text_buffer_get_char_count(buf()) == (int)bigText.size() &&
               MCGuiEditorText() == bigText;
    });
    MCGuiStep("clean again, within a bound; ten keys typed with the bar open", [] {
        const double ms = MCGuiThreadCpuMs() - gCpu;
        MCGuiNote("its undo: %.0f ms of main-thread CPU (GTK puts each edit back and the caret "
                  "after each)", ms);
        // About 1.6 s on the ThinkPad. With the highlighter following each
        // of the 40,000 edits GTK replays one by one, 6.2 s (Document.cpp).
        // On a loaded machine this thread is slower at everything: with three
        // runs at once and a load of 8 to 13 the undo took 2.5 to 4.6 s, and
        // the Replace All just before it 0.6 to 1.1 s, the undo always 3 to 5
        // times the Replace All (3 on CI). So the bound grows with the Replace
        // All measured on this thread moments ago: the regression above
        // makes the undo 12 to 16 times it.
        const double replaceAll = bar()->lastReplaceAllMs();
        MCGuiNote("the undo is %.1f times the Replace All", replaceAll > 0 ? ms / replaceAll : 0.0);
        GCHECK_TIME(ms, std::max(4500.0, 8 * replaceAll), "the undo of Replace All of 20,000 matches");
        GCHECK(!ed()->dirty());
        gChars = gtk_text_buffer_get_char_count(buf());
        caretAt(at("x = needle + 50000 "));
        gCpu = MCGuiThreadCpuMs();
        MCGuiType("abcdefghij");
    });
    MCGuiWait("typed", 10, [] { return gtk_text_buffer_get_char_count(buf()) == gChars + 10; });
    MCGuiStep("typing costs what it did; closed, the tab dropped", [] {
        const double ms = MCGuiThreadCpuMs() - gCpu;
        MCGuiNote("ten keys in a 100,000-line file with 20,000 matches found: %.0f ms of "
                  "main-thread CPU", ms);
        GCHECK_TIME(ms, 300, "ten keys typed with the find bar open over 20,000 matches");
        MCGuiKey("escape");
    });
    MCGuiWait("shut", 5, [] { return !bar()->shown(); });
    MCGuiStep("the big tabs closed unsaved", [] {
        for (const std::string& p : {bigPath(), longPath()})
            if (Document* d = ed()->documentFor(p)) ed()->closeTab(d);
    });

#ifdef MINICODE_ENABLE_TERMINAL
    // ---- the terminal keeps its Meta keys
    MCGuiStep("the terminal, with the keyboard", [] {
        // An earlier suite may have left it open.
        gTerminalWasOpen = gtk_widget_get_mapped(MCGuiApp().terminal->widget());
        if (gTerminalWasOpen) MCGuiClick(GTK_WIDGET(findVte(MCGuiApp().terminal->widget())));
        else MCGuiKey("ctrl+`");
    });
    MCGuiWait("it has it", 10, terminalHasKeyboard);
    MCGuiWait("the shell is up", 20, [] {
        return terminalText().find_first_not_of(" \n") != std::string::npos;
    });
    MCGuiStep("none of the options' keys is an accelerator there", [] {
        for (const char* a : {"win.findcase", "win.findword", "win.findregex", "win.findpreservecase",
                              "win.findinselection", "win.replace"})
            GCHECK(accels(a).empty());
        MCGuiType("stty raw -echo; head -c 10 > alt.bin; stty sane\n");
    });
    MCGuiWait("the program is reading", 20, [] { return MCGuiFileExists(MCGuiProjectFile("alt.bin")); });
    MCGuiStep("Alt+C, Alt+W, Alt+R, Alt+P and Alt+L", [] {
        for (const char* k : {"alt+c", "alt+w", "alt+r", "alt+p", "alt+l"}) MCGuiKey(k);
    });
    MCGuiWait("it got them all", 20, [] { return MCGuiReadFile(MCGuiProjectFile("alt.bin")).size() == 10; });
    MCGuiStep("as Meta (ESC and the letter), and no option flipped", [] {
        GCHECK(MCGuiReadFile(MCGuiProjectFile("alt.bin")) == "\x1b" "c\x1bw\x1br\x1bp\x1bl");
        GCHECK(!bar()->matchCase() && !bar()->wholeWord() && !bar()->regex() &&
               !bar()->preserveCase() && !bar()->findInSelection());
        if (!gTerminalWasOpen) MCGuiKey("ctrl+`");
        else MCGuiClick(ed()->textView());
    });
    MCGuiWait("the terminal put away", 10, [] { return !terminalHasKeyboard(); });
#endif
}
