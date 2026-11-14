// RecentList.cpp — see RecentList.h.
#include "RecentList.h"

#include "Json.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <sys/stat.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace RecentList {

namespace {

std::map<int, std::function<void()>>& listeners() {
    static auto* l = new std::map<int, std::function<void()>>();
    return *l;
}

bool flagSet(const char* name) {
    const char* v = getenv(name);
    return v && *v && std::string(v) != "0";
}

std::vector<std::string> read() {
    gchar* text = nullptr;
    gsize len = 0;
    std::vector<std::string> out;
    if (g_file_get_contents(file().c_str(), &text, &len, nullptr))
        out = decode(std::string(text, len));
    g_free(text);
    return out;
}

void write(const std::vector<std::string>& list) {
    const std::string path = file();
    gchar* dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    const std::string text = encode(list);
    GError* err = nullptr;
    if (!g_file_set_contents_full(path.c_str(), text.data(), (gssize)text.size(),
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0600, &err)) {
        fprintf(stderr, "minicode: could not write %s: %s\n", path.c_str(),
                err ? err->message : "unknown error");
        g_clear_error(&err);
    }
}

void tell() {
    // A copy: a listener may listen or unlisten.
    const auto all = listeners();
    for (const auto& kv : all)
        if (listeners().count(kv.first)) kv.second();
}

}  // namespace

std::vector<std::string> noted(const std::vector<std::string>& list, const std::string& resolved) {
    std::vector<std::string> out;
    if (resolved.empty()) return list;
    out.push_back(resolved);
    for (const std::string& p : list)
        if (p != resolved && out.size() < kMax) out.push_back(p);
    return out;
}

std::string encode(const std::vector<std::string>& list) {
    Json a = Json::array();
    for (const std::string& p : list) a.push(Json(p));
    return a.dump() + "\n";
}

std::vector<std::string> decode(const std::string& text) {
    std::vector<std::string> out;
    Json j;
    if (!Json::parse(text, j) || !j.isArray()) return out;
    for (const Json& e : j.elements())
        if (e.isString() && !e.asString().empty()) out.push_back(e.asString());
    return out;
}

std::string tilded(const std::string& path, const std::string& home) {
    if (home.empty() || home == "/") return path;
    if (path == home) return "~";
    if (path.size() > home.size() && path.compare(0, home.size(), home) == 0 &&
        path[home.size()] == '/')
        return "~" + path.substr(home.size());
    return path;
}

bool enabled() { return !flagSet("MINICODE_BENCH") && !flagSet("MINICODE_DEMO"); }

std::string file() {
    const char* s = getenv("XDG_STATE_HOME");
    std::string base = (s && *s) ? s : std::string(g_get_home_dir()) + "/.local/state";
    return base + "/minicode/recent-folders.json";
}

void note(const std::string& folder) {
    if (!enabled() || folder.empty()) return;
    char buf[PATH_MAX];
    const std::string resolved = realpath(folder.c_str(), buf) ? std::string(buf) : folder;
    const std::vector<std::string> before = read();
    const std::vector<std::string> after = noted(before, resolved);
    if (after == before) return;
    write(after);
    tell();
}

std::vector<std::string> folders() {
    std::vector<std::string> out;
    if (!enabled()) return out;
    for (const std::string& p : read()) {
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) out.push_back(p);
    }
    return out;
}

void clear() {
    if (!enabled()) return;
    if (read().empty()) return;
    write({});
    tell();
}

int listen(std::function<void()> fn) {
    static int next = 1;
    const int id = next++;
    listeners()[id] = std::move(fn);
    return id;
}

void unlisten(int id) { listeners().erase(id); }

}  // namespace RecentList
