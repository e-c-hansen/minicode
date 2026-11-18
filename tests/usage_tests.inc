// usage_tests.inc — the `usage:*` and `statusline:*` groups of run_tests.cpp
// (ClaudeUsage.h, ClaudeStatusLine.h: A13, the plan usage forecast), kept in
// their own file and included there.
#include "ClaudeStatusLine.h"
#include "ClaudeUsage.h"

namespace {

// An assistant line as Claude Code 2.1 writes it (fields it adds later are
// ignored; the content is a reply's, stepped over).
std::string usageLine(const std::string &msgId, const std::string &reqId, const std::string &stamp,
                      const std::string &model, const std::string &usage,
                      const std::string &session = "s1", bool sidechain = false,
                      const std::string &content = "[{\"type\":\"text\",\"text\":\"done\"}]") {
    std::string l = "{\"parentUuid\":\"p\",\"isSidechain\":";
    l += sidechain ? "true" : "false";
    l += ",\"type\":\"assistant\",\"message\":{\"id\":\"" + msgId + "\",\"type\":\"message\","
         "\"role\":\"assistant\",\"model\":\"" + model + "\",\"content\":" + content +
         ",\"usage\":" + usage + "}";
    if (!reqId.empty()) l += ",\"requestId\":\"" + reqId + "\"";
    l += ",\"sessionId\":\"" + session + "\",\"timestamp\":\"" + stamp + "\",\"version\":\"2.1.300\"}";
    return l;
}

std::string usageJson(int in, int out, int w5 = 0, int w1 = 0, int read = 0,
                      const char *speed = "standard") {
    char b[512];
    std::snprintf(b, sizeof b,
                  "{\"input_tokens\":%d,\"cache_creation_input_tokens\":%d,"
                  "\"cache_read_input_tokens\":%d,\"output_tokens\":%d,\"service_tier\":\"standard\","
                  "\"cache_creation\":{\"ephemeral_1h_input_tokens\":%d,\"ephemeral_5m_input_tokens\":%d},"
                  "\"speed\":\"%s\",\"server_tool_use\":{\"web_search_requests\":0}}",
                  in, w5 + w1, read, out, w1, w5, speed);
    return b;
}

ClaudeUsage::Entry costEntry(double t, double cost, const std::string &session = "s") {
    ClaudeUsage::Entry e;
    e.time = t;
    e.cost = cost;
    e.sessionId = session;
    return e;
}

bool usageNear(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

std::string readAll(const std::string &p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeAll(const std::string &p, const std::string &text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

static void testClaudeUsage() {
    using namespace ClaudeUsage;

    GROUP("usage:parse-line");
    {
        Entry e;
        std::string l = usageLine("msg_01", "req_01", "2026-11-17T22:18:42.513Z", "claude-opus-5-5",
                                  usageJson(4, 912, 100, 1358, 96280));
        CHECK(parseUsageLine(l.data(), l.size(), "file", &e));
        CHECK(e.messageId == "msg_01" && e.requestId == "req_01" && e.model == "claude-opus-5-5");
        CHECK(e.sessionId == "s1" && !e.sidechain && !e.fast && e.detailed);
        CHECK(e.tokens.input == 4 && e.tokens.output == 912 && e.tokens.cacheRead == 96280);
        CHECK(e.tokens.write1h == 1358 && e.tokens.write5m == 100);
        CHECK(usageNear(e.time, 1794953922.513, 1e-3));
        // The text of a reply that talks about usage, in its content, is
        // only a string: the real usage object is the one read.
        l = usageLine("msg_02", "req_02", "2026-11-17T22:18:43Z", "claude-sonnet-5-5",
                      usageJson(10, 20),
                      "s2", true,
                      "[{\"type\":\"text\",\"text\":\"{\\\"usage\\\":{\\\"input_tokens\\\":999}}\"},"
                      "{\"type\":\"tool_use\",\"id\":\"t\",\"name\":\"mcp\",\"input\":{\"usage\":{\"input_tokens\":5}}}]");
        CHECK(parseUsageLine(l.data(), l.size(), "file", &e));
        CHECK(e.tokens.input == 10 && e.tokens.output == 20 && e.sidechain && e.sessionId == "s2");
        // An older line: no cache split, no speed, no request id, no session.
        l = "{\"type\":\"assistant\",\"timestamp\":\"2025-06-01T10:00:00.000Z\",\"message\":{\"id\":\"msg_x\","
            "\"model\":\"claude-sonnet-4-20250514\",\"usage\":{\"input_tokens\":7,\"output_tokens\":3,"
            "\"cache_creation_input_tokens\":40}}}";
        CHECK(parseUsageLine(l.data(), l.size(), "abc", &e));
        CHECK(e.sessionId == "abc" && e.requestId.empty() && e.tokens.write5m == 40 &&
              e.tokens.write1h == 0 && !e.detailed);
        // Not usage: a user line, a line with no usage, a synthetic reply's
        // zeros, a line with no time, a torn line, not JSON at all.
        const std::vector<std::string> bad = {
            "{\"type\":\"user\",\"timestamp\":\"2026-11-17T22:18:42Z\",\"message\":{\"usage\":{\"input_tokens\":4,\"output_tokens\":1}}}",
            "{\"type\":\"assistant\",\"timestamp\":\"2026-11-17T22:18:42Z\",\"message\":{\"id\":\"m\"}}",
            usageLine("m", "", "2026-11-17T22:18:42Z", "<synthetic>", usageJson(0, 0)),
            "{\"type\":\"assistant\",\"message\":{\"usage\":{\"input_tokens\":4,\"output_tokens\":1}}}",
            l.substr(0, l.size() / 2),
            "not json",
            "{\"type\":\"assistant\",\"timestamp\":\"2026-11-17T22:18:42Z\",\"message\":{\"usage\":{\"input_tokens\":\"4\"}}}",
        };
        for (const std::string &b : bad) CHECK(!parseUsageLine(b.data(), b.size(), "f", &e));
        // Fast mode.
        l = usageLine("msg_f", "req_f", "2026-11-17T22:18:42Z", "claude-opus-5-5", usageJson(1000, 0, 0, 0, 0, "fast"));
        CHECK(parseUsageLine(l.data(), l.size(), "f", &e) && e.fast);
    }

    GROUP("usage:prices");
    {
        bool known = false;
        CHECK(normalizeModel("claude-opus-4-5-20251101") == "claude-opus-4-5");
        CHECK(normalizeModel("us.anthropic.claude-opus-4-8-v1:0") == "claude-opus-4-8");
        CHECK(normalizeModel("claude-opus-5-5[1m]") == "claude-opus-5-5");
        CHECK(normalizeModel("claude-opus-4-5@20251101") == "claude-opus-4-5");
        CHECK(normalizeModel("Claude-Sonnet-5-5") == "claude-sonnet-5-5");
        Price p = priceFor("claude-opus-5-5", &known);
        CHECK(known && usageNear(p.input, 4) && usageNear(p.output, 20) && usageNear(p.cacheRead, 0.2));
        // The longest name wins: 4-5 is not 4's price.
        CHECK(usageNear(priceFor("claude-opus-4-5-20251101", &known).input, 5) && known);
        CHECK(usageNear(priceFor("claude-opus-4-20250514", &known).input, 15) && known);
        CHECK(usageNear(priceFor("claude-sonnet-4-5", &known).input, 3) && known);
        CHECK(usageNear(priceFor("claude-3-5-haiku-20241022", &known).input, 0.8) && known);
        // An unknown model of a known family: its newest price, said so.
        CHECK(usageNear(priceFor("claude-opus-6", &known).input, 4) && !known);
        CHECK(usageNear(priceFor("claude-haiku-9", &known).input, 0.1) && !known);
        CHECK(usageNear(priceFor("mystery", &known).input, 4) && !known);
        // A million of each: input 4, 5 m writes 5, 1 h writes 8, reads 0.2,
        // output 20.
        Tokens t;
        t.input = t.write5m = t.write1h = t.cacheRead = t.output = 1000000;
        CHECK(usageNear(costOf(t, p, false), 4 + 5 + 8 + 0.2 + 20));
        CHECK(usageNear(costOf(t, p, true), 2 * (4 + 5 + 8 + 0.2 + 20)));
        // Haiku 5.5 above 100K tokens of context costs its long prices for
        // the whole request.
        Price h = priceFor("claude-haiku-5-5");
        Tokens small, wide;
        small.input = 100000;
        small.output = 1000000;
        wide.input = 100001;
        wide.output = 1000000;
        CHECK(usageNear(costOf(small, h, false), 0.01 + 0.5));
        CHECK(usageNear(costOf(wide, h, false), 100001 * 0.5 / 1e6 + 2.5));
        CHECK(usageNear(priceFor("claude-opus-4-6").fastMultiplier, 6));
    }

    GROUP("usage:dedupe");
    {
        Tally tally;
        Entry e;
        auto add = [&](const std::string &l) {
            Entry x;
            return parseUsageLine(l.data(), l.size(), "f", &x) && tally.add(x);
        };
        // One response, a line per content block: counted once.
        const std::string a = usageLine("msg_1", "req_1", "2026-11-17T10:00:00Z", "claude-opus-5-5", usageJson(100, 50));
        CHECK(add(a));
        CHECK(!add(usageLine("msg_1", "req_1", "2026-11-17T10:00:01Z", "claude-opus-5-5", usageJson(100, 50))));
        CHECK(tally.entries().size() == 1);
        // A copy that says more replaces it; a smaller one does not.
        CHECK(add(usageLine("msg_1", "req_1", "2026-11-17T10:00:02Z", "claude-opus-5-5", usageJson(100, 80))));
        CHECK(!add(usageLine("msg_1", "req_1", "2026-11-17T10:00:03Z", "claude-opus-5-5", usageJson(100, 10))));
        CHECK(tally.entries().size() == 1 && tally.entries()[0].tokens.output == 80);
        // The main thread's copy wins over a subagent's, whatever its size.
        CHECK(add(usageLine("msg_2", "req_2", "2026-11-17T10:01:00Z", "claude-opus-5-5", usageJson(500, 500), "s1", true)));
        CHECK(add(usageLine("msg_2", "req_2", "2026-11-17T10:01:00Z", "claude-opus-5-5", usageJson(5, 5), "s1", false)));
        CHECK(!add(usageLine("msg_2", "req_2", "2026-11-17T10:01:00Z", "claude-opus-5-5", usageJson(900, 900), "s1", true)));
        CHECK(tally.entries().size() == 2 && !tally.entries()[1].sidechain);
        // The same message id with another request id is another response.
        CHECK(add(usageLine("msg_2", "req_3", "2026-11-17T10:02:00Z", "claude-opus-5-5", usageJson(5, 5))));
        // No request id: an Anthropic id is one response per session...
        CHECK(add(usageLine("msg_3", "", "2026-11-17T10:03:00Z", "claude-opus-5-5", usageJson(5, 5), "s1")));
        CHECK(!add(usageLine("msg_3", "", "2026-11-17T10:03:09Z", "claude-opus-5-5", usageJson(5, 5), "s1")));
        CHECK(add(usageLine("msg_3", "", "2026-11-17T10:03:00Z", "claude-opus-5-5", usageJson(5, 5), "s2")));
        // ...a gateway's id is one per time, as it may reuse it.
        CHECK(add(usageLine("gw", "", "2026-11-17T10:04:00Z", "claude-opus-5-5", usageJson(5, 5))));
        CHECK(add(usageLine("gw", "", "2026-11-17T10:05:00Z", "claude-opus-5-5", usageJson(5, 5))));
        CHECK(tally.entries().size() == 7);
        // Per session and per model, priced.
        auto by = tally.bySession();
        CHECK(by.size() == 2 && by["s1"]["claude-opus-5-5"].responses == 6);
        auto models = tally.byModel();
        CHECK(models.size() == 1 && models["claude-opus-5-5"].tokens.input == 100 + 5 + 5 + 5 + 5 + 5 + 5);
        CHECK(usageNear(tally.entries()[0].cost, (100 * 4 + 80 * 20) / 1e6));
    }

    GROUP("usage:scanner");
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path() / ("mutible-usage-scan-" + std::to_string((long)getpid()));
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "-Users-x-proj" / "sess1" / "subagents", ec);
        fs::create_directories(dir / "-Users-x-other", ec);
        const std::string main = (dir / "-Users-x-proj" / "sess1.jsonl").string();
        const std::string sub = (dir / "-Users-x-proj" / "sess1" / "subagents" / "agent-a.jsonl").string();
        const std::string other = (dir / "-Users-x-other" / "o.jsonl").string();
        const std::string l1 = usageLine("msg_a", "req_a", "2026-11-17T10:00:00Z", "claude-opus-5-5", usageJson(1, 1), "sess1");
        const std::string l2 = usageLine("msg_b", "req_b", "2026-11-17T10:01:00Z", "claude-opus-5-5", usageJson(2, 2), "sess1");
        const std::string ls = usageLine("msg_c", "req_c", "2026-11-17T10:02:00Z", "claude-haiku-5-5", usageJson(3, 3), "sess1", true);
        writeAll(main, "{\"type\":\"user\",\"message\":{\"content\":\"hi\"}}\n" + l1 + "\n" + l2.substr(0, 30));
        writeAll(sub, ls + "\n");
        writeAll(other, "garbage\n" + std::string(100, 'x') + "\n");
        Tally tally;
        Scanner scanner;
        const AgentTranscript::Fs posix = AgentTranscript::posixFs();
        Scanner::Stats st = scanner.scan(posix, dir.string(), 0, tally);
        CHECK(st.files == 3 && st.entries == 2 && tally.entries().size() == 2);
        // The half-written line is read once it is whole, and nothing twice.
        writeAll(main, "{\"type\":\"user\",\"message\":{\"content\":\"hi\"}}\n" + l1 + "\n" + l2 + "\n");
        st = scanner.scan(posix, dir.string(), 0, tally);
        CHECK(st.entries == 1 && tally.entries().size() == 3);
        const uint64_t read = st.bytesRead;
        st = scanner.scan(posix, dir.string(), 0, tally);
        CHECK(st.entries == 0 && st.bytesRead == 0 && read > 0);
        // A file replaced by a shorter one is read again; its copies drop.
        writeAll(main, l1 + "\n");
        st = scanner.scan(posix, dir.string(), 0, tally);
        CHECK(st.entries == 0 && tally.entries().size() == 3);
        // Old files are skipped.
        Tally fresh;
        Scanner again;
        st = again.scan(posix, dir.string(), (double)time(nullptr) + 3600, fresh);
        CHECK(st.files == 0 && fresh.entries().empty());
        // Speed: 20,000 lines, most not usage, a megabyte line among them.
        {
            std::string big;
            for (int i = 0; i < 20000; i++) {
                if (i % 4 == 0) {
                    char id[32];
                    std::snprintf(id, sizeof id, "msg_%d", i);
                    big += usageLine(id, std::string("req_") + id, "2026-11-17T11:00:00Z", "claude-opus-5-5", usageJson(10, 10));
                } else {
                    big += "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"content\":\"" +
                           std::string(300, 'y') + "\"}]}}";
                }
                big += "\n";
            }
            big += usageLine("msg_huge", "req_huge", "2026-11-17T11:00:00Z", "claude-opus-5-5", usageJson(1, 1), "s",
                             false, "[{\"type\":\"text\",\"text\":\"" + std::string(1 << 20, 'z') + "\"}]") + "\n";
            writeAll((dir / "-Users-x-other" / "big.jsonl").string(), big);
            Tally t;
            Scanner s;
            const double c0 = cpuMs();
            st = s.scan(posix, dir.string(), 0, t);
            CHECK_TIME(cpuMs() - c0, 400);
            std::printf("    scanned %d usage lines\n", st.entries);
            CHECK(st.entries == 5000 + 1 + 2);
        }
        fs::remove_all(dir, ec);
    }

    GROUP("usage:blocks");
    {
        // ccusage's blocks: from the hour of the first entry, 5 hours; a
        // later entry, or one after 5 quiet hours, starts the next.
        const double h = 3600, t0 = 1763373600;   // 2025-11-17 10:00 UTC, on the hour
        std::vector<Entry> es = {costEntry(t0 + 0.5 * h, 1), costEntry(t0 + 1.5 * h, 2),
                                 costEntry(t0 + 4.9 * h, 3), costEntry(t0 + 5.2 * h, 4),
                                 costEntry(t0 + 11 * h, 5)};
        std::vector<Block> bs = blocks(es, t0 + 12 * h);
        CHECK(bs.size() == 3);
        CHECK(usageNear(bs[0].start, t0) && usageNear(bs[0].end, t0 + 5 * h) && usageNear(bs[0].cost, 6) && bs[0].entries == 3);
        CHECK(usageNear(bs[1].start, t0 + 5 * h) && usageNear(bs[1].cost, 4) && !bs[1].active);
        CHECK(usageNear(bs[2].start, t0 + 11 * h) && bs[2].active);
        // Burn: $6 from 10:30 to 14:54 = 4.4 h.
        CHECK(usageNear(blockCostPerHour(bs[0]), 6 / 4.4, 1e-9));
        CHECK(usageNear(blockCostPerHour(bs[2]), 0));
        CHECK(usageNear(costBetween(es, t0 + 0.5 * h, t0 + 4.9 * h), 5));   // after, up to and including
        std::vector<SessionRate> rates = sessionRates(
            {costEntry(t0 + 1, 1, "a"), costEntry(t0 + 600, 2, "b"), costEntry(t0 + 700, 1, "b")}, t0 + 900, 900);
        CHECK(rates.size() == 2 && rates[0].sessionId == "b" && usageNear(rates[0].costPerHour, 12));
    }

    GROUP("usage:samples");
    {
        // The status line's stdin, from Claude Code's documentation.
        const std::string in = R"({"model":{"id":"claude-opus-5-5","display_name":"Opus"},
            "rate_limits":{"five_hour":{"used_percentage":23.5,"resets_at":1738425600},
            "seven_day":{"used_percentage":41.2,"resets_at":1738857600},
            "spend_limit":{"used_percentage":62.8,"resets_at":1740787200}}})";
        Sample s;
        CHECK(sampleFromStatusJson(in, 1738420000, &s));
        CHECK(usageNear(s.fivePct, 23.5) && usageNear(s.fiveReset, 1738425600) && usageNear(s.weekPct, 41.2) &&
              usageNear(s.time, 1738420000));
        CHECK(formatSample(s) == "v1 1738420000 23.5 1738425600 41.2 1738857600");
        Sample back;
        CHECK(parseSample(formatSample(s), &back) && usageNear(back.fivePct, 23.5) && usageNear(back.weekReset, 1738857600));
        // One window only; none at all (an API key's session); not JSON.
        CHECK(sampleFromStatusJson(R"({"rate_limits":{"seven_day":{"used_percentage":5,"resets_at":"2026-11-20T00:00:00Z"}}})", 1, &s));
        CHECK(!s.hasFive() && s.hasWeek() && formatSample(s).find("v1 1 - - 5 ") == 0);
        CHECK(!sampleFromStatusJson(R"({"model":{"id":"x"}})", 1, &s));
        CHECK(!sampleFromStatusJson(R"({"rate_limits":{"five_hour":{"used_percentage":null}}})", 1, &s));
        CHECK(!sampleFromStatusJson("{", 1, &s));
        // The log: bad lines and other versions skipped, sorted by time.
        std::vector<Sample> log = parseLog("v1 200 10 5000 - -\njunk\nv2 1 2 3 4 5 6\nv1 100 5 5000 - -\nv1 300 x 5000 - -\n");
        CHECK(log.size() == 2 && usageNear(log[0].time, 100) && usageNear(log[1].fivePct, 10));
        // Logged when something changed or a minute went by.
        Sample a = log[0], b = log[0];
        b.time += 30;
        CHECK(!worthLogging(a, b));
        b.fivePct += 0.5;
        CHECK(worthLogging(a, b));
        b = a;
        b.time += 61;
        CHECK(worthLogging(a, b) && worthLogging(Sample(), a));
    }

