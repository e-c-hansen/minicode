// parser_fuzz.inc — the `fuzz:*` groups for the parsers that read what other
// programs send or what the user writes: Json::parse, the language servers'
// framing (Lsp::Framer) and its Reader, the IDE link's handshake, WebSocket frames and MCP
// requests, MarkdownParser, and the settings file. Included by
// run_tests.cpp.
//
// Each fuzzer is structured, like terminal_fuzz.inc: it builds valid input
// it knows the answer for, cuts it into random pieces or mutates it, and
// checks what must hold whatever arrives (round trips, ranges inside the
// text, UTF-8 in and out). Seeded, so a failure repeats; each group is kept
// to about a second in `make test`. `make test-asan` runs them under
// AddressSanitizer and UndefinedBehaviorSanitizer, which is where an
// out-of-bounds read shows up even when every check passes. For a longer
// run, MC_FUZZ_SCALE=50 ./build/test-asan/run_tests runs fifty times the
// rounds; the extra ones go on from the same seeds, past where the short
// run stops.
//
// Only public interfaces are used, so a parser can be rewritten without
// touching this file.

#include "IdeLink.h"
#include <cstdlib>
#include <random>

namespace {

const int kFuzzScale = [] {
    const char *s = std::getenv("MC_FUZZ_SCALE");
    int n = s ? std::atoi(s) : 1;
    return n > 0 ? n : 1;
}();

struct FuzzRng {
    std::mt19937 g;
    explicit FuzzRng(unsigned seed) : g(seed) {}
    size_t below(size_t n) { return n ? g() % n : 0; }
    bool chance(int oneIn) { return below((size_t)oneIn) == 0; }
    template <class T, size_t N> const T &pick(const T (&a)[N]) { return a[below(N)]; }
};

// A random edit of `s`: a byte changed, inserted or removed, a stretch cut,
// doubled or moved, or the end dropped. `alphabet` is what inserted bytes
// are drawn from (structural characters find more than random bytes).
std::string fuzzMutate(FuzzRng &r, std::string s, const std::string &alphabet) {
    int edits = 1 + (int)r.below(4);
    for (int e = 0; e < edits; e++) {
        size_t n = s.size();
        switch (r.below(7)) {
        case 0:
            if (n) s[r.below(n)] = alphabet[r.below(alphabet.size())];
            break;
        case 1: s.insert(r.below(n + 1), 1, alphabet[r.below(alphabet.size())]); break;
        case 2:
            if (n) s.erase(r.below(n), 1);
            break;
        case 3:
            if (n) {
                size_t a = r.below(n), len = 1 + r.below(std::min<size_t>(n - a, 16));
                s.erase(a, len);
            }
            break;
        case 4:
            if (n) {
                size_t a = r.below(n), len = 1 + r.below(std::min<size_t>(n - a, 16));
                s.insert(r.below(n + 1), s.substr(a, len));
            }
            break;
        case 5:
            if (n) s.resize(r.below(n));
            break;
        case 6:
            if (n) s[r.below(n)] = (char)r.below(256);
            break;
        }
    }
    return s;
}

// Cuts `s` into pieces of 1 to `most` bytes (sometimes one byte at a time,
// when it is short enough for that to stay quick).
std::vector<std::string> fuzzChunks(FuzzRng &r, const std::string &s, size_t most) {
    std::vector<std::string> out;
    bool bytes = r.chance(8) && s.size() < 4096;
    for (size_t at = 0; at < s.size();) {
        size_t n = bytes ? 1 : 1 + r.below(most);
        out.push_back(s.substr(at, n));
        at += n;
    }
    return out;
}

// A string of random pieces: ASCII, controls, quotes and backslashes,
// multi-byte UTF-8 (and, when `invalid`, bytes that are not UTF-8).
std::string fuzzText(FuzzRng &r, size_t most, bool invalid) {
    static const char *const pieces[] = {
        "a", "Z", "0", " ", "\"", "\\", "/", "\n", "\r", "\t", "\x01", "\x1f", "\x7f",
        "\xC3\xA9", "\xE6\x97\xA5", "\xF0\x9F\x98\x80", "\xE2\x80\xAE", "\xEF\xBB\xBF",
        "{", "}", "[", "]", ":", ","};
    static const char *const bad[] = {"\xC3", "\xA9", "\xFF", "\xED\xA0\x80", "\xC0\xAF"};
    std::string s;
    size_t n = r.below(most + 1);
    for (size_t i = 0; i < n; i++)
        s += invalid && r.chance(10) ? r.pick(bad) : r.pick(pieces);
    return s;
}

double fuzzNumber(FuzzRng &r) {
    switch (r.below(6)) {
    case 0: return (double)(int)r.below(1000) - 500;
    case 1: return (double)(int64_t)(((uint64_t)r.g() << 32 | r.g()) >> r.below(64));
    case 2: return -0.0;
    case 3: return (double)r.g() / 7.0;
    case 4: return std::ldexp((double)r.g(), (int)r.below(1990) - 1000);   // under 2^1022
    default: {
        // Any finite double, from its bits: the shortest-form writer has to
        // read every one back exactly.
        for (;;) {
            uint64_t bits = (uint64_t)r.g() << 32 | r.g();
            double d;
            std::memcpy(&d, &bits, sizeof d);
            if (std::isfinite(d)) return d;
        }
    }
    }
}

Json fuzzJsonValue(FuzzRng &r, int depth) {
    switch (r.below(depth > 4 ? 4 : 6)) {
    case 0: return Json();
    case 1: return Json(r.chance(2));
    case 2: return Json(fuzzNumber(r));
    case 3: return Json(fuzzText(r, 12, true));
    case 4: {
        Json a = Json::array();
        size_t n = r.below(5);
        for (size_t i = 0; i < n; i++) a.push(fuzzJsonValue(r, depth + 1));
        return a;
    }
    default: {
        Json o = Json::object();
        size_t n = r.below(5);
        for (size_t i = 0; i < n; i++) o.set(fuzzText(r, 6, true), fuzzJsonValue(r, depth + 1));
        return o;
    }
    }
}

// Every string in a parsed value (keys too), for the UTF-8 check.
void jsonStrings(const Json &j, std::vector<std::string> &out) {
    if (j.isString()) out.push_back(j.asString());
    for (const Json &e : j.elements()) jsonStrings(e, out);
    for (const auto &kv : j.items()) {
        out.push_back(kv.first);
        jsonStrings(kv.second, out);
    }
}

double fuzzMs(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since)
        .count();
}

