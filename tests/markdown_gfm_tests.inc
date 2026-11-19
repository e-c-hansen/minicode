// markdown_gfm_tests.inc — the `md:footnotes`, `md:html-block`,
// `md:nested-containers` and `mdedit:html` groups of run_tests.cpp: GitHub's
// footnotes, HTML blocks taken as HTML rather than Markdown, and quotes and
// code inside list items (and lists inside quotes) nesting as GitHub draws
// them, and GitHub's alerts, emoji shortcodes and front matter (`md:alerts`,
// `md:emoji`, `md:front-matter`). Included there; mdText, mdRun, mdAny and
// mdCount are its helpers.

static void testMarkdownGfm() {
    using P = MarkdownParser;

    GROUP("md:footnotes");
    {
        auto r = P::parse("Text[^1].\n\n[^1]: The note.\n");
        const MdRun ref = mdRun(r, "1");
        CHECK(ref.footnote == 1 && ref.link && ref.url == "#fn-1" && ref.anchor == "fnref-1");
        CHECK(mdRun(r, "Text").footnote == 0 && mdRun(r, ".").footnote == 0);
        // The definition shows only at the end: a rule, then a numbered list.
        CHECK(mdText(r).find("[^1]") == std::string::npos);
        size_t rule = 0;
        while (rule < r.size() && !r[rule].rule) rule++;
        CHECK(rule < r.size() && mdRun(r, "Text").line == 0);
        bool after = true;
        for (size_t k = 0; k < r.size(); k++)
            if (r[k].text == "The note.") after = k > rule;
        CHECK(after);
        const MdRun marker = mdRun(r, "  1. ");
        CHECK(marker.marker && marker.ordered && marker.footnotes && marker.anchor == "fn-1");
        CHECK(mdRun(r, "The note.").footnotes && mdRun(r, "The note.").listDepth == 1);
        CHECK(mdRun(r, "The note.").line == 2);   // a double-click edits the definition
        const MdRun back = mdRun(r, "\xE2\x86\xA9");   // ↩
        CHECK(back.link && back.url == "#fnref-1" && back.footnotes);
        CHECK(r.back().text == "\n" && r.back().footnotes);

        // Numbered by first reference, not by label or definition order;
        // a second reference gets a second way back.
        auto order = P::parse("A[^z] B[^a] C[^z].\n\n[^a]: Ay.\n[^z]: Zed.\n");
        CHECK(mdCount(order, [](const MdRun &x) { return x.footnote == 1; }) == 2);
        CHECK(mdCount(order, [](const MdRun &x) { return x.footnote == 2; }) == 1);
        std::vector<std::string> listed;
        for (const MdRun &x : order)
            if (x.footnotes && !x.marker && (x.text == "Zed." || x.text == "Ay."))
                listed.push_back(x.text);
        CHECK(listed == (std::vector<std::string>{"Zed.", "Ay."}));
        CHECK(mdAny(order, [](const MdRun &x) { return x.anchor == "fnref-1-2"; }));
        CHECK(mdRun(order, "\xE2\x86\xA9\xC2\xB2").url == "#fnref-1-2");   // ↩²

        // Labels ignore case; an undefined one, a definition no one names, and
        // "[^x](url)" are left alone.
        auto loose = P::parse("See[^Note] and [^none] and [^n](u).\n\n[^note]: N.\n[^n]: M.\n"
                              "[^unused]: U.\n");
        CHECK(mdRun(loose, "1").url == "#fn-1");
        CHECK(mdText(loose).find("[^none]") != std::string::npos);
        CHECK(mdRun(loose, "^n").link && mdRun(loose, "^n").url == "u");
        CHECK(mdText(loose).find("U.") == std::string::npos);
        CHECK(mdText(loose).find("M.") == std::string::npos);
        // No definitions: no list, and nothing changes.
        auto plain = P::parse("Text[^1].\n");
        CHECK(mdText(plain) == "Text[^1].\n");
        CHECK(!mdAny(plain, [](const MdRun &x) { return x.rule || x.footnotes; }));
        // The first definition of a label wins.
        auto twice = P::parse("x[^d]\n\n[^d]: First.\n[^d]: Second.\n");
        CHECK(mdText(twice).find("First.") != std::string::npos);
        CHECK(mdText(twice).find("Second.") == std::string::npos);

        // A definition goes on lazily and over indented paragraphs; a
        // footnote may refer to another, which joins the list.
        auto long_ = P::parse("x[^a]\n\n[^a]: One\ngoes on.\n\n    Two, see[^b].\n\nAfter.\n\n"
                              "[^b]: Bee.\n");
        CHECK(mdRun(long_, "One").footnotes && mdRun(long_, "goes on.").footnotes);
        CHECK(mdRun(long_, "Two, see").footnotes && mdRun(long_, "Two, see").listDepth == 1);
        CHECK(!mdRun(long_, "After.").footnotes);
        CHECK(mdRun(long_, "2").footnote == 2 && mdRun(long_, "2").footnotes);
        CHECK(mdRun(long_, "Bee.").footnotes);
        size_t aBack = 0, two = 0;
        for (size_t k = 0; k < long_.size(); k++) {
            if (long_[k].url == "#fnref-1") aBack = k;
            if (long_[k].text == "Two, see") two = k;
        }
        CHECK(aBack > two);   // the way back ends the footnote's last paragraph
        // A definition interrupts a paragraph, and an empty one still lists.
        auto bare = P::parse("Para[^e]\n[^e]:\n");
        CHECK(mdRun(bare, "Para").line == 0 && mdRun(bare, "  1. ").footnotes);
        // In a heading, a quote, a list item and a table cell.
        auto where = P::parse("# H[^a]\n\n> Q[^a]\n\n- I[^a]\n\n| c |\n|---|\n| T[^a] |\n\n"
                              "[^a]: A.\n");
        CHECK(mdCount(where, [](const MdRun &x) { return x.footnote == 1; }) == 4);
        CHECK(mdAny(where, [](const MdRun &x) { return x.anchor == "fnref-1-4"; }));
        // Lines: the definition's text is what a double-click edits.
        const std::string src = "x[^a]\n\n[^a]: The text.\n";
        auto lines = P::lines(src);
        CHECK(lines[2].kind == MdLine::Text && src.substr(lines[2].start, 9) == "The text.");
        MarkdownEdit::Block b = MarkdownEdit::blockAt(src, 2);
        CHECK(b.kind == MarkdownEdit::Block::Paragraph);
        CHECK(MarkdownEdit::replace(src, b, "New.") == "x[^a]\n\n[^a]: New.\n");
    }

    GROUP("md:html-block");
    {
        // CommonMark's HTML blocks: the inside is HTML, not Markdown.
        auto div = P::parse("<div>\n*hi* [x](y) `c`\n</div>\n");
        CHECK(mdText(div) == "*hi* [x](y) `c`\n");
        CHECK(!mdAny(div, [](const MdRun &x) { return x.italic || x.link || x.code; }));
        // Tags still mean what they mean, entities are read, white space
        // collapses, and block-level tags break the line.
        auto readme = P::parse("<div align=\"center\">\n  <a href=\"https://x.org\"><img "
                               "src=\"badge.svg\" alt=\"Badge\"></a>\n  Tom &amp; "
                               "<b>Jerry</b>\n  <p>Second</p>   line\n</div>\n");
        CHECK(readme[0].image && readme[0].src == "badge.svg" && readme[0].link &&
              readme[0].url == "https://x.org");
        CHECK(mdRun(readme, " Tom & ").text == " Tom & " && mdRun(readme, "Jerry").bold);
        CHECK(mdAny(readme, [](const MdRun &x) { return x.hardBreak; }));
        CHECK(mdRun(readme, "Second").line == 3 && mdRun(readme, "line").line == 3);
        CHECK(mdText(readme) == "Badge Tom & Jerry\nSecond\nline\n");
        // A heading line in HTML stays a heading, inside a block too.
        auto heads = P::parse("<div>\n<h1 align=\"center\">Title</h1>\n<p>under</p>\n</div>\n");
        CHECK(mdRun(heads, "Title").heading == 1 && mdRun(heads, "under").heading == 0);
        // The block ends at a blank line; Markdown follows.
        auto details = P::parse("<details>\n<summary>More</summary>\n\nIn **bold**.\n\n"
                                "</details>\n");
        CHECK(mdRun(details, "More").bold && mdRun(details, "bold").bold);
        CHECK(mdText(details).find("In ") != std::string::npos);
        // A lone tag on its line starts a block (CommonMark's seventh kind),
        // but cannot interrupt a paragraph; one inside a line is inline.
        auto lone = P::parse("<img src=\"a.png\">\n*text*\n");
        CHECK(lone[0].image && mdRun(lone, " *text*").text == " *text*");
        auto inPara = P::parse("Para\n<span>s</span> *em*\n");
        CHECK(mdRun(inPara, "em").italic);
        auto inLine = P::parse("<span>a</span> *em*\n");
        CHECK(mdRun(inLine, "em").italic);
        // A block-level tag may interrupt one.
        auto interrupt = P::parse("Para\n<div>\n*x*\n</div>\n");
        CHECK(mdText(interrupt).find("*x*") != std::string::npos);
        // <script> and <style> show nothing, to their closing tag; <pre> is
        // preformatted; <?...?> and declarations are hidden.
        auto hidden = P::parse("<script>\nalert(1)\n\nstill(2)\n</script>\nafter\n\n<style>"
                               "p{}</style>\n\n<?php x ?>\n\n<!DOCTYPE html>\n\nend\n");
        CHECK(mdText(hidden) == "after\n\nend\n");
        auto pre = P::parse("<pre>\nx &lt; 1\n  <b>y</b>\n\nz\n</pre>\n");
        CHECK(mdRun(pre, "x < 1\n").codeBlock && mdRun(pre, "  y\n").codeBlock);
        CHECK(mdRun(pre, "\n").codeBlock && mdRun(pre, "z\n").codeBlock);
        // Autolinks and comments are not HTML blocks.
        auto auto_ = P::parse("<https://example.com>\n");
        CHECK(auto_[0].link && auto_[0].url == "https://example.com");
        CHECK(mdText(P::parse("<!-- c -->\ntext\n")) == "text\n");
        // In a list item, an HTML block keeps the item's indent.
        auto item = P::parse("- item\n\n  <div>\n  *in*\n  </div>\n");
        CHECK(mdRun(item, "*in*").listDepth == 1);
        // A block of tags alone shows nothing and takes no space.
        CHECK(mdText(P::parse("a\n\n<div>\n</div>\n\nb\n")) == "a\n\nb\n");
    }

    GROUP("mdedit:html");
    {
        const std::string src = "Intro\n\n<div>\n  *x*\n</div>\n\nEnd\n";
        auto lines = P::lines(src);
        CHECK(lines[2].kind == MdLine::Html && lines[3].kind == MdLine::Html &&
              lines[4].kind == MdLine::Html && lines[2].block == lines[4].block);
        MarkdownEdit::Block b = MarkdownEdit::blockAt(src, 3);
        CHECK(b.kind == MarkdownEdit::Block::Html && b.firstLine == 2 && b.lastLine == 4);
        CHECK(src.substr(b.start, b.end - b.start) == "<div>\n  *x*\n</div>");
        CHECK(MarkdownEdit::replace(src, b, "<p>y</p>") == "Intro\n\n<p>y</p>\n\nEnd\n");
        const std::string hid = "<script>\nx\n</script>\n";
        CHECK(MarkdownEdit::blockAt(hid, 1).kind == MarkdownEdit::Block::None);
        CHECK(P::lines(hid)[1].kind == MdLine::Hidden);
        // A heading line inside a block edits as a heading.
        const std::string h = "<div>\n<h2>Title</h2>\n</div>\n";
        MarkdownEdit::Block hb = MarkdownEdit::blockAt(h, 1);
        CHECK(hb.kind == MarkdownEdit::Block::Heading &&
              h.substr(hb.start, hb.end - hb.start) == "Title");
    }

    GROUP("md:nested-containers");
    {
        // A list inside a quote, and a quote inside its item.
        auto q = P::parse("> - a\n>   > b\n");
        CHECK(mdRun(q, "a").quoteDepth == 1 && mdRun(q, "a").listDepth == 1);
        const MdRun b = mdRun(q, "b");
        CHECK(b.quote && b.quoteDepth == 2 && b.listDepth == 1 && b.quoteIndent == 0);
        // A nested quote, and the outer quote's text going on lazily in it.
        auto nest = P::parse("> outer\n>\n> > deep\n> back\n");
        CHECK(mdRun(nest, "outer").quoteDepth == 1 && mdRun(nest, "deep").quoteDepth == 2);
        CHECK(mdRun(nest, "back").quoteDepth == 2 && mdRun(nest, "back").line == 3);
        CHECK(mdAny(nest, [](const MdRun &x) { return x.gap && x.quote && x.quoteDepth == 1; }));
        // A quote inside a list item: at the item's indent, its bar too.
        auto inItem = P::parse("1. step\n\n   > said\n\n   more\n2. next\n");
        const MdRun said = mdRun(inItem, "said");
        CHECK(said.quote && said.listDepth == 1 && said.quoteIndent == 1 && said.quoteDepth == 1);
        CHECK(mdRun(inItem, "more").listDepth == 1 && !mdRun(inItem, "more").quote);
        CHECK(mdRun(inItem, "  2. ").marker);
        // Not indented enough, a quote ends the list.
        auto out = P::parse("- item\n> apart\n");
        CHECK(mdRun(out, "apart").listDepth == 0 && mdRun(out, "apart").quoteIndent == 0);
        // Code in a list item keeps the item's indent.
        auto code = P::parse("1. Run:\n\n   ```sh\n   make\n   ```\n2. Then\n");
        CHECK(mdRun(code, "make\n").codeBlock && mdRun(code, "make\n").listDepth == 1);
        CHECK(mdRun(code, "  2. ").marker);
        CHECK(mdRun(P::parse("```\nx\n```\n"), "x\n").listDepth == 0);
        // "- > text": the item's text is quoted.
        auto qi = P::parse("- > quoted\n");
        CHECK(mdRun(qi, "quoted").quote && mdRun(qi, "  \xE2\x80\xA2 ").marker &&
              !mdRun(qi, "  \xE2\x80\xA2 ").quote);
        CHECK(mdText(qi).find('>') == std::string::npos);
        // A list in a quote ends where the quote's text comes back out.
        auto back = P::parse("> - a\n>\n>   inside a\n>\n> outside\n");
        CHECK(mdRun(back, "inside a").listDepth == 1 && mdRun(back, "outside").listDepth == 0);
        // Blocks still edit as before.
        const std::string src = "> - a\n>   > b\n";
        MarkdownEdit::Block blk = MarkdownEdit::blockAt(src, 1);
        CHECK(blk.kind == MarkdownEdit::Block::Quote && blk.firstLine == 0 && blk.lastLine == 1);
        const std::string items = "1. step\n\n   ```\n   x\n   ```\n";
        MarkdownEdit::Block cb = MarkdownEdit::blockAt(items, 3);
        CHECK(cb.kind == MarkdownEdit::Block::Code);
    }

    GROUP("md:footnote-code");
    {
        // A fence indented under a definition is the footnote's code
        // block, as on GitHub, not inline code holding its info string.
        auto r = P::parse("Text[^1].\n\n[^1]: The note:\n\n    ```py\n    print(1)\n    ```\n\n"
                          "    And after.\n\nBody again.\n");
        const MdRun code = mdRun(r, "print(1)\n");
        CHECK(code.codeBlock && code.footnotes && code.lang == "py" && code.line == 5);
        CHECK(mdText(r).find("py print") == std::string::npos);
        CHECK(mdText(r).find("```") == std::string::npos);
        CHECK(mdRun(r, "And after.").footnotes);
        // The way back ends the note's last paragraph, or after its last
        // code block stands on a line of its own, outside the code.
        CHECK(!mdRun(r, "\xE2\x86\xA9").codeBlock && mdRun(r, "\xE2\x86\xA9").footnotes);
        auto last = P::parse("x[^a]\n\n[^a]: N\n\n    ```\n    a\n    ```\n");
        const MdRun backLink = mdRun(last, "\xE2\x86\xA9");
        CHECK(backLink.link && !backLink.codeBlock && !backLink.code && backLink.footnotes);
        CHECK(mdText(last).find("a\n\xE2\x86\xA9\n") != std::string::npos);
        CHECK(!mdRun(r, "Body again.").footnotes);
        // Body comes before the footnotes, the code inside them.
        size_t body = 0, at = 0;
        for (size_t k = 0; k < r.size(); k++) {
            if (r[k].text == "Body again.") body = k;
            if (r[k].text == "print(1)\n") at = k;
        }
        CHECK(body < at);
        // A fence interrupts the definition's text, as it does a paragraph.
        auto tight = P::parse("x[^a]\n\n[^a]: Run\n    ```\n    make\n    ```\n");
        CHECK(mdRun(tight, "make\n").codeBlock && mdRun(tight, "make\n").footnotes);
        CHECK(mdRun(tight, "Run").footnotes && !mdRun(tight, "Run").code);
        // Eight spaces after a blank line: indented code in the footnote.
        auto indented = P::parse("x[^a]\n\n[^a]: See:\n\n        two = 2\n        three\n\n    Done.\n");
        CHECK(mdRun(indented, "two = 2\n").codeBlock && mdRun(indented, "two = 2\n").footnotes);
        CHECK(mdRun(indented, "three\n").codeBlock);
        CHECK(mdRun(indented, "Done.").footnotes && !mdRun(indented, "Done.").code);
        // The code's lines edit as a code block.
        const std::string src = "x[^a]\n\n[^a]: N\n\n    ```\n    a\n    ```\n";
        CHECK(MarkdownEdit::blockAt(src, 5).kind == MarkdownEdit::Block::Code);
        // With nothing referring to it, nothing shows.
        CHECK(mdText(P::parse("[^z]: N\n\n    ```\n    a\n    ```\n")).find('a') == std::string::npos);
    }

    GROUP("md:list-numbers");
    {
        // An ordered list counts on from its first item's number, whatever
        // the later items say (CommonMark, GitHub): "1. 3. 4. 5." reads
        // 1, 2, 3, 4 (the owner's STATUS.md, 2026-11-18).
        auto markers = [](const std::string &md) {
            std::string out;
            for (const MdRun &r : P::parse(md))
                if (r.marker) out += r.text + "|";
            return out;
        };
        CHECK(markers("1. a\n3. b\n4. c\n5. d\n") == "  1. |  2. |  3. |  4. |");
        CHECK(markers("3. x\n4. y\n9. z\n") == "  3. |  4. |  5. |");
        CHECK(markers("0. a\n0. b\n") == "  0. |  1. |");
        // A loose list (blank lines between items) is still one list.
        CHECK(markers("1. a\n\n1. b\n\n1. c\n") == "  1. |  2. |  3. |");
        // A paragraph under an item keeps the list going; one out of it ends it.
        CHECK(markers("1. a\n\n   more of a\n\n7. b\n") == "  1. |  2. |");
        CHECK(markers("1. a\n\nbetween\n\n7. b\n") == "  1. |  7. |");
        CHECK(markers("1. a\n# Heading\n4. b\n") == "  1. |  4. |");
        // Another delimiter or a bullet starts another list.
        CHECK(markers("1. a\n5) b\n6) c\n") == "  1. |  5) |  6) |");
        CHECK(markers("1. a\n- b\n4. c\n") == "  1. |  \xE2\x80\xA2 |  4. |");
        // Nested lists count on their own, and a sibling of their item ends them.
        CHECK(markers("1. a\n   3. x\n   3. y\n1. b\n   8. z\n") ==
              "  1. |    3. |    4. |  2. |    8. |");
        // In a quote too; a task keeps the counted number before its box.
        CHECK(markers("> 2. a\n> 2. b\n") == "  2. |  3. |");
        CHECK(markers("1. [ ] a\n1. [x] b\n") == "  1. \xE2\x98\x90 |  2. \xE2\x98\x91 |");
        // The HTML export starts the list at the first number and the
        // browser counts on, as the page does.
        const std::string html = MarkdownHtml::page("4. a\n1. b\n", "t");
        CHECK(html.find("<ol start=\"4\">") != std::string::npos);
        CHECK(html.find("start=\"1\"") == std::string::npos);
    }

    GROUP("md:numbered-task");
    {
        // A task in an ordered list keeps its number before the box.
        auto r = P::parse("1. [ ] first\n2. [x] second\n- [ ] bullet\n");
        const MdRun one = mdRun(r, "  1. \xE2\x98\x90 ");
        CHECK(one.marker && one.ordered && one.task == 1);
        const MdRun two = mdRun(r, "  2. \xE2\x98\x91 ");
        CHECK(two.marker && two.task == 2);
        CHECK(mdRun(r, "  \xE2\x98\x90 ").marker);   // a bullet task: the box alone
        CHECK(mdRun(r, "first").task == 1 && mdRun(r, "second").task == 2);
        // Ticking it still finds the box.
        CHECK(MarkdownTasks::toggleBox("1. [ ] first\n", 0) == "1. [x] first\n");
    }

    GROUP("md:alerts");
    {
        // GitHub's alerts: the kind on every run of the quote, the title's
        // runs marked, the quote kept for ports that draw no alert.
        auto r = P::parse("> [!WARNING]\n> Mind the *step*.\n>\n> - a list\n\nafter\n");
        const MdRun title = mdRun(r, "Warning");
        CHECK(title.alert == 4 && title.alertTitle && title.bold && title.quote);
        CHECK(mdRun(r, "Mind the ").alert == 4 && !mdRun(r, "Mind the ").alertTitle);
        CHECK(mdRun(r, "step").alert == 4 && mdRun(r, "step").italic);
        CHECK(mdRun(r, "a list").alert == 4 && mdRun(r, "a list").quote);
        CHECK(mdRun(r, "after").alert == 0 && !mdRun(r, "after").quote);
        CHECK(mdText(r).find("[!WARNING]") == std::string::npos);
        int kinds = 0;
        for (const char *k : {"NOTE", "TIP", "IMPORTANT", "WARNING", "CAUTION"}) {
            auto one = P::parse(std::string("> [!") + k + "]\n> x\n");
            const int a = mdRun(one, "x").alert;
            if (a >= 1 && a <= 5 && std::string(P::alertName(a)) ==
                [](std::string s) { for (char &c : s) c = (char)std::tolower((unsigned char)c); return s; }(k))
                kinds |= 1 << a;
        }
        CHECK(kinds == 0x3E);
        CHECK(std::string(P::alertName(0)).empty() && std::string(P::alertName(6)).empty());
        // Lowercase markers count, as on GitHub; an unknown one, a marker
        // after the first line, and a nested quote's are plain quotes.
        CHECK(mdRun(P::parse("> [!tip]\n> y\n"), "y").alert == 2);
        CHECK(mdRun(P::parse("> [!NOPE]\n> y\n"), "y").alert == 0);
        CHECK(mdRun(P::parse("> first\n> [!NOTE]\n"), "first").alert == 0);
        CHECK(mdRun(P::parse("> > [!NOTE]\n> > y\n"), "y").alert == 0);
        // Two quotes, one an alert: the other stays plain.
        auto two = P::parse("> [!CAUTION]\n> hot\n\n> cold\n");
        CHECK(mdRun(two, "hot").alert == 5 && mdRun(two, "cold").alert == 0);
        // The space between its blocks is in it too, so its bar goes on.
        CHECK(mdAny(r, [](const MdRun &x) { return x.gap && x.quote && x.alert == 4; }));
        CHECK(!mdAny(r, [](const MdRun &x) { return x.gap && x.quote && x.alert == 0; }));
    }

    GROUP("md:emoji");
    {
        CHECK(MarkdownEmoji::count() >= 300);
        CHECK(MarkdownEmoji::lookup("tada") == "\xF0\x9F\x8E\x89");
        CHECK(MarkdownEmoji::lookup("white_check_mark") == "\xE2\x9C\x85");
        CHECK(MarkdownEmoji::lookup("+1") == "\xF0\x9F\x91\x8D");
        // A character whose default is text gets the emoji selector.
        CHECK(MarkdownEmoji::lookup("heart") == "\xE2\x9D\xA4\xEF\xB8\x8F");
        CHECK(MarkdownEmoji::lookup("warning") == "\xE2\x9A\xA0\xEF\xB8\x8F");
        CHECK(MarkdownEmoji::lookup("no_such_code").empty());
        CHECK(MarkdownEmoji::replace("Shipped :tada: and :rocket:!") ==
              "Shipped \xF0\x9F\x8E\x89 and \xF0\x9F\x9A\x80!");
        // Unknown codes, times, and words around colons stay as written.
        CHECK(MarkdownEmoji::replace(":not_an_emoji: x") == ":not_an_emoji: x");
        CHECK(MarkdownEmoji::replace("at 12:100:30") == "at 12:100:30");
        CHECK(MarkdownEmoji::replace("a:tada:b") == "a:tada:b");
        CHECK(MarkdownEmoji::replace("::tada::") == ":\xF0\x9F\x8E\x89:");
        CHECK(MarkdownEmoji::replace("(:+1:)") == "(\xF0\x9F\x91\x8D)");
        CHECK(MarkdownEmoji::replace("key: value") == "key: value");
        // In the page: text only, never code, math or an address.
        auto r = P::parse("Done :white_check_mark: `:tada:` and **:fire:**\n\n```\n:tada:\n```\n"
                          "\n$a:b:c$ <https://x.org/:tada:>\n");
        CHECK(mdText(r).find("Done \xE2\x9C\x85 ") != std::string::npos);
        CHECK(mdRun(r, ":tada:").code);
        CHECK(mdRun(r, ":tada:\n").codeBlock);
        CHECK(mdRun(r, "\xF0\x9F\x94\xA5").bold);
        CHECK(mdAny(r, [](const MdRun &x) { return x.link && x.text == "https://x.org/:tada:"; }));
        // In a heading and a table cell too; the table is measured after.
        auto t = P::parse("# Hi :wave:\n\n| a |\n|---|\n| :x: |\n");
        CHECK(mdRun(t, "Hi \xF0\x9F\x91\x8B").heading == 1);
        CHECK(mdRun(t, "\xE2\x9D\x8C").table);
    }

    GROUP("md:front-matter");
    {
        // A table, as GitHub shows it: the keys a header row, their values
        // the row under it.
        const std::string src = "---\ntitle: \"Hello\"\ntags:\n  - a\n  - b\n# a comment\n"
                                "summary: >\n  two\n  lines\ndate: 2026-11-19\n---\n# Body\n";
        auto r = P::parse(src);
        CHECK(!mdAny(r, [](const MdRun &x) { return x.rule; }));
        CHECK(!mdAny(r, [](const MdRun &x) { return x.codeBlock; }));
        auto cell = [&](int row, int col) {
            std::string s;
            for (const MdRun &x : r)
                if (x.frontMatter && x.tableRow == row && x.tableCol == col) s += x.text;
            return s;
        };
        CHECK(cell(0, 0) == "title" && cell(1, 0) == "Hello");
        CHECK(cell(0, 1) == "tags" && cell(1, 1) == "a, b");
        CHECK(cell(0, 2) == "summary" && cell(1, 2) == "two lines");
        CHECK(cell(0, 3) == "date" && cell(1, 3) == "2026-11-19");
        CHECK(mdRun(r, "title").bold && mdRun(r, "title").table && mdRun(r, "title").tableCols == 4);
        CHECK(mdRun(r, "Body").heading == 1 && !mdRun(r, "Body").frontMatter);
        CHECK(mdRun(r, "title").line == 1);
        // Values show as written: no emphasis, links or emoji made of them.
        auto lit = P::parse("---\nname: *not em* [x](y)\n---\n");
        bool plain = false;
        for (const MdRun &x : lit)
            if (x.tableRow == 1 && x.text == "*not em* [x](y)") plain = !x.italic && !x.link;
        CHECK(plain);
        // Its lines are still the code block a double-click edits.
        const auto lines = P::lines(src);
        CHECK(lines[1].kind == MdLine::Code && lines[0].kind == MdLine::Fence);
        CHECK(MarkdownEdit::blockAt(src, 1).kind == MarkdownEdit::Block::Code);
        // Not front matter: not at the top, or no key on its first line.
        CHECK(!mdAny(P::parse("x\n\n---\ntitle: y\n---\n"), [](const MdRun &x) { return x.frontMatter; }));
        CHECK(!mdAny(P::parse("---\nplain words\n---\n"), [](const MdRun &x) { return x.frontMatter; }));
    }

    GROUP("md:gfm-speed");
    {
        // Inputs that would make footnotes, HTML blocks or quote lists slow.
        std::string notes = "[^a]: x\n\n", tags = "<div>\n", quotes;
        for (int k = 0; k < 15000; k++) notes += "[^";
        for (int k = 0; k < 20000; k++) tags += "<b> ";
        for (int k = 0; k < 3000; k++) quotes += "> " + std::string((size_t)(k % 40), ' ') + "- x\n";
        double worst = 0;
        for (const std::string *s : {&notes, &tags, &quotes}) {
            const double t0 = cpuMs();
            P::parse(*s);
            worst = std::max(worst, cpuMs() - t0);
        }
        CHECK_TIME(worst, 1000);
    }
}
