// markdown_media_tests.inc — the `md:img-size`, `md:details` and
// `md:media` groups of run_tests.cpp:
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

    GROUP("md:details");
    {
        auto has = [](const std::vector<MdRun> &r, const std::string &t) {
            return mdText(r).find(t) != std::string::npos;
        };
        auto shut = [](int, bool) { return false; };
        auto openAll = [](int, bool) { return true; };
        auto asTag = [](int, bool open) { return open; };
        // GitHub's way: a summary, a blank line, Markdown, the closing tag.
        const std::string md = "Before\n\n<details>\n<summary>More</summary>\n\nIn **bold**.\n\n"
                               "</details>\n\nAfter\n";
        auto r = P::parse(md);
        const MdRun more = mdRun(r, "More");
        CHECK(more.summary && more.details == 1 && more.detailsLine == 2 && !more.detailsOpen &&
              more.bold);
        const MdRun body = mdRun(r, "bold");
        CHECK(body.bold && !body.summary && body.details == 1 && body.detailsLine == 2);
        CHECK(mdRun(r, "After").details == 0 && mdRun(r, "Before").details == 0);
        // Folded shut: the summary behind a triangle, the body gone, one gap
        // on each side of the summary.
        auto closed = P::fold(r, shut);
        CHECK(!has(closed, "bold") && has(closed, "\xE2\x96\xB8 More"));
        CHECK(mdText(closed) == "Before\n\n\xE2\x96\xB8 More\n\nAfter\n");
        CHECK(mdRun(closed, "\xE2\x96\xB8 ").summary);
        // Open: a downward triangle and everything.
        auto open = P::fold(r, openAll);
        CHECK(has(open, "\xE2\x96\xBE More") && has(open, "In bold."));
        CHECK(mdRun(open, "bold").detailsOpen && mdRun(open, "More").detailsOpen);
        // The tag's own open attribute is the default.
        auto tagOpen = P::parse("<details open>\n<summary>S</summary>\n\nBody\n\n</details>\n");
        CHECK(mdRun(tagOpen, "S").detailsOpen);
        CHECK(has(P::fold(tagOpen, asTag), "Body") && !has(P::fold(r, asTag), "bold"));
        // The block is asked about by its tag's line.
        int asked = -1;
        P::fold(r, [&](int line, bool) { asked = line; return false; });
        CHECK(asked == 2);
        // All on one line: the summary still ends its own line.
        auto one = P::parse("<details><summary>S</summary>Hidden body</details>\n");
        CHECK(mdText(P::fold(one, shut)) == "\xE2\x96\xB8 S\n");
        CHECK(has(P::fold(one, openAll), "Hidden body"));
        CHECK(!mdAny(one, [](const MdRun &x) { return x.hardBreak && x.summary; }));
        // No summary: "Details", as a browser shows it.
        auto bare = P::parse("<details>\n\nSecret\n\n</details>\n\nEnd\n");
        CHECK(mdText(P::fold(bare, shut)) == "\xE2\x96\xB8 Details\n\nEnd\n");
        CHECK(has(P::fold(bare, openAll), "Secret"));
        // Nested: a closed outer block hides the inner one's summary too.
        auto nested = P::parse("<details>\n<summary>Outer</summary>\n\n<details>\n"
                               "<summary>Inner</summary>\n\nDeep\n\n</details>\n\n"
                               "</details>\n");
        CHECK(mdRun(nested, "Inner").details == 2 && mdRun(nested, "Deep").details == 2);
        auto outerShut = P::fold(nested, [](int line, bool) { return line != 0; });
        CHECK(!has(outerShut, "Inner") && !has(outerShut, "Deep") && has(outerShut, "Outer"));
        auto innerShut = P::fold(nested, [](int line, bool) { return line == 0; });
        CHECK(has(innerShut, "Inner") && !has(innerShut, "Deep"));
        // A port that does not fold shows it all, the summary on its own line.
        CHECK(mdText(r).find("More\n\nIn bold.") != std::string::npos);
        // A page with none comes back as it was.
        auto plain = P::parse("# T\n\ntext\n");
        CHECK(mdText(P::fold(plain, shut)) == mdText(plain));
        // One left open at the end leaves the footnotes alone.
        auto unclosed = P::parse("<details>\n<summary>S</summary>\n\nA[^1].\n\n[^1]: Note.\n");
        CHECK(mdRun(unclosed, "Note.").details == 0);
        // A double-click inside an open body still finds its paragraph.
        const MarkdownEdit::Block b = MarkdownEdit::blockAt(md, 5);
        CHECK(b.kind == MarkdownEdit::Block::Paragraph &&
              md.substr(b.start, b.end - b.start) == "In **bold**.");
        // The side preview's pieces see a fold change.
        const auto pc = MarkdownPage::split(closed), po = MarkdownPage::split(open);
        bool differs = pc.size() != po.size();
        for (size_t k = 0; !differs && k < pc.size(); k++)
            differs = !MarkdownPage::alike(pc[k], po[k]);
        CHECK(differs);
        // Export as HTML writes real <details>, open as the tag says.
        const std::string html = MarkdownHtml::page(md, "t");
        CHECK(html.find("<details>\n<summary><strong>More</strong></summary>\n<p>In "
                        "<strong>bold</strong>.</p>\n</details>\n<p>After</p>") != std::string::npos);
        CHECK(MarkdownHtml::page("<details open>\n<summary>S</summary>\n\nB\n\n</details>\n", "t")
                  .find("<details open>") != std::string::npos);
        // Unbalanced and deep tags stay cheap and harmless.
        std::string deep;
        for (int k = 0; k < 5000; k++) deep += "<details><summary>s</summary>\n";
        for (int k = 0; k < 3000; k++) deep += "</details></summary></details>\n";
        const double t0 = cpuMs();
        auto many = P::parse(deep);
        P::fold(many, shut);
        MarkdownHtml::page(deep, "t");
        CHECK_TIME(cpuMs() - t0, 1000);
    }

    GROUP("md:media");
    {
        auto firstMedia = [](const std::string &md) {
            for (const MdRun &r : P::parse(md))
                if (r.media) return r;
            return MdRun();
        };
        // <video src>, with its poster and size; the fallback inside is left out.
        auto v = P::parse("<video src=\"clip.mp4\" poster=\"still.png\" width=\"320\" controls>"
                          "Your browser cannot play this.</video>\n\nAfter\n");
        const MdRun video = firstMedia("<video src=\"clip.mp4\" poster=\"still.png\" width=\"320\">"
                                       "x</video>\n");
        CHECK(video.media == 1 && video.src == "clip.mp4" && video.poster == "still.png" &&
              video.width.value == 320 && video.text == "clip.mp4" && !video.image);
        CHECK(mdText(v).find("cannot play") == std::string::npos && mdText(v).find("After") != std::string::npos);
        // A <source> inside gives the address when the tag has none.
        const MdRun sourced = firstMedia("<video controls>\n<source src=\"media/a.mov\" type=\"video/quicktime\">\n"
                                         "Fallback text\n</video>\n");
        CHECK(sourced.media == 1 && sourced.src == "media/a.mov" && sourced.text == "a.mov");
        CHECK(mdText(P::parse("<video controls>\n<source src=\"a.mov\">\nFallback text\n</video>\n"))
                  .find("Fallback") == std::string::npos);
        // Audio, inline in a paragraph too.
        const MdRun audio = firstMedia("Listen: <audio src=\"https://x.org/talk.mp3?dl=1\"></audio> now.\n");
        CHECK(audio.media == 2 && audio.src == "https://x.org/talk.mp3?dl=1" && audio.text == "talk.mp3");
        CHECK(mdText(P::parse("Listen: <audio src=\"t.mp3\"></audio> now.\n")).find("now.") !=
              std::string::npos);
        // A picture whose address is a video or a sound is a player.
        const MdRun pic = firstMedia("![The demo](demo/run.MP4)\n");
        CHECK(pic.media == 1 && !pic.image && pic.text == "The demo" && pic.src == "demo/run.MP4");
        CHECK(firstMedia("![](a.m4a#t=3)\n").media == 2 && firstMedia("![](a.m4a)\n").text == "a.m4a");
        CHECK(firstMedia("![x](a.png)\n").media == 0 && firstMedia("![x](a.mp4.png)\n").media == 0);
        CHECK(P::mediaKind("x.webm") == 0 && P::mediaKind("dir.mp4/file") == 0 &&
              P::mediaKind("a.wav") == 2 && P::mediaKind("a.m4v?x=1") == 1);
        // No address at all: nothing shows, and no player.
        CHECK(firstMedia("<video controls></video>\n").media == 0);
        // The side preview's pieces see the address and the poster.
        auto one = P::parse("<video src=a.mp4 poster=p.png></video>\n");
        auto two = P::parse("<video src=a.mp4 poster=q.png></video>\n");
        const auto pa = MarkdownPage::split(one), pb = MarkdownPage::split(two);
        CHECK(pa.size() == 1 && pb.size() == 1 && !MarkdownPage::alike(pa[0], pb[0]));
        // Export as HTML: players with the browser's controls, for relative
        // and https addresses only.
        const std::string html = MarkdownHtml::page(
            "<video src=\"clip.mp4\" poster=\"still.png\" width=\"320\"></video>\n\n"
            "![talk](https://x.org/t.mp3)\n\n<video src=\"javascript:alert(1)\"></video>\n", "t");
        CHECK(html.find("<video controls src=\"clip.mp4\" poster=\"still.png\" width=\"320\">clip.mp4</video>") !=
              std::string::npos);
        CHECK(html.find("<audio controls src=\"https://x.org/t.mp3\">talk</audio>") != std::string::npos);
        CHECK(html.find("javascript") == std::string::npos || html.find("src=\"javascript") == std::string::npos);
    }
}
