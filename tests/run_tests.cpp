// run_tests.cpp — unit tests for the pure-C++ core (no framework, no deps).
// Build/run with `make test`. Exits non-zero if any check fails.
#include "SyntaxHighlighter.h"
#include "MarkdownParser.h"
#include <cstdio>
#include <string>
#include <vector>

// --------------------------------------------------------------- tiny harness
static int g_pass = 0, g_fail = 0;
static const char *g_group = "";

#define GROUP(name) g_group = name
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else {                                                                 \
            g_fail++;                                                          \
            std::printf("  FAIL [%s] %s:%d  %s\n", g_group, __FILE__,          \
                        __LINE__, #cond);                                      \
        }                                                                      \
    } while (0)

// --------------------------------------------------------- syntax highlighter
namespace {

std::string slice(const std::string &s, const Token &t) {
    return s.substr(t.start, t.length);
}

// Is there a token whose exact text is `word` with style `st`?
bool hasToken(const std::string &text, const std::string &ext,
              const std::string &word, TokenStyle st) {
    for (const Token &t : SyntaxHighlighter::highlight(text, ext))
        if (slice(text, t) == word && t.style == st) return true;
    return false;
}

// Is there any token with style `st`?
bool hasStyle(const std::string &text, const std::string &ext, TokenStyle st) {
    for (const Token &t : SyntaxHighlighter::highlight(text, ext))
        if (t.style == st) return true;
    return false;
}

void testSyntax() {
    GROUP("syntax:supports");
    CHECK(SyntaxHighlighter::supports("py"));
    CHECK(SyntaxHighlighter::supports("cpp"));
    CHECK(SyntaxHighlighter::supports("js"));
    CHECK(!SyntaxHighlighter::supports("xyz"));
    CHECK(!SyntaxHighlighter::supports(""));

    GROUP("syntax:python");
    std::string py = "def foo(x):\n    return 1  # note\n";
    CHECK(hasToken(py, "py", "def", TokenStyle::Keyword));
    CHECK(hasToken(py, "py", "return", TokenStyle::Keyword));
    CHECK(hasToken(py, "py", "1", TokenStyle::Number));
    CHECK(hasToken(py, "py", "# note", TokenStyle::Comment));
    CHECK(hasToken(py, "py", "foo", TokenStyle::Function));  // foo(
    CHECK(hasToken("s = \"hi\"", "py", "\"hi\"", TokenStyle::String));
    // A '#' comment must not be mistaken for anything else.
    CHECK(!hasStyle("# just a comment", "py", TokenStyle::Keyword));

    GROUP("syntax:cpp");
    std::string cpp = "int main() {\n  // c\n  return 0;\n}\n";
    CHECK(hasToken(cpp, "cpp", "int", TokenStyle::Type));
    CHECK(hasToken(cpp, "cpp", "return", TokenStyle::Keyword));
    CHECK(hasToken(cpp, "cpp", "0", TokenStyle::Number));
    CHECK(hasToken(cpp, "cpp", "// c", TokenStyle::Comment));
    CHECK(hasToken(cpp, "cpp", "main", TokenStyle::Function));
    CHECK(hasStyle("#include <cstdio>", "cpp", TokenStyle::Preprocessor));
    CHECK(hasToken("/* block */ x", "cpp", "/* block */", TokenStyle::Comment));

    GROUP("syntax:strings-escapes");
    // An escaped quote should not end the string early.
    CHECK(hasToken("\"a\\\"b\"", "cpp", "\"a\\\"b\"", TokenStyle::String));

    GROUP("syntax:plain");
    // Plain text / unknown extension yields no styled tokens.
    CHECK(SyntaxHighlighter::highlight("just words here", "txt").empty());
}

// --------------------------------------------------------------- markdown
bool anyRun(const std::vector<MdRun> &runs,
            bool (*pred)(const MdRun &)) {
    for (const MdRun &r : runs) if (pred(r)) return true;
    return false;
}

void testMarkdown() {
    GROUP("md:headings");
    auto h = MarkdownParser::parse("# Title\n");
    CHECK(anyRun(h, [](const MdRun &r) {
        return r.heading == 1 && r.text.find("Title") != std::string::npos;
    }));
    auto h2 = MarkdownParser::parse("## Sub\n");
    CHECK(anyRun(h2, [](const MdRun &r) { return r.heading == 2; }));
    // '#' without a following space is not a heading.
    auto notH = MarkdownParser::parse("#nospace\n");
    CHECK(!anyRun(notH, [](const MdRun &r) { return r.heading > 0; }));

    GROUP("md:inline");
    CHECK(anyRun(MarkdownParser::parse("**bold**"),
                 [](const MdRun &r) { return r.bold && r.text == "bold"; }));
    CHECK(anyRun(MarkdownParser::parse("*ital*"),
                 [](const MdRun &r) { return r.italic && r.text == "ital"; }));
    CHECK(anyRun(MarkdownParser::parse("`code`"),
                 [](const MdRun &r) { return r.code && r.text == "code"; }));
    CHECK(anyRun(MarkdownParser::parse("[t](http://x)"), [](const MdRun &r) {
        return r.link && r.text == "t" && r.url == "http://x";
    }));

    GROUP("md:blocks");
    CHECK(anyRun(MarkdownParser::parse("```\nx=1\n```\n"), [](const MdRun &r) {
        return r.codeBlock && r.text.find("x=1") != std::string::npos;
    }));
    CHECK(anyRun(MarkdownParser::parse("- item\n"),
                 [](const MdRun &r) { return r.listDepth > 0; }));
    CHECK(anyRun(MarkdownParser::parse("> quote\n"),
                 [](const MdRun &r) { return r.quote; }));
    CHECK(anyRun(MarkdownParser::parse("---\n"),
                 [](const MdRun &r) { return r.rule; }));

    GROUP("md:tables");
    auto tbl = MarkdownParser::parse("| A | B |\n| --- | --- |\n| 1 | 2 |\n");
    CHECK(anyRun(tbl, [](const MdRun &r) { return r.table; }));
    CHECK(anyRun(tbl, [](const MdRun &r) {  // header row is bold
        return r.table && r.bold && r.text.find("A") != std::string::npos;
    }));
    CHECK(anyRun(tbl, [](const MdRun &r) {
        return r.table && r.text.find("1") != std::string::npos;
    }));
    // A pipe line without a separator row underneath is NOT a table.
    auto notTbl = MarkdownParser::parse("a | b is just text\n");
    CHECK(!anyRun(notTbl, [](const MdRun &r) { return r.table; }));

    GROUP("md:block-separation");
    // A heading directly after a paragraph (no blank line) must still be a
    // heading, preceded by a newline rather than glued onto the paragraph.
    auto glued = MarkdownParser::parse("paragraph text\n# Heading\n");
    CHECK(anyRun(glued, [](const MdRun &r) {
        return r.heading == 1 && r.text.find("Heading") != std::string::npos;
    }));
    bool sawNewlineBeforeHeading = false;
    {
        bool prevEndsNewline = true;  // start of doc counts as a line start
        for (const MdRun &r : glued) {
            if (r.heading == 1) { sawNewlineBeforeHeading = prevEndsNewline; break; }
            prevEndsNewline = !r.text.empty() && r.text.back() == '\n';
        }
    }
    CHECK(sawNewlineBeforeHeading);
}

}  // namespace

int main() {
    std::printf("Running MiniCode core tests...\n");
    testSyntax();
    testMarkdown();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
