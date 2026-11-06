// lsp_watch_tests.inc — the `lsp:glob` and `lsp:watch-*` groups of
// run_tests.cpp: workspace/didChangeWatchedFiles. The LSP's glob, the
// watchers a server registers through client/registerCapability (and
// unregisters), which changes reach which server, and how one batch of
// file-system events becomes the changes sent. Included there, after the
// fake server and the Json helpers.

static void testLspWatchedFiles() {
    GROUP("lsp:glob");
    {
        using Lsp::globMatch;
        // * and ? stay within a segment.
        CHECK(globMatch("*.py", "a.py") && globMatch("*.py", ".py") && !globMatch("*.py", "d/a.py"));
        CHECK(globMatch("a?c", "abc") && !globMatch("a?c", "a/c") && !globMatch("a?c", "ac"));
        CHECK(globMatch("*", "anything") && !globMatch("*", "a/b"));
        // ** is any number of whole segments, none included.
        CHECK(globMatch("**/*.py", "a.py") && globMatch("**/*.py", "x/y/a.py"));
        CHECK(!globMatch("**/*.py", "x/y/a.pyc") && !globMatch("**/*.py", "x/a.py/b"));
        CHECK(globMatch("**", "a") && globMatch("**", "a/b/c") && globMatch("**", ""));
        CHECK(globMatch("src/**/test/*.ts", "src/test/a.ts") &&
              globMatch("src/**/test/*.ts", "src/x/y/test/a.ts") &&
              !globMatch("src/**/test/*.ts", "lib/test/a.ts"));
        CHECK(globMatch("a/**", "a/b/c") && globMatch("a/**", "a") && !globMatch("a/**", "ab"));
        // ** inside a segment is a plain star.
        CHECK(globMatch("a**b", "axxb") && !globMatch("a**b", "ax/xb"));
        // Alternatives, nested ones too.
        CHECK(globMatch("**/*.{py,pyi}", "pkg/m.pyi") && globMatch("**/*.{py,pyi}", "m.py") &&
              !globMatch("**/*.{py,pyi}", "m.pyx"));
        CHECK(globMatch("{a,b{c,d}}.txt", "bd.txt") && globMatch("{a,b{c,d}}.txt", "a.txt") &&
              !globMatch("{a,b{c,d}}.txt", "b.txt"));
        CHECK(globMatch("**/{pyproject.toml,ruff.toml}", "x/ruff.toml"));
        CHECK(globMatch("{,.}ruff.toml", ".ruff.toml") && globMatch("{,.}ruff.toml", "ruff.toml"));
        // Character sets, negated ones, never a slash.
        CHECK(globMatch("example.[0-9]", "example.7") && !globMatch("example.[0-9]", "example.a"));
        CHECK(globMatch("example.[!0-9]", "example.a") && !globMatch("example.[!0-9]", "example.3"));
        CHECK(!globMatch("a[/]b", "a/b") && globMatch("[]]", "]"));
        // Unclosed braces and brackets stand for themselves.
        CHECK(globMatch("a{b", "a{b") && globMatch("a[b", "a[b") && !globMatch("a[b", "ab"));
        // Whole and case-sensitive.
        CHECK(!globMatch("*.py", "a.PY") && !globMatch("a.py", "xa.py") && !globMatch("a", "ab"));
        // Many stars against a long name stay quick (memoized).
        std::string longName(400, 'a');
        CHECK(!globMatch("*a*a*a*a*a*a*a*a*a*a*b", longName));
        CHECK(globMatch("**/**/**/**/x", "a/b/c/d/e/f/g/x"));
    }

    GROUP("lsp:watch-parse");
    {
        auto ws = Lsp::parseFileWatchers(parsed(
            "{\"watchers\":["
            "{\"globPattern\":\"**/*.py\"},"
            "{\"globPattern\":\"**/pyrightconfig.json\",\"kind\":5},"
            "{\"globPattern\":{\"baseUri\":\"file:///opt/lib/python3/site-packages\","
            "\"pattern\":\"**\"},\"kind\":7},"
            "{\"globPattern\":{\"baseUri\":{\"uri\":\"file:///w\",\"name\":\"w\"},"
            "\"pattern\":\"*.toml\"}},"
            "{\"globPattern\":{\"baseUri\":\"https://example.com/x\",\"pattern\":\"**\"}},"
            "{\"globPattern\":42},"
            "{\"globPattern\":\"\"}]}"));
        CHECK(ws.size() == 4);
        if (ws.size() == 4) {
            CHECK(ws[0].pattern == "**/*.py" && ws[0].base.empty() && ws[0].kind == 7);
            CHECK(ws[1].kind == 5);
            CHECK(ws[2].base == "/opt/lib/python3/site-packages" && ws[2].pattern == "**");
            CHECK(ws[3].base == "/w" && ws[3].pattern == "*.toml");   // a WorkspaceFolder
        }
        CHECK(Lsp::parseFileWatchers(Json()).empty());
    }

    GROUP("lsp:watch-match");
    {
        using Lsp::FileChange;
        using Lsp::FileChangeType;
        const std::string root = "/w";
        auto change = [](const std::string &p, FileChangeType t) { return FileChange{p, t}; };
        Lsp::FileWatcher py{"**/*.py", "", 7};
        CHECK(Lsp::watcherMatches(py, root, change("/w/pkg/a.py", FileChangeType::Created)));
        CHECK(!Lsp::watcherMatches(py, root, change("/w/notes.txt", FileChangeType::Changed)));
        // A pattern with no slash is about the name.
        Lsp::FileWatcher name{"ruff.toml", "", 7};
        CHECK(Lsp::watcherMatches(name, root, change("/w/sub/ruff.toml", FileChangeType::Changed)));
        // An absolute one is about the whole path.
        Lsp::FileWatcher abs{"/w/src/**/*.c", "", 7};
        CHECK(Lsp::watcherMatches(abs, root, change("/w/src/x/m.c", FileChangeType::Changed)) &&
              !Lsp::watcherMatches(abs, root, change("/v/src/m.c", FileChangeType::Changed)));
        // A relative one without ** is matched below the root.
        Lsp::FileWatcher rel{"src/*.c", "", 7};
        CHECK(Lsp::watcherMatches(rel, root, change("/w/src/m.c", FileChangeType::Changed)) &&
              !Lsp::watcherMatches(rel, root, change("/w/lib/src/m.c", FileChangeType::Changed)));
        // A RelativePattern, below its base only.
        Lsp::FileWatcher lib{"**", "/opt/site-packages", 7};
        CHECK(Lsp::watcherMatches(lib, root, change("/opt/site-packages/requests/__init__.py",
                                                    FileChangeType::Created)));
        CHECK(!Lsp::watcherMatches(lib, root, change("/opt/site-packages-old/x.py",
                                                     FileChangeType::Created)));
        CHECK(!Lsp::watcherMatches(lib, root, change("/opt/site-packages", FileChangeType::Deleted)));
        Lsp::FileWatcher toml{"*.toml", "/w/", 7};   // a trailing slash on the base
        CHECK(Lsp::watcherMatches(toml, root, change("/w/ruff.toml", FileChangeType::Changed)) &&
              !Lsp::watcherMatches(toml, root, change("/w/sub/ruff.toml", FileChangeType::Changed)));
        // Kinds: 1 created, 2 changed, 4 deleted.
        Lsp::FileWatcher noChange{"**/*.py", "", 5};
        CHECK(Lsp::watcherMatches(noChange, root, change("/w/a.py", FileChangeType::Created)) &&
              !Lsp::watcherMatches(noChange, root, change("/w/a.py", FileChangeType::Changed)) &&
              Lsp::watcherMatches(noChange, root, change("/w/a.py", FileChangeType::Deleted)));
    }

    GROUP("lsp:watch-events");
    {
        using Lsp::FileChange;
        using Lsp::FileChangeType;
        {
            Lsp::FileEventClassifier k;
            // New, then written again with FSEvents' created flag still on.
            CHECK(k.classify("/w/a.py", true, true) == FileChangeType::Created);
            CHECK(k.classify("/w/a.py", true, true) == FileChangeType::Changed);
            CHECK(k.classify("/w/a.py", true, false) == FileChangeType::Changed);
            // Gone, then back.
            CHECK(k.classify("/w/a.py", false, true) == FileChangeType::Deleted);
            CHECK(k.classify("/w/a.py", false, false) == FileChangeType::Deleted);
            CHECK(k.classify("/w/a.py", true, true) == FileChangeType::Created);
            // A file there before the watcher began, written: Changed.
            CHECK(k.classify("/w/old.py", true, false) == FileChangeType::Changed);
            CHECK(k.classify("/w/old.py", true, true) == FileChangeType::Changed);
            // A rename: the old name gone, the new one there.
            CHECK(k.classify("/w/x.py", false, true) == FileChangeType::Deleted);
            CHECK(k.classify("/w/y.py", true, true) == FileChangeType::Created);
        }
        auto c = Lsp::coalesceFileChanges({{"/a", FileChangeType::Created},
                                           {"/b", FileChangeType::Changed},
                                           {"/a", FileChangeType::Changed},
                                           {"/c", FileChangeType::Created},
                                           {"/c", FileChangeType::Deleted},
                                           {"/d", FileChangeType::Deleted},
                                           {"/d", FileChangeType::Created},
                                           {"/b", FileChangeType::Deleted},
                                           {"/c", FileChangeType::Created}});
        CHECK(c.size() == 4);
        if (c.size() == 4) {
            CHECK(c[0].path == "/a" && c[0].type == FileChangeType::Created);
            CHECK(c[1].path == "/b" && c[1].type == FileChangeType::Deleted);
            CHECK(c[2].path == "/d" && c[2].type == FileChangeType::Changed);
            CHECK(c[3].path == "/c" && c[3].type == FileChangeType::Created);
        }
        CHECK(Lsp::watchExcluded("/w/.git/index") && Lsp::watchExcluded("/w/.git") &&
              Lsp::watchExcluded("/w/sub/.git/HEAD"));
        CHECK(!Lsp::watchExcluded("/w/.github/workflows/ci.yml") &&
              !Lsp::watchExcluded("/w/.gitignore") && !Lsp::watchExcluded("/w/a.git/x"));
        CHECK(Lsp::watchExcluded("/w/.hg/store/data") && !Lsp::watchExcluded("/w/.hg/hgrc"));
        CHECK(Lsp::watchExcluded("/w/node_modules/react/index.js") &&
              !Lsp::watchExcluded("/w/node_modules/react") &&
              !Lsp::watchExcluded("/w/node_modules/.package-lock.json"));
        CHECK(!Lsp::watchExcluded("/w/.venv/lib/python3.13/site-packages/x.py"));
    }

    GROUP("lsp:watch-register");
    {
        FakeServer srv;
        Lsp::Client client([&](const std::string &b) { srv.take(b); });
        int changes = 0;
        client.onWatchersChanged = [&] { changes++; };
        client.setFileWatching(true);
        client.initialize("/w", 1);
        const Json &caps = srv.sent[0]["params"]["capabilities"]["workspace"];
        CHECK(caps["didChangeWatchedFiles"]["dynamicRegistration"].asBool() &&
              caps["didChangeWatchedFiles"]["relativePatternSupport"].asBool());
        // Off (the GTK port, which sends none), it is not offered, so a
        // server that can watches for itself.
        {
            FakeServer s2;
            Lsp::Client plain([&](const std::string &b) { s2.take(b); });
            plain.initialize("/w", 1);
            CHECK(!s2.sent[0]["params"]["capabilities"]["workspace"].has("didChangeWatchedFiles"));
            CHECK(s2.sent[0]["params"]["capabilities"]["workspace"]["applyEdit"].asBool());
        }
        // Nothing goes before the server is ready or has registered.
        CHECK(client.didChangeWatchedFiles({{"/w/a.py", Lsp::FileChangeType::Created}}) == 0);
        client.receive(reply(1, "{\"capabilities\":{}}"));
        CHECK(client.state() == Lsp::Client::State::Ready && !client.watchesFiles());
        size_t before = srv.sent.size();
        CHECK(client.didChangeWatchedFiles({{"/w/a.py", Lsp::FileChangeType::Created}}) == 0);
        CHECK(srv.sent.size() == before);

        // Two registrations in one request, one of them for something else.
        client.receive(Lsp::Framer::frame(
            "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"client/registerCapability\","
            "\"params\":{\"registrations\":["
            "{\"id\":\"py\",\"method\":\"workspace/didChangeWatchedFiles\","
            "\"registerOptions\":{\"watchers\":[{\"globPattern\":\"**/*.{py,pyi}\"},"
            "{\"globPattern\":{\"baseUri\":\"file:///opt/lib\",\"pattern\":\"**\"},\"kind\":4}]}},"
            "{\"id\":\"fmt\",\"method\":\"textDocument/formatting\",\"registerOptions\":{}}]}}"));
        CHECK(srv.sent.back()["id"].asInt() == 7 && srv.sent.back().has("result") &&
              srv.sent.back()["result"].isNull() && !srv.sent.back().has("error"));
        CHECK(changes == 1 && client.watchesFiles() && client.fileWatchers().size() == 2);
        CHECK(client.watchedBases().size() == 1 && client.watchedBases()[0] == "/opt/lib");

        // Only what some watcher wants, in one notification.
        before = srv.sent.size();
        size_t n = client.didChangeWatchedFiles({{"/w/pkg/a.py", Lsp::FileChangeType::Created},
                                                 {"/w/notes.txt", Lsp::FileChangeType::Changed},
                                                 {"/w/my file.pyi", Lsp::FileChangeType::Changed},
                                                 {"/opt/lib/m.py", Lsp::FileChangeType::Changed},
                                                 {"/opt/lib/old.so", Lsp::FileChangeType::Deleted}});
        CHECK(n == 4 && srv.sent.size() == before + 1);
        const Json &note = srv.sent.back();
        CHECK(note["method"].asString() == "workspace/didChangeWatchedFiles" && !note.has("id"));
        const Json &list = note["params"]["changes"];
        CHECK(list.size() == 4);
        if (list.size() == 4) {
            CHECK(list[0]["uri"].asString() == "file:///w/pkg/a.py" && list[0]["type"].asInt() == 1);
            CHECK(list[1]["uri"].asString() == "file:///w/my%20file.pyi" &&
                  list[1]["type"].asInt() == 2);
            // m.py matches **/*.py, though the /opt/lib watcher wants deletions only.
            CHECK(list[2]["uri"].asString() == "file:///opt/lib/m.py");
            CHECK(list[3]["uri"].asString() == "file:///opt/lib/old.so" &&
                  list[3]["type"].asInt() == 3);
        }
        // Nothing wanted, nothing sent.
        before = srv.sent.size();
        std::vector<Lsp::FileChange> text = {{"/w/x.txt", Lsp::FileChangeType::Changed}};
        CHECK(client.didChangeWatchedFiles(text) == 0 && srv.sent.size() == before);

        // A second registration adds to the first; unregistering (in the
        // specification's spelling, and in the corrected one) takes it away.
        client.receive(Lsp::Framer::frame(
            "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"client/registerCapability\","
            "\"params\":{\"registrations\":[{\"id\":\"cfg\","
            "\"method\":\"workspace/didChangeWatchedFiles\","
            "\"registerOptions\":{\"watchers\":[{\"globPattern\":\"**/ruff.toml\"}]}}]}}"));
        CHECK(changes == 2 && client.fileWatchers().size() == 3);
        client.receive(Lsp::Framer::frame(
            "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"client/unregisterCapability\","
            "\"params\":{\"unregisterations\":[{\"id\":\"py\","
            "\"method\":\"workspace/didChangeWatchedFiles\"}]}}"));
        CHECK(srv.sent.back()["id"].asInt() == 9 && srv.sent.back()["result"].isNull());
        CHECK(changes == 3 && client.fileWatchers().size() == 1 && client.watchedBases().empty());
        CHECK(client.didChangeWatchedFiles({{"/w/a.py", Lsp::FileChangeType::Changed},
                                            {"/w/ruff.toml", Lsp::FileChangeType::Changed}}) == 1);
        // An id nobody registered changes nothing.
        client.receive(Lsp::Framer::frame(
            "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"client/unregisterCapability\","
            "\"params\":{\"unregistrations\":[{\"id\":\"nope\",\"method\":\"x\"}]}}"));
        CHECK(changes == 3 && client.watchesFiles());
        client.receive(Lsp::Framer::frame(
            "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"client/unregisterCapability\","
            "\"params\":{\"unregistrations\":[{\"id\":\"cfg\","
            "\"method\":\"workspace/didChangeWatchedFiles\"}]}}"));
        CHECK(changes == 4 && !client.watchesFiles());
        // Registering something else is acknowledged and changes nothing.
        client.receive(Lsp::Framer::frame(
            "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"client/registerCapability\","
            "\"params\":{\"registrations\":[{\"id\":\"x\",\"method\":\"textDocument/hover\"}]}}"));
        CHECK(srv.sent.back()["id"].asInt() == 12 && srv.sent.back()["result"].isNull());
        CHECK(changes == 4 && !client.watchesFiles());
    }
}