void testFuzzJson() {
    auto started = std::chrono::steady_clock::now();
    GROUP("fuzz:json");
    FuzzRng r(11);
    const std::string alphabet = "{}[]:,\"\\-+.eE0123456789tfnul \n\t";
    int roundTrips = 0, roundTripsBad = 0, mutated = 0, parsedBad = 0;
    std::string firstBad;
    for (int round = 0; round < 10000 * kFuzzScale; round++) {
        Json v = fuzzJsonValue(r, 0);
        std::string text = v.dump();
        Json back;
        roundTrips++;
        if (!Json::parse(text, back) || back != v || back.dump() != text) {
            if (roundTripsBad++ == 0) firstBad = text;
            continue;
        }
        // The same text with whitespace between every token still reads.
        for (int m = 0; m < 4; m++) {
            std::string bent = fuzzMutate(r, text, alphabet);
            Json p;
            mutated++;
            if (!Json::parse(bent, p)) continue;
            // Whatever reads, writes out as text that reads back the same
            // (compared as text: a number past a double's range reads as
            // infinity, which is written as null).
            Json again;
            if (!Json::parse(p.dump(), again) || again.dump() != p.dump()) parsedBad++;
        }
    }
    if (roundTripsBad) std::printf("  fuzz:json: %d round trips failed, first %s\n", roundTripsBad,
                                   firstBad.substr(0, 200).c_str());
    CHECK(roundTripsBad == 0);
    CHECK(parsedBad == 0);
    CHECK(roundTrips == 10000 * kFuzzScale && mutated > 0);

    // \u escapes, any mix of surrogates: what comes out is UTF-8, lone
    // and mismatched halves as U+FFFD.
    {
        static const char *const escapes[] = {"\\uD800", "\\uDBFF", "\\uDC00", "\\uDFFF",
                                              "\\u0041", "\\u00e9", "\\u0000", "\\uFFFF",
                                              "\\uFEFF", "x", "\\n", "\\ud83d", "\\ude00"};
        int notUtf8 = 0;
        std::string first;
        for (int round = 0; round < 4000 * kFuzzScale; round++) {
            std::string s = "\"";
            size_t n = 1 + r.below(6);
            for (size_t i = 0; i < n; i++) s += r.pick(escapes);
            s += "\"";
            Json j;
            if (!Json::parse(s, j) || !IdeLink::validUtf8(j.asString())) {
                if (notUtf8++ == 0) first = s;
            }
        }
        if (notUtf8) std::printf("  fuzz:json: %d escapes gave bad UTF-8, first %s\n", notUtf8,
                                 first.c_str());
        CHECK(notUtf8 == 0);
        Json j;
        CHECK(Json::parse("\"\\uD800\\uD800\\uDC00\"", j) &&
              j.asString() == "\xEF\xBF\xBD\xF0\x90\x80\x80");
        CHECK(Json::parse("\"\\ud83d\\ude00\"", j) && j.asString() == "\xF0\x9F\x98\x80");
    }

    // Valid UTF-8 in, valid UTF-8 out, for mutated text that still parses.
    {
        int bad = 0;
        for (int round = 0; round < 2000 * kFuzzScale; round++) {
            Json v = Json::object();
            for (int k = 0; k < 3; k++) v.set(fuzzText(r, 8, false), fuzzText(r, 8, false));
            std::string text = fuzzMutate(r, v.dump(), "\"\\u0123456789abcdefDdEe");
            Json p;
            if (!IdeLink::validUtf8(text) || !Json::parse(text, p)) continue;
            std::vector<std::string> strings;
            jsonStrings(p, strings);
            for (const std::string &s : strings)
                if (!IdeLink::validUtf8(s)) bad++;
        }
        CHECK(bad == 0);
    }

    // Nesting is refused past 256 levels, without running out of stack,
    // however deep the text goes.
    {
        Json j;
        std::string error;
        CHECK(Json::parse(std::string(256, '[') + std::string(256, ']'), j));
        CHECK(!Json::parse(std::string(257, '[') + std::string(257, ']'), j, &error) &&
              error.find("deeply") != std::string::npos);
        CHECK(!Json::parse(std::string(200000, '['), j));
        std::string objects;
        for (int i = 0; i < 100000; i++) objects += "{\"a\":";
        CHECK(!Json::parse(objects, j));
        CHECK(!Json::parse(std::string(100000, '"'), j));
        CHECK(!Json::parse("-", j) && !Json::parse("1e", j) && !Json::parse("\"\\u12", j) &&
              !Json::parse("[1,]", j) && !Json::parse("", j) && !Json::parse("tru", j));
    }
    std::printf("  fuzz:json: %d round trips, %d mutations in %.0f ms\n", roundTrips, mutated,
                fuzzMs(started));
}

