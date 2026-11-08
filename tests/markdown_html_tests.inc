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
        // A browser strips leading and trailing controls and spaces and
        // removes tabs and line breaks anywhere before it reads the scheme,
        // so these are javascript: links too and are left out (round-6
        // documents defect 2).
        for (const char *md : {"[x](&#9;javascript:alert(1))\n", "[x](< javascript:alert(1)>)\n",
                               "[x](<java\tscript:alert(1)>)\n", "[x](&#x09;javascript:alert(1))\n",
                               "[x](&#X9;JAVASCRIPT:alert(1))\n", "[x](&Tab;javascript:alert(1))\n",
                               "[x](<\x01javascript:alert(1)>)\n", "[x](<\x1f\x02 javascript:x>)\n",
                               "[x](java&#10;script:x)\n", "[x](<java&#13;script:x>)\n",
                               "[x](javascript&colon;x)\n", "[x](&#32;JaVaScRiPt:x)\n",
                               "[x](<jav\tas\ncript:x>)\n", "[x](vbscript:msgbox(1))\n",
                               "[x](VBScript:x)\n", "[x](data:text/html,<b>x</b>)\n",
                               "[x](&#9;data:text/html;base64,PGI+)\n", "[x](file:///etc/passwd)\n",
                               "<a href=\" javascript:x\">x</a>\n", "<a href=\"\tjavascript:x\">x</a>\n"})
            CHECK(body(md).find("href") == std::string::npos);
        // What a browser reads as relative, http, https or mailto stays.
        CHECK(has(body("[x](<  https://x.org/>)\n"), "<a href=\"  https://x.org/\">"));
        CHECK(has(body("[x](HTTPS://x.org)\n"), "href=\"HTTPS://x.org\""));
        CHECK(has(body("[x](notes/a:b.md)\n"), "href=\"notes/a:b.md\""));
        CHECK(has(body("[x](1a:b)\n"), "href=\"1a:b\""));

        // Pictures: relative, https and data:image keep their addresses.
        CHECK(has(body("![a pic](img/p.png)\n"), "<img src=\"img/p.png\" alt=\"a pic\">"));
        CHECK(has(body("![w](https://x.org/p.png)\n"), "<img src=\"https://x.org/p.png\""));
        CHECK(has(body("![d](data:image/png;base64,AAAA)\n"), "src=\"data:image/png;base64,AAAA\""));
        CHECK(body("![plain](http://x.org/p.png)\n") == "<p>plain</p>\n");
        CHECK(body("![f](file:///etc/p.png)\n") == "<p>f</p>\n");
        // Read as a browser reads it, as links are.
        for (const char *md : {"![f](&#9;javascript:x)\n", "![f](< javascript:x>)\n",
                               "![f](<java\tscript:x>)\n", "![f](&Tab;file:///etc/p.png)\n",
                               "![f](<\x01http://x.org/p.png>)\n", "![f](&#9;data:text/html,x)\n",
                               "![f](<da\tta:text/html,x>)\n", "![f](<ht\ntp://x.org/p.png>)\n"})
            CHECK(body(md).find("<img") == std::string::npos);
        CHECK(has(body("![d](< data:image/png;base64,AAAA>)\n"), "<img src=\" data:image/png"));

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

        // Random addresses spelled to slip past a filter (controls, spaces,
        // entity-coded tabs and line breaks before and inside the scheme,
        // mixed case, angle brackets, HTML anchors): no href or src on the
        // page has a scheme off the allow list as a browser reads it. The
        // browser's reading is written out here again, apart from the
        // export's: the attribute's references decoded, leading and
        // trailing C0 controls and spaces stripped, tabs and line breaks
        // removed, then a letter and scheme characters up to a colon.
        auto browserScheme = [](std::string v) {
            std::string d;
            for (size_t i = 0; i < v.size();) {
                if (v[i] == '&') {
                    const size_t semi = v.find(';', i);
                    if (semi != std::string::npos) {
                        const std::string ref = v.substr(i + 1, semi - i - 1);
                        long cp = -1;
                        if (ref == "amp") cp = '&';
                        else if (ref == "lt") cp = '<';
                        else if (ref == "gt") cp = '>';
                        else if (ref == "quot") cp = '"';
                        else if (ref.size() > 1 && ref[0] == '#')
                            cp = (ref[1] == 'x' || ref[1] == 'X') ? std::strtol(ref.c_str() + 2, nullptr, 16)
                                                                   : std::strtol(ref.c_str() + 1, nullptr, 10);
                        if (cp >= 0) {
                            d += cp < 0x80 ? (char)cp : '\x01';
                            i = semi + 1;
                            continue;
                        }
                    }
                }
                d += v[i++];
            }
            size_t a = 0, b = d.size();
            while (a < b && (unsigned char)d[a] <= 0x20) a++;
            while (b > a && (unsigned char)d[b - 1] <= 0x20) b--;
            std::string u;
            for (size_t k = a; k < b; k++)
                if (d[k] != '\t' && d[k] != '\n' && d[k] != '\r') u += d[k];
            if (u.empty() || !std::isalpha((unsigned char)u[0])) return std::make_pair(std::string(), u);
            for (size_t k = 1; k < u.size(); k++) {
                if (u[k] == ':') {
                    std::string s = u.substr(0, k);
                    for (char &c : s) c = (char)std::tolower((unsigned char)c);
                    return std::make_pair(s, u);
                }
                if (!std::isalnum((unsigned char)u[k]) && u[k] != '+' && u[k] != '-' && u[k] != '.')
                    break;
            }
            return std::make_pair(std::string(), u);
        };
        auto attributeValues = [](const std::string &page, const std::string &name) {
            std::vector<std::string> values;
            const std::string open = " " + name + "=\"";
            for (size_t at = page.find(open); at != std::string::npos; at = page.find(open, at + 1)) {
                const size_t from = at + open.size(), to = page.find('"', from);
                if (to == std::string::npos) break;
                values.push_back(page.substr(from, to - from));
            }
            return values;
        };
        static const char *const kPrefixes[] = {"", " ", "  ", "&#9;", "&#x09;", "&#X9;", "&Tab;",
                                                "&#10;", "&#13;", "&#32;", "&NewLine;", "\x01",
                                                "\x1f", "\x7f", "&#0;", "&#1;", "&#x1F;", "\t"};
        static const char *const kSchemes[] = {"javascript", "vbscript", "data", "file", "http",
                                               "https", "mailto", "livescript", "jar"};
        static const char *const kInside[] = {"\t", "&#9;", "&#10;", "&#13;", "&Tab;", "\n", ""};
        int links = 0, pictures = 0;
        bool schemesAllowed = true;
        for (int round = 0; round < 3000; round++) {
            auto next = [&] {
                seed = seed * 1103515245u + 12345u;
                return seed >> 16;
            };
            std::string addr = kPrefixes[next() % (sizeof kPrefixes / sizeof *kPrefixes)];
            std::string scheme = kSchemes[next() % (sizeof kSchemes / sizeof *kSchemes)];
            for (char &c : scheme)
                if (next() % 2) c = (char)std::toupper((unsigned char)c);
            const size_t cut = next() % (scheme.size() + 1);
            const char *inside = next() % 3 ? "" : kInside[next() % (sizeof kInside / sizeof *kInside)];
            addr += scheme.substr(0, cut) + inside + scheme.substr(cut);
            addr += next() % 4 ? ":" : (next() % 2 ? "&colon;" : "&#58;");
            addr += next() % 2 ? "alert(1)" : "image/png;base64,AA";
            if (next() % 3 == 0) addr += kPrefixes[next() % (sizeof kPrefixes / sizeof *kPrefixes)];
            std::string md;
            switch (next() % 4) {
            case 0: md = "[t](" + addr + ")\n"; break;
            case 1: md = "[t](<" + addr + ">)\n"; break;
            case 2: md = "![t](<" + addr + ">)\n"; break;
            default: md = "<a href=\"" + addr + "\">t</a>\n"; break;
            }
            const std::string out = page(md, "f");
            for (const std::string &v : attributeValues(out, "href")) {
                links++;
                const auto read = browserScheme(v);
                const std::string &s = read.first;
                if (!(s.empty() || s == "http" || s == "https" || s == "mailto")) {
                    std::printf("  href=\"%s\" is read as %s: (from %s)\n", v.c_str(), s.c_str(), md.c_str());
                    schemesAllowed = false;
                }
            }
            for (const std::string &v : attributeValues(out, "src")) {
                pictures++;
                const auto read = browserScheme(v);
                const std::string &s = read.first;
                std::string head = read.second.substr(0, 11);
                for (char &c : head) c = (char)std::tolower((unsigned char)c);
                if (!(s.empty() || s == "https" || (s == "data" && head == "data:image/"))) {
                    std::printf("  src=\"%s\" is read as %s: (from %s)\n", v.c_str(), s.c_str(), md.c_str());
                    schemesAllowed = false;
                }
            }
        }
        CHECK(schemesAllowed);
        CHECK(links > 100 && pictures > 50);   // the allowed spellings still come through
    }
}
