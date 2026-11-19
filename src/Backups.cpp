// Backups.cpp — see Backups.h.
#include "Backups.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace Backups {

static const char *const kMagic = "mutible-backup 1";
static const char *const kSuffix = ".mcbackup";

std::string lineEndingsOf(const std::string &text) {
    bool lf = false, crlf = false, cr = false;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') { crlf = true; i++; }
            else cr = true;
        } else if (text[i] == '\n') {
            lf = true;
        }
    }
    const int kinds = (int)lf + (int)crlf + (int)cr;
    if (kinds == 0) return "none";
    if (kinds > 1) return "mixed";
    return lf ? "lf" : crlf ? "crlf" : "cr";
}

bool validId(const std::string &id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-'))
            return false;
    return true;
}

std::string fileName(const std::string &id) { return id + kSuffix; }

// Header values are one line: '%', CR and LF are written %25, %0D, %0A.
static std::string escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '%') out += "%25";
        else if (c == '\n') out += "%0A";
        else if (c == '\r') out += "%0D";
        else out += c;
    }
    return out;
}
static std::string unescape(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const std::string h = s.substr(i + 1, 2);
            if (h == "25") { out += '%'; i += 2; continue; }
            if (h == "0A") { out += '\n'; i += 2; continue; }
            if (h == "0D") { out += '\r'; i += 2; continue; }
        }
        out += s[i];
    }
    return out;
}

static std::string num(double d) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", d);   // read back exactly
    return buf;
}

std::string encode(const Entry &e, const std::string &text) {
    std::string h = kMagic;
    h += "\n";
    auto line = [&h](const char *k, const std::string &v) {
        h += k;
        h += ": ";
        h += escape(v);
        h += "\n";
    };
    line("id", e.id);
    line("path", e.path);
    line("root", e.root);
    line("disk-existed", e.diskExisted ? "1" : "0");
    line("disk-mtime", num(e.diskMtime));
    line("disk-inode", std::to_string(e.diskInode));
    line("encoding", e.encoding);
    if (!e.fileEncoding.empty()) line("file-encoding", e.fileEncoding);
    if (!e.window.empty()) line("window", e.window);
    line("line-endings", lineEndingsOf(text));
    line("saved", num(e.saved));
    line("owner-pid", std::to_string(e.ownerPid));
    line("owner-start", num(e.ownerStart));
    line("bytes", std::to_string(text.size()));
    h += "\n";
    h += text;
    return h;
}

// rename-mutible: keep-begin
// The magic before 1.6.0 (src/Rename.h): the first launch copies minicode's
// Backups folder, and the unsaved tabs in it come back as they would have.
static const char *const kOldMagic = "minicode-backup 1";
// rename-mutible: keep-end

bool decodeHeader(const std::string &data, Entry *e, size_t *textStart) {
    std::string magic = std::string(kMagic) + "\n";
    if (data.compare(0, magic.size(), magic) != 0) {
        magic = std::string(kOldMagic) + "\n";
        if (data.compare(0, magic.size(), magic) != 0) return false;
    }
    const size_t end = data.find("\n\n", magic.size() - 1);
    if (end == std::string::npos) return false;
    Entry out;
    bool haveId = false, haveBytes = false;
    size_t pos = magic.size();
    while (pos < end + 1) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos || nl > end) nl = end;
        const std::string ln = data.substr(pos, nl - pos);
        pos = nl + 1;
        const size_t colon = ln.find(": ");
        if (colon == std::string::npos) continue;
        const std::string k = ln.substr(0, colon), v = unescape(ln.substr(colon + 2));
        char *stop = nullptr;
        if (k == "id") { out.id = v; haveId = true; }
        else if (k == "path") out.path = v;
        else if (k == "root") out.root = v;
        else if (k == "disk-existed") out.diskExisted = v == "1";
        else if (k == "disk-mtime") out.diskMtime = std::strtod(v.c_str(), &stop);
        else if (k == "disk-inode") out.diskInode = std::strtoull(v.c_str(), &stop, 10);
        else if (k == "encoding") out.encoding = v;
        else if (k == "file-encoding") out.fileEncoding = v;
        else if (k == "window") out.window = validId(v) ? v : std::string();
        else if (k == "line-endings") out.lineEndings = v;
        else if (k == "saved") out.saved = std::strtod(v.c_str(), &stop);
        else if (k == "owner-pid") out.ownerPid = std::strtoll(v.c_str(), &stop, 10);
        else if (k == "owner-start") out.ownerStart = std::strtod(v.c_str(), &stop);
        else if (k == "bytes") {
            out.bytes = std::strtoull(v.c_str(), &stop, 10);
            haveBytes = true;
        }
        // Unknown keys are skipped, so a later mutible can add some.
    }
    if (!haveId || !haveBytes || !validId(out.id) || out.path.empty()) return false;
    *e = out;
    if (textStart) *textStart = end + 2;
    return true;
}

