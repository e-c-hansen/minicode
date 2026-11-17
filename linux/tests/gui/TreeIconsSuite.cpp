// TreeIconsSuite.cpp — the file tree's icons by kind (`./build/minicode-guitest
// tree_icons`): a row's icon is chosen from what a click on the file opens
// (FileIcon::kindOf), as on the Mac. The names are read off the rows' images.
#include "Harness.h"

#include "Editor.h"
#include "FileTree.h"

#include <cstring>

namespace {

std::string dir() { return MCGuiProjectFile("0-tree-icons"); }

struct Want { const char* file; const char* icon; };
const Want kWant[] = {
    {"a.txt", "text-x-generic-symbolic"},
    {"a.py", "utilities-terminal-symbolic"},
    {"a.png", "image-x-generic-symbolic"},
    {"a.mp4", "video-x-generic-symbolic"},
    {"a.mp3", "audio-x-generic-symbolic"},
    {"a.pdf", "x-office-document-symbolic"},
    {"a.csv", "x-office-spreadsheet-symbolic"},
    {"a.ipynb", "accessories-dictionary-symbolic"},
    {"a.sqlite", "drive-harddisk-symbolic"},
    {"a.zip", "package-x-generic-symbolic"},
    {"a.html", "web-browser-symbolic"},
    {"a.url", "user-bookmarks-symbolic"},
};

GtkWidget* row(const std::string& name) { return MCGuiFindLabel(MCGuiApp().tree->widget(), name); }

// The icon name of the image beside a row's label, "" when none.
std::string iconOf(const std::string& name) {
    GtkWidget* label = row(name);
    GtkWidget* image = label ? gtk_widget_get_prev_sibling(label) : nullptr;
    const char* n = image && GTK_IS_IMAGE(image) ? gtk_image_get_icon_name(GTK_IMAGE(image)) : nullptr;
    return n ? n : "";
}

bool allShown() {
    for (const Want& w : kWant) if (!row(w.file)) return false;
    return true;
}

}  // namespace

MC_GUI_SUITE(tree_icons) {
    MCGuiStep("a folder with one file of each kind", [] {
        for (const Want& w : kWant) MCGuiWriteFile(dir() + "/" + w.file, "x\n");
    });
    MCGuiWait("the folder is listed", 20, [] { return row("0-tree-icons") != nullptr; });
    MCGuiStep("open it", [] { MCGuiClick(row("0-tree-icons")); });
    MCGuiWait("its rows are on show", 20, [] { return allShown(); });
    MCGuiStep("each row carries its kind's icon", [] {
        for (const Want& w : kWant) GCHECK_EQ_STR(iconOf(w.file), std::string(w.icon));
        GCHECK_EQ_STR(iconOf("0-tree-icons"), std::string("folder-symbolic"));
    });
    MCGuiStep("close it", [] { MCGuiClick(row("0-tree-icons")); });
}
