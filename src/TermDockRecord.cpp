// TermDockRecord.cpp — see TermDockRecord.h.
#include "TermDockRecord.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace Shared {

namespace {

using Tab = TermDock::Tab;

// Shares that read: as many as wanted, each above 0 and at most 1,
// summing to about 1 (TabSessions' rule for the window list's tabs).
bool readShares(const Json& j, size_t want, TerminalLayout::Fractions* out) {
    if (!j.isArray() || j.size() != want || want == 0) return false;
    TerminalLayout::Fractions fr;
    double sum = 0;
    for (const Json& e : j.elements()) {
        const double d = e.asNumber(-1);
        if (!(d > 0) || d > 1) return false;
        fr.push_back(d);
        sum += d;
    }
    if (std::fabs(sum - 1) > 0.01) return false;
    for (double& d : fr) d /= sum;
    *out = fr;
    return true;
}

// A terminal id: a whole number from 1 below 2^53; 0 for anything else.
std::uint64_t idOf(const Json& j) {
    const double d = j.asNumber(0);
    if (!(d >= 1) || d >= 9007199254740992.0 || d != std::floor(d)) return 0;
    return (std::uint64_t)d;
}

bool sharesMatch(const Tab& t) {
    if (t.layout.columns.size() != t.columns.size() || t.layout.rows.size() != t.columns.size())
        return false;
    for (size_t c = 0; c < t.columns.size(); c++)
        if (t.layout.rows[c].size() != t.columns[c].size()) return false;
    return true;
}

// No empty column, shares that match the panes (equal ones when they do
// not), the active pane one there is.
void sanitize(Tab& t) {
    const size_t before = t.columns.size();
    t.columns.erase(std::remove_if(t.columns.begin(), t.columns.end(),
                                   [](const std::vector<std::uint64_t>& c) { return c.empty(); }),
                    t.columns.end());
    if (t.columns.size() != before || !sharesMatch(t)) {
        t.layout.columns = TerminalLayout::even(t.columns.size());
        t.layout.rows.clear();
        for (const auto& c : t.columns) t.layout.rows.push_back(TerminalLayout::even(c.size()));
    }
    if (t.columns.empty()) {
        t.activeColumn = t.activeRow = 0;
        return;
    }
    if (t.activeColumn < 0 || t.activeColumn >= (int)t.columns.size()) t.activeColumn = 0;
    if (t.activeRow < 0 || t.activeRow >= (int)t.columns[(size_t)t.activeColumn].size()) t.activeRow = 0;
}

// Pane (c, r) out of its tab: its share to its neighbour, as the dock's
// closing gives it (TerminalLayout::removePane), and the active place
// moving as the keyboard would (TerminalGroup::take, the Mac's panel):
// the pane above, else below; a column gone, the column before, else after.
void removeAt(Tab& t, size_t c, size_t r) {
    const bool columnGoes = t.columns[c].size() == 1;
    const bool wasActive = t.activeColumn == (int)c && t.activeRow == (int)r;
    TerminalLayout::removePane(t.layout, c, r);
    t.columns[c].erase(t.columns[c].begin() + (long)r);
    if (columnGoes) t.columns.erase(t.columns.begin() + (long)c);
    if (wasActive) {
        if (!columnGoes) {
            t.activeColumn = (int)c;
            t.activeRow = r > 0 ? (int)r - 1 : 0;
        } else {
            t.activeColumn = c > 0 ? (int)c - 1 : 0;
            t.activeRow = 0;
        }
    } else if (columnGoes) {
        if (t.activeColumn > (int)c) t.activeColumn--;
    } else if (t.activeColumn == (int)c && t.activeRow > (int)r) {
        t.activeRow--;
    }
}

Json shares(const TerminalLayout::Fractions& fr) {
    Json a = Json::array();
    // Four places: a record that changes only past them is not sent again.
    for (double d : fr) a.push(Json(std::max(0.0001, std::round(d * 10000) / 10000)));
    return a;
}

TermDock parse(const Json& layout) {
    TermDock d;
    if (!layout.isObject() || !layout["tabs"].isArray()) return d;
    for (const Json& tj : layout["tabs"].elements()) {
        if (!tj.isObject() || !tj["columns"].isArray()) continue;
        Tab t;
        for (const Json& cj : tj["columns"].elements()) {
            std::vector<std::uint64_t> column;
            // A pane whose id does not read stays a pane until prune, so
            // the shares still match the record's panes.
            if (cj.isArray())
                for (const Json& p : cj.elements()) column.push_back(idOf(p));
            t.columns.push_back(column);
        }
        TerminalLayout::Layout l;
        bool ok = readShares(tj["columnShares"], t.columns.size(), &l.columns);
        const Json& rows = tj["rowShares"];
        l.rows.clear();
        if (ok && rows.isArray() && rows.size() == t.columns.size()) {
            for (size_t c = 0; c < t.columns.size() && ok; c++) {
                TerminalLayout::Fractions fr;
                ok = readShares(rows[c], t.columns[c].size(), &fr);
                l.rows.push_back(fr);
            }
        } else {
            ok = false;
        }
        if (ok) t.layout = l;
        else t.layout.rows.clear();   // sanitize evens them
        t.activeColumn = (int)tj["activeColumn"].asNumber(0);
        t.activeRow = (int)tj["activeRow"].asNumber(0);
        sanitize(t);
        if (!t.columns.empty()) d.tabs.push_back(std::move(t));
    }
    d.activeTab = (int)layout["activeTab"].asNumber(0);
    if (d.activeTab < 0 || d.activeTab >= (int)d.tabs.size()) d.activeTab = 0;
    return d;
}

}  // namespace

