// lsp_sync_tests.inc — the `lsp:synced-text` and `lsp:sync-oracle` groups
// of run_tests.cpp: the client's UTF-16 copy of each document with its
// line starts (SyncedText), and ranged didChange read from the editor only
// where it changed (Client::didChange with a TextReader), checked against
// a server that applies what it is sent. Included there, after the fake
// server and reply().

static void testLspSync() {
    GROUP("lsp:synced-text");
    {
        // The line starts kept through random replacements agree with
        // PositionIndex built afresh, at every offset, CRLF and lone CR
        // included (a \r and \n brought together or pulled apart).
        std::mt19937 rng(11082026);
        const std::u16string pieces[] = {u"a", u"bc", u"\n", u"\r", u"\r\n", u"\U0001F600", u" "};
        auto randomText = [&](int n) {
            std::u16string s;
            for (int i = 0; i < n; i++) s += pieces[rng() % 7];
            return s;
        };
        bool same = true;
        for (int round = 0; round < 2000 && same; round++) {
            Lsp::SyncedText t(randomText((int)(rng() % 20)));
            std::u16string mirror = t.text();
            for (int step = 0; step < 8 && same; step++) {
                const size_t a = rng() % (mirror.size() + 1);
                const size_t b = a + rng() % (mirror.size() - a + 1);
                const std::u16string with = randomText((int)(rng() % 4));
                t.replace(a, b, with);
                mirror.replace(a, b - a, with);
                const Lsp::PositionIndex index(mirror);
                same = t == mirror;
                for (size_t k = 0; k <= mirror.size() && same; k++)
                    same = t.position(k) == index.position(k);
            }
        }
        CHECK(same);
        Lsp::SyncedText crlf(u"a\r\nb\rc\n");
        CHECK(crlf.lines() == 4 && crlf.position(2) == (Lsp::Position{0, 2}) &&
              crlf.position(3) == (Lsp::Position{1, 0}) && crlf.position(5) == (Lsp::Position{2, 0}));
        crlf.replace(2, 2, u"x");   // a\rx\nb...: the \r now ends a line of its own
        CHECK(crlf.position(2) == (Lsp::Position{1, 0}) && crlf.lines() == 5);
        crlf.replace(2, 3, u"");    // a\r\nb again
        CHECK(crlf.position(3) == (Lsp::Position{1, 0}) && crlf.lines() == 4);
    }

    GROUP("lsp:sync-oracle");
    {
        // 5,000 random edit sequences, CRLF lines and characters outside
        // the BMP among them, each flushed a few times as the editor does
        // (the regions, and the text read only where they say): the server
        // applying what it was sent always holds the editor's text, and so
        // does the client's copy (Client::textFor).
        std::mt19937 rng(5000);
        const std::u16string pieces[] = {u"a", u"de", u"\n", u"\r\n", u"\r", u"\U0001F42A",
                                         u"é", u" ", u"\t"};
        auto randomText = [&](int n) {
            std::u16string s;
            for (int i = 0; i < n; i++) s += pieces[rng() % 9];
            return s;
        };
        size_t sequences = 0, wrong = 0, ranged = 0, full = 0, reads = 0, readUnits = 0;
        for (int round = 0; round < 5000; round++) {
            FakeServer srv;
            Lsp::Client client([&](const std::string &b) { srv.take(b); });
            client.initialize("/p", 1);
            client.receive(reply(1, "{\"capabilities\":{\"textDocumentSync\":2}}"));
            const std::string uri = "file:///p/a.py";
            // Long enough that a few ranges come to less than the text.
            std::u16string buffer = randomText(40);
            for (int k = 0; k < 12; k++)
                buffer += u"# padding line " + std::u16string(k % 2 ? u"\r\n" : u"\n") +
                          randomText(3);
            client.didOpen(uri, "python", Lsp::toUtf8(buffer));
            std::u16string server = buffer;
            for (int flush = 0; flush < 4; flush++) {
                Lsp::ChangeTracker t;
                const int edits = 1 + (int)(rng() % 5);
                for (int e = 0; e < edits; e++) {
                    const size_t a = rng() % (buffer.size() + 1);
                    const size_t removed = std::min<size_t>(rng() % 5, buffer.size() - a);
                    const std::u16string with = randomText((int)(rng() % 4));
                    buffer.replace(a, removed, with);
                    t.edit(a, removed, with.size());
                }
                const size_t before = srv.sent.size();
                client.didChange(uri, buffer.size(), t, [&](size_t a, size_t n) {
                    reads++;
                    readUnits += n;
                    return buffer.substr(a, n);
                });
                for (size_t m = before; m < srv.sent.size(); m++)
                    for (const Json &c : srv.sent[m]["params"]["contentChanges"].elements()) {
                        if (c.has("range")) {
                            const Json &r = c["range"];
                            server = Lsp::applyContentChanges(
                                server, {{{{(int)r["start"]["line"].asInt(),
                                            (int)r["start"]["character"].asInt()},
                                           {(int)r["end"]["line"].asInt(),
                                            (int)r["end"]["character"].asInt()}},
                                          c["text"].asString()}});
                        } else {
                            server = Lsp::toUtf16(c["text"].asString());
                        }
                    }
                // A lone surrogate, which an edit can leave, does not
                // survive UTF-8 on the wire: the server holds U+FFFD there.
                const std::u16string expect = Lsp::toUtf16(Lsp::toUtf8(buffer));
                const std::u16string *copy = client.textFor(uri);
                if (server != expect || !copy || *copy != buffer) wrong++;
            }
            ranged += client.rangedChangesSent();
            full += client.fullChangesSent();
            sequences++;
        }
        CHECK(sequences == 5000 && wrong == 0);
        if (wrong) std::printf("  %zu flushes left the server's copy wrong\n", wrong);
        // Many flushes went as ranges (random offsets land between \r and
        // \n or inside a pair often, which sends the whole text).
        CHECK(ranged > 5000);
        std::printf("  sync oracle: %zu ranged, %zu whole, %.1f units read per read\n", ranged,
                    full, reads ? (double)readUnits / (double)reads : 0.0);

        // The regions are trusted: an edit the tracker missed that keeps
        // the length goes unseen, unless the check is on, which repairs it.
        for (int checking = 0; checking < 2; checking++) {
            FakeServer srv;
            Lsp::Client client([&](const std::string &b) { srv.take(b); });
            client.setCheckSync(checking == 1);
            client.initialize("/p", 1);
            client.receive(reply(1, "{\"capabilities\":{\"textDocumentSync\":2}}"));
            std::u16string buffer = u"hello\n" + std::u16string(300, u'x') + u"\n";
            client.didOpen("file:///p/b.py", "python", Lsp::toUtf8(buffer));
            buffer[0] = u'J';    // not told
            buffer.insert(10, u"y");
            Lsp::ChangeTracker t;
            t.edit(10, 0, 1);
            client.didChange("file:///p/b.py", buffer.size(), t,
                             [&](size_t a, size_t n) { return buffer.substr(a, n); });
            const std::u16string *copy = client.textFor("file:///p/b.py");
            CHECK(copy && client.rangedChangesSent() == 1);
            if (checking) {
                CHECK(client.syncRepairs() == 1 && *copy == buffer && client.fullChangesSent() == 1);
                CHECK(srv.sent.back()["params"]["contentChanges"][0]["text"].asString() ==
                      Lsp::toUtf8(buffer));
            } else {
                CHECK(client.syncRepairs() == 0 && *copy != buffer);
                // didSave compares the whole text and puts it right.
                client.didSave("file:///p/b.py", Lsp::toUtf8(buffer));
                CHECK(*client.textFor("file:///p/b.py") == buffer && client.fullChangesSent() == 1);
            }
        }

        // Nothing edited: nothing read, nothing sent (every request flushes
        // first); a length that changed with no regions sends the whole.
        {
            FakeServer srv;
            Lsp::Client client([&](const std::string &b) { srv.take(b); });
            client.initialize("/p", 1);
            client.receive(reply(1, "{\"capabilities\":{\"textDocumentSync\":2}}"));
            std::u16string buffer = u"abc\n";
            client.didOpen("file:///p/c.py", "python", "abc\n");
            size_t n = 0;
            auto read = [&](size_t a, size_t len) {
                n++;
                return buffer.substr(a, len);
            };
            const size_t before = srv.sent.size();
            client.didChange("file:///p/c.py", buffer.size(), Lsp::ChangeTracker(), read);
            CHECK(n == 0 && srv.sent.size() == before);
            buffer += u"d";
            client.didChange("file:///p/c.py", buffer.size(), Lsp::ChangeTracker(), read);
            CHECK(srv.sent.size() == before + 1 && client.fullChangesSent() == 1 &&
                  *client.textFor("file:///p/c.py") == buffer);
        }
    }

    GROUP("lsp:sync-speed");
    {
        // A pause after typing two characters in a 2.5 MB file: the flush
        // reads what was typed and finds its positions from the kept line
        // starts. Converting and comparing the whole text (the old way)
        // is the baseline, timed on this thread in this step.
        FakeServer srv;
        Lsp::Client client([&](const std::string &b) { srv.take(b); });
        client.initialize("/p", 1);
        client.receive(reply(1, "{\"capabilities\":{\"textDocumentSync\":2}}"));
        std::u16string buffer;
        while (buffer.size() < 2500000)
            buffer += u"def f_" + Lsp::toUtf16(std::to_string(buffer.size())) +
                      u"(x):  # \U0001F600 é\r\n    return x\r\n";
        client.didOpen("file:///p/big.py", "python", Lsp::toUtf8(buffer));
        double t0 = cpuMs();
        const std::string whole = Lsp::toUtf8(buffer);
        const bool equal = whole == Lsp::toUtf8(*client.textFor("file:///p/big.py"));
        const double baseline = cpuMs() - t0;
        CHECK(equal);
        Lsp::ChangeTracker t;
        buffer.insert(1250000, u"zz");
        t.edit(1250000, 0, 2);
        buffer.insert(10, u"q");
        t.edit(10, 0, 1);
        t0 = cpuMs();
        client.didChange("file:///p/big.py", buffer.size(), t,
                         [&](size_t a, size_t n) { return buffer.substr(a, n); });
        const double flush = cpuMs() - t0;
        CHECK(client.rangedChangesSent() == 1 && *client.textFor("file:///p/big.py") == buffer);
        std::printf("  sync: a 2.5 MB file's flush %.2f ms of CPU (whole-text way %.2f ms)\n",
                    flush, baseline);
        CHECK(flush < baseline / 2 || flush < 1.0);
    }
}
