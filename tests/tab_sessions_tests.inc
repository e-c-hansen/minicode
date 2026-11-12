// tab_sessions_tests.inc — the `tab-sessions:*` group of run_tests.cpp
// (TabSessions.h, the GTK port's sessions file), kept in its own file and
// included there.

static void testTabSessions() {
    using namespace TabSessions;

    auto tab = [](const std::string &path, long a, long b, bool view, long top, long into,
                  bool page = false) {
        Tab t;
        t.path = path;
        t.selStart = a;
        t.selEnd = b;
        t.haveView = view;
        t.top = top;
        t.into = into;
        t.page = page;
        return t;
    };

    GROUP("tab-sessions:round-trip");
    {
        Sessions all;
        Session s;
        s.tabs = {tab("/p/a.txt", 3, 7, true, 120, 4), tab("/p/b.md", 0, 0, false, 0, 0, true),
                  tab("/p/ünï code/c d.txt", 5, 5, true, 0, -6)};
        s.active = 1;
        s.preview = 2;
        put(all, "/p", s, 1762700000.25);
        Session q;
        q.tabs = {tab("/q/x\"y\\z.txt", 1, 1, false, 0, 0)};
        q.active = 0;
        put(all, "/q", q, 1762700001.5);
        const Sessions back = decode(encode(all));
        CHECK(back.size() == 2);
        const Session &p = back.at("/p");
        CHECK(p.tabs.size() == 3);
        CHECK(p.active == 1 && p.preview == 2);
        CHECK(p.used == 1762700000.25);
        CHECK(p.tabs[0].path == "/p/a.txt" && p.tabs[0].selStart == 3 && p.tabs[0].selEnd == 7);
        CHECK(p.tabs[0].haveView && p.tabs[0].top == 120 && p.tabs[0].into == 4);
        CHECK(!p.tabs[0].page && p.tabs[1].page);
        CHECK(!p.tabs[1].haveView);
        CHECK(p.tabs[2].path == "/p/ünï code/c d.txt" && p.tabs[2].into == -6);
        CHECK(back.at("/q").tabs[0].path == "/q/x\"y\\z.txt");
        CHECK(back.at("/q").preview == -1);
        // Where a view was, as Reopen Closed Tab keeps it.
        const EditorTabs::ClosedTab c = place(p.tabs[0]);
        CHECK(c.path == "/p/a.txt" && c.selStart == 3 && c.selEnd == 7 && c.haveView &&
              c.top == 120 && c.topOffset == 4 && !c.preview);
        CHECK(place(p.tabs[1]).preview && !place(p.tabs[1]).haveView);
        const EditorTabs::Session f = files(p);
        CHECK(f.files.size() == 3 && f.files[1] == "/p/b.md" && f.active == 1 && f.preview == 2);
    }

    GROUP("tab-sessions:damaged");
    {
        // Not JSON, or not an object: nothing, never a half-read file.
        CHECK(decode("").empty());
        CHECK(decode("{\"/p\": {\"tabs\": [").empty());
        CHECK(decode("[1, 2]").empty());
        CHECK(decode("\"/p\"").empty());
        // A folder whose entry does not read is left out; the others stay.
        const Sessions s = decode(
            "{\"/bad\": 3, \"/nothing\": {\"tabs\": []}, \"/notabs\": {\"active\": 0},"
            " \"\": {\"tabs\": [{\"path\": \"/x\"}]},"
            " \"/p\": {\"used\": \"soon\", \"active\": 2, \"preview\": 0,"
            "   \"tabs\": [{\"path\": \"/p/a\"}, 7, {\"path\": \"\"}, {\"path\": \"/p/b\","
            "   \"sel\": [1, -2], \"top\": 1.5, \"page\": \"yes\"}, {\"path\": \"/p/c\","
            "   \"sel\": [4], \"top\": 9, \"into\": \"far\"}]}}");
        CHECK(s.size() == 1);
        const Session &p = s.at("/p");
        CHECK(p.tabs.size() == 3);
        CHECK(p.tabs[0].path == "/p/a" && p.tabs[1].path == "/p/b" && p.tabs[2].path == "/p/c");
        // The active tab named one that did not read: none is on show.
        CHECK(p.active == -1);
        CHECK(p.preview == 0);
        CHECK(p.used == 0);
        // A selection or a top that does not read is left out, not guessed.
        CHECK(p.tabs[1].selStart == 0 && p.tabs[1].selEnd == 0 && !p.tabs[1].haveView);
        CHECK(!p.tabs[1].page);
        CHECK(p.tabs[2].haveView && p.tabs[2].top == 9 && p.tabs[2].into == 0);
        CHECK(p.tabs[2].selStart == 0);
        // Indices past the tabs, or not whole, are none.
        const Sessions r = decode("{\"/r\": {\"active\": 5, \"preview\": 0.5,"
                                  " \"tabs\": [{\"path\": \"/r/a\"}]}}");
        CHECK(r.at("/r").active == -1 && r.at("/r").preview == -1);
        // Indices after a tab that did not read move with the tabs kept.
        const Sessions m = decode("{\"/m\": {\"active\": 2, \"tabs\": [{\"path\": \"/m/a\"},"
                                  " {\"nopath\": 1}, {\"path\": \"/m/c\"}]}}");
        CHECK(m.at("/m").active == 1 && m.at("/m").tabs[1].path == "/m/c");
    }

    GROUP("tab-sessions:put");
    {
        Sessions all;
        Session one;
        one.tabs = {tab("/a/x", 0, 0, false, 0, 0)};
        one.active = 0;
        // Past 50 folders, the ones written longest ago go, never the one
        // being written.
        for (int i = 0; i < 55; i++) put(all, "/f" + std::to_string(i), one, 1000.0 + i);
        CHECK(all.size() == kMaxFolders);
        CHECK(!all.count("/f0") && !all.count("/f4") && all.count("/f5") && all.count("/f54"));
        // Written again, a folder is the newest.
        put(all, "/f5", one, 2000);
        put(all, "/new", one, 2001);
        CHECK(all.count("/f5") && !all.count("/f6") && all.count("/new"));
        CHECK(all.at("/new").used == 2001);
        // One even older than every other is still kept while it is the one
        // being written.
        put(all, "/old", one, 1.0);
        CHECK(all.count("/old") && all.size() == kMaxFolders);
        // A session with no tab takes the folder out.
        put(all, "/new", Session(), 3000);
        CHECK(!all.count("/new"));
        // Indices past the tabs are dropped.
        Session bad = one;
        bad.active = 3;
        bad.preview = 1;
        put(all, "/bad", bad, 4000);
        CHECK(all.at("/bad").active == -1 && all.at("/bad").preview == -1);
        // No key, nothing.
        const size_t n = all.size();
        put(all, "", one, 5000);
        CHECK(all.size() == n);
    }

    GROUP("tab-sessions:merged");
    {
        // This window's tabs first, then the other windows' not already
        // there, in their order; the tab on show and the preview tab stay
        // this window's.
        Session mine;
        mine.tabs = {tab("/p/a", 1, 1, false, 0, 0), tab("/p/b", 0, 0, false, 0, 0)};
        mine.active = 1;
        mine.preview = 0;
        Session w2;
        w2.tabs = {tab("/p/b", 9, 9, true, 50, 0), tab("/p/c", 2, 2, false, 0, 0)};
        w2.active = 0;
        Session w3;
        w3.tabs = {tab("/p/d", 0, 0, false, 0, 0), tab("/p/c", 0, 0, false, 0, 0),
                   tab("/p/a", 0, 0, false, 0, 0)};
        const Session m = merged(mine, {w2, w3});
        CHECK(m.tabs.size() == 4);
        CHECK(m.tabs[0].path == "/p/a" && m.tabs[1].path == "/p/b" && m.tabs[2].path == "/p/c" &&
              m.tabs[3].path == "/p/d");
        // A file this window has keeps this window's place for it.
        CHECK(m.tabs[1].selStart == 0 && !m.tabs[1].haveView);
        CHECK(m.tabs[2].selStart == 2);
        CHECK(m.active == 1 && m.preview == 0);
        // Alone, it is itself.
        const Session alone = merged(mine, {});
        CHECK(alone.tabs.size() == 2 && alone.active == 1);
        // A window with no tab of its own still keeps the others'.
        const Session empty = merged(Session(), {w2});
        CHECK(empty.tabs.size() == 2 && empty.active == -1 && empty.preview == -1);
    }

    // The split editor's groups (PARITY.md 4.6): written only when split,
    // read back as written, and an old or damaged record is one group.
    GROUP("tab-sessions:groups");
    {
        Sessions all;
        Session s;
        s.tabs = {tab("/p/a.txt", 0, 0, false, 0, 0), tab("/p/b.txt", 0, 0, false, 0, 0)};
        s.active = 0;
        s.split = true;
        s.groups.groups[0] = {"/p/a.txt", "/p/b.txt"};
        s.groups.groups[1] = {"/p/b.txt"};
        s.groups.active[0] = "/p/a.txt";
        s.groups.active[1] = "/p/b.txt";
        s.groups.focused = 1;
        s.groups.fraction = 0.3;
        put(all, "/p", s, 1762700000);
        const std::string text = encode(all);
        CHECK(text.find("\"groups\"") != std::string::npos);
        const Session q = decode(text)["/p"];
        CHECK(q.split);
        CHECK(q.groups.groups[0] == s.groups.groups[0] && q.groups.groups[1] == s.groups.groups[1]);
        CHECK(q.groups.active[0] == "/p/a.txt" && q.groups.active[1] == "/p/b.txt");
        CHECK(q.groups.focused == 1 && std::fabs(q.groups.fraction - 0.3) < 1e-9);

        // Not split: nothing written, and none read.
        Sessions one;
        Session plain = s;
        plain.split = false;
        put(one, "/p", plain, 1762700000);
        CHECK(encode(one).find("\"groups\"") == std::string::npos);
        CHECK(!decode(encode(one))["/p"].split);

        // A group left empty, a bad share or a wrong type: one group.
        const Sessions bad = decode(
            "{\"/p\":{\"tabs\":[{\"path\":\"/p/a.txt\"}],\"groups\":{\"left\":[\"/p/a.txt\"],"
            "\"right\":[]}},\"/q\":{\"tabs\":[{\"path\":\"/q/a.txt\"}],\"groups\":{"
            "\"left\":[\"/q/a.txt\"],\"right\":[\"/q/a.txt\",7],\"focused\":3,\"fraction\":9}},"
            "\"/r\":{\"tabs\":[{\"path\":\"/r/a.txt\"}],\"groups\":\"x\"}}");
        CHECK(!bad.at("/p").split && bad.at("/p").groups.groups[0].empty());
        CHECK(bad.at("/q").split && bad.at("/q").groups.groups[1].size() == 1);
        CHECK(bad.at("/q").groups.focused == 0 && bad.at("/q").groups.fraction == 0.5);
        CHECK(!bad.at("/r").split);
    }
}
