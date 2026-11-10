// AutoSaveSuite.cpp — editor.auto-save (`./build/minicode-guitest auto_save`,
// linux/PARITY.md item 1.3), the counterpart of the Mac's checks in
// tests/gui/SaveSuite.mm and LooseEndsSuite.mm, by real key presses:
// after-delay writes once typing pauses; focus-change writes when the
// editor loses the keyboard and when another tab is shown, and not before;
// closing a tab writes it first instead of asking; a file changed on disk
// since it was read is not written over, and the status bar names it; one
// deleted outside is not made again; text an input method is still
// composing waits; and with the setting off nothing is written.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"

#include <glib/gstdio.h>

#include <string>

namespace {

std::string F(const std::string& name) { return MCGuiProjectFile("autosave/" + name); }
Editor* ed() { return MCGuiApp().editor; }
std::string Status() { return gtk_label_get_text(GTK_LABEL(MCGuiApp().statusLabel)); }

bool QuestionUp() {
    GtkWindow* top = MCGuiTopWindow();
    if (top == MCGuiApp().window) return false;
    GtkWidget* b = MCGuiFindButton(GTK_WIDGET(top), "Cancel");
    if (!b) b = MCGuiFindButton(GTK_WIDGET(top), "Keep Mine");
    return b && gtk_widget_get_width(b) > 0;
}

void Answer(const char* button) { MCGuiClick(MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), button)); }

// A file in a tab of its own, on show, with the keyboard in its text.
void OpenForTyping(const std::string& name) {
    MCGuiStep("open " + name, [name] { MCGuiOpenFileKept(F(name)); });
    MCGuiWait(name + " on show", 10, [name] { return ed()->currentPath() == F(name); });
    MCGuiStep("the keyboard in its text, at its start", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+Home");
    });
    MCGuiWait("the editor has it", 10, [] { return gtk_widget_has_focus(ed()->textView()); });
}

}  // namespace

