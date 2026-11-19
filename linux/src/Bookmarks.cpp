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
#include <map>
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

std::vector<WebBookmark::Name> names(const std::string& dir) {
    std::vector<std::string> all;
    GDir* d = g_dir_open(dir.c_str(), 0, nullptr);
    if (!d) return {};
    while (const char* n = g_dir_read_name(d)) all.push_back(n);
    g_dir_close(d);
    std::sort(all.begin(), all.end(), nameBefore);
    std::vector<WebBookmark::Name> out;
    out.reserve(all.size());
    for (const std::string& n : all)
        out.push_back({n, n[0] != '.' && g_file_test((dir + "/" + n).c_str(), G_FILE_TEST_IS_DIR)});
    return out;
}

std::vector<Entry> list(const std::string& dir, size_t limit) {
    std::vector<Entry> out;
    for (const WebBookmark::Entry& e : WebBookmark::entries(dir, names(dir), false, limit))
        out.push_back({e.path.substr(e.path.rfind('/') + 1), e.path, e.folder});
    return out;
}

WebBookmark::Places places(const std::string& root, const std::string& shared) {
    const std::string r = !root.empty() && sameFolder(root, shared) ? shared : root;
    return WebBookmark::places(r, shared);
}

bool inside(const std::string& path, const std::string& dir) {
    if (path.empty() || dir.empty()) return false;
    char a[PATH_MAX], b[PATH_MAX];
    const std::string p = realpath(path.c_str(), a) ? std::string(a) : path;
    const std::string d = realpath(dir.c_str(), b) ? std::string(b) : dir;
    const std::string prefix = d == "/" ? d : d + "/";
    return p.size() > prefix.size() && p.compare(0, prefix.size(), prefix) == 0;
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

// One menu being built: what is left of its limit, the folders already in
// it (symlink loops cut), and the submenu labels used. GtkPopoverMenu names
// each submenu's page by its label and refuses a second page of the same
// name ("duplicate child name in GtkStack"), whose button then opens the
// first: Shared's Move to This Workspace showed Reading's bookmarks so. A
// label met again in one menu gets zero-width spaces after it, unseen.
struct Builder {
    size_t left = 0;
    std::set<std::string> visited;
    std::map<std::string, int> labels;

    void appendSubmenu(GMenu* m, const std::string& text, GMenu* sub) {
        std::string label = menuLabel(text);
        for (int n = labels[label]++; n > 0; n--) label += "\xE2\x80\x8B";   // U+200B
        g_menu_append_submenu(m, label.c_str(), G_MENU_MODEL(sub));
    }

    // "Move to This Workspace" as the last section of `m` (a shared
    // folder's submenu): a submenu of the bookmark files among `rows`, that
    // folder's; nothing when it has none.
    void appendMoveItems(GMenu* m, const std::vector<WebBookmark::Entry>& rows) {
        GMenu* files = g_menu_new();
        for (const WebBookmark::Entry& e : rows)
            if (!e.folder) appendItem(files, e.title, "win.movebookmark", e.path);
        if (g_menu_model_get_n_items(G_MENU_MODEL(files)) > 0) {
            GMenu* tail = g_menu_new();
            appendSubmenu(tail, "Move to This Workspace", files);
            g_menu_append_section(m, nullptr, G_MENU_MODEL(tail));
            g_object_unref(tail);
        }
        g_object_unref(files);
    }

    // One folder's rows (WebBookmark::entries) into `m`: a bookmark an
    // item, a folder a submenu opening with Open All in Tabs and, below
    // the shared folder, ending with Move to This Workspace.
    void appendRows(GMenu* m, const std::vector<WebBookmark::Entry>& rows, int depth) {
        for (const WebBookmark::Entry& e : rows) {
            if (left == 0) return;
            --left;
            if (!e.folder) {
                appendItem(m, e.title, "win.openbookmark", e.path);
                continue;
            }
            GMenu* sub = g_menu_new();
            GMenu* head = g_menu_new();
            appendItem(head, "Open All in Tabs", "win.openbookmarkfolder", e.path);
            g_menu_append_section(sub, nullptr, G_MENU_MODEL(head));
            g_object_unref(head);
            GMenu* body = g_menu_new();
            const std::vector<WebBookmark::Entry> inner = fill(body, e.path, e.shared, depth + 1);
            g_menu_append_section(sub, nullptr, G_MENU_MODEL(body));
            g_object_unref(body);
            if (e.shared) appendMoveItems(sub, inner);
            appendSubmenu(m, e.title, sub);
            g_object_unref(sub);
        }
    }

    // The folder `dir`'s rows into `m`; returns them. Six levels at most.
    std::vector<WebBookmark::Entry> fill(GMenu* m, const std::string& dir, bool shared, int depth) {
        char real[PATH_MAX];
        if (depth > 6 || !realpath(dir.c_str(), real) || !visited.insert(real).second) return {};
        const std::vector<WebBookmark::Entry> rows = WebBookmark::entries(dir, names(dir), shared, left);
        appendRows(m, rows, depth);
        return rows;
    }

    // A top folder as already in the menu, so a link back to it is cut.
    void enter(const std::string& dir, size_t limit) {
        char real[PATH_MAX];
        if (realpath(dir.c_str(), real)) visited.insert(real);
        left = limit;
    }
};

}  // namespace

