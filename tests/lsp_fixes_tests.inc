// lsp_fixes_tests.inc — the `lsp:index`, `lsp:framing-limits`,
// `lsp:versions-reopen`, `lsp:diagnostic-tags`, `lsp:stale-edits`,
// `lsp:store-generations` and `outline:long-headers` groups of
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
}
