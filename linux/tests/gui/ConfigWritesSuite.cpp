// ConfigWritesSuite.cpp — `./build/minicode-guitest config_writes`
// (PARITY.md 2.11; the Mac's config_writes suite): minicode's own first
// writes of keys.conf and settings.conf go through the atomic writer
// (writeConfigFile in AppSettings.h, FolderSearch::writeFileAtomically), so
// with either file a dangling symlink into a dotfiles folder, Edit >
// Keyboard Shortcuts… (from the palette) and Adjust Settings… (from the
// status bar's right click) make the file the link names, leave the link a
// link, and open it; g_file_set_contents replaced the link with a plain
// file. View > Theme's own write through a link is the themes suite's.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "Keys.h"
#include "PalettePanel.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <functional>
#include <string>

namespace {

bool gKeysBefore = false;
std::string gKeysText;

std::string settingsPath() { return g_getenv("MINICODE_SETTINGS"); }
std::string dots(const std::string& name) { return MCGuiScratch() + "/dots/" + name; }
Editor* ed() { return MCGuiApp().editor; }

bool isLink(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

GtkWidget* menuWith(GtkWidget* root, const std::string& label) {
    if (!root || !gtk_widget_get_mapped(root)) return nullptr;
    if (GTK_IS_POPOVER_MENU(root) && MCGuiFindLabel(root, label)) return root;
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = menuWith(c, label)) return hit;
    return nullptr;
}
GtkWidget* popoverWith(const std::string& label) {
    return menuWith(GTK_WIDGET(MCGuiApp().window), label);
}

bool noTemporaries(const std::string& dir) {
    bool clean = true;
    GDir* d = g_dir_open(dir.c_str(), 0, nullptr);
    while (const char* name = d ? g_dir_read_name(d) : nullptr)
        if (strstr(name, ".minicode-")) clean = false;
    if (d) g_dir_close(d);
    return clean;
}

}  // namespace

MC_GUI_SUITE(config_writes) {
    // ---- keys.conf
    MCGuiStep("keys.conf a symlink into a dotfiles folder that has no copy yet", [] {
        if (ed()->dirty()) MCGuiKey("ctrl+s");
        gKeysBefore = MCGuiFileExists(Keys::path());
        gKeysText = MCGuiReadFile(Keys::path());
        g_mkdir_with_parents((MCGuiScratch() + "/dots").c_str(), 0755);
        unlink(Keys::path().c_str());
        unlink(dots("keys.conf").c_str());
        GCHECK(symlink(dots("keys.conf").c_str(), Keys::path().c_str()) == 0);
        GCHECK(isLink(Keys::path()) && !MCGuiFileExists(dots("keys.conf")));
        // From the palette: the Edit menu has grown taller than the checks'
        // screen, and Keyboard Shortcuts… is its last row.
        MCGuiKey("ctrl+shift+p");
    });
    MCGuiWait("the palette", 10, [] { return MCGuiApp().palette && MCGuiApp().palette->isOpen(); });
    MCGuiStep("Keyboard Shortcuts… typed and run", [] { MCGuiType("keyboard shortcuts\n"); });
    MCGuiWait("the dotfiles copy written and open", 10, [] {
        return MCGuiReadFile(dots("keys.conf")).size() > 100 && ed()->currentPath() == Keys::path();
    });
    MCGuiStep("the link still a link, every command commented out, nothing left beside", [] {
        GCHECK(isLink(Keys::path()));
        const std::string text = MCGuiReadFile(dots("keys.conf"));
        GCHECK(text.find("# ") != std::string::npos);
        GCHECK(text.find("\nctrl") == std::string::npos);
        GCHECK(noTemporaries(MCGuiScratch()) && noTemporaries(MCGuiScratch() + "/dots"));
        GCHECK(Keys::errors().empty());
        MCGuiKey("ctrl+w");
    });
    MCGuiWait("its tab closed", 10, [] { return ed()->currentPath() != Keys::path(); });
    MCGuiStep("keys.conf as it was", [] {
        unlink(Keys::path().c_str());
        unlink(dots("keys.conf").c_str());
        if (gKeysBefore) MCGuiSetKeys(gKeysText);
        else MCGuiReloadKeys();
        GCHECK(!isLink(Keys::path()));
    });

    // ---- settings.conf
    MCGuiStep("settings.conf a dangling symlink too", [] {
        unlink(settingsPath().c_str());
        unlink(dots("settings.conf").c_str());
        GCHECK(symlink(dots("settings.conf").c_str(), settingsPath().c_str()) == 0);
        MCGuiReloadSettings();
        MCGuiClick(MCGuiApp().statusBar, 6, -1, 3);
    });
    MCGuiWait("the status bar's menu", 10, [] { return popoverWith("Adjust Settings…") != nullptr; });
    MCGuiStep("Adjust Settings…", [] {
        MCGuiClick(MCGuiFindLabel(popoverWith("Adjust Settings…"), "Adjust Settings…"));
    });
    MCGuiWait("the default file written through the link, open at the status bar's keys", 10, [] {
        Document* d = ed()->active();
        return MCGuiReadFile(dots("settings.conf")) == Settings::defaultFileText() && d &&
               d->isSettingsFile() && !d->dirty();
    });
    MCGuiStep("the link still a link, nothing left beside", [] {
        GCHECK(isLink(settingsPath()));
        GCHECK(noTemporaries(MCGuiScratch()) && noTemporaries(MCGuiScratch() + "/dots"));
        GtkTextIter s, e;
        GCHECK(gtk_text_buffer_get_selection_bounds(ed()->buffer(), &s, &e));
        GCHECK(gtk_text_iter_get_line(&s) ==
               Settings::surfaceKeyLines(Settings::defaultFileText(), "statusbar")[0]);
        MCGuiKey("ctrl+w");
    });
    MCGuiWait("its tab closed", 10, [] {
        return !ed()->active() || !ed()->active()->isSettingsFile();
    });
    MCGuiStep("the plain file back", [] {
        unlink(settingsPath().c_str());
        unlink(dots("settings.conf").c_str());
        MCGuiSetSettings("lsp.enabled = false\n");
        GCHECK(!isLink(settingsPath()));
    });
}
