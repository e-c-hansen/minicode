// lsp_semantic_tests.inc — the `lsp:semantic-*` groups of run_tests.cpp:
// textDocument/semanticTokens. Which style each token type and modifier
// gets, the legend, the numbers decoded into spans over the text (a fixture
// in basedpyright's legend), /full/delta edits, what the client asks for
// and offers, and workspace/semanticTokens/refresh. Included there, after
// the fake server and the Json helpers.

// This thread's CPU time in seconds, so a busy machine does not fail a bound.
static double semanticThreadCpu() {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void testLspSemanticTokens() {
    // basedpyright's legend, as its semanticTokensProvider.ts declares it.
    const char *kBasedPyrightCaps =
        "{\"semanticTokensProvider\":{\"legend\":{"
        "\"tokenTypes\":[\"namespace\",\"type\",\"class\",\"enum\",\"typeParameter\","
        "\"parameter\",\"variable\",\"property\",\"enumMember\",\"function\",\"method\","
        "\"keyword\",\"decorator\",\"selfParameter\",\"clsParameter\"],"
        "\"tokenModifiers\":[\"declaration\",\"definition\",\"readonly\",\"static\",\"async\","
        "\"defaultLibrary\",\"builtin\",\"classMember\",\"parameter\"]},"
        "\"full\":{\"delta\":true},\"range\":false}}";

    GROUP("lsp:semantic-styles");
    {
        using Lsp::SemanticStyle;
        using Lsp::semanticStyle;
        for (const char *t : {"namespace", "type", "class", "enum", "interface", "struct",
                              "typeParameter"})
            CHECK(semanticStyle(t, false) == SemanticStyle::Class);
        for (const char *t : {"function", "method", "member", "decorator"})
            CHECK(semanticStyle(t, false) == SemanticStyle::Function);
        CHECK(semanticStyle("variable", false) == SemanticStyle::Variable);
        CHECK(semanticStyle("event", false) == SemanticStyle::Variable);
        CHECK(semanticStyle("parameter", false) == SemanticStyle::Parameter);
        CHECK(semanticStyle("property", false) == SemanticStyle::Property);
        CHECK(semanticStyle("enumMember", false) == SemanticStyle::Constant);
        // readonly makes a variable or a property a constant, nothing else.
        CHECK(semanticStyle("variable", true) == SemanticStyle::Constant);
        CHECK(semanticStyle("property", true) == SemanticStyle::Constant);
        CHECK(semanticStyle("parameter", true) == SemanticStyle::Parameter);
        CHECK(semanticStyle("class", true) == SemanticStyle::Class);
        // The lexer's own, and types a server makes up, keep its colors.
        for (const char *t : {"keyword", "comment", "string", "number", "regexp", "operator",
                              "macro", "label", "modifier", "selfParameter", "clsParameter",
                              "", "Variable"})
            CHECK(semanticStyle(t, false) == SemanticStyle::None);
    }

    GROUP("lsp:semantic-legend");
    {
        Json caps = parsed(kBasedPyrightCaps);
        Lsp::SemanticLegend l = Lsp::SemanticLegend::from(caps);
        CHECK(l.types.size() == 15 && l.modifiers.size() == 9 && l.styles.size() == 15);
        CHECK(l.readonlyBit == 4u);   // the third modifier
        CHECK(l.styles[5] == Lsp::SemanticStyle::Parameter &&
              l.styles[13] == Lsp::SemanticStyle::None);
        CHECK(Lsp::offersSemanticTokens(caps) && Lsp::offersSemanticTokensDelta(caps));
        CHECK(Lsp::offers(caps, Lsp::Feature::SemanticTokens));
        // full: true without delta; ranges only; no legend; nothing.
        Json fullOnly = parsed("{\"semanticTokensProvider\":{\"legend\":{\"tokenTypes\":[],"
                               "\"tokenModifiers\":[]},\"full\":true}}");
        CHECK(Lsp::offersSemanticTokens(fullOnly) && !Lsp::offersSemanticTokensDelta(fullOnly));
        CHECK(Lsp::SemanticLegend::from(fullOnly).readonlyBit == 0);
        CHECK(!Lsp::offersSemanticTokens(parsed("{\"semanticTokensProvider\":{\"legend\":{"
                                              "\"tokenTypes\":[]},\"range\":true}}")));
        CHECK(!Lsp::offersSemanticTokens(parsed("{\"semanticTokensProvider\":{\"full\":true}}")));
        CHECK(!Lsp::offers(parsed("{}"), Lsp::Feature::SemanticTokens));
        CHECK(Lsp::SemanticLegend::from(parsed("{}")).types.empty());
    }

    GROUP("lsp:semantic-decode");
    {
        // def area(width, height):\r\n    PI = 3\r\n    return width * PI\n
        // as basedpyright tokens it: area function (declaration), the two
        // parameters, PI a variable (readonly by its name), self-made
        // types left alone.
        const std::u16string text = Lsp::toUtf16(
            "def area(width, height):\r\n"
            "    PI = 3\r\n"
            "    return width * PI\n"
            "x = \xF0\x9F\x98\x80 + width\n");   // an emoji: two UTF-16 units
        Lsp::PositionIndex index(text);
        Lsp::SemanticLegend legend = Lsp::SemanticLegend::from(parsed(kBasedPyrightCaps));
        // line, start, length, type, modifiers (relative, five a token)
        std::vector<uint32_t> data = {
            0, 4, 4, 9, 1,      // area: function, declaration
            0, 5, 5, 5, 1,      // width: parameter
            0, 7, 6, 5, 1,      // height: parameter
            1, 4, 2, 6, 1 | 4,  // PI: variable, readonly -> constant
            1, 11, 5, 5, 0,     // width
            0, 8, 2, 6, 4,      // PI
            0, 9, 4, 11, 0,     // past the line's end: cut to nothing, left out
            1, 0, 1, 13, 0,     // x as selfParameter: no style, left out
            0, 9, 50, 6, 0,     // width after the emoji, cut at the line's end
            5, 0, 3, 6, 0,      // a line past the text: the end
        };
        std::vector<Lsp::SemanticSpan> spans = Lsp::decodeSemanticTokens(data, legend, index);
        auto at = [&](size_t i, const char *word, Lsp::SemanticStyle st) {
            if (i >= spans.size()) return false;
            std::u16string w = text.substr(spans[i].start, spans[i].length);
            return w == Lsp::toUtf16(word) && spans[i].style == st;
        };
        CHECK(spans.size() == 7);
        CHECK(at(0, "area", Lsp::SemanticStyle::Function));
        CHECK(at(1, "width", Lsp::SemanticStyle::Parameter));
        CHECK(at(2, "height", Lsp::SemanticStyle::Parameter));
        CHECK(at(3, "PI", Lsp::SemanticStyle::Constant));
        CHECK(at(4, "width", Lsp::SemanticStyle::Parameter));
        CHECK(at(5, "PI", Lsp::SemanticStyle::Constant));
        // width is at character 9 of its line: the emoji counts two.
        CHECK(at(6, "width", Lsp::SemanticStyle::Variable));
        for (size_t i = 1; i < spans.size(); i++)
            CHECK(spans[i].start >= spans[i - 1].start + spans[i - 1].length);

        // Overlapping and backwards tokens are dropped; a short or empty
        // array decodes to nothing; an unknown type index is skipped.
        std::vector<uint32_t> odd = {0, 4, 4, 9, 0, 0, 2, 4, 9, 0, 0, 3, 5, 99, 0, 0, 1, 3};
        spans = Lsp::decodeSemanticTokens(odd, legend, index);
        CHECK(spans.size() == 1 && at(0, "area", Lsp::SemanticStyle::Function));
        CHECK(Lsp::decodeSemanticTokens({}, legend, index).empty());
        CHECK(Lsp::decodeSemanticTokens(data, Lsp::SemanticLegend(), index).empty());

        // A long file decodes in time proportional to its tokens.
        std::string big;
        for (int i = 0; i < 100000; i++) big += "value = other + third\n";
        const std::u16string bigText = Lsp::toUtf16(big);
        Lsp::PositionIndex bigIndex(bigText);
        std::vector<uint32_t> many;
        for (int i = 0; i < 100000; i++) {
            const uint32_t row[] = {i ? 1u : 0u, 0, 5, 6, 0, 0, 8, 5, 6, 0, 0, 8, 5, 6, 0};
            many.insert(many.end(), std::begin(row), std::end(row));
        }
        const double t0 = semanticThreadCpu();
        spans = Lsp::decodeSemanticTokens(many, legend, bigIndex);
        const double cpu = semanticThreadCpu() - t0;
        std::printf("  semantic tokens: 300,000 decoded in %.1f ms of CPU\n", cpu * 1000);
        CHECK(spans.size() == 300000);
        CHECK(cpu < 1.0);
    }

    GROUP("lsp:semantic-utf8");
    {
        // The GTK port's spans: UTF-8 bytes of the same text. "é" is two
        // bytes and one unit, "€" three and one, the emoji four and two.
        using Lsp::SemanticSpan;
        using Lsp::SemanticStyle;
        const std::string utf8 = "ab \xC3\xA9x \xE2\x82\xAC y \xF0\x9F\x98\x80z w";
        const std::u16string text = Lsp::toUtf16(utf8);
        CHECK(text.size() == 15);
        std::vector<SemanticSpan> spans = {
            {0, 2, SemanticStyle::Class},      // ab
            {3, 2, SemanticStyle::Variable},   // éx
            {8, 1, SemanticStyle::Function},   // y
            {10, 1, SemanticStyle::Constant},  // half the emoji: its start both ends, empty
            {11, 2, SemanticStyle::Property},  // the emoji's second half and z: from its start
            {14, 5, SemanticStyle::Parameter}, // w, running past the end: cut there
        };
        size_t length = 0;
        std::vector<SemanticSpan> out = Lsp::semanticSpansInUtf8(text, spans, &length);
        CHECK(length == utf8.size());
        CHECK(out.size() == 5);
        if (out.size() == 5) {
            CHECK(out[0].start == 0 && out[0].length == 2 && out[0].style == SemanticStyle::Class);
            CHECK(out[1].start == 3 && out[1].length == 3);
            CHECK(utf8.substr(out[2].start, out[2].length) == "y");
            CHECK(utf8.substr(out[3].start, out[3].length) == "\xF0\x9F\x98\x80z" &&
                  out[3].style == SemanticStyle::Property);
            CHECK(utf8.substr(out[4].start, out[4].length) == "w");
        }
        // Plain ASCII: the same offsets.
        const std::u16string ascii = Lsp::toUtf16("one two\nthree");
        out = Lsp::semanticSpansInUtf8(ascii, {{4, 3, SemanticStyle::Variable},
                                               {8, 5, SemanticStyle::Class}});
        CHECK(out.size() == 2 && out[0].start == 4 && out[1].start == 8 && out[1].length == 5);
        CHECK(Lsp::semanticSpansInUtf8(ascii, {}).empty());
    }

    GROUP("lsp:semantic-over-code");
    {
        // A server's token over a string, an escape or a comment leaves it
        // the lexer's: only the parts over code keep the token.
        using Lsp::SemanticSpan;
        using Lsp::SemanticStyle;
        const std::string line = "label = f\"Wid{name}\\n\" # Widget";
        std::vector<Token> tokens;
        IncrementalHighlighter<char> hl("py");
        hl.reset(StringSource<char>(line), &tokens);
        auto styleAt = [&](size_t i) {
            for (const Token &t : tokens)
                if (t.start <= i && i < t.start + t.length) return t.style;
            return TokenStyle::Plain;
        };
        const size_t brace = line.find('{'), comment = line.find('#');
        CHECK(isStringStyle(styleAt(line.find("Wid"))) && !isStringStyle(styleAt(brace + 1)));
        CHECK(styleAt(comment + 2) == TokenStyle::Comment);
        std::vector<SemanticSpan> spans = {
            {0, 5, SemanticStyle::Variable},                          // label: code
            {(uint32_t)line.find("Wid"), 13, SemanticStyle::Class},   // Wid{name}\n": cut
            {(uint32_t)comment + 2, 6, SemanticStyle::Class},         // in the comment: gone
        };
        std::vector<SemanticSpan> out = Lsp::semanticSpansOverCode(spans, tokens);
        std::vector<std::string> kept;
        for (const SemanticSpan &s : out) kept.push_back(line.substr(s.start, s.length));
        // Only label and the field's code (its braces are code too).
        CHECK(!kept.empty() && kept[0] == "label");
        bool overString = false, field = false;
        for (const SemanticSpan &s : out)
            for (size_t i = s.start; i < s.start + s.length; i++) {
                if (isStringStyle(styleAt(i)) || styleAt(i) == TokenStyle::Comment)
                    overString = true;
                if (i == brace + 1) field = true;
            }
        CHECK(!overString && field);
        // No lexer tokens: the spans as they are.
        CHECK(Lsp::semanticSpansOverCode(spans, {}).size() == 3);
    }

    GROUP("lsp:semantic-delta");
    {
        std::vector<uint32_t> data = {0, 0, 3, 1, 0, 1, 2, 4, 1, 0, 0, 6, 2, 2, 0};
        // Replace the second token, insert one at the end.
        Json edits = parsed("[{\"start\":15,\"deleteCount\":0,\"data\":[1,0,1,0,0]},"
                            "{\"start\":5,\"deleteCount\":5,\"data\":[2,0,9,3,0]}]");
        CHECK(Lsp::applySemanticEdits(data, edits));
        CHECK((data == std::vector<uint32_t>{0, 0, 3, 1, 0, 2, 0, 9, 3, 0, 0, 6, 2, 2, 0,
                                             1, 0, 1, 0, 0}));
        // A deletion without data, at the front.
        CHECK(Lsp::applySemanticEdits(data, parsed("[{\"start\":0,\"deleteCount\":5}]")));
        CHECK(data.size() == 15 && data[0] == 2);
        // Past the end, overlapping, negative and fractional are refused
        // and leave the data alone.
        const std::vector<uint32_t> before = data;
        CHECK(!Lsp::applySemanticEdits(data, parsed("[{\"start\":14,\"deleteCount\":2}]")));
        CHECK(!Lsp::applySemanticEdits(data, parsed("[{\"start\":16,\"deleteCount\":0}]")));
        CHECK(!Lsp::applySemanticEdits(data, parsed("[{\"start\":0,\"deleteCount\":5},"
                                                    "{\"start\":4,\"deleteCount\":1}]")));
        CHECK(!Lsp::applySemanticEdits(data, parsed("[{\"start\":-1,\"deleteCount\":0}]")));
        CHECK(!Lsp::applySemanticEdits(data, parsed("[{\"start\":1.5,\"deleteCount\":0}]")));
        CHECK(!Lsp::applySemanticEdits(data, parsed("[{\"start\":0,\"deleteCount\":0,"
                                                    "\"data\":[-3]}]")));
        CHECK(!Lsp::applySemanticEdits(data, parsed("{}")));
        CHECK(data == before);
        CHECK(Lsp::applySemanticEdits(data, parsed("[]")) && data == before);

        // SemanticTokens keeps the resultId for the next delta, takes a
        // full answer in place of a delta, and forgets everything after an
        // answer it cannot use.
        Lsp::SemanticTokens st;
        CHECK(st.apply(parsed("{\"resultId\":\"1\",\"data\":[0,0,3,1,0,1,2,4,1,0]}")));
        CHECK(st.resultId == "1" && st.data.size() == 10);
        CHECK(st.apply(parsed("{\"resultId\":\"2\",\"edits\":[{\"start\":5,\"deleteCount\":5}]}")));
        CHECK(st.resultId == "2" && st.data.size() == 5);
        CHECK(st.apply(parsed("{\"resultId\":\"3\",\"data\":[]}")) && st.data.empty() &&
              st.resultId == "3");
        CHECK(st.apply(parsed("{\"data\":[0,0,1,0,0]}")) && st.resultId.empty());
        CHECK(!st.apply(parsed("{\"resultId\":\"4\",\"edits\":[{\"start\":9,\"deleteCount\":1}]}")));
        CHECK(st.data.empty() && st.resultId.empty());
        CHECK(!st.apply(parsed("{\"resultId\":\"5\",\"data\":[0,0,1]}")));   // not fives
        CHECK(st.data.empty() && st.resultId.empty());
        CHECK(!st.apply(Json()) && !st.apply(parsed("[]")));
    }

    GROUP("lsp:semantic-client");
    {
        FakeServer srv;
        Lsp::Client client([&](const std::string &b) { srv.take(b); });
        int refreshed = 0;
        client.onSemanticTokensRefresh = [&] { refreshed++; };
        client.initialize("/w", 1);
        const Json &caps = srv.sent[0]["params"]["capabilities"];
        const Json &sem = caps["textDocument"]["semanticTokens"];
        CHECK(sem["requests"]["full"]["delta"].asBool() && !sem["requests"]["range"].asBool(true));
        CHECK(sem["formats"][0].asString() == "relative");
        CHECK(!sem["multilineTokenSupport"].asBool(true) &&
              !sem["overlappingTokenSupport"].asBool(true));
        CHECK(sem["augmentsSyntaxTokens"].asBool() && sem["tokenTypes"].size() >= 20);
        CHECK(caps["workspace"]["semanticTokens"]["refreshSupport"].asBool());
        client.receive(reply(1, std::string("{\"capabilities\":") + kBasedPyrightCaps + "}"));
        CHECK(client.state() == Lsp::Client::State::Ready);
        CHECK(client.offers(Lsp::Feature::SemanticTokens));
        client.didOpen("file:///w/a.py", "python", "x = 1\n");

        Json got;
        int id = client.semanticTokensFull("file:///w/a.py", [&](Json &r, const Json &) {
            got = std::move(r);
        });
        CHECK(srv.sent.back()["method"].asString() == "textDocument/semanticTokens/full");
        CHECK(srv.sent.back()["params"]["textDocument"]["uri"].asString() == "file:///w/a.py");
        client.receive(reply(id, "{\"resultId\":\"7\",\"data\":[0,0,1,6,0]}"));
        Lsp::SemanticTokens st;
        CHECK(st.apply(got) && st.resultId == "7");
        id = client.semanticTokensDelta("file:///w/a.py", st.resultId, [&](Json &r, const Json &) {
            got = std::move(r);
        });
        CHECK(srv.sent.back()["method"].asString() == "textDocument/semanticTokens/full/delta");
        CHECK(srv.sent.back()["params"]["previousResultId"].asString() == "7");
        client.receive(reply(id, "{\"resultId\":\"8\",\"edits\":[{\"start\":0,\"deleteCount\":5,"
                                 "\"data\":[0,4,1,6,0]}]}"));
        CHECK(st.apply(got) && st.resultId == "8" && st.data[1] == 4);

        // The server's refresh is answered, then the owner is told.
        client.receive(Lsp::Framer::frame(
            "{\"jsonrpc\":\"2.0\",\"id\":41,\"method\":\"workspace/semanticTokens/refresh\"}"));
        CHECK(refreshed == 1 && srv.sent.back()["id"].asInt() == 41 &&
              srv.sent.back().has("result") && !srv.sent.back().has("error"));
    }
}
