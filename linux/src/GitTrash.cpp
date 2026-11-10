// GitTrash.cpp — see GitTrash.h.
#include "GitTrash.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mutex>
#include <vector>

namespace GitTrash {
namespace {

std::mutex gMutex;
std::string gFolder;   // the checks' stand-in, or ""
int gCount = 0;

// The checks' folder, and the name the next file gets in it, or "".
std::string checksTarget(const std::string& path) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gFolder.empty()) return "";
    g_mkdir_with_parents(gFolder.c_str(), 0700);
    gchar* base = g_path_get_basename(path.c_str());
    std::string to = gFolder + "/" + std::to_string(++gCount) + "-" + base;
    g_free(base);
    return to;
}

std::string message(GError* e, const char* fallback) {
    std::string out = e && e->message ? e->message : fallback;
    return out;
}

// One file or folder moved (a rename on one filesystem, a copy and delete
// across two), symlinks as they are, never over something there.
bool move(const std::string& from, const std::string& to, std::string* error) {
    GFile* a = g_file_new_for_path(from.c_str());
    GFile* b = g_file_new_for_path(to.c_str());
    GError* e = nullptr;
    const bool ok = g_file_move(a, b,
                                (GFileCopyFlags)(G_FILE_COPY_NOFOLLOW_SYMLINKS |
                                                 G_FILE_COPY_ALL_METADATA),
                                nullptr, nullptr, nullptr, &e);
    if (!ok && error) *error = message(e, "it could not be moved");
    g_clear_error(&e);
    g_object_unref(a);
    g_object_unref(b);
    return ok;
}

bool copy(const std::string& from, const std::string& to, std::string* error) {
    GFile* a = g_file_new_for_path(from.c_str());
    GFile* b = g_file_new_for_path(to.c_str());
    GError* e = nullptr;
    const bool ok = g_file_copy(a, b,
                                (GFileCopyFlags)(G_FILE_COPY_NOFOLLOW_SYMLINKS |
                                                 G_FILE_COPY_ALL_METADATA),
                                nullptr, nullptr, nullptr, &e);
    if (!ok && error) *error = message(e, "it could not be copied");
    g_clear_error(&e);
    g_object_unref(a);
    g_object_unref(b);
    return ok;
}

std::string dirOf(const std::string& p) {
    gchar* d = g_path_get_dirname(p.c_str());
    std::string out = d;
    g_free(d);
    return out;
}

std::string baseOf(const std::string& p) {
    gchar* b = g_path_get_basename(p.c_str());
    std::string out = b;
    g_free(b);
    return out;
}

// A Trash folder (with files/ and info/ in it) that may have taken a file,
// and the top of the filesystem a relative Path in its .trashinfo is under.
struct Place {
    std::string dir;
    std::string top;   // "" for the home Trash, whose Paths are absolute
};

// Where GIO puts a file, as g_local_file_trash chooses: the home Trash for
// the home folder's filesystem, else the top of the file's own. All of them
// are looked in, in case GIO went past the first.
std::vector<Place> placesFor(const std::string& path) {
    std::vector<Place> out;
    out.push_back({std::string(g_get_user_data_dir()) + "/Trash", ""});
    struct stat fileSt;
    if (lstat(path.c_str(), &fileSt) != 0) return out;
    std::string top = dirOf(path);
    for (;;) {
        if (top == "/") break;
        const std::string up = dirOf(top);
        struct stat upSt;
        if (stat(up.c_str(), &upSt) != 0 || upSt.st_dev != fileSt.st_dev) break;
        top = up;
    }
    const std::string uid = std::to_string((unsigned long)getuid());
    const std::string base = top == "/" ? "" : top;
    out.push_back({base + "/.Trash/" + uid, top});
    out.push_back({base + "/.Trash-" + uid, top});
    return out;
}

