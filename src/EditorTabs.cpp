// EditorTabs.cpp — see EditorTabs.h.
#include "EditorTabs.h"

#include <set>

namespace EditorTabs {

int insertionIndex(int active, int count) {
    if (count <= 0) return 0;
    if (active < 0 || active >= count) return count;
    return active + 1;
}

int indexForNumber(int n, int count) {
    if (count <= 0 || n < 1 || n > 9) return -1;
    if (n == 9) return count - 1;
    return n <= count ? n - 1 : -1;
}

int step(int active, int count, int delta) {
    if (count <= 0) return -1;
    if (active < 0 || active >= count) return delta < 0 ? count - 1 : 0;
    int i = (active + delta) % count;
    return i < 0 ? i + count : i;
}

int activeAfterClose(const std::vector<uint64_t> &lastUsed, int active, int closed) {
    const int count = (int)lastUsed.size();
    if (closed < 0 || closed >= count || count <= 1) return -1;
    if (active != closed) {
        if (active < 0 || active >= count) return -1;
        return active > closed ? active - 1 : active;
    }
    int best = -1;
    for (int i = 0; i < count; i++) {
        if (i == closed) continue;
        if (best < 0 || lastUsed[(size_t)i] > lastUsed[(size_t)best]) best = i;
    }
    return best > closed ? best - 1 : best;
}

Session restorable(const Session &saved,
                   const std::function<bool(const std::string &)> &exists) {
    Session out;
    std::set<std::string> seen;
    // Where the saved active file lands, or the nearest kept file before it.
    int activeAt = -1, before = -1;
    for (int i = 0; i < (int)saved.files.size(); i++) {
        const std::string &f = saved.files[(size_t)i];
        const bool keep = !f.empty() && !seen.count(f) && exists(f);
        if (keep) {
            seen.insert(f);
            out.files.push_back(f);
        }
        if (i == saved.active) {
            if (keep) activeAt = (int)out.files.size() - 1;
            // A repeat of a file already kept: that tab.
            else if (!f.empty() && seen.count(f))
                for (int k = 0; k < (int)out.files.size(); k++)
                    if (out.files[(size_t)k] == f) activeAt = k;
            before = (int)out.files.size() - 1;
        }
    }
    if (out.files.empty()) return out;
    if (activeAt >= 0) out.active = activeAt;
    else if (before >= 0) out.active = before;
    else out.active = 0;
    return out;
}

// The last `depth` components of a path ("b/c.txt" for depth 2 of "/a/b/c.txt").
static std::string tail(const std::string &path, int depth) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < path.size()) {
        size_t slash = path.find('/', i);
        if (slash == std::string::npos) slash = path.size();
        if (slash > i) parts.push_back(path.substr(i, slash - i));
        i = slash + 1;
    }
    std::string out;
    const size_t first = parts.size() > (size_t)depth ? parts.size() - (size_t)depth : 0;
    for (size_t k = first; k < parts.size(); k++) {
        if (!out.empty()) out += '/';
        out += parts[k];
    }
    return out;
}

std::vector<std::string> labels(const std::vector<std::string> &paths) {
    const size_t n = paths.size();
    std::vector<std::string> out(n);
    std::vector<int> depth(n, 1);
    for (size_t i = 0; i < n; i++)
        if (!paths[i].empty()) out[i] = tail(paths[i], 1);
    // Lengthen the labels that clash until they differ, or their paths run
    // out (the same file twice).
    for (int round = 0; round < 64; round++) {
        bool changed = false;
        std::vector<bool> clash(n, false);   // judged before any label grows
        for (size_t i = 0; i < n; i++)
            for (size_t j = 0; j < n && !clash[i]; j++)
                clash[i] = j != i && !paths[i].empty() && !paths[j].empty() &&
                           out[j] == out[i] && paths[j] != paths[i];
        for (size_t i = 0; i < n; i++) {
            if (!clash[i]) continue;
            std::string longer = tail(paths[i], depth[i] + 1);
            if (longer != out[i]) {
                depth[i]++;
                out[i] = longer;
                changed = true;
            }
        }
        if (!changed) break;
    }
    return out;
}

}  // namespace EditorTabs