    GROUP("usage:fit");
    {
        // Every dollar at API prices is 2% of the 5-hour window. Spend $1
        // every 10 minutes from the window's start, and read it every 20.
        const double reset = 1763400000, start = reset - 5 * 3600, now = start + 3 * 3600;
        std::vector<Entry> es;
        for (int i = 1; i <= 18; i++) es.push_back(costEntry(start + i * 600 - 30, 1));
        std::vector<Sample> ss;
        for (int i = 1; i <= 9; i++) {
            Sample s;
            s.time = start + i * 1200;
            s.fiveReset = reset;
            s.fivePct = 2.0 * (2 * i);
            ss.push_back(s);
        }
        Fit f = fitScale(ss, es, Window::FiveHour);
        CHECK(f.ok && usageNear(f.scale, 2) && f.readings == 9);
        // Another computer used 6 points between two readings: the fit does
        // not move, and the forecast reports the gap.
        for (size_t i = 4; i < ss.size(); i++) ss[i].fivePct += 6;
        f = fitScale(ss, es, Window::FiveHour);
        CHECK(f.ok && usageNear(f.scale, 2));
        Forecast fc = forecast(ss, es, now);
        CHECK(fc.five.known && !fc.five.estimated && usageNear(fc.five.unseenPct, 6));
        // 36% from transcripts plus the 6 unseen, at 42% now.
        CHECK(usageNear(fc.five.pctNow, 42) && usageNear(fc.five.resetsAt, reset));
        // $6 an hour is 12% an hour: 58 points left, 4 h 50 m, after the
        // reset in 2 h: it reaches 42 + 24 = 66% by then.
        CHECK(usageNear(fc.five.pctPerHour, 12) && fc.five.hitAt == 0 && usageNear(fc.five.pctAtReset, 66));
        CHECK(usageNear(fc.costPerHour, 6));
        // Spending three times as fast in the last 15 minutes widens the
        // range to the early side.
        es.push_back(costEntry(now - 300, 3));
        es.push_back(costEntry(now - 200, 3));
        std::stable_sort(es.begin(), es.end(), [](const Entry &a, const Entry &b) { return a.time < b.time; });
        fc = forecast(ss, es, now);
        CHECK(fc.five.pctPerHourHigh > fc.five.pctPerHour);
        // No readings: no fit, and the window is unknown.
        fc = forecast({}, es, now);
        CHECK(!fc.five.known && !fc.five.fit.ok && !fc.week.known);
        // Readings without transcripts: their own slope.
        fc = forecast(ss, {}, now);
        CHECK(fc.five.known && !fc.five.fit.ok && fc.five.pctPerHour > 0);
    }

