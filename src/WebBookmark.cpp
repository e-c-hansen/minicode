// WebBookmark.cpp — see WebBookmark.h.
#include "WebBookmark.h"

#include "BrowserTabs.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace WebBookmark {

namespace {

std::string lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool endsWith(const std::string &s, const std::string &tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    return s.substr(a, b - a);
}

size_t utf8Length(unsigned char c) {
    if (c >= 0xF0) return 4;
    if (c >= 0xE0) return 3;
    if (c >= 0xC0) return 2;
    return 1;
}

// The [InternetShortcut] section's URL value, read line by line.
std::string shortcutAddress(const std::string &bytes) {
    std::string text = bytes;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    bool inSection = false;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            inSection = lower(line) == "[internetshortcut]";
            continue;
        }
        if (!inSection) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (lower(trim(line.substr(0, eq))) == "url") return trim(line.substr(eq + 1));
    }
    return "";
}

// XML's five entities and numeric references (to UTF-8).
std::string decodeXml(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        const std::string name = s.substr(i + 1, semi - i - 1);
        unsigned long cp = 0;
        bool numeric = false;
        if (name == "amp") out += '&';
        else if (name == "lt") out += '<';
        else if (name == "gt") out += '>';
        else if (name == "quot") out += '"';
        else if (name == "apos") out += '\'';
        else if (name.size() > 1 && name[0] == '#') {
            const bool hex = name[1] == 'x' || name[1] == 'X';
            const std::string digits = name.substr(hex ? 2 : 1);
            if (digits.empty() ||
                !std::all_of(digits.begin(), digits.end(), [hex](char c) {
                    return hex ? std::isxdigit((unsigned char)c) : std::isdigit((unsigned char)c);
                })) {
                out += s[i];
                continue;
            }
            cp = std::stoul(digits, nullptr, hex ? 16 : 10);
            numeric = true;
        } else {
            out += s[i];
            continue;
        }
        if (numeric) {
            if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                out += s[i];
                continue;
            }
            if (cp < 0x80) {
                out += (char)cp;
            } else if (cp < 0x800) {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            } else {
                out += (char)(0xF0 | (cp >> 18));
                out += (char)(0x80 | ((cp >> 12) & 0x3F));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
        }
        i = semi;
    }
    return out;
}

// The string after <key>URL</key> in an XML property list.
std::string weblocAddress(const std::string &bytes) {
    if (bytes.compare(0, 6, "bplist") == 0) return "";
    const size_t key = bytes.find("<key>URL</key>");
    if (key == std::string::npos) return "";
    size_t at = key + 14;
    while (at < bytes.size() && std::isspace((unsigned char)bytes[at])) at++;
    if (bytes.compare(at, 8, "<string>") != 0) return "";
    at += 8;
    const size_t end = bytes.find("</string>", at);
    if (end == std::string::npos) return "";
    return trim(decodeXml(bytes.substr(at, end - at)));
}

}  // namespace

bool isBookmarkPath(const std::string &path) {
    const size_t slash = path.rfind('/');
    const std::string name = lower(slash == std::string::npos ? path : path.substr(slash + 1));
    // A name that is only the extension (".url") is a dotfile, not a bookmark.
    return (endsWith(name, ".url") && name.size() > 4) ||
           (endsWith(name, ".webloc") && name.size() > 7);
}

bool isWebAddress(const std::string &url) {
    if (url.size() > 8192) return false;
    const std::string l = lower(url.substr(0, 8));
    size_t host;
    if (l.compare(0, 7, "http://") == 0) host = 7;
    else if (l.compare(0, 8, "https://") == 0) host = 8;
    else return false;
    if (host >= url.size() || url[host] == '/' || url[host] == '?' || url[host] == '#')
        return false;
    for (unsigned char c : url)
        if (c <= 0x20 || c == 0x7F) return false;
    return true;
}

std::string addressIn(const std::string &path, const std::string &bytes) {
    const std::string l = lower(path);
    std::string url;
    if (endsWith(l, ".url")) url = shortcutAddress(bytes);
    else if (endsWith(l, ".webloc")) url = weblocAddress(bytes);
    return isWebAddress(url) ? url : "";
}

std::string shortcutText(const std::string &url) {
    return "[InternetShortcut]\r\nURL=" + url + "\r\n";
}

std::string fileName(const std::string &title, const std::string &url, size_t maxBytes) {
    // The tab's own title (or short address), uncut.
    std::string base = BrowserTabs::tabTitle(title, url, 0);
    if (base == "New Tab" && title.empty()) base.clear();
    std::string clean;
    for (unsigned char c : base) {
        if (c < 0x20 || c == 0x7F) continue;
        clean += (c == '/' || c == ':') ? '-' : (char)c;
    }
    size_t a = 0;
    while (a < clean.size() && (clean[a] == '.' || clean[a] == ' ')) a++;
    clean.erase(0, a);
    // Cut at a character boundary.
    if (clean.size() > maxBytes) {
        size_t cut = 0;
        while (cut < clean.size()) {
            const size_t n = utf8Length((unsigned char)clean[cut]);
            if (cut + n > maxBytes) break;
            cut += n;
        }
        clean.resize(cut);
    }
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) clean.pop_back();
    if (clean.empty()) clean = "Bookmark";
    return clean + ".url";
}

std::string uniqueName(const std::string &name,
                       const std::function<bool(const std::string &)> &taken) {
    if (!taken(name)) return name;
    const size_t dot = name.rfind('.');
    const bool hasExt = dot != std::string::npos && dot > 0;
    const std::string stem = hasExt ? name.substr(0, dot) : name;
    const std::string ext = hasExt ? name.substr(dot) : "";
    std::string candidate;
    for (int n = 2; n < 100000; n++) {
        candidate = stem + " " + std::to_string(n) + ext;
        if (!taken(candidate)) break;
    }
    return candidate;   // the writer's exclusive create refuses a name still taken
}

std::string tabsFolderName(const std::string &when) { return "Tabs " + when; }

std::vector<std::string> tabAddresses(const std::vector<std::string> &urls) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const std::string &u : urls)
        if (isWebAddress(u) && seen.insert(u).second) out.push_back(u);
    return out;
}

}  // namespace WebBookmark
