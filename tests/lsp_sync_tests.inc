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
            std::u16string mirror = t.str();
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
    {
        // The same across chunks (SyncedText::kChunk units each): texts of
        // tens of thousands of units, replacements small and spanning
        // chunks, deletions down to nothing and back, a \r and \n meeting
        // across a chunk's edge. Positions agree with PositionIndex at
        // random offsets and every chunk edge's neighbours, and the number
        // of chunks follows the size.
        std::mt19937 rng(8112026);
        const std::u16string pieces[] = {u"abc", u"\n", u"\r", u"\r\n", u"\U0001F600", u"x y"};
        auto randomText = [&](size_t n) {
            std::u16string s;
            while (s.size() < n) s += pieces[rng() % 6];
            return s;
        };
        bool same = true, bounded = true;
        size_t maxChunks = 0;
        for (int round = 0; round < 40 && same; round++) {
            std::u16string mirror = randomText(1 + rng() % 40000);
            Lsp::SyncedText t(mirror);
            for (int step = 0; step < 60 && same; step++) {
                const size_t a = rng() % (mirror.size() + 1);
                const size_t span = rng() % 4 == 0 ? 20000 : 12;
                const size_t b = a + rng() % (std::min(span, mirror.size() - a) + 1);
                const int kind = (int)(rng() % 10);
                const std::u16string with = kind == 0   ? std::u16string()
                                            : kind == 1 ? randomText(rng() % 15000)
                                                        : randomText(rng() % 6);
                if (kind == 2 && step % 7 == 0) {
                    t.replace(0, mirror.size(), u"");   // everything goes
                    mirror.clear();
                }
                t.replace(a > mirror.size() ? mirror.size() : a,
                          b > mirror.size() ? mirror.size() : b, with);
                mirror.replace(std::min(a, mirror.size()),
                               std::min(b, mirror.size()) - std::min(a, mirror.size()), with);
                same = t == mirror && t.size() == mirror.size() && t.str() == mirror &&
                       *t.snapshot() == mirror;
                const Lsp::PositionIndex index(mirror);
                same = same && t.lines() == (size_t)index.position(mirror.size()).line + 1;
                std::vector<size_t> at;
                for (int k = 0; k < 200; k++) at.push_back(rng() % (mirror.size() + 1));
                for (size_t e = 0; e <= mirror.size(); e += Lsp::SyncedText::kChunk / 4)
                    for (size_t d = 0; d < 3; d++)
                        if (e + d >= 1) at.push_back(std::min(mirror.size(), e + d - 1));
                for (size_t k : at) same = same && t.position(k) == index.position(k);
                for (size_t k = 0; k < mirror.size() && same && k < 64; k++)
                    same = t.at(k) == mirror[k];
                maxChunks = std::max(maxChunks, t.chunks());
                bounded = bounded &&
                          t.chunks() <= mirror.size() / (Lsp::SyncedText::kChunk / 4) + 1;
            }
        }
        CHECK(same && bounded);
        if (!bounded) std::printf("  as many as %zu chunks\n", maxChunks);
        // A \r ending one chunk and a \n starting the next are one break.
        std::u16string edge(Lsp::SyncedText::kChunk - 1, u'a');
        edge += u"\r\nb";
        Lsp::SyncedText e(edge);
        CHECK(e.chunks() == 2 && e.lines() == 2 &&
              e.position(edge.size() - 1) == (Lsp::Position{1, 0}));
        e.replace(Lsp::SyncedText::kChunk, Lsp::SyncedText::kChunk + 1, u"c");   // \rcb
        CHECK(e.lines() == 2 && e.position(Lsp::SyncedText::kChunk) == (Lsp::Position{1, 0}));
        e.replace(Lsp::SyncedText::kChunk, Lsp::SyncedText::kChunk, u"\n");      // \r\ncb
        CHECK(e.lines() == 2 && e.position(Lsp::SyncedText::kChunk + 1) == (Lsp::Position{1, 0}));
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
                auto copy = client.textFor(uri);
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
            auto copy = client.textFor("file:///p/b.py");
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

    GROUP("lsp:sync-scale");
    {
        // A flush costs what was typed, not the file: 300 flushes of two
        // edits each (one near the start, one in the middle, a line break
        // among them now and then) in a 1 MB text and in a 16 MB one. The
        // copy as one string moved its whole tail and shifted every later
        // line start per edit, about 2 ms a flush at 16 MB.
        auto run = [](size_t units, bool &right) {
            size_t bytes = 0;
            Lsp::Client client([&](const std::string &b) { bytes += b.size(); });
            client.initialize("/p", 1);
            client.receive(reply(1, "{\"capabilities\":{\"textDocumentSync\":2}}"));
            std::u16string buffer;
            buffer.reserve(units + 64);
            while (buffer.size() < units)
                buffer += u"    let value_" + Lsp::toUtf16(std::to_string(buffer.size() % 997)) +
                          u" = compute(é, 42);\n";
            client.didOpen("file:///p/big.rs", "rust", Lsp::toUtf8(buffer));
            double spent = 0;
            for (int i = 0; i < 300; i++) {
                Lsp::ChangeTracker t;
                const size_t mid = buffer.size() / 2 + (size_t)i * 7;
                const std::u16string typed = i % 25 == 0 ? u"\n" : u"x";
                buffer.insert(mid, typed);
                t.edit(mid, 0, typed.size());
                buffer.erase(20 + (size_t)i, 1);
                t.edit(20 + (size_t)i, 1, 0);
                const double t0 = cpuMs();
                client.didChange("file:///p/big.rs", buffer.size(), t,
                                 [&](size_t a, size_t n) { return buffer.substr(a, n); });
                spent += cpuMs() - t0;
            }
            right = client.rangedChangesSent() == 300 && *client.textFor("file:///p/big.rs") ==
                                                              buffer;
            (void)bytes;
            return spent;
        };
        bool smallRight = false, bigRight = false;
        const double small = run(1u << 20, smallRight);
        const double big = run(16u << 20, bigRight);
        CHECK(smallRight && bigRight);
        std::printf("  sync scale: 300 flushes %.2f ms of CPU at 1 MB, %.2f ms at 16 MB\n", small,
                    big);
        CHECK_TIME(big, 100);
        CHECK(big < small * 4 + 15 || timingSkipped());
    }
}
