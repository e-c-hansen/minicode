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
        // ...unless the tree was focused on a folder there (Focus on
        // Folder), which comes back with the folder; it reads back too.
        Session focused;
        focused.treeFocus = "mac/src";
        put(all, "/new", focused, 3001);
        CHECK(all.count("/new") && all.at("/new").treeFocus == "mac/src");
        Sessions back = decode(encode(all));
        CHECK(back.count("/new") && back.at("/new").treeFocus == "mac/src");
        CHECK(back.at("/f5").treeFocus.empty());
        CHECK(encode(Sessions{{"/f", one}}).find("treeFocus") == std::string::npos);
        put(all, "/new", Session(), 3002);
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

    GROUP("tab-sessions:windows");
    {
        // Two windows, front first, with all a window brings back.
        Window a;
        a.root = "/p";
        a.haveFrame = true;
        a.frame = Frame{120.5, 80, 1400, 900};
        a.tabs.tabs = {tab("/p/a.txt", 3, 7, true, 120, 4), tab("/p/b.md", 0, 0, false, 0, 0, true)};
        a.tabs.tabs[1].side = true;
        a.tabs.active = 1;
        a.tabs.preview = 0;
        a.tabs.split = true;
        a.tabs.groups.groups[0] = {"/p/a.txt"};
        a.tabs.groups.groups[1] = {"/p/b.md"};
        a.tabs.groups.active[0] = "/p/a.txt";
        a.tabs.groups.active[1] = "/p/b.md";
        a.tabs.groups.focused = 1;
        a.tabs.groups.fraction = 0.4;
        a.sidebar = "git";
        a.sidebarWidth = 310;
        a.problems = true;
        a.terminal = true;
        a.terminalHeight = 260;
        TerminalTab t;
        t.columns = {{{"/p", ""}, {"/p/src", "server"}}, {{"/tmp", ""}}};
        t.layout.columns = {0.6, 0.4};
        t.layout.rows = {{0.5, 0.5}, {1.0}};
        t.activeColumn = 0;
        t.activeRow = 1;
        a.terminals = {t, TerminalTab()};
        a.terminals[1].columns = {{{"/p/docs", ""}}};
        a.terminalTab = 1;
        a.expanded = {"/p/src", "/p/src/x"};
        a.selected = "/p/src/x/y.c";
        a.treeScroll = 42;
        Window b;
        b.root = "/q";
        b.sidebarHidden = true;
        b.editorHidden = true;
        b.fullScreen = true;
        const std::vector<Window> back = decodeWindows(encodeWindows({a, b}));
        CHECK(back.size() == 2);
        const Window &w = back[0];
        CHECK(w.root == "/p" && w.haveFrame && w.frame.x == 120.5 && w.frame.y == 80 &&
              w.frame.w == 1400 && w.frame.h == 900 && !w.fullScreen);
        CHECK(w.tabs.tabs.size() == 2 && w.tabs.active == 1 && w.tabs.preview == 0);
        CHECK(w.tabs.tabs[0].selStart == 3 && w.tabs.tabs[0].selEnd == 7 &&
              w.tabs.tabs[0].haveView && w.tabs.tabs[0].top == 120 && w.tabs.tabs[0].into == 4);
        CHECK(!w.tabs.tabs[0].side && w.tabs.tabs[1].side && w.tabs.tabs[1].page);
        CHECK(w.tabs.split && w.tabs.groups.groups[1].size() == 1 && w.tabs.groups.focused == 1 &&
              w.tabs.groups.fraction == 0.4 && w.tabs.groups.active[1] == "/p/b.md");
        CHECK(w.sidebar == "git" && w.sidebarWidth == 310 && !w.sidebarHidden);
        CHECK(w.problems && !w.editorHidden);
        CHECK(w.terminal && w.terminalHeight == 260 && w.terminalTab == 1);
        CHECK(w.terminals.size() == 2);
        CHECK(w.terminals[0].columns.size() == 2 && w.terminals[0].columns[0].size() == 2);
        CHECK(w.terminals[0].columns[0][1].cwd == "/p/src" &&
              w.terminals[0].columns[0][1].title == "server");
        CHECK(w.terminals[0].layout.columns.size() == 2 &&
              std::fabs(w.terminals[0].layout.columns[0] - 0.6) < 1e-9);
        CHECK(w.terminals[0].layout.rows[0].size() == 2 && w.terminals[0].layout.rows[1].size() == 1);
        CHECK(w.terminals[0].activeColumn == 0 && w.terminals[0].activeRow == 1);
        CHECK(w.terminals[1].columns[0][0].cwd == "/p/docs");
        CHECK(w.expanded.size() == 2 && w.expanded[1] == "/p/src/x");
        CHECK(w.selected == "/p/src/x/y.c" && w.treeScroll == 42);
        CHECK(back[1].root == "/q" && !back[1].haveFrame && back[1].sidebar == "tree");
        CHECK(back[1].sidebarHidden && back[1].editorHidden && back[1].fullScreen);
        CHECK(back[1].tabs.tabs.empty() && back[1].terminals.empty() && !back[1].terminal);

        // Damaged: none, never a half-read list; a bad part is left out.
        CHECK(decodeWindows("").empty());
        CHECK(decodeWindows("[1,2]").empty());
        CHECK(decodeWindows("{\"windows\": [").empty());
        const std::vector<Window> odd = decodeWindows(
            "{\"windows\":[{\"root\":\"\"},7,{\"root\":\"/r\",\"frame\":[1,2,-3,4],"
            "\"sidebar\":\"nonsense\",\"sidebarWidth\":-5,\"terminal\":true,"
            "\"terminals\":[{\"columns\":[[{\"cwd\":\"/r\"}],[]],\"columnShares\":[0.9,0.9],"
            "\"activeColumn\":5}],\"terminalTab\":9,\"expanded\":[\"/r/a\",3],"
            "\"tabs\":\"x\"}]}");
        CHECK(odd.size() == 1);
        CHECK(odd[0].root == "/r" && !odd[0].haveFrame && odd[0].sidebar == "tree");
        CHECK(odd[0].sidebarWidth == 0 && odd[0].tabs.tabs.empty());
        CHECK(odd[0].terminals.size() == 1 && odd[0].terminalTab == 0 && odd[0].terminal);
        CHECK(odd[0].terminals[0].columns.size() == 1 &&
              odd[0].terminals[0].layout.columns.size() == 1 &&
              odd[0].terminals[0].activeColumn == 0);
        CHECK(odd[0].expanded.size() == 1);
    }

    GROUP("tab-sessions:terminal-tab");
    {
        // Shares that do not match the panes are made even; empty columns go.
        TerminalTab t;
        t.columns = {{{"/a", ""}}, {}, {{"/b", ""}, {"/c", ""}}};
        t.layout.columns = {0.2, 0.3, 0.5};
        t.layout.rows = {{1.0}, {1.0}, {0.5, 0.5}};
        t.activeColumn = 2;
        t.activeRow = 7;
        const TerminalTab s = sanitize(t);
        CHECK(s.columns.size() == 2 && s.layout.columns.size() == 2);
        CHECK(std::fabs(s.layout.columns[0] - 0.5) < 1e-9);
        CHECK(s.layout.rows[1].size() == 2);
        CHECK(s.activeColumn == 0 && s.activeRow == 0);
        // Nothing at all: one pane in the window's folder ("").
        const TerminalTab none = sanitize(TerminalTab());
        CHECK(none.columns.size() == 1 && none.columns[0].size() == 1 &&
              none.columns[0][0].cwd.empty());
        // Shares as written are kept when they match.
        TerminalTab ok;
        ok.columns = {{{"/a", ""}, {"/b", ""}}};
        ok.layout.columns = {1.0};
        ok.layout.rows = {{0.7, 0.3}};
        CHECK(sanitize(ok).layout.rows[0][0] == 0.7);
    }

    GROUP("tab-sessions:mac-browser");
    {
        // The Mac's browser pane in a folder's session: each tab's record
        // and its form, whether the pane showed and its share.
        Sessions all;
        Session s;
        s.tabs = {tab("/p/a.txt", 0, 0, false, 0, 0)};
        BrowserTab b;
        b.url = "https://example.com/";
        b.title = "Example";
        b.state = "AAEC";
        b.archived = true;
        BrowserTab plain;
        plain.url = "http://localhost:5173/";
        s.browser = {b, plain};
        s.browserActive = 1;
        s.browserShown = true;
        s.browserFraction = 0.4;
        put(all, "/p", s, 1);
        const Session back = decode(encode(all)).at("/p");
        CHECK(back.browser.size() == 2 && back.browserActive == 1);
        CHECK(back.browser[0].archived && back.browser[0].state == "AAEC");
        CHECK(!back.browser[1].archived && back.browser[1].state.empty());
        CHECK(back.browserShown && back.browserFraction == 0.4);
        // A session from before (GTK's, or with no pane) reads as hidden.
        const Sessions old = decode(R"({"/q": {"tabs": [], "browser": [{"url": "https://a/"}]}})");
        CHECK(old.count("/q") && !old.at("/q").browserShown && old.at("/q").browserFraction == 0);
        CHECK(!old.at("/q").browser[0].archived);
    }

    GROUP("tab-sessions:store");
    {
        // sessions.json with its folders and the windows beside them.
        Store st;
        Session s;
        s.tabs = {tab("/p/a.txt", 3, 7, true, 120, 4)};
        s.active = 0;
        put(st.folders, "/p", s, 1762700000.5);
        Window w;
        w.root = "/p";
        w.haveFrame = true;
        w.frame = Frame{0, 0, 1200, 800};
        w.tabs.tabs = {tab("/p/a.txt", 3, 7, true, 120, 4), tab("/p/b.txt", 1, 1, false, 0, 0)};
        w.tabs.active = 1;
        w.sidebarHidden = true;
        w.terminal = true;
        w.terminals = {TerminalTab()};
        w.terminals[0].columns = {{{"/p", ""}}};
        w.terminalTab = 0;
        Window v;
        v.root = "/q";
        st.windows = {w, v};
        const std::string text = encodeStore(st);
        const Store back = decodeStore(text);
        CHECK(back.folders.size() == 1 && back.folders.count("/p") == 1);
        CHECK(back.folders.at("/p").tabs.size() == 1);
        CHECK(back.windows.size() == 2);
        CHECK(back.windows[0].root == "/p" && back.windows[1].root == "/q");
        CHECK(back.windows[0].tabs.tabs.size() == 2 && back.windows[0].tabs.active == 1);
        CHECK(back.windows[0].tabs.tabs[0].selStart == 3 && back.windows[0].tabs.tabs[0].top == 120);
        CHECK(back.windows[0].haveFrame && back.windows[0].frame.w == 1200);
        CHECK(back.windows[0].sidebarHidden && back.windows[0].terminal);
        // The file is the Mac's window record too: decodeWindows reads its
        // windows, and decode (a minicode from before) only its folders,
        // never "windows" as a folder.
        CHECK(decodeWindows(text).size() == 2);
        const Sessions old = decode(text);
        CHECK(old.size() == 1 && old.count("/p") == 1 && old.count(kWindowsKey) == 0);
        // A sessions.json from before: its folders, no window.
        const Store before = decodeStore(encode(st.folders));
        CHECK(before.folders.size() == 1 && before.windows.empty());
        // The Mac's MCOpenWindows record: its windows, no folder.
        const Store mac = decodeStore(encodeWindows(st.windows));
        CHECK(mac.folders.empty() && mac.windows.size() == 2);
        // No windows: no key at all, so a minicode from before reads the file
        // as it always did.
        Store none;
        none.folders = st.folders;
        CHECK(encodeStore(none).find("\"windows\"") == std::string::npos);
        CHECK(encodeStore(none) == encode(st.folders));
        // Nothing can be saved as a folder named "windows".
        Sessions all;
        put(all, kWindowsKey, s, 1);
        CHECK(all.empty());
        // Not JSON: nothing at all.
        const Store bad = decodeStore("{\"/p\": {\"tabs\": [");
        CHECK(bad.folders.empty() && bad.windows.empty());
        // Windows that do not read are left out, the folders kept.
        const Store mixed = decodeStore(
            "{\"/p\": {\"tabs\": [{\"path\": \"/p/a\"}]}, \"windows\": [7, {\"root\": \"\"}, "
            "{\"root\": \"/r\"}]}");
        CHECK(mixed.folders.size() == 1 && mixed.windows.size() == 1 && mixed.windows[0].root == "/r");
    }

    GROUP("tab-sessions:after-closing");
    {
        Window a, b;
        a.root = "/a";
        b.root = "/b";
        // A window closing while another stays: dropped.
        std::vector<Window> left = afterClosing({a, b}, 0);
        CHECK(left.size() == 1 && left[0].root == "/b");
        // The last one: kept, so the next launch brings it back.
        left = afterClosing({b}, 0);
        CHECK(left.size() == 1 && left[0].root == "/b");
        CHECK(afterClosing({a, b}, 5).size() == 2);
        CHECK(afterClosing({}, 0).empty());
    }

    // Two minicodes at once share the record (a build beside the Homebrew
    // copy, `minicode -n`, the GUI checks' parent and its child apps): each
    // writes its own windows over its own, never over the other's.
    GROUP("tab-sessions:windows-of-several");
    {
        struct Running {
            std::vector<Process> live;
        } running;
        auto isRunning = [](long pid, double start, void *context) {
            for (const Process &p : static_cast<Running *>(context)->live)
                if (sameProcess(pid, start, p)) return true;
            return false;
        };
        auto win = [](const std::string &root, long pid, double start, double saved) {
            Window w;
            w.root = root;
            w.ownerPid = pid;
            w.ownerStart = start;
            w.saved = saved;
            return w;
        };
        auto roots = [](const std::vector<Window> &ws) {
            std::string out;
            for (const Window &w : ws) out += w.root + " ";
            return out;
        };
        const Process me{100, 5000}, other{200, 4000}, last{300, 1000};
        Window mine;
        mine.root = "/mine";
        // One minicode, as always: its windows replace the last run's, the
        // untagged ones of minicode 1.5.31 and before too, stamped as its.
        std::vector<Window> out = mergeWindows(
            {mine}, {win("/last", last.pid, last.start, 4900), win("/old", 0, 0, 0)}, me, 5100,
            isRunning, &running);
        CHECK(roots(out) == "/mine ");
        CHECK(out.size() == 1 && out[0].ownerPid == 100 && out[0].ownerStart == 5000 &&
              out[0].saved == 5100);
        // It reads back with its owner.
        const std::vector<Window> back = decodeWindows(encodeWindows(out));
        CHECK(back.size() == 1 && back[0].ownerPid == 100 && back[0].ownerStart == 5000 &&
              back[0].saved == 5100);
        // Its own earlier windows go too (a window closed since).
        out = mergeWindows({mine}, {win("/closed", 100, 5000.4, 5050)}, me, 5100, isRunning, &running);
        CHECK(roots(out) == "/mine ");
        // Another running minicode's windows stay, after this one's, even
        // written before this one started.
        running.live = {other};
        out = mergeWindows({mine}, {win("/other", other.pid, other.start, 4500)}, me, 5100, isRunning,
                           &running);
        CHECK(roots(out) == "/mine /other ");
        // One that quit while this one ran keeps what it left: four
        // windows quit with stay when this one writes three seconds later.
        running.live = {};
        out = mergeWindows({mine},
                           {win("/q1", other.pid, other.start, 5200), win("/q2", other.pid, other.start, 5200)},
                           me, 5203, isRunning, &running);
        CHECK(roots(out) == "/mine /q1 /q2 ");
        // A pid reused by a later process is not the one that wrote it.
        running.live = {Process{200, 4800}};
        out = mergeWindows({mine}, {win("/other", other.pid, other.start, 4500)}, me, 5100, isRunning,
                           &running);
        CHECK(roots(out) == "/mine ");
        // Nothing of this one's (no window yet): the others' as they were.
        running.live = {other};
        out = mergeWindows({}, {win("/other", other.pid, other.start, 4500)}, me, 5100, isRunning,
                           &running);
        CHECK(roots(out) == "/other ");

        // A launch with nothing to open brings back all but what another
        // running minicode holds open.
        running.live = {other};
        const std::vector<Window> stored = {win("/other", other.pid, other.start, 4500),
                                            win("/last", last.pid, last.start, 900),
                                            win("/old", 0, 0, 0), win("/self", 100, 5000, 5001)};
        CHECK(roots(toRestore(stored, me, isRunning, &running)) == "/last /old /self ");
        running.live = {};
        CHECK(roots(toRestore(stored, me, isRunning, &running)) == "/other /last /old /self ");
    }

    GROUP("tab-sessions:fit-frame");
    {
        const std::vector<Frame> one = {Frame{0, 0, 1440, 875}};
        // On the screen: where it was.
        Frame f = fitFrame(Frame{100, 50, 1100, 720}, one, 640, 400);
        CHECK(f.x == 100 && f.y == 50 && f.w == 1100 && f.h == 720);
        // Hanging off the right edge: moved on, same size.
        f = fitFrame(Frame{1000, 50, 1100, 720}, one, 640, 400);
        CHECK(f.x == 340 && f.y == 50 && f.w == 1100 && f.h == 720);
        // Taller than the screen: shrunk to it.
        f = fitFrame(Frame{0, -100, 800, 2000}, one, 640, 400);
        CHECK(f.h == 875 && f.y == 0 && f.w == 800);
        // Under the minimum size: grown to it.
        f = fitFrame(Frame{10, 10, 100, 100}, one, 640, 400);
        CHECK(f.w == 640 && f.h == 400 && f.x == 10 && f.y == 10);
        // A second screen to the right, where the window was.
        const std::vector<Frame> two = {Frame{0, 0, 1440, 875}, Frame{1440, -200, 2560, 1415}};
        f = fitFrame(Frame{2000, 100, 1600, 1000}, two, 640, 400);
        CHECK(f.x == 2000 && f.y == 100 && f.w == 1600 && f.h == 1000);
        // That screen unplugged: in the middle of the main one, fitted.
        f = fitFrame(Frame{2000, 100, 1600, 1000}, one, 640, 400);
        CHECK(f.w == 1440 && f.h == 875 && f.x == 0 && f.y == 0);
        f = fitFrame(Frame{3000, 100, 1000, 600}, one, 640, 400);
        CHECK(f.w == 1000 && f.h == 600 && f.x == 220 && f.y == 137.5);
        // Mostly on the second screen: onto it, not the main one.
        f = fitFrame(Frame{1300, 100, 1000, 600}, two, 640, 400);
        CHECK(f.x == 1440 && f.y == 100);
        // No screens known: as saved.
        f = fitFrame(Frame{5, 6, 7, 8}, {}, 640, 400);
        CHECK(f.x == 5 && f.w == 7);
    }
}