    GROUP("usage:forecast");
    {
        const double h = 3600, now = 1763400000;
        // $1 every 6 minutes for the last 3 hours. The 5-hour window began
        // 3 h ago at 1% per dollar: 30% now, 10% an hour, 50% when it
        // resets in 2 h. The week, at 0.1% per dollar: 3%, 1% an hour.
        std::vector<Entry> es;
        for (int i = 0; i < 30; i++) es.push_back(costEntry(now - 3 * h + 360 * i + 1, 1));
        Sample s0, s1;
        s0.time = now - h;
        s0.fiveReset = now + 2 * h;
        s0.fivePct = 20;
        s0.weekReset = now + 72 * h;
        s0.weekPct = 2;
        s1 = s0;
        s1.time = now;
        s1.fivePct = 30;
        s1.weekPct = 3;
        Forecast f = forecast({s0, s1}, es, now);
        CHECK(f.five.fit.ok && usageNear(f.five.fit.scale, 1) && f.five.fit.readings == 2 && usageNear(f.five.pctNow, 30));
        CHECK(usageNear(f.five.pctPerHour, 10) && f.five.hitAt == 0 && usageNear(f.five.pctAtReset, 50));
        CHECK(f.week.fit.ok && usageNear(f.week.fit.scale, 0.1) && usageNear(f.week.pctPerHour, 1));
        CHECK(usageNear(f.five.unseenPct, 0) && usageNear(f.week.unseenPct, 0));
        std::vector<std::string> lines = describe(f, now);
        for (const std::string &l : lines) std::printf("    %s\n", l.c_str());
        CHECK(lines.size() == 4);
        CHECK(lines[0] == "5-hour window: 30% used. At this rate it is about 50% when it resets in 2 h.");
        CHECK(lines[1] == "Week: 3.0% used. At this rate it is about 75% when it resets in 3 d.");
        CHECK(lines[2] == "Burn rate: $10.00 an hour at API prices; 1 session active in the last 15 minutes.");
        CHECK(lines[3] == "Fit: 1% of the 5-hour window is about $1.00 at API prices (2 readings); "
                          "1% of the week about $10.00 (2 readings).");
        // Fifteen minutes on, $3 every 90 seconds since: 60% now, and at
        // this rate 100% before the reset, with a range.
        const double later = now + 900;
        for (int i = 1; i <= 10; i++) es.push_back(costEntry(now + 90 * i - 1, 3));
        f = forecast({s0, s1}, es, later);
        CHECK(usageNear(f.five.pctNow, 60));
        CHECK(f.five.hitAt > later && f.five.hitAt < f.five.resetsAt);
        CHECK(f.five.hitEarliest > later && f.five.hitEarliest <= f.five.hitAt);
        lines = describe(f, later);
        CHECK(lines[0].find("5-hour window: 60% used. At this rate it reaches 100% in about ") == 0);
        CHECK(lines[0].find(" (") != std::string::npos);
        // Unseen usage is said, apart from the fit.
        Sample s2 = s1;
        s2.time = now + 600;
        s2.fivePct = 30 + 18 + 8;   // $18 in those ten minutes, and 8 points from elsewhere
        f = forecast({s0, s1, s2}, es, later);
        CHECK(usageNear(f.five.unseenPct, 8) && usageNear(f.five.fit.scale, 1));
        lines = describe(f, later);
        CHECK(lines[3] == "Unseen usage: 8 points of the 5-hour window that this computer's transcripts do "
                          "not explain (another computer, claude.ai, or the cloud).");
        // The window reset since the last reading: worked out.
        Sample old = s1;
        old.fiveReset = now - 2 * h;
        old.time = now - 3 * h;
        f = forecast({old}, es, now);
        CHECK(f.five.known && f.five.estimated && f.five.resetsAt > now);
        // No reading at all.
        lines = describe(forecast({}, {}, now), now);
        CHECK(lines[0] == "5-hour window: no reading yet. Claude Code reports it to the status line after its first reply.");
        CHECK(lines[2] == "Burn rate: nothing in the last hour.");
        CHECK(formatDuration(30) == "under a minute" && formatDuration(45 * 60) == "45 m" &&
              formatDuration(2 * h + 600) == "2 h 10 m" && formatDuration(76 * h) == "3 d 4 h" &&
              formatDuration(3 * h) == "3 h");
    }
}

