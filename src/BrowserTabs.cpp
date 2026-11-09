// BrowserTabs.cpp — see BrowserTabs.h.
#include "BrowserTabs.h"

#include <algorithm>
#include <numeric>

namespace BrowserTabs {

namespace {

// The tabs, `active` first (when it is one), then the rest from the most
// recently used; ties go to the tab further left.
std::vector<int> byRecency(const std::vector<uint64_t> &lastUsed, int active) {
    std::vector<int> order(lastUsed.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if ((a == active) != (b == active)) return a == active;
        return lastUsed[(size_t)a] > lastUsed[(size_t)b];
    });
    return order;
}

// The number of bytes the UTF-8 sequence starting with `c` takes (1 for a
// stray continuation byte, so a bad string still advances).
size_t utf8Length(unsigned char c) {
    if (c >= 0xF0) return 4;
    if (c >= 0xE0) return 3;
    if (c >= 0xC0) return 2;
    return 1;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

std::string foldSpaces(const std::string &s) {
    std::string out;
    bool pending = false;
    for (char c : s) {
        if (isSpace(c)) {
            pending = !out.empty();
            continue;
        }
        if (pending) out += ' ';
        pending = false;
        out += c;
    }
    return out;
}

std::string shortAddress(const std::string &url) {
    std::string u = foldSpaces(url);
    if (u.empty() || u == "about:blank") return "";
    const size_t scheme = u.find("://");
    if (u.compare(0, 7, "file://") == 0) {
        std::string path = u.substr(7);
        const size_t cut = path.find_first_of("?#");
        if (cut != std::string::npos) path.resize(cut);
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        const size_t slash = path.rfind('/');
        std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        return name.empty() ? path : name;
    }
    if (scheme != std::string::npos) u = u.substr(scheme + 3);
    if (u.compare(0, 4, "www.") == 0) u = u.substr(4);
    if (u.size() > 1 && u.back() == '/') u.pop_back();
    return u;
}

}  // namespace

std::vector<bool> livePages(const std::vector<uint64_t> &lastUsed, int active, int maxLive) {
    std::vector<bool> live(lastUsed.size(), false);
    int left = std::max(0, maxLive);
    for (int i : byRecency(lastUsed, active)) {
        if (left == 0) break;
        live[(size_t)i] = true;
        left--;
    }
    return live;
}

std::string tabTitle(const std::string &title, const std::string &url, size_t maxChars) {
    std::string s = foldSpaces(title);
    if (s.empty()) s = shortAddress(url);
    if (s.empty()) s = "New Tab";
    if (maxChars == 0) return s;
    size_t i = 0, chars = 0;
    while (i < s.size() && chars < maxChars) {
        i += utf8Length((unsigned char)s[i]);
        chars++;
    }
    if (i >= s.size()) return s;
    // Cut one character earlier, to make room for the ellipsis.
    size_t keep = 0, n = 0;
    while (keep < s.size() && n + 1 < maxChars) {
        keep += utf8Length((unsigned char)s[keep]);
        n++;
    }
    std::string out = s.substr(0, std::min(keep, s.size()));
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out + "\xE2\x80\xA6";
}

std::vector<bool> keptRecords(const std::vector<size_t> &sizes,
                              const std::vector<uint64_t> &lastUsed, int active, size_t perTab,
                              size_t total) {
    std::vector<bool> kept(sizes.size(), false);
    std::vector<uint64_t> stamps = lastUsed;
    stamps.resize(sizes.size(), 0);
    size_t used = 0;
    for (int i : byRecency(stamps, active)) {
        const size_t n = sizes[(size_t)i];
        if (n == 0 || n > perTab || n > total - used) continue;
        kept[(size_t)i] = true;
        used += n;
    }
    return kept;
}

int restoredActive(int saved, int count) {
    if (count <= 0) return -1;
    return saved >= 0 && saved < count ? saved : count - 1;
}

}  // namespace BrowserTabs
