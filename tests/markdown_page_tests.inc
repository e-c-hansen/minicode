// markdown_page_tests.inc — the `md:page-pieces` and `md:page-match` groups
// of run_tests.cpp: a Markdown page split into pieces that render on their
// own, and a new parse's pieces paired with the last one's, so the preview
// to the side builds again only what an edit changed (MarkdownPage.h).
// Included there.

static void testMarkdownPage() {
    using namespace MarkdownPage;

    GROUP("md:page-pieces");
    {
        const std::string md =
            "# Title\n\nA paragraph\nover two lines.\n\n- one\n- two\n\n> quoted\n>\n> more\n\n"
            "```\ncode\nmore code\n```\n\n| a | b |\n|---|---|\n| c | d |\n\nEnd  \nbroken.\n";
        const std::vector<MdRun> runs = MarkdownParser::parse(md);
        const std::vector<Piece> pieces = split(runs);
        // They cover the runs, in order, with nothing left out.
        bool covers = !pieces.empty() && pieces.front().begin == 0 && pieces.back().end == runs.size();
        for (size_t k = 1; k < pieces.size(); k++) covers = covers && pieces[k].begin == pieces[k - 1].end;
        CHECK(covers);
        // Never inside a quote, a code block, a table, or after a line break
        // inside a paragraph; each starts a line.
        bool clean = true;
        for (const Piece &p : pieces) {
            if (p.begin == 0) continue;
            const MdRun &r = runs[p.begin], &prev = runs[p.begin - 1];
            clean = clean && !r.quote && !r.codeBlock && !prev.codeBlock && !prev.hardBreak &&
                    prev.text.back() == '\n' && !(r.tableId && r.tableId == prev.tableId);
        }
        CHECK(clean);
        auto pieceOf = [&](const std::string &text) {
            for (size_t k = 0; k < pieces.size(); k++)
                for (size_t i = pieces[k].begin; i < pieces[k].end; i++)
                    if (runs[i].text == text) return (long)k;
            return -1L;
        };
        CHECK(pieceOf("quoted") == pieceOf("more") && pieceOf("quoted") >= 0);
        CHECK(pieceOf("code\n") == pieceOf("more code\n"));
        CHECK(pieceOf("c") == pieceOf("a") && pieceOf("a") >= 0);
        CHECK(pieceOf("End") == pieceOf("broken."));
        CHECK(pieceOf("one") != pieceOf("two"));   // a tight list's items stand apart
        CHECK(pieceOf("Title") != pieceOf("A paragraph"));
        CHECK(pieces.size() >= 7);
        // A piece's base is its first line, and the key counts lines from it.
        CHECK(pieces[0].base == 0);
        CHECK(split({}).empty());
    }

    GROUP("md:page-match");
    {
        std::string md;
        for (int k = 0; k < 300; k++)
            md += "## Part " + std::to_string(k) + "\n\nText of part " + std::to_string(k) +
                  " with **bold**.\n\n- [ ] task " + std::to_string(k) + "\n\n";
        const std::vector<MdRun> a = MarkdownParser::parse(md);
        const std::vector<Piece> pa = split(a);
        auto kept = [](const std::vector<long> &m) {
            size_t n = 0;
            for (long v : m) n += v >= 0;
            return n;
        };
        auto ordered = [](const std::vector<long> &m) {
            long last = -1;
            for (long v : m) {
                if (v < 0) continue;
                if (v <= last) return false;
                last = v;
            }
            return true;
        };
        // Unchanged: every piece kept, each in its own place.
        std::vector<long> same0 = match(pa, {}, pa);
        CHECK(kept(same0) == pa.size() && ordered(same0));
        // A word typed in the middle: only its piece is built again.
        std::string typed = md;
        typed.insert(typed.find("Text of part 150") + 5, "new ");
        const std::vector<MdRun> b = MarkdownParser::parse(typed);
        const std::vector<Piece> pb = split(b);
        std::vector<long> m = match(pa, {}, pb);
        CHECK(pb.size() == pa.size() && kept(m) == pa.size() - 1 && ordered(m));
        // A paragraph added at the top moves every line below it: the
        // pieces after the first (which now has a gap before it) are still
        // the same, their lines counted from their own.
        const std::vector<MdRun> c = MarkdownParser::parse("Intro.\n\n" + md);
        const std::vector<Piece> pc = split(c);
        m = match(pa, {}, pc);
        CHECK(pc.size() == pa.size() + 1 && kept(m) == pa.size() - 1 && ordered(m));
        CHECK(m[0] == -1 && m[1] == -1 && m[2] == 1 && pc[2].base == pa[1].base + 2);
        // Ticking a task far down the page: that piece alone.
        std::string ticked = md;
        ticked.replace(ticked.find("- [ ] task 299"), 5, "- [x]");
        const std::vector<MdRun> d = MarkdownParser::parse(ticked);
        m = match(pa, {}, split(d));
        CHECK(kept(m) == pa.size() - 1);
        // A piece that may not be kept (still waiting for a picture) is built.
        std::vector<bool> keep(pa.size(), true);
        keep[3] = false;
        m = match(pa, keep, pa);
        CHECK(kept(m) == pa.size() - 1 && m[3] == -1);
        // A table added above another renumbers the tables: still the same.
        const std::string tables = "| a |\n|---|\n| b |\n\ntext\n";
        const std::vector<MdRun> t1 = MarkdownParser::parse("Intro\n\n" + tables);
        const std::vector<MdRun> t2 =
            MarkdownParser::parse("Intro\n\n| z |\n|---|\n| y |\n\n" + tables);
        const std::vector<Piece> q1 = split(t1), q2 = split(t2);
        m = match(q1, {}, q2);
        CHECK(kept(m) == q1.size());
        // Different text is never taken for the same, and what match goes
        // by (the hashes) agrees with the runs themselves.
        const std::vector<MdRun> e = MarkdownParser::parse("one\n");
        const std::vector<MdRun> f = MarkdownParser::parse("two\n");
        CHECK(!same(e, split(e)[0], f, split(f)[0]) && !alike(split(e)[0], split(f)[0]));
        bool agree = true;
        for (size_t k = 0; k < pb.size(); k++)
            for (size_t j = (k > 3 ? k - 3 : 0); j < pa.size() && j < k + 3; j++)
                agree = agree && alike(pa[j], pb[k]) == same(a, pa[j], b, pb[k]);
        for (size_t k = 1; k < pc.size(); k++)
            agree = agree && alike(pa[k - 1], pc[k]) == same(a, pa[k - 1], c, pc[k]);
        CHECK(agree);
        // Fast enough for every pause in typing at 20,000 lines.
        std::string big;
        for (int k = 0; k < 2000; k++)
            big += "## S" + std::to_string(k) + "\n\nLine one of " + std::to_string(k) +
                   "\nline two.\n\n- a\n- b\n\n```\ncode\n```\n\n";
        const std::vector<MdRun> g = MarkdownParser::parse(big);
        const std::vector<Piece> pg = split(g);
        const double t0 = cpuMs();
        const std::vector<Piece> again = split(g);
        m = match(pg, {}, again);
        CHECK(kept(m) == pg.size());
        CHECK_TIME(cpuMs() - t0, 200);
    }
}
