// SessionFiles.cpp — see SessionFiles.h.
#include "SessionFiles.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>

#include "FileDetails.h"
#include "FolderSearch.h"

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

Json readWorkspace(const std::string& root, const Json& request) {
    const std::string op = request["op"].asString();
    const std::string path = request["path"].asString();
    if (op == "list") return listFolder(root, path, request["hidden"].asBool());
    return failed(op, path, "the host has no read \"" + op + "\"");
}

}  // namespace Shared
