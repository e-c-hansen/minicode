// TabSessions.cpp — see TabSessions.h.
#include "TabSessions.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "Json.h"

namespace TabSessions {

namespace {

Json encodeTab(const Tab &t) {
    Json j = Json::object();
    j.set("path", t.path);
    j.set("page", t.page);
    if (t.side) j.set("side", true);
    Json sel = Json::array();
    sel.push(Json((int64_t)t.selStart));
    sel.push(Json((int64_t)t.selEnd));
    j.set("sel", sel);
    if (t.haveView) {
        j.set("top", Json((int64_t)t.top));
        j.set("into", Json((int64_t)t.into));
    }
    return j;
}

// A count that reads: a whole number, not negative, within reason.
bool readCount(const Json &j, long *out) {
    if (!j.isNumber()) return false;
    const double d = j.asNumber();
    if (!(d >= 0) || d > 1e12 || std::floor(d) != d) return false;
    *out = (long)d;
    return true;
}

bool decodeTab(const Json &j, Tab *out) {
    if (!j.isObject() || !j["path"].isString()) return false;
    Tab t;
    t.path = j["path"].asString();
    if (t.path.empty()) return false;
    t.page = j["page"].asBool(false);
    t.side = j["side"].asBool(false);
    const Json &sel = j["sel"];
    long a = 0, b = 0;
    if (sel.isArray() && sel.size() == 2 && readCount(sel[0], &a) && readCount(sel[1], &b)) {
        t.selStart = a;
        t.selEnd = b;
    }
    long top = 0;
    if (readCount(j["top"], &top)) {
        t.haveView = true;
        t.top = top;
        // How far into the line may be above it, too (the view starting in
        // the margin over the first line).
        if (j["into"].isNumber()) t.into = (long)j["into"].asNumber();
    }
    *out = t;
    return true;
}

// An index into `count` tabs, or -1.
int readIndex(const Json &j, size_t count) {
    if (!j.isNumber()) return -1;
    const double d = j.asNumber();
    if (!(d >= 0) || d >= (double)count || std::floor(d) != d) return -1;
    return (int)d;
}

Json encodeSession(const Session &s) {
    Json e = Json::object();
    e.set("used", s.used);
    e.set("active", s.active);
    e.set("preview", s.preview);
    Json tabs = Json::array();
    for (const Tab &t : s.tabs) tabs.push(encodeTab(t));
    e.set("tabs", tabs);
    if (!s.browser.empty()) {
        Json bt = Json::array();
        for (const BrowserTab &b : s.browser) {
            Json o = Json::object();
            o.set("url", b.url);
            o.set("title", b.title);
            if (!b.state.empty()) o.set("state", b.state);
            if (b.archived) o.set("archived", true);
            bt.push(o);
        }
        e.set("browser", bt);
        e.set("browserActive", s.browserActive);
        if (s.browserShown) e.set("browserShown", true);
        if (s.browserFraction > 0) e.set("browserFraction", s.browserFraction);
    }
    if (s.split) {
        Json g = Json::object();
        for (int k = 0; k < 2; k++) {
            Json files = Json::array();
            for (const std::string &f : s.groups.groups[k]) files.push(Json(f));
            g.set(k == 0 ? "left" : "right", files);
        }
        g.set("leftActive", s.groups.active[0]);
        g.set("rightActive", s.groups.active[1]);
        g.set("focused", s.groups.focused);
        g.set("fraction", s.groups.fraction);
        e.set("groups", g);
    }
    if (!s.treeFocus.empty()) {
        Json focus = Json::array();
        for (const std::string &f : s.treeFocus) focus.push(Json(f));
        e.set("treeFocus", focus);
    }
    return e;
}

// A session as encodeSession wrote it; NO when it is not one at all. One
// with neither a tab nor a browser tab reads, empty.
bool decodeSession(const Json &e, Session *out) {
    if (!e.isObject() || !e["tabs"].isArray()) return false;
    Session s;
    if (e["browser"].isArray()) {
        std::vector<int> keptBrowser;
        for (const Json &j : e["browser"].elements()) {
            if (!j.isObject() || !j["url"].isString()) {
                keptBrowser.push_back(-1);
                continue;
            }
            BrowserTab b;
            b.url = j["url"].asString();
            b.title = j["title"].isString() ? j["title"].asString() : "";
            b.state = j["state"].isString() ? j["state"].asString() : "";
            b.archived = j["archived"].asBool(false);
            keptBrowser.push_back((int)s.browser.size());
            s.browser.push_back(b);
        }
        const int ba = readIndex(e["browserActive"], keptBrowser.size());
        s.browserActive = ba >= 0 ? keptBrowser[(size_t)ba] : (s.browser.empty() ? -1 : 0);
        s.browserShown = e["browserShown"].asBool(false);
        const double fr = e["browserFraction"].asNumber(0);
        s.browserFraction = fr > 0 && fr < 1 ? fr : 0;
    }
    // Indices count the tabs as written, so a tab that does not read
    // moves the ones after it: they are mapped to what is kept.
    std::vector<int> keptAt;
    for (const Json &j : e["tabs"].elements()) {
        Tab t;
        if (decodeTab(j, &t)) {
            keptAt.push_back((int)s.tabs.size());
            s.tabs.push_back(t);
        } else {
            keptAt.push_back(-1);
        }
    }
    const int active = readIndex(e["active"], keptAt.size());
    const int preview = readIndex(e["preview"], keptAt.size());
    s.active = active >= 0 ? keptAt[(size_t)active] : -1;
    s.preview = preview >= 0 ? keptAt[(size_t)preview] : -1;
    s.used = e["used"].asNumber(0);
    // A list of folders; before 2026-11-18, one folder's path.
    const Json &focus = e["treeFocus"];
    if (focus.isString() && !focus.asString().empty()) s.treeFocus.push_back(focus.asString());
    if (focus.isArray())
        for (const Json &f : focus.elements())
            if (f.isString() && !f.asString().empty()) s.treeFocus.push_back(f.asString());
    const Json &g = e["groups"];
    if (g.isObject() && g["left"].isArray() && g["right"].isArray()) {
        for (int k = 0; k < 2; k++)
            for (const Json &f : g[k == 0 ? "left" : "right"].elements())
                if (f.isString() && !f.asString().empty())
                    s.groups.groups[k].push_back(f.asString());
        s.groups.active[0] = g["leftActive"].isString() ? g["leftActive"].asString() : "";
        s.groups.active[1] = g["rightActive"].isString() ? g["rightActive"].asString() : "";
        s.groups.focused = g["focused"].asNumber(0) == 1 ? 1 : 0;
        const double fr = g["fraction"].asNumber(0.5);
        s.groups.fraction = fr > 0 && fr < 1 ? fr : 0.5;
        s.split = !s.groups.groups[0].empty() && !s.groups.groups[1].empty();
        if (!s.split) s.groups = EditorTabs::GroupsSession();
    }
    *out = s;
    return true;
}

}  // namespace

std::string encode(const Sessions &all) {
    Json root = Json::object();
    for (const auto &kv : all) root.set(kv.first, encodeSession(kv.second));
    return root.dump() + "\n";
}

Sessions decode(const std::string &text) {
    Sessions out;
    Json root;
    if (!Json::parse(text, root) || !root.isObject()) return out;
    for (const auto &kv : root.items()) {
        Session s;
        if (kv.first.empty() || kv.first == kWindowsKey || !decodeSession(kv.second, &s)) continue;
        if (s.tabs.empty() && s.browser.empty() && s.treeFocus.empty()) continue;
        out[kv.first] = s;
    }
    return out;
}

Session merged(const Session &mine, const std::vector<Session> &others) {
    Session out = mine;
    std::set<std::string> have;
    for (const Tab &t : mine.tabs) have.insert(t.path);
    for (const Session &o : others)
        for (const Tab &t : o.tabs)
            if (have.insert(t.path).second) out.tabs.push_back(t);
    return out;
}

void put(Sessions &all, const std::string &key, Session s, double now, size_t max) {
    if (key.empty() || key == kWindowsKey) return;
    if (s.tabs.empty() && s.browser.empty() && s.treeFocus.empty()) {
        all.erase(key);
        return;
    }
    if (s.active >= (int)s.tabs.size()) s.active = -1;
    if (s.preview >= (int)s.tabs.size()) s.preview = -1;
    s.used = now;
    all[key] = s;
    while (all.size() > max) {
        auto oldest = all.end();
        for (auto it = all.begin(); it != all.end(); ++it)
            if (it->first != key && (oldest == all.end() || it->second.used < oldest->second.used))
                oldest = it;
        if (oldest == all.end()) break;
        all.erase(oldest);
    }
}

EditorTabs::Session files(const Session &s) {
    EditorTabs::Session out;
    for (const Tab &t : s.tabs) out.files.push_back(t.path);
    out.active = s.active;
    out.preview = s.preview;
    return out;
}

EditorTabs::ClosedTab place(const Tab &t) {
    EditorTabs::ClosedTab c;
    c.path = t.path;
    c.preview = t.page;
    c.selStart = t.selStart;
    c.selEnd = t.selEnd;
    c.haveView = t.haveView;
    c.top = t.top;
    c.topOffset = (double)t.into;
    return c;
}

// ------------------------------------------------- the windows, next launch

namespace {

Json numbers(const std::vector<double> &v) {
    Json a = Json::array();
    for (double d : v) a.push(Json(d));
    return a;
}

// Shares that read: as many as wanted, each above 0, summing to about 1.
bool readShares(const Json &j, size_t want, TerminalLayout::Fractions *out) {
    if (!j.isArray() || j.size() != want || want == 0) return false;
    TerminalLayout::Fractions fr;
    double sum = 0;
    for (const Json &e : j.elements()) {
        const double d = e.asNumber(-1);
        if (!(d > 0) || d > 1) return false;
        fr.push_back(d);
        sum += d;
    }
    if (std::fabs(sum - 1) > 0.01) return false;
    for (double &d : fr) d /= sum;
    *out = fr;
    return true;
}

Json encodeTerminalTab(const TerminalTab &t) {
    Json o = Json::object();
    Json cols = Json::array();
    for (const auto &column : t.columns) {
        Json c = Json::array();
        for (const TerminalPane &p : column) {
            Json pane = Json::object();
            pane.set("cwd", p.cwd);
            if (!p.title.empty()) pane.set("title", p.title);
            c.push(pane);
        }
        cols.push(c);
    }
    o.set("columns", cols);
    o.set("columnShares", numbers(t.layout.columns));
    Json rows = Json::array();
    for (const auto &r : t.layout.rows) rows.push(numbers(r));
    o.set("rowShares", rows);
    o.set("activeColumn", t.activeColumn);
    o.set("activeRow", t.activeRow);
    return o;
}

bool decodeTerminalTab(const Json &o, TerminalTab *out) {
    if (!o.isObject() || !o["columns"].isArray()) return false;
    TerminalTab t;
    for (const Json &c : o["columns"].elements()) {
        if (!c.isArray()) continue;
        std::vector<TerminalPane> column;
        for (const Json &p : c.elements()) {
            if (!p.isObject() || !p["cwd"].isString()) continue;
            TerminalPane pane;
            pane.cwd = p["cwd"].asString();
            pane.title = p["title"].isString() ? p["title"].asString() : "";
            column.push_back(pane);
        }
        if (!column.empty()) t.columns.push_back(column);
    }
    if (t.columns.empty()) return false;
    TerminalLayout::Layout l;
    bool shares = readShares(o["columnShares"], t.columns.size(), &l.columns);
    const Json &rows = o["rowShares"];
    l.rows.clear();
    if (shares && rows.isArray() && rows.size() == t.columns.size()) {
        for (size_t c = 0; c < t.columns.size() && shares; c++) {
            TerminalLayout::Fractions fr;
            shares = readShares(rows[c], t.columns[c].size(), &fr);
            l.rows.push_back(fr);
        }
    } else {
        shares = false;
    }
    if (shares) t.layout = l;
    t.activeColumn = (int)o["activeColumn"].asNumber(0);
    t.activeRow = (int)o["activeRow"].asNumber(0);
    *out = sanitize(t);
    return true;
}

}  // namespace

TerminalTab sanitize(TerminalTab t) {
    std::vector<std::vector<TerminalPane>> columns;
    for (auto &c : t.columns)
        if (!c.empty()) columns.push_back(c);
    if (columns.empty()) columns.push_back({TerminalPane()});
    const bool matches = t.layout.columns.size() == columns.size() &&
                         t.layout.rows.size() == columns.size() && columns.size() == t.columns.size();
    bool rowsMatch = matches;
    for (size_t c = 0; rowsMatch && c < columns.size(); c++)
        rowsMatch = t.layout.rows[c].size() == columns[c].size();
    if (!rowsMatch) {
        t.layout.columns = TerminalLayout::even(columns.size());
        t.layout.rows.clear();
        for (const auto &c : columns) t.layout.rows.push_back(TerminalLayout::even(c.size()));
    }
    t.columns = columns;
    if (t.activeColumn < 0 || t.activeColumn >= (int)t.columns.size()) t.activeColumn = 0;
    if (t.activeRow < 0 || t.activeRow >= (int)t.columns[(size_t)t.activeColumn].size())
        t.activeRow = 0;
    return t;
}

namespace {

Json encodeWindowList(const std::vector<Window> &windows) {
    Json list = Json::array();
    for (const Window &w : windows) {
        Json o = Json::object();
        o.set("root", w.root);
        if (w.haveFrame) {
            Json f = Json::array();
            for (double d : {w.frame.x, w.frame.y, w.frame.w, w.frame.h}) f.push(Json(d));
            o.set("frame", f);
        }
        if (w.fullScreen) o.set("fullScreen", true);
        o.set("tabs", encodeSession(w.tabs));
        o.set("sidebar", w.sidebar);
        o.set("sidebarWidth", w.sidebarWidth);
        o.set("sidebarHidden", w.sidebarHidden);
        o.set("editorHidden", w.editorHidden);
        o.set("problems", w.problems);
        o.set("terminal", w.terminal);
        o.set("terminalHeight", w.terminalHeight);
        Json terms = Json::array();
        for (const TerminalTab &t : w.terminals) terms.push(encodeTerminalTab(t));
        o.set("terminals", terms);
        o.set("terminalTab", w.terminalTab);
        Json expanded = Json::array();
        for (const std::string &e : w.expanded) expanded.push(Json(e));
        o.set("expanded", expanded);
        if (!w.selected.empty()) o.set("selected", w.selected);
        o.set("treeScroll", w.treeScroll);
        if (w.ownerPid > 0) {
            o.set("ownerPid", Json((double)w.ownerPid));
            o.set("ownerStart", w.ownerStart);
            o.set("saved", w.saved);
        }
        list.push(o);
    }
    return list;
}

std::vector<Window> decodeWindowList(const Json &list) {
    std::vector<Window> out;
    if (!list.isArray()) return out;
    for (const Json &o : list.elements()) {
        if (!o.isObject() || !o["root"].isString() || o["root"].asString().empty()) continue;
        Window w;
        w.root = o["root"].asString();
        const Json &f = o["frame"];
        if (f.isArray() && f.size() == 4) {
            double v[4];
            bool ok = true;
            for (size_t i = 0; i < 4; i++) {
                v[i] = f[i].asNumber(NAN);
                ok = ok && std::isfinite(v[i]) && std::fabs(v[i]) < 1e6;
            }
            if (ok && v[2] > 0 && v[3] > 0) {
                w.haveFrame = true;
                w.frame = Frame{v[0], v[1], v[2], v[3]};
            }
        }
        w.fullScreen = o["fullScreen"].asBool(false);
        Session s;
        if (decodeSession(o["tabs"], &s)) w.tabs = s;
        const std::string sidebar = o["sidebar"].isString() ? o["sidebar"].asString() : "";
        if (sidebar == "git" || sidebar == "agents" || sidebar == "processes") w.sidebar = sidebar;
        const double sw = o["sidebarWidth"].asNumber(0);
        w.sidebarWidth = std::isfinite(sw) && sw > 0 && sw < 1e5 ? sw : 0;
        w.sidebarHidden = o["sidebarHidden"].asBool(false);
        w.editorHidden = o["editorHidden"].asBool(false);
        w.problems = o["problems"].asBool(false);
        w.terminal = o["terminal"].asBool(false);
        const double th = o["terminalHeight"].asNumber(0);
        w.terminalHeight = std::isfinite(th) && th > 0 && th < 1e5 ? th : 0;
        if (o["terminals"].isArray())
            for (const Json &t : o["terminals"].elements()) {
                TerminalTab tab;
                if (decodeTerminalTab(t, &tab)) w.terminals.push_back(tab);
            }
        w.terminalTab = readIndex(o["terminalTab"], w.terminals.size());
        if (w.terminalTab < 0 && !w.terminals.empty()) w.terminalTab = 0;
        if (w.terminals.empty()) w.terminal = false;
        if (o["expanded"].isArray())
            for (const Json &e : o["expanded"].elements())
                if (e.isString() && !e.asString().empty()) w.expanded.push_back(e.asString());
        w.selected = o["selected"].isString() ? o["selected"].asString() : "";
        const double ts = o["treeScroll"].asNumber(0);
        w.treeScroll = std::isfinite(ts) && ts > 0 && ts < 1e9 ? ts : 0;
        const double pid = o["ownerPid"].asNumber(0), start = o["ownerStart"].asNumber(0),
                     saved = o["saved"].asNumber(0);
        if (std::isfinite(pid) && pid > 0 && pid < 1e9 && std::isfinite(start) && start > 0 &&
            std::isfinite(saved) && saved >= 0) {
            w.ownerPid = (long)pid;
            w.ownerStart = start;
            w.saved = saved;
        }
        out.push_back(w);
    }
    return out;
}

}  // namespace

std::string encodeWindows(const std::vector<Window> &windows) {
    Json root = Json::object();
    root.set("windows", encodeWindowList(windows));
    return root.dump() + "\n";
}

std::vector<Window> decodeWindows(const std::string &text) {
    Json root;
    if (!Json::parse(text, root) || !root.isObject()) return {};
    return decodeWindowList(root["windows"]);
}

std::string encodeStore(const Store &store) {
    Json root = Json::object();
    for (const auto &kv : store.folders)
        if (kv.first != kWindowsKey) root.set(kv.first, encodeSession(kv.second));
    if (!store.windows.empty()) root.set(kWindowsKey, encodeWindowList(store.windows));
    return root.dump() + "\n";
}

Store decodeStore(const std::string &text) {
    Store out;
    out.folders = decode(text);
    out.windows = decodeWindows(text);
    return out;
}

std::vector<Window> afterClosing(const std::vector<Window> &open, size_t closing) {
    if (closing >= open.size()) return open;
    if (open.size() == 1) return open;
    std::vector<Window> out = open;
    out.erase(out.begin() + (long)closing);
    return out;
}

bool sameProcess(long pid, double start, const Process &p) {
    return pid > 0 && pid == p.pid && std::fabs(start - p.start) < 1.0;
}

namespace {
// Another mutible's window that one running now holds open.
bool heldElsewhere(const Window &w, const Process &me, IsRunning running, void *context) {
    return w.ownerPid > 0 && !sameProcess(w.ownerPid, w.ownerStart, me) && running &&
           running(w.ownerPid, w.ownerStart, context);
}
}  // namespace

std::vector<Window> mergeWindows(const std::vector<Window> &mine, const std::vector<Window> &stored,
                                 const Process &me, double now, IsRunning running, void *context) {
    std::vector<Window> out;
    for (Window w : mine) {
        w.ownerPid = me.pid;
        w.ownerStart = me.start;
        w.saved = now;
        out.push_back(w);
    }
    for (const Window &w : stored) {
        if (w.ownerPid <= 0 || sameProcess(w.ownerPid, w.ownerStart, me)) continue;
        if (heldElsewhere(w, me, running, context) || w.saved >= me.start) out.push_back(w);
    }
    return out;
}

std::vector<Window> toRestore(const std::vector<Window> &stored, const Process &me,
                              IsRunning running, void *context) {
    std::vector<Window> out;
    for (const Window &w : stored)
        if (!heldElsewhere(w, me, running, context)) out.push_back(w);
    return out;
}

Frame fitFrame(const Frame &saved, const std::vector<Frame> &screens, double minW, double minH) {
    if (screens.empty()) return saved;
    auto overlap = [](const Frame &a, const Frame &b) {
        const double w = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x);
        const double h = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
        return w > 0 && h > 0 ? w * h : 0.0;
    };
    auto inside = [](const Frame &a, const Frame &s) {
        return a.x >= s.x && a.y >= s.y && a.x + a.w <= s.x + s.w && a.y + a.h <= s.y + s.h;
    };
    Frame f = saved;
    f.w = std::max(f.w, minW);
    f.h = std::max(f.h, minH);
    for (const Frame &s : screens)
        if (inside(f, s)) return f;
    const Frame *best = &screens[0];
    double most = 0;
    for (const Frame &s : screens) {
        const double o = overlap(f, s);
        if (o > most) {
            most = o;
            best = &s;
        }
    }
    const Frame &s = *best;
    f.w = std::max(std::min(f.w, s.w), std::min(minW, s.w));
    f.h = std::max(std::min(f.h, s.h), std::min(minH, s.h));
    if (most == 0) {   // on no screen now: in the middle of the main one
        f.x = s.x + (s.w - f.w) / 2;
        f.y = s.y + (s.h - f.h) / 2;
        return f;
    }
    f.x = std::min(std::max(f.x, s.x), s.x + s.w - f.w);
    f.y = std::min(std::max(f.y, s.y), s.y + s.h - f.h);
    return f;
}

}  // namespace TabSessions
