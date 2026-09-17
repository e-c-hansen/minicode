// run_tests.cpp — unit tests for the pure-C++ core (no framework, no deps).
// Build/run with `make test`. Exits non-zero if any check fails.
#include "SyntaxHighlighter.h"
#include "MarkdownParser.h"
#include "TerminalStream.h"
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

// Join the table runs into display lines (the text a user actually sees).
std::vector<std::string> tableLines(const std::vector<MdRun> &runs) {
    std::vector<std::string> lines;
    std::string cur;
    for (const MdRun &r : runs) {
        if (!r.table) continue;
        for (char c : r.text) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else cur += c;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// Display columns of every column divider (│ in body rows, ┼ in the rule).
// Counts code points, with CJK ideographs as two columns.
std::vector<size_t> dividerColumns(const std::string &line) {
    std::vector<size_t> cols;
    size_t col = 0;
    for (size_t i = 0; i < line.size();) {
        unsigned char c = (unsigned char)line[i];
        int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        std::string ch = line.substr(i, len);
        if (ch == "\xE2\x94\x82" || ch == "\xE2\x94\xBC") cols.push_back(col);
        col += (len == 3 && c >= 0xE3) ? 2 : 1;   // U+3000.. are wide here
        i += len;
    }
    return cols;
}

// Every row puts its dividers in the same screen columns.
bool columnsAligned(const std::vector<std::string> &lines) {
    if (lines.empty()) return false;
    auto first = dividerColumns(lines[0]);
    for (const auto &l : lines)
        if (dividerColumns(l) != first) return false;
    return true;
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

    GROUP("md:tables-layout");
    // Rows come out as: header, rule, body rows.
    auto lay = tableLines(MarkdownParser::parse(
        "| Name | Qty |\n|------|-----|\n| apple | 3 |\n| fig | 12 |\n"));
    CHECK(lay.size() == 4);
    CHECK(lay.size() == 4 && lay[0] == "Name  │ Qty");
    CHECK(lay.size() == 4 && lay[1] == "──────┼────");
    CHECK(lay.size() == 4 && lay[2] == "apple │ 3  ");
    CHECK(lay.size() == 4 && lay[3] == "fig   │ 12 ");
    CHECK(columnsAligned(lay));

    GROUP("md:tables-unicode");
    // Widths are measured in characters, not UTF-8 bytes: "café" and "✓ ok"
    // are longer in bytes than on screen, which used to push columns right.
    auto uni = tableLines(MarkdownParser::parse(
        "| Word | Mark |\n|---|---|\n| café | ✓ ok |\n| tea | x |\n"));
    CHECK(uni.size() == 4);
    CHECK(columnsAligned(uni));
    CHECK(uni.size() == 4 && uni[2] == "café │ ✓ ok");
    CHECK(uni.size() == 4 && uni[3] == "tea  │ x   ");
    // Wide (CJK) characters take two columns.
    auto cjk = tableLines(MarkdownParser::parse(
        "| k | v |\n|---|---|\n| 日本 | 1 |\n| ab | 2 |\n"));
    CHECK(cjk.size() == 4 && cjk[3] == "ab   │ 2");
    CHECK(columnsAligned(cjk));

    GROUP("md:tables-inline");
    // Inline markup in cells is rendered, not shown raw, and the hidden markup
    // characters don't count toward the column width.
    auto inl = MarkdownParser::parse(
        "| Kind | Example |\n|---|---|\n| **strong** | `x = 1` |\n"
        "| link | [docs](http://d) |\n| plain | abc |\n");
    auto inlLines = tableLines(inl);
    CHECK(columnsAligned(inlLines));
    CHECK(inlLines.size() == 5 && inlLines[2] == "strong │ x = 1  ");
    CHECK(inlLines.size() == 5 && inlLines[3] == "link   │ docs   ");
    CHECK(anyRun(inl, [](const MdRun &r) {
        return r.table && r.bold && r.text == "strong";
    }));
    CHECK(anyRun(inl, [](const MdRun &r) {
        return r.table && r.code && r.text == "x = 1";
    }));
    CHECK(anyRun(inl, [](const MdRun &r) {
        return r.table && r.link && r.text == "docs" && r.url == "http://d";
    }));
    CHECK(!anyRun(inl, [](const MdRun &r) {
        return r.text.find("**") != std::string::npos ||
               r.text.find('`') != std::string::npos;
    }));

    GROUP("md:tables-align");
    auto al = tableLines(MarkdownParser::parse(
        "| L | C | R |\n|:--|:-:|--:|\n| a | b | c |\n| long | wide | 12345 |\n"));
    CHECK(al.size() == 4 && al[0] == "L    │  C   │     R");
    CHECK(al.size() == 4 && al[2] == "a    │  b   │     c");
    CHECK(columnsAligned(al));

    GROUP("md:tables-shape");
    // No leading/trailing pipes is still a table.
    CHECK(tableLines(MarkdownParser::parse("A | B\n--- | ---\n1 | 2\n")).size() == 3);
    // Short rows are padded out to the header's column count, long rows are cut.
    auto ragged = tableLines(MarkdownParser::parse(
        "| a | b | c |\n|---|---|---|\n| 1 |\n| 1 | 2 | 3 | 4 |\n"));
    CHECK(ragged.size() == 4 && ragged[2] == "1 │   │  ");
    CHECK(ragged.size() == 4 && ragged[3] == "1 │ 2 │ 3");
    // An escaped pipe is cell content, not a column break.
    auto esc = tableLines(MarkdownParser::parse(
        "| op | means |\n|---|---|\n| a \\| b | or |\n"));
    CHECK(esc.size() == 3 && esc[2] == "a | b │ or   ");
    // Header and separator column counts must match.
    CHECK(!anyRun(MarkdownParser::parse("| a | b |\n| --- |\n"),
                  [](const MdRun &r) { return r.table; }));
    // A separator cell must be dashes (optionally colon-wrapped).
    CHECK(!anyRun(MarkdownParser::parse("| a | b |\n| -x- | --- |\n"),
                  [](const MdRun &r) { return r.table; }));
    // The table ends at a blank line or a line without pipes, and the text
    // around it stays ordinary paragraph text on its own lines.
    auto around = MarkdownParser::parse(
        "Before\n| a | b |\n|---|---|\n| 1 | 2 |\nAfter\n");
    CHECK(tableLines(around).size() == 3);
    CHECK(anyRun(around, [](const MdRun &r) {
        return !r.table && r.text == "After";
    }));
    CHECK(!anyRun(around, [](const MdRun &r) {
        return r.table && r.text.find("fter") != std::string::npos;
    }));

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

// ---------------------------------------------------------- terminal stream
namespace {

// All Text events joined, as the view would show them (ignoring \r/\b).
std::string streamText(const std::vector<TermEvent> &events) {
    std::string s;
    for (const TermEvent &e : events)
        if (e.kind == TermEvent::Text) s += e.text;
    return s;
}

// Feed `input` one byte at a time, the worst case for split sequences.
std::vector<TermEvent> feedBytewise(const std::string &input) {
    TerminalStream ts;
    std::vector<TermEvent> all;
    for (char c : input) {
        auto part = ts.feed(&c, 1);
        all.insert(all.end(), part.begin(), part.end());
    }
    return all;
}

// A compact trace of event kinds: T=text R=\r B=\b A C D P(ath).
std::string kinds(const std::vector<TermEvent> &events) {
    std::string s;
    for (const TermEvent &e : events) {
        switch (e.kind) {
            case TermEvent::Text:           s += 'T'; break;
            case TermEvent::CarriageReturn: s += 'R'; break;
            case TermEvent::Backspace:      s += 'B'; break;
            case TermEvent::PromptStart:    s += 'A'; break;
            case TermEvent::CommandStart:   s += 'C'; break;
            case TermEvent::CommandEnd:     s += 'D'; break;
            case TermEvent::Directory:      s += 'P'; break;
        }
    }
    return s;
}

const TermEvent *firstOf(const std::vector<TermEvent> &events,
                         TermEvent::Kind kind) {
    for (const TermEvent &e : events) if (e.kind == kind) return &e;
    return nullptr;
}

void testTerminalStream() {
    GROUP("term:text");
    {
        TerminalStream ts;
        auto ev = ts.feed("hello\r\nworld\r\n");
        CHECK(streamText(ev) == "hello\nworld\n");   // \r\n becomes \n
        CHECK(kinds(ev) == "T");
    }

    GROUP("term:escapes-dropped");
    {
        TerminalStream ts;
        // SGR color, erase line, cursor move, private mode, charset select.
        auto ev = ts.feed("\x1b[1;31mred\x1b[0m \x1b[K\x1b[10;5H\x1b[?25lok\x1b(B!");
        CHECK(streamText(ev) == "red ok!");
        // An OSC we don't use (window title) and a DCS string vanish too.
        auto ev2 = ts.feed("\x1b]0;my title\x07" "a\x1bPq#0;2\x1b\\b");
        CHECK(streamText(ev2) == "ab");
        CHECK(kinds(ev2) == "T");
    }

    GROUP("term:split-across-reads");
    {
        // Byte-at-a-time delivery must give the same result as one read.
        std::string input = "\x1b[32mgreen\x1b[0m caf\xC3\xA9 \xE2\x9C\x93\r\n"
                            "\x1b]133;D;7\x07\x1b]7;file://h/tmp\x1b\\";
        TerminalStream whole;
        auto a = whole.feed(input);
        auto b = feedBytewise(input);
        CHECK(streamText(a) == "green caf\xC3\xA9 \xE2\x9C\x93\n");
        CHECK(streamText(b) == streamText(a));
        const TermEvent *end = firstOf(b, TermEvent::CommandEnd);
        CHECK(end && end->status == 7);
        const TermEvent *dir = firstOf(b, TermEvent::Directory);
        CHECK(dir && dir->text == "/tmp");
        // Half a UTF-8 character is held back, not emitted as garbage.
        TerminalStream ts;
        auto first = ts.feed("x\xE2\x9C");
        CHECK(streamText(first) == "x");
        CHECK(streamText(ts.feed("\x93")) == "\xE2\x9C\x93");
    }

    GROUP("term:invalid-utf8");
    {
        TerminalStream ts;
        auto ev = ts.feed("a\xFF" "b\xC0\xAF" "c\r\n");
        CHECK(streamText(ev) == "a\xEF\xBF\xBD" "b\xEF\xBF\xBD\xEF\xBF\xBD" "c\n");
    }

    GROUP("term:shell-integration");
    {
        TerminalStream ts;
        auto ev = ts.feed("\x1b]133;A\x07");
        CHECK(kinds(ev) == "A");
        ev = ts.feed("\x1b]133;C\x07out\r\n\x1b]133;D;130\x07"
                     "\x1b]7;file://Host.local/Users/me/my%20dir%25\x07\x1b]133;A\x07");
        CHECK(kinds(ev) == "CTDPA");                 // order is preserved
        CHECK(streamText(ev) == "out\n");
        const TermEvent *end = firstOf(ev, TermEvent::CommandEnd);
        CHECK(end && end->status == 130);
        const TermEvent *dir = firstOf(ev, TermEvent::Directory);
        CHECK(dir && dir->text == "/Users/me/my dir%");
        // D without a status, and ST (ESC \) instead of BEL as terminator.
        auto noStatus = ts.feed("\x1b]133;D\x1b\\");
        CHECK(kinds(noStatus) == "D" && noStatus[0].status == 0);
        // 133;B (end of prompt) is recognized and ignored.
        CHECK(ts.feed("\x1b]133;B\x07").empty());
        // A malformed OSC 7 doesn't produce a directory.
        CHECK(!firstOf(ts.feed("\x1b]7;file://nohostpath\x07"),
                       TermEvent::Directory));
    }

    GROUP("term:carriage-return");
    {
        TerminalStream ts;
        auto ev = ts.feed("10%\r20%\r\n");
        CHECK(kinds(ev) == "TRT");
        CHECK(ev.size() == 3 && ev[0].text == "10%" && ev[2].text == "20%\n");
        // \r split from its \n across reads is still just a line ending.
        TerminalStream ts2;
        auto a = ts2.feed("line\r");
        auto b = ts2.feed("\nnext");
        CHECK(kinds(a) == "T" && kinds(b) == "T");
        CHECK(streamText(a) + streamText(b) == "line\nnext");
        // \r followed by an escape sequence still counts as a return.
        TerminalStream ts3;
        CHECK(kinds(ts3.feed("50%\r\x1b[Kdone")) == "TRT");
        // Backspace is reported so a spinner can erase its last character.
        TerminalStream ts4;
        CHECK(kinds(ts4.feed("|\b/")) == "TBT");
    }

    GROUP("term:robustness");
    {
        // An OSC that never terminates is abandoned, and output resumes after
        // its eventual terminator instead of being swallowed forever.
        TerminalStream ts;
        std::string runaway = "\x1b]133;" + std::string(20000, 'x');
        auto ev = ts.feed(runaway);
        CHECK(streamText(ev).empty());
        CHECK(streamText(ts.feed("\x07visible")) == "visible");
        // ESC inside an OSC starts a new sequence rather than ending the text.
        TerminalStream ts2;
        CHECK(kinds(ts2.feed("\x1b]0;title\x1b]133;A\x07")) == "A");
        // Other control characters are dropped; tabs are kept.
        TerminalStream ts3;
        CHECK(streamText(ts3.feed("a\x07\x01\tb")) == "a\tb");
    }
}

}  // namespace

int main() {
    std::printf("Running MiniCode core tests...\n");
    testSyntax();
    testMarkdown();
    testTerminalStream();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
