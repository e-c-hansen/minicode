// lsp_fixes_tests.inc — the `lsp:index`, `lsp:framing-limits`,
// `lsp:versions-reopen`, `lsp:diagnostic-tags`, `lsp:stale-edits`,
// `lsp:store-generations`, `outline:long-headers`, `lsp:incremental-changes`,
// `lsp:late-additional-edits` and `lsp:taking-handler` groups of
// run_tests.cpp: the third review's fixes to the language server core.
// Included there, after the fake server and the Json helpers.

static void testLspReviewFixes() {
    GROUP("lsp:index");
    {
        // Every offset and position agrees with the scanning functions, on
        // texts with every kind of line break, empty lines, and a break at
        // either end.
        const char *texts[] = {"", "a", "\n", "\r", "\r\n", "ab\ncd\r\nef\rgh",
                               "\n\nx\r\r\n\n", "line\r\n", "caf\xC3\xA9 \xF0\x9F\x98\x80\nz"};
        bool same = true;
        for (const char *t : texts) {
            std::u16string u = Lsp::toUtf16(t);
            Lsp::PositionIndex index(u);
            for (size_t i = 0; i <= u.size() + 2; i++)
                if (!(index.position(i) == Lsp::positionForOffset(u, i))) same = false;
            for (int line = -1; line < 8; line++)
                for (int ch = -1; ch < 8; ch++)
                    if (index.offset({line, ch}) != Lsp::offsetForPosition(u, {line, ch}))
                        same = false;
            for (int line = -1; line < 8; line++)
                if (Lsp::lineOf(index, line) != Lsp::lineOf(u, line)) same = false;
        }
        CHECK(same);
        // Random texts too.
        unsigned seed = 7;
        auto next = [&] { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7fff; };
        bool random = true;
        for (int round = 0; round < 200; round++) {
            std::u16string u;
            const int n = (int)(next() % 60);
            for (int k = 0; k < n; k++) {
                unsigned r = next() % 8;
                u += r == 0 ? u'\n' : r == 1 ? u'\r' : (char16_t)(u'a' + r);
            }
            Lsp::PositionIndex index(u);
            for (size_t i = 0; i <= u.size(); i++)
                if (!(index.position(i) == Lsp::positionForOffset(u, i))) random = false;
            for (int line = 0; line < 12; line++)
                for (int ch = 0; ch < 6; ch++)
                    if (index.offset({line, ch}) != Lsp::offsetForPosition(u, {line, ch}))
                        random = false;
        }
        CHECK(random);
        // Many diagnostics in a big file: linear, where every conversion
        // used to scan from the start (3,000 of them over 20,000 lines took
        // about a second, unoptimized, before).
        std::string big;
        for (int i = 0; i < 20000; i++) big += "value_" + std::to_string(i) + " = compute(x)\n";
        std::u16string u = Lsp::toUtf16(big);
        std::vector<Lsp::Diagnostic> diags;
        for (int i = 0; i < 3000; i++) {
            Lsp::Diagnostic d;
            int line = (i * 7) % 20000;
            d.range = Lsp::Range{{line, 0}, {line, 5}};
            diags.push_back(d);
        }
        auto t0 = std::chrono::steady_clock::now();
        std::vector<Lsp::DiagnosticSpan> spans = Lsp::diagnosticSpans(nullptr, u, diags);
        Lsp::PositionIndex index(u);
        bool placed = spans.size() == 3000;
        for (const Lsp::DiagnosticSpan &sp : spans)
            if (index.position(sp.start).line != diags[sp.index].range.start.line) placed = false;
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count();
        CHECK(placed);
        CHECK(ms < 200);
        std::vector<Lsp::TextEdit> edits;
        for (int i = 0; i < 5000; i++)
            edits.push_back(Lsp::TextEdit{Lsp::Range{{i * 4, 0}, {i * 4, 5}}, "v"});
        t0 = std::chrono::steady_clock::now();
        std::vector<Lsp::OffsetEdit> oe = Lsp::offsetEdits(u, edits);
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                 .count();
        CHECK(oe.size() == 5000 && oe.back().start == 0 && ms < 200);
    }

    GROUP("lsp:framing-limits");
    {
        // A length past 19 digits made std::stoll throw, which took the app
        // down; now the header is skipped like any other unusable one, and
        // the next message still arrives.
        Lsp::Framer f;
        std::string body;
        f.feed("Content-Length: 99999999999999999999999999\r\n\r\n"
               "Content-Length: 2\r\n\r\n[]");
        bool threw = false;
        try {
            CHECK(f.next(body) && body == "[]");
        } catch (...) {
            threw = true;
        }
        CHECK(!threw);
        // Past 1 GiB is not believed either.
        Lsp::Framer g;
        g.feed("Content-Length: 2000000000\r\n\r\nContent-Length: 2\r\n\r\n{}");
        CHECK(g.next(body) && body == "{}");
        // A sign or a space inside is not a length.
        Lsp::Framer h;
        h.feed("Content-Length: -5\r\n\r\nContent-Length: 1 2\r\n\r\nContent-Length: 1\r\n\r\n7");
        CHECK(h.next(body) && body == "7");
    }

    GROUP("lsp:versions-reopen");
    {
        FakeServer srv;
        Lsp::Client client([&](const std::string &b) { srv.take(b); });
        client.initialize("/p", 1);
        client.receive(reply(1, "{\"capabilities\":{}}"));
        client.didOpen("file:///p/a.py", "python", "x = 1\n");
        client.didChange("file:///p/a.py", "x = 12\n");
        client.didChange("file:///p/a.py", "x = 123\n");
        client.didChange("file:///p/a.py", "x = 1234\n");   // version 4
        CHECK(client.version("file:///p/a.py") == 4);
        client.didClose("file:///p/a.py");
        // Reopened: the count goes on, so a publish for the discarded text
        // (version 4, still on its way) is stale.
        client.didOpen("file:///p/a.py", "python", "x = 1\n");
        CHECK(client.version("file:///p/a.py") == 5);
        CHECK(srv.sent.back()["params"]["textDocument"]["version"].asInt() == 5);
        int calls = 0;
        client.onDiagnostics = [&](const std::string &, const std::vector<Lsp::Diagnostic> &) {
            calls++;
        };
        auto publish = [&](int version) {
            client.receive(Lsp::Framer::frame(
                "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":"
                "{\"uri\":\"file:///p/a.py\",\"version\":" + std::to_string(version) +
                ",\"diagnostics\":[]}}"));
        };
        publish(4);
        CHECK(calls == 0 && client.staleDiagnosticsDropped() == 1);
        publish(5);
        CHECK(calls == 1);
        // Another file starts at 1 as before.
        client.didOpen("file:///p/b.py", "python", "y\n");
        CHECK(client.version("file:///p/b.py") == 1);
        Lsp::DocVersions v = client.versions();
        CHECK(v.size() == 2 && v["file:///p/a.py"] == 5 && v["file:///p/b.py"] == 1);
    }

    GROUP("lsp:diagnostic-tags");
    {
        Json params = parsed(
            "{\"uri\":\"file:///p/a.py\",\"diagnostics\":["
            "{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":1}},"
            "\"severity\":4,\"message\":\"x is not accessed\",\"tags\":[1]},"
            "{\"range\":{\"start\":{\"line\":1,\"character\":0},\"end\":{\"line\":1,\"character\":1}},"
            "\"severity\":2,\"message\":\"old\",\"tags\":[2,7]},"
            "{\"range\":{\"start\":{\"line\":2,\"character\":0},\"end\":{\"line\":2,\"character\":1}},"
            "\"message\":\"plain\",\"tags\":\"nonsense\"}]}");
        std::vector<Lsp::Diagnostic> d = Lsp::parseDiagnostics(params);
        CHECK(d.size() == 3);
        CHECK(d.size() == 3 && d[0].unnecessary && !d[0].deprecated &&
              d[0].severity == Lsp::Severity::Hint);
        CHECK(d.size() == 3 && !d[1].unnecessary && d[1].deprecated);
        CHECK(d.size() == 3 && !d[2].unnecessary && !d[2].deprecated);
        // The client says it understands them, and versions.
        FakeServer srv;
        Lsp::Client client([&](const std::string &b) { srv.take(b); });
        client.initialize("/p", 1);
        const Json &pd = srv.sent[0]["params"]["capabilities"]["textDocument"]["publishDiagnostics"];
        CHECK(pd["tagSupport"]["valueSet"].size() == 2 && pd["versionSupport"].asBool());
    }

    GROUP("lsp:stale-edits");
    {
        auto edit = [](const char *json) { return Lsp::parseWorkspaceEdit(parsed(json)); };
        // documentChanges carries the version each file's edits are for.
        Lsp::WorkspaceEdit we = edit(
            "{\"documentChanges\":["
            "{\"textDocument\":{\"uri\":\"file:///p/a.py\",\"version\":3},\"edits\":["
            "{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":1}},"
            "\"newText\":\"y\"}]},"
            "{\"textDocument\":{\"uri\":\"file:///p/b.py\",\"version\":null},\"edits\":["
            "{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":1}},"
            "\"newText\":\"y\"}]}]}");
        CHECK(we.files.size() == 2 && we.files[0].version == 3 && we.files[1].version == -1);
        CHECK(we.problem.empty());
        Lsp::DocVersions now = {{"file:///p/a.py", 3}};
        CHECK(Lsp::staleEditFile(we, {}, now).empty());
        // The file moved on since the server computed the edit.
        now["file:///p/a.py"] = 4;
        CHECK(Lsp::staleEditFile(we, {}, now) == "file:///p/a.py");
        // `changes` has no versions: what was open when asked decides.
        Lsp::WorkspaceEdit ch = edit(
            "{\"changes\":{\"file:///p/u.py\":[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
            "\"end\":{\"line\":0,\"character\":1}},\"newText\":\"z\"}]}}");
        Lsp::DocVersions asked = {{"file:///p/u.py", 7}, {"file:///p/other.py", 2}};
        CHECK(Lsp::staleEditFile(ch, asked, {{"file:///p/u.py", 7}, {"file:///p/other.py", 9}})
                  .empty());   // only the files edited count
        // util.py reloaded from disk while the server worked: didChange v8.
        CHECK(Lsp::staleEditFile(ch, asked, {{"file:///p/u.py", 8}}) == "file:///p/u.py");
        // Closed since (Don't Save): its text is now the disk's.
        CHECK(Lsp::staleEditFile(ch, asked, {}) == "file:///p/u.py");
        // Not open when asked: the edit is for the disk, which the
        // applier's own check covers.
        CHECK(Lsp::staleEditFile(ch, {}, {{"file:///p/u.py", 1}}).empty());
        // Another spelling of the same file is found by its path.
        CHECK(Lsp::staleEditFile(ch, {{"file:///p/%75.py", 7}}, {{"file:///p/u.py", 8}}) ==
              "file:///p/u.py");
        // Two entries for one file at different versions cannot both hold.
        Lsp::WorkspaceEdit two = edit(
            "{\"documentChanges\":["
            "{\"textDocument\":{\"uri\":\"file:///p/a.py\",\"version\":3},\"edits\":[]},"
            "{\"textDocument\":{\"uri\":\"file:///p/a.py\",\"version\":4},\"edits\":["
            "{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":1}},"
            "\"newText\":\"y\"}]}]}");
        CHECK(!two.problem.empty());
    }

    GROUP("lsp:store-generations");
    {
        Lsp::DiagnosticStore store;
        Lsp::Diagnostic d;
        d.message = "m";
        CHECK(store.generation("a") == 0);
        store.publish("a", "s1", {d}, nullptr);
        uint64_t g1 = store.generation("a");
        CHECK(g1 > 0 && store.generation("b") == 0);
        store.publish("b", "s1", {d}, nullptr);
        CHECK(store.generation("a") == g1);   // another file's publish leaves it
        store.publish("a", "s2", {d}, nullptr);
        uint64_t g2 = store.generation("a");
        CHECK(g2 > g1);
        CHECK(!store.erase("a", "s9") && store.generation("a") == g2);   // nothing changed
        CHECK(store.erase("a", "s2") && store.generation("a") > g2);
        uint64_t g3 = store.generation("a");
        store.clear();
        store.publish("a", "s1", {d}, nullptr);
        CHECK(store.generation("a") > g3);   // never an earlier value again
    }

    GROUP("outline:long-headers");
    {
        // 30,000 brace-free lines before one `{` took a minute: every tail
        // of the header was copied and classified. Only the last 20 lines
        // are tried now, which still finds a declaration over a few lines.
        std::string text;
        for (int i = 0; i < 30000; i++) text += "x" + std::to_string(i) + "\n";
        text += "void f(int a,\n       int b)\n{\n}\n";
        auto t0 = std::chrono::steady_clock::now();
        std::vector<Lsp::Symbol> syms = Outline::outline("a.c", Lsp::toUtf16(text));
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count();
        CHECK(ms < 2000);
        CHECK(syms.size() == 1 && syms.size() == 1 && syms[0].name == "f");
        std::string go = "var (\n\ta = 1\n\tb = 2\n)\n\nfunc (s *Server) Handle(\n\tw http.ResponseWriter,\n"
                         "\tr *http.Request,\n) {\n}\n";
        std::vector<Lsp::Symbol> gs = Outline::outline("s.go", Lsp::toUtf16(go));
        bool handle = false;
        for (const Lsp::Symbol &s : gs) if (s.name == "Handle") handle = true;
        CHECK(handle);
    }

    GROUP("lsp:incremental-changes");
    {
        // The tracker's regions, applied to the text as sent, give the text
        // now: each region's old range replaced by its new range's text, and
        // everything between them the same in both.
        auto regionsRebuild = [](const std::u16string &before, const std::u16string &after,
                                 const Lsp::ChangeTracker &t) {
            std::u16string out;
            size_t at = 0;
            for (const auto &r : t.regions()) {
                if (r.oldStart < at || r.oldEnd > before.size() || r.newEnd > after.size())
                    return false;
                out += before.substr(at, r.oldStart - at);
                out += after.substr(r.newStart, r.newEnd - r.newStart);
                at = r.oldEnd;
            }
            out += before.substr(at);
            return out == after;
        };
        // Random edits to texts with every kind of line break, accents and
        // a character outside the BMP (two UTF-16 units), as the storage
        // reports them; edits never split the pair, as NSString's do not.
        unsigned seed = 11;
        auto next = [&] { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7fff; };
        const std::u16string pieces[] = {u"a", u"b", u" ", u"\n", u"\r\n", u"\r", u"é",
                                         u"\U0001F600", u"xyz"};
        auto randomText = [&](int n) {
            std::u16string s;
            for (int k = 0; k < n; k++) s += pieces[next() % 9];
            return s;
        };
        auto boundary = [](const std::u16string &s, size_t at) {
            return at == 0 || at >= s.size() || !(s[at] >= 0xDC00 && s[at] <= 0xDFFF);
        };
        bool tracked = true, rebuilt = true;
        int ranged = 0, refused = 0;
        for (int round = 0; round < 3000; round++) {
            const std::u16string orig = randomText((int)(next() % 40));
            std::u16string now = orig;
            Lsp::ChangeTracker t;
            const int edits = 1 + (int)(next() % 6);
            for (int e = 0; e < edits; e++) {
                size_t start = now.empty() ? 0 : next() % (now.size() + 1);
                while (!boundary(now, start)) start--;
                size_t removed = now.size() - start ? next() % std::min<size_t>(now.size() - start + 1, 6) : 0;
                while (!boundary(now, start + removed)) removed++;
                const std::u16string ins = (next() % 3) ? randomText((int)(next() % 4)) : u"";
                now.replace(start, removed, ins);
                t.edit(start, removed, ins.size());
            }
            if (!regionsRebuild(orig, now, t)) tracked = false;
            std::vector<Lsp::ContentChange> changes;
            if (Lsp::rangedChanges(Lsp::toUtf8(orig), Lsp::toUtf8(now), t.regions(), changes)) {
                ranged++;
                if (Lsp::applyContentChanges(orig, changes) != now) rebuilt = false;
            } else {
                refused++;
            }
        }
        CHECK(tracked);
        CHECK(rebuilt);
        // Refused only where a boundary falls between \r and \n.
        CHECK(ranged > 2000);
        CHECK(refused > 0);
        // The ranges are UTF-16 positions in the text as sent, the last
        // region first: an emoji counts two, a CRLF ends one line.
        {
            const std::u16string orig = u"\U0001F600x\r\nab\r\ncd";
            std::u16string now = orig;
            Lsp::ChangeTracker t;
            now.replace(2, 1, u"Y");  t.edit(2, 1, 1);       // x -> Y
            now.replace(10, 0, u"!"); t.edit(10, 0, 1);      // after "cd"'s c
            std::vector<Lsp::ContentChange> changes;
            CHECK(Lsp::rangedChanges(Lsp::toUtf8(orig), Lsp::toUtf8(now), t.regions(), changes));
            CHECK(changes.size() == 2);
            if (changes.size() == 2) {
                CHECK(changes[0].range.start.line == 2 && changes[0].range.start.character == 1);
                CHECK(changes[0].range.end.line == 2 && changes[0].range.end.character == 1);
                CHECK(changes[0].text == "!");
                CHECK(changes[1].range.start.line == 0 && changes[1].range.start.character == 2);
                CHECK(changes[1].range.end.character == 3 && changes[1].text == "Y");
            }
            CHECK(Lsp::applyContentChanges(orig, changes) == now);
        }
        // A region that does not account for a difference elsewhere is
        // refused, and so is one past the end, or inside the emoji.
        {
            std::vector<Lsp::ContentChange> changes;
            Lsp::ChangeTracker t;
            t.edit(0, 1, 1);
            CHECK(!Lsp::rangedChanges("abc", "Xbd", t.regions(), changes));
            CHECK(Lsp::rangedChanges("abc", "Xbc", t.regions(), changes) && changes.size() == 1);
            Lsp::ChangeTracker past;
            past.edit(5, 1, 1);
            CHECK(!Lsp::rangedChanges("abc", "abc", past.regions(), changes));
            Lsp::ChangeTracker half;
            half.edit(1, 0, 1);
            CHECK(!Lsp::rangedChanges("\xF0\x9F\x98\x80", "\xF0\x9F" "x\x98\x80", half.regions(),
                                      changes));
        }
        // Edits next to each other merge into one region; past kMaxRegions
        // apart, the tracker gives up.
        {
            Lsp::ChangeTracker t;
            t.edit(3, 0, 1);
            t.edit(4, 0, 1);
            t.edit(2, 1, 0);
            CHECK(t.regions().size() == 1);
            CHECK(t.regions()[0].oldStart == 2 && t.regions()[0].oldEnd == 3);
            CHECK(t.regions()[0].newStart == 2 && t.regions()[0].newEnd == 4);
            Lsp::ChangeTracker many;
            for (size_t k = 0; k <= Lsp::ChangeTracker::kMaxRegions; k++) many.edit(k * 3, 1, 1);
            CHECK(many.overflowed() && many.regions().empty());
            many.reset();
            CHECK(many.empty());
            many.invalidate();
            CHECK(many.overflowed());
        }
        // The client: ranges for a server whose sync is incremental, the
        // whole text for one that wants it, for a batch larger than the
        // text, after the tracker gave up, and when the regions are wrong;
        // what the server rebuilds is the text every time.
        auto session = [&](Json sync, auto &&script) {
            FakeServer srv;
            Lsp::Client client([&](const std::string &b) { srv.take(b); });
            client.initialize("/p", 1);
            Json caps = Json::object({{"textDocumentSync", sync}});
            client.receive(reply(1, Json::object({{"capabilities", caps}}).dump()));
            const std::string uri = "file:///p/a.py";
            // Long enough that a few ranges come to less than the text.
            std::string start = "import os\r\nx = 1\n\U0001F600 = 2\n" + std::string(400, '#') + "\n";
            client.didOpen(uri, "python", start);
            std::u16string server = Lsp::toUtf16(start);
            auto change = [&](const std::string &now, const Lsp::ChangeTracker &t) {
                const size_t before = srv.sent.size();
                client.didChange(uri, now, t);
                if (srv.sent.size() == before) return std::string("none");
                const Json &msg = srv.sent.back();
                bool hasRange = false;
                for (const Json &c : msg["params"]["contentChanges"].elements()) {
                    std::vector<Lsp::ContentChange> one;
                    if (c.has("range")) {
                        hasRange = true;
                        const Json &r = c["range"];
                        one.push_back({{{(int)r["start"]["line"].asInt(), (int)r["start"]["character"].asInt()},
                                        {(int)r["end"]["line"].asInt(), (int)r["end"]["character"].asInt()}},
                                       c["text"].asString()});
                        server = Lsp::applyContentChanges(server, one);
                    } else {
                        server = Lsp::toUtf16(c["text"].asString());
                    }
                }
                return std::string(hasRange ? "ranges" : "full");
            };
            script(client, change, server, start);
        };
        session(Json(2), [&](Lsp::Client &client, auto &change, std::u16string &server,
                             const std::string &start) {
            CHECK(client.syncKind() == 2);
            // "x = 1" -> "x = 10", and a letter after the emoji.
            std::u16string now = Lsp::toUtf16(start);
            Lsp::ChangeTracker t;
            now.insert(16, u"0");                  t.edit(16, 0, 1);
            now.insert(20, u"z");                  t.edit(20, 0, 1);
            CHECK(change(Lsp::toUtf8(now), t) == "ranges");
            CHECK(server == now);
            CHECK(client.rangedChangesSent() == 1 && client.fullChangesSent() == 0);
            // The same text again sends nothing.
            CHECK(change(Lsp::toUtf8(now), Lsp::ChangeTracker()) == "none");
            // A tracker that missed an edit: the whole text, and still right.
            std::u16string next = now;
            next[0] = u'I';
            Lsp::ChangeTracker wrong;
            wrong.edit(5, 1, 1);
            CHECK(change(Lsp::toUtf8(next), wrong) == "full");
            CHECK(server == next);
            // Given up, or nothing tracked: the whole text.
            std::u16string third = next + u"y";
            Lsp::ChangeTracker gaveUp;
            gaveUp.invalidate();
            CHECK(change(Lsp::toUtf8(third), gaveUp) == "full");
            CHECK(server == third);
            // A batch as large as the text: the whole text.
            std::u16string all = u"completely different";
            Lsp::ChangeTracker big;
            big.edit(0, third.size(), all.size());
            CHECK(change(Lsp::toUtf8(all), big) == "full");
            CHECK(server == all);
        });
        session(Json::object({{"openClose", true}, {"change", 2}}),
                [&](Lsp::Client &client, auto &change, std::u16string &server, const std::string &start) {
            CHECK(client.syncKind() == 2);
            std::u16string now = Lsp::toUtf16(start);
            Lsp::ChangeTracker t;
            now.erase(0, 1);
            t.edit(0, 1, 0);
            CHECK(change(Lsp::toUtf8(now), t) == "ranges");
            CHECK(server == now);
        });
        session(Json::object({{"change", 1}}),
                [&](Lsp::Client &client, auto &change, std::u16string &server, const std::string &start) {
            CHECK(client.syncKind() == 1);
            std::u16string now = Lsp::toUtf16(start) + u"#";
            Lsp::ChangeTracker t;
            t.edit(now.size() - 1, 0, 1);
            CHECK(change(Lsp::toUtf8(now), t) == "full");
            CHECK(server == now);
        });
    }

    GROUP("lsp:late-additional-edits");
    {
        // An import arriving after the word: placed in the text as it is
        // now, when it lies before everything changed since.
        const std::u16string now = u"import os\n\nx = Path";
        std::vector<Lsp::TextEdit> imp = {{{{0, 0}, {0, 0}}, "from pathlib import Path\n"}};
        std::vector<Lsp::OffsetEdit> out;
        CHECK(Lsp::lateAdditionalEdits(now, imp, 15, out));
        CHECK(out.size() == 1 && out[0].start == 0 && out[0].length == 0 &&
              out[0].text == u"from pathlib import Path\n");
        // Past the limit (an edit there since): refused, nothing given.
        std::vector<Lsp::TextEdit> late = {{{{2, 0}, {2, 1}}, "y"}};
        CHECK(!Lsp::lateAdditionalEdits(now, late, 11, out) && out.empty());
        CHECK(Lsp::lateAdditionalEdits(now, late, 12, out) && out.size() == 1 && out[0].start == 11);
        // Sorted; two overlapping refused.
        std::vector<Lsp::TextEdit> two = {{{{1, 0}, {1, 0}}, "b"}, {{{0, 0}, {0, 6}}, "from"}};
        CHECK(Lsp::lateAdditionalEdits(now, two, 15, out) && out.size() == 2 &&
              out[0].start == 0 && out[1].start == 10);
        std::vector<Lsp::TextEdit> overlap = {{{{0, 0}, {0, 6}}, "a"}, {{{0, 3}, {0, 8}}, "b"}};
        CHECK(!Lsp::lateAdditionalEdits(now, overlap, 15, out));
    }

    GROUP("lsp:taking-handler");
    {
        // A completion handler may move the result away (to parse it on
        // another thread); other handlers see the same message unharmed.
        FakeServer srv;
        Lsp::Client client([&](const std::string &b) { srv.take(b); });
        client.initialize("/p", 1);
        client.receive(reply(1, "{\"capabilities\":{}}"));
        Json taken;
        const int id = client.completion("file:///p/a.py", {0, 0},
                                         [&](Json &result, const Json &) { taken = std::move(result); });
        std::string hover;
        const int h = client.hover("file:///p/a.py", {0, 0},
                                   [&](const Json &r, const Json &) { hover = r["contents"].asString(); });
        client.receive(reply(id, "{\"isIncomplete\":false,\"items\":[{\"label\":\"x\"}]}"));
        client.receive(reply(h, "{\"contents\":\"doc\"}"));
        CHECK(taken["items"].size() == 1 && taken["items"][0]["label"].asString() == "x");
        CHECK(hover == "doc");
        // A plain const handler still works for completion.
        bool seen = false;
        const int c2 = client.completion("file:///p/a.py", {0, 0},
                                         [&](const Json &r, const Json &) { seen = r.isArray(); });
        client.receive(reply(c2, "[]"));
        CHECK(seen);
    }
}
