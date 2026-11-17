// spell_check_tests.inc — the `spell:*` groups of run_tests.cpp: which
// words of a file the spell checker is given (SpellCheck.h): prose in
// Markdown and LaTeX, comments, docstrings and sentence-like strings in
// code, never identifiers, keywords, paths, or addresses. Included there.
#include "SpellCheck.h"

namespace {

// The words to check in `text` (ASCII, so bytes are UTF-16 units) from
// `from` on, joined by "|".
std::string spWords(Spell::Kind kind, const std::string& text, const std::string& ext = "",
                    size_t from = 0) {
    const std::u16string t(text.begin(), text.end());
    std::vector<Token> tokens;
    if (!ext.empty()) tokens = SyntaxHighlighter::highlight(text, ext);
    std::string out;
    for (const Spell::Span& w : Spell::checkable(kind, t, from, tokens)) {
        if (!out.empty()) out += "|";
        out += text.substr(w.first, w.second - w.first);
    }
    return out;
}

}  // namespace

static void testSpellCheck() {
    using Spell::Kind;
    using Spell::Mode;

    GROUP("spell:kinds");
    CHECK(Spell::kindForExtension("md", false) == Kind::Markdown);
    CHECK(Spell::kindForExtension("markdown", false) == Kind::Markdown);
    CHECK(Spell::kindForExtension("tex", true) == Kind::Latex);
    CHECK(Spell::kindForExtension("py", true) == Kind::Code);
    CHECK(Spell::kindForExtension("txt", false) == Kind::Plain);
    CHECK(Spell::kindForExtension("csv", false) == Kind::None);
    // prose-only (the default) checks Markdown, LaTeX and commit messages.
    CHECK(Spell::checks(Mode::ProseOnly, Kind::Markdown));
    CHECK(Spell::checks(Mode::ProseOnly, Kind::Latex));
    CHECK(Spell::checks(Mode::ProseOnly, Kind::Commit));
    CHECK(!Spell::checks(Mode::ProseOnly, Kind::Code));
    CHECK(!Spell::checks(Mode::ProseOnly, Kind::Plain));
    CHECK(Spell::checks(Mode::On, Kind::Code));
    CHECK(Spell::checks(Mode::On, Kind::Plain));
    CHECK(!Spell::checks(Mode::On, Kind::None));
    CHECK(!Spell::checks(Mode::Off, Kind::Markdown));

    GROUP("spell:words");
    // Letters only, trimmed of punctuation; identifiers, paths, addresses,
    // numbers and acronyms left out.
    CHECK(spWords(Kind::Plain, "Hello, world! (it's) \"quoted\" well-known.") ==
          "Hello|world|it's|quoted|well-known");
    CHECK(spWords(Kind::Plain, "see src/main.cpp and https://example.com or a@b.org") ==
          "see|and|or");
    CHECK(spWords(Kind::Plain, "call fooBar and NSString with HTTP on macOS") ==
          "call|and|with|on");
    CHECK(spWords(Kind::Plain, "snake_case x2 v1.5 README.md e.g. 3rd").empty());
    CHECK(spWords(Kind::Plain, "**bold** _em_ ~gone~") == "bold|em|gone");
    {
        // Dashes written without spaces part two words.
        const std::u16string dash = u"this\u2014that and\u2013so";
        std::vector<Spell::Span> w = Spell::words(dash, {{0, dash.size()}});
        CHECK(w.size() == 4);
    }
    CHECK(spWords(Kind::Plain, "a I").empty());   // single letters are never wrong
    CHECK(spWords(Kind::Plain, "--flag foo() `name` #tag").empty());

    GROUP("spell:markdown");
    const std::string md =
        "# A heading wrod\n"
        "\n"
        "Some prose with `codde` and $mathh$ and [a linkk](http://x.org/pagge).\n"
        "\n"
        "```python\n"
        "def recieve(): pass\n"
        "```\n"
        "\n"
        "    indented codde\n"
        "\n"
        "- item tekst <span class=\"clas\">inner</span>\n"
        "\n"
        "> quoted lien\n"
        "\n"
        "$$\n"
        "x = dispaly\n"
        "$$\n"
        "\n"
        "| cel | other |\n"
        "|-----|-------|\n"
        "\n"
        "[ref]: http://example.com/refff\n"
        "<!-- hiden comment -->\n";
    const std::string mdWords = spWords(Kind::Markdown, md);
    CHECK(mdWords ==
          "heading|wrod|Some|prose|with|and|and|linkk|item|tekst|inner|quoted|lien|cel|other");
    // From a line on: a fence opened above still holds what follows it.
    const size_t fenceBody = md.find("def recieve");
    CHECK(spWords(Kind::Markdown, md, "", fenceBody).find("recieve") == std::string::npos);
    CHECK(spWords(Kind::Markdown, md, "", md.find("- item")).rfind("item", 0) == 0);

    GROUP("spell:latex");
    const std::string tex =
        "\\documentclass[11pt]{articel}\n"
        "\\usepackage[utf8]{inputenc}\n"
        "\\begin{document}\n"
        "\\section{Introducion}\n"
        "Text with \\emph{emphasys} and $x_{varr}$ math, see \\ref{sec:intro} and \\cite{knuth84}.\n"
        "\\includegraphics[width=0.5\\textwidth]{figurre}\n"
        "\\begin{tabular}{lcr} cell \\end{tabular}\n"
        "% a coment here\n"
        "\\begin{verbatim}\n"
        "raw verbatum\n"
        "\\end{verbatim}\n"
        "\\href{http://x.org}{linkk text} \\url{http://y.org/pathh}\n"
        "\\end{document}\n";
    CHECK(spWords(Kind::Latex, tex, "tex") ==
          "Introducion|Text|with|emphasys|and|math|see|and|cell|coment|here|linkk|text");

    GROUP("spell:code");
    const std::string py =
        "# A coment about recieve_data\n"
        "def recieve_data(sockett):\n"
        "    \"\"\"Return the recieved bytes.\n"
        "\n"
        "    More detials here.\n"
        "    \"\"\"\n"
        "    msg = \"Could not recieve the file\"\n"
        "    key = \"utf-8\"\n"
        "    fmt = \"%s wrod %d\"\n"
        "    url = \"http://exmaple.com/a b\"\n"
        "    return sockett.recv(1024)  # trailing noet\n";
    CHECK(spWords(Kind::Code, py, "py") ==
          "coment|about|Return|the|recieved|bytes|More|detials|here|Could|not|recieve|the|"
          "file|trailing|noet");
    const std::string cpp =
        "// Line coment\n"
        "/* Block\n"
        " * comentary */\n"
        "int recieve = 0; const char *s = \"two wrds\";\n"
        "const char *k = \"single\";\n";
    CHECK(spWords(Kind::Code, cpp, "cpp") == "Line|coment|Block|comentary|two|wrds");
    // A file with no grammar: nothing (no tokens to tell code from text).
    CHECK(spWords(Kind::None, "anything heer").empty());
}