// What a language server might put before a body: the length in its
// spellings, a Content-Type before or after it.
std::string fuzzLspHeader(FuzzRng &r, size_t length) {
    static const char *const names[] = {"Content-Length", "content-length", "CONTENT-LENGTH",
                                        "Content-length"};
    std::string len = std::string(r.pick(names)) + (r.chance(2) ? ": " : ":") +
                      (r.chance(4) ? " " : "") + std::to_string(length) +
                      (r.chance(4) ? " " : "");
    std::string type = "Content-Type: application/vscode-jsonrpc; charset=utf-8";
    switch (r.below(3)) {
    case 0: return len + "\r\n\r\n";
    case 1: return type + "\r\n" + len + "\r\n\r\n";
    default: return len + "\r\n" + type + "\r\n\r\n";
    }
}

void testFuzzLspFrames() {
    auto started = std::chrono::steady_clock::now();
    GROUP("fuzz:lsp-frames");
    FuzzRng r(23);
    int sessions = 0, lost = 0, leftOver = 0;
    for (int round = 0; round < 1500 * kFuzzScale; round++, sessions++) {
        // Bodies that hold what a header looks like, cut at random places,
        // with header blocks that carry no usable length (skipped) between.
        std::vector<std::string> bodies;
        std::string stream;
        size_t n = 1 + r.below(8);
        for (size_t i = 0; i < n; i++) {
            std::string body = fuzzText(r, 40, true);
            if (r.chance(4)) body += "\r\n\r\nContent-Length: 3\r\n\r\n";
            if (r.chance(20)) body = std::string(70000 + r.below(1000), 'x');
            if (r.chance(5)) {
                static const char *const junk[] = {
                    "Content-Length: 99999999999999999999999\r\n\r\n", "Content-Length: -4\r\n\r\n",
                    "Content-Length: 12abc\r\n\r\n", "Content-Length:\r\n\r\n",
                    "X-Nothing: here\r\n\r\n", "Content-Length: 2147483648\r\n\r\n"};
                stream += r.pick(junk);
            }
            stream += fuzzLspHeader(r, body.size()) + body;
            bodies.push_back(body);
        }
        Lsp::Framer f;
        std::vector<std::string> got;
        for (const std::string &piece : fuzzChunks(r, stream, 64)) {
            f.feed(piece);
            std::string body;
            while (f.next(body)) got.push_back(body);
        }
        if (got != bodies) lost++;
        if (f.buffered() != 0) leftOver++;
    }
    CHECK(lost == 0);
    CHECK(leftOver == 0);

    // A framed stream with random damage: next() either hands out a body no
    // longer than what was fed or says it needs more, and always ends.
    int stuck = 0, tooLong = 0;
    for (int round = 0; round < 3000 * kFuzzScale; round++) {
        std::string stream;
        for (int i = 0; i < 4; i++) {
            std::string body = fuzzText(r, 30, true);
            stream += Lsp::Framer::frame(body);
        }
        stream = fuzzMutate(r, stream, "\r\n:Content-Length 0123456789");
        Lsp::Framer f;
        size_t fed = 0;
        int calls = 0;
        for (const std::string &piece : fuzzChunks(r, stream, 32)) {
            f.feed(piece);
            fed += piece.size();
            std::string body;
            while (f.next(body)) {
                if (body.size() > fed) tooLong++;
                if (++calls > (int)stream.size() + 8) { stuck++; break; }
            }
        }
    }
    CHECK(stuck == 0 && tooLong == 0);

    // A body cut short waits, and the frame after a bad length still reads.
    {
        Lsp::Framer f;
        std::string body;
        f.feed("Content-Length: 10\r\n\r\n12345");
        CHECK(!f.next(body) && f.buffered() == 27);
        f.feed("67890Content-Length: 99999999999999999999\r\n\r\nContent-Length: 2\r\n\r\nok");
        CHECK(f.next(body) && body == "1234567890");
        CHECK(f.next(body) && body == "ok" && f.buffered() == 0);
    }
    std::printf("  fuzz:lsp-frames: %d sessions in %.0f ms\n", sessions, fuzzMs(started));
}

