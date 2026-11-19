// WindowTitleSuite.cpp — the window's title (`./build/mutible-guitest
// window_title`, main.cpp's setWindowTitle over the core's WindowTitle.h,
// the Mac's rule): the document's path from the window's folder
// ("title/inner.txt — mutible"), with "~" for a file in the home folder
// outside it and in full for one outside both, "● " in front while it is
// unsaved, whichever document pane (the editor, the file tree) has the
// keyboard; with no tab, the folder's name. It follows the keyboard between
// panes (Ctrl+0, Ctrl+1, a click) and the tab on show (Ctrl+Page Up), by
// real keys and clicks.
#include "Harness.h"

#include "Editor.h"
#include "FileTree.h"
#include "Keys.h"

#include <functional>
#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& n) { return MCGuiProjectFile(n); }
std::string title() {
    const char* t = gtk_window_get_title(MCGuiApp().window);
    return t ? t : "";
}
GtkWidget* focus() { return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)); }
bool editorHasKeyboard() { return focus() == ed()->textView(); }
bool treeHasKeyboard() {
    GtkWidget* f = focus();
    return f && gtk_widget_is_ancestor(f, MCGuiApp().tree->widget());
}
// Outside the project: in the home folder, and outside both (the scratch
// world is made before the suites run).
// The window's folder's last part, as the title names it with no tab.
std::string folderName() {
    std::string root = MCGuiProject();
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    return root.substr(root.rfind('/') + 1);
}
std::string homeFile() { return MCGuiScratch() + "/home/title-notes.txt"; }
std::string outsideFile() { return MCGuiScratch() + "/outside/title-out.txt"; }

// Opens the file `pathOf` names in a tab of its own and clicks in its
// text; `pathOf` is asked when the step runs.
void openAndClick(std::function<std::string()> pathOf) {
    MCGuiStep("open a file", [pathOf] { MCGuiOpenFileKept(pathOf()); });
    MCGuiWait("it is on show", 10, [pathOf] { return ed()->currentPath() == pathOf(); });
    MCGuiStep("a click in its text", [] { MCGuiClick(ed()->textView()); });
    MCGuiWait("the editor has the keyboard", 5, editorHasKeyboard);
}

}  // namespace

MC_GUI_SUITE(window_title) {
    MCGuiStep("files in the folder, in the home folder, and outside both", [] {
        ed()->closeAllTabs();
        MCGuiWriteFile(F("title/inner.txt"), "inner\n");
        MCGuiWriteFile(F("title-b.txt"), "b\n");
        MCGuiWriteFile(homeFile(), "notes\n");
        MCGuiWriteFile(outsideFile(), "out\n");
        GCHECK_EQ_STR(Keys::label("workbench.action.focusSideBar", "+"), "Ctrl+0");
        GCHECK_EQ_STR(Keys::label("workbench.action.focusFirstEditorGroup", "+"), "Ctrl+1");
        GCHECK_EQ_STR(Keys::label("workbench.action.previousEditor", "+"), "Ctrl+Page Up");
    });
    MCGuiWait("no tab", 10, [] { return ed()->tabCount() == 0; });
    MCGuiStep("with no file, the title is the folder's name", [] { GCHECK_EQ_STR(title(), folderName() + " — mutible"); });

    openAndClick([] { return F("title/inner.txt"); });
    MCGuiWait("the title shows the path from the folder", 5,
              [] { return title() == "title/inner.txt — mutible"; });

    MCGuiStep("Ctrl+0, the file tree", [] { MCGuiKey("ctrl+0"); });
    MCGuiWait("the tree has the keyboard", 5, treeHasKeyboard);
    MCGuiWait("the tree is a document pane: the path stays", 5, [] {
        return title() == "title/inner.txt — mutible";
    });
    MCGuiStep("Ctrl+1, the editor", [] { MCGuiKey("ctrl+1"); });
    MCGuiWait("the editor has the keyboard (2)", 5, editorHasKeyboard);
    MCGuiWait("the path again", 5, [] { return title() == "title/inner.txt — mutible"; });

    openAndClick([] { return F("title-b.txt"); });
    MCGuiWait("a file at the folder's top: its name is its path", 5,
              [] { return title() == "title-b.txt — mutible"; });
    MCGuiStep("Ctrl+Page Up, the tab before", [] { MCGuiKey("ctrl+pageup"); });
    MCGuiWait("inner on show, the title its path", 5, [] {
        return ed()->currentPath() == F("title/inner.txt") &&
               title() == "title/inner.txt — mutible";
    });
    MCGuiStep("Ctrl+0 again", [] { MCGuiKey("ctrl+0"); });
    MCGuiWait("the tree has the keyboard (2)", 5, treeHasKeyboard);
    MCGuiWait("the path, the tree having it", 5, [] { return title() == "title/inner.txt — mutible"; });

    openAndClick(homeFile);
    MCGuiWait("a file in the home folder, outside the window's: ~", 5,
              [] { return title() == "~/title-notes.txt — mutible"; });
    MCGuiStep("typing in it", [] { MCGuiType("x"); });
    MCGuiWait("the unsaved mark in front", 5,
              [] { return title() == "● ~/title-notes.txt — mutible"; });
    MCGuiStep("Ctrl+0 with it unsaved", [] { MCGuiKey("ctrl+0"); });
    MCGuiWait("the tree: the same path and mark", 5, [] { return title() == "● ~/title-notes.txt — mutible"; });
    MCGuiStep("Ctrl+1, the editor again", [] {
        MCGuiKey("ctrl+1");
    });
    MCGuiWait("the editor has the keyboard (3)", 5, editorHasKeyboard);
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("unchanged again", 5, [] { return title() == "~/title-notes.txt — mutible"; });

    openAndClick(outsideFile);
    MCGuiWait("a file outside both: in full", 5,
              [] { return title() == outsideFile() + " — mutible"; });
    MCGuiStep("the tabs closed", [] { ed()->closeAllTabs(); });
    MCGuiWait("none left, the title the folder's name", 10,
              [] { return ed()->tabCount() == 0 && title() == folderName() + " — mutible"; });
}
