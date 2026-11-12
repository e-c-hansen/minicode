// bookmark_tests.inc — the `bookmarks:*` groups of run_tests.cpp: web
// bookmarks kept as .url and .webloc files (WebBookmark.h).

void testWebBookmarks() {
    using namespace WebBookmark;

    GROUP("bookmarks:paths");
    {
        CHECK(isBookmarkPath("/Users/me/Bookmarks/Google.url"));
        CHECK(isBookmarkPath("Docs.URL"));
        CHECK(isBookmarkPath("/a/b/Apple.webloc"));
        CHECK(isBookmarkPath("x.WebLoc"));
        CHECK(!isBookmarkPath("/a/b/.url"));        // a dotfile named for the extension
        CHECK(!isBookmarkPath("/a/b/.webloc"));
        CHECK(!isBookmarkPath("/a/b/url"));
        CHECK(!isBookmarkPath("/a/b.url/readme.md"));
        CHECK(!isBookmarkPath("notes.urls"));
        CHECK(!isBookmarkPath(""));
    }

    GROUP("bookmarks:addresses");
    {
        CHECK(isWebAddress("https://www.google.com"));
        CHECK(isWebAddress("http://localhost:5173/"));
        CHECK(isWebAddress("HTTPS://Example.com/a?b=c#d"));
        CHECK(isWebAddress("https://[::1]:8080/"));
        CHECK(!isWebAddress("https://"));
        CHECK(!isWebAddress("https:///path"));
        CHECK(!isWebAddress("http://?q"));
        CHECK(!isWebAddress("file:///etc/passwd"));
        CHECK(!isWebAddress("javascript:alert(1)"));
        CHECK(!isWebAddress(" https://example.com"));
        CHECK(!isWebAddress("https://exa mple.com"));
        CHECK(!isWebAddress("https://example.com/\n"));
        CHECK(!isWebAddress("https://example.com/\x7F"));
        CHECK(!isWebAddress("ftp://example.com/"));
        CHECK(!isWebAddress("www.google.com"));
        CHECK(!isWebAddress(""));
        CHECK(isWebAddress("https://a.b/" + std::string(8000, 'x')));
        CHECK(!isWebAddress("https://a.b/" + std::string(9000, 'x')));
    }

    GROUP("bookmarks:url-files");
    {
        CHECK(addressIn("a.url", "[InternetShortcut]\r\nURL=https://example.com/\r\n") ==
              "https://example.com/");
        // LF, a byte order mark, cases, spaces around '=', other keys and
        // sections, comments.
        CHECK(addressIn("A.URL", "\xEF\xBB\xBF[{000214A0-0000-0000-C000-000000000046}]\n"
                                 "Prop3=19,11\n"
                                 "; a comment\n"
                                 "[internetshortcut]\n"
                                 "IDList=\n"
                                 "  url = https://example.com/x?y=1  \n"
                                 "IconIndex=0\n") == "https://example.com/x?y=1");
        // The URL key of another section is not the shortcut's.
        CHECK(addressIn("a.url", "[Other]\nURL=https://wrong.example/\n"
                                 "[InternetShortcut]\nURL=https://right.example/\n") ==
              "https://right.example/");
        CHECK(addressIn("a.url", "[Other]\nURL=https://wrong.example/\n") == "");
        // The first URL in the section wins.
        CHECK(addressIn("a.url", "[InternetShortcut]\nURL=https://one.example/\n"
                                 "URL=https://two.example/\n") == "https://one.example/");
        // No final line end.
        CHECK(addressIn("a.url", "[InternetShortcut]\nURL=https://end.example/") ==
              "https://end.example/");
        // Addresses a bookmark must not open.
        CHECK(addressIn("a.url", "[InternetShortcut]\nURL=file:///etc/passwd\n") == "");
        CHECK(addressIn("a.url", "[InternetShortcut]\nURL=javascript:alert(1)\n") == "");
        CHECK(addressIn("a.url", "[InternetShortcut]\nURL=\n") == "");
        CHECK(addressIn("a.url", "") == "");
        CHECK(addressIn("a.url", "not an ini file at all") == "");
        // The extension decides the reader.
        CHECK(addressIn("a.txt", "[InternetShortcut]\nURL=https://example.com/\n") == "");
    }

    GROUP("bookmarks:webloc-files");
    {
        const std::string xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\">\n<dict>\n\t<key>URL</key>\n"
            "\t<string>https://example.com/a?b=1&amp;c=2&#38;d=&#x33;</string>\n"
            "</dict>\n</plist>\n";
        CHECK(addressIn("x.webloc", xml) == "https://example.com/a?b=1&c=2&d=3");
        CHECK(addressIn("X.WEBLOC", "<plist><dict><key>URL</key><string>https://e.org/</string>"
                                    "</dict></plist>") == "https://e.org/");
        // A binary property list is the Mac's to read.
        CHECK(addressIn("x.webloc", std::string("bplist00\xD1\x01\x02SURL_", 15)) == "");
        // Missing or unsafe addresses.
        CHECK(addressIn("x.webloc", "<plist><dict></dict></plist>") == "");
        CHECK(addressIn("x.webloc", "<plist><dict><key>URL</key><integer>3</integer>"
                                    "</dict></plist>") == "");
        CHECK(addressIn("x.webloc", "<plist><dict><key>URL</key><string>file:///etc</string>"
                                    "</dict></plist>") == "");
        CHECK(addressIn("x.webloc", "<plist><dict><key>URL</key><string>https://e.org/") == "");
        // A bad entity stays as written, so the address has a space-free '&'.
        CHECK(addressIn("x.webloc", "<key>URL</key><string>https://e.org/?a&b</string>") ==
              "https://e.org/?a&b");
        // A NUL by reference is refused, never decoded.
        CHECK(addressIn("x.webloc", "<key>URL</key><string>https://e.org/&#0;</string>") ==
              "https://e.org/&#0;");
    }

    GROUP("bookmarks:write");
    {
        CHECK(shortcutText("https://example.com/") ==
              "[InternetShortcut]\r\nURL=https://example.com/\r\n");
        // What is written reads back.
        const std::string u = "https://example.com/search?q=a&b=%20#top";
        CHECK(addressIn("Saved.url", shortcutText(u)) == u);
    }

    GROUP("bookmarks:names");
    {
        CHECK(fileName("Google", "https://www.google.com/") == "Google.url");
        CHECK(fileName("  Two\n lines ", "https://e.org/") == "Two lines.url");
        CHECK(fileName("a/b: c", "https://e.org/") == "a-b- c.url");
        CHECK(fileName("", "https://www.example.com/docs/x") == "example.com-docs-x.url");
        CHECK(fileName("", "http://localhost:5173/") == "localhost-5173.url");
        CHECK(fileName("...hidden", "https://e.org/") == "hidden.url");
        CHECK(fileName("Title. . .", "https://e.org/") == "Title.url");
        CHECK(fileName("tab\there", "https://e.org/") == "tab here.url");
        CHECK(fileName("bell\x07ring", "https://e.org/") == "bellring.url");
        CHECK(fileName("", "") == "Bookmark.url");
        CHECK(fileName("...", "") == "Bookmark.url");
        // Cut at a character, never inside one.
        CHECK(fileName("abcdefgh", "https://e.org/", 5) == "abcde.url");
        CHECK(fileName("\xC3\xA9\xC3\xA9\xC3\xA9", "https://e.org/", 5) ==
              "\xC3\xA9\xC3\xA9.url");
        CHECK(fileName("abcd efgh", "https://e.org/", 5) == "abcd.url");
        CHECK(fileName(std::string(500, 'x'), "https://e.org/").size() == 120 + 4);
    }

    GROUP("bookmarks:unique");
    {
        std::set<std::string> have = {"Google.url", "Google 2.url", "README"};
        auto taken = [&](const std::string &n) { return have.count(n) > 0; };
        CHECK(uniqueName("Other.url", taken) == "Other.url");
        CHECK(uniqueName("Google.url", taken) == "Google 3.url");
        CHECK(uniqueName("README", taken) == "README 2");
        CHECK(tabsFolderName("2026-11-12 14.03") == "Tabs 2026-11-12 14.03");
    }

    GROUP("bookmarks:tab-addresses");
    {
        const std::vector<std::string> in = {"https://a.org/", "file:///x", "https://b.org/",
                                             "https://a.org/", "", "http://c.org/"};
        const std::vector<std::string> want = {"https://a.org/", "https://b.org/",
                                               "http://c.org/"};
        CHECK(tabAddresses(in) == want);
        CHECK(tabAddresses({}).empty());
    }
}
