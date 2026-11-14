// DockMenu.cpp — see DockMenu.h.
#include "DockMenu.h"

#include "RecentList.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace DockMenu {

namespace {

const char* kFileName = "org.minicode.Editor.desktop";

struct Group {
    std::string header;                // "[Desktop Entry]"
    std::vector<std::string> lines;    // verbatim, no trailing blank lines
};

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(i, e - i);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
        i = e + 1;
    }
    return out;
}

std::vector<Group> groupsOf(const std::string& text) {
    std::vector<Group> out;
    for (const std::string& l : splitLines(text)) {
        if (!l.empty() && l[0] == '[') {
            out.push_back({l, {}});
            continue;
        }
        if (!out.empty()) out.back().lines.push_back(l);
    }
    for (Group& g : out)
        while (!g.lines.empty() && g.lines.back().empty()) g.lines.pop_back();
    return out;
}

bool startsWith(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

// The first word of an Exec value, as written (quoted or not).
std::string firstWord(const std::string& exec) {
    if (exec.empty()) return "";
    if (exec[0] == '"') {
        for (size_t i = 1; i < exec.size(); i++) {
            if (exec[i] == '\\') i++;
            else if (exec[i] == '"') return exec.substr(0, i + 1);
        }
        return exec;
    }
    return exec.substr(0, exec.find_first_of(" \t"));
}

bool hasControl(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return true;
    return false;
}

std::string valueEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

std::string readFile(const std::string& path, bool* ok = nullptr) {
    gchar* text = nullptr;
    gsize len = 0;
    const bool got = g_file_get_contents(path.c_str(), &text, &len, nullptr);
    if (ok) *ok = got;
    std::string out = got ? std::string(text, len) : std::string();
    g_free(text);
    return out;
}

bool isOurs(const std::string& text) {
    for (const std::string& l : splitLines(text))
        if (l == kMarker) return true;
    return false;
}

std::string dataHome() {
    const char* s = getenv("XDG_DATA_HOME");
    return (s && *s) ? s : std::string(g_get_home_dir()) + "/.local/share";
}

}  // namespace

std::string execArg(const std::string& arg) {
    std::string q = "\"";
    for (char c : arg) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') q += '\\';
        q += c;
        if (c == '%') q += '%';
    }
    q += '"';
    return valueEscape(q);
}

std::string render(const std::string& installed, const std::vector<std::string>& folders,
                   const std::string& home) {
    std::vector<Group> groups = groupsOf(installed);
    const Group* entry = nullptr;
    for (const Group& g : groups)
        if (g.header == "[Desktop Entry]") { entry = &g; break; }
    if (!entry) return "";
    std::string binary;
    for (const std::string& l : entry->lines)
        if (startsWith(l, "Exec=")) { binary = firstWord(l.substr(5)); break; }
    if (binary.empty()) return "";

    // The installed file's own actions stay, after ours.
    std::set<std::string> ours = {"open-folder", "new-window"};
    std::vector<std::string> recent;
    std::vector<std::string> recentGroups;
    for (const std::string& f : folders) {
        if (recent.size() >= kMaxRecent) break;
        if (f.empty() || hasControl(f)) continue;
        const std::string id = "recent-" + std::to_string(recent.size() + 1);
        ours.insert(id);
        recent.push_back(id);
        recentGroups.push_back("[Desktop Action " + id + "]\nName=" +
                               valueEscape(RecentList::tilded(f, home)) + "\nExec=" + binary +
                               " " + execArg(f) + "\n");
    }
    std::vector<std::string> kept;
    std::string keptText;
    for (const Group& g : groups) {
        if (!startsWith(g.header, "[Desktop Action ")) continue;
        const std::string id = g.header.substr(16, g.header.size() - 17);
        if (ours.count(id)) continue;   // an installed New Window is replaced by ours
        kept.push_back(id);
        keptText += g.header + "\n";
        for (const std::string& l : g.lines) keptText += l + "\n";
        keptText += "\n";
    }

    std::string out = entry->header + "\n";
    for (const std::string& l : entry->lines) {
        if (startsWith(l, "Actions=") || l == kMarker) continue;
        out += l + "\n";
    }
    std::string actions = "Actions=open-folder;new-window;";
    for (const std::string& r : recent) actions += r + ";";
    for (const std::string& k : kept) actions += k + ";";
    out += actions + "\n" + kMarker + "\n\n";
    out += "[Desktop Action open-folder]\nName=Open Folder…\nExec=" + binary +
           " --open-folder\n\n";
    out += "[Desktop Action new-window]\nName=New Window\nExec=" + binary +
           " --new-window\n\n";
    for (const std::string& r : recentGroups) out += r + "\n";
    out += keptText;
    while (out.size() > 1 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n')
        out.pop_back();
    return out;
}

std::string installedFile() {
    const char* s = getenv("XDG_DATA_DIRS");
    const std::string dirs = (s && *s) ? s : "/usr/local/share:/usr/share";
    const std::string mine = generatedFile();
    size_t i = 0;
    while (i <= dirs.size()) {
        size_t e = dirs.find(':', i);
        if (e == std::string::npos) e = dirs.size();
        const std::string d = dirs.substr(i, e - i);
        i = e + 1;
        if (d.empty()) continue;
        const std::string path = d + "/applications/" + kFileName;
        bool ok = false;
        const std::string text = readFile(path, &ok);
        if (ok && path != mine && !isOurs(text)) return path;
    }
    return "";
}

std::string generatedFile() { return dataHome() + "/applications/" + kFileName; }

void update(bool enabled) {
    const std::string target = generatedFile();
    bool exists = false;
    const std::string old = readFile(target, &exists);
    if (exists && !isOurs(old)) {
        static std::string told;
        if (told != target) {
            told = target;
            fprintf(stderr, "minicode: %s is not minicode's own copy (no %s), so the dock "
                            "menu's recent folders are left out\n", target.c_str(), kMarker);
        }
        return;
    }
    if (!enabled) {
        if (exists) g_unlink(target.c_str());
        return;
    }
    const std::string installed = installedFile();
    if (installed.empty()) return;
    const std::string text =
        render(readFile(installed), RecentList::folders(), g_get_home_dir());
    if (text.empty() || (exists && text == old)) return;
    gchar* dir = g_path_get_dirname(target.c_str());
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    GError* err = nullptr;
    if (!g_file_set_contents_full(target.c_str(), text.data(), (gssize)text.size(),
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0644, &err)) {
        fprintf(stderr, "minicode: could not write %s: %s\n", target.c_str(),
                err ? err->message : "unknown error");
        g_clear_error(&err);
    }
}

}  // namespace DockMenu
