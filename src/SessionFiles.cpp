// SessionFiles.cpp — see SessionFiles.h.
#include "SessionFiles.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "Document.h"
#include "FileDetails.h"
#include "FolderSearch.h"
#include "Text.h"

namespace Shared {

namespace {

// The real place of `path`, symlinks followed; "" when there is none.
std::string realPlace(const std::string& path) {
    char buf[PATH_MAX];
    if (!::realpath(path.c_str(), buf)) return std::string();
    return buf;
}

bool inside(const std::string& realRoot, const std::string& real) {
    if (real == realRoot) return true;
    const std::string prefix = realRoot == "/" ? realRoot : realRoot + "/";
    return real.compare(0, prefix.size(), prefix) == 0;
}

// The names of `rel`, or false (with why) for one that is not a path from
// the folder.
bool namesOf(const std::string& rel, std::vector<std::string>* out, std::string* why) {
    out->clear();
    if (rel.empty()) return true;
    if (rel.find('\0') != std::string::npos) {
        *why = "a path with a NUL in it";
        return false;
    }
    if (rel[0] == '/') {
        *why = "an absolute path";
        return false;
    }
    std::size_t at = 0;
    for (;;) {
        const std::size_t slash = rel.find('/', at);
        const std::string name = rel.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        if (name.empty() || name == ".") {
            *why = "a path with an empty or \".\" name";
            return false;
        }
        if (name == "..") {
            *why = "a path that leaves the folder";
            return false;
        }
        out->push_back(name);
        if (slash == std::string::npos) return true;
        at = slash + 1;
    }
}

Json failed(const std::string& op, const std::string& path, const std::string& why) {
    Json j = Json::object();
    j.set("op", Json(op));
    j.set("path", Json(path));
    j.set("error", Json(why));
    return j;
}

Json listFolder(const std::string& root, const std::string& rel, bool hidden) {
    std::string abs, why;
    if (!confinedPath(root, rel, &abs, &why)) return failed("list", rel, why);
    const std::string realRoot = realPlace(root);
    DIR* d = ::opendir(abs.c_str());
    if (!d) return failed("list", rel, std::strerror(errno));
    struct Item {
        std::string name;
        bool dir = false, link = false, outside = false;
    };
    std::vector<Item> items;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        if (!hidden && name[0] == '.') continue;   // dotfiles, as the apps' trees
        Item it;
        it.name = name;
        const std::string full = abs + "/" + name;
        struct stat lst, st;
        if (::lstat(full.c_str(), &lst) != 0) continue;   // gone meanwhile
        it.link = S_ISLNK(lst.st_mode);
        if (it.link) {
            it.dir = ::stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            const std::string real = realPlace(full);
            it.outside = !real.empty() && !inside(realRoot, real);
        } else {
            it.dir = S_ISDIR(lst.st_mode);
        }
        items.push_back(std::move(it));
    }
    ::closedir(d);

    // The ignore files from the folder's top down to this one; a folder
    // they ignore leaves everything below it ignored, as git has it.
    std::vector<std::string> names;
    namesOf(rel, &names, &why);
    FolderSearch::Ignore::Rules rules = FolderSearch::Ignore::Rules::atTop(root);
    bool allIgnored = false;
    std::string dir = root, sofar;
    for (const std::string& n : names) {
        sofar = sofar.empty() ? n : sofar + "/" + n;
        if (!allIgnored && rules.ignored(sofar, true)) allIgnored = true;
        dir += "/" + n;
        if (!allIgnored) rules = rules.enter(dir, sofar);
    }