GMenu* menu(const WebBookmark::Places& p, size_t limit) {
    GMenu* m = g_menu_new();
    const std::vector<WebBookmark::Entry> rows = WebBookmark::mergedEntries(
        p, names(p.save), p.shared.empty() ? std::vector<WebBookmark::Name>() : names(p.shared), limit);
    std::vector<WebBookmark::Entry> own, shared;
    for (const WebBookmark::Entry& e : rows) (e.shared ? shared : own).push_back(e);
    Builder b;

    // The window's own folder's rows, as the one folder's were.
    b.enter(p.save, limit);
    b.appendRows(m, own, 0);
    if (own.empty()) g_menu_append(m, "No bookmarks", "win.nobookmarks");

    // Then Shared, when the shared folder has anything.
    if (!shared.empty()) {
        GMenu* sub = g_menu_new();
        GMenu* body = g_menu_new();
        b.enter(p.shared, limit);
        b.appendRows(body, shared, 0);
        g_menu_append_section(sub, nullptr, G_MENU_MODEL(body));
        g_object_unref(body);
        b.appendMoveItems(sub, shared);
        GMenu* part = g_menu_new();
        b.appendSubmenu(part, "Shared", sub);
        g_menu_append_section(m, nullptr, G_MENU_MODEL(part));
        g_object_unref(part);
        g_object_unref(sub);
    }
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

std::string fileFor(const WebBookmark::Places& p, const std::string& url) {
    for (const std::string& dir : WebBookmark::lookupOrder(p)) {
        const std::string have = fileFor(dir, url);
        if (!have.empty()) return have;
    }
    return "";
}

std::string moveToWorkspace(const WebBookmark::Places& p, const std::string& file,
                            std::string* error) {
    auto taken = [&p](const std::string& n) { return exists(p.save + "/" + n); };
    if (WebBookmark::moveDestination(p, file, taken).empty()) {
        if (error) *error = "it is not one of the shared bookmarks";
        return "";
    }
    if (!exists(file)) {
        if (error) *error = "it is no longer there";
        return "";
    }
    if (!ensureFolder(p.save, error)) return "";
    // Never over a file: without G_FILE_COPY_OVERWRITE the move refuses a
    // name taken since it was found free, and the next free one is tried.
    // (Across file systems GIO copies and deletes.)
    GFile* from = g_file_new_for_path(file.c_str());
    std::string to, why = "no free name for the bookmark";
    for (int attempt = 0; attempt < 5; attempt++) {
        const std::string dest = WebBookmark::moveDestination(p, file, taken);
        GFile* target = g_file_new_for_path(dest.c_str());
        GError* err = nullptr;
        const bool moved =
            g_file_move(from, target, G_FILE_COPY_NOFOLLOW_SYMLINKS, nullptr, nullptr, nullptr, &err);
        g_object_unref(target);
        if (moved) {
            to = dest;
            break;
        }
        const bool again = err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS);
        why = err ? err->message : "the file could not be moved";
        g_clear_error(&err);
        if (!again) break;
    }
    g_object_unref(from);
    if (to.empty() && error) *error = why;
    return to;
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
