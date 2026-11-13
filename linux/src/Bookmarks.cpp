// Bookmarks.cpp — see Bookmarks.h.
#include "Bookmarks.h"

#include "WebBookmark.h"

#include <glib.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <set>
#include <climits>

namespace Bookmarks {
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

bool nameBefore(const std::string& a, const std::string& b) {
    const std::string ka = sortKey(a), kb = sortKey(b);
    return ka != kb ? ka < kb : a < b;
}

std::string readSmall(const std::string& path);

std::vector<std::string> bookmarkNames(const std::string& dir) {
    std::vector<std::string> names;
    GDir* d = g_dir_open(dir.c_str(), 0, nullptr);
    if (!d) return names;
    while (const char* n = g_dir_read_name(d))
        if (n[0] != '.' && WebBookmark::isBookmarkPath(n)) names.push_back(n);
    g_dir_close(d);
    std::sort(names.begin(), names.end(), nameBefore);
    return names;
}

bool exists(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0;
}

}  // namespace

std::string folder(const std::string& setting) {
    if (!setting.empty()) {
        std::string p = setting;
        if (p == "~" || p.compare(0, 2, "~/") == 0) p = home() + p.substr(1);
        else if (p[0] != '/') p = home() + "/" + p;
        char* canon = g_canonicalize_filename(p.c_str(), nullptr);
        std::string out = canon ? canon : p;
        g_free(canon);
        return out;
    }
    return home() + "/Bookmarks";
}

bool ensureFolder(const std::string& dir, std::string* error) {
    struct stat st;
    if (stat(dir.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) return true;
        if (error) *error = dir + " is a file, not a folder";
        return false;
    }
    // Each missing level, as mkdir -p.
    for (size_t i = 1; i <= dir.size(); i++) {
        if (i < dir.size() && dir[i] != '/') continue;
        const std::string part = dir.substr(0, i);
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) {
            if (error) *error = std::strerror(errno);
            return false;
        }
    }
    return true;
}

bool sameFolder(const std::string& path, const std::string& dir) {
    char a[PATH_MAX], b[PATH_MAX];
    if (realpath(path.c_str(), a) && realpath(dir.c_str(), b)) return std::string(a) == b;
    return path == dir;
}

std::string addressOf(const std::string& path) {
    if (!WebBookmark::isBookmarkPath(path)) return "";
    return WebBookmark::addressIn(path, readSmall(path));
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

std::vector<Entry> list(const std::string& dir, size_t limit) {
    std::vector<std::string> dirs, files;
    GDir* d = g_dir_open(dir.c_str(), 0, nullptr);
    if (!d) return {};
    while (const char* n = g_dir_read_name(d)) {
        if (n[0] == '.') continue;
        const std::string path = dir + "/" + n;
        if (g_file_test(path.c_str(), G_FILE_TEST_IS_DIR)) dirs.push_back(n);
        else if (WebBookmark::isBookmarkPath(n)) files.push_back(n);
    }
    g_dir_close(d);
    std::sort(dirs.begin(), dirs.end(), nameBefore);
    std::sort(files.begin(), files.end(), nameBefore);
    std::vector<Entry> out;
    for (const std::string& n : dirs) out.push_back({n, dir + "/" + n, true});
    for (const std::string& n : files) out.push_back({n, dir + "/" + n, false});
    if (out.size() > limit) out.resize(limit);
    return out;
}

namespace {

void collect(const std::string& root, const std::string& dir, const std::string& rel, size_t limit,
             size_t* seen, int depth, std::set<std::string>* visited, std::vector<Found>* out) {
    char real[PATH_MAX];
    if (depth > 20 || !realpath(dir.c_str(), real) || !visited->insert(real).second) return;
    for (const Entry& e : list(dir, limit)) {
        if (++*seen > limit) return;
        if (e.isFolder) {
            collect(root, e.path, rel + e.name + "/", limit, seen, depth + 1, visited, out);
            continue;
        }
        const std::string addr = addressOf(e.path);
        if (!addr.empty()) out->push_back({e.path, rel + WebBookmark::menuTitle(e.name), addr});
    }
}

}  // namespace

std::vector<Found> all(const std::string& dir, size_t limit) {
    std::vector<Found> out;
    size_t seen = 0;
    std::set<std::string> visited;
    collect(dir, dir, "", limit, &seen, 0, &visited, &out);
    return out;
}

namespace {

// A menu label: underscores are mnemonics in a GTK menu, so each is doubled.
std::string menuLabel(const std::string& text) {
    std::string out;
    for (char c : text) {
        out += c;
        if (c == '_') out += '_';
    }
    return out;
}

void appendItem(GMenu* m, const std::string& label, const char* action, const std::string& target) {
    GMenuItem* item = g_menu_item_new(menuLabel(label).c_str(), nullptr);
    g_menu_item_set_action_and_target_value(item, action, g_variant_new_string(target.c_str()));
    g_menu_append_item(m, item);
    g_object_unref(item);
}

void fill(GMenu* m, const std::string& dir, size_t* left, int depth, std::set<std::string>* visited) {
    char real[PATH_MAX];
    if (depth > 6 || !realpath(dir.c_str(), real) || !visited->insert(real).second) return;
    for (const Entry& e : list(dir, *left)) {
        if (*left == 0) return;
        --*left;
        if (!e.isFolder) {
            appendItem(m, WebBookmark::menuTitle(e.name), "win.openbookmark", e.path);
            continue;
        }
        GMenu* sub = g_menu_new();
        GMenu* head = g_menu_new();
        appendItem(head, "Open All in Tabs", "win.openbookmarkfolder", e.path);
        g_menu_append_section(sub, nullptr, G_MENU_MODEL(head));
        g_object_unref(head);
        GMenu* body = g_menu_new();
        fill(body, e.path, left, depth + 1, visited);
        g_menu_append_section(sub, nullptr, G_MENU_MODEL(body));
        g_object_unref(body);
        g_menu_append_submenu(m, menuLabel(e.name).c_str(), G_MENU_MODEL(sub));
        g_object_unref(sub);
    }
}

}  // namespace

GMenu* menu(const std::string& dir, size_t limit) {
    GMenu* m = g_menu_new();
    size_t left = limit;
    std::set<std::string> visited;
    fill(m, dir, &left, 0, &visited);
    if (g_menu_model_get_n_items(G_MENU_MODEL(m)) == 0) g_menu_append(m, "No bookmarks", "win.nobookmarks");
    return m;
}

std::string write(const std::string& dir, const std::string& title, const std::string& url,
                  std::string* error) {
    if (!WebBookmark::isWebAddress(url)) {
        if (error) *error = "only http and https pages can be bookmarked";
        return "";
    }
    const std::string text = WebBookmark::shortcutText(url);
    std::string name = WebBookmark::fileName(title, url);
    auto taken = [&dir](const std::string& n) { return exists(dir + "/" + n); };
    // A name free now may be taken before the create: O_EXCL refuses it and
    // the next free name is tried.
    for (int attempt = 0; attempt < 5; attempt++) {
        name = WebBookmark::uniqueName(name, taken);
        const std::string path = dir + "/" + name;
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            if (error) *error = std::strerror(errno);
            return "";
        }
        size_t done = 0;
        int err = 0;
        while (done < text.size()) {
            const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { err = errno; break; }
            done += (size_t)n;
        }
        close(fd);
        if (done != text.size()) {
            unlink(path.c_str());
            if (error) *error = std::strerror(err);
            return "";
        }
        return path;
    }
    if (error) *error = "no free name for the bookmark";
    return "";
}