// One message a server might send, as a body: answers to the client's
// requests (ids 1 to 8, some never asked), the notifications and requests
// the client handles, with params of any shape, and bodies that are not
// JSON at all.
std::string fuzzServerBody(FuzzRng &r) {
    static const char *const notes[] = {
        "window/logMessage", "window/showMessage", "textDocument/publishDiagnostics",
        "$/progress", "$/logTrace", "telemetry/event", "x/unknown"};
    static const char *const asks[] = {
        "workspace/configuration", "window/workDoneProgress/create", "client/registerCapability",
        "client/unregisterCapability", "workspace/applyEdit", "workspace/workspaceFolders",
        "window/showMessageRequest", "x/unknown"};
    Json m = Json::object();
    m.set("jsonrpc", "2.0");
    switch (r.below(6)) {
    case 0:
        return fuzzText(r, 40, true);
    case 1: case 2:
        m.set("id", Json((int64_t)(1 + r.below(8))));
        if (r.chance(4)) {
            Json e = Json::object();
            e.set("code", Json((int64_t)-32603));
            e.set("message", fuzzText(r, 10, true));
            m.set("error", e);
        } else {
            m.set("result", fuzzJsonValue(r, 1));
        }
        break;
    case 3: {
        m.set("method", r.pick(notes));
        Json p = fuzzJsonValue(r, 2);
        if (r.chance(2)) {
            p = Json::object();
            p.set("type", Json((int64_t)r.below(5)));
            p.set("message", fuzzText(r, 20, true));
            p.set("uri", "file:///w/a.py");
            p.set("diagnostics", fuzzJsonValue(r, 3));
            p.set("token", r.chance(2) ? Json("t") : Json((int64_t)r.below(3)));
            Json v = Json::object();
            static const char *const kinds[] = {"begin", "report", "end", "other"};
            v.set("kind", r.pick(kinds));
            v.set("title", fuzzText(r, 10, true));
            v.set("percentage", fuzzJsonValue(r, 4));
            p.set("value", v);
        }
        m.set("params", p);
        break;
    }
    default:
        m.set("id", r.chance(2) ? Json((int64_t)r.below(1000)) : Json(fuzzText(r, 6, false)));
        m.set("method", r.pick(asks));
        m.set("params", fuzzJsonValue(r, 2));
        break;
    }
    std::string body = m.dump();
    if (r.chance(10)) body = fuzzMutate(r, body, "{}[]\":,0123456789");
    return body;
}

void testFuzzLspReader() {
    auto started = std::chrono::steady_clock::now();
    GROUP("fuzz:lsp-reader");
    FuzzRng r(29);
    // A Reader hands out what the Framer and Json::parse make of each body,
    // in order, whatever the pieces: the same value or the same failure.
    int differ = 0, sessions = 0;
    for (int round = 0; round < 1200 * kFuzzScale; round++, sessions++) {
        std::vector<std::string> bodies;
        std::string stream;
        size_t n = 1 + r.below(8);
        for (size_t i = 0; i < n; i++) {
            std::string body = r.chance(3) ? fuzzServerBody(r) : fuzzJsonValue(r, 0).dump();
            stream += fuzzLspHeader(r, body.size()) + body;
            bodies.push_back(body);
        }
        Lsp::Reader reader;
        std::vector<Lsp::Incoming> got;
        for (const std::string &piece : fuzzChunks(r, stream, 48)) {
            reader.feed(piece);
            Lsp::Incoming in;
            while (reader.next(in)) got.push_back(in);
        }
        if (got.size() != bodies.size() || reader.buffered() != 0) { differ++; continue; }
        for (size_t i = 0; i < bodies.size(); i++) {
            Json want;
            bool ok = Json::parse(bodies[i], want);
            if (ok != got[i].error.empty()) differ++;
            else if (ok && want.dump() != got[i].message.dump()) differ++;
            else if (!ok && got[i].message.dump() != Json().dump()) differ++;
        }
    }
    CHECK(differ == 0);

    // A client fed by receive() and one fed by deliver() of a Reader's
    // messages answer the same random traffic alike: the same bytes sent
    // back, the same handlers run with the same results, the same
    // protocol errors and progress. Under the sanitizers this is also
    // every handler meeting params of the wrong shape.
    int mismatched = 0;
    size_t handled = 0, sentBytes = 0;
    for (int round = 0; round < 300 * kFuzzScale; round++, sessions++) {
        std::string sent[2];
        std::vector<std::string> events[2];
        std::unique_ptr<Lsp::Client> c[2];
        for (int k = 0; k < 2; k++) {
            std::string &out = sent[k];
            std::vector<std::string> &ev = events[k];
            c[k].reset(new Lsp::Client([&out](const std::string &x) { out += x; }));
            Lsp::Client *self = c[k].get();
            c[k]->onProtocolError = [&ev](const std::string &e) { ev.push_back("error " + e); };
            c[k]->onShowMessage = [&ev](int t, const std::string &m) {
                ev.push_back("show " + std::to_string(t) + " " + m);
            };
            c[k]->onDiagnostics = [&ev](const std::string &uri, const std::vector<Lsp::Diagnostic> &d) {
                ev.push_back("diags " + uri + " " + std::to_string(d.size()));
            };
            c[k]->onApplyEdit = [&ev](const Json &, Lsp::Client::ApplyEditReply reply) {
                ev.push_back("edit");
                reply(false, "no");
            };
            c[k]->onProgress = [&ev, self] {
                ev.push_back("progress " + std::to_string(self->progressCount()));
            };
            c[k]->initialize("/w", 1);
            for (int q = 0; q < 6; q++)
                c[k]->request("x/y", Json::object(), [&ev, q](const Json &res, const Json &err) {
                    ev.push_back("answer " + std::to_string(q) + " " + res.dump() + " " + err.dump());
                });
        }
        std::string stream = Lsp::Framer::frame(
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":{},\"serverInfo\":{\"name\":\"s\"}}}");
        size_t n = 1 + r.below(12);
        for (size_t i = 0; i < n; i++) stream += Lsp::Framer::frame(fuzzServerBody(r));
        Lsp::Reader reader;
        for (const std::string &piece : fuzzChunks(r, stream, 64)) {
            c[0]->receive(piece);
            reader.feed(piece);
            Lsp::Incoming in;
            while (reader.next(in)) c[1]->deliver(in);
        }
        if (sent[0] != sent[1] || events[0] != events[1] || c[0]->state() != c[1]->state() ||
            c[0]->progressCount() != c[1]->progressCount())
            mismatched++;
        handled += events[0].size();
        sentBytes += sent[0].size();
    }
    CHECK(mismatched == 0);
    CHECK(handled > 300 && sentBytes > 100000);
    std::printf("  fuzz:lsp-reader: %d sessions in %.0f ms (%zu handler calls, %zu bytes answered)\n",
                sessions, fuzzMs(started), handled, sentBytes);
}

