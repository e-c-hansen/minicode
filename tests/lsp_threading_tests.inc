// lsp_threading_tests.inc — the `lsp:framer-chunks`, `lsp:reader-thread` and
// `lsp:progress` groups of run_tests.cpp: a server's output read into
// messages apart from the client (the Mac frames and parses on the server's
// read queue and hands each message to the client on the main thread), and
// $/progress as the status bar shows it. Included there, after the fake
// server and the Json helpers.
#include <condition_variable>
#include <mutex>
#include <thread>

static void testLspThreading() {
    GROUP("lsp:framer-chunks");
    {
        // Three messages, one of them not JSON, cut at every place: each
        // whole message comes out once, in order, wherever the cuts fall.
        const std::string a = Lsp::Framer::frame("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":"
                                                 "{\"s\":\"caf\xC3\xA9 \\r\\n\"}}");
        const std::string b = Lsp::Framer::frame("not json");
        const std::string c = Lsp::Framer::frame("{\"method\":\"x\",\"params\":[1,2,3]}");
        const std::string all = a + b + c;
        bool every = true;
        for (size_t i = 0; i <= all.size() && every; i++)
            for (size_t j = i; j <= all.size(); j += 7) {
                Lsp::Reader r;
                std::vector<Lsp::Incoming> got;
                Lsp::Incoming in;
                for (auto piece : {all.substr(0, i), all.substr(i, j - i), all.substr(j)}) {
                    r.feed(piece);
                    while (r.next(in)) got.push_back(in);
                }
                if (got.size() != 3 || got[0].message.isNull() ||
                    got[0].message["result"]["s"].asString() != "caf\xC3\xA9 \r\n" ||
                    got[1].error.empty() || !got[1].message.isNull() ||
                    !got[2].error.empty() || got[2].message["params"].size() != 3 ||
                    r.buffered() != 0) {
                    every = false;
                    break;
                }
            }
        CHECK(every);
        // Byte by byte, and nothing before the last byte of the body.
        {
            Lsp::Reader r;
            Lsp::Incoming in;
            size_t seen = 0;
            for (size_t i = 0; i < a.size(); i++) {
                r.feed(a.data() + i, 1);
                while (r.next(in)) seen++;
                if (i + 1 < a.size() && seen) break;
            }
            CHECK(seen == 1 && in.message["id"].asInt() == 1);
        }
        // A header with no usable length is skipped, and the next message
        // still comes through.
        {
            Lsp::Reader r;
            r.feed("Content-Length: x\r\n\r\n" + c);
            Lsp::Incoming in;
            CHECK(r.next(in) && in.message["method"].asString() == "x" && !r.next(in));
        }
        // Client::receive and deliver of a Reader's messages do the same.
        FakeServer s1, s2;
        Lsp::Client one([&](const std::string &x) { s1.take(x); });
        Lsp::Client two([&](const std::string &x) { s2.take(x); });
        std::vector<std::string> errs1, errs2;
        one.onProtocolError = [&](const std::string &e) { errs1.push_back(e); };
        two.onProtocolError = [&](const std::string &e) { errs2.push_back(e); };
        one.initialize("/w", 1);
        two.initialize("/w", 1);
        const std::string init = reply(1, "{\"capabilities\":{},\"serverInfo\":{\"name\":\"s\"}}");
        const std::string bad = Lsp::Framer::frame("{oops");
        one.receive(init + bad);
        Lsp::Reader r;
        r.feed(init + bad);
        Lsp::Incoming in;
        while (r.next(in)) two.deliver(in);
        CHECK(one.state() == Lsp::Client::State::Ready && two.state() == one.state());
        CHECK(two.serverName() == "s" && errs1.size() == 1 && errs1 == errs2);
        CHECK(s1.methods() == s2.methods());
        // Once exited, delivered messages are dropped, as receive drops them.
        int answered = 0;
        two.request("x/y", Json::object(), [&](const Json &, const Json &) { answered++; });
        two.exitNow();
        r.feed(reply(2, "1"));
        while (r.next(in)) two.deliver(in);
        CHECK(answered == 0);
    }

    GROUP("lsp:reader-thread");
    {
        // The Mac's arrangement: bytes framed and parsed on one thread, the
        // messages handled on another, in order. 2,000 messages in uneven
        // chunks reach the client's handlers in the order they were sent.
        FakeServer srv;
        Lsp::Client client([&](const std::string &x) { srv.take(x); });
        client.initialize("/w", 1);
        client.receive(reply(1, "{\"capabilities\":{}}"));
        std::vector<int> order;
        std::vector<int> ids;
        for (int k = 0; k < 2000; k++)
            ids.push_back(client.request("x/n", Json::object({{"k", k}}),
                                         [&order, k](const Json &r, const Json &) {
                                             if (r["k"].asInt() == k) order.push_back(k);
                                         }));
        std::string bytes;
        for (int k = 0; k < 2000; k++) bytes += reply(ids[(size_t)k], "{\"k\":" + std::to_string(k) + "}");
        std::mutex m;
        std::condition_variable cv;
        std::vector<Lsp::Incoming> handoff;
        bool finished = false;
        std::thread reader([&] {
            Lsp::Reader r;
            size_t at = 0, step = 1;
            while (at < bytes.size()) {
                size_t n = std::min(step, bytes.size() - at);
                r.feed(bytes.data() + at, n);
                at += n;
                step = step * 3 % 977 + 1;
                Lsp::Incoming in;
                std::vector<Lsp::Incoming> batch;
                while (r.next(in)) batch.push_back(std::move(in));
                if (batch.empty()) continue;
                std::lock_guard<std::mutex> lock(m);
                for (auto &x : batch) handoff.push_back(std::move(x));
                cv.notify_one();
            }
            std::lock_guard<std::mutex> lock(m);
            finished = true;
            cv.notify_one();
        });
        while (true) {
            std::vector<Lsp::Incoming> take;
            bool done;
            {
                std::unique_lock<std::mutex> lock(m);
                cv.wait(lock, [&] { return finished || !handoff.empty(); });
                take.swap(handoff);
                done = finished;
            }
            for (const auto &in : take) client.deliver(in);
            if (done) {
                std::lock_guard<std::mutex> lock(m);
                if (handoff.empty()) break;
            }
        }
        reader.join();
        bool inOrder = order.size() == 2000;
        for (size_t k = 0; inOrder && k < order.size(); k++) inOrder = order[k] == (int)k;
        CHECK(inOrder && client.pendingRequests() == 0);
    }

    GROUP("lsp:progress");
    {
        FakeServer srv;
        Lsp::Client client([&](const std::string &x) { srv.take(x); });
        int changes = 0;
        client.onProgress = [&] { changes++; };
        client.initialize("/w", 1);
        CHECK(srv.sent[0]["params"]["capabilities"]["window"]["workDoneProgress"].asBool());
        client.receive(reply(1, "{\"capabilities\":{}}"));
        auto progress = [&](const std::string &token, const std::string &value) {
            client.receive(Lsp::Framer::frame(
                "{\"jsonrpc\":\"2.0\",\"method\":\"$/progress\",\"params\":{\"token\":" + token +
                ",\"value\":" + value + "}}"));
        };
        auto text = [&] {
            const Lsp::Progress *p = client.progress();
            return p ? Lsp::progressText(*p) : std::string("-");
        };
        // The server makes a token (answered null), then begins.
        client.receive(Lsp::Framer::frame(
            "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"window/workDoneProgress/create\","
            "\"params\":{\"token\":\"idx\"}}"));
        CHECK(srv.sent.back()["id"].asInt() == 7 && srv.sent.back().has("result") &&
              srv.sent.back()["result"].isNull() && !srv.sent.back().has("error"));
        CHECK(text() == "-" && changes == 0);
        progress("\"idx\"", "{\"kind\":\"begin\",\"title\":\"Indexing\",\"percentage\":0}");
        CHECK(text() == "Indexing 0%" && changes == 1 && client.progressCount() == 1);
        progress("\"idx\"", "{\"kind\":\"report\",\"message\":\"3/10 files\",\"percentage\":30}");
        CHECK(text() == "Indexing: 3/10 files 30%" && changes == 2);
        // A report that leaves out the message keeps it; the percentage too.
        progress("\"idx\"", "{\"kind\":\"report\",\"percentage\":40.6}");
        CHECK(text() == "Indexing: 3/10 files 40%");
        progress("\"idx\"", "{\"kind\":\"report\",\"message\":\"4/10 files\"}");
        CHECK(text() == "Indexing: 4/10 files 40%");
        // A second token, a number, begun later: it is the one shown.
        progress("5", "{\"kind\":\"begin\",\"title\":\"Analyzing\"}");
        CHECK(text() == "Analyzing" && client.progressCount() == 2);
        // Reports on the older one change it, not which is shown.
        progress("\"idx\"", "{\"kind\":\"report\",\"percentage\":90}");
        CHECK(text() == "Analyzing");
        // The string "5" is another token than the number 5.
        progress("\"5\"", "{\"kind\":\"end\"}");
        CHECK(client.progressCount() == 2 && text() == "Analyzing");
        progress("5", "{\"kind\":\"end\"}");
        CHECK(text() == "Indexing: 4/10 files 90%" && client.progressCount() == 1);
        // An end, or a report, for a token never begun changes nothing.
        int before = changes;
        progress("\"nope\"", "{\"kind\":\"end\"}");
        progress("\"nope\"", "{\"kind\":\"report\",\"percentage\":5}");
        progress("\"idx\"", "{\"kind\":\"sideways\"}");
        progress("\"idx\"", "[1,2]");
        CHECK(changes == before && client.progressCount() == 1);
        progress("\"idx\"", "{\"kind\":\"end\",\"message\":\"done\"}");
        CHECK(text() == "-" && client.progressCount() == 0 && changes == before + 1);
        // Begun again under the same token: from scratch.
        progress("\"idx\"", "{\"kind\":\"begin\",\"title\":\"\",\"message\":\"a\\nb\"}");
        CHECK(text() == "a b");
        progress("\"idx\"", "{\"kind\":\"begin\",\"title\":\"\"}");
        CHECK(text() == "working\xE2\x80\xA6");
        progress("\"idx\"", "{\"kind\":\"begin\",\"title\":\"T\",\"percentage\":250}");
        CHECK(text() == "T 100%");

        // Cut to about 60 bytes, never inside a character.
        Lsp::Progress p;
        p.title = std::string(59, 'x') + "\xC3\xA9\xC3\xA9";
        CHECK(Lsp::progressText(p) == std::string(59, 'x') + "\xE2\x80\xA6");
        p.title = std::string(58, 'x') + "\xC3\xA9\xC3\xA9";
        CHECK(Lsp::progressText(p) == std::string(58, 'x') + "\xC3\xA9\xE2\x80\xA6");
        p.title = std::string(30, 'y');
        CHECK(Lsp::progressText(p) == p.title);

        // The status bar: the newest begun among the running servers stands
        // where "no problems" would, any counts after it.
        using S = Lsp::ServerState;
        Lsp::ServerStatus based{"basedpyright", S::Running, true};
        Lsp::ServerStatus ruff{"ruff", S::Running, false};
        based.progress = "Indexing 40%";
        based.progressBegun = 3;
        CHECK(Lsp::statusLine("Python", {based}, 0, 0).text == "basedpyright: Indexing 40%");
        CHECK(Lsp::statusLine("Python", {based}, 2, 0).text ==
              "basedpyright: Indexing 40%, 2 errors");
        CHECK(Lsp::statusLine("Python", {based, ruff}, 0, 1).text ==
              "basedpyright + ruff: Indexing 40%, 1 warning");
        ruff.progress = "Linting";
        ruff.progressBegun = 9;
        CHECK(Lsp::statusLine("Python", {based, ruff}, 0, 0).text == "basedpyright + ruff: Linting");
        ruff.progressBegun = 1;
        CHECK(Lsp::statusLine("Python", {based, ruff}, 0, 0).text ==
              "basedpyright + ruff: Indexing 40%");
        // A server not running shows none (its last report is stale).
        based.state = S::Restarting;
        ruff.progress.clear();
        CHECK(Lsp::statusLine("Python", {based, ruff}, 0, 0).text.find("Indexing") ==
              std::string::npos);
        // Without progress the line is as it always was.
        Lsp::ServerStatus plain{"basedpyright", S::Running, true};
        CHECK(Lsp::statusLine("Python", {plain}, 0, 0).text == "basedpyright: no problems");

        // A new client (the server restarted) starts with none.
        Lsp::Client fresh([](const std::string &) {});
        CHECK(fresh.progress() == nullptr);
    }
}
