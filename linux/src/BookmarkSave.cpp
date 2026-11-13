// BookmarkSave.cpp — see BookmarkSave.h.
#include "BookmarkSave.h"

#include "WebBookmark.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <set>

namespace BookmarkSave {
namespace {

std::string home() {
    const char* h = std::getenv("HOME");
    return h && *h ? h : "/";
}

bool exists(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0;
}

}  // namespace

std::string folderFor(const std::string& setting) {
    if (!setting.empty()) {
        std::string p = setting;
        if (p == "~" || p.compare(0, 2, "~/") == 0) p = home() + p.substr(1);
        else if (p[0] != '/') p = home() + "/" + p;
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        return p;
    }
    return home() + "/Bookmarks";
}

bool ensureFolder(const std::string& folder, std::string* error) {
    struct stat st;
    if (stat(folder.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) return true;
        if (error) *error = folder + " is a file, not a folder";
        return false;
    }
    // Each missing level, as mkdir -p.
    for (size_t i = 1; i <= folder.size(); i++) {
        if (i < folder.size() && folder[i] != '/') continue;
        const std::string part = folder.substr(0, i);
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) {
            if (error) *error = std::strerror(errno);
            return false;
        }
    }
    return true;
}

std::string write(const std::string& folder, const std::string& title, const std::string& url,
                  std::string* error) {
    if (!WebBookmark::isWebAddress(url)) {
        if (error) *error = "only http and https pages can be bookmarked";
        return "";
    }
    const std::string text = WebBookmark::shortcutText(url);
    std::string name = WebBookmark::fileName(title, url);
    auto taken = [&folder](const std::string& n) { return exists(folder + "/" + n); };
    // A name free now may be taken before the create: O_EXCL refuses it and
    // the next free name is tried.
    for (int attempt = 0; attempt < 5; attempt++) {
        name = WebBookmark::uniqueName(name, taken);
        const std::string path = folder + "/" + name;
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

std::string fileFor(const std::string& folder, const std::string& url) {
    if (folder.empty() || !WebBookmark::isWebAddress(url)) return "";
    size_t seen = 0;
    std::set<std::string> visited;
    return search(folder, url, &seen, 0, &visited);
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

}  // namespace BookmarkSave