std::string fuzzIdeRequest(const std::string &token) {
    return "GET / HTTP/1.1\r\nHost: 127.0.0.1:45123\r\nUpgrade: websocket\r\n"
           "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: mcp\r\n"
           "X-Claude-Code-Ide-Authorization: " + token + "\r\n\r\n";
}

void testFuzzIdeLink() {
    using namespace IdeLink;
    auto started = std::chrono::steady_clock::now();
    GROUP("fuzz:idelink");
    FuzzRng r(37);

    // Messages cut into fragments with pings, pongs and closes between them,
    // masked at random and fed in random pieces, come out whole and in order.
    int sessions = 0, wrong = 0;
    std::string firstWrong;
    for (int round = 0; round < 800 * kFuzzScale; round++, sessions++) {
        struct Want { Opcode op; std::string payload; uint16_t code; };
        std::vector<Want> want;
        std::string stream;
        size_t n = 1 + r.below(6);
        for (size_t i = 0; i < n; i++) {
            Opcode op = r.chance(3) ? Opcode::Binary : Opcode::Text;
            std::string payload = op == Opcode::Text ? fuzzText(r, 60, false) : fuzzText(r, 60, true);
            if (r.chance(15)) payload = std::string(126 + r.below(70000), 'p');
            size_t parts = 1 + r.below(4);
            size_t at = 0;
            for (size_t p = 0; p < parts; p++) {
                bool last = p + 1 == parts;
                size_t len = last ? payload.size() - at : r.below(payload.size() - at + 1);
                stream += encodeClientFrame(p == 0 ? op : Opcode::Continuation,
                                            payload.substr(at, len), r.g(), last);
                at += len;
                if (last) want.push_back({op, payload, 0});
                if (r.chance(3)) {
                    // Control frames may come between the fragments of a message.
                    std::string ping = fuzzText(r, 20, true).substr(0, 125);
                    Opcode c = r.chance(2) ? Opcode::Ping : Opcode::Pong;
                    stream += encodeClientFrame(c, ping, r.g());
                    want.push_back({c, ping, 0});
                }
            }
        }
        if (r.chance(3)) {
            std::string close;
            close += (char)0x03;
            close += (char)0xE8;   // 1000
            close += "bye \xC3\xA9";
            stream += encodeClientFrame(Opcode::Close, close, r.g());
            want.push_back({Opcode::Close, "bye \xC3\xA9", 1000});
        }
        FrameReader reader;
        std::vector<Message> got;
        bool failed = false;
        for (const std::string &piece : fuzzChunks(r, stream, 300)) {
            reader.feed(piece);
            Message m;
            FrameReader::Result res;
            while ((res = reader.next(m)) == FrameReader::Ready) got.push_back(m);
            if (res == FrameReader::Error) { failed = true; break; }
        }
        bool same = !failed && got.size() == want.size() && reader.buffered() == 0;
        for (size_t i = 0; same && i < got.size(); i++)
            same = got[i].op == want[i].op && got[i].payload == want[i].payload &&
                   (want[i].op != Opcode::Close || got[i].closeCode == want[i].code);
        if (!same && wrong == 0 && !failed)
            for (size_t i = 0; i < got.size() && i < want.size(); i++)
                if (got[i].op != want[i].op || got[i].payload != want[i].payload ||
                    (want[i].op == Opcode::Close && got[i].closeCode != want[i].code))
                    std::printf("  fuzz:idelink: message %zu: op %d want %d, %zu bytes want %zu, code %d\n",
                                i, (int)got[i].op, (int)want[i].op, got[i].payload.size(),
                                want[i].payload.size(), got[i].closeCode);
        if (!same && wrong == 0 && !failed)
            for (size_t i = 0; i < got.size() && i < want.size(); i++)
                if (got[i].op != want[i].op || got[i].payload != want[i].payload ||
                    (want[i].op == Opcode::Close && got[i].closeCode != want[i].code))
                    std::printf("  fuzz:idelink: message %zu: op %d want %d, %zu bytes want %zu, code %d\n",
                                i, (int)got[i].op, (int)want[i].op, got[i].payload.size(),
                                want[i].payload.size(), got[i].closeCode);
        if (!same && wrong++ == 0)
            firstWrong = failed ? reader.error() : "got " + std::to_string(got.size()) +
                                                       " of " + std::to_string(want.size());
    }
    if (wrong) std::printf("  fuzz:idelink: %d sessions wrong, first: %s\n", wrong, firstWrong.c_str());
    CHECK(wrong == 0);

    // Damaged streams: an error stays an error, text is UTF-8, a message is
    // never over the limit, and nothing is read past what was fed.
    int notSticky = 0, badText = 0, overLimit = 0;
    for (int round = 0; round < 3000 * kFuzzScale; round++) {
        std::string stream;
        for (int i = 0; i < 3; i++)
            stream += encodeClientFrame(r.chance(2) ? Opcode::Text : Opcode::Ping,
                                        fuzzText(r, 20, false), r.g(), !r.chance(4));
        stream = fuzzMutate(r, stream, std::string("\x81\x01\x80\x88\x89\x8a\x7e\x7f\x00", 9));
        FrameReader reader(200);
        Message m;
        for (const std::string &piece : fuzzChunks(r, stream, 16)) {
            reader.feed(piece);
            FrameReader::Result res;
            while ((res = reader.next(m)) == FrameReader::Ready) {
                if (m.op == Opcode::Text && !validUtf8(m.payload)) badText++;
                if (m.payload.size() > 200) overLimit++;
            }
            if (res == FrameReader::Error) {
                if (reader.next(m) != FrameReader::Error || reader.errorCode() == 0) notSticky++;
                break;
            }
        }
    }
    CHECK(notSticky == 0 && badText == 0 && overLimit == 0);
    {
        FrameReader reader(100);
        reader.feed(encodeClientFrame(Opcode::Text, std::string(60, 'a'), 7, false));
        reader.feed(encodeClientFrame(Opcode::Continuation, std::string(60, 'a'), 9, true));
        Message m;
        CHECK(reader.next(m) == FrameReader::Error && reader.errorCode() == 1009);
        FrameReader huge;
        // A 64-bit length near the top: refused, not allocated.
        std::string h = "\x82\xFF";
        h += std::string("\x7F\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8);
        h += "mask";
        huge.feed(h);
        CHECK(huge.next(m) == FrameReader::Error && huge.errorCode() == 1009);
    }

    // The handshake: damage never crashes it, an accepted one carried the
    // token and a 101, and every prefix of a good request is incomplete.
    {
        const std::string token = "0123456789abcdef0123456789abcdef0123456789abcdef";
        const std::string good = fuzzIdeRequest(token);
        CHECK(readHandshake(good, token).state == Handshake::Accepted);
        int prefixBad = 0, acceptedBad = 0, consumedBad = 0;
        for (size_t cut = 0; cut + 1 < good.size(); cut++)
            if (readHandshake(good.substr(0, cut), token).state != Handshake::Incomplete)
                prefixBad++;
        for (int round = 0; round < 6000 * kFuzzScale; round++) {
            std::string req = fuzzMutate(r, good, "\r\n: ,GETHOSTUpgrade-websocket13");
            Handshake h = readHandshake(req, token);
            if (h.state == Handshake::Accepted &&
                (req.find(token) == std::string::npos || h.response.rfind("HTTP/1.1 101 ", 0) != 0))
                acceptedBad++;
            if (h.consumed > req.size()) consumedBad++;
        }
        CHECK(prefixBad == 0 && acceptedBad == 0 && consumedBad == 0);
        CHECK(readHandshake(std::string(kMaxHandshakeBytes + 1, 'G'), token).state ==
              Handshake::Refused);
        CHECK(readHandshake(good, "").state == Handshake::Refused);
    }

    // MCP requests, whole and damaged: every answer is a JSON-RPC 2.0 object.
    {
        McpServer server("minicode", "0");
        Tool echo;
        echo.name = "echo";
        echo.inputSchema = objectSchema({{"text", schemaProperty("string", "what to echo")}});
        echo.run = [](const Json &args, std::function<void(ToolResult)> respond) {
            respond(ToolResult::text(args["text"].asString()));
        };
        server.addTool(echo);
        int answers = 0, badAnswers = 0;
        server.send = [&](const std::string &text) {
            answers++;
            Json j;
            if (!Json::parse(text, j) || !j.isObject() || j["jsonrpc"].asString() != "2.0" ||
                !(j.has("result") || j.has("error")))
                badAnswers++;
        };
        server.onNotification = [](const std::string &, const Json &) {};
        const char *const requests[] = {
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
            "{\"protocolVersion\":\"2025-06-18\"}}",
            "{\"jsonrpc\":\"2.0\",\"id\":\"a\",\"method\":\"tools/list\"}",
            "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":"
            "{\"name\":\"echo\",\"arguments\":{\"text\":\"hi\"}}}",
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
            "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"ping\"}",
            "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{}}"};
        for (int round = 0; round < 4000 * kFuzzScale; round++) {
            std::string req = r.pick(requests);
            if (!r.chance(4)) req = fuzzMutate(r, req, "{}[]:,\"0123456789idmethodparams");
            server.handle(req);
        }
        CHECK(answers > 0 && badAnswers == 0);
    }
    std::printf("  fuzz:idelink: %d frame sessions in %.0f ms\n", sessions, fuzzMs(started));
}

void testFuzzMarkdown() {
    auto started = std::chrono::steady_clock::now();
    GROUP("fuzz:markdown");
    FuzzRng r(41);
    static const char *const pieces[] = {
        "# ", "## ", "###### ", "####### ", "#", " #", "text", "word ", "é", "日本", "\xF0\x9F\x98\x80",
        "*", "**", "_", "__", "~~", "`", "```", "```python\n", "~~~", "$", "$$", "\\(", "\\)",
        "\\[", "\\]", "\\", "[", "]", "(", ")", "![", "](", "<", ">", "> ", "<br>", "<!--",
        "-->", "<div>", "</div>", "<https://x.y>", "http://a.b/c", "- ", "* ", "+ ", "1. ",
        "2) ", "- [ ] ", "- [x] ", "|", " | ", "|---|", "|:-:|", "---", "***", "===", "    ",
        "\t", "  \n", "\n", "\n\n", "\r\n", "\r", "[^1]", "[^1]: note", "&amp;", "&#x1F600;",
        "&", "\"", "'", "{", "}"};
    int runsBad = 0, linesBad = 0, utf8Bad = 0, docs = 0;
    std::string firstBad;
    double slowest = 0;
    for (int round = 0; round < 12000 * kFuzzScale; round++, docs++) {
        std::string md;
        size_t n = 1 + r.below(60);
        for (size_t i = 0; i < n; i++) md += r.pick(pieces);
        if (r.chance(4)) md = fuzzMutate(r, md, "#*_`$[]()<>|-\n \\!~");
        bool validIn = IdeLink::validUtf8(md);
        auto t0 = std::chrono::steady_clock::now();
        std::vector<MdRun> runs = MarkdownParser::parse(md);
        std::vector<MdLine> lines = MarkdownParser::lines(md);
        slowest = std::max(slowest, fuzzMs(t0));
        // Every run within the source's lines and its fields in range.
        for (const MdRun &run : runs) {
            bool ok = run.line >= -1 && run.line < (int)lines.size() && run.heading >= 0 &&
                      run.heading <= 6 && run.listDepth >= 0 && run.task >= 0 && run.task <= 2 &&
                      run.math >= 0 && run.math <= 2 && run.tableAlign >= 0 &&
                      run.tableAlign <= 2 && (run.tableCol < 0 || run.tableCol < run.tableCols);
            if (!ok && runsBad++ == 0) firstBad = md;
            // What a GUI turns into an NSString must be UTF-8 when the
            // source was (a cut through a character would draw nothing).
            if (validIn && !(IdeLink::validUtf8(run.text) && IdeLink::validUtf8(run.url) &&
                             IdeLink::validUtf8(run.src))) {
                if (utf8Bad++ == 0 && firstBad.empty()) firstBad = md;
            }
        }
        // Lines in order, each inside the source, its text inside the line.
        size_t prevEnd = 0;
        for (size_t i = 0; i < lines.size(); i++) {
            const MdLine &l = lines[i];
            bool ok = l.lineStart <= l.lineEnd && l.lineEnd <= md.size() &&
                      l.lineStart <= l.start && l.start <= l.end && l.end <= l.lineEnd &&
                      (i == 0 || l.lineStart > prevEnd);
            prevEnd = l.lineEnd;
            if (!ok) {
                if (linesBad++ == 0 && firstBad.empty()) {
                    firstBad = md;
                    std::printf("  fuzz:markdown: line %zu of %zu: %zu %zu %zu %zu (prev end %zu, size %zu)\n",
                                i, lines.size(), l.lineStart, l.start, l.end, l.lineEnd,
                                i ? lines[i - 1].lineEnd : 0, md.size());
                }
                break;
            }
        }
    }
    if (runsBad || linesBad || utf8Bad) {
        std::string shown;
        for (char c : firstBad.substr(0, 2000))
            shown += c == '\n' ? std::string("\\n") : c == '\r' ? std::string("\\r") : std::string(1, c);
        std::printf("  fuzz:markdown: %d runs, %d line tables, %d UTF-8 cuts; first: %s\n",
                    runsBad, linesBad, utf8Bad, shown.c_str());
    }
    CHECK(runsBad == 0);
    CHECK(linesBad == 0);
    CHECK(utf8Bad == 0);
    // A table's separator row has its text inside its own line (it was
    // left at the top of the file, [0, 0)).
    {
        std::string md = "a|b\n|-|-|\nc|d\n";
        std::vector<MdLine> lines = MarkdownParser::lines(md);
        CHECK(lines.size() >= 2 && lines[1].kind == MdLine::TableSep &&
              lines[1].start == lines[1].lineStart && lines[1].end == lines[1].lineEnd &&
              md.substr(lines[1].start, lines[1].end - lines[1].start) == "|-|-|");
    }
    // Deep nesting and long runs of one delimiter stay quick.
    {
        auto t0 = std::chrono::steady_clock::now();
        (void)MarkdownParser::parse(std::string(20000, '>') + " x");
        (void)MarkdownParser::parse(std::string(20000, '['));
        (void)MarkdownParser::parse(std::string(20000, '*') + "x" + std::string(20000, '_'));
        std::string lists;
        for (int i = 0; i < 300; i++) lists += std::string(i * 2, ' ') + "- x\n";
        (void)MarkdownParser::parse(lists);
        (void)MarkdownParser::parse(std::string(20000, '`'));
        (void)MarkdownParser::lines(std::string(20000, '\r'));
        double ms = fuzzMs(t0);
        CHECK(ms < 5000);
    }
    std::printf("  fuzz:markdown: %d documents in %.0f ms (slowest %.1f ms)\n", docs,
                fuzzMs(started), slowest);
}

void testFuzzSettings() {
    auto started = std::chrono::steady_clock::now();
    GROUP("fuzz:settings");
    FuzzRng r(53);
    // The default file's lines, commented out and not, as the seeds.
    std::vector<std::string> seeds;
    {
        std::string text = Settings::defaultFileText();
        for (size_t at = 0; at < text.size();) {
            size_t nl = text.find('\n', at);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(at, nl - at);
            seeds.push_back(line);
            if (line.rfind("# ", 0) == 0 && line.find(" = ") != std::string::npos)
                seeds.push_back(line.substr(2));
            at = nl + 1;
        }
    }
    CHECK(seeds.size() > 100);
    static const char *const values[] = {"#fff", "#12345678", "#GGG", "#", "80%", "0.5", "-1",
                                         "1e400", "on", "off", "\"a # b\"", "'x", "\xC3\xA9",
                                         "99999999999999999999", "", " # c"};
    int errorLineBad = 0, colorBad = 0, opacityBad = 0, valueBad = 0, files = 0;
    for (int round = 0; round < 4000 * kFuzzScale; round++, files++) {
        std::string text;
        size_t n = 1 + r.below(40);
        int lineCount = 0;
        for (size_t i = 0; i < n; i++) {
            std::string line = seeds[r.below(seeds.size())];
            if (r.chance(3)) {
                size_t eq = line.find('=');
                if (eq != std::string::npos) line = line.substr(0, eq + 1) + " " + r.pick(values);
            }
            if (r.chance(5)) line = fuzzMutate(r, line, "#=%. \t\"'abc0123456789");
            text += line + (r.chance(10) ? "\r\n" : "\n");
            lineCount++;
        }
        // A mutation may have put a line break in a line.
        lineCount = 1 + (int)std::count(text.begin(), text.end(), '\n');
        std::vector<SettingsError> errors;
        (void)Settings::parse(text, &errors);
        for (const SettingsError &e : errors)
            if (e.line < 1 || e.line > lineCount + 1 || e.message.empty()) {
                if (errorLineBad++ == 0) {
                    std::string shown;
                    for (char c : text.substr(0, 3000))
                        shown += c == '\n' ? std::string("\\n\n") : c == '\r' ? std::string("\\r") : std::string(1, c);
                    std::printf("  fuzz:settings: error on line %d of %d (%s) in:\n%s\n", e.line,
                                lineCount, e.message.c_str(), shown.c_str());
                }
            }

        // The swatch and opacity edits on each line: spans inside the line,
        // and what was written reads back.
        for (size_t at = 0; at < text.size();) {
            size_t nl = text.find('\n', at);
            if (nl == std::string::npos) nl = text.size();
            std::string line8 = text.substr(at, nl - at);
            at = nl + 1;
            std::u16string line(line8.begin(), line8.end());   // ASCII is enough here
            ColorSpan span;
            if (Settings::findColor(line, span)) {
                if (span.start + span.length > line.size()) colorBad++;
                Rgba c = Rgba::hex((uint32_t)r.below(0x1000000));
                ColorSpan again;
                std::u16string set = Settings::setColor(line, c);
                if (!Settings::findColor(set, again) || again.color != c) colorBad++;
            }
            ValueSpan vs;
            double value = 0;
            if (Settings::findOpacity(line, vs, value)) {
                if (vs.start + vs.length > line.size()) opacityBad++;
                double want = (double)r.below(101) / 100.0;
                ValueSpan again;
                double read = -1;
                if (!Settings::findOpacity(Settings::setOpacity(line, want), again, read) ||
                    std::fabs(read - want) > 0.006)
                    opacityBad++;
            }
            if (Settings::findValue(line, vs) && vs.start + vs.length > line.size()) valueBad++;
        }

        // withValue puts the key where findKeyLine finds it, holding the value.
        static const char *const keys[] = {"editor.font-size", "terminal.scrollback", "theme",
                                           "editor.word-wrap", "lsp.python"};
        static const char *const newValues[] = {"14", "5000", "monokai", "off", "pylsp --x"};
        size_t k = r.below(5);
        std::string edited = Settings::withValue(text, keys[k], newValues[k]);
        int line = Settings::findKeyLine(edited, keys[k]);
        std::string found;
        for (size_t at = 0, i = 0; at <= edited.size() && line >= 0; i++) {
            size_t nl = edited.find('\n', at);
            if (nl == std::string::npos) nl = edited.size();
            if ((int)i == line) { found = edited.substr(at, nl - at); break; }
            at = nl + 1;
        }
        if (line < 0 || found.find(newValues[k]) == std::string::npos ||
            found.rfind("#", 0) == 0)
            valueBad++;
    }
    CHECK(errorLineBad == 0);
    CHECK(colorBad == 0);
    CHECK(opacityBad == 0);
    CHECK(valueBad == 0);
    std::printf("  fuzz:settings: %d files in %.0f ms\n", files, fuzzMs(started));
}

} // namespace

void testParserFuzz() {
    testFuzzJson();
    testFuzzLspFrames();
    testFuzzLspReader();
    testFuzzIdeLink();
    testFuzzMarkdown();
    testFuzzSettings();
}
