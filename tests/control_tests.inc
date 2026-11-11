// control_tests.inc — the `control:*` and `sidebyside:*` groups of
// run_tests.cpp (ControlProtocol.h, SideBySide.h), kept in their own file
// and included there.
#include <stdexcept>

static void testControlProtocol() {
    using namespace ControlChannel;

    GROUP("control:parse");
    {
        Request r;
        std::string err;
        CHECK(parseRequest(R"({"v":1,"id":7,"cmd":"state"})", r, err));
        CHECK(r.cmd == "state" && r.id.asInt() == 7 && r.args.isObject());
        CHECK(parseRequest(R"({"v":1,"id":"a","cmd":"browse","args":{"url":"https://x"}})", r, err));
        CHECK(r.id.asString() == "a" && r.args["url"].asString() == "https://x");
        // Broken requests: each an error, with the id when it was readable.
        CHECK(!parseRequest("{", r, err) && err.find("not JSON") == 0 && r.id.isNull());
        CHECK(!parseRequest("[1]", r, err) && err.find("object") != std::string::npos);
        CHECK(!parseRequest(R"({"v":1,"cmd":"state"})", r, err) && err.find("\"id\"") != std::string::npos);
        CHECK(!parseRequest(R"({"v":2,"id":1,"cmd":"state"})", r, err) && r.id.asInt() == 1);
        CHECK(!parseRequest(R"({"id":1,"cmd":"state"})", r, err) && err.find("\"v\"") != std::string::npos);
        CHECK(!parseRequest(R"({"v":1,"id":1})", r, err) && err.find("\"cmd\"") != std::string::npos);
        CHECK(!parseRequest(R"({"v":1,"id":1,"cmd":"rm"})", r, err) &&
              err == "unknown command \"rm\"");
        CHECK(!parseRequest(R"({"v":1,"id":1,"cmd":"state","x":1})", r, err) &&
              err.find("unknown field \"x\"") != std::string::npos);
        CHECK(!parseRequest(R"({"v":1,"id":1,"cmd":"browse","args":{"url":"u","tab":1}})", r, err) &&
              err.find("unknown field \"tab\"") != std::string::npos);
        CHECK(!parseRequest(R"({"v":1,"id":1,"cmd":"browse","args":{}})", r, err) &&
              err.find("needs \"url\"") != std::string::npos);
        CHECK(!parseRequest(R"({"v":1,"id":1,"cmd":"browse","args":{"url":3}})", r, err) &&
              err.find("must be a string") != std::string::npos);
        CHECK(!parseRequest(R"({"v":1,"id":1,"cmd":"term-output","args":{"terminal":1.5}})", r, err));
        CHECK(!parseRequest(R"({"v":1,"id":1,"cmd":"browse","args":[]})", r, err));
        CHECK(!parseRequest(R"({"v":1,"id":{},"cmd":"state"})", r, err));
        // Nested objects are checked too.
        CHECK(parseRequest(R"({"v":1,"id":1,"cmd":"open","args":{"files":[{"path":"/a","line":3}]}})",
                           r, err));
        CHECK(!parseRequest(R"({"v":1,"id":1,"cmd":"open","args":{"files":[{"line":3}]}})", r, err) &&
              err.find("item 1") != std::string::npos);
        CHECK(!parseRequest(R"({"v":1,"id":1,"cmd":"open","args":{"files":[{"path":"/a","colour":1}]}})",
                            r, err));
        CHECK(!parseRequest(R"({"v":1,"id":1,"cmd":"open","args":{"files":["/a"]}})", r, err));
        // Every command in the table validates its own empty-ish args sanely.
        for (const CommandSpec &c : commands()) {
            bool needs = false;
            for (const ArgSpec &a : c.args) needs = needs || a.required;
            CHECK(validateArgs(c, Json::object(), err) == !needs);
        }
        // Garbage of every kind never crashes.
        const char *junk[] = {"", "null", "\"x\"", "{\"v\":1,\"id\":1,\"cmd\":null}",
                              "{\"v\":\"1\",\"id\":1,\"cmd\":\"state\"}", "{}{}", "\xff\xfe"};
        for (const char *j : junk) CHECK(!parseRequest(j, r, err));
    }

    GROUP("control:lines");
    {
        CHECK(okLine(Json(3), Json::object({{"a", 1}})) == "{\"id\":3,\"ok\":true,\"result\":{\"a\":1}}\n");
        CHECK(errorLine(Json("x"), "no") == "{\"id\":\"x\",\"ok\":false,\"error\":\"no\"}\n");
        CHECK(errorLine(Json(), "no") == "{\"id\":null,\"ok\":false,\"error\":\"no\"}\n");
        std::string line = requestLine(Json(1), "state", Json::object());
        Request r;
        std::string err;
        CHECK(line.back() == '\n' && parseRequest(line, r, err) && r.cmd == "state");
    }

    GROUP("control:dispatch");
    {
        Dispatcher d;
        std::vector<std::string> sent;
        auto send = [&](const std::string &s) { sent.push_back(s); };
        d.on("ping", [](const Request &, Dispatcher::Respond respond) {
            respond(Result::success(Json::object({{"pid", 42}})));
            respond(Result::failure("twice"));   // dropped
        });
        Dispatcher::Respond later;
        d.on("state", [&](const Request &, Dispatcher::Respond respond) { later = respond; });
        d.on("selection", [](const Request &, Dispatcher::Respond) {
            throw std::runtime_error("boom");
        });
        d.on("window", [](const Request &, Dispatcher::Respond respond) {
            respond(Result::failure(""));
        });
        CHECK(d.handles("ping") && !d.handles("diff"));
        d.handle(R"({"v":1,"id":1,"cmd":"ping"})", send);
        CHECK(sent.size() == 1 && sent[0] == "{\"id\":1,\"ok\":true,\"result\":{\"pid\":42}}\n");
        d.handle(R"({"v":1,"id":2,"cmd":"state"})", send);
        CHECK(sent.size() == 1);   // answered later
        later(Result::success(Json::array()));
        CHECK(sent.size() == 2 && sent[1] == "{\"id\":2,\"ok\":true,\"result\":[]}\n");
        later(Result::success(Json::array()));
        CHECK(sent.size() == 2);
        d.handle(R"({"v":1,"id":3,"cmd":"selection"})", send);
        CHECK(sent.size() == 3 && sent[2].find("internal error: boom") != std::string::npos &&
              sent[2].find("\"id\":3") != std::string::npos);
        d.handle(R"({"v":1,"id":4,"cmd":"diff","args":{"path":"/a"}})", send);
        CHECK(sent.size() == 4 && sent[3].find("not available") != std::string::npos);
        d.handle(R"({"v":1,"id":5,"cmd":"nope"})", send);
        CHECK(sent.size() == 5 && sent[4].find("unknown command") != std::string::npos &&
              sent[4].find("\"id\":5") != std::string::npos);
        d.handle(R"({"v":1,"id":6,"cmd":"window"})", send);
        CHECK(sent.size() == 6 && sent[5].find("\"error\":\"failed\"") != std::string::npos);
        // Every answer is one line of JSON.
        for (const std::string &s : sent) {
            Json j;
            CHECK(s.back() == '\n' && s.find('\n') == s.size() - 1 &&
                  Json::parse(s, j) && j.has("id") && j["ok"].isBool());
        }
    }

    GROUP("control:path-spec");
    {
        std::set<std::string> files = {"/p/a.py", "/p/odd:12"};
        auto exists = [&](const std::string &p) { return files.count(p) > 0; };
        PathSpec s = splitPathSpec("/p/a.py", exists);
        CHECK(s.path == "/p/a.py" && s.line == 0 && s.column == 0);
        s = splitPathSpec("/p/a.py:12", exists);
        CHECK(s.path == "/p/a.py" && s.line == 12 && s.column == 0);
        s = splitPathSpec("/p/a.py:12:5", exists);
        CHECK(s.path == "/p/a.py" && s.line == 12 && s.column == 5);
        s = splitPathSpec("/p/odd:12", exists);   // a file whose name ends in :12
        CHECK(s.path == "/p/odd:12" && s.line == 0);
        s = splitPathSpec("/p/odd:12:3", exists);
        CHECK(s.path == "/p/odd:12" && s.line == 3);
        s = splitPathSpec("/p/new.py:7", exists);  // not there (yet): still split
        CHECK(s.path == "/p/new.py" && s.line == 7);
        s = splitPathSpec("/p/a.py:x", exists);
        CHECK(s.path == "/p/a.py:x" && s.line == 0);
        CHECK(absolutePath("src/x.mm", "/r", "/h") == "/r/src/x.mm");
        CHECK(absolutePath("./a/../b//c", "/r/", "/h") == "/r/b/c");
        CHECK(absolutePath("~/n.md", "/r", "/Users/me") == "/Users/me/n.md");
        CHECK(absolutePath("/x/../../y", "/r", "/h") == "/y");
        CHECK(absolutePath(".", "/r", "/h") == "/r");
    }

    GROUP("control:command-line");
    {
        std::set<std::string> files = {"/w/src/main.mm", "/w/a.txt", "/w/b.txt"};
        auto exists = [&](const std::string &p) { return files.count(p) > 0; };
        auto parse = [&](std::vector<std::string> a) {
            return parseCommandLine(a, "/w", "/Users/me", exists);
        };
        CliCommand c = parse({"open", "src/main.mm:100"});
        CHECK(c.error.empty() && c.cmd == "open" && c.output == Output::None);
        CHECK(c.args["files"].size() == 1 && c.args["files"][0]["path"].asString() == "/w/src/main.mm" &&
              c.args["files"][0]["line"].asInt() == 100 && !c.args.has("note"));
        std::string err;
        CHECK(validateArgs(*findCommand("open"), c.args, err));
        c = parse({"open", "src/main.mm:3090", "--to", "3185", "--note", "Applies\n rename  edits",
                   "--from=Reviewer", "--keep", "--window", "2"});
        CHECK(c.error.empty() && c.args["to_line"].asInt() == 3185 &&
              c.args["note"].asString() == "Applies rename edits" &&
              c.args["from"].asString() == "Reviewer" && c.args["keep"].asBool() &&
              c.args["window"].asInt() == 2);
        CHECK(validateArgs(*findCommand("open"), c.args, err));
        c = parse({"open", "a.txt", "b.txt"});
        CHECK(c.error.empty() && c.args["files"].size() == 2);
        CHECK(!parse({"open", "a.txt", "b.txt", "--note", "x"}).error.empty());
        CHECK(!parse({"open", "a.txt", "--to", "9"}).error.empty());   // no starting line
        CHECK(parse({"open", "a.txt:3", "--to", "9:4"}).args["to_column"].asInt() == 4);
        CHECK(!parse({"open"}).error.empty());
        CHECK(!parse({"open", "a.txt", "--bogus"}).error.empty());
        CHECK(!parse({"open", "a.txt", "--to"}).error.empty());
        CHECK(parse({"open", "--", "--odd"}).args["files"][0]["path"].asString() == "/w/--odd");

        c = parse({"window", "--folder", "proj", "--new"});
        CHECK(c.error.empty() && c.args["folder"].asString() == "/w/proj" && c.args["new"].asBool() &&
              c.output == Output::Id && c.outputField == "window" && c.launchFolder == "/w/proj");
        CHECK(!parse({"window", "--window", "1"}).error.empty());
        c = parse({"browse", "https://example.com"});
        CHECK(c.error.empty() && c.args["url"].asString() == "https://example.com" &&
              c.launchFolder == "/w");
        CHECK(!parse({"browse"}).error.empty());
        c = parse({"term", "--cwd", "sub", "--title", "Tests", "--run", "make test", "--pid", "123"});
        CHECK(c.error.empty() && c.args["cwd"].asString() == "/w/sub" &&
              c.args["title"].asString() == "Tests" && c.args["run"].asString() == "make test" &&
              c.pid == 123 && c.output == Output::Id && c.outputField == "terminal");
        CHECK(validateArgs(*findCommand("term"), c.args, err));
        CHECK(!parse({"term", "--pid", "x"}).error.empty());
        c = parse({"diff", "a.txt"});
        CHECK(c.error.empty() && c.args["path"].asString() == "/w/a.txt" && !c.args.has("other"));
        c = parse({"diff", "a.txt", "--base", "main~2"});
        CHECK(c.args["base"].asString() == "main~2");
        c = parse({"diff", "a.txt", "b.txt"});
        CHECK(c.args["other"].asString() == "/w/b.txt");
        CHECK(!parse({"diff", "a.txt", "b.txt", "--base", "x"}).error.empty());
        // A revision that would read as an option is refused.
        CHECK(!parse({"diff", "a.txt", "--base", "--output=/tmp/x"}).error.empty());
        CHECK(!parse({"diff", "a.txt", "--base=-p"}).error.empty());
        CHECK(isSafeRef("main~2") && isSafeRef("HEAD") && !isSafeRef("") && !isSafeRef("-x") &&
              !isSafeRef(std::string("a\nb")));
        // Reads never start minicode; everything else does.
        CHECK(!launchesApp("state") && !launchesApp("selection") && !launchesApp("term-output"));
        CHECK(launchesApp("open") && launchesApp("browse") && launchesApp("term") &&
              launchesApp("diff") && launchesApp("tour") && launchesApp("window"));
        CHECK(!parse({"diff"}).error.empty());
        c = parse({"split", "--right", "term", "--run", "ls"});
        CHECK(c.error.empty() && c.cmd == "term" && c.args["split"].asString() == "right");
        c = parse({"split", "--below", "browse", "http://localhost:8000"});
        CHECK(c.error.empty() && c.cmd == "browse" && c.args["split"].asString() == "below");
        CHECK(parse({"split", "term"}).args["split"].asString() == "right");
        CHECK(!parse({"split", "--right"}).error.empty());
        CHECK(!parse({"split", "state"}).error.empty());
        c = parse({"state", "--pretty"});
        CHECK(c.error.empty() && c.pretty && c.output == Output::Json);
        CHECK(!parse({"state", "extra"}).error.empty());
        c = parse({"selection", "--folder", "/x"});
        CHECK(c.error.empty() && c.output == Output::Text && c.args["folder"].asString() == "/x");
        c = parse({"term-output", "4", "--lines", "50"});
        CHECK(c.error.empty() && c.args["terminal"].asInt() == 4 && c.args["lines"].asInt() == 50);
        CHECK(!parse({"term-output"}).error.empty());
        CHECK(!parse({"term-output", "x"}).error.empty());
        c = parse({"tour", "t.json"});
        CHECK(c.error.empty() && c.tourFile == "/w/t.json");
        c = parse({"help"});
        CHECK(c.help && c.helpTopic.empty());
        c = parse({"diff", "-h"});
        CHECK(c.help && c.helpTopic == "diff");
        CHECK(!parse({"frobnicate"}).error.empty());
        CHECK(!parse({}).error.empty());
        CHECK(isSubcommand("term-output") && !isSubcommand("notes.md"));
        // Every subcommand is in the help, and every protocol command but
        // ping has a subcommand.
        std::string help = usageText();
        for (const char *w : {"open", "window", "browse", "term", "diff", "split", "state",
                              "selection", "term-output", "tour", "--pid", "--window", "--folder",
                              "--pretty", "--note", "--keep", "--run", "Exit codes"})
            CHECK(help.find(w) != std::string::npos);
        for (const CommandSpec &cs : commands())
            CHECK(cs.name == "ping" || isSubcommand(cs.name));
        CHECK(usageText("tour").find("Option+]") != std::string::npos);
        CHECK(usageText("nope").empty());
    }

    GROUP("control:linux-socket-folder");
    {
        // Linux's folder: the test's folder, else the runtime folder's, else
        // one of the user's own in the temporary folder.
        CHECK(socketFolder("/x/y/", "/run/user/1000", "/tmp", 1000) == "/x/y");
        CHECK(socketFolder("", "/run/user/1000/", "/tmp", 1000) == "/run/user/1000/minicode");
        CHECK(socketFolder(nullptr, "/run/user/7", nullptr, 7) == "/run/user/7/minicode");
        CHECK(socketFolder(nullptr, nullptr, "/var/tmp/", 42) == "/var/tmp/minicode-42");
        CHECK(socketFolder(nullptr, "relative", "", 5) == "/tmp/minicode-5");
    }

    GROUP("control:launcher-words");
    {
        // The launcher (bash, so it cannot ask the core) hands the words of
        // one case pattern to minicode-cli; they must be exactly the core's
        // subcommands, or a new subcommand opens a file of that name instead.
        std::ifstream lf("scripts/minicode-launcher");
        if (!lf) {
            std::printf("  (scripts/ not found here; launcher check skipped)\n");
        } else {
            std::stringstream ls;
            ls << lf.rdbuf();
            std::string text = ls.str();
            size_t a = text.find("\n  open|");
            size_t b = a == std::string::npos ? a : text.find(")", a);
            CHECK(a != std::string::npos && b != std::string::npos);
            if (a != std::string::npos && b != std::string::npos) {
                std::set<std::string> fromLauncher;
                std::string pattern = text.substr(a + 3, b - a - 3), w;
                std::stringstream ws(pattern);
                while (std::getline(ws, w, '|')) fromLauncher.insert(w);
                std::set<std::string> fromCore(subcommands().begin(), subcommands().end());
                for (const std::string &x : fromLauncher)
                    if (!fromCore.count(x)) std::printf("  the launcher passes on %s\n", x.c_str());
                for (const std::string &x : fromCore)
                    if (!fromLauncher.count(x)) std::printf("  the launcher lacks %s\n", x.c_str());
                CHECK(fromLauncher == fromCore);
                // Its own word, install-skill, is not the client's.
                CHECK(text.find("\n  install-skill)") != std::string::npos);
                CHECK(!isSubcommand("install-skill"));
            }
        }
    }

    GROUP("control:tour-file");
    {
        Json stops;
        std::string err;
        CHECK(tourStops(R"([{"path":"src/a.mm","line":3,"to":9,"note":"First"},
                            {"path":"/abs/b.py","line":10,"note":"Second\nline"}])",
                        "/w", "/h", stops, err));
        CHECK(stops.size() == 2 && stops[0]["path"].asString() == "/w/src/a.mm" &&
              stops[0]["to"].asInt() == 9 && stops[1]["note"].asString() == "Second line" &&
              !stops[1].has("to"));
        CHECK(validateArgs(*findCommand("tour"), Json::object({{"stops", stops}}), err));
        CHECK(tourStops(R"({"stops":[{"path":"x","line":1,"note":"n"}]})", "/w", "/h", stops, err));
        CHECK(stops.size() == 1);
        CHECK(!tourStops("[]", "/w", "/h", stops, err));
        CHECK(!tourStops("nope", "/w", "/h", stops, err));
        CHECK(!tourStops(R"([{"path":"x"}])", "/w", "/h", stops, err) &&
              err.find("stop 1") == 0);
        CHECK(!tourStops(R"([{"path":"x","line":0,"note":""}])", "/w", "/h", stops, err));
        // A "to" before the line is dropped rather than refused.
        CHECK(tourStops(R"([{"path":"x","line":5,"to":2,"note":"n"}])", "/w", "/h", stops, err) &&
              !stops[0].has("to"));
    }

    GROUP("control:note-text");
    {
        CHECK(cleanNoteText("  two\tparts\r\nhere  ") == "two parts here");
        CHECK(cleanNoteText("") == "");
        CHECK(cleanNoteText("caf\xC3\xA9 \xE2\x9C\x93") == "caf\xC3\xA9 \xE2\x9C\x93");
        std::string longText(600, 'a');
        std::string cut = cleanNoteText(longText);
        CHECK(cut.size() == 499 + 3 && cut.substr(499) == "\xE2\x80\xA6");
        std::string exact(500, 'b');
        CHECK(cleanNoteText(exact) == exact);
        // Counted in characters, not bytes.
        std::string wide;
        for (int i = 0; i < 500; i++) wide += "\xC3\xA9";
        CHECK(cleanNoteText(wide) == wide);
        // Separators read as spaces; bidi controls, which would show the
        // text in another order, are dropped; so are C1 controls.
        CHECK(cleanNoteText("one\xE2\x80\xA8two\xE2\x80\xA9three") == "one two three");
        CHECK(cleanNoteText("a\xE2\x80\xAE" "evil\xE2\x80\xAC" "b") == "aevilb");
        CHECK(cleanNoteText("\xE2\x81\xA6x\xE2\x81\xA9\xE2\x80\x8Fy\xE2\x80\x8E") == "xy");
        CHECK(cleanNoteText("a\xC2\x85" "b") == "a b");
        CHECK(cleanNoteText("\xC2\xA0nbsp \xE2\x80\x94 dash") == "\xC2\xA0nbsp \xE2\x80\x94 dash");
        CHECK(clipUtf8("ab\xC3\xA9", 3) == "ab");
        CHECK(clipUtf8("abc", 10) == "abc");
    }

    GROUP("control:sockets");
    {
        CHECK(pidFromSocketName("123.sock") == 123);
        CHECK(pidFromSocketName(socketName(99)) == 99);
        CHECK(pidFromSocketName("x.sock") == 0 && pidFromSocketName("12.socket") == 0 &&
              pidFromSocketName(".sock") == 0 && pidFromSocketName("12") == 0);
        std::set<int> alive = {10, 30};
        auto isAlive = [&](int pid) { return alive.count(pid) > 0; };
        std::vector<SocketEntry> found = {{10, 100}, {20, 300}, {30, 200}};
        CHECK(chooseSocket(found, 0, isAlive) == 30);   // 20 is newest but dead
        CHECK(chooseSocket(found, 10, isAlive) == 10);
        CHECK(chooseSocket(found, 20, isAlive) == 0);
        CHECK(chooseSocket(found, 77, isAlive) == 0);
        CHECK(chooseSocket({}, 0, isAlive) == 0);
        // Every live one, newest first, each pid once.
        std::vector<SocketEntry> two = {{10, 100}, {20, 300}, {30, 200}, {10, 400}};
        CHECK((rankSockets(two, 0, isAlive) == std::vector<int>{10, 30}));
        CHECK((rankSockets(two, 30, isAlive) == std::vector<int>{30}));
        CHECK(rankSockets(two, 20, isAlive).empty());
    }

    GROUP("change:prompt-files");
    {
        using ChangeRequest::promptFileIsStale;
        auto only42 = [](int pid) { return pid == 42; };
        CHECK(!promptFileIsStale("request-42-3.txt", 60, only42));       // its writer runs
        CHECK(promptFileIsStale("request-41-3.txt", 60, only42));        // its writer has gone
        CHECK(promptFileIsStale("task-42-1.txt", 2 * 24 * 3600, only42));   // a day old
        CHECK(!promptFileIsStale("task-42-1.txt", 10, only42));
        CHECK(!promptFileIsStale("notes.txt", 1e9, only42));             // not one of ours
        CHECK(!promptFileIsStale("request-x-1.txt", 1e9, only42) &&
              !promptFileIsStale("request-42.txt", 1e9, only42) &&
              !promptFileIsStale("task-42-1.md", 1e9, only42) &&
              !promptFileIsStale("request--1.txt", 1e9, only42));
    }

    GROUP("control:pretty");
    {
        Json j = Json::object({{"a", 1}, {"b", Json::array()}, {"c", Json::object({{"d", "x"}})}});
        Json list = Json::array();
        list.push(true);
        list.push(Json());
        j.set("e", list);
        std::string p = prettyJson(j);
        CHECK(p == "{\n  \"a\": 1,\n  \"b\": [],\n  \"c\": {\n    \"d\": \"x\"\n  },\n"
                   "  \"e\": [\n    true,\n    null\n  ]\n}");
        Json back;
        CHECK(Json::parse(p, back) && back == j);
    }
}

