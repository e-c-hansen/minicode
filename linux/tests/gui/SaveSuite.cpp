// SaveSuite.cpp — the path every edit takes to the disk, by real input
// (`./build/minicode-guitest save`): a click on a row in the tree opens the
// file, a click in the text and key presses edit it, and Ctrl+S goes through
// GTK's own accelerator matching to win.save. The disk is read directly.
#include "Harness.h"

#include "Editor.h"

static std::string notesPath() { return MCGuiProjectFile("notes.txt"); }

static GSimpleAction* saveAction() {
    return G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(MCGuiApp().window), "save"));
}

MC_GUI_SUITE(save) {
    MCGuiStep("a file in the project", [] { MCGuiWriteFile(notesPath(), "first line\n"); });
    MCGuiWait("the tree lists it", 10, [] {
        return MCGuiFindLabel(GTK_WIDGET(MCGuiApp().window), "notes.txt") != nullptr;
    });
    MCGuiStep("a click on its row opens it", [] {
        MCGuiClick(MCGuiFindLabel(GTK_WIDGET(MCGuiApp().window), "notes.txt"));
    });
    MCGuiWait("the editor shows the file", 10, [] {
        return MCGuiApp().editor->currentPath() == notesPath() &&
               MCGuiEditorText() == "first line\n";
    });
    MCGuiStep("a click in the text gives it the keyboard", [] {
        MCGuiClick(MCGuiApp().editor->textView());
    });
    MCGuiWait("the text view has the keyboard", 5, [] {
        return gtk_widget_has_focus(MCGuiApp().editor->textView());
    });
    MCGuiStep("types at the end of the file", [] {
        GCHECK(!MCGuiApp().editor->dirty());
        MCGuiKey("ctrl+End");
        MCGuiType("typed by XTest, with Shift: A~\n");
    });
    MCGuiWait("the keys reach the buffer", 10, [] {
        return MCGuiEditorText() == "first line\ntyped by XTest, with Shift: A~\n";
    });
    MCGuiStep("the buffer is marked unsaved", [] {
        GCHECK(MCGuiApp().editor->dirty());
        GCHECK(std::string(gtk_window_get_title(MCGuiApp().window)).find(" *") != std::string::npos);
    });
    MCGuiStep("with the action disabled, Ctrl+S saves nothing", [] {
        g_simple_action_set_enabled(saveAction(), FALSE);
        MCGuiKey("ctrl+s");
    });
    // A pause before a negative check: nothing must happen in this time.
    MCGuiPause(0.3);
    MCGuiStep("the file on disk is unchanged", [] {
        GCHECK_EQ_STR(MCGuiReadFile(notesPath()), "first line\n");
        GCHECK(MCGuiApp().editor->dirty());
        g_simple_action_set_enabled(saveAction(), TRUE);
    });
    MCGuiStep("Ctrl+S, through the accelerator", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("the disk has the text", 10, [] {
        return MCGuiReadFile(notesPath()) == "first line\ntyped by XTest, with Shift: A~\n";
    });
    MCGuiStep("the buffer is clean and the file is what it showed", [] {
        GCHECK(!MCGuiApp().editor->dirty());
        GCHECK(std::string(gtk_window_get_title(MCGuiApp().window)).find(" *") == std::string::npos);
        GCHECK_EQ_STR(MCGuiReadFile(notesPath()), MCGuiEditorText());
    });
}