bool decode(const std::string &data, Entry *e, std::string *text) {
    Entry h;
    size_t start = 0;
    if (!decodeHeader(data, &h, &start)) return false;
    if (data.size() - start != h.bytes) return false;
    *e = h;
    if (text) *text = data.substr(start);
    return true;
}

// ------------------------------------------------------------------ files
// Writes a file's (or a folder's) data through to the disk itself, not just
// to the drive's cache.
static bool syncToDisk(int fd) {
#ifdef F_FULLFSYNC
    if (fcntl(fd, F_FULLFSYNC) == 0) return true;
#endif
    while (fsync(fd) != 0) {
        if (errno != EINTR) return false;
    }
    return true;
}

Fs posixFs() {
    Fs fs;
    fs.list = [](const std::string &dir) {
        std::vector<std::string> names;
        if (DIR *d = opendir(dir.c_str())) {
            while (dirent *ent = readdir(d)) {
                const std::string n = ent->d_name;
                if (n != "." && n != "..") names.push_back(n);
            }
            closedir(d);
        }
        std::sort(names.begin(), names.end());
        return names;
    };
    fs.read = [](const std::string &path, std::string *out, size_t maxBytes) {
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        out->clear();
        char buf[65536];
        while (maxBytes == 0 || out->size() < maxBytes) {
            size_t want = sizeof buf;
            if (maxBytes) want = std::min(want, maxBytes - out->size());
            const ssize_t n = ::read(fd, buf, want);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) { close(fd); return false; }
            if (n == 0) break;
            out->append(buf, (size_t)n);
        }
        close(fd);
        return true;
    };
    fs.writeAtomic = [](const std::string &path, const std::string &data) {
        const size_t slash = path.rfind('/');
        const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
        const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        // The folder, made private when missing (and its parents).
        struct stat st;
        if (::stat(dir.c_str(), &st) != 0) {
            for (size_t i = 1; i <= dir.size(); i++)
                if (i == dir.size() || dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0700);
        }
        static std::atomic<unsigned> counter{0};
        const std::string tmp = dir + "/." + name + "." + std::to_string(getpid()) + "." +
                                std::to_string(++counter) + ".tmp";
        const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) return false;
        size_t done = 0;
        bool ok = true;
        while (done < data.size()) {
            const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ok = false; break; }
            done += (size_t)n;
        }
        // On the Mac fsync only hands the data to the drive, whose cache
        // can still lose it when the power goes; F_FULLFSYNC asks the
        // drive to write it out. Some volumes (network ones) refuse it,
        // and then plain fsync is the best there is.
        if (ok && !syncToDisk(fd)) ok = false;
        if (close(fd) != 0) ok = false;
        if (ok && rename(tmp.c_str(), path.c_str()) != 0) ok = false;
        if (!ok) {
            unlink(tmp.c_str());
            return false;
        }
        // The rename is an entry in the folder, so the folder is synced
        // too, or after a power loss the backup may not be found under its
        // name. A failure here leaves a complete file, so it is not one.
        const int dfd = open(dir.c_str(), O_RDONLY | O_CLOEXEC);
        if (dfd >= 0) {
            syncToDisk(dfd);
            close(dfd);
        }
        return true;
    };
    fs.remove = [](const std::string &path) {
        return unlink(path.c_str()) == 0 || errno == ENOENT;
    };
    fs.stat = [](const std::string &path) {
        Stat s;
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) return s;
        s.exists = true;
        s.regular = S_ISREG(st.st_mode);
#ifdef __APPLE__
        s.mtime = (double)st.st_mtimespec.tv_sec + st.st_mtimespec.tv_nsec / 1e9;
#else
        s.mtime = (double)st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9;
#endif
        s.inode = (uint64_t)st.st_ino;
        return s;
    };
    return fs;
}

Disk compare(const Entry &e, const Stat &now) {
    if (!e.diskExisted) return now.exists ? Disk::Appeared : Disk::NeverSaved;
    if (!now.exists) return Disk::Gone;
    // A millisecond of slack: dates go through NSDate and back.
    const bool sameTime = now.mtime - e.diskMtime < 0.001 && e.diskMtime - now.mtime < 0.001;
    const bool sameFile = e.diskInode == 0 || now.inode == 0 || e.diskInode == now.inode;
    return sameTime && sameFile ? Disk::Same : Disk::Changed;
}

