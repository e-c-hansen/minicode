// session_model_tests.inc — the `session:*` groups of run_tests.cpp: the
// core's Model::Window and Model::Session (EditorTabs.h, SESSION-DESIGN.md
// stage 3a), held to the Mac's tab rules as CLAUDE.md gives them ("Tabs,
// sessions and the disk", "The preview tab", "Two editor groups"): where a
// new tab goes, which shows after a close, the keys, labels, the preview
// tab and the agents', Reopen Closed Tab, and the split editor's two
// groups. A random walk checks that the window's own bookkeeping holds
// whatever is done to it. Included there, after the editor tabs' tests.

namespace {

using Model::DocId;
using Model::Preview;
using EditorTabs::OpenFrom;

std::vector<DocId> ids(std::initializer_list<DocId> l) { return std::vector<DocId>(l); }

// A window with tabs 1..n of /p/f1 .. /p/fn, the first on show.
Model::Window windowWith(int n) {
    Model::Window w;
    for (int i = 1; i <= n; i++) w.append((DocId)i, "/p/f" + std::to_string(i));
    if (n > 0) w.activate(1);
    return w;
}

// What must hold of any window: each group's tabs once each, its tab on
// show one of them (or none only when it is empty), a second group only
// while split and never empty, the focus on a group that exists, and every
// document known exactly while it has a tab.
bool windowHolds(const Model::Window &w) {
    if (w.focused() < 0 || w.focused() >= w.groupCount()) return false;
    if (w.split() && w.tabs(1).empty()) return false;
    for (int g = 0; g < w.groupCount(); g++) {
        const std::vector<DocId> &t = w.tabs(g);
        std::set<DocId> seen(t.begin(), t.end());
        if (seen.size() != t.size() || seen.count(0)) return false;
        if (t.empty() ? w.active(g) != 0 : (w.active(g) != 0 && !seen.count(w.active(g))))
            return false;
    }
    const std::vector<DocId> all = w.documents();
    if (std::set<DocId>(all.begin(), all.end()).size() != all.size()) return false;
    for (DocId id : all)
        if (!w.isOpen(id)) return false;
    return true;
}

}  // namespace

