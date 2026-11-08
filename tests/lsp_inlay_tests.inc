// lsp_inlay_tests.inc — the `lsp:inlay-*` groups of run_tests.cpp: inlay
// hints (InlayHints.h). The request and what initialize offers, the
// answer read (labels as a string and as parts, padding, kinds, tooltips),
// VS Code's maximumLength cut, where hints fall in a window of the text,
// and how they follow edits. Included there, after the fake server and
// the Json helpers.
#include "InlayHints.h"

static void testLspInlayHints() {
    GROUP("lsp:inlay-request");
    {
        FakeServer srv;
        Lsp::Client client([&](const std::string &b) { srv.take(b); });
        int refreshed = 0;
        client.onInlayHintRefresh = [&] { refreshed++; };
        client.initialize("/w", 1);
        const Json &caps = srv.sent[0]["params"]["capabilities"];
        CHECK(caps["textDocument"].has("inlayHint"));
        CHECK(caps["workspace"]["inlayHint"]["refreshSupport"].asBool());
        client.receive(reply(1, "{\"capabilities\":{\"inlayHintProvider\":{}}}"));
        CHECK(client.offers(Lsp::Feature::InlayHint));
        CHECK(Lsp::offers(parsed("{\"inlayHintProvider\":true}"), Lsp::Feature::InlayHint));
        CHECK(!Lsp::offers(parsed("{\"inlayHintProvider\":false}"), Lsp::Feature::InlayHint));
        CHECK(!Lsp::offers(parsed("{}"), Lsp::Feature::InlayHint));
        client.didOpen("file:///w/a.py", "python", "x = 1\n");
        int answered = 0;
        int id = client.inlayHints("file:///w/a.py", Lsp::Range{{0, 0}, {40, 0}},
                                   [&](const Json &r, const Json &e) {
                                       if (e.isNull() && r.isArray()) answered++;
                                   });
        const Json &q = srv.sent.back();
        CHECK(q["method"].asString() == "textDocument/inlayHint" && q["id"].asInt() == id);
        CHECK(q["params"]["textDocument"]["uri"].asString() == "file:///w/a.py");
        CHECK(q["params"]["range"]["start"]["line"].asInt() == 0 &&
              q["params"]["range"]["end"]["line"].asInt() == 40 &&
              q["params"]["range"]["end"]["character"].asInt() == 0);
        client.receive(reply(id, "[]"));
        CHECK(answered == 1);
        // The server's refresh is answered, and the owner asks again.
        client.receive(Lsp::Framer::frame(
            "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"workspace/inlayHint/refresh\"}"));
        CHECK(refreshed == 1 && srv.sent.back()["id"].asInt() == 9 &&
              srv.sent.back().has("result") && !srv.sent.back().has("error"));
        // basedpyright is told to send its hints.
        Json bs = Lsp::pythonSettings(Lsp::PythonServer::BasedPyright, "");
        const Json &ih = bs["basedpyright"]["analysis"]["inlayHints"];
        CHECK(ih["variableTypes"].asBool() && ih["functionReturnTypes"].asBool() &&
              ih["callArgumentNames"].asBool());
        CHECK(!Lsp::pythonSettings(Lsp::PythonServer::Pyright, "")["python"]["analysis"]
                   .has("inlayHints"));
    }

    GROUP("lsp:inlay-parse");
    {
        // A string label, label parts, kinds, padding, both tooltip forms,
        // and ones left out: no position, a negative or fractional one, an
        // empty label. Sorted by position whatever order they came in.
        std::vector<Lsp::InlayHint> h = Lsp::parseInlayHints(parsed(
            "[{\"position\":{\"line\":3,\"character\":9},\"label\":\"count=\",\"kind\":2,"
            "\"paddingRight\":true},"
            "{\"position\":{\"line\":0,\"character\":1},\"label\":[{\"value\":\": \"},"
            "{\"value\":\"list[int]\",\"tooltip\":\"from the call\","
            "\"location\":{\"uri\":\"file:///t.pyi\",\"range\":{}}}],\"kind\":1},"
            "{\"position\":{\"line\":2,\"character\":4},\"label\":\"-> str\","
            "\"paddingLeft\":true,\"tooltip\":{\"kind\":\"markdown\",\"value\":\"**str**\"}},"
            "{\"label\":\"no position\"},"
            "{\"position\":{\"line\":-1,\"character\":0},\"label\":\"x\"},"
            "{\"position\":{\"line\":1.5,\"character\":0},\"label\":\"x\"},"
            "{\"position\":{\"line\":1,\"character\":0},\"label\":\"\"},"
            "{\"position\":{\"line\":1,\"character\":0},\"label\":[]},"
            "7, null]"));
        CHECK(h.size() == 3);
        if (h.size() == 3) {
            CHECK(h[0].position == (Lsp::Position{0, 1}) && h[0].label == ": list[int]" &&
                  h[0].kind == Lsp::InlayHintKind::Type && !h[0].paddingLeft &&
                  !h[0].paddingRight && h[0].tooltip == "from the call");
            CHECK(h[1].position == (Lsp::Position{2, 4}) && h[1].label == "-> str" &&
                  h[1].kind == Lsp::InlayHintKind::Other && h[1].paddingLeft &&
                  h[1].tooltip == "**str**");
            CHECK(h[2].label == "count=" && h[2].kind == Lsp::InlayHintKind::Parameter &&
                  h[2].paddingRight && !h[2].paddingLeft && h[2].tooltip.empty());
        }
        CHECK(Lsp::parseInlayHints(Json()).empty());
        CHECK(Lsp::parseInlayHints(parsed("{\"label\":\"x\"}")).empty());
    }

    GROUP("lsp:inlay-shorten");
    {
        auto hint = [](int line, int ch, const std::string &label) {
            Lsp::InlayHint h;
            h.position = Lsp::Position{line, ch};
            h.label = label;
            return h;
        };
        // VS Code's 43: a short line keeps everything; on a long one the
        // hint crossing the limit is cut with an ellipsis where it falls,
        // and the hints after it on that line go. The next line starts over.
        std::vector<Lsp::InlayHint> v = {
            hint(0, 1, ": int"),
            hint(1, 1, ": " + std::string(30, 'a')),   // 32
            hint(1, 9, ": " + std::string(20, 'b')),   // 54 in all: 11 over
            hint(1, 20, ": c"),
            hint(2, 0, std::string(43, 'd')),          // exactly the limit
            hint(2, 5, "e"),                           // then one more: "…"
            hint(2, 7, "f"),                           // and the line is full
        };
        Lsp::shortenInlayLabels(v);
        CHECK(v.size() == 5);
        if (v.size() == 5) {
            CHECK(v[0].label == ": int" && v[1].label.size() == 32);
            CHECK(v[2].label == ": " + std::string(9, 'b') + "\xE2\x80\xA6");
            CHECK(v[3].position == (Lsp::Position{2, 0}) && v[3].label.size() == 43);
            CHECK(v[4].position == (Lsp::Position{2, 5}) && v[4].label == "\xE2\x80\xA6");
        }
        // A cut never splits a character outside the BMP (two UTF-16 units).
        std::vector<Lsp::InlayHint> w = {hint(0, 1, std::string(42, 'x') + "\xF0\x9F\x98\x80")};
        Lsp::shortenInlayLabels(w);
        CHECK(w.size() == 1 && w[0].label == std::string(42, 'x') + "\xE2\x80\xA6");
        // 0 never cuts.
        std::vector<Lsp::InlayHint> z = {hint(0, 1, std::string(100, 'q'))};
        Lsp::shortenInlayLabels(z, 0);
        CHECK(z[0].label.size() == 100);
    }

    GROUP("lsp:inlay-place");
    {
        auto hint = [](int line, int ch) {
            Lsp::InlayHint h;
            h.position = Lsp::Position{line, ch};
            h.label = "x";
            return h;
        };
        // A window of lines 10 to 12 (CRLF and LF) starting at offset 500.
        const std::u16string window = u"def f(a, b):\r\n    v = g(a)\n    return v\n";
        std::vector<Lsp::InlayHint> hs = {
            hint(9, 3),     // before the window
            hint(10, 11),   // after "def f(a, b)": a return type
            hint(11, 5),    // after "    v": a type, following the name
            hint(11, 10),   // before "a" in g(a): a parameter name
            hint(11, 0),    // at a line's start: no character before it
            hint(12, 99),   // past its line's end: at the end
            hint(13, 0),    // the empty "line" after the window's last break
        };
        std::vector<Lsp::PlacedInlayHint> p = Lsp::placeInlayHints(hs, window, 500, 10);
        CHECK(p.size() == 4);
        if (p.size() == 4) {
            CHECK(p[0].hint == 1 && p[0].offset == 511 && !p[0].followsBefore);   // after ')'
            CHECK(p[1].hint == 2 && p[1].offset == 500 + 14 + 5 && p[1].followsBefore);
            CHECK(p[2].hint == 3 && p[2].offset == 500 + 14 + 10 && !p[2].followsBefore);
            CHECK(p[3].hint == 5 && p[3].offset == 500 + 14 + 13 + 12);
        }
        CHECK(Lsp::placeInlayHints(hs, u"", 0, 0).empty());
        CHECK(Lsp::inlayWordChar(u'a') && Lsp::inlayWordChar(u'_') && Lsp::inlayWordChar(u'9') &&
              Lsp::inlayWordChar(0x00E9) && !Lsp::inlayWordChar(u')') &&
              !Lsp::inlayWordChar(u' ') && !Lsp::inlayWordChar(u'('));

        // Lines counted piece by piece, a CRLF split across two pieces once.
        Lsp::LineBreakCounter n;
        const std::u16string a = u"a\r", b = u"\nb\rc\n\n";
        n.feed(a.data(), a.size());
        n.feed(b.data(), b.size());
        CHECK(n.lines() == 4);
    }

    GROUP("lsp:inlay-positions");
    {
        // A hint at 10 that follows the word before it (x: int), and one
        // that goes with what comes after it (count=).
        auto follow = [](size_t at, bool before, size_t s, size_t oldLen, size_t newLen,
                         size_t &out) {
            out = at;
            return Lsp::inlayFollowEdit(out, before, s, oldLen, newLen);
        };
        size_t o = 0;
        // Typing before it moves it; deleting before it too.
        CHECK(follow(10, true, 2, 0, 3, o) && o == 13);
        CHECK(follow(10, false, 2, 4, 0, o) && o == 6);
        // An edit after it leaves it alone.
        CHECK(follow(10, true, 11, 0, 5, o) && o == 10);
        CHECK(follow(10, false, 10, 3, 1, o) && o == 10);
        // Typing right at it: the name's type moves along with the name,
        // the parameter's name stays before what is typed.
        CHECK(follow(10, true, 10, 0, 2, o) && o == 12);
        CHECK(follow(10, false, 10, 0, 2, o) && o == 10);
        // Backspace right before it, or the word before replaced: it stays
        // at the end of the new text.
        CHECK(follow(10, true, 9, 1, 0, o) && o == 9);
        CHECK(follow(10, true, 7, 3, 5, o) && o == 12);
        CHECK(follow(10, false, 7, 3, 0, o) && o == 7);
        // Text on both sides of it replaced: it goes.
        CHECK(!follow(10, true, 8, 4, 1, o));
        CHECK(!follow(10, false, 0, 20, 20, o));
        // Nothing left before it: it goes too.
        CHECK(!follow(3, true, 0, 3, 0, o));
    }

    GROUP("lsp:inlay-settings");
    {
        Settings d = parseSettings("");
        CHECK(d.inlayHints());
        CHECK(d.inlayHintColor() == Rgba::hex(0x969696));
        CHECK(d.inlayHintBackground().rgb() == 0x4D4D4D && d.inlayHintBackground().a < 0.11 &&
              d.inlayHintBackground().a > 0.09);
        // The documented defaults are the real ones.
        Settings un = parseSettings(uncommentKeys(Settings::defaultFileText()));
        CHECK(un.inlayHints() == d.inlayHints());
        CHECK(un.inlayHintColor() == d.inlayHintColor());
        CHECK(un.inlayHintBackground() == d.inlayHintBackground());
        // The Modern themes have badges of their own, light themes a light one.
        CHECK(parseSettings("theme = dark-modern").inlayHintBackground().rgb() == 0x616161);
        CHECK(parseSettings("theme = light-modern").inlayHintBackground().rgb() == 0xCCCCCC);
        CHECK(parseSettings("theme = quiet-light").inlayHintBackground().rgb() == 0xC4C4C4);
        std::vector<SettingsError> e;
        Settings off = parseSettings("editor.inlay-hints = off\n"
                                     "editor.inlay-hint-color = #FF0000\n", &e);
        CHECK(e.empty() && !off.inlayHints() && off.inlayHintColor() == Rgba::hex(0xFF0000));
        parseSettings("editor.inlay-hints = sometimes\n", &e);
        CHECK(e.size() == 1);
    }
}
