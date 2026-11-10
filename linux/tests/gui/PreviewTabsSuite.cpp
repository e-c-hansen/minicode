// PreviewTabsSuite.cpp — `./build/minicode-guitest preview_tabs`: VS Code's
// preview tab (workbench.enable-preview, EditorTabs.h "The preview tab"), as
// the Mac's suite of the same name checks it, by real clicks. A single click
// on a file in the tree opens it in the preview tab, its title in italics,
// which the next single click replaces, so tabs do not pile up; a
// double-click on the file, Ctrl+click, an edit, a save, a double-click on
// the tab's title or Keep Open keeps it. A single click on a Find in Folder
// result previews it, a double-click keeps it. The setting off opens every
// file kept.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "FileTree.h"
#include "TabStrip.h"

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
TabStrip* strip() { return ed()->strip(); }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

// The tabs as names, the preview tab's in brackets, a star for unsaved
// changes: "p-a.txt [p-b.txt*]".
std::string row() {
    std::string out;
    for (int i = 0; i < strip()->count(); i++) {
        const TabStrip::Item& it = strip()->item(i);
        std::string t = it.preview ? "[" + it.title + "]" : it.title;
        if (it.dirty) t += "*";
        out += (out.empty() ? "" : " ") + t;
    }
    return out;
}

// The strip draws the preview tab in italics (its class) and no other.
bool italicsAgree() {
    for (int i = 0; i < ed()->tabCount(); i++)
        if (gtk_widget_has_css_class(strip()->tabWidget(i), "preview") !=
            ed()->document(i)->isPreviewTab())
            return false;
    return true;
}

GtkWidget* treeRow(const std::string& name) {
    return MCGuiFindLabel(MCGuiApp().tree->widget(), name);
}

bool activeIs(const std::string& name) { return ed()->currentPath() == F(name); }

bool editorHasKeyboard() { return gtk_widget_has_focus(ed()->textView()); }

// The first label on screen under `root` whose text holds `part`.
GtkWidget* labelHolding(GtkWidget* root, const std::string& part) {
    if (!root || !gtk_widget_get_mapped(root)) return nullptr;
    if (GTK_IS_LABEL(root)) {
        const char* text = gtk_label_get_text(GTK_LABEL(root));
        if (text && std::string(text).find(part) != std::string::npos)
            return MCGuiOnScreen(root) ? root : nullptr;
    }
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = labelHolding(c, part)) return hit;
    return nullptr;
}

GtkWindow* gSearch = nullptr;
size_t gClosed = 0;

// A click on a Find in Folder result, its window brought up first (opening
// a result raises the editor's window over it).
void clickResult(const std::string& part, bool twice) {
    MCGuiSetKeyWindow(gSearch);
    MCGuiFocusKeyWindow();
    GtkWidget* label = labelHolding(GTK_WIDGET(gSearch), part);
    if (twice) MCGuiDoubleClick(label);
    else MCGuiClick(label);
}

}  // namespace

