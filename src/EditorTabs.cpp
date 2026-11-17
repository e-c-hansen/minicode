// EditorTabs.cpp — see EditorTabs.h.
#include "EditorTabs.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
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
    int previewAt = -1;
    for (int i = 0; i < (int)saved.files.size(); i++) {
        const std::string &f = saved.files[(size_t)i];
        const bool keep = !f.empty() && !seen.count(f) && exists(f);
        if (keep) {
            seen.insert(f);
            out.files.push_back(f);
            if (i == saved.preview) previewAt = (int)out.files.size() - 1;
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
    out.preview = previewAt;
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

int dropIndex(const std::vector<double> &widths, int from, double x) {
    const int n = (int)widths.size();
    if (from < 0 || from >= n) return from;
    int before = 0;
    double left = 0;
    for (int i = 0; i < n; i++) {
        if (i != from && left + widths[(size_t)i] / 2 < x) before++;
        left += widths[(size_t)i];
    }
    return before;
}

void ClosedTabs::remember(const ClosedTab &t) {
    if (t.path.empty() || limit_ == 0) return;
    list_.push_back(t);
    if (list_.size() > limit_)
        list_.erase(list_.begin(), list_.begin() + (std::ptrdiff_t)(list_.size() - limit_));
}

bool ClosedTabs::take(ClosedTab *out,
                      const std::function<bool(const std::string &)> &reopenable) {
    while (!list_.empty()) {
        ClosedTab t = list_.back();
        list_.pop_back();
        if (!reopenable || reopenable(t.path)) {
            if (out) *out = t;
            return true;
        }
    }
    return false;
}

int ClosedTabs::indexIn(const ClosedTab &t, int count) {
    if (count <= 0) return 0;
    if (t.index < 0) return 0;
    return t.index > count ? count : t.index;
}

bool opensInPreview(OpenFrom from, const PreviewSettings &s) {
    if (!s.enabled) return false;
    switch (from) {
    case OpenFrom::TreeClick:
    case OpenFrom::CodeNavigation:
    case OpenFrom::SearchClick:
        return true;
    case OpenFrom::QuickOpen:
        return s.fromQuickOpen;
    case OpenFrom::Other:
    case OpenFrom::TreeDoubleClick:
    case OpenFrom::TreeCommandClick:
    case OpenFrom::TreeReturn:
    case OpenFrom::SearchOpen:
        return false;
    }
    return false;
}

OpenFrom treeClick(int clickCount, bool sameRowAsLastPress, bool command) {
    if (command) return OpenFrom::TreeCommandClick;
    if (clickCount >= 2 && sameRowAsLastPress) return OpenFrom::TreeDoubleClick;
    return OpenFrom::TreeClick;
}

bool staysPreview(bool isPreview, bool openInPreview) { return isPreview && openInPreview; }

PreviewPlace placePreview(int active, int preview, int count) {
    PreviewPlace p;
    if (count < 0) count = 0;
    if (active >= count) active = -1;
    int target = insertionIndex(active, count);
    if (preview >= 0 && preview < count) {
        p.replace = preview;
        // The old preview tab goes, so a place right of it moves left one.
        if (target > preview) target--;
    }
    p.index = target;
    return p;
}

int moveTarget(int focused, bool split, int direction) {
    if (!split) return direction > 0 ? 1 : -1;
    if (direction > 0) return focused == 0 ? 1 : -1;
    return focused == 1 ? 0 : -1;
}

double leftGroupWidth(double total, double fraction, double minWidth) {
    if (total <= 0) return 0;
    if (!(fraction > 0 && fraction < 1)) fraction = 0.5;
    if (total < 2 * minWidth) return std::round(total / 2);
    return std::min(std::max(std::round(total * fraction), minWidth), total - minWidth);
}

GroupLayout restoreGroups(const GroupsSession &saved, const std::vector<std::string> &open) {
    GroupLayout out;
    const int focused = saved.focused == 1 ? 1 : 0;
    std::vector<bool> placed(open.size(), false);
    for (int g = 0; g < 2; g++) {
        std::set<int> seen;
        for (const std::string &f : saved.groups[g]) {
            for (size_t i = 0; i < open.size(); i++) {
                if (open[i] != f || seen.count((int)i)) continue;
                seen.insert((int)i);
                out.groups[g].push_back((int)i);
                placed[i] = true;
                break;
            }
        }
    }
    for (size_t i = 0; i < open.size(); i++)
        if (!placed[i]) out.groups[focused].push_back((int)i);
    if (out.groups[0].empty() || out.groups[1].empty()) return GroupLayout();
    out.split = true;
    out.focused = focused;
    for (int g = 0; g < 2; g++) {
        out.active[g] = 0;
        for (size_t k = 0; k < out.groups[g].size(); k++)
            if (open[(size_t)out.groups[g][k]] == saved.active[g]) {
                out.active[g] = (int)k;
                break;
            }
    }
    double f = saved.fraction;
    if (!(f >= 0.15)) f = f > 0 ? 0.15 : 0.5;
    out.fraction = std::min(f, 0.85);
    return out;
}

}  // namespace EditorTabs

// ---------------------------------------------------------------- Model::Window

namespace Model {

namespace {
const std::vector<DocId> kNoTabs;
const std::string kNoPath;
}  // namespace

const std::vector<DocId> &Window::tabs(int group) const {
    return group >= 0 && group < groupCount() ? groups_[group].tabs : kNoTabs;
}

DocId Window::active(int group) const {
    return group >= 0 && group < groupCount() ? groups_[group].active : 0;
}

DocId Window::at(int index) const {
    const std::vector<DocId> &t = tabs();
    return index >= 0 && index < (int)t.size() ? t[(size_t)index] : 0;
}

int Window::indexOf(DocId id, int group) const {
    if (!id) return -1;
    const std::vector<DocId> &t = tabs(group);
    const auto it = std::find(t.begin(), t.end(), id);
    return it == t.end() ? -1 : (int)(it - t.begin());
}

int Window::groupOf(DocId id) const {
    if (inGroup(id, focused_)) return focused_;
    const int o = other();
    return o >= 0 && inGroup(id, o) ? o : -1;
}

std::vector<DocId> Window::documents() const {
    std::vector<DocId> out = tabs(focused_);
    for (DocId id : tabs(other()))
        if (!inGroup(id, focused_)) out.push_back(id);
    return out;
}

void Window::setFraction(double f) {
    if (!(f >= 0)) f = 0;   // NaN too
    fraction_ = std::min(f, 1.0);
}

Window::Info *Window::infoOf(DocId id) {
    for (auto &p : info_)
        if (p.first == id) return &p.second;
    return nullptr;
}

const Window::Info *Window::infoOf(DocId id) const {
    for (const auto &p : info_)
        if (p.first == id) return &p.second;
    return nullptr;
}

const std::string &Window::path(DocId id) const {
    const Info *i = infoOf(id);
    return i ? i->path : kNoPath;
}

void Window::setPath(DocId id, const std::string &path) {
    if (Info *i = infoOf(id)) i->path = path;
}

DocId Window::find(const std::string &path) const {
    if (path.empty()) return 0;
    for (DocId id : documents())
        if (this->path(id) == path) return id;
    return 0;
}

Preview Window::preview(DocId id) const {
    const Info *i = infoOf(id);
    return i ? i->preview : Preview::None;
}

void Window::setPreview(DocId id, Preview p) {
    if (Info *i = infoOf(id)) i->preview = p;
}

DocId Window::previewTab(Preview kind) const {
    if (kind == Preview::None) return 0;
    for (DocId id : kind == Preview::User ? tabs() : documents())
        if (preview(id) == kind) return id;
    return 0;
}

bool Window::keep(DocId id) {
    Info *i = infoOf(id);
    if (!i || i->preview == Preview::None) return false;
    i->preview = Preview::None;
    return true;
}

std::uint64_t Window::lastUsed(DocId id) const {
    const Info *i = infoOf(id);
    return i ? i->lastUsed : 0;
}

std::vector<std::string> Window::labels(int group) const {
    std::vector<std::string> paths;
    for (DocId id : tabs(group)) paths.push_back(path(id));
    return EditorTabs::labels(paths);
}

void Window::ensure(DocId id, const std::string &path, Preview kind) {
    if (Info *i = infoOf(id)) {
        i->path = path;
        i->preview = kind;
        return;
    }
    Info i;
    i.path = path;
    i.preview = kind;
    info_.emplace_back(id, i);
}

void Window::forgetIfClosed(DocId id) {
    if (isOpen(id)) return;
    info_.erase(std::remove_if(info_.begin(), info_.end(),
                               [id](const std::pair<DocId, Info> &p) { return p.first == id; }),
                info_.end());
}

void Window::touch(DocId id) {
    if (Info *i = infoOf(id)) i->lastUsed = ++stamp_;
}

void Window::insertAt(int group, int index, DocId id) {
    std::vector<DocId> &t = groups_[group].tabs;
    index = std::max(0, std::min(index, (int)t.size()));
    t.insert(t.begin() + index, id);
}

// ---- opening

Window::Open Window::prepareOpen(const std::string &path, EditorTabs::OpenFrom from,
                                 const EditorTabs::PreviewSettings &settings) {
    Open o;
    o.inPreview = EditorTabs::opensInPreview(from, settings);
    const DocId have = find(path);
    const DocId on = active();
    if (from == EditorTabs::OpenFrom::CodeNavigation && o.inPreview && on &&
        preview(on) == Preview::User && have != on)
        keep(on);
    if (have) {
        if (isPreview(have) && !EditorTabs::staysPreview(true, o.inPreview))
            keep(have);
        o.existing = have;
    }
    return o;
}

DocId Window::add(DocId id, const std::string &path, Preview kind,
                  const std::function<bool(DocId)> &keepOld,
                  const std::function<bool(DocId)> &passOver) {
    if (!id || isOpen(id)) return 0;
    const int f = focused_;
    DocId old = 0;
    if (kind != Preview::None)
        for (DocId t : kind == Preview::User ? tabs(f) : documents())
            if (preview(t) == kind && !(passOver && passOver(t))) {
                old = t;
                break;
            }
    if (old && keepOld && keepOld(old)) {
        keep(old);
        old = 0;
    }
    const int on = indexOf(groups_[f].active, f);
    const int n = (int)groups_[f].tabs.size();
    const int oldAt = indexOf(old, f);
    int at;
    if (kind == Preview::User) {
        const EditorTabs::PreviewPlace place = EditorTabs::placePreview(on, oldAt, n);
        // `index` counts without the old tab, which is still in the row.
        at = place.index;
        if (place.replace >= 0 && at >= place.replace) at++;
    } else if (old && oldAt >= 0) {
        at = oldAt;   // the agents' new preview where the old one was
    } else {
        at = EditorTabs::insertionIndex(on, n);
    }
    ensure(id, path, kind);
    insertAt(f, at, id);
    if (old) {
        if (groups_[f].active == old) groups_[f].active = id;
        // A group's own preview tab leaves that group only; the agents'
        // leaves the window.
        if (kind == Preview::User) close(old, f, nullptr);
        else remove(old);
    }
    return old;
}

void Window::append(DocId id, const std::string &path, Preview kind) {
    if (!id || isOpen(id)) return;
    ensure(id, path, kind);
    groups_[focused_].tabs.push_back(id);
}

void Window::insert(DocId id, const std::string &path, int index) {
    if (!id || isOpen(id)) return;
    ensure(id, path, Preview::None);
    insertAt(focused_, index, id);
}

// ---- on show

bool Window::activate(DocId id) {
    if (!isOpen(id)) return false;
    Group &g = groups_[focused_];
    if (!inGroup(id, focused_))
        insertAt(focused_, EditorTabs::insertionIndex(indexOf(g.active, focused_), (int)g.tabs.size()),
                 id);
    g.active = id;
    touch(id);
    return true;
}

DocId Window::select(int index) {
    const DocId id = at(index);
    if (id) activate(id);
    return id;
}

DocId Window::step(int delta) { return select(EditorTabs::step(activeIndex(), count(), delta)); }

DocId Window::selectNumber(int n) { return select(EditorTabs::indexForNumber(n, count())); }

bool Window::move(int from, int to, int group) {
    if (group < 0) group = focused_;
    if (group >= groupCount()) return false;
    std::vector<DocId> &t = groups_[group].tabs;
    const int n = (int)t.size();
    if (from < 0 || from >= n || to < 0 || to >= n) return false;
    const DocId id = t[(size_t)from];
    t.erase(t.begin() + from);
    t.insert(t.begin() + to, id);
    return true;
}

// ---- closing

DocId Window::nextAfterClose(int group, int index) const {
    const Group &g = groups_[group];
    std::vector<std::uint64_t> stamps;
    for (DocId id : g.tabs) stamps.push_back(lastUsed(id));
    const int next = EditorTabs::activeAfterClose(stamps, indexOf(g.active, group), index);
    if (next < 0) return 0;
    // `next` counts in the row without the closed tab.
    return g.tabs[(size_t)(next >= index ? next + 1 : next)];
}

void Window::dropGroup(int group) {
    if (group == 0) groups_[0] = std::move(groups_[1]);
    groups_[1] = Group();
    focused_ = 0;
    if (groups_[0].active) touch(groups_[0].active);
}

Window::Closed Window::close(DocId id, int group, const EditorTabs::ClosedTab *remember) {
    Closed r;
    if (group < 0) group = focused_;
    const int index = indexOf(id, group);
    if (index < 0) return r;
    const bool wasSplit = split();
    r.closed = true;
    r.stillOpen = wasSplit && inGroup(id, 1 - group);
    if (remember && !r.stillOpen && !path(id).empty()) {
        EditorTabs::ClosedTab c = *remember;
        c.path = path(id);
        c.index = index;
        closed_.remember(c);
    }
    const DocId shownBefore = active();
    Group &g = groups_[group];
    const bool wasActive = g.active == id;
    const DocId next = wasActive ? nextAfterClose(group, index) : g.active;
    g.tabs.erase(g.tabs.begin() + index);
    if (wasActive) {
        g.active = next;
        if (next) touch(next);
    }
    if (wasSplit && g.tabs.empty()) {
        dropGroup(group);
        r.groupClosed = true;
    }
    forgetIfClosed(id);
    r.activeChanged = active() != shownBefore;
    return r;
}

void Window::remove(DocId id) {
    // The right group first: closing the left one's last tab renumbers it.
    for (int g : {1, 0})
        if (inGroup(id, g)) close(id, g, nullptr);
}

void Window::clear() {
    groups_[0] = Group();
    groups_[1] = Group();
    focused_ = 0;
    info_.clear();
}

std::vector<DocId> Window::othersThan(DocId id) const {
    std::vector<DocId> out;
    if (indexOf(id) < 0) return out;
    for (DocId t : tabs())
        if (t != id) out.push_back(t);
    return out;
}

std::vector<DocId> Window::rightOf(DocId id) const {
    const int i = indexOf(id);
    if (i < 0) return {};
    return std::vector<DocId>(tabs().begin() + i + 1, tabs().end());
}

// ---- Reopen Closed Tab

bool Window::takeClosed(EditorTabs::ClosedTab *out,
                        const std::function<bool(const std::string &)> &reopenable) {
    return closed_.take(out, reopenable);
}

// ---- the split editor

bool Window::putInOther(DocId id) {
    if (!isOpen(id)) return false;
    const int o = split() ? 1 - focused_ : 1;   // not split: focused_ is 0
    Group &g = groups_[o];
    if (std::find(g.tabs.begin(), g.tabs.end(), id) == g.tabs.end()) {
        const auto on = std::find(g.tabs.begin(), g.tabs.end(), g.active);
        const int at = on == g.tabs.end() ? -1 : (int)(on - g.tabs.begin());
        insertAt(o, EditorTabs::insertionIndex(at, (int)g.tabs.size()), id);
    }
    g.active = id;
    return true;
}

bool Window::focus(int group) {
    if (group < 0 || group >= groupCount()) return false;
    focused_ = group;
    if (groups_[group].active) touch(groups_[group].active);
    return true;
}

bool Window::splitEditor() {
    const DocId a = active();
    if (!a || !putInOther(a)) return false;
    return focus(other());
}

bool Window::canMove(int direction) const {
    const int target = EditorTabs::moveTarget(focused(), split(), direction);
    return active() && target >= 0 && target != focused() && (split() || count() >= 2);
}

bool Window::moveActive(int direction) {
    if (!canMove(direction)) return false;
    const DocId a = active();
    putInOther(a);
    focus(other());
    close(a, other(), nullptr);   // out of the group it left
    return true;
}

EditorTabs::GroupsSession Window::groupsSession() const {
    EditorTabs::GroupsSession s;
    for (int g = 0; g < 2; g++) {
        for (DocId id : tabs(g))
            if (!path(id).empty()) s.groups[g].push_back(path(id));
        s.active[g] = path(active(g));
    }
    s.focused = focused();
    s.fraction = fraction_;
    return s;
}

bool Window::restoreGroups(const EditorTabs::GroupLayout &layout) {
    if (!layout.split || split()) return false;
    const std::vector<DocId> all = documents();
    std::vector<DocId> g[2];
    for (int k = 0; k < 2; k++)
        for (int i : layout.groups[k])
            if (i >= 0 && i < (int)all.size() &&
                std::find(g[k].begin(), g[k].end(), all[(size_t)i]) == g[k].end())
                g[k].push_back(all[(size_t)i]);
    if (g[0].empty() || g[1].empty()) return false;
    const int f = layout.focused == 1 ? 1 : 0;
    // Nothing is left out of both.
    for (DocId id : all)
        if (std::find(g[0].begin(), g[0].end(), id) == g[0].end() &&
            std::find(g[1].begin(), g[1].end(), id) == g[1].end())
            g[f].push_back(id);
    for (int k = 0; k < 2; k++) {
        const int a = layout.active[k];
        groups_[k].tabs = g[k];
        groups_[k].active = a >= 0 && a < (int)g[k].size() ? g[k][(size_t)a] : g[k].front();
    }
    focused_ = f;
    setFraction(layout.fraction);
    touch(groups_[1 - f].active);
    touch(groups_[f].active);
    return true;
}

// ---------------------------------------------------------------- Model::Session

Session::WindowId Session::addWindow() {
    const WindowId id = ++next_;
    windows_.emplace_back(id, std::unique_ptr<Window>(new Window()));
    return id;
}

void Session::removeWindow(WindowId id) {
    windows_.erase(std::remove_if(windows_.begin(), windows_.end(),
                                  [id](const std::pair<WindowId, std::unique_ptr<Window>> &w) {
                                      return w.first == id;
                                  }),
                   windows_.end());
}

Window *Session::window(WindowId id) {
    for (auto &w : windows_)
        if (w.first == id) return w.second.get();
    return nullptr;
}

const Window *Session::window(WindowId id) const {
    for (const auto &w : windows_)
        if (w.first == id) return w.second.get();
    return nullptr;
}

std::vector<Session::WindowId> Session::windows() const {
    std::vector<WindowId> out;
    for (const auto &w : windows_) out.push_back(w.first);
    return out;
}

std::vector<Session::WindowId> Session::windowsOn(const std::string &root) const {
    std::vector<WindowId> out;
    for (const auto &w : windows_)
        if (w.second->root() == root) out.push_back(w.first);
    return out;
}

Session::WindowId Session::windowWith(const std::string &path) const {
    for (const auto &w : windows_)
        if (w.second->find(path)) return w.first;
    return 0;
}

}  // namespace Model