namespace {

std::string readSmall(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 1024 * 1024) return "";
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return "";
    std::string out;
    char buf[4096];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        out.append(buf, (size_t)n);
    }
    close(fd);
    return out;
}

// Breadth first would be tidier; depth first with a count is enough.
std::string search(const std::string& dir, const std::string& url, size_t* seen, int depth,
                   std::set<std::string>* visited) {
    char real[4096];
    if (depth > 20 || !realpath(dir.c_str(), real) || !visited->insert(real).second) return "";
    DIR* d = opendir(dir.c_str());
    if (!d) return "";
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        names.push_back(e->d_name);
    }
    closedir(d);
    for (const std::string& n : names) {
        if (++*seen > 10000) return "";
        const std::string path = dir + "/" + n;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            const std::string r = search(path, url, seen, depth + 1, visited);
            if (!r.empty()) return r;
        } else if (S_ISREG(st.st_mode) && WebBookmark::isBookmarkPath(path)) {
            const std::string have = WebBookmark::addressIn(path, readSmall(path));
            if (!have.empty() && WebBookmark::sameAddress(have, url)) return path;
        }
    }
    return "";
}

}  // namespace

std::string fileFor(const std::string& dir, const std::string& url) {
    if (dir.empty() || !WebBookmark::isWebAddress(url)) return "";
    size_t seen = 0;
    std::set<std::string> visited;
    return search(dir, url, &seen, 0, &visited);
}

std::string shortPath(const std::string& path) {
    const std::string h = home();
    if (h.size() > 1 && path.compare(0, h.size(), h) == 0 &&
        (path.size() == h.size() || path[h.size()] == '/'))
        return "~" + path.substr(h.size());
    return path;
}

std::string writeTabs(const std::string& parent, const std::string& when,
                      const std::vector<std::string>& titles, const std::vector<std::string>& urls,
                      size_t* saved, std::string* error) {
    *saved = 0;
    const std::string name = WebBookmark::uniqueName(
        WebBookmark::tabsFolderName(when),
        [&parent](const std::string& n) { return exists(parent + "/" + n); });
    const std::string folder = parent + "/" + name;
    if (mkdir(folder.c_str(), 0755) != 0) {
        if (error) *error = std::strerror(errno);
        return "";
    }
    std::set<std::string> once;
    for (size_t i = 0; i < urls.size(); i++) {
        if (!WebBookmark::isWebAddress(urls[i]) || !once.insert(urls[i]).second) continue;
        std::string err;
        if (write(folder, i < titles.size() ? titles[i] : "", urls[i], &err).empty()) {
            if (error && error->empty()) *error = err;
        } else {
            ++*saved;
        }
    }
    return folder;
}

}  // namespace Bookmarks
