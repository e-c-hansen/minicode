// BookmarkOpen.cpp — see BookmarkOpen.h.
#include "BookmarkOpen.h"

#include "WebBookmark.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <climits>
#include <cstdlib>
#include <algorithm>
#include <utility>

namespace BookmarkOpen {

namespace {

std::string home() {
    const char* h = g_get_home_dir();
    return h && *h ? h : "/";
}

// The tree's name order: case folded, then collated.
std::string sortKey(const std::string& name) {
    char* folded = g_utf8_casefold(name.c_str(), -1);
    char* key = g_utf8_collate_key(folded ? folded : name.c_str(), -1);
    std::string out = key ? key : name;
    g_free(folded);
    g_free(key);
    return out;
}

std::vector<std::string> bookmarkNames(const std::string& dir) {
    std::vector<std::string> names;
    GDir* d = g_dir_open(dir.c_str(), 0, nullptr);
    if (!d) return names;
    while (const char* n = g_dir_read_name(d))
        if (n[0] != '.' && WebBookmark::isBookmarkPath(n)) names.push_back(n);
    g_dir_close(d);
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        const std::string ka = sortKey(a), kb = sortKey(b);
        return ka != kb ? ka < kb : a < b;
    });
    return names;
}

}  // namespace

std::string folder(const std::string& setting) {
    std::string path = setting;
    if (!path.empty()) {
        if (path == "~" || path.rfind("~/", 0) == 0) path = home() + path.substr(1);
        else if (path[0] != '/') path = home() + "/" + path;
        char* canon = g_canonicalize_filename(path.c_str(), nullptr);
        std::string out = canon ? canon : path;
        g_free(canon);
        return out;
    }
    return home() + "/Bookmarks";
}

bool ensureFolder(const std::string& dir, std::string* error) {
    if (g_file_test(dir.c_str(), G_FILE_TEST_IS_DIR)) return true;
    if (g_file_test(dir.c_str(), G_FILE_TEST_EXISTS)) {
        if (error) *error = dir + " is a file, not a folder";
        return false;
    }
    if (g_mkdir_with_parents(dir.c_str(), 0755) == 0) return true;
    if (error) *error = "the folder could not be made";
    return false;
}

bool sameFolder(const std::string& path, const std::string& dir) {
    char a[PATH_MAX], b[PATH_MAX];
    if (realpath(path.c_str(), a) && realpath(dir.c_str(), b)) return std::string(a) == b;
    return path == dir;
}

std::string addressOf(const std::string& path) {
    if (!WebBookmark::isBookmarkPath(path)) return "";
    if (!g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR)) return "";
    gchar* bytes = nullptr;
    gsize n = 0;
    if (!g_file_get_contents(path.c_str(), &bytes, &n, nullptr)) return "";
    std::string text;
    if (n <= (1u << 20)) text.assign(bytes, n);   // a bookmark is a few lines
    g_free(bytes);
    return WebBookmark::addressIn(path, text);
}

std::vector<std::string> addressesIn(const std::string& dir) {
    std::vector<std::string> out;
    for (const std::string& n : bookmarkNames(dir)) {
        const std::string u = addressOf(dir + "/" + n);
        if (!u.empty()) out.push_back(u);
    }
    return out;
}

bool hasBookmarkIn(const std::string& dir) { return !bookmarkNames(dir).empty(); }

}  // namespace BookmarkOpen