static void testClaudeStatusLine() {
    using namespace ClaudeStatusLine;

    GROUP("statusline:words");
    {
        std::vector<std::string> w;
        CHECK(shellWords("a 'b c' \"d \\\"e\\\"\" f\\ g", &w));
        CHECK(w.size() == 4 && w[1] == "b c" && w[2] == "d \"e\"" && w[3] == "f g");
        CHECK(!shellWords("a 'b", &w) && !shellWords("a \"b", &w));
        // Whatever the old command holds, it comes back as it was.
        for (const std::string &cmd : {std::string("~/.claude/statusline.sh"),
                                       std::string("jq -r '\"[\\(.model.display_name)] \\(.context_window.used_percentage // 0)% context\"'"),
                                       std::string("bash -c 'echo \"it'\"'\"'s $HOME\"'"),
                                       std::string("printf '%s\\n' \"$(git branch --show-current)\" | cat"),
                                       std::string("a\tb\nc")}) {
            const std::string wrapped = wrapperCommand(kCommand, cmd);
            std::string prev;
            CHECK(isWrapper(wrapped, &prev) && prev == cmd);
        }
        std::string prev = "x";
        CHECK(isWrapper("mutible statusline", &prev) && prev.empty());
        CHECK(isWrapper("'/Applications/My Apps/Mutible.app/Contents/Resources/mutible' statusline"));
        CHECK(!isWrapper("mutible notify --from-hook") && !isWrapper("statusline mutible") && !isWrapper(""));
        CHECK(commandFor("") == "mutible statusline");
        CHECK(commandFor("/Apps/My Apps/mutible") == "'/Apps/My Apps/mutible' statusline");
        CHECK(shellQuote("it's") == "'it'\\''s'" && shellQuote("/usr/bin/x") == "/usr/bin/x");
    }

    GROUP("statusline:plan");
    {
        // No statusLine: one is added after the rest.
        Plan p = planInstall("{\n  \"model\": \"opus\"\n}\n", kCommand);
        CHECK(p.ok && p.changed && !p.hadStatusLine && p.previous.empty());
        CHECK(p.text == "{\n  \"model\": \"opus\",\n  \"statusLine\": {\n    \"type\": \"command\",\n"
                        "    \"command\": \"mutible statusline\"\n  }\n}\n");
        CHECK(installed(p.text) && !installed("{}"));
        // Undo removes it.
        Plan u = planUninstall(p.text);
        CHECK(u.ok && u.changed && u.statusLine.isNull() && u.text == "{\n  \"model\": \"opus\"\n}\n");
        // A statusLine of the user's: chained, its padding and place kept.
        const std::string mine =
            "{\"statusLine\":{\"type\":\"command\",\"command\":\"bash ~/s.sh 'a b'\",\"padding\":2,"
            "\"refreshInterval\":5},\"hooks\":{}}";
        p = planInstall(mine, kCommand);
        CHECK(p.ok && p.changed && p.hadStatusLine && p.previous == "bash ~/s.sh 'a b'");
        Json j;
        CHECK(Json::parse(p.text, j));
        CHECK(j.items().size() == 2 && j.items()[0].first == "statusLine");
        CHECK(j["statusLine"]["padding"].asNumber() == 2 && j["statusLine"]["refreshInterval"].asNumber() == 5);
        CHECK(j["statusLine"]["command"].asString() == "mutible statusline --then 'bash ~/s.sh '\\''a b'\\'''");
        // Again: nothing to do.
        Plan again = planInstall(p.text, kCommand);
        CHECK(again.ok && !again.changed && again.previous == "bash ~/s.sh 'a b'");
        // Undo: the old value exactly.
        u = planUninstall(p.text);
        Json orig;
        CHECK(Json::parse(mine, orig) && u.ok && u.changed && u.statusLine == orig["statusLine"]);
        CHECK(Json::parse(u.text, j) && j == orig);
        CHECK(planUninstall(mine).ok && !planUninstall(mine).changed);
        // Refused, nothing lost.
        CHECK(!planInstall("[1]", kCommand).ok && !planInstall("{x", kCommand).ok);
        CHECK(!planInstall("{\"statusLine\":\"echo\"}", kCommand).ok);
        CHECK(!planInstall("{\"statusLine\":{\"type\":\"command\"}}", kCommand).ok);
        CHECK(!planInstall("{\"statusLine\":{\"type\":\"other\",\"command\":\"x\"}}", kCommand).ok);
        CHECK(planInstall("", kCommand).ok && planInstall("  \n", kCommand).changed);
        Record r{"/p/settings.json", true, "{\"a\":1}\n", "{\"b\":2}\n"}, back;
        CHECK(parseRecord(recordText(r), &back) && back.path == r.path && back.hadFile &&
              back.original == r.original && back.written == r.written);
        CHECK(!parseRecord("{}", &back) && !parseRecord("nope", &back));
    }

    GROUP("statusline:files");
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path() / ("mutible-statusline-" + std::to_string((long)getpid()));
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "claude", ec);
        const std::string settings = (dir / "claude" / "settings.json").string();
        const std::string record = recordPath((dir / "usage").string());
        // The user's file, in their own spacing: undo writes it back byte for byte.
        const std::string original = "{\n    \"model\": \"opus\",\n    \"statusLine\": {\"type\": \"command\", "
                                     "\"command\": \"~/bin/status --short\", \"padding\": 0}\n}\n";
        writeAll(settings, original);
        ::chmod(settings.c_str(), 0640);
        Result r = install(settings, kCommand, record, 1763200000);
        CHECK(r.ok && r.changed && !r.backup.empty() && r.previous == "~/bin/status --short");
        CHECK(readAll(r.backup) == original);
        CHECK(installed(readAll(settings)) && fs::exists(record));
        struct stat st;
        CHECK(::stat(settings.c_str(), &st) == 0 && (st.st_mode & 07777) == 0640);
        CHECK(::stat((dir / "usage").c_str(), &st) == 0 && (st.st_mode & 07777) == 0700);
        r = install(settings, kCommand, record, 1763200000);
        CHECK(r.ok && !r.changed);
        r = uninstall(settings, record, 1763200001);
        CHECK(r.ok && r.changed && r.exact && r.previous == "~/bin/status --short");
        CHECK(readAll(settings) == original && !fs::exists(record));
        CHECK(::stat(settings.c_str(), &st) == 0 && (st.st_mode & 07777) == 0640);
        r = uninstall(settings, record, 1763200001);
        CHECK(r.ok && !r.changed);
        // Changed since install (another key added): only statusLine goes back.
        CHECK(install(settings, kCommand, record, 1763200002).changed);
        Json j;
        CHECK(Json::parse(readAll(settings), j));
        j.set("theme", "dark");
        writeAll(settings, j.dump());
        r = uninstall(settings, record, 1763200003);
        CHECK(r.ok && r.changed && !r.exact && !r.backup.empty());
        CHECK(Json::parse(readAll(settings), j) && j["theme"].asString() == "dark" &&
              j["statusLine"]["command"].asString() == "~/bin/status --short" &&
              j["statusLine"]["padding"].asNumber() == 0);
        // No file before: undo removes the one install made.
        const std::string none = (dir / "fresh" / "settings.json").string();
        r = install(none, kCommand, record, 1763200004);
        CHECK(r.ok && r.changed && r.backup.empty() && fs::exists(none));
        r = uninstall(none, record, 1763200005);
        CHECK(r.ok && r.exact && !fs::exists(none));
        // A file it would break is left alone.
        writeAll(settings, "{\"statusLine\": 3}");
        r = install(settings, kCommand, record, 1763200006);
        CHECK(!r.ok && r.refused && readAll(settings) == "{\"statusLine\": 3}");
        fs::remove_all(dir, ec);
    }

    GROUP("statusline:log");
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        CHECK(usageDir("/x", "/h", "/s", true) == "/x");
        CHECK(usageDir(nullptr, "/h", "/s", true) == "/h/Library/Application Support/Mutible/usage");
        CHECK(usageDir("", "/h", "/s", false) == "/s/mutible/usage");
        CHECK(usageDir(nullptr, "/h", "rel", false) == "/h/.local/state/mutible/usage");
        const fs::path dir = fs::temp_directory_path() / ("mutible-usage-log-" + std::to_string((long)getpid()));
        fs::remove_all(dir, ec);
        const std::string log = logPath((dir / "usage").string());
        ClaudeUsage::Sample s;
        s.time = 1000;
        s.fivePct = 10;
        s.fiveReset = 5000;
        CHECK(appendSample(log, s));
        s.time = 1010;
        CHECK(appendSample(log, s));   // the same a moment later: not written
        s.fivePct = 11;
        CHECK(appendSample(log, s));
        s.time = 1080;
        CHECK(appendSample(log, s));   // a minute on: written
        std::vector<ClaudeUsage::Sample> back = ClaudeUsage::parseLog(readLog(log));
        CHECK(back.size() == 3 && usageNear(back[1].fivePct, 11) && usageNear(back[2].time, 1080));
        // Over a megabyte, the log moves aside and both are read.
        {
            std::ofstream big(log, std::ios::app);
            for (int i = 0; i < 70000; i++) big << "v1 " << (500 + i % 400) << " 1 5000 - -\n";
        }
        s.time = 2000;
        CHECK(appendSample(log, s));
        CHECK(fs::exists(log + ".1") && fs::file_size(log) < 100);
        CHECK(ClaudeUsage::parseLog(readLog(log)).size() == 70004);
        fs::remove_all(dir, ec);
        CHECK(defaultLine(R"({"model":{"display_name":"Opus 5.5"},"rate_limits":{"five_hour":{"used_percentage":42.4,"resets_at":5}}})") ==
              "Opus 5.5 \xC2\xB7 5h 42%");
        CHECK(defaultLine("x") == "Claude Code" && defaultLine("{}") == "Claude Code");
    }
}
