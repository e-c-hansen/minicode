// markdown_media_tests.inc — the `md:img-size` group of run_tests.cpp:
// what the parser reads of pictures, video, audio and folding blocks
// written as HTML in Markdown. Included there; mdText, mdRun, mdAny and
// mdCount are its helpers.

static void testMarkdownMedia() {
    using P = MarkdownParser;

    GROUP("md:img-size");
    {
        auto first = [](const std::string &md) {
            for (const MdRun &r : P::parse(md))
                if (r.image) return r;
            return MdRun();
        };
        // Quoted, unquoted, single-quoted; pixels and percentages.
        MdRun a = first("<img src=\"a.png\" width=\"120\">\n");
        CHECK(a.image && a.width.value == 120 && !a.width.percent && a.height.value == 0);
        MdRun b = first("<img src=a.png width=50% height='80'>\n");
        CHECK(b.width.value == 50 && b.width.percent && b.height.value == 80 &&
              !b.height.percent);
        // Read as HTML reads dimensions: spaces, then digits; a fraction
        // and a unit are dropped.
        CHECK(first("<img src=a.png width=\" 120px\">\n").width.value == 120);
        CHECK(first("<img src=a.png width=\"99.7\">\n").width.value == 99);
        CHECK(first("<img src=a.png WIDTH=\"40\">\n").width.value == 40);
        // Junk, nothing, zero and a negative are no size at all.
        for (const char *md : {"<img src=a.png width=\"wide\">\n", "<img src=a.png width>\n",
                               "<img src=a.png width=\"\">\n", "<img src=a.png width=0>\n",
                               "<img src=a.png width=\"-5\">\n", "<img src=a.png width=\"%\">\n"}) {
            const MdRun r = first(md);
            CHECK(r.image && r.width.value == 0 && !r.width.percent);
        }
        // Absurd sizes are capped: nothing shows wider than the line anyway.
        CHECK(first("<img src=a.png width=99999999999>\n").width.value == 10000);
        CHECK(first("<img src=a.png width=250%>\n").width.value == 100);
        // Inline in a paragraph too, and a Markdown picture has none.
        auto para = P::parse("Logo <img src=\"l.png\" height=\"24\"> here.\n");
        CHECK(mdAny(para, [](const MdRun &x) { return x.image && x.height.value == 24; }));
        CHECK(first("![x](a.png)\n").width.value == 0);
        // A changed size is a changed piece of the side preview's page.
        auto one = P::parse("<img src=a.png width=10>\n"), two = P::parse("<img src=a.png width=20>\n");
        const auto pa = MarkdownPage::split(one), pb = MarkdownPage::split(two);
        CHECK(pa.size() == 1 && pb.size() == 1 && !MarkdownPage::alike(pa[0], pb[0]) &&
              !MarkdownPage::same(one, pa[0], two, pb[0]));
        // Export as HTML writes them back.
        const std::string html = MarkdownHtml::page(
            "<img src=\"a.png\" alt=\"A\" width=\"120\" height=\"50%\">\n", "t");
        CHECK(html.find("<img src=\"a.png\" alt=\"A\" width=\"120\" height=\"50%\">") !=
              std::string::npos);
    }
}
