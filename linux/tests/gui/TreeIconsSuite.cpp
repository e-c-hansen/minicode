// TreeIconsSuite.cpp — the file tree's icons by kind (`./build/minicode-guitest
// tree_icons`): a row's icon is chosen from what a click on the file opens
// (FileIcon::kindOf), as on the Mac. The names are read off the rows' images.
#include "Harness.h"

#include "Editor.h"
#include "FileTree.h"

#include <cstring>
#include <filesystem>

namespace {

std::string dir() { return MCGuiProjectFile("0-tree-icons"); }

struct Want { const char* file; const char* icon; };
const Want kWant[] = {
    {"tree-icon.txt", "text-x-generic-symbolic"},
    {"tree-icon.py", "utilities-terminal-symbolic"},
    {"tree-icon.png", "image-x-generic-symbolic"},
    {"tree-icon.mp4", "video-x-generic-symbolic"},
    {"tree-icon.mp3", "audio-x-generic-symbolic"},
    {"tree-icon.pdf", "x-office-document-symbolic"},
    {"tree-icon.csv", "x-office-spreadsheet-symbolic"},
    {"tree-icon.ipynb", "accessories-dictionary-symbolic"},
    {"tree-icon.sqlite", "drive-harddisk-symbolic"},
    {"tree-icon.zip", "package-x-generic-symbolic"},
    {"tree-icon.html", "web-browser-symbolic"},
    {"tree-icon.url", "user-bookmarks-symbolic"},
};

GtkWidget* row(const std::string& name) { return MCGuiFindLabel(MCGuiApp().tree->widget(), name); }

// The icon name of the image beside a row's label, "" when none.
std::string iconOf(const std::string& name) {
    GtkWidget* label = row(name);
    GtkWidget* image = label ? gtk_widget_get_prev_sibling(label) : nullptr;
    const char* n = image && GTK_IS_IMAGE(image) ? gtk_image_get_icon_name(GTK_IMAGE(image)) : nullptr;
    return n ? n : "";
}

// A full run leaves the project folder with a hundred entries and the tree
// scrolled anywhere, and its list view makes rows only for what is on screen,
// so each row is revealed (its folder expanded, the row scrolled to) before
// it is read. The folder may still be arriving from the file monitor, so the
// reveal is asked again until the row is there.
bool shown(const std::string& path, const std::string& name) {
    if (row(name)) return true;
    MCGuiApp().tree->revealPath(path);
    return row(name) != nullptr;
}

}  // namespace

MC_GUI_SUITE(tree_icons) {
    MCGuiStep("a folder with one file of each kind", [] {
        for (const Want& w : kWant) MCGuiWriteFile(dir() + "/" + w.file, "x\n");
    });
    for (const Want& w : kWant) {
        std::string file = w.file, icon = w.icon;
        MCGuiWait(file + "'s row is on show", 20, [file] { return shown(dir() + "/" + file, file); });
        MCGuiStep(file + "'s row carries " + icon, [file, icon] { GCHECK_EQ_STR(iconOf(file), icon); });
    }
    MCGuiWait("the folder's row is on show", 20, [] { return shown(dir(), "0-tree-icons"); });
    MCGuiStep("a folder keeps the folder icon", [] {
        GCHECK_EQ_STR(iconOf("0-tree-icons"), std::string("folder-symbolic"));
    });
    // The folder goes again, so later suites find the tree as they would
    // have without it.
    MCGuiStep("the folder removed", [] { std::filesystem::remove_all(dir()); });
    MCGuiWait("its row gone from the tree", 20, [] { return row("0-tree-icons") == nullptr; });
}