static void testSessionModel() {
    GROUP("session:insert");
    {   // a new tab goes just right of the one on show, at the end with none
        Model::Window w;
        w.add(1, "/p/a");
        CHECK(w.tabs() == ids({1}) && w.active() == 0);
        w.activate(1);
        w.add(2, "/p/b");
        w.add(3, "/p/c");
        CHECK(w.tabs() == ids({1, 3, 2}));
        w.activate(2);
        w.add(4, "/p/d");
        CHECK(w.tabs() == ids({1, 3, 2, 4}) && w.active() == 2);
        w.append(5, "/p/e");
        w.insert(6, "/p/f", 1);
        w.insert(7, "/p/g", 99);   // clamped to the end
        CHECK(w.tabs() == ids({1, 6, 3, 2, 4, 5, 7}));
        w.add(1, "/p/a");          // already open: nothing
        CHECK(w.count() == 7 && w.find("/p/f") == 6 && w.find("/p/zz") == 0 && w.find("") == 0);
        CHECK(windowHolds(w));
    }

    GROUP("session:keys");
    {   // Ctrl+Tab wraps; 9 is the last tab; out of range selects nothing
        Model::Window w = windowWith(4);
        CHECK(w.step(1) == 2 && w.active() == 2);
        w.select(3);
        CHECK(w.step(1) == 1 && w.step(-1) == 4);
        CHECK(w.selectNumber(9) == 4 && w.selectNumber(2) == 2 && w.active() == 2);
        CHECK(w.selectNumber(5) == 0 && w.active() == 2);
        CHECK(w.select(-1) == 0 && w.select(4) == 0 && w.active() == 2);
        Model::Window none;
        CHECK(none.step(1) == 0 && none.selectNumber(1) == 0);
    }

    GROUP("session:close");
    {   // another tab closes: the one on show stays
        Model::Window w = windowWith(3);
        w.activate(3);
        Model::Window::Closed c = w.close(1, -1, nullptr);
        CHECK(c.closed && !c.stillOpen && !c.groupClosed && !c.activeChanged);
        CHECK(w.tabs() == ids({2, 3}) && w.active() == 3);
        CHECK(!w.close(1, -1, nullptr).closed);   // gone already
    }
    {   // the one on show closes: the one used most recently of the rest
        Model::Window w = windowWith(4);
        w.activate(3);
        w.activate(2);
        w.activate(4);
        Model::Window::Closed c = w.close(4, -1, nullptr);
        CHECK(c.activeChanged && w.active() == 2);
        w.close(2, -1, nullptr);
        CHECK(w.active() == 3);
        w.close(3, -1, nullptr);
        CHECK(w.active() == 1);
        w.close(1, -1, nullptr);
        CHECK(w.active() == 0 && w.tabs().empty() && w.documents().empty());
        CHECK(windowHolds(w));
    }
    {   // Close Other Tabs and Close Tabs to the Right name the right tabs
        Model::Window w = windowWith(4);
        CHECK(w.othersThan(2) == ids({1, 3, 4}) && w.rightOf(2) == ids({3, 4}));
        CHECK(w.rightOf(4).empty() && w.othersThan(9).empty() && w.rightOf(9).empty());
    }

    GROUP("session:labels");
    {   // names that clash take folders; a tab with no file is ""
        Model::Window w;
        w.append(1, "/p/src/main.cpp");
        w.append(2, "/p/tests/main.cpp");
        w.append(3, "");
        w.append(4, "/p/x.h");
        CHECK((w.labels(0) ==
               std::vector<std::string>{"src/main.cpp", "tests/main.cpp", "", "x.h"}));
        w.setPath(2, "/p/tests/other.cpp");   // a rename follows
        CHECK(w.labels(0)[0] == "main.cpp" && w.labels(0)[1] == "other.cpp");
        CHECK(w.labels(1).empty());
    }

    GROUP("session:order");
    {
        Model::Window w = windowWith(4);
        CHECK(w.move(0, 2) && w.tabs() == ids({2, 3, 1, 4}) && w.active() == 1);
        CHECK(w.move(3, 0) && w.tabs() == ids({4, 2, 3, 1}));
        CHECK(!w.move(4, 0) && !w.move(0, -1) && w.tabs() == ids({4, 2, 3, 1}));
        CHECK(!w.move(0, 1, 1));   // no second group
        w.splitEditor();           // {4,2,3,1} | {1}
        w.activate(3);             // joins the focused group: {1,3}
        w.putInOther(3);           // and is the left group's tab on show
        CHECK(w.focused() == 1 && w.tabs(1) == ids({1, 3}));
        CHECK(w.move(0, 3, 0) && w.tabs(0) == ids({2, 3, 1, 4}) && w.tabs(1) == ids({1, 3}));
    }

    GROUP("session:preview");
    const EditorTabs::PreviewSettings on;
    {   // a tree click opens in the preview tab, which the next replaces in place
        Model::Window w = windowWith(2);   // 1 on show
        Model::Window::Open o = w.prepareOpen("/p/a", OpenFrom::TreeClick, on);
        CHECK(o.inPreview && !o.existing);
        CHECK(w.add(10, "/p/a", Preview::User) == 0);
        w.activate(10);
        CHECK(w.tabs() == ids({1, 10, 2}) && w.previewTab() == 10);
        CHECK(w.add(11, "/p/b", Preview::User) == 10);   // replaced, on show: the new one is
        CHECK(w.tabs() == ids({1, 11, 2}) && w.active() == 11 && !w.isOpen(10));
        CHECK(w.preview(11) == Preview::User && w.preview(10) == Preview::None);
        // never in Reopen Closed Tab
        CHECK(!w.canReopen());
        // from another tab on show, the new preview goes right of that one
        w.activate(2);
        CHECK(w.add(12, "/p/c", Preview::User) == 11);
        CHECK(w.tabs() == ids({1, 2, 12}));
        CHECK(windowHolds(w));
    }
    {   // a preview tab with unsaved changes is kept instead of replaced
        Model::Window w = windowWith(1);
        w.add(10, "/p/a", Preview::User);
        w.activate(10);
        CHECK(w.add(11, "/p/b", Preview::User, [](DocId id) { return id == 10; }) == 0);
        CHECK(w.preview(10) == Preview::None && w.previewTab() == 11);
        CHECK(w.tabs() == ids({1, 10, 11}));
    }
    {   // an edit, a save, a double-click or Keep Open keeps it
        Model::Window w = windowWith(1);
        w.add(10, "/p/a", Preview::User);
        CHECK(w.keep(10) && !w.keep(10) && w.previewTab() == 0);
        CHECK(w.add(11, "/p/b", Preview::User) == 0 && w.isOpen(10));
    }
    {   // a kept open of the preview tab's file keeps it; an open in preview
        // leaves a kept tab kept
        Model::Window w = windowWith(1);
        w.add(10, "/p/a", Preview::User);
        Model::Window::Open o = w.prepareOpen("/p/a", OpenFrom::TreeClick, on);
        CHECK(o.existing == 10 && o.inPreview && w.preview(10) == Preview::User);
        o = w.prepareOpen("/p/a", OpenFrom::TreeDoubleClick, on);
        CHECK(o.existing == 10 && !o.inPreview && w.preview(10) == Preview::None);
        o = w.prepareOpen("/p/f1", OpenFrom::TreeClick, on);
        CHECK(o.existing == 1 && w.preview(1) == Preview::None);
    }
    {   // a code navigation keeps the preview tab it starts from
        Model::Window w = windowWith(1);
        w.add(10, "/p/a", Preview::User);
        w.activate(10);
        Model::Window::Open o = w.prepareOpen("/p/b", OpenFrom::CodeNavigation, on);
        CHECK(o.inPreview && w.preview(10) == Preview::None);
        w.add(11, "/p/b", Preview::User);
        w.activate(11);
        o = w.prepareOpen("/p/b", OpenFrom::CodeNavigation, on);   // to itself: stays
        CHECK(o.existing == 11 && w.preview(11) == Preview::User);
    }
    {   // Quick Open only with its setting; none at all with previews off
        Model::Window w = windowWith(1);
        CHECK(!w.prepareOpen("/p/a", OpenFrom::QuickOpen, on).inPreview);
        EditorTabs::PreviewSettings qo;
        qo.fromQuickOpen = true;
        CHECK(w.prepareOpen("/p/a", OpenFrom::QuickOpen, qo).inPreview);
        EditorTabs::PreviewSettings off;
        off.enabled = false;
        CHECK(!w.prepareOpen("/p/a", OpenFrom::TreeClick, off).inPreview);
        CHECK(w.prepareOpen("/p/a", OpenFrom::SearchClick, on).inPreview);
        CHECK(!w.prepareOpen("/p/a", OpenFrom::SearchOpen, on).inPreview);
    }
    {   // the agents' preview tab is its own: neither replaces the other
        Model::Window w = windowWith(1);
        w.add(10, "/p/a", Preview::User);
        w.activate(10);
        CHECK(w.add(20, "/p/n1", Preview::Agent) == 0);
        CHECK(w.tabs() == ids({1, 10, 20}) && w.active() == 10);   // added beside, not shown
        w.activate(1);
        // the next takes the old one's place, not a place beside the tab on show
        CHECK(w.add(21, "/p/n2", Preview::Agent) == 20 && w.isOpen(10));
        CHECK(w.tabs() == ids({1, 10, 21}));
        CHECK(w.previewTab(Preview::User) == 10 && w.previewTab(Preview::Agent) == 21);
        CHECK(w.add(11, "/p/b", Preview::User) == 10 && w.isOpen(21));
        // never the tab the user is typing in (the port's passOver says): it
        // stays a preview, and the next of its kind is the one replaced
        w.activate(21);
        CHECK(w.add(22, "/p/n3", Preview::Agent, nullptr,
                    [&w](DocId id) { return id == w.active(); }) == 0);
        CHECK(w.isOpen(21) && w.preview(21) == Preview::Agent && w.preview(22) == Preview::Agent);
        CHECK(w.add(23, "/p/n4", Preview::Agent, nullptr,
                    [&w](DocId id) { return id == w.active(); }) == 22);
        CHECK(w.isOpen(21) && w.preview(23) == Preview::Agent);
        CHECK(windowHolds(w));
    }
    {   // a note's preview tab and a diff's are apart: a diff after a note
        // leaves the note's tab where it was
        Model::Window w = windowWith(1);
        CHECK(w.add(20, "/p/n1", Preview::Agent) == 0);
        CHECK(w.add(30, "", Preview::AgentDiff) == 0);
        CHECK(w.isOpen(20) && w.isOpen(30));
        CHECK(w.add(31, "", Preview::AgentDiff) == 30 && w.isOpen(20));
        CHECK(w.previewTab(Preview::Agent) == 20 && w.previewTab(Preview::AgentDiff) == 31);
        CHECK(Model::isAgentPreview(w.preview(31)) && !Model::isAgentPreview(Preview::User));
        // a kept open of an agents' preview tab's file keeps it, as the user's
        Model::Window::Open o = w.prepareOpen("/p/n1", OpenFrom::TreeClick, on);
        CHECK(o.existing == 20 && w.preview(20) == Preview::Agent);
        o = w.prepareOpen("/p/n1", OpenFrom::Other, on);
        CHECK(o.existing == 20 && w.preview(20) == Preview::None);
        CHECK(windowHolds(w));
    }
    {   // each group has its own preview tab: one in the other group is not
        // replaced by a tree click here
        Model::Window w = windowWith(2);
        w.add(10, "/p/a", Preview::User);
        w.activate(10);
        CHECK(w.splitEditor() && w.focused() == 1);   // 10 in both groups
        w.focus(0);
        w.activate(1);
        w.focus(1);
        CHECK(w.previewTab(Preview::User) == 10);     // the right group has 10 too
        w.close(10, 0, nullptr);                      // the left group's 10 goes
        w.focus(0);
        CHECK(w.previewTab(Preview::User) == 0 && w.isOpen(10));
        CHECK(w.add(11, "/p/b", Preview::User) == 0);
        CHECK(w.isOpen(10) && w.preview(10) == Preview::User && w.preview(11) == Preview::User);
        CHECK(windowHolds(w));
    }
    {   // a replaced preview tab the other group also shows stays there, a
        // preview still; only the focused group's tab of it goes
        Model::Window w = windowWith(2);
        w.add(10, "/p/a", Preview::User);
        w.activate(10);
        CHECK(w.splitEditor() && w.focused() == 1 && w.inBothGroups(10));
        w.focus(0);
        CHECK(w.add(11, "/p/b", Preview::User) == 10);
        CHECK(w.active() == 11 && !w.inGroup(10, 0) && w.inGroup(10, 1));
        CHECK(w.preview(10) == Preview::User && w.active(1) == 10);
        CHECK(!w.canReopen());
        CHECK(windowHolds(w));
    }
    {   // the agents' replaced preview tab leaves both groups, and a group it
        // was the last of closes
        Model::Window w = windowWith(1);
        w.add(20, "/p/n1", Preview::Agent);
        w.activate(20);
        CHECK(w.moveActive(1) && w.split() && w.tabs(1) == ids({20}) && w.focused() == 1);
        w.focus(0);
        CHECK(w.add(21, "/p/n2", Preview::Agent) == 20);
        CHECK(!w.isOpen(20) && !w.split() && w.tabs() == ids({1, 21}));
        CHECK(windowHolds(w));
    }

    GROUP("session:reopen");
    {   // the newest first, at its old place, with the place the port gave
        Model::Window w = windowWith(4);
        EditorTabs::ClosedTab place;
        place.haveView = true;
        place.selStart = 3;
        place.selEnd = 7;
        place.path = "/ignored";   // the window's own path wins
        w.close(2, -1, &place);
        w.close(4, -1, &place);
        CHECK(w.closedCount() == 2);
        EditorTabs::ClosedTab c;
        CHECK(w.takeClosed(&c, nullptr) && c.path == "/p/f4" && c.index == 2);
        CHECK(EditorTabs::ClosedTabs::indexIn(c, w.count()) == 2);
        w.insert(14, c.path, EditorTabs::ClosedTabs::indexIn(c, w.count()));
        w.activate(14);
        CHECK(w.tabs() == ids({1, 3, 14}));
        // files gone are passed over
        CHECK(!w.takeClosed(&c, [](const std::string &) { return false; }) && !w.canReopen());
    }
    {   // a tab with no file is not remembered, nor one closed without asking
        Model::Window w = windowWith(1);
        w.add(5, "");
        EditorTabs::ClosedTab place;
        w.close(5, -1, &place);
        w.remove(1);
        CHECK(!w.canReopen());
    }
    {   // at most 50, the oldest dropped
        Model::Window w;
        EditorTabs::ClosedTab place;
        for (int i = 1; i <= 53; i++) {
            w.append((DocId)i, "/p/" + std::to_string(i));
            w.close((DocId)i, -1, &place);
        }
        CHECK(w.closedCount() == 50);
    }

    GROUP("session:groups");
    {   // Split Editor: the tab on show in a second group on the right, which
        // takes the keyboard; one document, a tab in each
        Model::Window w = windowWith(3);
        w.activate(2);
        CHECK(!w.split() && w.groupCount() == 1 && w.other() == -1);
        CHECK(w.splitEditor());
        CHECK(w.split() && w.focused() == 1 && w.other() == 0);
        CHECK(w.tabs(0) == ids({1, 2, 3}) && w.tabs(1) == ids({2}));
        CHECK(w.active(0) == 2 && w.active(1) == 2 && w.inBothGroups(2));
        CHECK(w.documents() == ids({2, 1, 3}));   // the focused group's first, each once
        // a second split sends the tab to the other group, not a third
        w.focus(0);
        w.activate(3);
        CHECK(w.splitEditor() && w.focused() == 1 && w.tabs(1) == ids({2, 3}) && w.active(1) == 3);
        CHECK(w.groupCount() == 2 && windowHolds(w));
        // an open of a file only the other group has joins this one
        w.focus(0);
        w.close(3, 0, nullptr);
        CHECK(!w.inGroup(3, 0) && w.inGroup(3, 1));
        CHECK(w.prepareOpen("/p/f3", OpenFrom::Other, on).existing == 3);
        w.activate(3);
        CHECK(w.inBothGroups(3) && w.active(0) == 3);
        CHECK(windowHolds(w));
    }
    {   // closing a tab the other group still has: not closed, not remembered
        Model::Window w = windowWith(2);
        w.splitEditor();   // 1 in both
        EditorTabs::ClosedTab place;
        Model::Window::Closed c = w.close(1, 0, &place);
        CHECK(c.closed && c.stillOpen && !c.groupClosed && !w.canReopen());
        CHECK(w.isOpen(1) && w.tabs(0) == ids({2}));
    }
    {   // the focused group's last tab: the group closes, the other is group 0
        Model::Window w = windowWith(2);
        w.activate(2);
        w.splitEditor();          // group 1: {2}, focused
        w.focus(0);
        w.activate(1);
        w.focus(1);
        w.activate(2);
        Model::Window::Closed c = w.close(2, -1, nullptr);
        CHECK(c.closed && c.stillOpen && c.groupClosed && c.activeChanged);
        CHECK(!w.split() && w.focused() == 0 && w.tabs() == ids({1, 2}) && w.active() == 1);
        // and from the left: the right group becomes group 0
        w.activate(2);
        w.splitEditor();          // {1,2} | {2}, focused 1
        w.focus(0);
        w.close(1, -1, nullptr);
        w.close(2, -1, nullptr);
        CHECK(!w.split() && w.focused() == 0 && w.tabs() == ids({2}) && w.active() == 2);
        CHECK(windowHolds(w));
    }
    {   // Move Editor into Next and Previous Group
        Model::Window w = windowWith(1);
        CHECK(!w.canMove(1) && !w.moveActive(1));   // alone in the only group
        w.append(2, "/p/f2");
        CHECK(!w.canMove(-1));                      // no group to the left
        CHECK(w.moveActive(1));
        CHECK(w.split() && w.focused() == 1 && w.tabs(0) == ids({2}) && w.tabs(1) == ids({1}));
        CHECK(w.active(0) == 2 && w.active(1) == 1 && !w.canMove(1));
        CHECK(w.moveActive(-1));   // its group's last: that group closes
        CHECK(!w.split() && w.tabs() == ids({2, 1}) && w.active() == 1);
        CHECK(windowHolds(w));
    }
    {   // the session's groups, and back
        Model::Window w = windowWith(3);
        w.add(9, "");   // a diff: no file to remember
        w.activate(3);
        w.splitEditor();
        w.setFraction(0.3);
        EditorTabs::GroupsSession s = w.groupsSession();
        CHECK((s.groups[0] == std::vector<std::string>{"/p/f1", "/p/f2", "/p/f3"}));
        CHECK((s.groups[1] == std::vector<std::string>{"/p/f3"}));
        CHECK(s.active[0] == "/p/f3" && s.active[1] == "/p/f3" && s.focused == 1 && s.fraction == 0.3);
        Model::Window back = windowWith(3);
        std::vector<std::string> open;
        for (DocId id : back.documents()) open.push_back(back.path(id));
        s.groups[1] = {"/p/f2"};
        s.active[1] = "/p/f2";
        const EditorTabs::GroupLayout layout = EditorTabs::restoreGroups(s, open);
        CHECK(back.restoreGroups(layout));
        CHECK(back.split() && back.focused() == 1 && back.tabs(1) == ids({2}));
        CHECK(back.active(1) == 2 && back.active(0) == 3 && back.fraction() == 0.3);
        CHECK(!back.restoreGroups(layout));   // split already
        CHECK(windowHolds(back));
        Model::Window plain = windowWith(2);
        CHECK(!plain.restoreGroups(EditorTabs::GroupLayout()) && !plain.split());
        plain.setFraction(7);
        CHECK(plain.fraction() == 1);
    }
    {   // the Trash takes a file out of both groups
        Model::Window w = windowWith(2);
        w.splitEditor();
        w.remove(1);
        CHECK(!w.split() && !w.isOpen(1) && w.tabs() == ids({2}) && w.active() == 2);
        CHECK(!w.canReopen() && w.path(1).empty());
        w.clear();
        CHECK(w.documents().empty() && w.count() == 0 && !w.split());
    }
    {   // the preview state is the document's, kept once for both groups
        Model::Window w = windowWith(1);
        w.add(10, "/p/a", Preview::User);
        w.activate(10);
        w.splitEditor();
        CHECK(w.preview(10) == Preview::User);
        // out of the focused group only: the left group's tab of it stays,
        // still a preview
        CHECK(w.add(11, "/p/b", Preview::User) == 10);
        CHECK(w.tabs(0) == ids({1, 10}) && w.tabs(1) == ids({11}) && windowHolds(w));
        CHECK(w.preview(10) == Preview::User && w.active(0) == 10);
    }

    GROUP("session:windows");
    {
        Model::Session s;
        const Model::Session::WindowId a = s.addWindow(), b = s.addWindow();
        Model::Window *wa = s.window(a);
        CHECK(a != b && wa && s.window(b) && s.windows().size() == 2);
        wa->append(1, "/p/x");
        s.window(b)->append(1, "/p/y");   // ids are the port's, per window
        CHECK(s.windowWith("/p/y") == b && s.windowWith("/p/x") == a && s.windowWith("/p/z") == 0);
        wa->setRoot("/p");
        s.window(b)->setRoot("/q");
        const Model::Session::WindowId c = s.addWindow();
        s.window(c)->setRoot("/p");
        CHECK((s.windowsOn("/p") == std::vector<Model::Session::WindowId>{a, c}));
        CHECK(s.windowsOn("/r").empty() && s.window(b)->root() == "/q");
        s.removeWindow(c);
        for (int i = 0; i < 20; i++) s.addWindow();   // the first's Window stays put
        CHECK(s.window(a) == wa);
        s.removeWindow(a);
        CHECK(!s.window(a) && s.windows().size() == 21 && s.windowWith("/p/x") == 0);
    }

    GROUP("session:walk");
    {   // random opens, closes, keys, moves, splits and previews
        std::mt19937 rng(20261116);
        for (int round = 0; round < 40; round++) {
            Model::Window w;
            DocId next = 1;
            bool ok = true;
            for (int k = 0; k < 400 && ok; k++) {
                const std::vector<DocId> all = w.documents();
                const DocId some = all.empty() ? 0 : all[rng() % all.size()];
                switch (rng() % 12) {
                case 0: case 1: {
                    const Preview p = (Preview)(rng() % 4);
                    const bool pass = rng() % 4 == 0;
                    w.add(next, "/p/" + std::to_string(next % 17), p,
                          [&](DocId id) { return id % 3 == 0; },
                          [&](DocId id) { return pass && id == w.active(); });
                    if (rng() % 2) w.activate(next);
                    next++;
                    break;
                }
                case 2: w.activate(some); break;
                case 3: w.step(rng() % 2 ? 1 : -1); break;
                case 4: {
                    EditorTabs::ClosedTab place;
                    w.close(some, (int)(rng() % 3) - 1, &place);
                    break;
                }
                case 5: w.splitEditor(); break;
                case 6: w.moveActive(rng() % 2 ? 1 : -1); break;
                case 7: w.focus((int)(rng() % 2)); break;
                case 8: w.move((int)(rng() % 6), (int)(rng() % 6)); break;
                case 9: w.remove(some); break;
                case 10: w.keep(some); break;
                case 11: {
                    EditorTabs::ClosedTab c;
                    if (w.takeClosed(&c, nullptr)) {
                        w.insert(next, c.path, EditorTabs::ClosedTabs::indexIn(c, w.count()));
                        w.activate(next++);
                    }
                    break;
                }
                }
                // (a group may hold two preview tabs of a kind: one the
                // other group's that came over, one passed over)
                ok = windowHolds(w) && w.previewTab(Preview::None) == 0;
            }
            CHECK(ok);
        }
    }
}
