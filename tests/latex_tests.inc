// latex_tests.inc — the `synctex:*`, `latex:root-comment` and `latexlog:*`
// groups of run_tests.cpp: where a source line went on the typeset pages
// (SyncTexIndex::boxesForLine, over tests/latex/forward.synctex, which
// tectonic 0.17.0 wrote for tests/latex/forward.tex), an \input file's
// SyncTeX tag, the "% !TEX root" comment, and the problems and warnings in
// tectonic's output and TeX's transcript (LatexLog.h, over the real logs in
// tests/latex/logs). Included there.

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
        const int tag = idx.tagForPath("/elsewhere/.fwd.mutible.tex");
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

    GROUP("synctex:child-tag");
    {
        // tectonic names every input by its absolute path; other engines
        // write them as TeX opened them, relative to where it ran. Inputs
        // 2 to 4 are a class and its options (no path), as tectonic leaves
        // them.
        SyncTexIndex idx = SyncTexIndex::parse(
            "SyncTeX Version:1\n"
            "Input:1:/private/tmp/doc/.main.4242-1.mutible.tex\n"
            "Input:2:\nInput:3:\nInput:4:\n"
            "Input:5:/private/tmp/doc/parts/intro.tex\n"
            "Input:6:./parts/../chapters/two.tex\n"
            "Input:7:/private/tmp/doc/other/intro.tex\n"
            "Input:8:/private/tmp/doc/appendix.tex\n"
            "Unit:1\nContent:\n{1\n(5,3:0,0:100,10,2\n)\n}1\n");
        CHECK(idx.tagForFile("/private/tmp/doc/parts/intro.tex", "/private/tmp/doc") == 5);
        CHECK(idx.tagForFile("/private/tmp/doc/other/intro.tex", "/private/tmp/doc") == 7);
        CHECK(idx.tagForFile("/private/tmp/doc/chapters/two.tex", "/private/tmp/doc") == 6);
        CHECK(idx.tagForFile("/private/tmp/doc/./parts//intro.tex", "/private/tmp/doc") == 5);
        // The same file under another spelling, through `canonical`.
        auto real = [](const std::string &p) {
            return p.compare(0, 5, "/tmp/") == 0 ? "/private" + p : p;
        };
        CHECK(idx.tagForFile("/tmp/doc/parts/intro.tex", "/tmp/doc") == 0);   // two intro.tex
        CHECK(idx.tagForFile("/tmp/doc/parts/intro.tex", "/tmp/doc", real) == 5);
        CHECK(idx.tagForFile("/tmp/doc/chapters/two.tex", "/tmp/doc", real) == 6);
        // A file name one input has stands in for a path that differs (a
        // copy elsewhere); one two inputs have does not.
        CHECK(idx.tagForFile("/elsewhere/appendix.tex", "/private/tmp/doc") == 8);
        CHECK(idx.tagForFile("/elsewhere/intro.tex", "/private/tmp/doc") == 0);
        // Not an input at all.
        CHECK(idx.tagForFile("/private/tmp/doc/notes.tex", "/private/tmp/doc") == 0);
        CHECK(idx.tagForFile("", "/private/tmp/doc") == 0);
        // The child's lines on the page, through its own tag.
        CHECK(idx.boxesForLine(5, 3).size() == 1);
        CHECK(idx.boxesForLine(1, 3).empty());
    }
    GROUP("synctex:input-clicks");
    {
        // The committed two-file fixture (tests/latex/fixtures: main.tex
        // reads chapter.tex and appendix.tex, as tectonic 0.17.0 typeset
        // it): a click in an \input file's text names that file first, a
        // click in the document's own text the document, and the document
        // always comes last when nothing under the point named it.
        const std::string fx = ReadFixture("tests/latex/fixtures/main.synctex");
        if (fx.empty()) {
            std::printf("    (skipped: run from the repository root for tests/latex)\n");
        } else {
            SyncTexIndex idx = SyncTexIndex::parse(fx);
            const int mainTag = idx.tagForPath("@DIR@/.main.mutible.tex");
            CHECK(mainTag == 1);
            CHECK(idx.inputs().size() == 3);   // the engine's own files have no path
            CHECK(idx.pathForTag(7) == "@DIR@/chapter.tex");
            CHECK(idx.inputs().count(8) == 1 && idx.inputs().at(8) == "@DIR@/appendix.tex");
            struct Case { int tag, line, page; };
            for (const Case &c : {Case{7, 5, 1}, Case{7, 3, 1}, Case{8, 2, 2}, Case{1, 4, 1}}) {
                const std::vector<SyncTexBox> b = idx.boxesForLine(c.tag, c.line);
                CHECK(!b.empty());
                if (b.empty()) continue;
                CHECK(b[0].page == c.page);
                const std::vector<int> tags = idx.tagsAtPoint(
                    b[0].page, b[0].x + b[0].width / 4, b[0].y + b[0].height / 2, mainTag);
                CHECK(!tags.empty() && tags.front() == c.tag);
                CHECK(std::count(tags.begin(), tags.end(), mainTag) == 1);
            }
            // A page with nothing on it: the document alone, or nothing.
            CHECK(idx.tagsAtPoint(9, 10, 10, mainTag) == std::vector<int>{mainTag});
            CHECK(idx.tagsAtPoint(9, 10, 10, 0).empty());
        }
        // An \input file is body text from its first line: parsed as a
        // document of its own (no \begin{document}) it has no spans.
        const std::string chapter = ReadFixture("tests/latex/fixtures/chapter.tex");
        if (!chapter.empty()) {
            auto holds = [](const LatexDoc &d, const char *word) {
                for (const LatexSpan &sp : d.spans)
                    if (sp.display.find(word) != std::string::npos) return true;
                return false;
            };
            CHECK(!holds(LatexDoc::parse(chapter), "marmot"));
            const LatexDoc part = LatexDoc::parseInput(chapter);
            CHECK(holds(part, "marmot") && holds(part, "lemur"));
            const LatexSpan *sp = part.spanForClick({5}, "lemur");
            CHECK(sp && chapter.substr(sp->start, sp->end - sp->start).find("lemur in the snow") !=
                            std::string::npos);
            // A full document read as an input keeps to its body.
            const LatexDoc whole = LatexDoc::parseInput(
                "\\documentclass{article}\n\\begin{document}\nBody words here.\n"
                "\\end{document}\nafter the end\n");
            CHECK(holds(whole, "Body words") && !holds(whole, "after the end"));
        }
    }
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

    GROUP("synctex:top-line");
    {
        // Shift+Cmd+V from the page: the document's line whose words are
        // highest in what is on show, on the committed fixture. Its words'
        // glue says the line, never the line of text's own box (which
        // names where the paragraph ended), and the leftmost of a row wins
        // (the row ending "heron" also holds the chapter's first kern).
        const std::string fx = ReadFixture("tests/latex/fixtures/main.synctex");
        if (fx.empty()) {
            std::printf("    (skipped: run from the repository root for tests/latex)\n");
        } else {
            SyncTexIndex idx = SyncTexIndex::parse(fx);
            struct Case { int page; double top, bottom; int tag, line; };
            for (const Case &c : {Case{1, 0, 1000, 1, 3},      // the section heading
                                  Case{1, 140, 1000, 1, 4},    // below it: the walrus
                                  Case{1, 160, 1000, 1, 6},    // below that: the heron
                                  Case{1, 0, 150, 1, 3},
                                  Case{1, 0, 1000, 7, 2},      // the chapter's own lines
                                  Case{1, 175, 1000, 7, 2},
                                  Case{1, 210, 1000, 7, 3},
                                  Case{2, 0, 1000, 1, 9},
                                  Case{1, 140, 150, 1, 0},     // nothing of it there
                                  Case{1, 0, 1000, 5, 0},      // an input with no text
                                  Case{9, 0, 1000, 1, 0}}) {   // no such page
                const int got = idx.topLineInRange(c.page, c.top, c.bottom, c.tag);
                if (got != c.line)
                    std::printf("    page %d, %g to %g, tag %d: line %d, not %d\n", c.page,
                                c.top, c.bottom, c.tag, got, c.line);
                CHECK(got == c.line);
            }
            // Forward and back agree: a line's box on the page reads that line again.
            for (int line : {4, 6}) {
                const std::vector<SyncTexBox> b = idx.boxesForLine(1, line);
                CHECK(!b.empty() && idx.topLineInRange(b[0].page, b[0].y, b[0].y + b[0].height,
                                                       1) == line);
            }
        }
        // A display with no words: the line of text's box stands in.
        SyncTexIndex d = SyncTexIndex::parse(
            "SyncTeX Version:1\nInput:1:/tmp/a.tex\nUnit:1\nContent:\n{1\n"
            "(1,7:0,6553600:6553600,655360,0\n)\n}1\n");
        CHECK(d.topLineInRange(1, 0, 200, 1) == 7);
        CHECK(d.topLineInRange(1, 0, 80, 1) == 0);   // its top is at 90
        CHECK(SyncTexIndex::parse("").topLineInRange(1, 0, 1000, 1) == 0);
    }

    GROUP("latexlog:summary");
    struct Case {
        const char *name, *file;
        int line;
        const char *message;
    };
    const Case cases[] = {
        {"twoerr", ".twoerr.mutible.tex", 3, "Undefined control sequence"},
        {"child", "child", 2, "Undefined control sequence"},
        {"env", ".env.mutible.tex", 5,
         "LaTeX Error: \\begin{itemize} on input line 3 ended by \\end{enumerate}"},
        {"missing-dollar", ".missing-dollar.mutible.tex", 3, "Missing $ inserted"},
        {"missing-sty", ".missing-sty.mutible.tex", 3,
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

    GROUP("latex:root-comment");
    {
        using LatexLog::rootComment;
        using LatexLog::resolveRoot;
        CHECK(rootComment("% !TEX root = main.tex\nText.\n") == "main.tex");
        CHECK(rootComment("%!TeX root=../main.tex\n") == "../main.tex");
        CHECK(rootComment("Text first.\n  % !tex root = paper.tex  \r\n") == "paper.tex");
        CHECK(rootComment("% ! TEX root = my thesis.tex\n") == "my thesis.tex");
        CHECK(rootComment("% !TEX program = xelatex\n% !TEX root = a.tex\n") == "a.tex");
        CHECK(rootComment("").empty());
        CHECK(rootComment("% TEX root = main.tex\n").empty());     // no bang
        CHECK(rootComment("x % !TEX root = main.tex\n").empty());  // not a comment line
        CHECK(rootComment("% !TEX root =\n").empty());
        CHECK(rootComment("% !TEXroot = main.tex\n").empty());
        CHECK(resolveRoot("/w/doc/parts/intro.tex", "../main.tex") == "/w/doc/main.tex");
        CHECK(resolveRoot("/w/doc/intro.tex", "main.tex") == "/w/doc/main.tex");
        CHECK(resolveRoot("/w/doc/intro.tex", "./sub/../main.tex") == "/w/doc/main.tex");
        CHECK(resolveRoot("/w/doc/intro.tex", "/abs/main.tex") == "/abs/main.tex");
        CHECK(resolveRoot("/w/doc/intro.tex", "").empty());
    }

    GROUP("latexlog:source");
    {
        using LatexLog::sourcePath;
        const std::string tex = "/w/doc/paper.tex", sib = "/w/doc/.paper.mutible.tex";
        auto exists = [](const std::string &p) {
            return p == "/w/doc/child.tex" || p == "/w/doc/parts/intro.tex" ||
                   p == "/abs/x.tex" || p == "/w/doc/data";
        };
        CHECK(sourcePath(".paper.mutible.tex", tex, sib, exists) == tex);
        CHECK(sourcePath(".paper.mutible", tex, sib, exists) == tex);
        CHECK(sourcePath("./.paper.mutible.tex", tex, sib, exists) == tex);
        CHECK(sourcePath(sib, tex, sib, exists) == tex);
        CHECK(sourcePath("", tex, sib, exists) == tex);
        CHECK(sourcePath("child", tex, sib, exists) == "/w/doc/child.tex");
        CHECK(sourcePath("./child.tex", tex, sib, exists) == "/w/doc/child.tex");
        CHECK(sourcePath("parts/intro", tex, sib, exists) == "/w/doc/parts/intro.tex");
        CHECK(sourcePath("/abs/x.tex", tex, sib, exists) == "/abs/x.tex");
        CHECK(sourcePath("data", tex, sib, exists) == "/w/doc/data");
        CHECK(sourcePath("missing", tex, sib, exists).empty());
        // A sibling's name in another folder is that folder's file.
        CHECK(sourcePath("/other/.paper.mutible.tex", tex, sib, exists).empty());
    }

    // tests/latex/logs/warnings.txt and warnings.log: what tectonic 0.17.0
    // printed and kept (--keep-logs) for a document that typesets with
    // warnings: an undefined reference and citation on line 4, a label
    // defined twice, a reference long enough that TeX wrapped its line, an
    // \input{parts/chapter} with an undefined reference on its line 1 and
    // an overfull box on its line 3, overfull boxes on line 11 and in the
    // paragraph of lines 13 to 17, a package warning over two lines on line
    // 16, an underfull box on line 18 and a font substitution on line 19.
    GROUP("latexlog:warnings");
    {
        const std::string out = ReadFixture("tests/latex/logs/warnings.txt");
        const std::string log = ReadFixture("tests/latex/logs/warnings.log");
        if (out.empty() || log.empty()) {
            std::printf("    (skipped: run from the repository root for tests/latex/logs)\n");
        } else {
            using LatexLog::Severity;
            auto show = [](const std::vector<LatexLog::Problem> &ps) {
                for (const LatexLog::Problem &p : ps)
                    std::printf("    %d [%s] %d [%s]\n", (int)p.severity, p.file.c_str(), p.line,
                                p.message.c_str());
            };
            // tectonic's own lines: the overfull boxes, each once (it prints
            // each pass's), the paragraph's at its first line; the underfull
            // box and "warnings were issued" left out.
            std::vector<LatexLog::Problem> ps = LatexLog::parse(out);
            CHECK(ps.size() == 3);
            if (ps.size() == 3) {
                CHECK(ps[0].severity == Severity::Warning && ps[0].file == "parts/chapter" &&
                      ps[0].line == 3 &&
                      ps[0].message == "Overfull \\hbox (86.74pt too wide) detected at line 3");
                CHECK(ps[1].file == ".warnings.mutible.tex" && ps[1].line == 11);
                CHECK(ps[2].file == ".warnings.mutible.tex" && ps[2].line == 13 &&
                      ps[2].message.find("in paragraph at lines 13--17") != std::string::npos);
            } else {
                show(ps);
            }
            // The transcript's.
            ps = LatexLog::parseTranscriptWarnings(log);
            CHECK(ps.size() == 6);
            if (ps.size() == 6) {
                for (const LatexLog::Problem &p : ps) CHECK(p.severity == Severity::Warning);
                CHECK(ps[0].file.empty() && ps[0].line == 0 &&
                      ps[0].message == "LaTeX Warning: Label `sec:intro' multiply defined");
                CHECK(ps[1].file == ".warnings.mutible.tex" && ps[1].line == 4 &&
                      ps[1].message == "LaTeX Warning: Reference `sec:missing' on page 1 undefined");
                CHECK(ps[2].file == ".warnings.mutible.tex" && ps[2].line == 4 &&
                      ps[2].message == "LaTeX Warning: Citation `nobody' on page 1 undefined");
                CHECK(ps[3].line == 7 &&
                      ps[3].message == "LaTeX Warning: Reference `a-label-long-enough-to-make-"
                                       "latex-wrap-the-warning-line-in-the-log' on page 1 undefined");
                CHECK(ps[4].file == "parts/chapter" && ps[4].line == 1 &&
                      ps[4].message == "LaTeX Warning: Reference `nowhere' on page 1 undefined");
                CHECK(ps[5].file == ".warnings.mutible.tex" && ps[5].line == 16 &&
                      ps[5].message == "Package mypkg Warning: Something odd here, over two lines");
            } else {
                show(ps);
            }
            // Both, for the preview: nine warnings, no errors.
            ps = LatexLog::collect(out, log);
            CHECK(ps.size() == 9);
            // A failed run's errors come first, its warnings after.
            ps = LatexLog::collect(
                "warning: a.tex:2: Overfull \\hbox (3pt too wide) detected at line 2\n"
                "error: a.tex:5: Undefined control sequence\n",
                "(a.tex\n\nLaTeX Warning: Reference `x' on page 1 undefined on input line 3.\n\n)\n");
            CHECK(ps.size() == 3);
            CHECK(ps.size() == 3 && ps[0].severity == Severity::Error && ps[0].line == 5 &&
                  ps[1].line == 2 && ps[2].line == 3 && ps[2].file == "a.tex");
        }
        // Parentheses in a box's own text and an error's quoted line do not
        // move the file; one in a warning's text does not either.
        std::vector<LatexLog::Problem> ps = LatexLog::parseTranscriptWarnings(
            "(main.tex (child.tex\n"
            "Overfull \\hbox (1pt too wide) in paragraph at lines 1--2\n"
            "[]\\TU/lmr/m/n/10 text (with an open paren\n"
            "\n"
            "! Undefined control sequence.\n"
            "l.3 \\foo (still open\n"
            "                   more)\n"
            "\n"
            "Package hyperref Warning: Token not allowed (here) on input line 4.\n"
            "\n"
            "Class beamer Warning: A note without a line.\n"
            "\n"
            ")\n"
            "LaTeX Warning: Citation `k' on page 2 undefined on input line 9.\n"
            "\n"
            "LaTeX Warning: There were undefined references.\n"
            "LaTeX Font Warning: Font shape `x' undefined\n"
            "(Font)              using `y' instead on input line 12.\n"
            "\n");
        CHECK(ps.size() == 2);
        CHECK(ps.size() > 0 && ps[0].file == "child.tex" && ps[0].line == 4 &&
              ps[0].message == "Package hyperref Warning: Token not allowed (here)");
        CHECK(ps.size() > 1 && ps[1].file == "main.tex" && ps[1].line == 9);
        CHECK(LatexLog::parseTranscriptWarnings("").empty());
        // A log with a warning but no line to place it: left out.
        CHECK(LatexLog::parseTranscriptWarnings(
                  "LaTeX Warning: Rerun to get cross-references right.\n\n").empty());
    }
}