MC_GUI_SUITE(auto_save) {
    MCGuiStep("files, and auto-save after 200 ms", [] {
        MCGuiSetKeyWindow(nullptr);
        for (const char* n : {"delay.txt", "focus.txt", "tab.txt", "close.txt", "outside.txt",
                              "other.txt", "ime.txt", "off.txt", "witness.txt", "deleted.txt"})
            MCGuiWriteFile(F(n), std::string(n) + "\n");
        ed()->closeAllTabs();
        MCGuiSetSettings("lsp.enabled = false\neditor.auto-save = after-delay\n"
                         "editor.auto-save-delay = 200\n");
    });

    // ---- after-delay
    OpenForTyping("delay.txt");
    MCGuiStep("type", [] {
        MCGuiKey("ctrl+End");
        MCGuiType("typed");
    });
    MCGuiWait("written once typing pauses", 10, [] {
        return MCGuiReadFile(F("delay.txt")) == "delay.txt\ntyped";
    });
    MCGuiStep("and the tab is clean", [] { GCHECK(!ed()->dirty()); });

    // ---- off: nothing written
    MCGuiStep("auto-save off", [] { MCGuiSetSettings("lsp.enabled = false\n"); });
    OpenForTyping("off.txt");
    MCGuiStep("type", [] { MCGuiType("x"); });
    MCGuiWait("unsaved", 10, [] { return ed()->dirty(); });
    MCGuiPause(0.6);   // a negative check: nothing is written in this time
    MCGuiStep("nothing written", [] {
        GCHECK_EQ_STR(MCGuiReadFile(F("off.txt")), "off.txt\n");
        GCHECK(ed()->dirty());
    });

    // ---- focus-change
    MCGuiStep("auto-save on focus change", [] {
        MCGuiSetSettings("lsp.enabled = false\neditor.auto-save = focus-change\n");
    });
    OpenForTyping("focus.txt");
    MCGuiStep("type", [] { MCGuiType("k"); });
    MCGuiWait("unsaved", 10, [] { return ed()->dirty(); });
    MCGuiPause(0.6);   // a negative check: not written while the editor keeps the keyboard
    MCGuiStep("not written yet; Ctrl+0 takes the keyboard to the tree", [] {
        GCHECK_EQ_STR(MCGuiReadFile(F("focus.txt")), "focus.txt\n");
        MCGuiKey("ctrl+0");
    });
    MCGuiWait("written when the editor lost the keyboard", 10, [] {
        return MCGuiReadFile(F("focus.txt")) == "kfocus.txt\n";
    });
    MCGuiStep("and off.txt with it, every tab with unsaved changes", [] {
        GCHECK_EQ_STR(MCGuiReadFile(F("off.txt")), "xoff.txt\n");
        GCHECK(ed()->dirtyDocuments().empty());
    });
    OpenForTyping("tab.txt");
    MCGuiStep("type, then the tab before it, by its key", [] {
        MCGuiType("t");
        MCGuiKey("ctrl+pageup");
    });
    MCGuiWait("written when another tab is shown", 10, [] {
        return MCGuiReadFile(F("tab.txt")) == "ttab.txt\n" && ed()->currentPath() == F("focus.txt");
    });

    // ---- the close question runs it first
    OpenForTyping("close.txt");
    MCGuiStep("type, then Ctrl+W", [] {
        MCGuiType("c");
        MCGuiKey("ctrl+w");
    });
    MCGuiWait("the tab closes, written, and nothing was asked", 10, [] {
        return !ed()->documentFor(F("close.txt")) && MCGuiReadFile(F("close.txt")) == "cclose.txt\n";
    });
    MCGuiStep("no question", [] { GCHECK(!QuestionUp()); });

    // ---- a file changed on disk is not written over
    MCGuiStep("auto-save off, to set up", [] { MCGuiSetSettings("lsp.enabled = false\n"); });
    OpenForTyping("outside.txt");
    MCGuiStep("an edit", [] { MCGuiType("mine "); });
    MCGuiWait("unsaved", 10, [] { return ed()->dirty(); });
    OpenForTyping("other.txt");
    MCGuiStep("the first changed on disk while another tab is shown", [] {
        MCGuiWriteFile(F("outside.txt"), "from outside\n");
    });
    MCGuiPause(0.6);   // a negative check: a tab not on show is not asked about
    MCGuiStep("after-delay again, and an edit in the tab on show", [] {
        GCHECK(!QuestionUp());
        MCGuiSetSettings("lsp.enabled = false\neditor.auto-save = after-delay\n"
                         "editor.auto-save-delay = 200\n");
        MCGuiType("o");
    });
    MCGuiWait("the tab on show is written", 10, [] {
        return MCGuiReadFile(F("other.txt")) == "oother.txt\n";
    });
    MCGuiStep("the other is not, and the status bar says so", [] {
        GCHECK_EQ_STR(MCGuiReadFile(F("outside.txt")), "from outside\n");
        Document* outside = ed()->documentFor(F("outside.txt"));
        GCHECK(outside && outside->dirty());
        GCHECK_EQ_STR(Status(), "Not auto-saved: outside.txt changed on disk.");
        MCGuiKey("ctrl+pageup");
    });
    MCGuiWait("shown, it asks", 10, QuestionUp);
    // The question does not stop the main loop. Showing the tab started the
    // delay's timer, and its auto-save came while the question was up: it
    // wrote the tab's edits over the very change asked about, and Reload
    // then read them back ("mine outside.txt", in loaded runs, where the
    // click came after the 200 ms).
    MCGuiPause(0.6);   // a negative check: the delay passes with the question up
    MCGuiStep("not written while it asks", [] {
        GCHECK_EQ_STR(MCGuiReadFile(F("outside.txt")), "from outside\n");
        GCHECK(QuestionUp());
    });
    MCGuiStep("Reload", [] { Answer("Reload"); });
    MCGuiWait("the disk's text, clean", 10, [] {
        return MCGuiEditorText() == "from outside\n" && !ed()->dirty();
    });

    // ---- a file deleted outside is not written again
    MCGuiStep("auto-save off, to set up", [] { MCGuiSetSettings("lsp.enabled = false\n"); });
    OpenForTyping("witness.txt");
    MCGuiStep("an edit", [] { MCGuiType("w"); });
    MCGuiWait("unsaved", 10, [] { return ed()->dirty(); });
    OpenForTyping("deleted.txt");
    MCGuiStep("an edit", [] { MCGuiType("d"); });
    MCGuiWait("unsaved", 10, [] { return ed()->dirty(); });
    MCGuiStep("its file deleted outside", [] { g_remove(F("deleted.txt").c_str()); });
    MCGuiPause(0.4);   // a negative check: the deletion is seen and nothing is asked
    MCGuiStep("after-delay again", [] {
        GCHECK(!QuestionUp());
        MCGuiSetSettings("lsp.enabled = false\neditor.auto-save = after-delay\n"
                         "editor.auto-save-delay = 200\n");
    });
    MCGuiWait("the other tab is written", 10, [] {
        return MCGuiReadFile(F("witness.txt")) == "wwitness.txt\n";
    });
    MCGuiStep("by the same auto-save, the deleted file is not made again", [] {
        GCHECK(!MCGuiFileExists(F("deleted.txt")));
        Document* d = ed()->documentFor(F("deleted.txt"));
        GCHECK(d && d->dirty());
        if (d) ed()->closeTab(d);   // let go, unasked
    });

    // ---- an input method composing
    OpenForTyping("ime.txt");
    MCGuiStep("type, then a dead key, which an input method composes", [] {
        MCGuiType("z");
        MCGuiKey("dead_acute");
    });
    MCGuiWait("composing", 10, [] { return ed()->composing(); });
    MCGuiPause(0.6);   // a negative check: the 200 ms delay passes while it composes
    MCGuiStep("not written while it composes; the letter ends it", [] {
        GCHECK_EQ_STR(MCGuiReadFile(F("ime.txt")), "ime.txt\n");
        GCHECK(ed()->dirty());
        MCGuiKey("e");
    });
    MCGuiWait("written with what it composed", 10, [] {
        return MCGuiReadFile(F("ime.txt")) == "zéime.txt\n";
    });
    MCGuiStep("auto-save off again", [] {
        MCGuiSetSettings("lsp.enabled = false\n");
        ed()->closeAllTabs();
    });
}
