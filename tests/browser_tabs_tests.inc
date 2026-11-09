// browser_tabs_tests.inc — the `browsertabs:*` groups of run_tests.cpp: the
// browser pane's tabs (BrowserTabs.h): which keep a live page, what each
// is called, and which records a session keeps.

void testBrowserTabs() {
    using namespace BrowserTabs;

    GROUP("browsertabs:live");
    {
        // The one on show and the one used just before it; the rest go.
        std::vector<bool> live = livePages({5, 9, 7, 1}, 0, kLivePages);
        CHECK(live.size() == 4);
        CHECK(live[0] && live[1] && !live[2] && !live[3]);
        // The active tab is live even when its stamp is the oldest.
        live = livePages({1, 9, 7, 8}, 0, 2);
        CHECK(live[0] && live[1] && !live[2] && !live[3]);
        // Ties go to the tab further left.
        live = livePages({0, 3, 3, 3}, 0, 2);
        CHECK(live[0] && live[1] && !live[2] && !live[3]);
        // One live page: only the active one; none, or no tabs, nothing.
        live = livePages({4, 5, 6}, 1, 1);
        CHECK(!live[0] && live[1] && !live[2]);
        live = livePages({4, 5, 6}, 1, 0);
        CHECK(!live[0] && !live[1] && !live[2]);
        CHECK(livePages({}, -1, 2).empty());
        // No active tab (the agent page on show): the two used last.
        live = livePages({4, 5, 6}, -1, 2);
        CHECK(!live[0] && live[1] && live[2]);
        // More room than tabs: all of them.
        live = livePages({1, 2}, 0, 5);
        CHECK(live[0] && live[1]);
        // Switching between two tabs back and forth keeps both, and a third
        // shown pushes out the one used least recently.
        std::vector<uint64_t> used = {1, 2, 0};
        live = livePages(used, 1, 2);
        CHECK(live[0] && live[1] && !live[2]);
        used[2] = 3;
        live = livePages(used, 2, 2);
        CHECK(!live[0] && live[1] && live[2]);
    }

    GROUP("browsertabs:title");
    {
        CHECK(tabTitle("GitHub", "https://github.com/") == "GitHub");
        CHECK(tabTitle("  Two\n  lines\t here ", "") == "Two lines here");
        CHECK(tabTitle("", "https://www.example.com/") == "example.com");
        CHECK(tabTitle("", "http://localhost:5173/") == "localhost:5173");
        CHECK(tabTitle("", "https://a.org/docs/x?y=1") == "a.org/docs/x?y=1");
        CHECK(tabTitle("", "file:///Users/me/site/index.html") == "index.html");
        CHECK(tabTitle("", "file:///Users/me/site/") == "site");
        CHECK(tabTitle("", "file:///") == "/");
        CHECK(tabTitle("", "") == "New Tab");
        CHECK(tabTitle("   ", "about:blank") == "New Tab");
        // Cut at a character, never inside one, with an ellipsis.
        CHECK(tabTitle("abcdefghij", "", 5) == "abcd\xE2\x80\xA6");
        CHECK(tabTitle("abcde", "", 5) == "abcde");
        CHECK(tabTitle("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", "", 4) ==
              "\xC3\xA9\xC3\xA9\xC3\xA9\xE2\x80\xA6");
        CHECK(tabTitle("ab cdef", "", 4) == "ab\xE2\x80\xA6");   // no space before it
        CHECK(tabTitle("anything", "", 0) == "anything");        // 0: no limit
    }

    GROUP("browsertabs:records");
    {
        // All fit: all kept.
        std::vector<bool> k = keptRecords({10, 20, 30}, {1, 2, 3}, 0, 100, 100);
        CHECK(k[0] && k[1] && k[2]);
        // One over the limit a tab is left out, the others kept.
        k = keptRecords({10, 200, 30}, {1, 2, 3}, 0, 100, 1000);
        CHECK(k[0] && !k[1] && k[2]);
        // The total: the active tab first, then the most recently used.
        k = keptRecords({60, 50, 50}, {1, 2, 3}, 0, 100, 120);
        CHECK(k[0] && !k[1] && k[2]);
        // One that does not fit is passed over for a smaller one after it.
        k = keptRecords({60, 50, 10}, {9, 8, 1}, 0, 100, 75);
        CHECK(k[0] && !k[1] && k[2]);
        // An empty record (a tab never loaded) is not a record.
        k = keptRecords({0, 5}, {1, 2}, 1, 100, 100);
        CHECK(!k[0] && k[1]);
        // A short list of stamps counts the rest as oldest.
        k = keptRecords({5, 5, 5}, {3}, -1, 100, 10);
        CHECK(k[0] && k[1] && !k[2]);
    }

    GROUP("browsertabs:restore");
    {
        CHECK(restoredActive(1, 3) == 1);
        CHECK(restoredActive(3, 3) == 2);
        CHECK(restoredActive(-1, 3) == 2);
        CHECK(restoredActive(0, 0) == -1);
    }
}