MC_GUI_SUITE(preview_tabs) {
    MCGuiStep("a folder of files, and no tab open", [] {
        ed()->closeAllTabs();
        for (const char* n : {"p-a.txt", "p-b.txt", "p-c.txt", "p-d.txt", "p-e.txt"})
            MCGuiWriteFile(F(n), std::string(n) + "\nline 2\n");
        MCGuiWriteFile(F("found/one.txt"), "first\nneedle one\n");
        MCGuiWriteFile(F("found/two.txt"), "needle two\n");
    });
    MCGuiWait("the tree lists them", 10, [] {
        return ed()->tabCount() == 0 && treeRow("p-a.txt") && treeRow("p-e.txt");
    });

    MCGuiStep("a single click opens the file in the preview tab, in italics", [] {
        MCGuiClick(treeRow("p-a.txt"));
    });
    MCGuiWait("it is the preview tab", 10, [] { return row() == "[p-a.txt]"; });
    MCGuiStep("drawn in italics", [] {
        GCHECK(ed()->active()->isPreviewTab());
        GCHECK(italicsAgree());
        GCHECK(gtk_widget_has_css_class(strip()->tabWidget(0), "preview"));
    });
    MCGuiPause(0.5);   // the double-click interval: the next click is a new one
    MCGuiStep("another single click replaces it: still one tab", [] {
        gClosed = ed()->closedTabCount();
        MCGuiClick(treeRow("p-b.txt"));
    });
    MCGuiWait("p-b in the preview tab", 10, [] { return row() == "[p-b.txt]" && activeIs("p-b.txt"); });
    MCGuiStep("a replaced preview tab is not a closed tab to reopen", [] {
        GCHECK(ed()->closedTabCount() == gClosed);
        GCHECK(italicsAgree());
    });
    MCGuiPause(0.5);   // the double-click interval
    MCGuiStep("a double-click on the file keeps it", [] { MCGuiDoubleClick(treeRow("p-b.txt")); });
    MCGuiWait("an ordinary tab now", 10, [] { return row() == "p-b.txt"; });
    MCGuiStep("no italics", [] { GCHECK(italicsAgree() && !ed()->active()->isPreviewTab()); });
    MCGuiPause(0.5);   // the double-click interval
    MCGuiStep("the next click opens a new preview tab beside the kept one", [] {
        MCGuiClick(treeRow("p-c.txt"));
    });
    MCGuiWait("beside it", 10, [] { return row() == "p-b.txt [p-c.txt]"; });
    MCGuiPause(0.5);   // the double-click interval
    MCGuiStep("Ctrl+click opens a kept tab and leaves the preview tab alone", [] {
        MCGuiClickWith("ctrl", treeRow("p-d.txt"));
    });
    MCGuiWait("a kept tab right of the active one", 10, [] {
        return row() == "p-b.txt [p-c.txt] p-d.txt" && activeIs("p-d.txt");
    });
    MCGuiPause(0.5);   // the double-click interval
    MCGuiStep("Ctrl+click on the preview tab's file keeps it", [] {
        MCGuiClickWith("ctrl", treeRow("p-c.txt"));
    });
    MCGuiWait("kept, and on show", 10, [] {
        return row() == "p-b.txt p-c.txt p-d.txt" && activeIs("p-c.txt");
    });
    MCGuiPause(0.5);   // the double-click interval
    MCGuiStep("a click on a file open in a kept tab only shows it", [] { MCGuiClick(treeRow("p-b.txt")); });
    MCGuiWait("p-b on show, the row as it was", 10, [] {
        return row() == "p-b.txt p-c.txt p-d.txt" && activeIs("p-b.txt");
    });

    // ---- what keeps a preview tab
    MCGuiPause(0.5);   // the double-click interval
    MCGuiStep("a preview tab again", [] { MCGuiClick(treeRow("p-a.txt")); });
    MCGuiWait("right of the active one", 10, [] { return row() == "p-b.txt [p-a.txt] p-c.txt p-d.txt"; });
    MCGuiStep("a click in its text", [] { MCGuiClick(ed()->textView()); });
    MCGuiWait("the editor has the keyboard", 5, editorHasKeyboard);
    MCGuiStep("typing in it", [] {
        MCGuiKey("ctrl+home");
        MCGuiType("typed ");
    });
    MCGuiWait("keeps it", 10, [] { return row() == "p-b.txt p-a.txt* p-c.txt p-d.txt"; });
    MCGuiStep("saved and closed", [] {
        GCHECK(italicsAgree());
        MCGuiKey("ctrl+s");
        MCGuiKey("ctrl+w");
    });
    MCGuiWait("gone, and on disk", 10, [] {
        return row() == "p-b.txt p-c.txt p-d.txt" &&
               MCGuiReadFile(F("p-a.txt")) == "typed p-a.txt\nline 2\n";
    });
    MCGuiPause(0.5);   // the double-click interval
    MCGuiStep("another preview tab", [] { MCGuiClick(treeRow("p-e.txt")); });
    MCGuiWait("a preview tab", 10, [] { return row() == "p-b.txt [p-e.txt] p-c.txt p-d.txt"; });
    MCGuiStep("Ctrl+S, which writes a clean file too, keeps it", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("kept", 10, [] { return row() == "p-b.txt p-e.txt p-c.txt p-d.txt"; });
    MCGuiStep("closed", [] { MCGuiKey("ctrl+w"); });
    MCGuiWait("gone", 10, [] { return row() == "p-b.txt p-c.txt p-d.txt"; });
    MCGuiPause(0.5);   // the double-click interval
    MCGuiStep("a preview tab once more", [] { MCGuiClick(treeRow("p-e.txt")); });
    MCGuiWait("a preview tab, drawn", 10, [] {
        return row() == "p-b.txt [p-e.txt] p-c.txt p-d.txt" &&
               MCGuiOnScreen(strip()->titleWidget(1));
    });
    MCGuiStep("a double-click on its title keeps it", [] { MCGuiDoubleClick(strip()->titleWidget(1)); });
    MCGuiWait("kept", 10, [] { return row() == "p-b.txt p-e.txt p-c.txt p-d.txt" && italicsAgree(); });
    MCGuiStep("closed", [] { MCGuiKey("ctrl+w"); });
    MCGuiWait("gone", 10, [] { return row() == "p-b.txt p-c.txt p-d.txt"; });
    MCGuiPause(0.5);   // the double-click interval
    MCGuiStep("and once more", [] { MCGuiClick(treeRow("p-e.txt")); });
    MCGuiWait("a preview tab, drawn", 10, [] {
        return row() == "p-b.txt [p-e.txt] p-c.txt p-d.txt" &&
               MCGuiOnScreen(strip()->titleWidget(1));
    });
    MCGuiStep("its right click menu", [] { MCGuiClick(strip()->titleWidget(1), -1, -1, 3); });
    MCGuiWait("offers Keep Open", 10, [] {
        return strip()->menu() && gtk_widget_get_mapped(strip()->menu()) &&
               MCGuiFindLabel(strip()->menu(), "Keep Open");
    });
    MCGuiStep("Keep Open", [] { MCGuiClick(MCGuiFindLabel(strip()->menu(), "Keep Open")); });
    MCGuiWait("kept", 10, [] { return row() == "p-b.txt p-e.txt p-c.txt p-d.txt" && italicsAgree(); });
    MCGuiStep("closed", [] { MCGuiKey("ctrl+w"); });
    MCGuiWait("gone", 10, [] { return row() == "p-b.txt p-c.txt p-d.txt"; });
    MCGuiPause(0.5);   // the double-click interval
    MCGuiStep("and the File menu's Keep Open", [] { MCGuiClick(treeRow("p-e.txt")); });
    MCGuiWait("a preview tab", 10, [] { return row() == "p-b.txt [p-e.txt] p-c.txt p-d.txt"; });
    MCGuiStep("File > Keep Open", [] {
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApp().window), "keeptab", nullptr);
    });
    MCGuiWait("kept", 10, [] { return row() == "p-b.txt p-e.txt p-c.txt p-d.txt"; });

    // ---- the setting
    MCGuiStep("with workbench.enable-preview off, a click opens a kept tab", [] {
        MCGuiSetSettings("lsp.enabled = false\nworkbench.enable-preview = off\n");
    });
    MCGuiPause(0.5);   // the double-click interval, and the settings read
    MCGuiStep("a click on p-a", [] { MCGuiClick(treeRow("p-a.txt")); });
    MCGuiWait("kept, right of the active one", 10, [] {
        return row() == "p-b.txt p-e.txt p-a.txt p-c.txt p-d.txt" && activeIs("p-a.txt");
    });
    MCGuiStep("the setting back on", [] { MCGuiSetSettings("lsp.enabled = false\n"); });

    // ---- Find in Folder
    MCGuiStep("Find in Folder for a word in two files", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+shift+f");
    });
    MCGuiWait("its window", 10, [] { return MCGuiTopWindow() != MCGuiApp().window; });
    MCGuiStep("the word typed", [] {
        gSearch = MCGuiTopWindow();
        MCGuiSetKeyWindow(gSearch);
        MCGuiType("needle");
    });
    MCGuiWait("both results", 10, [] {
        return labelHolding(GTK_WIDGET(gSearch), "needle one") &&
               labelHolding(GTK_WIDGET(gSearch), "needle two");
    });
    MCGuiStep("a single click on a result previews it", [] { clickResult("needle one", false); });
    MCGuiWait("in the preview tab, the match selected", 10, [] {
        return activeIs("found/one.txt") && ed()->active()->isPreviewTab() &&
               row() == "p-b.txt p-e.txt p-a.txt [one.txt] p-c.txt p-d.txt";
    });
    MCGuiPause(0.5);   // the double-click interval
    MCGuiStep("the next result's click replaces it in the preview tab", [] {
        clickResult("needle two", false);
    });
    MCGuiWait("two in the preview tab, one gone", 10, [] {
        return activeIs("found/two.txt") && ed()->documentFor(F("found/one.txt")) == nullptr &&
               row() == "p-b.txt p-e.txt p-a.txt [two.txt] p-c.txt p-d.txt";
    });
    MCGuiPause(0.5);   // the double-click interval
    MCGuiStep("a double-click on it keeps it", [] { clickResult("needle two", true); });
    MCGuiWait("kept", 10, [] { return row() == "p-b.txt p-e.txt p-a.txt two.txt p-c.txt p-d.txt"; });
    MCGuiStep("the search window closed", [] {
        MCGuiSetKeyWindow(gSearch);
        MCGuiKey("escape");
    });
    MCGuiWait("gone", 10, [] { return MCGuiTopWindow() == MCGuiApp().window; });
    MCGuiStep("the keyboard back in the app's window", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
    });
}