// The trashed file GIO made of `path` just now: the newest .trashinfo in the
// places whose Path names it, and its file under files/.
std::string findTrashed(const std::string& path) {
    GFile* f = g_file_new_for_path(path.c_str());
    gchar* canon = g_file_get_path(f);
    const std::string want = canon ? canon : path;
    g_free(canon);
    g_object_unref(f);
    std::string best;
    gint64 bestTime = -1;
    for (const Place& p : placesFor(path)) {
        const std::string info = p.dir + "/info";
        GDir* d = g_dir_open(info.c_str(), 0, nullptr);
        if (!d) continue;
        while (const char* name = g_dir_read_name(d)) {
            const std::string n = name;
            const std::string suffix = ".trashinfo";
            if (n.size() <= suffix.size() || n.compare(n.size() - suffix.size(), suffix.size(), suffix))
                continue;
            const std::string file = info + "/" + n;
            gchar* text = nullptr;
            if (!g_file_get_contents(file.c_str(), &text, nullptr, nullptr)) continue;
            std::string recorded;
            gchar** lines = g_strsplit(text, "\n", -1);
            for (gchar** l = lines; *l; l++) {
                if (!g_str_has_prefix(*l, "Path=")) continue;
                if (gchar* un = g_uri_unescape_string(*l + 5, nullptr)) {
                    recorded = un;
                    g_free(un);
                }
            }
            g_strfreev(lines);
            g_free(text);
            if (recorded.empty()) continue;
            if (recorded[0] != '/') recorded = (p.top == "/" ? "" : p.top) + "/" + recorded;
            if (recorded != want) continue;
            GStatBuf st;
            if (g_stat(file.c_str(), &st) != 0) continue;
            const gint64 t = (gint64)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
            const std::string trashed = p.dir + "/files/" + n.substr(0, n.size() - suffix.size());
            struct stat there;
            if (t > bestTime && lstat(trashed.c_str(), &there) == 0) {
                bestTime = t;
                best = trashed;
            }
        }
        g_dir_close(d);
    }
    return best;
}

// The .trashinfo of a file in a Trash's files/ folder, or "".
std::string infoOf(const std::string& where) {
    const std::string files = dirOf(where);
    if (baseOf(files) != "files") return "";
    return dirOf(files) + "/info/" + baseOf(where) + ".trashinfo";
}

// Deletes a file, or a folder and everything in it.
void removeAll(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (GDir* d = g_dir_open(path.c_str(), 0, nullptr)) {
            while (const char* name = g_dir_read_name(d)) removeAll(path + "/" + name);
            g_dir_close(d);
        }
        g_rmdir(path.c_str());
        return;
    }
    g_unlink(path.c_str());
}

}  // namespace

void useFolderForChecks(const std::string& dir) {
    std::lock_guard<std::mutex> lock(gMutex);
    gFolder = dir;
}

bool trash(const std::string& path, std::string* where, std::string* error) {
    if (where) where->clear();
    const std::string to = checksTarget(path);
    if (!to.empty()) {
        if (!move(path, to, error)) return false;
        if (where) *where = to;
        return true;
    }
    GFile* f = g_file_new_for_path(path.c_str());
    GError* e = nullptr;
    const bool ok = g_file_trash(f, nullptr, &e);
    g_object_unref(f);
    if (!ok) {
        if (error) *error = message(e, "the Trash refused it");
        g_clear_error(&e);
        return false;
    }
    // Moved; if where it went cannot be found, Undo Discard says so.
    if (where) *where = findTrashed(path);
    return true;
}

bool trashCopy(const std::string& path, std::string* where, std::string* error) {
    if (where) where->clear();
    const std::string to = checksTarget(path);
    if (!to.empty()) {
        if (!copy(path, to, error)) return false;
        if (where) *where = to;
        return true;
    }
    // A folder of our own beside the file, so the copy is on the file's own
    // filesystem and goes to the Trash the file would.
    std::string tmpl = dirOf(path) + "/.minicode-discard-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!g_mkdtemp_full(buf.data(), 0700)) {
        if (error) *error = "no folder could be made beside it";
        return false;
    }
    const std::string dir = buf.data();
    const std::string copyPath = dir + "/" + baseOf(path);
    bool ok = copy(path, copyPath, error) && trash(copyPath, where, error);
    removeAll(dir);
    return ok;
}

bool putBack(const std::string& where, const std::string& path, std::string* error) {
    struct stat st;
    if (lstat(path.c_str(), &st) == 0) {
        if (error) *error = "a file is in its place";
        return false;
    }
    g_mkdir_with_parents(dirOf(path).c_str(), 0755);
    if (!move(where, path, error)) return false;
    const std::string info = infoOf(where);
    if (!info.empty()) g_unlink(info.c_str());
    return true;
}

void remove(const std::string& where) {
    if (where.empty()) return;
    removeAll(where);
    const std::string info = infoOf(where);
    if (!info.empty()) g_unlink(info.c_str());
}

bool stillThere(const std::string& where) {
    struct stat st;
    return !where.empty() && lstat(where.c_str(), &st) == 0;
}

}  // namespace GitTrash
