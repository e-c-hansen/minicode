// page_check_tests.inc — the `page:*` groups of run_tests.cpp (PageCheck.h,
// and the `browse` and `page` command lines in ControlProtocol.h): agents
// checking pages in the browser pane. Included there.

static void testPageCheck() {
    using namespace PageCheck;

    GROUP("page:local");
    for (const char *u : {"http://localhost:5173/", "http://localhost", "https://LOCALHOST:8443/x?y#z",
                          "http://127.0.0.1:8000/index.html", "http://[::1]:3000/",
                          "http://[0:0:0:0:0:0:0:1]/", "http://app.localhost:5173/",
                          "http://a.b.localhost/", "http://localhost./", "file:///Users/me/x.html",
                          "FILE:///tmp/a.html", "http://user:pw@localhost:9/"})
        CHECK(isLocalAddress(u));
    for (const char *u : {"https://example.com", "http://localhost.example.com/",
                          "http://127.0.0.1.evil.com/", "http://localhost@evil.com/",
                          "http://evil.com\\@localhost/", "http://evil.com#@localhost",
                          "http://evil.com?@localhost", "http://127.0.0.2/", "http://[::2]/",
                          "http://localhost:80x/", "about:blank", "data:text/html,hi",
                          "javascript:alert(1)", "ftp://localhost/", "http:localhost", "",
                          "http://.localhost/", "http://x..localhost/", "http://local%68ost/",
                          "http://localhost /", "http://exa\nmple@localhost/", "file:/x",
                          "http://[::1/"})
        CHECK(!isLocalAddress(u));
    {
        std::string e = originError("https://example.com/");
        CHECK(e.find("control.page-any-origin = true") != std::string::npos &&
              e.find("https://example.com/") != std::string::npos);
    }

    GROUP("page:address");
    {
        auto exists = [](const std::string &p) { return p == "/w/site/index.html" || p == "/w/a b.html"; };
        auto addr = [&](const std::string &raw, std::string &err) {
            std::string url;
            err.clear();
            return agentAddress(raw, exists, url, err) ? url : std::string("ERR");
        };
        std::string err;
        CHECK(addr("localhost:5173", err) == "http://localhost:5173");
        CHECK(addr("  localhost:5173/app  ", err) == "http://localhost:5173/app");
        CHECK(addr("localhost", err) == "http://localhost");
        CHECK(addr("127.0.0.1:8000/x?y=1", err) == "http://127.0.0.1:8000/x?y=1");
        CHECK(addr("127.0.0.1", err) == "http://127.0.0.1");
        CHECK(addr("[::1]:8080", err) == "http://[::1]:8080");
        CHECK(addr("app.localhost:3000", err) == "http://app.localhost:3000");
        CHECK(addr("myhost:8080", err) == "http://myhost:8080");
        CHECK(addr("http://localhost:5173/", err) == "http://localhost:5173/");
        CHECK(addr("https://example.com", err) == "https://example.com");
        CHECK(addr("example.com", err) == "https://example.com");
        CHECK(addr("file:///w/x.html", err) == "file:///w/x.html");
        CHECK(addr("about:blank", err) == "about:blank");
        CHECK(addr("/w/site/index.html", err) == "file:///w/site/index.html");
        CHECK(addr("/w/a b.html", err) == "file:///w/a%20b.html");
        CHECK(addr("/w/missing.html", err) == "ERR" && err.find("no such file") != std::string::npos);
        // Never a search, never another scheme.
        CHECK(addr("how to center a div", err) == "ERR" && err.find("never searches") != std::string::npos);
        CHECK(addr("javascript:alert(1)", err) == "ERR" && err.find("javascript:") != std::string::npos);
        CHECK(addr("JavaScript://%0aalert(1)", err) == "ERR");
        CHECK(addr("data:text/html,<b>x</b>", err) == "ERR");
        CHECK(addr("mailto:a@b.com", err) == "ERR");
        CHECK(addr("http:example.com", err) == "ERR");
        CHECK(addr("", err) == "ERR" && err == "browse needs an address");
        CHECK(addr("http://x/\x01", err) == "ERR");
        CHECK(addr("localhost:abc", err) == "ERR");
        CHECK(fileAddress("/a/b#c?.html") == "file:///a/b%23c%3F.html");
    }

    GROUP("page:console-log");
    {
        ConsoleLog log;
        CHECK(log.lastSeq() == 0 && log.since(0).empty());
        ConsoleEntry e;
        e.level = "log";
        e.text = "one";
        CHECK(log.add(e) == 1);
        e.text = "two";
        CHECK(log.add(e) == 2);
        CHECK(log.since(0).size() == 2 && log.since(1).size() == 1 && log.since(1)[0].text == "two");
        CHECK(log.since(2).empty());
        // A new page: entries go, numbers go on.
        log.newPage();
        CHECK(log.size() == 0 && log.lastSeq() == 2);
        e.text = "three";
        CHECK(log.add(e) == 3 && log.since(0).size() == 1 && log.since(0)[0].seq == 3);
        // The last 500 are kept.
        for (int i = 0; i < 600; i++) log.add(e);
        CHECK(log.size() == kMaxConsoleEntries && log.dropped() == 101);
        CHECK(log.since(0).front().seq == 104 && log.since(0).back().seq == 603);
        CHECK(log.since(600).size() == 3);
        log.newPage();
        CHECK(log.dropped() == 0);
    }

    GROUP("page:stack");
    {
        Frame f;
        CHECK(callerFrame("global code@http://127.0.0.1:8000/page.html:5:14", f) &&
              f.source == "http://127.0.0.1:8000/page.html" && f.line == 5 && f.column == 14);
        // The capture's own frames (no address, or a user script's) and
        // native code are passed over.
        CHECK(callerFrame("log@\n[native code]\nwrap@user-script:3:10\n"
                          "onClick@http://localhost:5173/src/app.js:12:7\nglobal code@http://x/:1:1",
                          f) &&
              f.source == "http://localhost:5173/src/app.js" && f.line == 12 && f.column == 7);
        CHECK(callerFrame("http://localhost/a.js:9:2", f) && f.source == "http://localhost/a.js" &&
              f.line == 9);
        CHECK(callerFrame("f@file:///Users/me/x.html:3", f) && f.source == "file:///Users/me/x.html" &&
              f.line == 3 && f.column == 0);
        CHECK(!callerFrame("", f));
        CHECK(!callerFrame("log@\n[native code]\nanonymous@", f));
        CHECK(!callerFrame("f@notaurl:1:2", f));
    }

    GROUP("page:console-message");
    {
        ConsoleEntry e;
        CHECK(parseConsoleMessage(R"({"kind":"console","level":"error","text":"x",)"
                                  R"("stack":"global code@http://127.0.0.1:9/p.html:7:15"})", e));
        CHECK(e.level == "error" && e.text == "x" && !e.uncaught && e.source == "http://127.0.0.1:9/p.html" &&
              e.line == 7 && e.column == 15);
        CHECK(parseConsoleMessage(R"({"kind":"uncaught","text":"TypeError: boom",)"
                                  R"("source":"http://localhost/app.js","line":3,"column":9})", e));
        CHECK(e.level == "error" && e.uncaught && e.text == "TypeError: boom" && e.line == 3);
        CHECK(parseConsoleMessage(R"({"kind":"rejection","text":"nope"})", e) && e.uncaught &&
              e.text == "Unhandled promise rejection: nope");
        // Unknown kinds and levels, broken JSON, and from the page's world
        // anything but a console call.
        CHECK(!parseConsoleMessage(R"({"kind":"console","level":"shout","text":"x"})", e));
        CHECK(!parseConsoleMessage(R"({"kind":"eval","text":"x"})", e));
        CHECK(!parseConsoleMessage("{", e));
        CHECK(!parseConsoleMessage("[]", e));
        CHECK(!parseConsoleMessage(R"({"kind":"uncaught","text":"fake"})", e, true));
        CHECK(parseConsoleMessage(R"({"kind":"console","level":"warn","text":"w"})", e, true) &&
              e.level == "warn" && e.source.empty() && e.line == 0);
        CHECK(parseConsoleMessage(R"({"kind":"console","level":"log","text":"x","line":-4})", e) &&
              e.line == 0);
        // Long text is cut on a character boundary.
        std::string big = "{\"kind\":\"console\",\"level\":\"log\",\"text\":\"";
        for (int i = 0; i < 3000; i++) big += "\xC3\xA9";   // é
        big += "\"}";
        CHECK(parseConsoleMessage(big, e) && e.text.size() <= kMaxConsoleText &&
              e.text.size() % 2 == 0);
        Json j = entryJson(e);
        CHECK(j["seq"].asInt() == 0 && j["level"].asString() == "log" && j["source"].isNull() &&
              j["line"].isNull() && !j.has("uncaught"));
        e.uncaught = true;
        e.source = "http://localhost/a.js";
        e.line = 2;
        j = entryJson(e);
        CHECK(j["uncaught"].asBool() && j["line"].asInt() == 2 &&
              j["source"].asString() == "http://localhost/a.js");
    }

    GROUP("page:state");
    {
        Json s = pageState("http://localhost:5173/", 200, "App", false);
        CHECK(s.dump() == R"({"url":"http://localhost:5173/","status":200,"title":"App","loading":false})");
        s = pageState("file:///x.html", 0, "", true);
        CHECK(s["status"].isNull() && s["loading"].asBool());
        CHECK(pageState("", 0, "", false)["url"].isNull());
        CHECK(clipUtf8("h\xC3\xA9llo", 2) == "h" && clipUtf8("abc", 9) == "abc");
    }

    GROUP("page:snapshot");
    {
        SnapshotPlan p = snapshotPlan(600, 5000, 2, false);
        CHECK(p.height == 600 && !p.cut);
        p = snapshotPlan(600, 5000, 2, true);
        CHECK(p.height == 5000 && !p.cut);
        p = snapshotPlan(600, 100, 2, true);   // a short page: the visible area
        CHECK(p.height == 600);
        p = snapshotPlan(600, 20000, 2, true);   // 16,384 pixels at 2x
        CHECK(p.height == 8192 && p.cut);
        p = snapshotPlan(600, 20000, 1, true);
        CHECK(p.height == 16384 && p.cut);
        p = snapshotPlan(0, 0, 0, true);
        CHECK(p.height == 1);
        std::string err;
        CHECK(screenshotPath("/tmp/s/shot.png", false, err));
        CHECK(screenshotPath("/tmp/s/SHOT.PNG", false, err));
        CHECK(!screenshotPath("/tmp/s/shot.jpg", false, err) && err.find(".png") != std::string::npos);
        CHECK(!screenshotPath("/tmp/s/.zshrc", false, err));
        CHECK(!screenshotPath("rel.png", false, err));
        CHECK(!screenshotPath("/tmp/dir.png", true, err) && err.find("folder") != std::string::npos);
    }

    GROUP("page:command-line");
    {
        using namespace ControlChannel;
        auto exists = [](const std::string &p) { return p == "/w/site/index.html"; };
        auto parse = [&](std::vector<std::string> a) {
            return parseCommandLine(a, "/w", "/Users/me", exists);
        };
        std::string err;
        CliCommand c = parse({"browse", "localhost:5173", "--wait"});
        CHECK(c.error.empty() && c.args["wait"].asBool() && c.output == Output::Json &&
              c.args["url"].asString() == "localhost:5173");
        CHECK(validateArgs(*findCommand("browse"), c.args, err));
        c = parse({"browse", "localhost:5173"});
        CHECK(c.error.empty() && c.output == Output::None && !c.args.has("wait"));
        c = parse({"browse", "site/index.html"});   // a file named from here
        CHECK(c.args["url"].asString() == "/w/site/index.html");
        CHECK(!parse({"browse", "x", "--yours"}).error.empty());
        c = parse({"page", "--text"});
        CHECK(c.error.empty() && c.cmd == "page" && c.args["action"].asString() == "text" &&
              c.output == Output::Text && c.outputField == "text");
        c = parse({"page", "--url", "--yours"});
        CHECK(c.error.empty() && c.args["yours"].asBool() && c.output == Output::Json);
        c = parse({"page", "--console", "--since", "12"});
        CHECK(c.error.empty() && c.args["since"].asInt() == 12 && c.output == Output::JsonLines &&
              c.outputField == "entries");
        c = parse({"page", "--screenshot", "shots/a.png", "--full"});
        CHECK(c.error.empty() && c.args["action"].asString() == "screenshot" &&
              c.args["path"].asString() == "/w/shots/a.png" && c.args["full"].asBool() &&
              c.output == Output::Line && c.outputField == "path");
        c = parse({"page", "--screenshot=~/s.png"});
        CHECK(c.args["path"].asString() == "/Users/me/s.png");
        c = parse({"page", "--reload", "--wait"});
        CHECK(c.error.empty() && c.args["wait"].asBool() && c.output == Output::Json);
        CHECK(parse({"page", "--reload"}).output == Output::None);
        CHECK(validateArgs(*findCommand("page"), parse({"page", "--console", "--since", "3"}).args, err));
        // One action, its own options only.
        CHECK(!parse({"page"}).error.empty());
        CHECK(!parse({"page", "--text", "--url"}).error.empty());
        CHECK(!parse({"page", "--text", "--full"}).error.empty());
        CHECK(!parse({"page", "--text", "--since", "2"}).error.empty());
        CHECK(!parse({"page", "--text", "--wait"}).error.empty());
        CHECK(!parse({"page", "--console", "--since", "x"}).error.empty());
        CHECK(!parse({"page", "--screenshot"}).error.empty());
        CHECK(!parse({"page", "--text", "extra"}).error.empty());
        CHECK(!parse({"page", "--eval", "1"}).error.empty());   // there is no eval
        CHECK(isSubcommand("page") && !launchesApp("page") && launchesApp("browse"));
        CHECK(usageText("page").find("control.page-any-origin") != std::string::npos);
        CHECK(usageText("browse").find("--wait") != std::string::npos);
        // The protocol table has no field that carries script.
        for (const ArgSpec &s : findCommand("page")->args)
            CHECK(s.name != "script" && s.name != "js" && s.name != "eval");
        Request r;
        CHECK(!parseRequest(R"({"v":1,"id":1,"cmd":"page","args":{"action":"text","script":"1"}})", r, err));
        CHECK(!parseRequest(R"({"v":1,"id":1,"cmd":"page","args":{}})", r, err));
    }

    GROUP("page:skill");
    {
        // R1.11 and R6.6: the skill stays short and says how to check pages.
        std::ifstream f("skills/minicode/SKILL.md");
        if (!f) {
            std::printf("  (skills/minicode/SKILL.md not found here; skill checks skipped)\n");
        } else {
            std::stringstream ss;
            ss << f.rdbuf();
            const std::string skill = ss.str();
            CHECK(std::count(skill.begin(), skill.end(), '\n') < 150);
            for (const char *w : {"minicode browse", "--wait", "minicode page --text", "--console",
                                  "--screenshot", "--yours", "Playwright", "scratchpad",
                                  "control.page-any-origin"})
                CHECK(skill.find(w) != std::string::npos);
        }
    }

    GROUP("page:settings");
    {
        std::vector<SettingsError> errors;
        Settings s = Settings::parse("control.page-any-origin = true\n", &errors);
        CHECK(errors.empty() && s.pageAnyOrigin());
        CHECK(!Settings::parse("", nullptr).pageAnyOrigin());
        Settings::parse("control.page-any-origin = maybe\n", &errors);
        CHECK(!errors.empty());
        CHECK(std::string(Settings::defaultFileText()).find("# control.page-any-origin = false\n") !=
              std::string::npos);
    }
}