std::vector<std::uint64_t> TermDock::order() const {
    std::vector<std::uint64_t> out;
    for (const Tab& t : tabs)
        for (const auto& column : t.columns) out.insert(out.end(), column.begin(), column.end());
    return out;
}

void TermDock::prune(const std::function<bool(std::uint64_t)>& keep) {
    std::set<std::uint64_t> seen;
    for (size_t i = 0; i < tabs.size();) {
        Tab& t = tabs[i];
        sanitize(t);
        for (size_t c = 0; c < t.columns.size();) {
            bool columnGone = false;
            for (size_t r = 0; r < t.columns[c].size();) {
                const std::uint64_t id = t.columns[c][r];
                if (id && (!keep || keep(id)) && seen.insert(id).second) {
                    r++;
                    continue;
                }
                const bool last = t.columns[c].size() == 1;
                removeAt(t, c, r);
                if (last) {
                    columnGone = true;
                    break;
                }
            }
            if (!columnGone) c++;
        }
        if (!t.columns.empty()) {
            i++;
            continue;
        }
        // A tab left with no pane goes; the tab on show moves as closing it would.
        tabs.erase(tabs.begin() + (long)i);
        if (activeTab > (int)i) activeTab--;
        else if (activeTab == (int)i) activeTab = std::max(0, (int)i - 1);
    }
    if (activeTab < 0 || activeTab >= (int)tabs.size()) activeTab = 0;
}

Json termLayoutJson(TermDock dock) {
    dock.prune(nullptr);
    if (dock.tabs.empty()) return Json();
    Json tabs = Json::array();
    for (const Tab& t : dock.tabs) {
        Json o = Json::object();
        Json cols = Json::array();
        for (const auto& column : t.columns) {
            Json c = Json::array();
            for (std::uint64_t id : column) c.push(Json((int64_t)id));
            cols.push(std::move(c));
        }
        o.set("columns", std::move(cols));
        o.set("columnShares", shares(t.layout.columns));
        Json rows = Json::array();
        for (const auto& r : t.layout.rows) rows.push(shares(r));
        o.set("rowShares", std::move(rows));
        o.set("activeColumn", Json(t.activeColumn));
        o.set("activeRow", Json(t.activeRow));
        tabs.push(std::move(o));
    }
    Json out = Json::object();
    out.set("tabs", std::move(tabs));
    out.set("activeTab", Json(dock.activeTab));
    return out;
}

TermDock termDockFrom(const Json& recordTabs, const std::vector<std::uint64_t>& taken) {
    TermDock d = parse(recordTabs["termLayout"]);
    const std::set<std::uint64_t> want(taken.begin(), taken.end());
    d.prune([&want](std::uint64_t id) { return want.count(id) > 0; });
    // What the layout does not place: a tab each, in the order taken.
    const std::vector<std::uint64_t> placedNow = d.order();
    std::set<std::uint64_t> placed(placedNow.begin(), placedNow.end());
    for (std::uint64_t id : taken) {
        if (!id || !placed.insert(id).second) continue;
        Tab t;
        t.columns = {{id}};
        d.tabs.push_back(std::move(t));
    }
    if (d.activeTab < 0 || d.activeTab >= (int)d.tabs.size()) d.activeTab = 0;
    return d;
}

}  // namespace Shared
