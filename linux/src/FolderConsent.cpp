// FolderConsent.cpp — see FolderConsent.h.
#include "FolderConsent.h"

#include "FolderTools.h"

#include <gio/gio.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

namespace FolderConsent {

namespace {

std::set<std::string>& putOffKeys() {
    static std::set<std::string>* keys = new std::set<std::string>();
    return *keys;
}

std::map<int, std::function<void(const std::string&)>>& listeners() {
    static auto* l = new std::map<int, std::function<void(const std::string&)>>();
    return *l;
}

std::map<std::string, std::string> readAll() {
    gchar* text = nullptr;
    gsize len = 0;
    std::map<std::string, std::string> out;
    if (g_file_get_contents(file().c_str(), &text, &len, nullptr))
        out = FolderTools::decode(std::string(text, len));
    g_free(text);
    return out;
}

void writeAll(const std::map<std::string, std::string>& all) {
    const std::string path = file();
    gchar* dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    const std::string text = FolderTools::encode(all);
    GError* err = nullptr;
    // A temporary renamed over the old file; it names the user's folders,
    // so it is theirs alone.
    if (!g_file_set_contents_full(path.c_str(), text.data(), (gssize)text.size(),
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0600, &err)) {
        fprintf(stderr, "minicode: could not write %s: %s\n", path.c_str(),
                err ? err->message : "unknown error");
        g_clear_error(&err);
    }
}

void tell(const std::string& key) {
    // A copy: a listener may listen or unlisten (a window closing).
    const auto all = listeners();
    for (const auto& kv : all)
        if (listeners().count(kv.first)) kv.second(key);
}

void store(const std::string& root, const std::string& value) {
    const std::string key = keyFor(root);
    if (key.empty()) return;
    std::map<std::string, std::string> all = readAll();
    if (value.empty()) all.erase(key);
    else all[key] = value;
    writeAll(all);
    tell(key);
}

}  // namespace

std::string keyFor(const std::string& root) {
    if (root.empty()) return "";
    char buf[PATH_MAX];
    return realpath(root.c_str(), buf) ? std::string(buf) : root;
}

std::string answer(const std::string& root) {
    const std::string key = keyFor(root);
    if (key.empty()) return "";
    const std::map<std::string, std::string> all = readAll();
    auto it = all.find(key);
    return it == all.end() ? std::string() : it->second;
}

void set(const std::string& root, bool allow) { store(root, allow ? "allow" : "deny"); }

void forget(const std::string& root) {
    putOffKeys().erase(keyFor(root));
    store(root, "");
}

void putOff(const std::string& root) {
    const std::string key = keyFor(root);
    if (key.empty()) return;
    putOffKeys().insert(key);
    tell(key);
}

bool isPutOff(const std::string& root) { return putOffKeys().count(keyFor(root)) > 0; }

int listen(std::function<void(const std::string& key)> fn) {
    static int next = 1;
    const int id = next++;
    listeners()[id] = std::move(fn);
    return id;
}

void unlisten(int id) { listeners().erase(id); }

std::string file() {
    gchar* p = g_build_filename(g_get_user_state_dir(), "minicode", "folder-tools.json", nullptr);
    std::string out = p;
    g_free(p);
    return out;
}

}  // namespace FolderConsent
