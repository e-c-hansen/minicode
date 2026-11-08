// lexer_styles_tests.inc — the lexer's finer styles, groups of run_tests.cpp:
// `syntax:function-names` (a name a definition keyword introduces),
// `syntax:type-names`, `syntax:self`, `syntax:decorators` and
// `syntax:escapes` (in which strings, of which languages). Included there.

namespace {

// The style at each unit, '-' where no token is (TokenStyle order).
std::string styleMap(const std::string &text, const std::string &ext) {
    std::string m(text.size(), '-');
    const char *letters = "pktscnPfeS";
    for (const Token &t : SyntaxHighlighter::highlight(text, ext))
        for (size_t k = t.start; k < t.start + t.length && k < m.size(); ++k)
            m[k] = letters[(int)t.style];
    return m;
}

}  // namespace

static void testLexerStyles() {
    GROUP("syntax:function-names");
    // A name a definition keyword introduces is a function even without a
    // '(' after it (Rust's generics, a Python def split over lines); calls
    // as before.
    CHECK(hasToken("def area(w, h):\n", "py", "area", TokenStyle::Function));
    CHECK(hasToken("def area \\\n  (w):\n", "py", "area", TokenStyle::Function));
    CHECK(hasToken("x = area(1, 2)\n", "py", "area", TokenStyle::Function));
    CHECK(hasToken("int area(int w) { return w; }\n", "cpp", "area", TokenStyle::Function));
    CHECK(hasToken("auto v = obj.size ();\n", "cpp", "size", TokenStyle::Function));
    CHECK(hasToken("function area(w) {}\n", "js", "area", TokenStyle::Function));
    CHECK(hasToken("const f = function named (w) {}\n", "js", "named", TokenStyle::Function));
    CHECK(hasToken("func area(w int) int {\n", "go", "area", TokenStyle::Function));
    CHECK(hasToken("func Area() int {\n", "go", "Area", TokenStyle::Function));
    CHECK(hasToken("fn area<T: Num>(w: T) -> T {\n", "rs", "area", TokenStyle::Function));
    CHECK(hasToken("fun area(w: Int) = w\n", "kt", "area", TokenStyle::Function));
    // A word that only ends in "def" is not def.
    CHECK(!hasToken("undef area\n", "py", "area", TokenStyle::Function));

    GROUP("syntax:type-names");
    CHECK(hasToken("class shape:\n", "py", "shape", TokenStyle::Type));
    CHECK(hasToken("struct point { int x; };\n", "cpp", "point", TokenStyle::Type));
    CHECK(hasToken("struct point *p;\n", "c", "point", TokenStyle::Type));
    CHECK(hasToken("enum class color { red };\n", "cpp", "class", TokenStyle::Keyword));
    CHECK(hasToken("enum class color { red };\n", "cpp", "color", TokenStyle::Type));
    CHECK(hasToken("interface shape { }\n", "ts", "shape", TokenStyle::Type));
    CHECK(hasToken("trait shape { }\n", "rs", "shape", TokenStyle::Type));
    // Not after a line break: the word before must be on the same line.
    CHECK(!hasToken("class\nshape = 1\n", "py", "shape", TokenStyle::Type));

    GROUP("syntax:self");
    CHECK(hasToken("def f(self):\n    return self.x\n", "py", "self", TokenStyle::Self));
    CHECK(hasToken("def f(cls):\n", "py", "cls", TokenStyle::Self));
    // obj.self is an attribute, not the instance.
    CHECK(styleMap("a.self", "py") == "--kkkk");
    CHECK(hasToken("this.x = super.y;\n", "js", "this", TokenStyle::Self));
    CHECK(hasToken("this.x = super.y;\n", "js", "super", TokenStyle::Self));
    CHECK(hasToken("return this->x;\n", "cpp", "this", TokenStyle::Self));
    CHECK(!hasToken("int self = 0;\n", "cpp", "self", TokenStyle::Self));
    CHECK(hasToken("let s = self.x;\n", "rs", "self", TokenStyle::Self));
    CHECK(hasToken("[self run];\n", "m", "self", TokenStyle::Self));
    CHECK(hasToken("base.Run();\n", "cs", "base", TokenStyle::Self));

    GROUP("syntax:decorators");
    // A decorator is its name in the function color; its arguments are code.
    CHECK(styleMap("@property\ndef f(): pass", "py") ==
          "fffffffff-" "kkk-f----kkkk");
    CHECK(styleMap("  @app.route(\"/x\", 2)\n", "py") ==
          "--ffffffffff-ssss--n--");
    CHECK(styleMap("@ name\n", "py") == "ffffff-");
    // A lone @ (matrix multiplication at a line's start) is not one.
    CHECK(styleMap("@ (a)\n", "py") == "------");
    // C's preprocessor lines are unchanged.
    CHECK(styleMap("#define F(x) (x)\n", "c") == "PPPPPPPPPPPPPPPP-");

    GROUP("syntax:escapes");
    CHECK(styleMap("s = \"a\\nb\"", "py") == "----sseess");
    CHECK(styleMap("s = 'a\\x41\\u00e9\\N{DASH}b'", "py") ==
          "----sseeeeeeeeeeeeeeeeeess");
    CHECK(styleMap("s = \"\\101\\0\\\\\"", "py") == "----seeeeeeees");
    // Raw strings have none, nor do their later lines.
    CHECK(styleMap("s = r\"a\\nb\"", "py") == "-----ssssss");
    CHECK(styleMap("s = r'''a\n\\n'''", "py") == "-----sssss" "sssss");
    CHECK(styleMap("s = '''a\n\\n'''", "py") == "----sssss" "eesss");
    // f-strings: in the text, not in the fields.
    CHECK(styleMap("f'\\t{x}'", "py") == "-seek-ks");
    CHECK(styleMap("rf'\\t{x}'", "py") == "--sssk-ks");
    // C++: escapes in strings and characters, none in raw strings.
    CHECK(hasToken("c = '\\n';", "cpp", "\\n", TokenStyle::Escape));
    CHECK(hasToken("s = \"\\x1b[0m\";", "cpp", "\\x1b", TokenStyle::Escape));
    CHECK(!hasStyle("auto r = R\"(a\\nb)\";", "cpp", TokenStyle::Escape));
    // Rust: plain strings and characters; raw strings none.
    CHECK(hasToken("let c = '\\u{1F600}';", "rs", "\\u{1F600}", TokenStyle::Escape));
    CHECK(!hasStyle("let r = r\"a\\nb\";", "rs", TokenStyle::Escape));
    // Go: none in a raw string.
    CHECK(hasToken("s := \"a\\tb\"", "go", "\\t", TokenStyle::Escape));
    CHECK(!hasStyle("s := `a\\tb`", "go", TokenStyle::Escape));
    // C#'s verbatim strings have none.
    CHECK(!hasStyle("var p = @\"C:\\new\";", "cs", TokenStyle::Escape));
    // JavaScript: strings and templates, not regexes.
    CHECK(hasToken("s = 'a\\'b';", "js", "\\'", TokenStyle::Escape));
    CHECK(hasToken("s = `a\\`b ${x}`;", "js", "\\`", TokenStyle::Escape));
    CHECK(!hasStyle("r = /a\\d+/;", "js", TokenStyle::Escape));
    // JSON's \u escapes.
    CHECK(hasToken("{\"k\": \"\\u00e9\"}", "json", "\\u00e9", TokenStyle::Escape));
    // A shell's single quotes have none; its double quotes do.
    CHECK(!hasStyle("echo 'a\\nb'", "sh", TokenStyle::Escape));
    CHECK(hasToken("echo \"a\\$b\"", "sh", "\\$", TokenStyle::Escape));
    // A backslash before a line break continues the string; before a
    // character beyond ASCII it is no escape (tokens end at ASCII).
    CHECK(styleMap("s = 'a\\\nb'", "py") == "----ssss" "ss");
    CHECK(!hasStyle("s = \"\\\xC3\xA9\"", "py", TokenStyle::Escape));
    // Escapes are strings to everything that asks.
    CHECK(isStringStyle(TokenStyle::Escape) && isStringStyle(TokenStyle::String) &&
          !isStringStyle(TokenStyle::Comment));
}
