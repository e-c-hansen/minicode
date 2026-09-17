// AppSettings.cpp — see AppSettings.h.
#include "AppSettings.h"

#include <glib/gstdio.h>
#include <cstdio>

AppSettings::AppSettings() {
    const char* env = g_getenv("MINICODE_SETTINGS");
    if (env && *env) {
        path_ = env;
    } else {
        char* p = g_build_filename(g_get_user_config_dir(), "minicode",
                                   "settings.conf", nullptr);
        path_ = p;
        g_free(p);
    }
    load();
    watch();
}

AppSettings::~AppSettings() {
    if (monitor_) g_object_unref(monitor_);
}

bool AppSettings::load() {
    char* contents = nullptr;
    gsize len = 0;
    std::string text;
    if (g_file_get_contents(path_.c_str(), &contents, &len, nullptr)) {
        text.assign(contents, len);
        g_free(contents);
    }
    if (loaded_ && text == text_) return false;
    loaded_ = true;
    text_ = text;

    std::vector<SettingsError> errs;
    settings_ = Settings::parse(text_, &errs);
    errors_.clear();
    for (const SettingsError& e : errs) {
        errors_.push_back("line " + std::to_string(e.line) + ": " + e.message);
        std::fprintf(stderr, "MiniCode settings %s\n", errors_.back().c_str());
    }
    return true;
}

void AppSettings::reload() {
    if (load() && cb_) cb_(user_);
}

// A file monitor on the path itself. GLib watches the directory underneath,
// so it sees the file appear, disappear, and be replaced by an editor's
// write-to-temp-and-rename save, not just in-place writes.
void AppSettings::watch() {
    if (monitor_) return;
    GFile* file = g_file_new_for_path(path_.c_str());
    monitor_ = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    g_object_unref(file);
    if (!monitor_) return;
    g_file_monitor_set_rate_limit(monitor_, 100);
    g_signal_connect(monitor_, "changed", G_CALLBACK(onChanged), this);
}

void AppSettings::onChanged(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent ev,
                            gpointer selfp) {
    switch (ev) {
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_RENAMED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
    case G_FILE_MONITOR_EVENT_CHANGED:
        static_cast<AppSettings*>(selfp)->reload();
        break;
    default:
        break;
    }
}

bool AppSettings::ensureFileExists() {
    if (g_file_test(path_.c_str(), G_FILE_TEST_EXISTS)) return true;
    char* dir = g_path_get_dirname(path_.c_str());
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    const char* body = Settings::defaultFileText();
    if (!g_file_set_contents(path_.c_str(), body, -1, nullptr)) return false;
    // Watching a path in a directory that didn't exist yet may have failed.
    if (!monitor_) watch();
    reload();
    return true;
}
