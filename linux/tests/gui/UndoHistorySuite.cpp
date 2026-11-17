// UndoHistorySuite.cpp — `./build/minicode-guitest undo_history`: the
// editor's undo, the core's UndoHistory (src/UndoHistory.h, SESSION-DESIGN.md
// stage 2) in place of GTK's buffer undo, with every key real: a run of
// typing (spaces and all) is one step named Typing, and one Ctrl+Z takes it
// back with the caret where the run began, Ctrl+Shift+Z puts it back; the
// caret moving away and back starts a new step; an edit at three places
// (MultiEdit, as Replace All makes it) is one step under its name; a clean
// tab following its file changed outside is one step, Reload from Disk,
// and undoing it gives back the text before while the steps before it
// stay; Ctrl+Y redoes; the right click menu's own Undo is named for the
// step and takes it back; and Ctrl+Z with nothing left changes nothing.
#include "Harness.h"

#include "DocumentView.h"
#include "Editor.h"
#include "MultiEdit.h"

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
Document* doc() { return ed()->active(); }
GtkTextBuffer* buf() { return ed()->buffer(); }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

const char* kBase = "lsp.enabled = false\n";
const std::string kFile = "alpha\nbeta\n";
const std::string kTyped = "alpha\nbeta\none two three";
const std::string kReplaced = "Alpha\nBeta\nONE two three";
const std::string kOutside = "Alpha\nBeta\nONE 2 three\nfour\n";

std::string text() { return MCGuiEditorText(); }

int caret() {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(buf(), &it, gtk_text_buffer_get_insert(buf()));
    return gtk_text_iter_get_offset(&it);
}

// What the right click menu's Undo (or Redo) is named now.
std::string menuLabel(bool redo) {
    ed()->refreshUndoMenu();
    return ed()->undoLabel(redo);
}

// A mapped popover under `root` holding a label `text`, or null.
GtkWidget* popoverWith(GtkWidget* root, const std::string& text) {
    if (!root || !gtk_widget_get_mapped(root)) return nullptr;
    if (GTK_IS_POPOVER(root)) return MCGuiFindLabel(root, text) ? root : nullptr;
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = popoverWith(c, text)) return hit;
    return nullptr;
}

}  // namespace

