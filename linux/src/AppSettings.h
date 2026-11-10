// AppSettings.h — the settings file on Linux: where it lives, loading it with
// the shared Settings parser, and watching it so changes apply live. The GTK
// counterpart of the macOS src/AppSettings.mm.
#pragma once

#include "Settings.h"
#include <gio/gio.h>
#include <functional>
#include <string>
#include <vector>

class AppSettings {
public:
    using ChangeCb = void(*)(void* user);

    // $MINICODE_SETTINGS if set, else $XDG_CONFIG_HOME/minicode/settings.conf
    // (~/.config/minicode/settings.conf by default), the same file macOS uses.
    AppSettings();
    ~AppSettings();

    const Settings& settings() const { return settings_; }
    const std::string& path() const { return path_; }
    // Problems in the file as "line N: message"; empty when it parsed cleanly.
    const std::vector<std::string>& errors() const { return errors_; }

    // Called after the file changes and has been re-read.
    void setChangeCallback(ChangeCb cb, void* user) { cb_ = cb; user_ = user; }

    // Write the commented default file if there is none yet (through
    // writeConfigFile, so a dangling symlink stays a link and the file it
    // names is made).
    bool ensureFileExists();
    // Re-read now; calls the change callback if the contents differ.
    void reload();
    // The file's text as it is on disk now ("" when there is none).
    std::string fileText() const;
    // Writes `theme = <id>` into the file (Settings::withTheme), commenting
    // out the file's own colors with `commentColors`, and applies it at once.
    // False with *why set when nothing was written: an unknown theme, a tab
    // holding the file with unsaved changes, or a failed write.
    bool writeTheme(const std::string& id, bool commentColors, std::string* why);

    // minicode's own rewrites of the file (a theme) are refused while a tab
    // holds it with unsaved changes, since the tab and the disk would then
    // part ways (the Mac's AppSettings.unsavedTextCheck). Set once by
    // main.cpp: whether any window has a tab of `path` with unsaved changes.
    static std::function<bool(const std::string& path)> unsavedTextCheck;

private:
    bool load();   // true if the contents changed
    void watch();
    bool rewriteFileWith(const std::string& text, std::string* why);
    static void onChanged(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent ev,
                          gpointer self);

    std::string path_;
    std::string text_;
    bool loaded_ = false;
    Settings settings_;
    std::vector<std::string> errors_;
    GFileMonitor* monitor_ = nullptr;
    ChangeCb cb_ = nullptr;
    void* user_ = nullptr;
};

// Writes `text` to a file minicode keeps for the user (settings.conf,
// keys.conf) through FolderSearch::writeFileAtomically, as the Mac's
// MCWriteConfigFile: a symlink, a dangling one too, stays a link and the file
// it names changes; a hard link is written in place; mode and extended
// attributes are kept. With `checkTabs`, refused while
// AppSettings::unsavedTextCheck says a tab holds the file with unsaved
// changes. False with *why set when nothing was written.
bool writeConfigFile(const std::string& path, const std::string& text, bool checkTabs,
                     std::string* why);
