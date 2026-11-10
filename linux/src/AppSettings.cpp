// AppSettings.cpp — see AppSettings.h.
#include "AppSettings.h"

#include "FolderSearch.h"

#include <glib/gstdio.h>
#include <cstdio>

std::function<bool(const std::string& path)> AppSettings::unsavedTextCheck;

static std::string baseNameOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

static std::string unsavedTabWhy(const std::string& path) {
    return baseNameOf(path) + " was not changed: a tab holds it with unsaved changes. Save "
                              "or close that tab, then try again.";
}

bool writeConfigFile(const std::string& path, const std::string& text, bool checkTabs,
                     std::string* why) {
    if (checkTabs && AppSettings::unsavedTextCheck && AppSettings::unsavedTextCheck(path)) {
        if (why) *why = unsavedTabWhy(path);
        return false;
    }
    std::string error;
    if (!FolderSearch::writeFileAtomically(path, text, &error)) {
        if (why) *why = "Could not write " + baseNameOf(path) + ": " + error + ".";
        return false;
    }
    return true;
}

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
    // A dangling symlink (a dotfiles folder whose copy is not there yet)
    // stays a link, and the file it names is made: g_file_set_contents
    // replaced the link with a plain file.
    std::string why;
    if (!writeConfigFile(path_, Settings::defaultFileText(), false, &why)) {
        std::fprintf(stderr, "minicode: %s\n", why.c_str());
        return false;
    }
    // Watching a path in a directory that didn't exist yet may have failed.
    if (!monitor_) watch();
    reload();
    return true;
}

std::string AppSettings::fileText() const {
    char* contents = nullptr;
    gsize len = 0;
    std::string text;
    if (g_file_get_contents(path_.c_str(), &contents, &len, nullptr)) {
        text.assign(contents, len);
        g_free(contents);
    }
    return text;
}

// The file rewritten by minicode itself, refused while a tab holds it with
// unsaved changes, and applied at once rather than when the monitor notices.
bool AppSettings::rewriteFileWith(const std::string& text, std::string* why) {
    std::string problem;
    if (!writeConfigFile(path_, text, true, &problem)) {
        std::fprintf(stderr, "minicode: %s\n", problem.c_str());
        if (why) *why = problem;
        return false;
    }
    if (!monitor_) watch();
    reload();
    return true;
}

bool AppSettings::writeTheme(const std::string& id, bool commentColors, std::string* why) {
    if (!Settings::findTheme(id)) {
        if (why) *why = "There is no theme called " + id + ".";
        return false;
    }
    // Asked before anything is written, so a refused theme makes no default
    // file either.
    if (unsavedTextCheck && unsavedTextCheck(path_)) {
        if (why) *why = unsavedTabWhy(path_);
        return false;
    }
    if (!ensureFileExists()) {
        if (why) *why = "Could not create " + path_ + ".";
        return false;
    }
    return rewriteFileWith(Settings::withTheme(fileText(), id, commentColors), why);
}
