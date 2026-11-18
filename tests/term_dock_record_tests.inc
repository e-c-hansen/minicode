// term_dock_record_tests.inc — the `termdock:*` groups of run_tests.cpp: a
// window's terminal dock in its record on the session's host
// (TermDockRecord.h, "termLayout" beside "terms"): written and read back,
// a record from an app before it, terminals gone while no window showed
// them, records that do not read, and through Shared::reattachFrom.
// Included after session_backups_tests.inc, whose stateRecord it uses.

#include "TermDockRecord.h"

namespace {

using Ids = std::vector<std::uint64_t>;
using Cols = std::vector<std::vector<std::uint64_t>>;

Shared::TermDock::Tab dockTab(Cols columns, TerminalLayout::Fractions colShares,
                              std::vector<TerminalLayout::Fractions> rowShares, int activeColumn = 0,
                              int activeRow = 0) {
    Shared::TermDock::Tab t;
    t.columns = std::move(columns);
    t.layout.columns = std::move(colShares);
    t.layout.rows = std::move(rowShares);
    t.activeColumn = activeColumn;
    t.activeRow = activeRow;
    return t;
}

// A record's tabs as an app writes them: "terms" and, unless null, the layout.
Json dockRecord(const Ids& terms, Json layout) {
    Json tabs = Json::object();
    tabs.set("groups", Json::array());
    Json t = Json::array();
    for (std::uint64_t id : terms) t.push(Json((int64_t)id));
    tabs.set("terms", std::move(t));
    if (!layout.isNull()) tabs.set("termLayout", std::move(layout));
    return tabs;
}

Json dockParsed(const std::string& text) {
    Json j;
    Json::parse(text, j);
    return j;
}

bool sharesNear(const TerminalLayout::Fractions& a, const TerminalLayout::Fractions& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::fabs(a[i] - b[i]) > 1e-9) return false;
    return true;
}

// A tab with one pane each, as a record without a layout comes back.
bool loneTabs(const Shared::TermDock& d, const Ids& ids) {
    if (d.tabs.size() != ids.size()) return false;
    for (size_t i = 0; i < ids.size(); i++) {
        const Shared::TermDock::Tab& t = d.tabs[i];
        if (t.columns != Cols{{ids[i]}} || !sharesNear(t.layout.columns, {1.0}) ||
            t.layout.rows.size() != 1 || !sharesNear(t.layout.rows[0], {1.0}) || t.activeColumn != 0 ||
            t.activeRow != 0)
            return false;
    }
    return true;
}

}  // namespace