std::string Store::pathFor(const std::string &id) const { return dir + "/" + fileName(id); }

bool Store::save(const Entry &e, const std::string &text) const {
    if (!validId(e.id)) return false;
    return fs.writeAtomic(pathFor(e.id), encode(e, text));
}

bool Store::discard(const std::string &id) const {
    if (!validId(id)) return false;
    return fs.remove(pathFor(id));
}

static bool isBackupName(const std::string &n, std::string *id) {
    const std::string suffix = kSuffix;
    if (n.size() <= suffix.size() || n[0] == '.') return false;
    if (n.compare(n.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    *id = n.substr(0, n.size() - suffix.size());
    return validId(*id);
}

std::vector<Entry> Store::list() const {
    std::vector<Entry> out;
    for (const std::string &n : fs.list(dir)) {
        std::string id;
        if (!isBackupName(n, &id)) continue;
        std::string head;
        // A header is a few hundred bytes; long paths still fit.
        if (!fs.read(dir + "/" + n, &head, 16384)) continue;
        Entry e;
        if (decodeHeader(head, &e) && e.id == id) out.push_back(e);
    }
    return out;
}

bool Store::load(const std::string &id, Entry *e, std::string *text) const {
    if (!validId(id)) return false;
    std::string data;
    if (!fs.read(pathFor(id), &data, 0)) return false;
    Entry got;
    if (!decode(data, &got, text) || got.id != id) return false;
    *e = got;
    return true;
}

int Store::sweepDebris(double now, double maxAge, double badAge) const {
    int gone = 0;
    for (const std::string &n : fs.list(dir)) {
        const std::string p = dir + "/" + n;
        std::string id;
        const bool tmp = n.size() > 4 && n[0] == '.' && n.compare(n.size() - 4, 4, ".tmp") == 0;
        const bool backup = isBackupName(n, &id);
        if (!tmp && !backup) continue;
        const Stat s = fs.stat(p);
        if (!s.exists || !s.regular) continue;
        if (tmp) {
            if (now - s.mtime > maxAge && fs.remove(p)) gone++;
            continue;
        }
        std::string head;
        Entry e;
        if (fs.read(p, &head, 16384) && decodeHeader(head, &e)) continue;
        if (now - s.mtime > badAge && fs.remove(p)) gone++;
    }
    return gone;
}

// ------------------------------------------------------------- decisions
Restore plan(const std::vector<Entry> &all, const std::string &root,
             const std::vector<std::string> &sessionFiles, const IsLive &live,
             const std::string &window, const std::set<std::string> &reserved) {
    Restore r;
    // The folder's backups this window may take: its own, and those of no
    // window still to come back (an older backup, a window not restored).
    std::map<std::string, std::vector<const Entry *>> byPath;
    for (const Entry &e : all) {
        if (e.root != root || live(e)) continue;
        if (!e.window.empty() && e.window != window && reserved.count(e.window)) continue;
        byPath[e.path].push_back(&e);
    }
    // Of several for one file, this window's own newest goes in the tab,
    // else the newest; every other is a draft of its own, never deleted.
    std::map<std::string, const Entry *> chosen;
    auto better = [&window](const Entry *a, const Entry *b) {
        const bool ownA = !window.empty() && a->window == window;
        const bool ownB = !window.empty() && b->window == window;
        if (ownA != ownB) return ownA;
        if (a->saved != b->saved) return a->saved > b->saved;
        return a->id > b->id;
    };
    for (auto &kv : byPath) {
        std::vector<const Entry *> &v = kv.second;
        std::sort(v.begin(), v.end(), better);
        chosen[kv.first] = v.front();
        for (size_t i = 1; i < v.size(); i++) r.conflicts.push_back({kv.first, v[i]->id, false});
    }
    std::set<std::string> taken;
    for (const std::string &f : sessionFiles) {
        auto it = chosen.find(f);
        if (it == chosen.end() || taken.count(f)) continue;
        taken.insert(f);
        r.items.push_back({f, it->second->id, true});
    }
    std::vector<const Entry *> rest;
    for (const auto &kv : chosen)
        if (!taken.count(kv.first)) rest.push_back(kv.second);
    std::sort(rest.begin(), rest.end(), [](const Entry *a, const Entry *b) {
        return a->saved != b->saved ? a->saved > b->saved : a->path < b->path;
    });
    for (const Entry *e : rest) r.items.push_back({e->path, e->id, false});
    return r;
}

std::string draftPath(const std::string &path,
                      const std::function<bool(const std::string &)> &taken) {
    const size_t slash = path.rfind('/');
    const std::string dir = slash == std::string::npos ? "" : path.substr(0, slash + 1);
    const std::string name = path.substr(dir.size());
    // "a.txt" -> "a (unsaved copy).txt"; a name with no extension, or a
    // dot file, takes it at the end.
    const size_t dot = name.rfind('.');
    const bool ext = dot != std::string::npos && dot > 0;
    const std::string stem = ext ? name.substr(0, dot) : name;
    const std::string tail = ext ? name.substr(dot) : "";
    for (int n = 1; n < 10000; n++) {
        const std::string mark =
            n == 1 ? " (unsaved copy)" : " (unsaved copy " + std::to_string(n) + ")";
        const std::string p = dir + stem + mark + tail;
        if (!taken || !taken(p)) return p;
    }
    return dir + stem + " (unsaved copy " + newId() + ")" + tail;
}

std::vector<std::string> foldersToReopen(const std::vector<Entry> &all,
                                         const std::vector<std::string> &openRoots,
                                         const IsLive &live) {
    std::map<std::string, double> latest;
    for (const Entry &e : all) {
        if (e.root.empty() || live(e)) continue;
        if (std::find(openRoots.begin(), openRoots.end(), e.root) != openRoots.end()) continue;
        double &t = latest[e.root];
        t = std::max(t, e.saved);
    }
    std::vector<std::pair<double, std::string>> v;
    for (const auto &kv : latest) v.push_back({kv.second, kv.first});
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    std::vector<std::string> out;
    for (const auto &p : v) out.push_back(p.second);
    return out;
}

std::vector<std::string> expired(const std::vector<Entry> &all, double now, double maxAge,
                                 const IsLive &live) {
    std::vector<std::string> out;
    for (const Entry &e : all)
        if (now - e.saved > maxAge && !live(e)) out.push_back(e.id);
    return out;
}

// ------------------------------------------------- shared by every writer
std::string defaultDir(const char *override, const char *home, const char *xdgDataHome, bool mac) {
    std::string dir;
    if (override && *override) {
        dir = override;
    } else if (mac) {
        if (!home || !*home) return "";
        dir = std::string(home) + "/Library/Application Support/Mutible/Backups";
    } else if (xdgDataHome && *xdgDataHome && xdgDataHome[0] == '/') {
        dir = std::string(xdgDataHome) + "/mutible/Backups";
    } else {
        if (!home || !*home) return "";
        dir = std::string(home) + "/.local/share/mutible/Backups";
    }
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    return dir;
}

double processStart(int64_t pid) {
    if (pid <= 0) return 0;
#ifdef __APPLE__
    struct kinfo_proc info;
    size_t size = sizeof info;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid};
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size == 0) return 0;
    if (info.kp_proc.p_stat == SZOMB) return 0;
    const struct timeval tv = info.kp_proc.p_starttime;
    return (double)tv.tv_sec + tv.tv_usec / 1e6;
#else
    // "pid (comm) state ppid ... starttime ...": the start is field 22, in
    // clock ticks since boot; comm may hold spaces and parentheses, so the
    // fields are counted from the last ')'.
    std::string text;
    if (!posixFs().read("/proc/" + std::to_string((long long)pid) + "/stat", &text, 4096)) return 0;
    const size_t close = text.rfind(')');
    if (close == std::string::npos || close + 2 >= text.size()) return 0;
    const char state = text[close + 2];
    if (state == 'Z' || state == 'X') return 0;
    size_t p = close + 2;
    for (int field = 3; field < 22; field++) {
        p = text.find(' ', p);
        if (p == std::string::npos) return 0;
        p++;
    }
    const long ticks = sysconf(_SC_CLK_TCK);
    if (ticks <= 0) return 0;
    return (double)std::strtoull(text.c_str() + p, nullptr, 10) / (double)ticks;
#endif
}

std::string newId() {
    std::random_device rd;
    std::string id;
    static const char *const kHex = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        if (i == 8 || i == 12 || i == 16 || i == 20) id += '-';
        id += kHex[rd() & 15];
    }
    return id;
}

std::string untitledPath(const std::string &folder,
                         const std::function<bool(const std::string &)> &taken) {
    std::string base = folder;
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    for (int n = 1;; n++) {
        const std::string p = base + "/Untitled-" + std::to_string(n);
        if (!taken || !taken(p)) return p;
    }
}

}  // namespace Backups
