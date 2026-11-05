// Backups.cpp — see Backups.h.
#include "Backups.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Backups {

static const char *const kMagic = "minicode-backup 1";
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
    line("line-endings", lineEndingsOf(text));
    line("saved", num(e.saved));
    line("owner-pid", std::to_string(e.ownerPid));
    line("owner-start", num(e.ownerStart));
    line("bytes", std::to_string(text.size()));
    h += "\n";
    h += text;
    return h;
}

bool decodeHeader(const std::string &data, Entry *e, size_t *textStart) {
    const std::string magic = std::string(kMagic) + "\n";
    if (data.compare(0, magic.size(), magic) != 0) return false;
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
        else if (k == "line-endings") out.lineEndings = v;
        else if (k == "saved") out.saved = std::strtod(v.c_str(), &stop);
        else if (k == "owner-pid") out.ownerPid = std::strtoll(v.c_str(), &stop, 10);
        else if (k == "owner-start") out.ownerStart = std::strtod(v.c_str(), &stop);
        else if (k == "bytes") {
            out.bytes = std::strtoull(v.c_str(), &stop, 10);
            haveBytes = true;
        }
        // Unknown keys are skipped, so a later minicode can add some.
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
        if (ok && fsync(fd) != 0) ok = false;
        if (close(fd) != 0) ok = false;
        if (ok && rename(tmp.c_str(), path.c_str()) != 0) ok = false;
        if (!ok) unlink(tmp.c_str());
        return ok;
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
             const std::vector<std::string> &sessionFiles, const IsLive &live) {
    Restore r;
    // The newest backup of each file in this folder; older ones go.
    std::map<std::string, const Entry *> newest;
    for (const Entry &e : all) {
        if (e.root != root || live(e)) continue;
        auto it = newest.find(e.path);
        if (it == newest.end()) { newest[e.path] = &e; continue; }
        const Entry *&have = it->second;
        if (e.saved > have->saved || (e.saved == have->saved && e.id > have->id)) {
            r.discard.push_back(have->id);
            have = &e;
        } else {
            r.discard.push_back(e.id);
        }
    }
    std::set<std::string> taken;
    for (const std::string &f : sessionFiles) {
        auto it = newest.find(f);
        if (it == newest.end() || taken.count(f)) continue;
        taken.insert(f);
        r.items.push_back({f, it->second->id, true});
    }
    std::vector<const Entry *> rest;
    for (const auto &kv : newest)
        if (!taken.count(kv.first)) rest.push_back(kv.second);
    std::sort(rest.begin(), rest.end(), [](const Entry *a, const Entry *b) {
        return a->saved != b->saved ? a->saved > b->saved : a->path < b->path;
    });
    for (const Entry *e : rest) r.items.push_back({e->path, e->id, false});
    return r;
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

}  // namespace Backups