static void testTermDockRecord() {
    using Shared::TermDock;

    GROUP("termdock:round-trip");
    {
        // Split right, then down in the right column, the right column made
        // narrower, its top pane with the keyboard; a second tab on show.
        TermDock d;
        d.tabs.push_back(dockTab({{11}, {12, 13}}, {0.6, 0.4}, {{1.0}, {0.25, 0.75}}, 1, 0));
        d.tabs.push_back(dockTab({{14}}, {1.0}, {{1.0}}));
        d.activeTab = 1;
        const Json j = Shared::termLayoutJson(d);
        // The shape, exactly (the Mac's half reads and writes the same).
        CHECK(j.dump() ==
              "{\"tabs\":[{\"columns\":[[11],[12,13]],\"columnShares\":[0.6,0.4],"
              "\"rowShares\":[[1],[0.25,0.75]],\"activeColumn\":1,\"activeRow\":0},"
              "{\"columns\":[[14]],\"columnShares\":[1],\"rowShares\":[[1]],\"activeColumn\":0,"
              "\"activeRow\":0}],\"activeTab\":1}");
        CHECK((d.order() == Ids{11, 12, 13, 14}));
        const TermDock back = Shared::termDockFrom(dockRecord({11, 12, 13, 14}, j), {11, 12, 13, 14});
        CHECK(back.tabs.size() == 2 && back.activeTab == 1);
        if (back.tabs.size() == 2) {
            const TermDock::Tab& t = back.tabs[0];
            CHECK((t.columns == Cols{{11}, {12, 13}}));
            CHECK(sharesNear(t.layout.columns, {0.6, 0.4}));
            CHECK(t.layout.rows.size() == 2 && sharesNear(t.layout.rows[0], {1.0}) &&
                  sharesNear(t.layout.rows[1], {0.25, 0.75}));
            CHECK(t.activeColumn == 1 && t.activeRow == 0);
            CHECK((back.tabs[1].columns == Cols{{14}}));
        }
        CHECK(Shared::termLayoutJson(back) == j);
        // Shares to four places, read back summing to 1.
        TermDock thirds;
        thirds.tabs.push_back(dockTab({{1}, {2}, {3}}, TerminalLayout::even(3), {{1.0}, {1.0}, {1.0}}));
        const Json tj = Shared::termLayoutJson(thirds);
        CHECK(tj["tabs"][0]["columnShares"].dump() == "[0.3333,0.3333,0.3333]");
        const TermDock tb = Shared::termDockFrom(dockRecord({1, 2, 3}, tj), {1, 2, 3});
        CHECK(tb.tabs.size() == 1 && sharesNear(tb.tabs[0].layout.columns, TerminalLayout::even(3)));
        // A pane not on the window's host (id 0) is left out as it is written;
        // nothing left writes no layout.
        TermDock zero;
        zero.tabs.push_back(dockTab({{0}, {7}}, {0.3, 0.7}, {{1.0}, {1.0}}, 0, 0));
        const Json zj = Shared::termLayoutJson(zero);
        CHECK(zj["tabs"][0]["columns"].dump() == "[[7]]" && zj["tabs"][0]["columnShares"].dump() == "[1]");
        TermDock none;
        none.tabs.push_back(dockTab({{0}}, {1.0}, {{1.0}}));
        CHECK(Shared::termLayoutJson(none).isNull());
        CHECK(Shared::termLayoutJson(TermDock()).isNull());
    }

    GROUP("termdock:no-layout");
    {
        // A record an app before the layout wrote: "terms" alone, a tab
        // each, in their order, the first on show.
        const TermDock old = Shared::termDockFrom(dockRecord({31, 32, 33}, Json()), {31, 32, 33});
        CHECK(loneTabs(old, {31, 32, 33}) && old.activeTab == 0);
        // No record at all (terminals no record names).
        const TermDock bare = Shared::termDockFrom(Json(), {40, 41});
        CHECK(loneTabs(bare, {40, 41}) && bare.activeTab == 0);
        // Nothing taken: nothing, whatever the record says.
        TermDock d;
        d.tabs.push_back(dockTab({{11}, {12}}, {0.5, 0.5}, {{1.0}, {1.0}}));
        CHECK(Shared::termDockFrom(dockRecord({11, 12}, Shared::termLayoutJson(d)), {}).tabs.empty());
        // A taken terminal the layout does not place (no record names it)
        // follows as a tab of its own.
        const TermDock more = Shared::termDockFrom(dockRecord({11, 12}, Shared::termLayoutJson(d)), {11, 12, 50});
        CHECK(more.tabs.size() == 2 && (more.tabs[0].columns == Cols{{11}, {12}}) &&
              (more.tabs[1].columns == Cols{{50}}));
        CHECK((more.order() == Ids{11, 12, 50}));
    }

    GROUP("termdock:gone");
    {
        // A pane in the middle of a column of three: its share to the one
        // above it, the one below keeping its place and share.
        TermDock d;
        d.tabs.push_back(dockTab({{1, 2, 3}}, {1.0}, {{0.2, 0.3, 0.5}}, 0, 2));
        TermDock got = Shared::termDockFrom(dockRecord({1, 2, 3}, Shared::termLayoutJson(d)), {1, 3});
        CHECK(got.tabs.size() == 1 && (got.tabs[0].columns == Cols{{1, 3}}));
        CHECK(got.tabs.size() == 1 && sharesNear(got.tabs[0].layout.rows[0], {0.5, 0.5}));
        CHECK(got.tabs.size() == 1 && got.tabs[0].activeColumn == 0 && got.tabs[0].activeRow == 1);
        // A whole column: its width to the column before; the one after
        // keeps its place.
        d.tabs = {dockTab({{1}, {2}, {3}}, {0.2, 0.3, 0.5}, {{1.0}, {1.0}, {1.0}}, 2, 0)};
        got = Shared::termDockFrom(dockRecord({1, 2, 3}, Shared::termLayoutJson(d)), {1, 3});
        CHECK(got.tabs.size() == 1 && (got.tabs[0].columns == Cols{{1}, {3}}));
        CHECK(got.tabs.size() == 1 && sharesNear(got.tabs[0].layout.columns, {0.5, 0.5}));
        CHECK(got.tabs.size() == 1 && got.tabs[0].activeColumn == 1 && got.tabs[0].activeRow == 0);
        // The first column: its width to the one after.
        d.tabs = {dockTab({{1}, {2}}, {0.3, 0.7}, {{1.0}, {1.0}})};
        got = Shared::termDockFrom(dockRecord({1, 2}, Shared::termLayoutJson(d)), {2});
        CHECK(got.tabs.size() == 1 && (got.tabs[0].columns == Cols{{2}}) &&
              sharesNear(got.tabs[0].layout.columns, {1.0}));
        // Split right, then down (the checks' shape), the bottom right pane
        // gone: the right column is one pane again, the widths kept.
        d.tabs = {dockTab({{1}, {2, 3}}, {0.6, 0.4}, {{1.0}, {0.5, 0.5}}, 1, 1)};
        got = Shared::termDockFrom(dockRecord({1, 2, 3}, Shared::termLayoutJson(d)), {1, 2});
        CHECK(got.tabs.size() == 1 && (got.tabs[0].columns == Cols{{1}, {2}}));
        CHECK(got.tabs.size() == 1 && sharesNear(got.tabs[0].layout.columns, {0.6, 0.4}) &&
              got.tabs[0].layout.rows.size() == 2 && sharesNear(got.tabs[0].layout.rows[1], {1.0}));
        // The pane with the keyboard gone: the one above it has it.
        CHECK(got.tabs.size() == 1 && got.tabs[0].activeColumn == 1 && got.tabs[0].activeRow == 0);
        // The top of a column with the keyboard: the one below.
        d.tabs = {dockTab({{1}, {2, 3}}, {0.6, 0.4}, {{1.0}, {0.5, 0.5}}, 1, 0)};
        got = Shared::termDockFrom(dockRecord({1, 2, 3}, Shared::termLayoutJson(d)), {1, 3});
        CHECK(got.tabs.size() == 1 && (got.tabs[0].columns == Cols{{1}, {3}}) &&
              got.tabs[0].activeColumn == 1 && got.tabs[0].activeRow == 0);
        // A lone first column with the keyboard: the column after.
        d.tabs = {dockTab({{1}, {2, 3}}, {0.6, 0.4}, {{1.0}, {0.5, 0.5}}, 0, 0)};
        got = Shared::termDockFrom(dockRecord({1, 2, 3}, Shared::termLayoutJson(d)), {2, 3});
        CHECK(got.tabs.size() == 1 && (got.tabs[0].columns == Cols{{2, 3}}) &&
              got.tabs[0].activeColumn == 0 && got.tabs[0].activeRow == 0 &&
              sharesNear(got.tabs[0].layout.rows[0], {0.5, 0.5}));
        // A pane before the active one in its column: the active one keeps
        // the keyboard, a row up.
        d.tabs = {dockTab({{1, 2, 3}}, {1.0}, {{0.2, 0.3, 0.5}}, 0, 2)};
        got = Shared::termDockFrom(dockRecord({1, 2, 3}, Shared::termLayoutJson(d)), {2, 3});
        CHECK(got.tabs.size() == 1 && (got.tabs[0].columns == Cols{{2, 3}}) && got.tabs[0].activeRow == 1);
        // A column before the active one: the active one keeps it.
        d.tabs = {dockTab({{1}, {2}, {3}}, {0.2, 0.3, 0.5}, {{1.0}, {1.0}, {1.0}}, 2, 0)};
        got = Shared::termDockFrom(dockRecord({1, 2, 3}, Shared::termLayoutJson(d)), {2, 3});
        CHECK(got.tabs.size() == 1 && (got.tabs[0].columns == Cols{{2}, {3}}) && got.tabs[0].activeColumn == 1);
        // Whole tabs: the one before the tab on show, and the one on show.
        d.tabs = {dockTab({{1}}, {1.0}, {{1.0}}), dockTab({{2}}, {1.0}, {{1.0}}), dockTab({{3}}, {1.0}, {{1.0}})};
        d.activeTab = 2;
        got = Shared::termDockFrom(dockRecord({1, 2, 3}, Shared::termLayoutJson(d)), {2, 3});
        CHECK(got.tabs.size() == 2 && got.activeTab == 1 && (got.tabs[1].columns == Cols{{3}}));
        d.activeTab = 1;
        got = Shared::termDockFrom(dockRecord({1, 2, 3}, Shared::termLayoutJson(d)), {1, 3});
        CHECK(got.tabs.size() == 2 && got.activeTab == 0 && (got.tabs[0].columns == Cols{{1}}));
        d.activeTab = 0;
        got = Shared::termDockFrom(dockRecord({1, 2, 3}, Shared::termLayoutJson(d)), {2, 3});
        CHECK(got.tabs.size() == 2 && got.activeTab == 0 && (got.tabs[0].columns == Cols{{2}}));
        // Every pane gone: nothing.
        got = Shared::termDockFrom(dockRecord({1, 2, 3}, Shared::termLayoutJson(d)), {});
        CHECK(got.tabs.empty() && got.activeTab == 0);
        // One terminal named twice (a record edited by hand): the first place.
        got = Shared::termDockFrom(
            dockParsed("{\"termLayout\":{\"tabs\":[{\"columns\":[[5],[6,5]],\"columnShares\":[0.5,0.5],"
                   "\"rowShares\":[[1],[0.5,0.5]],\"activeColumn\":1,\"activeRow\":1}],\"activeTab\":0}}"),
            {5, 6});
        CHECK(got.tabs.size() == 1 && (got.tabs[0].columns == Cols{{5}, {6}}) &&
              got.tabs[0].activeColumn == 1 && got.tabs[0].activeRow == 0);
    }

    GROUP("termdock:malformed");
    {
        // Shares that do not match the panes, or do not sum to 1, are equal
        // ones; places out of range are the first.
        TermDock got = Shared::termDockFrom(
            dockParsed("{\"termLayout\":{\"tabs\":[{\"columns\":[[1],[2,3]],\"columnShares\":[0.5],"
                   "\"rowShares\":[[1],[0.5,0.5]],\"activeColumn\":9,\"activeRow\":-1}],\"activeTab\":7}}"),
            {1, 2, 3});
        CHECK(got.tabs.size() == 1 && (got.tabs[0].columns == Cols{{1}, {2, 3}}));
        CHECK(got.tabs.size() == 1 && sharesNear(got.tabs[0].layout.columns, {0.5, 0.5}) &&
              sharesNear(got.tabs[0].layout.rows[1], {0.5, 0.5}));
        CHECK(got.tabs.size() == 1 && got.tabs[0].activeColumn == 0 && got.tabs[0].activeRow == 0 &&
              got.activeTab == 0);
        got = Shared::termDockFrom(
            dockParsed("{\"termLayout\":{\"tabs\":[{\"columns\":[[1],[2]],\"columnShares\":[0.9,0.9],"
                   "\"rowShares\":[[1],[1]]}]}}"),
            {1, 2});
        CHECK(got.tabs.size() == 1 && sharesNear(got.tabs[0].layout.columns, {0.5, 0.5}));
        // Shares that nearly sum to 1 are made to.
        got = Shared::termDockFrom(
            dockParsed("{\"termLayout\":{\"tabs\":[{\"columns\":[[1],[2]],\"columnShares\":[0.3333,0.6666],"
                   "\"rowShares\":[[1],[1]]}]}}"),
            {1, 2});
        CHECK(got.tabs.size() == 1 && std::fabs(got.tabs[0].layout.columns[0] + got.tabs[0].layout.columns[1] - 1) < 1e-12);
        // Ids that do not read, columns that are not lists, an empty column,
        // a tab that is not an object: dropped, the rest kept.
        got = Shared::termDockFrom(
            dockParsed("{\"termLayout\":{\"tabs\":[{\"columns\":[[1,\"x\",-4,2.5],\"y\",[],[2]],"
                   "\"columnShares\":[0.25,0.25,0.25,0.25]},7,{\"columns\":[]},{\"columns\":[[3]]}]}}"),
            {1, 2, 3});
        CHECK(got.tabs.size() == 2 && (got.tabs[0].columns == Cols{{1}, {2}}) &&
              (got.tabs[1].columns == Cols{{3}}));
        CHECK(got.tabs.size() == 2 && sharesNear(got.tabs[0].layout.columns, {0.5, 0.5}));
        // A layout that is not one: as no layout.
        for (const char* bad : {"{\"termLayout\":7}", "{\"termLayout\":{\"tabs\":{}}}", "{\"termLayout\":[]}"}) {
            got = Shared::termDockFrom(dockParsed(bad), {1, 2});
            CHECK(loneTabs(got, {1, 2}));
        }
    }

    GROUP("termdock:reattach");
    {
        // Through reattachFrom: a closed window's record names three
        // terminals in a split tab and a fourth in a tab of its own; one
        // exited while no window showed it (the host no longer lists it),
        // and another renderer's open window shows a terminal of its own.
        TermDock d;
        d.tabs.push_back(dockTab({{21}, {22, 23}}, {0.7, 0.3}, {{1.0}, {0.4, 0.6}}, 1, 1));
        d.tabs.push_back(dockTab({{24}}, {1.0}, {{1.0}}));
        Json st = Json::object();
        st.set("documents", Json::array());
        Json terms = Json::array();
        for (int64_t id : {21, 22, 24, 25}) {
            Json t = Json::object();
            t.set("term", Json(id));
            t.set("pid", Json(1000 + id));
            terms.push(std::move(t));
        }
        st.set("terminals", std::move(terms));
        Json wins = Json::array();
        Json closed = stateRecord(501, 9, false, 3, {}, {21, 22, 23, 24});
        if (Json* tabs = closed.member("tabs")) tabs->set("termLayout", Shared::termLayoutJson(d));
        wins.push(std::move(closed));
        wins.push(stateRecord(502, 8, true, 4, {}, {25}));
        st.set("windows", std::move(wins));
        const Shared::Reattach r = Shared::reattachFrom(st, 1, {}, nullptr, nullptr, false);
        Ids taken;
        for (const auto& t : r.terms) taken.push_back(t.id);
        CHECK(r.window == 501 && (taken == Ids{21, 22, 24}));
        const TermDock back = Shared::termDockFrom(r.tabs, taken);
        CHECK(back.tabs.size() == 2);
        if (back.tabs.size() == 2) {
            CHECK((back.tabs[0].columns == Cols{{21}, {22}}));
            CHECK(sharesNear(back.tabs[0].layout.columns, {0.7, 0.3}));
            CHECK(back.tabs[0].activeColumn == 1 && back.tabs[0].activeRow == 0);
            CHECK((back.tabs[1].columns == Cols{{24}}));
        }
        CHECK((back.order() == Ids{21, 22, 24}));
    }
}
