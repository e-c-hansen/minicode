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

}  // namespace

std::string encode(const Sessions &all) {
    Json root = Json::object();
    for (const auto &kv : all) {
        const Session &s = kv.second;
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
                bt.push(o);
            }
            e.set("browser", bt);
            e.set("browserActive", s.browserActive);
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
        root.set(kv.first, e);
    }
    return root.dump() + "\n";
}

Sessions decode(const std::string &text) {
    Sessions out;
    Json root;
    if (!Json::parse(text, root) || !root.isObject()) return out;
    for (const auto &kv : root.items()) {
        const Json &e = kv.second;
        if (kv.first.empty() || !e.isObject() || !e["tabs"].isArray()) continue;
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
                keptBrowser.push_back((int)s.browser.size());
                s.browser.push_back(b);
            }
            const int ba = readIndex(e["browserActive"], keptBrowser.size());
            s.browserActive = ba >= 0 ? keptBrowser[(size_t)ba] : (s.browser.empty() ? -1 : 0);
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
        if (s.tabs.empty() && s.browser.empty()) continue;
        const int active = readIndex(e["active"], keptAt.size());
        const int preview = readIndex(e["preview"], keptAt.size());
        s.active = active >= 0 ? keptAt[(size_t)active] : -1;
        s.preview = preview >= 0 ? keptAt[(size_t)preview] : -1;
        s.used = e["used"].asNumber(0);
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
    if (key.empty()) return;
    if (s.tabs.empty() && s.browser.empty()) {
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

}  // namespace TabSessions
