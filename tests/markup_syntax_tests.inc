// markup_syntax_tests.inc — the HTML and XML grammar (lexMarkupLine in
// SyntaxHighlighter.cpp), groups of run_tests.cpp: `syntax:markup` (tags,
// attributes, values, comments, entities, the doctype, XML's declaration
// and CDATA), `syntax:markup-embedded` (<script> as JavaScript and <style>
// as CSS, and where their bodies end), `syntax:markup-states` (what a line
// ends inside, and openToken) and `syntax:markup-folding`. Included there,
// after lexer_styles_tests.inc (styleMap: T tag, a attribute, < bracket) and
// fold_tests.inc (foldsOf). The incremental highlighter's random edits run
// html, xml and vue too (incremental:random-edits).

static void testMarkupSyntax() {
    GROUP("syntax:markup");
    for (const char *e : {"html", "htm", "xhtml", "vue", "xml", "svg", "plist", "xsd", "xsl"})
        CHECK(SyntaxHighlighter::supports(e));
    // A tag's brackets, name, attributes (quoted, unquoted, bare) and the
    // entities in text; "&bad" ends in no ';' and is text.
    CHECK(styleMap("<p class=\"x\" id=y hidden>a &amp; b &#38; &#x26; &bad</p>", "html") ==
          "<T-aaaaa-sss-aa-s-aaaaaa<--eeeee---eeeee-eeeeee-----<<T<");
    // An entity in a value is an escape there too.
    CHECK(styleMap("<a title=\"x&lt;y\">", "html") == "<T-aaaaa-sseeeess<");
    // The doctype is a tag whose words are attributes; a comment runs over
    // lines; a self-closing tag's />.
    CHECK(styleMap("<!DOCTYPE html>\n<!-- a\nb --><br/>", "html") ==
          "<<TTTTTTT-aaaa<-cccccccccccc<TT<<");
    // A tag over lines, a value over lines.
    CHECK(styleMap("<div\n  class=\"a\n  b\"\n  id=c>", "html") ==
          "<TTT---aaaaa-sssssss---aa-s<");
    // Text that only looks like markup stays text.
    CHECK(styleMap("a < b && c > d <3 </ x>", "html") == std::string(23, '-'));
    // XML: the declaration, prefixed names, CDATA between its marks.
    CHECK(styleMap("<?xml version=\"1.0\"?>\n<a:b x='1'><![CDATA[ <c> ]]></a:b>", "xml") ==
          "<<TTT-aaaaaaa-sssss<<-<TTT-a-sss<<<<<<<<<<sssss<<<<<TTT<");
    // Vue's directives are attributes.
    CHECK(hasToken("<div :a=\"b\" @click=\"go()\" v-on:x.prevent>", "vue", "@click",
                   TokenStyle::Attribute));
    CHECK(hasToken("<div :a=\"b\" @click=\"go()\" v-on:x.prevent>", "vue", "v-on:x.prevent",
                   TokenStyle::Attribute));
    // Tokens start and end at ASCII characters: a name with an accent at
    // its end is colored up to it.
    CHECK(hasToken("<caf\xC3\xA9 x>", "xml", "caf", TokenStyle::Tag));

    GROUP("syntax:markup-embedded");
    // A <script> body is JavaScript up to </script>, which ends it even
    // inside a string, as browsers end it.
    CHECK(styleMap("<script>\nlet s = \"</p>\"; // c\n</script>x", "html") ==
          "<TTTTTT<-kkk-----ssssss--cccc-<<TTTTTT<-");
    CHECK(styleMap("<script>let a = '</script>'; b</script >", "html") ==
          "<TTTTTT<kkk-----s<<TTTTTT<----<<TTTTTT-<");
    CHECK(styleMap("<SCRIPT>x()</SCRIPT>", "html") == "<TTTTTT<f--<<TTTTTT<");
    // A <style> body is CSS, its comments over lines.
    CHECK(styleMap("<style>\na { b: c; } /* d\n*/ e { f: \"g\" }\n</style><p>", "html") ==
          "<TTTTT<-------------ccccccc--------sss---<<TTTTT<<T<");
    // A script of another type is text to its end tag; a module and JSON
    // are code; <style type="text/css"> is still CSS.
    CHECK(styleMap("<script type=\"text/template\"><p>x</p></script><i>", "html") ==
          "<TTTTTT-aaaa-sssssssssssssss<--------<<TTTTTT<<T<");
    CHECK(hasToken("<script type=module>let x = 1</script>", "html", "let", TokenStyle::Keyword));
    CHECK(hasToken("<script type=\"application/ld+json\">{\"a\": true}</script>", "html", "true",
                   TokenStyle::Keyword));
    CHECK(hasToken("<style type=\"text/css\">a{b:url(\"c\")}</style>", "html", "url",
                   TokenStyle::Function));
    // A template literal over lines inside a script.
    CHECK(styleMap("<script>x = `a\nb`;</script>", "html") ==
          "<TTTTTT<----sss" "ss-<<TTTTTT<");
    // XML has no such bodies: an SVG's <style> is text.
    CHECK(styleMap("<svg><style>a{}</style></svg>", "svg") == "<TTT<<TTTTT<---<<TTTTT<<<TTT<");
    // Vue: the template's tags, then the script block as code.
    CHECK(hasToken("<template><p/></template>\n<script lang=\"ts\">\nexport default {}\n"
                   "</script>\n", "vue", "export", TokenStyle::Keyword));

    GROUP("syntax:markup-states");
    {
        auto ends = [](const std::string &t, const std::string &ext) {
            IncrementalHighlighter<char> h(ext);
            h.reset(StringSource<char>(t), nullptr);
            std::vector<LexState> v;
            for (size_t k = 0; k < h.lineCount(); ++k) v.push_back(h.endState(k));
            return v;
        };
        std::vector<LexState> st = ends("<p>\n<div\nid=\"a\nb\"><!--\n-->\n<script>\n"
                                        "/* c\n*/ x\n</script>\n", "html");
        CHECK(st.size() == 10);
        CHECK(st[0].kind == LexState::Normal);
        CHECK(st[1].kind == LexState::Markup && openToken(st[1]).kind == LexState::Normal);
        CHECK(st[2].kind == LexState::Markup && openToken(st[2]).kind == LexState::StringCont);
        CHECK(st[3].kind == LexState::Markup && openToken(st[3]).kind == LexState::BlockComment);
        CHECK(st[4].kind == LexState::Normal);
        CHECK(st[5].kind == LexState::MarkupEmbed && openToken(st[5]).kind == LexState::Normal);
        CHECK(st[6].kind == LexState::MarkupEmbed &&
              openToken(st[6]).kind == LexState::BlockComment);
        CHECK(st[7] == st[5]);   // the body again, its comment closed
        CHECK(st[8].kind == LexState::Normal);
        // Other languages' states are their own.
        LexState c{LexState::BlockComment, 0};
        CHECK(openToken(c) == c);
        // The same body in a <style> is another state.
        CHECK(ends("<style>\n", "html")[0] != st[5]);
    }

    GROUP("syntax:markup-folding");
    {
        // Lines in a <script> body are code to folding, so a function in
        // one folds; a comment over lines folds as a comment.
        std::u16string t = u16(
            "<body>\n"                // 0
            "  <script>\n"            // 1
            "    function f() {\n"    // 2
            "      return 1;\n"       // 3
            "    }\n"                 // 4
            "  </script>\n"           // 5
            "  <!-- a\n"              // 6
            "  b -->\n"               // 7
            "</body>\n");             // 8
        const std::string f = foldsOf(t, "index.html");
        std::printf("    html folds: %s\n", f.c_str());
        CHECK(f.find("2-") != std::string::npos);
        CHECK(f.find("6-7") != std::string::npos);
    }
}
