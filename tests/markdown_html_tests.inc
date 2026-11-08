// markdown_html_tests.inc — the `md:html-export` group of run_tests.cpp:
// File > Export as HTML…'s page (MarkdownHtml.h). Each construct is checked
// on its own, and tests/markdown/corpus.md against its golden page,
// tests/markdown/corpus.html. After a deliberate change to the page,
// MC_WRITE_GOLDEN=1 ./build/run_tests writes the golden file again; read
// its diff before committing it. Included there.

static void testMarkdownHtml() {
    using namespace MarkdownHtml;
    auto has = [](const std::string &page, const std::string &part) {
        return page.find(part) != std::string::npos;
    };
    // Whether every element the page opens, it closes.
    auto balanced = [](const std::string &page) {
        bool ok = true;
        for (const char *tag : {"ul", "ol", "li", "blockquote", "p", "table", "pre", "section",
                                "a", "strong", "em", "del", "code", "h1", "h2", "h3"}) {
            size_t opens = 0, closes = 0;
            const std::string o = std::string("<") + tag, c = std::string("</") + tag + ">";
            for (size_t at = page.find(o); at != std::string::npos; at = page.find(o, at + 1)) {
                const char next = at + o.size() < page.size() ? page[at + o.size()] : 0;
                if (next == '>' || next == ' ') opens++;
            }
            for (size_t at = page.find(c); at != std::string::npos; at = page.find(c, at + 1)) closes++;
            if (opens != closes) {
                std::printf("  <%s>: %zu opened, %zu closed\n", tag, opens, closes);
                ok = false;
            }
        }
        return ok;
    };
    auto body = [](const std::string &md) {
        const std::string page = MarkdownHtml::page(md, "t");
        const size_t a = page.find("<article class=\"markdown-body\">\n");
        const size_t b = page.find("</article>");
        if (a == std::string::npos || b == std::string::npos) return std::string("?");
        return page.substr(a + 32, b - a - 32);
    };

    GROUP("md:html-export");
    {
        // A standalone page: doctype, charset, its title, a stylesheet,
        // and nothing fetched.
        const std::string p = page("# Hello & Co <3\n\nText.\n", "file.md");
        CHECK(p.rfind("<!DOCTYPE html>\n", 0) == 0);
        CHECK(has(p, "<meta charset=\"utf-8\">") && has(p, "<style>"));
        CHECK(has(p, "<title>Hello &amp; Co &lt;3</title>"));
        CHECK(!has(p, "<script") && !has(p, "<link "));
        CHECK(titleFor(MarkdownParser::parse("Plain.\n"), "notes.md") == "notes.md");
        CHECK(titleFor(MarkdownParser::parse("## Two\n\n# One\n"), "x") == "One");

        // Escaped text; headings with GitHub's anchors, repeats numbered.
        CHECK(body("a < b & \"c\"\n") == "<p>a &lt; b &amp; &quot;c&quot;</p>\n");
        CHECK(body("# Intro\n\n## Intro\n\n### Intro\n") ==
              "<h1 id=\"intro\">Intro</h1>\n<h2 id=\"intro-1\">Intro</h2>\n"
              "<h3 id=\"intro-2\">Intro</h3>\n");
        CHECK(has(body("## Café & 🚀 rocket\n"), "id=\"café---rocket\""));

        // Emphasis, code, breaks, links, and the links that are left out.
        CHECK(body("*i* **b** ~~s~~ `c<d>`\n") ==
              "<p><em>i</em> <strong>b</strong> <del>s</del> <code>c&lt;d&gt;</code></p>\n");
        CHECK(has(body("one  \ntwo\n"), "one<br>\ntwo"));
        CHECK(body("[a **b**](https://x.org/?q=1&r=2)\n") ==
              "<p><a href=\"https://x.org/?q=1&amp;r=2\">a <strong>b</strong></a></p>\n");
        CHECK(body("[rel](docs/a.md#top) [in](#sec)\n") ==
              "<p><a href=\"docs/a.md#top\">rel</a> <a href=\"#sec\">in</a></p>\n");
        CHECK(body("[x](javascript:alert(1))\n") == "<p>x</p>\n");
        CHECK(body("[x](JavaScript:alert(1))\n") == "<p>x</p>\n");
        CHECK(has(body("<someone@example.org>\n"), "href=\"mailto:someone@example.org\""));

        // Pictures: relative, https and data:image keep their addresses.
        CHECK(has(body("![a pic](img/p.png)\n"), "<img src=\"img/p.png\" alt=\"a pic\">"));
        CHECK(has(body("![w](https://x.org/p.png)\n"), "<img src=\"https://x.org/p.png\""));
        CHECK(has(body("![d](data:image/png;base64,AAAA)\n"), "src=\"data:image/png;base64,AAAA\""));
        CHECK(body("![plain](http://x.org/p.png)\n") == "<p>plain</p>\n");
        CHECK(body("![f](file:///etc/p.png)\n") == "<p>f</p>\n");

        // Lists nest; an ordered one keeps its start; items stay together.
        CHECK(body("- a\n  - b\n- c\n") ==
              "<ul>\n<li>a<ul>\n<li>b</li>\n</ul>\n</li>\n<li>c</li>\n</ul>\n");
        CHECK(body("3. x\n4. y\n") == "<ol start=\"3\">\n<li>x</li>\n<li>y</li>\n</ol>\n");
        CHECK(body("- a\n\n1. b\n") == "<ul>\n<li>a</li>\n</ul>\n<ol>\n<li>b</li>\n</ol>\n");
        CHECK(body("- item\n\n  more of it\n") == "<ul>\n<li>item<p>more of it</p>\n</li>\n</ul>\n");
        // Task boxes: disabled checkboxes, a numbered task in its list.
        CHECK(body("- [ ] to do\n- [x] done\n") ==
              "<ul class=\"contains-task-list\">\n"
              "<li class=\"task-list-item\"><input type=\"checkbox\" disabled> to do</li>\n"
              "<li class=\"task-list-item\"><input type=\"checkbox\" disabled checked> done</li>\n"
              "</ul>\n");
        CHECK(has(body("2. [ ] second\n"), "<ol start=\"2\">\n<li class=\"task-list-item\">"));

        // Quotes, a list in a quote, and a quote in a list item.
        CHECK(body("> said\n>> more\n") ==
              "<blockquote>\n<p>said</p>\n<blockquote>\n<p>more</p>\n</blockquote>\n</blockquote>\n");
        CHECK(body("> - a\n> - b\n") ==
              "<blockquote>\n<ul>\n<li>a</li>\n<li>b</li>\n</ul>\n</blockquote>\n");
        CHECK(body("1. step\n\n   > inside\n") ==
              "<ol>\n<li>step<blockquote>\n<p>inside</p>\n</blockquote>\n</li>\n</ol>\n");

        // Code with its language, escaped; a rule.
        CHECK(body("```python\nif a < b:\n    pass\n```\n") ==
              "<pre><code class=\"language-python\">if a &lt; b:\n    pass\n</code></pre>\n");
        CHECK(body("    indented\n") == "<pre><code>indented\n</code></pre>\n");
        CHECK(body("a\n\n---\n\nb\n") == "<p>a</p>\n<hr>\n<p>b</p>\n");

        // Tables: a header row, alignment, and a cell's own markup.
        CHECK(body("| a | b |\n| :-: | --: |\n| *x* | [y](#y) |\n") ==
              "<table>\n<thead>\n<tr><th style=\"text-align: center\">a</th>"
              "<th style=\"text-align: right\">b</th></tr>\n</thead>\n<tbody>\n"
              "<tr><td style=\"text-align: center\"><em>x</em></td>"
              "<td style=\"text-align: right\"><a href=\"#y\">y</a></td></tr>\n</tbody>\n</table>\n");

        // Math as TeX between its delimiters, escaped.
        CHECK(body("Inline $a<b$ here.\n") ==
              "<p>Inline <span class=\"math inline\">\\(a&lt;b\\)</span> here.</p>\n");
        CHECK(has(body("$$\nx^2\n$$\n"), "<div class=\"math display\">\\[x^2\\]</div>"));

        // Footnotes: a raised link each way, the notes in a section of their own.
        const std::string notes = body("A[^n] and B[^n].\n\n[^n]: The note.\n\n    ```sh\n    make\n    ```\n");
        CHECK(has(notes, "<sup class=\"footnote-ref\"><a href=\"#fn-1\" id=\"fnref-1\">1</a></sup>"));
        CHECK(has(notes, "id=\"fnref-1-2\""));
        CHECK(has(notes, "<section class=\"footnotes\">\n<ol>\n<li id=\"fn-1\">The note."));
        CHECK(has(notes, "<pre><code class=\"language-sh\">make\n</code></pre>"));
        CHECK(has(notes, "<a href=\"#fnref-1\" class=\"footnote-backref\">"));
        CHECK(notes.find("<section") < notes.find("<pre>"));
        CHECK(notes.size() > 10 && notes.compare(notes.size() - 11, 11, "</section>\n") == 0);

        // The corpus against its golden page.
        std::ifstream in("tests/markdown/corpus.md");
        if (!in) {
            std::printf("  (tests/markdown/corpus.md not found here; the golden page is skipped)\n");
        } else {
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string got = page(ss.str(), "corpus.md");
            const char *write = std::getenv("MC_WRITE_GOLDEN");
            if (write && *write == '1') {
                std::ofstream("tests/markdown/corpus.html") << got;
                std::printf("  wrote tests/markdown/corpus.html\n");
            }
            std::ifstream g("tests/markdown/corpus.html");
            std::stringstream gs;
            gs << g.rdbuf();
            const std::string want = gs.str();
            if (got != want) {
                size_t k = 0;
                while (k < got.size() && k < want.size() && got[k] == want[k]) k++;
                std::printf("  corpus.html differs at byte %zu: got \"%s\"\n", k,
                            got.substr(k, 60).c_str());
            }
            CHECK(!want.empty() && got == want);
            CHECK(balanced(got));
        }

        // Random documents of the constructs above, nested every way:
        // the page stays balanced and nothing of the source escapes as markup.
        static const char *const kPieces[] = {
            "# H\n", "para <b>x</b> & y\n", "- item\n", "  - nested\n", "1. one\n",
            "2) two\n", "> quote\n", ">> deeper\n", "> - in quote\n", "   > in item\n",
            "```js\n", "code <x>\n", "```\n", "    indented\n", "| a | b |\n", "|---|---|\n",
            "| c | d |\n", "---\n", "- [ ] task\n", "3. [x] done\n", "x[^n]\n", "[^n]: note\n",
            "    more note\n", "$$\n", "a+b\n", "\n", "[l](javascript:x) <script>\n",
            "![p](http://x/p.png)\n", "*a **b** c*\n", "<div>\n", "</div>\n"};
        unsigned seed = 12345;
        bool allBalanced = true, noScript = true;
        for (int round = 0; round < 400; round++) {
            std::string md;
            for (int k = 0; k < 24; k++) {
                seed = seed * 1103515245u + 12345u;
                md += kPieces[(seed >> 16) % (sizeof kPieces / sizeof *kPieces)];
            }
            const std::string out = page(md, "f");
            allBalanced = allBalanced && balanced(out);
            noScript = noScript && out.find("<script") == std::string::npos &&
                       out.find("href=\"javascript") == std::string::npos;
        }
        CHECK(allBalanced && noScript);
    }
}
