// TabCloseNowSuite.cpp — `./build/minicode-guitest tab_close_now`: a tab
// opened and closed in the same turn of the main loop, before the view has
// drawn it. GtkTextLayout kept line displays of the closed buffer, and the
// next frame read its freed lines (signal 11, or "gtk_text_btree_line_number
// couldn't find line"), every time under MALLOC_PERTURB_ until CodeView
// emptied the cache before the view left a buffer (2026-11-10; the case
// came from writing item 1.7's suites). GutterSwitchSuite.cpp has the
// longer story.
#include "Harness.h"

#include "Editor.h"

MC_GUI_SUITE(tab_close_now) {
    for (int round = 0; round < 5; round++) {
        MCGuiStep("a file opened and every tab closed in one step", [] {
            MCGuiWriteFile(MCGuiProjectFile("close-now.txt"), "hello\nworld\n");
            MCGuiOpenFileKept(MCGuiProjectFile("close-now.txt"));
            MCGuiApp().editor->closeAllTabs();
        });
        MCGuiStep("the next frames draw", [] { GCHECK(MCGuiApp().editor->tabCount() == 0); });
    }
}