    std::vector<TreeSort::Entry> keys(items.size());
    for (std::size_t i = 0; i < items.size(); i++) {
        keys[i].name = items[i].name;
        keys[i].isDir = items[i].dir;
    }
    Json entries = Json::array();
    std::size_t listed = 0;
    for (std::size_t i : TreeSort::order(keys, TreeSort::Options())) {
        if (listed == kMaxListed) break;
        const Item& it = items[i];
        Json e = Json::object();
        e.set("name", Json(it.name));
        if (it.dir) e.set("dir", Json(true));
        if (it.link) e.set("link", Json(true));
        if (it.outside) e.set("outside", Json(true));
        if (allIgnored || rules.ignored(sofar.empty() ? it.name : sofar + "/" + it.name, it.dir))
            e.set("ignored", Json(true));
        if (!it.dir) e.set("kind", Json(FileIcon::kindName(FileIcon::kindOf(it.name))));
        entries.push(std::move(e));
        listed++;
    }
    Json j = Json::object();
    j.set("op", Json("list"));
    j.set("path", Json(rel));
    j.set("root", Json(root));
    j.set("entries", std::move(entries));
    if (items.size() > listed) j.set("more", Json((int64_t)(items.size() - listed)));
    return j;
}

// A piece of one file's bytes (the "read" op).
Json readFile(const std::string& root, const Json& request, std::string* bytes) {
    const std::string rel = request["path"].asString();
    std::string abs, why;
    std::vector<std::string> names;
    if (!namesOf(rel, &names, &why) || names.empty())
        return failed("read", rel, why.empty() ? "no file named" : why);
    if (!request["hidden"].asBool())
        for (const std::string& n : names)
            if (n[0] == '.') return failed("read", rel, "a hidden file (Show Hidden Files is off)");
    if (!confinedPath(root, rel, &abs, &why)) return failed("read", rel, why);
    // At its real place, with no symlink followed there: what was checked
    // is what is read, or nothing.
    const std::string real = realPlace(abs);
    if (real.empty()) return failed("read", rel, std::strerror(errno));
    const int fd = ::open(real.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return failed("read", rel, std::strerror(errno));
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return failed("read", rel, "not a file");
    }
    const std::uint64_t size = (std::uint64_t)st.st_size;
    const double asked = request["offset"].asNumber(0);
    const std::uint64_t offset = asked > 0 ? std::min<std::uint64_t>((std::uint64_t)asked, size) : 0;
    const double want = request["length"].asNumber(0);
    std::size_t length = want > 0 ? (std::size_t)std::min<double>(want, (double)kMaxReadBytes) : 0;
    length = (std::size_t)std::min<std::uint64_t>(length, size - offset);
    std::string got(length, '\0');
    std::size_t have = 0;
    while (have < length) {
        const ssize_t n = ::pread(fd, &got[have], length - have, (off_t)(offset + have));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            const std::string e = std::strerror(errno);
            ::close(fd);
            return failed("read", rel, e);
        }
        if (n == 0) break;   // shorter than it was a moment ago
        have += (std::size_t)n;
    }
    ::close(fd);
    got.resize(have);
    Json j = Json::object();
    j.set("op", Json("read"));
    j.set("path", Json(rel));
    j.set("size", Json((int64_t)size));
    j.set("offset", Json((int64_t)offset));
    j.set("length", Json((int64_t)have));
    if (bytes) *bytes = std::move(got);
    return j;
}

}  // namespace

bool confinedPath(const std::string& root, const std::string& rel, std::string* abs,
                  std::string* why) {
    std::string w;
    std::vector<std::string> names;
    if (root.empty() || root[0] != '/') w = "no folder to read in";
    else if (namesOf(rel, &names, &w)) {
        const std::string realRoot = realPlace(root);
        const std::string path = rel.empty() ? root : (root.back() == '/' ? root : root + "/") + rel;
        const std::string real = realPlace(path);
        if (realRoot.empty()) w = "the folder is not there";
        else if (real.empty()) w = std::strerror(errno);
        else if (!inside(realRoot, real)) w = "a path that leads out of the folder";
        else {
            if (abs) *abs = path;
            return true;
        }
    }
    if (why) *why = w;
    return false;
}

Json readWorkspace(const std::string& root, const Json& request, std::string* bytes) {
    const std::string op = request["op"].asString();
    const std::string path = request["path"].asString();
    if (bytes) bytes->clear();
    if (op == "list") return listFolder(root, path, request["hidden"].asBool());
    if (op == "read") return readFile(root, request, bytes);
    return failed(op, path, "the host has no read \"" + op + "\"");
}

// ---------------------------------------------------------------- a new path

