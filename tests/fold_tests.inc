// fold_tests.inc — included by run_tests.cpp. Code folding: the regions of
// each kind of file, sanitizing ranges, the folds that are closed as the
// text is edited, and the language server's foldingRange.

namespace {

// "start-end" for each range, space separated, to compare in one CHECK.
std::string foldList(const std::vector<Fold::Range> &rs) {
    std::string s;
    for (const Fold::Range &r : rs) {
        if (!s.empty()) s += ' ';
        s += std::to_string(r.start) + "-" + std::to_string(r.end);
    }
    return s;
}

std::string foldsOf(const std::u16string &t, const std::string &name) {
    return foldList(Fold::compute(t, name));
}

}  // namespace

static void testFolding() {
    GROUP("fold:method");
    CHECK(Fold::methodFor("a.cpp") == Fold::Method::Brackets);
    CHECK(Fold::methodFor("/x/y/App.TSX") == Fold::Method::Brackets);
    CHECK(Fold::methodFor("p.json") == Fold::Method::Brackets);
    CHECK(Fold::methodFor("refs.bib") == Fold::Method::Brackets);
    CHECK(Fold::methodFor("a.py") == Fold::Method::Indentation);
    CHECK(Fold::methodFor("c.yaml") == Fold::Method::Indentation);
    CHECK(Fold::methodFor("Makefile") == Fold::Method::Indentation);
    CHECK(Fold::methodFor("notes.txt") == Fold::Method::Indentation);
    CHECK(Fold::methodFor("README.md") == Fold::Method::Markdown);
    CHECK(Fold::methodFor("paper.tex") == Fold::Method::Latex);

    GROUP("fold:python");
    {
        std::u16string t = u16(
            "import os\n"            // 0
            "\n"                     // 1
            "def f(x):\n"            // 2
            "    if x:\n"            // 3
            "        return 1\n"     // 4
            "\n"                     // 5
            "    return 2\n"         // 6
            "\n"                     // 7
            "\n"                     // 8
            "class A:\n"             // 9
            "    def g(self):\n"     // 10
            "        pass\n"         // 11
            "    y = [\n"            // 12
            "        1,\n"           // 13
            "    ]\n");              // 14
        // Blank lines inside a block belong to it, those after it do not.
        CHECK(foldsOf(t, "a.py") == "2-6 3-4 9-14 10-11 12-13");
        // A docstring folds on its own, and its lines' indentation counts
        // for nothing.
        std::u16string d = u16(
            "def f():\n"             // 0
            "    \"\"\"Doc.\n"       // 1
            "\n"                     // 2
            "text at the left\n"     // 3
            "    \"\"\"\n"           // 4
            "    return 1\n"         // 5
            "x = 1\n");              // 6
        CHECK(foldsOf(d, "a.py") == "0-5 1-4");
        // Tabs count to the next multiple of four.
        CHECK(foldsOf(u16("if a:\n\tb\n    c\nd\n"), "a.py") == "0-2");
        CHECK(foldsOf(u16(""), "a.py").empty());
        CHECK(foldsOf(u16("x = 1"), "a.py").empty());
        // A last line with no line break.
        CHECK(foldsOf(u16("def f():\n    pass"), "a.py") == "0-1");
    }

    GROUP("fold:yaml");
    CHECK(foldsOf(u16("a:\n  b: 1\n  c:\n    - d\n    - e\nf: 2\n"), "c.yml") == "0-4 2-4");

    GROUP("fold:brackets");
    {
        std::u16string t = u16(
            "int main() {\n"         // 0
            "    if (x) {\n"         // 1
            "        y();\n"         // 2
            "    } else {\n"         // 3
            "        z();\n"         // 4
            "    }\n"                // 5
            "    int a[] = {1,\n"    // 6
            "               2};\n"   // 7
            "    call(\"{\",\n"      // 8
            "         '}');\n"       // 9
            "}\n");                  // 10
        // The closer's line stays in view when it starts the line; a closer
        // after text on its line folds with it. Brackets in strings are text.
        CHECK(foldsOf(t, "a.c") == "0-9 1-2 3-4 6-7 8-9");
        // Comments: a block comment, a run of line comments, and brackets
        // inside them ignored.
        std::u16string c = u16(
            "/* License\n"           // 0
            " * { text\n"            // 1
            " */\n"                  // 2
            "// one\n"               // 3
            "// two (\n"             // 4
            "int x;  // three\n"     // 5
            "// four\n");            // 6
        CHECK(foldsOf(c, "a.cpp") == "0-2 3-4");
        // JSON.
        CHECK(foldsOf(u16("{\n  \"a\": [\n    1\n  ],\n  \"b\": {}\n}\n"), "p.json") == "0-4 1-2");
        // Mismatched closers are skipped, not paired wrongly.
        CHECK(foldsOf(u16("f(\n  ]\n  x\n)\n"), "a.js") == "0-2");
        // JS template literals and regexes are not code.
        CHECK(foldsOf(u16("const s = `\n{\n`;\nf({\n  a\n});\n"), "a.js") == "3-4");
    }

    GROUP("fold:markers");
    {
        std::u16string t = u16(
            "// #region setup\n"     // 0
            "int a;\n"               // 1
            "//region inner\n"       // 2
            "int b;\n"               // 3
            "// endregion\n"         // 4
            "// #endregion\n"        // 5
            "int c;\n");             // 6
        CHECK(foldsOf(t, "a.ts") == "0-5 2-4");
        std::u16string p = u16("# region Helpers\nx = 1\ny = 2\n# endregion\n#region\nz\n#endregion\n");
        CHECK(foldsOf(p, "a.py") == "0-3 4-6");
        CHECK(foldsOf(u16("#pragma region A\nint a;\n#pragma endregion\n"), "a.cpp") == "0-2");
        CHECK(foldsOf(u16("<!-- #region -->\ntext\n<!-- #endregion -->\n"), "a.md") == "0-2");
        // Not a marker: "regions", or a region with no end.
        CHECK(foldsOf(u16("# regions\nx\n# endregion\n"), "a.py").empty());
        CHECK(foldsOf(u16("// #region\nint a;\n"), "a.c").empty());
    }

    GROUP("fold:markdown");
    {
        std::u16string t = u16(
            "# Title\n"              // 0
            "Intro.\n"               // 1
            "\n"                     // 2
            "## One\n"               // 3
            "Text.\n"                // 4
            "```py\n"                // 5
            "# not a heading\n"      // 6
            "\n"                     // 7
            "x = 1\n"                // 8
            "```\n"                  // 9
            "\n"                     // 10
            "Two\n"                  // 11
            "---\n"                  // 12
            "More.\n"                // 13
            "\n"                     // 14
            "# Next\n"               // 15
            "End.\n");               // 16
        CHECK(foldsOf(t, "a.md") == "0-13 3-9 5-9 11-13 15-16");
        // A heading with nothing under it does not fold.
        CHECK(foldsOf(u16("# A\n# B\n\n"), "a.md").empty());
        // Display math.
        CHECK(foldsOf(u16("Text\n\n$$\na^2\n$$\n"), "a.md") == "2-4");
    }

    GROUP("fold:latex");
    {
        std::u16string t = u16(
            "\\documentclass{article}\n"   // 0
            "\\begin{document}\n"          // 1
            "\\section{Intro}\n"           // 2
            "Text % \\section{no}\n"       // 3
            "\\begin{itemize}\n"           // 4
            "  \\item a\n"                 // 5
            "\\end{itemize}\n"             // 6
            "\\subsection*{Sub}\n"         // 7
            "More.\n"                      // 8
            "\n"                           // 9
            "\\section{Two}\n"             // 10
            "Last.\n"                      // 11
            "\\end{document}\n");          // 12
        CHECK(foldsOf(t, "p.tex") == "1-11 2-8 4-5 7-8 10-11");
        // A section running to the end of a file without \end{document}.
        CHECK(foldsOf(u16("\\chapter{A}\nx\n\\section{B}\ny\n"), "c.tex") == "0-3 2-3");
        CHECK(foldsOf(u16("% #region\nx\n% #endregion\n"), "c.tex") == "0-2");
    }

    GROUP("fold:cr-lines");
    {
        // An old Mac file (lines ending in '\r' alone) folds as the same
        // file with '\n' does, and so does one with U+2029 between lines.
        auto withBreak = [](const std::string &lf, char16_t br) {
            std::u16string t = u16(lf.c_str());
            for (char16_t &c : t)
                if (c == u'\n') c = br;
            return t;
        };
        const std::string py = "def f(x):\n    if x:\n        return 1\n    return 2\n\nx = 3\n";
        const std::string c = "int main() {\n    if (a) {\n        b();\n    }\n    return 0;\n}\n";
        CHECK(!foldsOf(u16(py.c_str()), "a.py").empty());
        CHECK(foldsOf(withBreak(py, u'\r'), "a.py") == foldsOf(u16(py.c_str()), "a.py"));
        CHECK(foldsOf(withBreak(c, u'\r'), "a.c") == foldsOf(u16(c.c_str()), "a.c"));
        CHECK(foldsOf(withBreak(c, 0x2029), "a.c") == foldsOf(u16(c.c_str()), "a.c"));
        CHECK(Fold::contentEnd(u"ab\rcd", 0, 3) == 2);
        CHECK(Fold::contentEnd(u"ab\r\ncd", 0, 4) == 2);
        CHECK(Fold::contentEnd(u"ab\u2029cd", 0, 3) == 2);
    }

    GROUP("fold:sanitize");
    {
        using R = Fold::Range;
        // Out of order, past the end, empty, a second one on a line, and one
        // crossing another's end: what is left nests.
        std::vector<R> in = {{5, 9}, {0, 20}, {2, 3}, {3, 3}, {2, 2}, {4, 6}, {5, 7},
                             {8, 30}, {7, 12}};
        CHECK(foldList(Fold::sanitize(in, 12)) == "0-11 2-3 4-6 7-11 8-11");
        CHECK(Fold::sanitize(in, 0).empty());
        std::vector<R> s = Fold::sanitize({{0, 9}, {1, 4}, {2, 3}, {6, 8}}, 10);
        std::vector<int> d = Fold::depths(s);
        CHECK(d.size() == 4 && d[0] == 1 && d[1] == 2 && d[2] == 3 && d[3] == 2);
        std::vector<size_t> c = Fold::containing(s, 3);
        CHECK(c.size() == 3 && c[0] == 2 && c[1] == 1 && c[2] == 0);
        CHECK(Fold::containing(s, 5).size() == 1 && Fold::containing(s, 12).empty());
        CHECK(Fold::startingAt(s, 6) == 3 && Fold::startingAt(s, 5) == -1);
        CHECK(Fold::kindFromLsp("comment") == Fold::Kind::Comment &&
              Fold::kindFromLsp("imports") == Fold::Kind::Imports &&
              Fold::kindFromLsp("region") == Fold::Kind::Region &&
              Fold::kindFromLsp("") == Fold::Kind::Block);
    }

    GROUP("fold:spans");
    {
        std::u16string t = u"a {\r\n  b\r\n}\r\nend";
        CHECK(Fold::contentEnd(t.data(), 0, 5) == 3);    // before \r\n
        CHECK(Fold::contentEnd(t.data(), 13, 16) == 16); // the last line
        CHECK(Fold::contentEnd(t.data(), 0, 0) == 0);
        Fold::Span s = Fold::spanOf(3, 8);
        CHECK(s.start == 3 && s.end == 8);
    }

    GROUP("fold:collapsed");
    {
        Fold::Collapsed c;
        CHECK(c.add({10, 20}) && !c.add({10, 20}) && !c.add({5, 5}));
        CHECK(c.add({12, 15}) && c.add({30, 40}) && c.size() == 3);
        CHECK(c.outermost().size() == 2 && c.outermost()[0] == (Fold::Span{10, 20}));
        CHECK(c.hiding(10) && c.hiding(10)->end == 20 && c.hiding(14)->start == 10);
        CHECK(!c.hiding(20) && !c.hiding(9) && c.hiding(35)->start == 30);
        // Around a position strictly inside: outermost first.
        std::vector<Fold::Span> a = c.around(13);
        CHECK(a.size() == 2 && a[0].start == 10 && a[1].start == 12);
        CHECK(c.around(10).empty() && c.around(20).empty());
        // A second fold on the same first line replaces the first.
        CHECK(c.add({30, 45}) && c.size() == 3 && c.all()[2].end == 45);
        CHECK(c.remove(30) && !c.remove(30) && c.size() == 2);
        {
            Fold::Collapsed m;
            m.add({10, 20});
            m.add({50, 60});
            CHECK(m.addAll({{70, 80}, {10, 20}, {50, 65}, {5, 5}, {30, 40}, {30, 45}}) == 3);
            CHECK(m.size() == 4 && m.all()[0] == (Fold::Span{10, 20}) &&
                  m.all()[1] == (Fold::Span{30, 40}) && m.all()[2] == (Fold::Span{50, 65}) &&
                  m.all()[3] == (Fold::Span{70, 80}));
        }

        // Edits before a fold move it; at its start without a line break too.
        std::vector<Fold::Span> opened;
        c.edit(0, 0, u"xxx", 3, u'a', &opened);
        CHECK(opened.empty() && c.all()[0] == (Fold::Span{13, 23}));
        c.edit(13, 0, u"xx", 2, u'a', &opened);              // typing at the end of its first line
        CHECK(opened.empty() && c.all()[0] == (Fold::Span{15, 25}));
        c.edit(10, 5, u"", 0, u'a', &opened);              // deleting up to its start
        CHECK(opened.empty() && c.all()[0] == (Fold::Span{10, 20}) &&
              c.all()[1] == (Fold::Span{12, 15}));
        // After it: nothing moves.
        c.edit(21, 4, u"\n", 1, u'a', &opened);
        CHECK(opened.empty() && c.size() == 2);
        // Inside the inner fold: it opens, and the outer one with it.
        c.edit(13, 0, u"x", 1, u'a', &opened);
        CHECK(c.empty() && opened.size() == 2 && opened[0].start == 10 && opened[0].end == 21);

        Fold::Collapsed d;
        d.add({10, 20});
        d.edit(10, 0, u"\n", 1, u'a', &opened);               // Return at the end of its first line
        CHECK(d.empty());
        d.add({10, 20});
        d.edit(20, 0, u"x", 1, u'a');                       // typing after the placeholder
        CHECK(d.empty());
        d.add({10, 20});
        d.edit(20, 1, u"", 0, u'a');                       // deleting the line break after it
        CHECK(d.empty());
        d.add({10, 20});
        d.edit(20, 0, u"\n    ", 5, u'a');                  // Return after the placeholder
        CHECK(d.size() == 1 && d.all()[0] == (Fold::Span{10, 20}));
        d.edit(20, 5, u"", 0, u'\n');                      // and undoing it
        CHECK(d.size() == 1 && d.all()[0] == (Fold::Span{10, 20}));
        d.edit(20, 1, u"", 0, 0);                          // the last line break of the text
        CHECK(d.size() == 1);
        d.edit(3, 0, u"\n", 1, u'a');                        // a new line above it
        CHECK(d.size() == 1 && d.all()[0] == (Fold::Span{11, 21}));
        d.clear();
        d.add({10, 20});
        d.edit(5, 5, u"\nx", 2, u'a');                         // a pasted line break at its start
        CHECK(d.empty());
        d.add({10, 20});
        d.edit(5, 5, u"xx", 2, u'a');                        // replacing text before it
        CHECK(d.size() == 1 && d.all()[0] == (Fold::Span{7, 17}));
        d.edit(0, 100, u"", 0, u'a');                      // the whole text replaced
        CHECK(d.empty());
    }

    GROUP("fold:timing");
    {
        // 100,000 lines of Python and of C, as the editor computes them off
        // the main thread.
        std::u16string py, c;
        for (int i = 0; i < 25000; i++) {
            py += u"def f" + u16(std::to_string(i)) + u"(x):\n    if x:\n        return \"{\"\n\n";
            c += u"int f" + u16(std::to_string(i)) + u"(int x) {\n  if (x) {\n    return '{';\n  }\n";
            if (i % 2) c += u"}\n"; else c += u"}\n";
        }
        const double t0 = cpuMs();
        std::vector<Fold::Range> a = Fold::compute(py, "big.py");
        const double t1 = cpuMs();
        std::vector<Fold::Range> b = Fold::compute(c, "big.c");
        const double t2 = cpuMs();
        CHECK(a.size() == 50000 && b.size() == 50000);
        std::printf("  folding 100,000 lines: Python %.1f ms, C %.1f ms (unoptimized)\n",
                    t1 - t0, t2 - t1);
        CHECK_TIME(t1 - t0, 1500);
        CHECK_TIME(t2 - t1, 1500);
    }

    GROUP("lsp:folding-parse");
    {
        std::vector<Lsp::FoldingRange> f = Lsp::parseFoldingRanges(parsed(
            "[{\"startLine\":0,\"endLine\":4},"
            "{\"startLine\":1,\"startCharacter\":5,\"endLine\":2,\"kind\":\"comment\"},"
            "{\"startLine\":3,\"endLine\":3},"
            "{\"startLine\":-1,\"endLine\":3},"
            "{\"startLine\":\"2\",\"endLine\":3},"
            "{\"startLine\":1.5,\"endLine\":3},"
            "{\"endLine\":3}]"));
        CHECK(f.size() == 2 && f[0].startLine == 0 && f[0].endLine == 4 && f[0].kind.empty() &&
              f[1].kind == "comment");
        CHECK(Lsp::parseFoldingRanges(parsed("null")).empty());
        CHECK(Lsp::parseFoldingRanges(parsed("{}")).empty());
    }

    GROUP("lsp:folding-client");
    {
        FakeServer srv;
        Lsp::Client client([&](const std::string &b) { srv.take(b); });
        client.initialize("/p", 1);
        const Json &caps = srv.sent[0]["params"]["capabilities"]["textDocument"];
        CHECK(caps["foldingRange"]["lineFoldingOnly"].asBool());
        CHECK(caps["foldingRange"]["foldingRangeKind"]["valueSet"].size() == 3);
        client.receive(reply(1, "{\"capabilities\":{\"foldingRangeProvider\":true}}"));
        CHECK(client.offers(Lsp::Feature::FoldingRange));
        std::vector<Lsp::FoldingRange> got;
        int id = client.foldingRanges("file:///p/a.py", [&](const Json &r, const Json &) {
            got = Lsp::parseFoldingRanges(r);
        });
        CHECK(srv.sent.back()["method"].asString() == "textDocument/foldingRange" &&
              srv.sent.back()["params"]["textDocument"]["uri"].asString() == "file:///p/a.py");
        client.receive(reply(id, "[{\"startLine\":2,\"endLine\":6,\"kind\":\"region\"}]"));
        CHECK(got.size() == 1 && got[0].endLine == 6 && got[0].kind == "region");
        CHECK(!Lsp::offers(parsed("{\"hoverProvider\":true}"), Lsp::Feature::FoldingRange));
        CHECK(Lsp::offers(parsed("{\"foldingRangeProvider\":{}}"), Lsp::Feature::FoldingRange));
    }
}
