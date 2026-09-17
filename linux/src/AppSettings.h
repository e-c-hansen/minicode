// AppSettings.h — the settings file on Linux: where it lives, loading it with
// the shared Settings parser, and watching it so changes apply live. The GTK
// counterpart of the macOS src/AppSettings.mm.
#pragma once

#include "Settings.h"
#include <gio/gio.h>
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

    // Write the commented default file if there is none yet.
    bool ensureFileExists();
    // Re-read now; calls the change callback if the contents differ.
    void reload();

private:
    bool load();   // true if the contents changed
    void watch();
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