NewPath newFilePath(const std::string& root, const std::string& rel, bool hidden) {
    NewPath p;
    auto refuse = [&](const char* reason, std::string why) {
        p.reason = reason;
        p.why = std::move(why);
        return p;
    };
    std::vector<std::string> names;
    std::string why;
    if (rel.empty()) return refuse("name", "Name the file to save.");
    if (rel.back() == '/') return refuse("name", "Name the file, not only its folder.");
    if (!namesOf(rel, &names, &why)) return refuse("name", "That name is " + why + ": name a file in the session's folder.");
    for (const std::string& n : names)
        if (n.size() > 255) return refuse("name", "That name is too long.");
    const std::string& name = names.back();
    for (const std::string& n : names)
        if (!hidden && n[0] == '.')
            return refuse("dotfile", n + " starts with a dot, and this page shows no dotfiles: name it otherwise, or "
                                         "turn on Show Hidden Files in the window.");
    if (root.empty()) return refuse("nofolder", "The session has no folder to save in.");
    const std::size_t slash = rel.rfind('/');
    const std::string dirRel = slash == std::string::npos ? std::string() : rel.substr(0, slash);
    std::string dirAbs;
    struct stat st;
    if (!confinedPath(root, dirRel, &dirAbs, &why) || ::stat(dirAbs.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return refuse("nofolder", "There is no folder " + (dirRel.empty() ? std::string("for it") : dirRel) +
                                      " in the session's folder: a page makes no folders.");
    p.abs = dirAbs + "/" + name;
    struct stat lst;
    if (::lstat(p.abs.c_str(), &lst) == 0) {
        // Something is there: a regular file inside the folder, by its
        // real place, or nothing a page may write.
        const std::string real = realPlace(p.abs);
        if (real.empty()) return refuse("notfile", name + " is a link to a file that is not there.");
        if (!inside(realPlace(root), real)) return refuse("outside", name + " leads outside the session's folder.");
        const bool there = ::stat(p.abs.c_str(), &st) == 0;
        if (!there || !S_ISREG(st.st_mode))
            return refuse("notfile", name + (there && S_ISDIR(st.st_mode) ? " is a folder." : " is not a regular file."));
        p.exists = true;
    }
    p.ok = true;
    return p;
}

// ---------------------------------------------------------------- saving

namespace {

std::string fileName(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The largest file read to see whether it changed on disk (as the host's
// backups read one for their base).
constexpr std::uintmax_t kMaxCompared = std::uintmax_t(512) << 20;

}  // namespace

SaveDone saveDocument(const SaveAsk& a) {
    SaveDone d;
    const std::string name = fileName(a.path);
    auto refuse = [&](const char* reason, std::string why) {
        d.reason = reason;
        d.why = std::move(why);
        return d;
    };
    if (a.path.empty() || a.path[0] != '/')
        return refuse("nofile", "This document has no file yet: Save As gives it a name.");
    if (!a.root.empty()) {
        // Its folder inside the session's, and the file too when it is
        // there: a link out of the folder is never written through.
        const std::string realRoot = realPlace(a.root);
        const std::size_t slash = a.path.rfind('/');
        const std::string realDir = realPlace(slash == 0 ? std::string("/") : a.path.substr(0, slash));
        bool in = !realRoot.empty() && !realDir.empty() && inside(realRoot, realDir);
        struct stat st;
        if (in && ::lstat(a.path.c_str(), &st) == 0) {
            const std::string real = realPlace(a.path);
            if (real.empty())
                return refuse("notfile", name + " is a link to a file that is not there: it was not written.");
            in = inside(realRoot, real);
        }
        if (!in)
            return refuse("outside", name + " is outside the session's folder: save it from a mutible window.");
    }
    if (Model::hasLoneSurrogate(a.text))
        return refuse("encoding", name + " could not be saved: the text holds a character UTF-8 cannot store.");

    // Unchanged on disk since the text the host counts saved? By the stamp
    // the host wrote it with, else by its text.
    FolderSearch::TextEncoding enc = a.encoding;
    bool bom = a.bom;
    const FolderSearch::FileStamp now = FolderSearch::stampOf(a.path);
    const bool sameStamp = a.stamp.exists && now == a.stamp;
    // An app's save: it asked its own questions about the disk, and writes
    // in its tab's encoding, so there is nothing to read the file for.
    const bool ownTerms = a.overwrite && a.keepEncoding;
    // Nothing there: a file not made yet (nothing was ever read from it or
    // written to it), or one deleted since, which only `overwrite` makes
    // again. False when refused.
    auto missing = [&]() -> bool {
        if (!a.stamp.exists && a.saved.empty()) {
            d.created = true;
            return true;
        }
        if (!a.overwrite) {
            refuse("gone", name + " was deleted on disk since it was read: saving would make it again.");
            return false;
        }
        d.created = true;
        return true;
    };
    if (!sameStamp && ownTerms) {
        if (!now.exists) d.created = true;
        else if (!now.regular) return refuse("notfile", name + " is not a regular file on disk: it was not written.");
    } else if (!sameStamp) {
        if (!now.exists) {
            if (!missing()) return d;
        } else if (!now.regular) {
            return refuse("notfile", name + " is not a regular file on disk: it was not written.");
        } else {
            std::string bytes;
            FolderSearch::FileStamp st;
            const FolderSearch::ReadStatus r = FolderSearch::readFileStable(a.path, kMaxCompared, &bytes, &st);
            if (r == FolderSearch::ReadStatus::Missing) {
                if (!missing()) return d;
            } else if (r == FolderSearch::ReadStatus::NotRegular) {
                return refuse("notfile", name + " is not a regular file on disk: it was not written.");
            } else if (r != FolderSearch::ReadStatus::Ok) {
                if (!a.overwrite)
                    return refuse("unreadable", name + " could not be read to see whether it changed on disk: "
                                                       "it was not written.");
            } else {
                std::u16string disk;
                FolderSearch::TextEncoding de = FolderSearch::TextEncoding::Utf8;
                bool db = false;
                const bool text = Model::decodeFileText(std::move(bytes), &disk, &de, &db);
                if (text && disk == a.saved) {
                    if (!a.keepEncoding) {
                        enc = de;
                        bom = db;
                    }
                } else if (!a.overwrite) {
                    return refuse("changed", name + " changed on disk since it was read: saving would write "
                                                    "over that change.");
                } else if (text && !a.stamp.exists && !a.keepEncoding) {
                    // Written over: in the encoding the host last wrote it
                    // in when it has one (as Keep Mine writes the tab's),
                    // else the best guess there is, the file's own now.
                    enc = de;
                    bom = db;
                }
            }
        }
    }

    // The file's own encoding, unless the text now holds what it cannot
    // store: then UTF-8, said so, as the Mac's save does (never a '?').
    const std::string utf8 = Model::utf8FromUtf16(a.text);
    std::string unheld;
    if (!FolderSearch::canEncode(utf8, enc, &unheld)) {
        d.note = "Saved " + name + " as UTF-8: " + FolderSearch::encodingLabel(enc, bom) + " cannot store “" +
                 unheld + "”";
        enc = FolderSearch::TextEncoding::Utf8;
        bom = false;
    }
    std::string error;
    if (!FolderSearch::writeFileAtomically(a.path, FolderSearch::encodeFileText(utf8, enc, bom), &error,
                                           &d.written)) {
        d.created = false;
        d.note.clear();
        return refuse("write", name + " could not be saved: " + error + ".");
    }
    d.ok = true;
    d.encoding = enc;
    d.bom = bom;
    return d;
}

Json saveDoneJson(const std::string& path, const SaveDone& done) {
    Json j = Json::object();
    j.set("path", Json(path));
    j.set("ok", Json(done.ok));
    if (!done.reason.empty()) j.set("reason", Json(done.reason));
    if (!done.why.empty()) j.set("why", Json(done.why));
    if (!done.note.empty()) j.set("note", Json(done.note));
    if (done.ok) {
        j.set("encoding", Json(std::string(FolderSearch::encodingLabel(done.encoding, done.bom))));
        j.set("encodingName", Json(std::string(FolderSearch::encodingName(done.encoding, done.bom))));
        j.set("stamp", stampJson(done.written));
    }
    if (done.created) j.set("created", Json(true));
    return j;
}

Json stampJson(const FolderSearch::FileStamp& s) {
    Json j = Json::object();
    j.set("exists", Json(s.exists));
    j.set("regular", Json(s.regular));
    j.set("sec", Json((std::int64_t)s.mtimeSec));
    j.set("nsec", Json((std::int64_t)s.mtimeNsec));
    j.set("dev", Json(std::to_string(s.device)));
    j.set("ino", Json(std::to_string(s.inode)));
    j.set("size", Json(std::to_string(s.size)));
    return j;
}

FolderSearch::FileStamp stampFromJson(const Json& j) {
    FolderSearch::FileStamp s;
    if (!j.isObject() || !j["exists"].asBool()) return s;
    auto number = [&](const char* key) -> std::uint64_t {
        const std::string& t = j[key].asString();
        if (t.empty() || t.find_first_not_of("0123456789") != std::string::npos) return 0;
        return std::strtoull(t.c_str(), nullptr, 10);
    };
    s.exists = true;
    s.regular = j["regular"].asBool();
    s.mtimeSec = j["sec"].asInt();
    s.mtimeNsec = (long)j["nsec"].asInt();
    s.device = number("dev");
    s.inode = number("ino");
    s.size = number("size");
    return s;
}

// ---------------------------------------------------------------- the disk

DiskAction diskAction(const DiskLook& l) {
    if (l.appHasIt) return DiskAction::Nothing;   // the app follows it, and says
    if (!l.exists) {
        // A path the document never had a file at (a new file not made
        // yet) has nothing to lose; one already marked gone is told once.
        if (!l.hadFile || l.wasGone) return DiskAction::Nothing;
        return DiskAction::Gone;
    }
    // A folder, a FIFO, bytes that are not text or too many of them: never
    // followed, whatever the document holds.
    if (!l.text) return DiskAction::Changed;
    if (l.sameAsSaved) return DiskAction::Same;
    return l.edited ? DiskAction::Changed : DiskAction::Follow;
}

DiskText readDiskText(const std::string& path) {
    DiskText d;
    std::string bytes;
    const FolderSearch::ReadStatus r = FolderSearch::readFileStable(path, kMaxFollowed, &bytes, &d.stamp);
    if (r == FolderSearch::ReadStatus::Missing) return d;   // (a link to nothing too)
    d.exists = true;
    if (r == FolderSearch::ReadStatus::NotRegular) return d;
    d.regular = true;
    if (r != FolderSearch::ReadStatus::Ok) return d;   // too large, or unreadable
    d.text = Model::decodeFileText(std::move(bytes), &d.body, &d.encoding, &d.bom);
    if (!d.text) d.body.clear();
    return d;
}

}  // namespace Shared
