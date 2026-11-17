// FolderPicker.cpp — see FolderPicker.h.
#include "FolderPicker.h"

#include "CommandPalette.h"
#include "LspClient.h"   // Lsp::toUtf16

#include <algorithm>
#include <set>

#include <dirent.h>
#include <sys/stat.h>

namespace FolderPicker {
namespace {

bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r'))
        b--;
    return s.substr(a, b - a);
}

std::string lowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool isPathText(const std::string& t) {
    return t.find('/') != std::string::npos || t == "~" || startsWith(t, "~/");
}

// "~" and "~/x" with the home folder; relative paths from base.
std::string expand(const std::string& t, const std::string& home, const std::string& base) {
    if (t == "~") return home;
    if (startsWith(t, "~/")) return join(home, t.substr(2));
    if (!t.empty() && t[0] == '/') return t;
    return join(base, t);
}

Row folderRow(const std::string& path, const std::string& home, bool recent) {
    Row r;
    r.kind = RowKind::Folder;
    r.path = path;
    r.recent = recent;
    const std::u16string label = Lsp::toUtf16(abbreviate(path, home));
    const std::u16string name = Lsp::toUtf16(lastComponent(abbreviate(path, home)));
    r.label = label;
    r.nameAt = label.size() >= name.size() ? label.size() - name.size() : 0;
    return r;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

std::string normalize(const std::string& path) {
    const bool absolute = !path.empty() && path[0] == '/';
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string part = path.substr(i, j - i);
        if (part.empty() || part == ".") {
            // nothing
        } else if (part == "..") {
            if (!parts.empty() && parts.back() != "..") parts.pop_back();
            else if (!absolute) parts.push_back(part);
        } else {
            parts.push_back(part);
        }
        i = j + 1;
    }
    std::string out = absolute ? "/" : "";
    for (size_t k = 0; k < parts.size(); k++) {
        if (k) out += '/';
        out += parts[k];
    }
    if (out.empty()) out = ".";
    return out;
}

std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (name.empty()) return dir;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string abbreviate(const std::string& path, const std::string& home) {
    std::string h = home;
    while (h.size() > 1 && h.back() == '/') h.pop_back();
    if (h.empty() || h == "/") return path;
    if (path == h) return "~";
    if (startsWith(path, h + "/")) return "~" + path.substr(h.size());
    return path;
}

std::string lastComponent(const std::string& path) {
    if (path == "/" || path.empty()) return path;
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    const size_t slash = p.rfind('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

Input parse(const std::string& text, const std::string& home, const std::string& base) {
    Input in;
    // Leading blanks never belong to a path; trailing ones may (rarely), so
    // a path keeps them.
    size_t lead = 0;
    while (lead < text.size() && (text[lead] == ' ' || text[lead] == '\t')) lead++;
    const std::string t = text.substr(lead);
    in.isPath = isPathText(t);
    if (!in.isPath) {
        in.dir = normalize(base);
        in.partial = trimmed(t);
        in.showHidden = !in.partial.empty() && in.partial[0] == '.';
        if (!in.partial.empty() && in.partial != "." && in.partial != "..")
            in.typed = normalize(join(in.dir, in.partial));
        return in;
    }
    const std::string spelled = t == "~" ? "~/" : t;
    const size_t slash = spelled.rfind('/');
    const std::string dirPart = spelled.substr(0, slash + 1);
    in.partial = spelled.substr(slash + 1);
    in.dir = normalize(expand(dirPart, home, base));
    in.showHidden = !in.partial.empty() && in.partial[0] == '.';
    in.typed = in.partial.empty() ? in.dir : normalize(join(in.dir, in.partial));
    return in;
}

std::string complete(const std::string& text, const std::string& path, const std::string& home) {
    size_t lead = 0;
    while (lead < text.size() && (text[lead] == ' ' || text[lead] == '\t')) lead++;
    const std::string t = text.substr(lead);
    if (isPathText(t)) {
        const std::string spelled = t == "~" ? "~/" : t;
        return spelled.substr(0, spelled.rfind('/') + 1) + lastComponent(path) + "/";
    }
    const std::string a = abbreviate(path, home);
    return a == "/" ? a : a + "/";
}

bool slashCompletes(const std::string& text) {
    if (text.empty() || text.back() == '/' || text == "~") return false;
    const std::string last = lastComponent(text);
    return !(last == "." || last == ".." || last == "~");
}

std::vector<std::string> listSubfolders(const std::string& dir, const std::atomic<bool>* cancel,
                                        bool* ok, size_t maxEntries) {
    std::vector<std::string> out;
    if (ok) *ok = false;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    if (ok) *ok = true;
    size_t seen = 0;
    while (struct dirent* e = readdir(d)) {
        if ((++seen & 255) == 0 && cancel && cancel->load(std::memory_order_relaxed)) break;
        const char* n = e->d_name;
        if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0))) continue;
        bool isDir = e->d_type == DT_DIR;
        if (e->d_type == DT_LNK || e->d_type == DT_UNKNOWN) {
            struct stat st;
            isDir = stat(join(dir, n).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (!isDir) continue;
        out.emplace_back(n);
        if (out.size() >= maxEntries) break;
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        const std::string la = lowerAscii(a), lb = lowerAscii(b);
        return la != lb ? la < lb : a < b;
    });
    return out;
}

std::vector<Row> rows(const Input& in, const std::vector<std::string>& recent,
                      const Listing& listing, const std::string& home, size_t limit) {
    std::vector<Row> folders;
    const std::u16string query = Lsp::toUtf16(in.partial);
    bool typedExists = false;
    if (in.isPath) {
        // The names alone are matched: the folder before them is the same
        // for every row and would only add matches of its own letters.
        std::vector<std::string> names;
        std::vector<std::u16string> keys;
        if (listing.done && listing.ok)
            for (const std::string& n : listing.names) {
                if (n[0] == '.' && !in.showHidden) continue;
                names.push_back(n);
                keys.push_back(Lsp::toUtf16(n));
            }
        for (const CommandPalette::Match& m : CommandPalette::rankFiles(keys, query, {}, limit)) {
            Row r = folderRow(join(in.dir, names[m.index]), home, false);
            for (size_t p : m.positions) r.positions.push_back(r.nameAt + p);
            folders.push_back(std::move(r));
        }
        typedExists = listing.done && listing.ok &&
                      (in.partial.empty() || contains(listing.names, in.partial));
    } else {
        std::vector<std::string> paths;
        std::vector<bool> isRecent;
        std::set<std::string> seen;
        for (const std::string& p : recent) {
            const std::string n = normalize(p);
            if (!seen.insert(n).second) continue;
            paths.push_back(p);
            isRecent.push_back(true);
        }
        if (listing.done && listing.ok)
            for (const std::string& n : listing.names) {
                if (n[0] == '.' && !in.showHidden) continue;
                const std::string p = join(in.dir, n);
                if (!seen.insert(normalize(p)).second) continue;
                paths.push_back(p);
                isRecent.push_back(false);
            }
        std::vector<std::u16string> keys;
        for (const std::string& p : paths) keys.push_back(Lsp::toUtf16(abbreviate(p, home)));
        for (const CommandPalette::Match& m : CommandPalette::rankFiles(keys, query, {}, limit)) {
            Row r = folderRow(paths[m.index], home, isRecent[m.index]);
            r.positions = m.positions;
            folders.push_back(std::move(r));
        }
        typedExists = !in.typed.empty() && listing.done && listing.ok &&
                      contains(listing.names, in.partial);
    }
    std::vector<Row> out;
    Row open;
    if (typedExists) {
        open = folderRow(in.typed, home, false);
        open.kind = RowKind::OpenTyped;
    }
    const bool openFirst = typedExists && in.isPath && in.partial.empty();
    if (openFirst) out.push_back(open);
    for (Row& r : folders) out.push_back(std::move(r));
    if (typedExists && !openFirst) out.push_back(open);
    Row finder;
    finder.kind = RowKind::ChooseInFinder;
    finder.label = u"Choose in Finder…";
    out.push_back(finder);
    return out;
}

std::string emptyText(const Input& in, const Listing& listing, const std::string& home) {
    if (!listing.done) return "Listing " + abbreviate(in.dir, home) + "…";
    if (!listing.ok) return "Cannot read " + abbreviate(in.dir, home);
    return in.isPath && in.partial.empty() ? "No folders in " + abbreviate(in.dir, home)
                                           : "No matching folders";
}

}  // namespace FolderPicker