MC_GUI_SUITE(undo_history) {
    MCGuiStep("a file, the caret at its end", [] {
        ed()->closeAllTabs();
        MCGuiSetSettings(kBase);
        MCGuiWriteFile(F("undo_history.txt"), kFile);
        MCGuiOpenFileKept(F("undo_history.txt"));
    });
    MCGuiWait("on show", 10, [] { return ed()->currentPath() == F("undo_history.txt") && text() == kFile; });
    MCGuiStep("focused, at the end; nothing to undo", [] {
        gtk_widget_grab_focus(ed()->textView());
        MCGuiKey("ctrl+end");
        GCHECK(doc() && !doc()->canUndo() && !doc()->canRedo());
        GCHECK_EQ_STR(doc() ? doc()->undoName() : "?", "");
        GCHECK_EQ_STR(menuLabel(false), "_Undo");
        GCHECK(!gtk_text_buffer_get_can_undo(buf()));   // GTK's own stays off
    });
    MCGuiWait("the caret at the end", 5, [] { return caret() == (int)kFile.size(); });

    // ---- a run of typing is one step
    MCGuiStep("one two three typed", [] { MCGuiType("one two three"); });
    MCGuiWait("typed", 10, [] { return text() == kTyped; });
    MCGuiStep("one step, Typing; Ctrl+Z", [] {
        GCHECK_EQ_STR(doc()->undoName(), "Typing");
        GCHECK_EQ_STR(menuLabel(false), "_Undo Typing");
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("all of it back in one", 10, [] { return text() == kFile; });
    MCGuiStep("the caret where the run began, the step to redo; Ctrl+Shift+Z", [] {
        GCHECK(caret() == (int)kFile.size());
        GCHECK(!doc()->canUndo());
        GCHECK_EQ_STR(doc()->redoName(), "Typing");
        GCHECK_EQ_STR(menuLabel(true), "_Redo Typing");
        MCGuiKey("ctrl+shift+z");
    });
    MCGuiWait("put back", 10, [] { return text() == kTyped && caret() == (int)kTyped.size(); });

    // ---- the right click menu: GtkTextView's own Undo, named and working
    MCGuiStep("a right click in the text", [] { MCGuiClick(ed()->textView(), -1, -1, 3); });
    MCGuiWait("its menu, with Undo Typing", 10, [] {
        return popoverWith(GTK_WIDGET(MCGuiApp().window), "Undo Typing") != nullptr;
    });
    MCGuiStep("Undo Typing chosen", [] {
        GtkWidget* pop = popoverWith(GTK_WIDGET(MCGuiApp().window), "Undo Typing");
        GCHECK(pop != nullptr);
        if (pop) MCGuiClick(MCGuiFindLabel(pop, "Undo Typing"));
    });
    MCGuiWait("the typing taken back", 10, [] { return text() == kFile; });
    MCGuiStep("the menu gone; Ctrl+Shift+Z", [] {
        if (GtkWidget* pop = popoverWith(GTK_WIDGET(MCGuiApp().window), "Redo Typing"))
            gtk_popover_popdown(GTK_POPOVER(pop));
        gtk_widget_grab_focus(ed()->textView());
        MCGuiKey("ctrl+shift+z");
    });
    MCGuiWait("put back again", 10, [] { return text() == kTyped && caret() == (int)kTyped.size(); });

    // ---- the caret moving breaks the run
    MCGuiStep("Left, Right, then !", [] {
        MCGuiKey("left");
        MCGuiKey("right");
        MCGuiType("!");
    });
    MCGuiWait("typed", 10, [] { return text() == kTyped + "!"; });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("only the ! goes", 10, [] { return text() == kTyped; });
    MCGuiStep("and the run before is a step of its own", [] {
        GCHECK_EQ_STR(doc()->undoName(), "Typing");
    });

    // ---- an edit at three places is one step, under its name
    MCGuiStep("three replacements as one edit", [] {
        GCHECK(MultiEdit::apply(buf(), {{0, 1, "A"}, {6, 1, "B"}, {11, 3, "ONE"}}, true, "Replace All"));
    });
    MCGuiWait("all three", 5, [] { return text() == kReplaced; });
    MCGuiStep("named; Ctrl+Z", [] {
        GCHECK_EQ_STR(doc()->undoName(), "Replace All");
        GCHECK_EQ_STR(menuLabel(false), "_Undo Replace All");
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("all three back in one", 10, [] { return text() == kTyped; });
    MCGuiStep("Ctrl+Y redoes", [] { MCGuiKey("ctrl+y"); });
    MCGuiWait("all three again", 10, [] { return text() == kReplaced; });

    // ---- Reload from Disk: one step, and the steps before it stay
    MCGuiStep("saved", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("clean, on disk", 10, [] {
        return !doc()->dirty() && MCGuiReadFile(F("undo_history.txt")) == kReplaced;
    });
    MCGuiStep("the file changed outside", [] { MCGuiWriteFile(F("undo_history.txt"), kOutside); });
    MCGuiWait("the clean tab follows it", 10, [] { return text() == kOutside && !doc()->dirty(); });
    MCGuiStep("as one step, Reload from Disk; Ctrl+Z", [] {
        GCHECK_EQ_STR(doc()->undoName(), "Reload from Disk");
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("the text before the reload, unsaved", 10, [] { return text() == kReplaced && doc()->dirty(); });
    MCGuiStep("the step before it is still there; Ctrl+Z", [] {
        GCHECK_EQ_STR(doc()->undoName(), "Replace All");
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("the replacements gone", 10, [] { return text() == kTyped; });
    MCGuiStep("Ctrl+Shift+Z twice", [] {
        MCGuiKey("ctrl+shift+z");
        MCGuiKey("ctrl+shift+z");
    });
    MCGuiWait("as on disk again, clean", 10, [] { return text() == kOutside && !doc()->dirty(); });

    // ---- nothing left: nothing changes
    MCGuiStep("every step undone", [] {
        int n = 0;
        while (doc()->canUndo() && n++ < 20) doc()->undo();
        GCHECK(!doc()->canUndo());
        GCHECK_EQ_STR(text(), kFile);
    });
    MCGuiStep("one Ctrl+Z more", [] { MCGuiKey("ctrl+z"); });
    // Time is the condition: an undo would have come at once.
    MCGuiPause(0.3);
    MCGuiStep("the text as it was, everything to redo", [] {
        GCHECK_EQ_STR(text(), kFile);
        GCHECK(doc()->canRedo());
    });
    MCGuiStep("every step redone: as on disk, clean", [] {
        int n = 0;
        while (doc()->canRedo() && n++ < 20) doc()->redo();
        GCHECK_EQ_STR(text(), kOutside);
        GCHECK(!doc()->dirty());
    });
    MCGuiStep("the tab closes", [] { ed()->closeAllTabs(); });
}
