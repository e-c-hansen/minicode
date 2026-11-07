// latex_tests.inc — the `synctex:forward` and `latexlog:*` groups of
// run_tests.cpp: where a source line went on the typeset pages
// (SyncTexIndex::boxesForLine, over tests/latex/forward.synctex, which
// tectonic 0.17.0 wrote for tests/latex/forward.tex), and the problems in
// tectonic's output (LatexLog.h, over the real logs in tests/latex/logs).
// Included there.

namespace {

std::string ReadFixture(const std::string &path) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::string out;
    char buf[4096];
    std::size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, got);
    std::fclose(f);
    return out;
}

bool Near(double a, double b) { return std::fabs(a - b) < 0.05; }

}  // namespace

static void testLatexForwardAndLog() {
    GROUP("synctex:forward");
    const std::string fwd = ReadFixture("tests/latex/forward.synctex");
    if (fwd.empty()) {
        std::printf("    (skipped: run from the repository root for tests/latex)\n");
    } else {
        SyncTexIndex idx = SyncTexIndex::parse(fwd);
        const int tag = idx.tagForPath("/elsewhere/.fwd.minicode.tex");
        CHECK(tag == 1);
        int found = -1;
        // "First paragraph words here." (line 3): the first line of text on
        // page 1, whose own box says line 4, where its paragraph ended.
        std::vector<SyncTexBox> b = idx.boxesForLine(tag, 3, &found);
        CHECK(found == 3);
        CHECK(b.size() == 1);
        const double baseline1 = 8865055 / 65536.0, baseline2 = 9651487 / 65536.0;
        if (!b.empty()) {
            CHECK(b[0].page == 1);
            CHECK(Near(b[0].x, 8799519 / 65536.0));
            CHECK(Near(b[0].y, baseline1 - 454820 / 65536.0));
            CHECK(Near(b[0].width, 22609920 / 65536.0));
            CHECK(Near(b[0].height, (454820 + 135003) / 65536.0));
        }
        // Lines 5 and 6 are one paragraph set on one line of text; the
        // paragraph's box carries line 7, the \newpage, which lights up
        // only through its own kern and glue.
        for (int l : {5, 6}) {
            b = idx.boxesForLine(tag, l, &found);
            CHECK(found == l);
            CHECK(b.size() == 1);
            CHECK(!b.empty() && b[0].page == 1 && Near(b[0].y, baseline2 - 462029 / 65536.0));
        }
        // Line 3's box is not line 5's, though line 7's box holds both
        // paragraphs' ends.
        CHECK(!idx.boxesForLine(tag, 3).empty() && !idx.boxesForLine(tag, 5).empty() &&
              !Near(idx.boxesForLine(tag, 3)[0].y, idx.boxesForLine(tag, 5)[0].y));
        // Page two's text.
        b = idx.boxesForLine(tag, 8, &found);
        CHECK(found == 8);
        CHECK(b.size() == 1 && b[0].page == 2);
        CHECK(!b.empty() && Near(b[0].y, baseline1 - 447611 / 65536.0));
        // The preamble put nothing on a page: the first line below that did.
        b = idx.boxesForLine(tag, 1, &found);
        CHECK(found == 3);
        CHECK(b.size() == 1 && b[0].page == 1);
        // Any input (0) is the same here; another input, or a line far past
        // the end, has nothing.
        CHECK(idx.boxesForLine(0, 8).size() == 1);
        CHECK(idx.boxesForLine(2, 8, &found).empty() && found == 0);
        CHECK(idx.boxesForLine(tag, 500, &found).empty() && found == 0);
        CHECK(idx.boxesForLine(tag, 0).size() == 1);   // as line 1
        // Forward then back: a click in the middle of line 3's box reads
        // line 3 again.
        b = idx.boxesForLine(tag, 3);
        if (!b.empty()) {
            std::vector<SyncTexHit> hits = idx.textHitsAtPoint(
                b[0].page, b[0].x + b[0].width / 4, b[0].y + b[0].height / 2, 4);
            CHECK(!hits.empty() && hits[0].line == 3);
        }
    }
    CHECK(SyncTexIndex::parse("").boxesForLine(0, 1).empty());
    {
        // Only a vertical box carries the line: the smallest one stands.
        SyncTexIndex v = SyncTexIndex::parse(
            "SyncTeX Version:1\nInput:1:/tmp/a.tex\nUnit:1\nContent:\n{1\n"
            "[1,4:0,6553600:6553600,6553600,0\n"
            "[1,4:0,3276800:655360,655360,0\n"
            "]\n]\n}1\n");
        std::vector<SyncTexBox> b = v.boxesForLine(1, 4);
        CHECK(b.size() == 1 && !b.empty() && Near(b[0].width, 10));
    }

    GROUP("latexlog:summary");
    struct Case {
        const char *name, *file;
        int line;
        const char *message;
    };
    const Case cases[] = {
        {"twoerr", ".twoerr.minicode.tex", 3, "Undefined control sequence"},
        {"child", "child", 2, "Undefined control sequence"},
        {"env", ".env.minicode.tex", 5,
         "LaTeX Error: \\begin{itemize} on input line 3 ended by \\end{enumerate}"},
        {"missing-dollar", ".missing-dollar.minicode.tex", 3, "Missing $ inserted"},
        {"missing-sty", ".missing-sty.minicode.tex", 3,
         "LaTeX Error: File `nosuchpackage.sty' not found"},
        {"noend", "", 0, "Emergency stop (job aborted, no legal \\end found)"},
        {"runaway", "", 0, "File ended while scanning use of \\textbf"},
    };
    bool any = false;
    for (const Case &c : cases) {
        const std::string log = ReadFixture(std::string("tests/latex/logs/") + c.name + ".txt");
        if (log.empty()) continue;
        any = true;
        std::vector<LatexLog::Problem> ps = LatexLog::parse(log);
        CHECK(ps.size() == 1);
        if (ps.size() != 1) {
            std::printf("    %s: %zu problems\n", c.name, ps.size());
            continue;
        }
        CHECK(ps[0].severity == LatexLog::Severity::Error);
        CHECK(ps[0].file == c.file);
        CHECK(ps[0].line == c.line);
        CHECK(ps[0].message == c.message);
        if (ps[0].message != c.message || ps[0].file != c.file || ps[0].line != c.line)
            std::printf("    %s: [%s] %d [%s]\n", c.name, ps[0].file.c_str(), ps[0].line,
                        ps[0].message.c_str());
    }
    if (!any) std::printf("    (skipped: run from the repository root for tests/latex/logs)\n");

    GROUP("latexlog:lines");
    {
        using LatexLog::parse;
        // A warning, a Windows line end, a file name holding a colon, the
        // same problem twice, and tectonic's own chatter, which is not a
        // problem.
        std::vector<LatexLog::Problem> ps = parse(
            "warning: paper.tex:4: LaTeX Warning: Reference `x' undefined\r\n"
            "error: my:notes.tex:12: Undefined control sequence\n"
            "error: my:notes.tex:12: Undefined control sequence\n"
            "error: something bad happened inside XeTeX; its output follows:\n"
            "error: the XeTeX engine had an unrecoverable error\n"
            "caused by: halted on potentially-recoverable error as specified\n"
            "error: halted on potentially-recoverable error as specified\n");
        CHECK(ps.size() == 2);
        CHECK(ps.size() > 0 && ps[0].severity == LatexLog::Severity::Warning &&
              ps[0].file == "paper.tex" && ps[0].line == 4 &&
              ps[0].message == "LaTeX Warning: Reference `x' undefined");
        CHECK(ps.size() > 1 && ps[1].file == "my:notes.tex" && ps[1].line == 12);
        CHECK(parse("").empty());
        CHECK(parse("Typesetting...\nAll done.\n").empty());
        // A complaint without a place is still one, with no line.
        ps = parse("error: failed to open input file: nothing.tex\n");
        CHECK(ps.size() == 1 && ps[0].line == 0 && ps[0].file.empty());
        // No summary placed anything: TeX's transcript does, with the file
        // it had open, and back in the main file once the child closed.
        ps = parse(
            "error: !Something went wrong\n"
            "=====\n"
            "(./paper.tex (article.cls (size10.clo)) (./child.tex\n"
            "! Undefined control sequence.\n"
            "l.7 \\foo\n"
            ")\n"
            "! Missing $ inserted.\n"
            "<inserted text>\n"
            "l.12 x^\n"
            "     2\n"
            "! Emergency stop.\n"
            "<*> paper.tex\n"
            "=====\n");
        CHECK(ps.size() == 3);
        CHECK(ps.size() > 0 && ps[0].file == "./child.tex" && ps[0].line == 7 &&
              ps[0].message == "Undefined control sequence");
        CHECK(ps.size() > 1 && ps[1].file == "./paper.tex" && ps[1].line == 12 &&
              ps[1].message == "Missing $ inserted");
        CHECK(ps.size() > 2 && ps[2].line == 0 && ps[2].message == "Something went wrong");
        // The summary placed it: TeX's own block is not read again.
        ps = parse("error: a.tex:3: Undefined control sequence\n(a.tex\n! Undefined control sequence.\nl.3 \\x\n");
        CHECK(ps.size() == 1);
    }

    GROUP("latexlog:source");
    {
        using LatexLog::sourcePath;
        const std::string tex = "/w/doc/paper.tex", sib = "/w/doc/.paper.minicode.tex";
        auto exists = [](const std::string &p) {
            return p == "/w/doc/child.tex" || p == "/w/doc/parts/intro.tex" ||
                   p == "/abs/x.tex" || p == "/w/doc/data";
        };
        CHECK(sourcePath(".paper.minicode.tex", tex, sib, exists) == tex);
        CHECK(sourcePath(".paper.minicode", tex, sib, exists) == tex);
        CHECK(sourcePath("./.paper.minicode.tex", tex, sib, exists) == tex);
        CHECK(sourcePath(sib, tex, sib, exists) == tex);
        CHECK(sourcePath("", tex, sib, exists) == tex);
        CHECK(sourcePath("child", tex, sib, exists) == "/w/doc/child.tex");
        CHECK(sourcePath("./child.tex", tex, sib, exists) == "/w/doc/child.tex");
        CHECK(sourcePath("parts/intro", tex, sib, exists) == "/w/doc/parts/intro.tex");
        CHECK(sourcePath("/abs/x.tex", tex, sib, exists) == "/abs/x.tex");
        CHECK(sourcePath("data", tex, sib, exists) == "/w/doc/data");
        CHECK(sourcePath("missing", tex, sib, exists).empty());
        // A sibling's name in another folder is that folder's file.
        CHECK(sourcePath("/other/.paper.minicode.tex", tex, sib, exists).empty());
    }
}