static void testSideBySide() {
    using namespace SideBySide;
    GROUP("sidebyside:align");
    auto kinds = [](const std::vector<Row> &rows) {
        std::string s;
        for (const Row &r : rows)
            s += r.kind == Kind::Same ? '=' : r.kind == Kind::Changed ? '~'
                 : r.kind == Kind::Removed ? '-' : '+';
        return s;
    };
    CHECK(lineCount(u"") == 0 && lineCount(u"a") == 1 && lineCount(u"a\n") == 1 &&
          lineCount(u"a\nb") == 2 && lineCount(u"\n\n") == 2);
    CHECK(align(u"", u"").empty());
    CHECK(kinds(align(u"a\nb\n", u"a\nb\n")) == "==");
    CHECK(kinds(align(u"a\nb", u"a\nb\n")) == "==");   // the last newline alone is no change
    CHECK(kinds(align(u"a\nb\nc\n", u"a\nB\nc\n")) == "=~=");
    CHECK(kinds(align(u"a\nc\n", u"a\nb\nc\n")) == "=+=");
    CHECK(kinds(align(u"a\nb\nc\n", u"a\nc\n")) == "=-=");
    CHECK(kinds(align(u"", u"x\ny\n")) == "++");
    CHECK(kinds(align(u"x\ny\n", u"")) == "--");
    // A run of two old lines becoming three: two pairs and one added.
    std::vector<Row> rows = align(u"a\nb\nc\nz\n", u"a\nB\nC\nD\nz\n");
    CHECK(kinds(rows) == "=~~+=");
    CHECK(rows[3].left == -1 && rows[3].right == 3 && rows[4].left == 3 && rows[4].right == 4);
    // Both sides' line numbers run in order, each line exactly once.
    std::u16string a = u"1\n2\n3\n4\n5\n6\n7\n8\n", b = u"1\n2\nx\n4\n6\n7\ny\nz\n8\n9\n";
    rows = align(a, b);
    int nl = 0, nr = 0;
    for (const Row &r : rows) {
        if (r.left >= 0) CHECK(r.left == nl++);
        if (r.right >= 0) CHECK(r.right == nr++);
        CHECK(r.left >= 0 || r.right >= 0);
    }
    CHECK(nl == 8 && nr == 10);
    std::vector<size_t> starts = changeStarts(rows);
    CHECK(!starts.empty() && rows[starts[0]].kind != Kind::Same);
    CHECK(changeStarts(align(u"a\n", u"a\n")).empty());
}
