// run_tests.cpp — unit tests for the pure-C++ core (no framework, no deps).
// Build/run with `make test`. Exits non-zero if any check fails.
#include "SyntaxHighlighter.h"
#include "MarkdownParser.h"
#include "TerminalStream.h"
#include "TerminalScreen.h"
#include "Settings.h"
#include "LineComments.h"
#include "LatexDoc.h"
#include "SyncTex.h"
#include <algorithm>
#include <cmath>
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

// Plays events back the way the panel does: ended lines are committed, the
// live line is replaced by each new Line event.
struct Panel {
    std::vector<std::vector<TermRun>> lines;   // ended lines
    std::vector<TermRun> live;
    std::string events;                        // trace: L l A C D P
    int lastStatus = -1;
    std::string lastDir;

    void apply(const std::vector<TermEvent> &evs) {
        for (const TermEvent &e : evs) {
            switch (e.kind) {
            case TermEvent::Line:
                events += e.ended ? 'L' : 'l';
                if (e.ended) { lines.push_back(e.runs); live.clear(); }
                else live = e.runs;
                break;
            case TermEvent::PromptStart:  events += 'A'; break;
            case TermEvent::CommandStart: events += 'C'; break;
            case TermEvent::CommandEnd:
                events += 'D'; lastStatus = e.status; break;
            case TermEvent::Directory:
                events += 'P'; lastDir = e.text; break;
            }
        }
    }
    static std::string plain(const std::vector<TermRun> &runs) {
        std::string s;
        for (const TermRun &r : runs) s += r.text;
        return s;
    }
    // Everything on screen as plain text, lines joined with \n.
    std::string text() const {
        std::string s;
        for (const auto &l : lines) s += plain(l) + "\n";
        return s + plain(live);
    }
};

Panel run(const std::string &input) {
    TerminalStream ts;
    Panel p;
    p.apply(ts.feed(input));
    return p;
}

// Feed `input` one byte at a time, the worst case for split sequences.
Panel runBytewise(const std::string &input) {
    TerminalStream ts;
    Panel p;
    for (char c : input) p.apply(ts.feed(&c, 1));
    return p;
}

// The style of the run containing `needle` anywhere in the panel.
const TermStyle *styleOf(const Panel &p, const std::string &needle) {
    auto search = [&](const std::vector<TermRun> &runs) -> const TermStyle * {
        for (const TermRun &r : runs)
            if (r.text.find(needle) != std::string::npos) return &r.style;
        return nullptr;
    };
    for (const auto &l : p.lines)
        if (const TermStyle *s = search(l)) return s;
    return search(p.live);
}

bool isIndexed(const TermColor &c, int index) {
    return c.kind == TermColor::Indexed && c.index == index;
}

void testTerminalStream() {
    GROUP("term:text");
    {
        Panel p = run("hello\r\nworld\r\n");
        CHECK(p.text() == "hello\nworld\n");    // \r\n ends a line
        CHECK(p.events == "LL");
        Panel partial = run("prompt> ");       // no newline: a live line
        CHECK(partial.events == "l" && partial.text() == "prompt> ");
        CHECK(run("a\r\n\r\nb").text() == "a\n\nb");   // empty lines survive
    }

    GROUP("term:escapes-dropped");
    {
        // Cursor visibility, other-line movement, charset select: no text.
        CHECK(run("\x1b[?25lok\x1b[2A\x1b[10;5H\x1b(B!\x1b[?25h").text() == "ok!");
        // An OSC we don't use (window title) and a DCS string vanish too.
        CHECK(run("\x1b]0;my title\x07" "a\x1bPq#0;2\x1b\\b").text() == "ab");
    }

    GROUP("term:sgr-basic");
    {
        Panel p = run("\x1b[31mred\x1b[0m plain \x1b[1;4;32mbold\x1b[22m under\x1b[m\n");
        CHECK(p.text() == "red plain bold under\n");
        const TermStyle *red = styleOf(p, "red");
        CHECK(red && isIndexed(red->fg, 1) && !red->bold);
        const TermStyle *plain = styleOf(p, "plain");
        CHECK(plain && *plain == TermStyle());
        const TermStyle *bold = styleOf(p, "bold");
        CHECK(bold && bold->bold && bold->underline && isIndexed(bold->fg, 2));
        const TermStyle *under = styleOf(p, "under");   // 22 ends bold only
        CHECK(under && !under->bold && under->underline && isIndexed(under->fg, 2));
        CHECK(p.lines.size() == 1 && p.lines[0].size() == 4);  // one run per style
    }

    GROUP("term:sgr-colors");
    {
        // Bright, 256-color (both separators), truecolor, backgrounds, resets.
        Panel p = run("\x1b[91ma\x1b[38;5;213mb\x1b[38:5:81mc"
                      "\x1b[38;2;10;20;30md\x1b[38:2::1:2:3me"
                      "\x1b[44;103mf\x1b[39;49mg\x1b[2;3;7;9mh\x1b[23;27;29mi");
        CHECK(p.text() == "abcdefghi");
        CHECK(isIndexed(styleOf(p, "a")->fg, 9));
        CHECK(isIndexed(styleOf(p, "b")->fg, 213));
        CHECK(isIndexed(styleOf(p, "c")->fg, 81));
        const TermColor &d = styleOf(p, "d")->fg;
        CHECK(d.kind == TermColor::RGB && d.r == 10 && d.g == 20 && d.b == 30);
        const TermColor &e = styleOf(p, "e")->fg;
        CHECK(e.kind == TermColor::RGB && e.r == 1 && e.g == 2 && e.b == 3);
        const TermStyle *f = styleOf(p, "f");
        CHECK(isIndexed(f->bg, 11) && e.kind == TermColor::RGB);
        const TermStyle *g = styleOf(p, "g");
        CHECK(g->fg.kind == TermColor::Default && g->bg.kind == TermColor::Default);
        const TermStyle *h = styleOf(p, "h");
        CHECK(h->dim && h->italic && h->inverse && h->strike);
        const TermStyle *i = styleOf(p, "i");
        CHECK(i->dim && !i->italic && !i->inverse && !i->strike);
        // Style carries across lines, as in a real terminal.
        Panel carry = run("\x1b[35mone\ntwo\x1b[0m");
        CHECK(isIndexed(styleOf(carry, "two")->fg, 5));
        // A malformed extended color is ignored rather than misread.
        Panel bad = run("\x1b[38;5mx");
        CHECK(styleOf(bad, "x")->fg.kind == TermColor::Default);
    }

    GROUP("term:palette");
    {
        TermColor c; c.kind = TermColor::Indexed;
        c.index = 1;   CHECK(c.rgb() == 0xCD3131);
        c.index = 16;  CHECK(c.rgb() == 0x000000);   // cube corner
        c.index = 196; CHECK(c.rgb() == 0xFF0000);   // cube pure red
        c.index = 213; CHECK(c.rgb() == 0xFF87FF);
        c.index = 232; CHECK(c.rgb() == 0x080808);   // gray ramp ends
        c.index = 255; CHECK(c.rgb() == 0xEEEEEE);
        TermColor rgb; rgb.kind = TermColor::RGB; rgb.r = 1; rgb.g = 2; rgb.b = 3;
        CHECK(rgb.rgb() == 0x010203);
    }

    GROUP("term:line-editing");
    {
        // \r then text overwrites in place; leftovers stay unless erased.
        CHECK(run("10%\r20%\n").text() == "20%\n");
        CHECK(run("abcdef\rXY\n").text() == "XYcdef\n");
        CHECK(run("downloading 99%\r\x1b[Kdone\n").text() == "done\n");
        // Erase to start of line, whole line, and N characters.
        CHECK(run("abcdef\x1b[3D\x1b[1K\n").text() == "    ef\n");
        CHECK(run("abcdef\x1b[2Kxy\n").text() == "      xy\n");
        CHECK(run("abcdef\r\x1b[2C\x1b[2X\n").text() == "ab  ef\n");
        // Backspace moves left; the next character overwrites.
        CHECK(run("ab\bc\n").text() == "ac\n");
        CHECK(run("|\b/\b-\n").text() == "-\n");
        // Cursor to column (1-based), forward past the end pads with spaces.
        CHECK(run("hello\x1b[1GJ\n").text() == "Jello\n");
        CHECK(run("a\x1b[3Cb\n").text() == "a   b\n");
        // Tabs move to the next multiple of 8, filling only past the end.
        CHECK(run("ab\tc\n").text() == "ab      c\n");
        CHECK(run("abcdefghij\r\tX\n").text() == "abcdefghXj\n");
        // A spinner redraw keeps its color on the rewritten character.
        Panel spin = run("\x1b[36m|\x1b[0m\r\x1b[36m/\x1b[0m");
        CHECK(spin.text() == "/" && isIndexed(styleOf(spin, "/")->fg, 6));
        // Combining marks join the previous character's cell.
        CHECK(run("e\xCC\x81x\b!\n").text() == "e\xCC\x81!\n");
    }

    GROUP("term:live-line-updates");
    {
        // Across reads, the live line is re-sent whole and replaced.
        TerminalStream ts;
        Panel p;
        p.apply(ts.feed("50%"));
        p.apply(ts.feed("\r75%"));
        CHECK(p.events == "ll" && p.text() == "75%");
        p.apply(ts.feed("\r\n"));
        CHECK(p.events == "llL" && p.text() == "75%\n");
        // Moving the cursor alone changes nothing on screen: no event.
        CHECK(ts.feed("\r").empty());
        // The host breaking the line starts a fresh one.
        p.apply(ts.feed("typed"));
        ts.breakLine();
        Panel q;
        q.apply(ts.feed("next"));
        CHECK(q.text() == "next");
        // A huge unbroken line is ended automatically.
        Panel big = run(std::string(TerminalStream::kMaxLineCells + 10, 'x'));
        CHECK(big.lines.size() == 1 &&
              Panel::plain(big.lines[0]).size() == TerminalStream::kMaxLineCells);
        CHECK(Panel::plain(big.live).size() == 10);
    }

    GROUP("term:split-across-reads");
    {
        std::string input = "\x1b[38;5;213mpink\x1b[0m caf\xC3\xA9 \xE2\x9C\x93\r\n"
                            "50%\r\x1b[Kdone\r\n"
                            "\x1b]133;D;7\x07\x1b]7;file://h/tmp\x1b\\";
        Panel whole = run(input);
        Panel bytes = runBytewise(input);
        CHECK(whole.text() == "pink caf\xC3\xA9 \xE2\x9C\x93\ndone\n");
        CHECK(bytes.text() == whole.text());
        CHECK(isIndexed(styleOf(bytes, "pink")->fg, 213));
        CHECK(bytes.lastStatus == 7 && bytes.lastDir == "/tmp");
        // Half a UTF-8 character is held back, not shown as garbage.
        TerminalStream ts;
        Panel p;
        p.apply(ts.feed("x\xE2\x9C"));
        CHECK(p.text() == "x");
        p.apply(ts.feed("\x93"));
        CHECK(p.text() == "x\xE2\x9C\x93");
    }

    GROUP("term:invalid-utf8");
    {
        CHECK(run("a\xFF" "b\xC0\xAF" "c\n").text() ==
              "a\xEF\xBF\xBD" "b\xEF\xBF\xBD\xEF\xBF\xBD" "c\n");
        // A sequence cut short by ESC or ASCII becomes one replacement.
        CHECK(run("\xE2\x9C" "a\xE2\x1b[31mb\n").text() ==
              "\xEF\xBF\xBD" "a\xEF\xBF\xBD" "b\n");
        CHECK(run("\xED\xA0\x80!\n").text() == "\xEF\xBF\xBD!\n");  // surrogate
    }

    GROUP("term:shell-integration");
    {
        TerminalStream ts;
        Panel p;
        p.apply(ts.feed("\x1b]133;A\x07"));
        CHECK(p.events == "A");
        p.apply(ts.feed("\x1b]133;C\x07out\r\npartial\x1b]133;D;130\x07"
                        "\x1b]7;file://Host.local/Users/me/my%20dir%25\x07\x1b]133;A\x07"));
        CHECK(p.events == "ACLlDPA");    // the live line is sent before D
        CHECK(p.text() == "out\npartial");
        CHECK(p.lastStatus == 130);
        CHECK(p.lastDir == "/Users/me/my dir%");
        // D without a status, and ST (ESC \) instead of BEL as terminator.
        auto noStatus = ts.feed("\x1b]133;D\x1b\\");
        CHECK(noStatus.size() == 1 && noStatus[0].kind == TermEvent::CommandEnd &&
              noStatus[0].status == 0);
        CHECK(ts.feed("\x1b]133;B\x07").empty());   // recognized, ignored
        CHECK(ts.feed("\x1b]7;file://nohostpath\x07").empty());
    }

    GROUP("term:robustness");
    {
        // An OSC that never terminates is abandoned; output resumes after its
        // eventual terminator instead of being swallowed forever.
        TerminalStream ts;
        Panel p;
        p.apply(ts.feed("\x1b]133;" + std::string(20000, 'x')));
        CHECK(p.text().empty());
        p.apply(ts.feed("\x07visible"));
        CHECK(p.text() == "visible");
        // ESC inside an OSC starts a new sequence.
        CHECK(run("\x1b]0;title\x1b]133;A\x07").events == "A");
        // An overlong CSI is ignored whole, not half-applied.
        std::string longCsi = "\x1b[" + std::string(200, '1') + "mz";
        Panel lc = run(longCsi);
        CHECK(lc.text() == "z" && *styleOf(lc, "z") == TermStyle());
        // Other control characters are dropped.
        CHECK(run("a\x07\x01" "b").text() == "ab");
    }
}

}  // namespace

// ------------------------------------------------------- terminal screen
namespace {

// Real output, recorded with `script -q` from programs on a 40x10 terminal
// with TERM=xterm-256color.
//
// vim -u NONE -N -i NONE f.txt (f.txt = "line one\nline two\n"), typed:
// "iHello <Esc>:wq<Enter>".
const std::string kVimEdit =
    "\033[\?1049h\033[>4;2m\033[\?1h\033=\033[\?2004h\033[\?1004h\033["
    "1;10r\033[\?12h\033[\?12l\033[22;2t\033[22;1t\033[27m\033[23m\033"
    "[29m\033[m\033[H\033[2J\033[\?25l\033[10;1H\"f.txt\" 2L, 18B\033["
    "2;1H\342\226\275\033[6n\033[2;1H  \033[3;1H\033Pzz\033\\\033[0%m\033"
    "[6n\033[3;1H           \033[1;1H\033[>c\033[1;1Hline one\r\n"
    "line two\033[2;9H\033[K\033[3;1H\033[94m~                        "
    "               \033[4;1H~                                       \033"
    "[5;1H~                                       \033[6;1H~          "
    "                             \033[7;1H~                          "
    "             \033[8;1H~                                       \033"
    "[9;1H~                                       \033[1;1H\033[\?25h\033"
    "[\?4m\033[\?25l\033[m\033[10;1H\033[1m-- INSERT --\033[m\033[10;1"
    "3H\033[K\033[10;1H\033[K\033[1;6H\rHello line one\033[10;1H\033[1"
    "m-- INSERT --\033[1;7H\033[m\033[10;30H^[\033[1;6H\033[\?25h\033["
    "\?25l\033[10;30H  \033[1;7H\033[10;1H\033[K\033[1;6H\033[\?25h\033"
    "[\?25l\033[10;1H:wq\r\033[\?2004l\033[>4;m\"f.txt\" 2L, 24B writt"
    "en\r\033[23;2t\033[23;1t\r\r\n"
    "\033[\?1004l\033[\?2004l\033[\?1l\033>\033[\?1049l\033[\?25h\033["
    ">4;m";

// vim on a 100-line file ("row 1".."row 100"), typed: Ctrl+E three times,
// "dd", "Onew<Esc>", ":q!<Enter>". Scrolls with a scroll region and redraws
// lines by overwriting only the characters that differ.
const std::string kVimScroll =
    "\033[\?1049h\033[>4;2m\033[\?1h\033=\033[\?2004h\033[\?1004h\033["
    "1;10r\033[\?12h\033[\?12l\033[22;2t\033[22;1t\033[27m\033[23m\033"
    "[29m\033[m\033[H\033[2J\033[\?25l\033[10;1H\"long2.txt\" 100L, 69"
    "2B\033[2;1H\342\226\275\033[6n\033[2;1H  \033[3;1H\033Pzz\033\\\033"
    "[0%m\033[6n\033[3;1H           \033[1;1H\033[>c\033[1;1Hrow 1\r\n"
    "row 2\033[2;6H\033[K\033[3;1Hrow 3\033[3;6H\033[K\033[4;1Hrow 4\r"
    "\n"
    "row 5\r\n"
    "row 6\r\n"
    "row 7\r\n"
    "row 8\r\n"
    "row 9\033[1;1H\033[\?25h\033[\?4m\033[\?25l\033[1;9r\033[9;1H\r\n"
    "\033[1;10r\033[9;1Hrow 10\033[10;1H\033[K\033[1;1H\033[\?25h\033["
    "\?25l\033[1;9r\033[9;1H\r\n"
    "\033[1;10r\033[9;1Hrow 11\033[1;1H\033[\?25h\033[\?25l\033[10;30H"
    "^E\033[1;1H\033[10;30H  \033[1;1H\033[1;9r\033[9;1H\r\n"
    "\033[1;10r\033[9;1Hrow 12\033[1;1H\033[\?25h\033[\?25l\033[10;30H"
    "dd\033[1;1H\033[10;30H  \033[1;1H\033[1;9r\033[9;1H\r\n"
    "\033[1;10r\033[9;1Hrow 13\033[1;1H\033[\?25h\033[\?25l\033[10;1H\033"
    "[1m-- INSERT --\033[m\033[10;1H\033[K\033[1;3H\rne\033[1;5H\033[K"
    "\033[2;5H5\r\n"
    "row 6\r\n"
    "row 7\r\n"
    "row 8\r\n"
    "row 9\033[6;6H\033[K\033[7;6H0\033[8;6H1\033[9;6H2\r\n"
    "\033[1m-- INSERT --\033[1;4H\033[m\033[10;30H^[\033[1;3H\033[\?25"
    "h\033[\?25l\033[10;30H  \033[1;4H\033[10;1H\033[K\033[1;3H\033[\?"
    "25h\033[\?25l\033[10;1H:q!\r\033[\?2004l\033[>4;m\033[23;2t\033[2"
    "3;1t\033[10;1H\033[K\033[10;1H\033[\?1004l\033[\?2004l\033[\?1l\033"
    ">\033[\?1049l\033[\?25h\033[>4;m";

// less on the same 100-line file, typed: Space, q.
const std::string kLess =
    "\033[\?1049h\033[10;1H\033[\?1h\033=\rrow 1\r\n"
    "row 2\r\nrow 3\r\nrow 4\r\nrow 5\r\nrow 6\r\nrow 7\r\nrow 8\r\nrow 9\r\n"
    "\033[7mlong.txt\033[27m\033[K\r\033[Krow 10\r\n"
    "row 11\r\nrow 12\r\nrow 13\r\nrow 14\r\nrow 15\r\nrow 16\r\nrow 17\r\n"
    "row 18\r\n"
    ":\033[K\r\033[K\033[\?1l\033>\033[\?1049l";

// Feed `data` up to (not including) the first `marker`.
void feedUntil(TerminalScreen &s, const std::string &data, const std::string &marker) {
    size_t at = data.find(marker);
    s.feed(data.substr(0, at));
}

// Rows [from, to) as text, one string per row.
std::vector<std::string> rowsOf(const TerminalScreen &s, int from, int to) {
    std::vector<std::string> out;
    for (int r = from; r < to; r++) out.push_back(s.rowText(r));
    return out;
}

std::vector<std::string> numberedRows(const std::string &prefix, int first, int last) {
    std::vector<std::string> out;
    for (int i = first; i <= last; i++) out.push_back(prefix + std::to_string(i));
    return out;
}

TerminalScreen screenAfter(const std::string &bytes, int cols = 10, int rows = 4) {
    TerminalScreen s(cols, rows);
    s.feed(bytes);
    return s;
}

void testTerminalScreen() {
    GROUP("screen:vim-capture");
    {
        TerminalScreen s(40, 10);
        feedUntil(s, kVimEdit, "\033[10;1H:wq");
        CHECK(s.altScreen());
        CHECK(s.appCursorKeys() && s.appKeypad() && s.bracketedPaste());
        CHECK(s.rowText(0) == "Hello line one");
        CHECK(s.rowText(1) == "line two");
        bool tildes = true;
        for (int r = 2; r <= 8; r++) tildes = tildes && s.rowText(r) == "~";
        CHECK(tildes);
        CHECK(s.rowText(9) == "");             // "-- INSERT --" was erased
        CHECK(isIndexed(s.cell(2, 0).style.fg, 12));   // ~ in bright blue
        CHECK(s.cell(0, 0).style == TermStyle());
        CHECK(s.cursorRow() == 0 && s.cursorCol() == 5);
        // vim asked where the cursor was (after an ambiguous-width probe).
        CHECK(s.takeReplies() == "\033[2;2R\033[3;1R");
        CHECK(s.takeReplies().empty());
        // "\e[0%m" and "\e[>4;2m" are not SGR: nothing turned bold or odd.
        CHECK(!s.cell(0, 0).style.bold && !s.cell(0, 0).style.underline);
        s.feed(kVimEdit.substr(kVimEdit.find("\033[10;1H:wq")));
        CHECK(!s.altScreen());
        CHECK(!s.appCursorKeys() && !s.appKeypad() && !s.bracketedPaste());
        CHECK(s.cursorVisible());
        CHECK(s.text().empty());               // the shell's screen, untouched
        CHECK(s.cursorRow() == 0 && s.cursorCol() == 0);
    }

    GROUP("screen:vim-scroll-capture");
    {
        TerminalScreen s(40, 10);
        feedUntil(s, kVimScroll, "\033[10;30Hdd");
        CHECK(rowsOf(s, 0, 9) == numberedRows("row ", 4, 12));   // three Ctrl+E
        CHECK(s.scrollTop() == 0 && s.scrollBottom() == 9);
        feedUntil(s, kVimScroll.substr(kVimScroll.find("\033[10;30Hdd")),
                  "\033[1m-- INSERT --");
        CHECK(rowsOf(s, 0, 9) == numberedRows("row ", 5, 13));   // dd
        TerminalScreen whole(40, 10);
        feedUntil(whole, kVimScroll, "\033[10;1H:q!");
        std::vector<std::string> want = {"new"};
        for (const std::string &r : numberedRows("row ", 5, 12)) want.push_back(r);
        CHECK(rowsOf(whole, 0, 9) == want);                      // Onew<Esc>
        CHECK(whole.cursorRow() == 0 && whole.cursorCol() == 2);
        // Byte at a time gives the same screen.
        TerminalScreen bytes(40, 10);
        std::string upTo = kVimScroll.substr(0, kVimScroll.find("\033[10;1H:q!"));
        for (char c : upTo) bytes.feed(&c, 1);
        CHECK(bytes.text() == whole.text());
        whole.feed(kVimScroll.substr(kVimScroll.find("\033[10;1H:q!")));
        CHECK(!whole.altScreen() && whole.text().empty());
    }

    GROUP("screen:less-capture");
    {
        TerminalScreen s(40, 10);
        s.feed("$ ls\r\nhello\r\n$ ");                 // on the main screen first
        feedUntil(s, kLess, "\033[\?1049l");
        CHECK(s.altScreen());
        CHECK(rowsOf(s, 0, 9) == numberedRows("row ", 10, 18));
        s.feed("\033[\?1049l");
        CHECK(!s.altScreen());
        CHECK(s.text() == "$ ls\nhello\n$");         // main screen restored
        CHECK(s.cursorRow() == 2 && s.cursorCol() == 2);
        // The log model keeps less's pages out of the scrollback.
        TerminalStream ts;
        std::string logged;
        for (const TermEvent &e : ts.feed("before\r\n" + kLess + "after\r\n"))
            if (e.kind == TermEvent::Line)
                for (const TermRun &r : e.runs) logged += r.text + (e.ended ? "|" : "");
        CHECK(logged.find("row") == std::string::npos);
        CHECK(logged.find("before") != std::string::npos &&
              logged.find("after") != std::string::npos);
        CHECK(!ts.altScreen());
    }

    GROUP("screen:cursor-addressing");
    {
        TerminalScreen s(10, 5);
        s.feed("\033[3;4HX");                  // CUP is 1-based
        CHECK(s.rowText(2) == "   X" && s.cursorRow() == 2 && s.cursorCol() == 4);
        s.feed("\033[2AY");                    // CUU
        CHECK(s.rowText(0) == "    Y");
        s.feed("\033[9BZ");                    // CUD clamps to the last row
        CHECK(s.rowText(4) == "     Z");
        s.feed("\033[3D!");                    // CUB
        CHECK(s.rowText(4) == "   ! Z");
        s.feed("\033[20C#");                   // CUF clamps to the last column
        CHECK(s.cell(4, 9).ch == "#");
        s.feed("\033[1G<\033[2d^");            // CHA, VPA
        CHECK(s.rowText(4)[0] == '<' && s.rowText(1) == " ^");
        s.feed("\033[H\033[2E.\033[F,");       // CNL, CPL move to column 0
        CHECK(s.rowText(2) == ".  X" && s.rowText(1) == ",^");
        s.feed("\033[;5H*");                   // empty parameter = default
        CHECK(s.rowText(0) == "    *");
        s.feed("\033[99;99H");
        CHECK(s.cursorRow() == 4 && s.cursorCol() == 9);
    }

    GROUP("screen:erase");
    {
        auto s = screenAfter("abcdefghij\r\nklmnopqrst\r\nuvwxyz\033[2;5H");
        TerminalScreen el0 = s; el0.feed("\033[K");
        CHECK(el0.rowText(1) == "klmn");
        TerminalScreen el1 = s; el1.feed("\033[1K");
        CHECK(el1.rowText(1) == "     pqrst");
        TerminalScreen el2 = s; el2.feed("\033[2K");
        CHECK(el2.rowText(1) == "" && el2.rowText(0) == "abcdefghij");
        TerminalScreen ed0 = s; ed0.feed("\033[J");
        CHECK(ed0.text() == "abcdefghij\nklmn");
        TerminalScreen ed1 = s; ed1.feed("\033[1J");
        CHECK(ed1.text() == "\n     pqrst\nuvwxyz");
        TerminalScreen ed2 = s; ed2.feed("\033[2J");
        CHECK(ed2.text().empty() && ed2.cursorRow() == 1 && ed2.cursorCol() == 4);
        TerminalScreen ech = s; ech.feed("\033[3X");
        CHECK(ech.rowText(1) == "klmn   rst" && ech.cursorCol() == 4);
        // Erase paints the current background (xterm's bce).
        TerminalScreen bce = s; bce.feed("\033[44m\033[K");
        CHECK(isIndexed(bce.cell(1, 7).style.bg, 4) && bce.cell(1, 7).ch == " ");
        CHECK(bce.cell(1, 3).style.bg.kind == TermColor::Default);
    }

    GROUP("screen:insert-delete");
    {
        auto s = screenAfter("abcdefghij\r\nklmnopqrst\r\nuvwxyz\033[1;3H");
        TerminalScreen ich = s; ich.feed("\033[2@");
        CHECK(ich.rowText(0) == "ab  cdefgh");
        TerminalScreen dch = s; dch.feed("\033[3P");
        CHECK(dch.rowText(0) == "abfghij");
        TerminalScreen il = s; il.feed("\033[2;5H\033[L");
        CHECK(il.text() == "abcdefghij\n\nklmnopqrst\nuvwxyz");
        CHECK(il.cursorCol() == 0);
        TerminalScreen dl = s; dl.feed("\033[M");
        CHECK(dl.text() == "klmnopqrst\nuvwxyz");
        TerminalScreen dl9 = s; dl9.feed("\033[2;1H\033[9M");
        CHECK(dl9.text() == "abcdefghij");
        // Insert mode (IRM) pushes the rest of the line right.
        TerminalScreen irm = s; irm.feed("\033[4hXY\033[4lZ");
        CHECK(irm.rowText(0) == "abXYZdefgh");
    }

    GROUP("screen:scroll-region");
    {
        // Rows 2-4 scroll; rows 1 and 5 stay put.
        auto s = screenAfter("top\r\na\r\nb\r\nc\r\nbottom\033[2;4r", 10, 5);
        CHECK(s.scrollTop() == 1 && s.scrollBottom() == 3);
        CHECK(s.cursorRow() == 0 && s.cursorCol() == 0);   // DECSTBM homes
        TerminalScreen lf = s; lf.feed("\033[4;1H\nd");
        CHECK(lf.text() == "top\nb\nc\nd\nbottom");
        TerminalScreen ind = s; ind.feed("\033[4;1H\033D");
        CHECK(ind.text() == "top\nb\nc\n\nbottom");
        TerminalScreen ri = s; ri.feed("\033[2;1H\033M");
        CHECK(ri.text() == "top\n\na\nb\nbottom");
        TerminalScreen su = s; su.feed("\033[2S");
        CHECK(su.text() == "top\nc\n\n\nbottom");
        TerminalScreen sd = s; sd.feed("\033[T");
        CHECK(sd.text() == "top\n\na\nb\nbottom");
        // IL/DL act inside the region only.
        TerminalScreen il = s; il.feed("\033[3;1H\033[L");
        CHECK(il.text() == "top\na\n\nb\nbottom");
        TerminalScreen outside = s; outside.feed("\033[5;1H\033[L");
        CHECK(outside.text() == "top\na\nb\nc\nbottom");
        // CUU/CUD stop at the margins when starting inside the region.
        TerminalScreen cuu = s; cuu.feed("\033[3;1H\033[9A");
        CHECK(cuu.cursorRow() == 1);
        TerminalScreen cud = s; cud.feed("\033[3;1H\033[9B");
        CHECK(cud.cursorRow() == 3);
        // An invalid region is ignored; no parameters resets it.
        TerminalScreen bad = s; bad.feed("\033[4;2r");
        CHECK(bad.scrollTop() == 1 && bad.scrollBottom() == 3);
        TerminalScreen reset = s; reset.feed("\033[r");
        CHECK(reset.scrollTop() == 0 && reset.scrollBottom() == 4);
        // Origin mode addresses rows from the top margin.
        TerminalScreen om = s; om.feed("\033[\?6h\033[1;1HX\033[9;1H");
        CHECK(om.rowText(1) == "X" && om.cursorRow() == 3);
    }

    GROUP("screen:alternate");
    {
        auto s = screenAfter("main\r\ntext\033[31m", 10, 4);
        TerminalScreen a = s;
        a.feed("\033[\?1049h");
        CHECK(a.altScreen() && a.text().empty());
        a.feed("\033[32m\033[3;3Halt");
        a.feed("\033[\?1049l");
        CHECK(!a.altScreen() && a.text() == "main\ntext");
        CHECK(a.cursorRow() == 1 && a.cursorCol() == 4);   // cursor restored
        a.feed("x");
        CHECK(isIndexed(a.cell(1, 4).style.fg, 1));          // and its style
        // 1049 always starts with a clear alternate screen.
        a.feed("\033[\?1049h");
        CHECK(a.text().empty());
        // 47 switches without saving the cursor or clearing.
        TerminalScreen b = s;
        b.feed("\033[\?47hAB\033[\?47l");
        CHECK(b.text() == "main\ntext" && b.cursorCol() == 6);
        b.feed("\033[\?47h");
        CHECK(b.text() == "\n    AB");                // kept from last time
        // 1047 clears the alternate screen when leaving it.
        TerminalScreen c = s;
        c.feed("\033[\?1047hAB\033[\?1047l\033[\?1047h");
        CHECK(c.text().empty());
    }

    GROUP("screen:save-restore");
    {
        TerminalScreen s(10, 4);
        s.feed("\033[2;3H\033[1;33m\0337\033[H\033[0mx\0338y");
        CHECK(s.rowText(1) == "  y" && s.cell(1, 2).style.bold &&
              isIndexed(s.cell(1, 2).style.fg, 3));
        CHECK(s.cell(0, 0).style == TermStyle());
        s.feed("\033[4;4H\033[s\033[H\033[uz");        // SCOSC / SCORC
        CHECK(s.rowText(3) == "   z");
        // Restoring with nothing saved goes home.
        TerminalScreen fresh(10, 4);
        fresh.feed("\033[3;3H\0338");
        CHECK(fresh.cursorRow() == 0 && fresh.cursorCol() == 0);
    }

    GROUP("screen:autowrap");
    {
        TerminalScreen s(5, 3);
        s.feed("abcde");
        // The cursor waits on the last column: the wrap is pending.
        CHECK(s.cursorRow() == 0 && s.cursorCol() == 4 && s.rowText(1) == "");
        TerminalScreen cr = s; cr.feed("\rX");
        CHECK(cr.rowText(0) == "Xbcde" && cr.rowText(1) == "");
        TerminalScreen more = s; more.feed("f");
        CHECK(more.rowText(0) == "abcde" && more.rowText(1) == "f");
        TerminalScreen lf = s; lf.feed("\r\nz");        // no blank line between
        CHECK(lf.text() == "abcde\nz");
        TerminalScreen el = s; el.feed("\033[K!");      // erase clears the pending wrap
        CHECK(el.rowText(0) == "abcd!" && el.rowText(1) == "");
        // Wrapping at the bottom scrolls.
        TerminalScreen bottom(5, 2);
        bottom.feed("12345678901234");
        CHECK(bottom.text() == "67890\n1234");
        // With autowrap off, the last column is overwritten.
        TerminalScreen off(5, 2);
        off.feed("\033[\?7labcdefg");
        CHECK(off.text() == "abcdg" && !off.autowrap());
    }

    GROUP("screen:tabs");
    {
        TerminalScreen s(20, 2);
        s.feed("a\tb\tc\td");
        CHECK(s.rowText(0) == "a       b       c  d");   // last stop clamps
        TerminalScreen h(20, 2);
        h.feed("\033[3g\033[4G\033H\033[1G\tx");        // clear all, set at col 4
        CHECK(h.rowText(0) == "   x");
        TerminalScreen cbt(20, 2);
        cbt.feed("\033[18G\033[Zy\033[2Zz");
        CHECK(cbt.rowText(0) == "        z       y");
        TerminalScreen cht(20, 2);
        cht.feed("\033[2Iq");
        CHECK(cht.cursorCol() == 17);
    }

    GROUP("screen:wide-chars");
    {
        TerminalScreen s(6, 2);
        s.feed("\xE4\xB8\xAD\xE6\x96\x87!");                  // 中文!
        CHECK(s.cell(0, 0).width == 2 && s.cell(0, 1).width == 0);
        CHECK(s.cell(0, 2).ch == "\xE6\x96\x87" && s.cell(0, 4).ch == "!");
        CHECK(s.cursorCol() == 5 && s.rowText(0) == "\xE4\xB8\xAD\xE6\x96\x87!");
        // A wide character that does not fit wraps whole.
        s.feed("\xE5\xAD\x97");                               // 字
        CHECK(s.rowText(0) == "\xE4\xB8\xAD\xE6\x96\x87!" &&
              s.rowText(1) == "\xE5\xAD\x97");
        // Overwriting half of a wide character blanks the other half.
        TerminalScreen o(6, 2);
        o.feed("\xE4\xB8\xAD\xE6\x96\x87\033[1;2Hx");
        CHECK(o.rowText(0) == " x\xE6\x96\x87");
        // Emoji are wide, combining marks join the previous cell.
        TerminalScreen e(6, 2);
        e.feed("\xF0\x9F\x98\x80" "e\xCC\x81.");
        CHECK(e.cell(0, 0).width == 2 && e.cell(0, 2).ch == "e\xCC\x81" &&
              e.cell(0, 3).ch == ".");
        CHECK(termCharWidth(0x4E2D) == 2 && termCharWidth('a') == 1 &&
              termCharWidth(0x0301) == 0 && termCharWidth(0x2713) == 1);
    }

    GROUP("screen:line-drawing");
    {
        TerminalScreen s(10, 2);
        s.feed("\033(0lqk\033(Bq\016x\017x");
        CHECK(s.rowText(0) == "\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90qxx");
        TerminalScreen g1(10, 2);
        g1.feed("\033)0a\016q\017q");
        CHECK(g1.rowText(0) == "a\xE2\x94\x80q");
        // REP repeats the last character.
        TerminalScreen rep(10, 2);
        rep.feed("-\033[4b|");
        CHECK(rep.rowText(0) == "-----|");
    }

    GROUP("screen:modes-and-replies");
    {
        TerminalScreen s(10, 4);
        CHECK(s.cursorVisible() && !s.appCursorKeys() && !s.bracketedPaste());
        s.feed("\033[\?25l\033[\?1h\033=\033[\?2004h");
        CHECK(!s.cursorVisible() && s.appCursorKeys() && s.appKeypad() &&
              s.bracketedPaste());
        s.feed("\033[\?1;2004l\033>\033[\?25h");        // several at once
        CHECK(s.cursorVisible() && !s.appCursorKeys() && !s.appKeypad() &&
              !s.bracketedPaste());
        s.feed("\033[3;7H\033[6n\033[5n\033[c\033[>c\033[\?6n");
        CHECK(s.takeReplies() == "\033[3;7R\033[0n\033[\?1;2c");
        s.feed("\033]0;my title\007");
        CHECK(s.title() == "my title");
        // Private and intermediate sequences never reach SGR.
        s.feed("\033[>4;2m\033[\?4m\033[0%m\033[2 qA");
        CHECK(s.cell(2, 6).style == TermStyle() && s.cell(2, 6).ch == "A");
        // Full reset.
        s.feed("\033[31m\033[\?1049h\033c");
        CHECK(!s.altScreen() && s.text().empty() && s.cursorRow() == 0);
    }

    GROUP("screen:resize");
    {
        TerminalScreen s(10, 4);
        s.feed("one\r\ntwo\r\nthree\r\nfour");
        s.resize(10, 2);                     // the cursor's line stays on screen
        CHECK(s.text() == "three\nfour" && s.cursorRow() == 1 && s.cursorCol() == 4);
        s.resize(3, 3);                      // columns are cut, rows added below
        CHECK(s.text() == "thr\nfou" && s.cursorCol() == 2 && s.rows() == 3);
        s.resize(6, 3);
        CHECK(s.cols() == 6 && s.rowText(0) == "thr");
        CHECK(s.scrollBottom() == 2);
        // A wide character split by the new edge becomes a blank.
        TerminalScreen w(4, 1);
        w.feed("a\xE4\xB8\xAD");
        w.resize(2, 1);
        CHECK(w.rowText(0) == "a");
        // The alternate screen resizes too.
        TerminalScreen alt(10, 4);
        alt.feed("\033[\?1049h\033[4;1Hlast");
        alt.resize(8, 2);
        CHECK(alt.altScreen() && alt.rowText(1) == "last");
    }

    GROUP("screen:selection-text");
    {
        auto s = screenAfter("abc\r\ndefgh\r\nij", 10, 3);
        CHECK(s.textBetween(0, 1, 1, 2) == "bc\ndef");
        CHECK(s.textBetween(1, 2, 0, 1) == "bc\ndef");     // either direction
        CHECK(s.textBetween(2, 0, 2, 9) == "ij");
        CHECK(s.textBetween(0, 0, 2, 9) == "abc\ndefgh\nij");
    }

    GROUP("screen:keys");
    {
        using K = TermKey;
        CHECK(TerminalScreen::encodeKey(K::Up, 0, false) == "\033[A");
        CHECK(TerminalScreen::encodeKey(K::Up, 0, true) == "\033OA");     // DECCKM
        CHECK(TerminalScreen::encodeKey(K::Left, 0, true) == "\033OD");
        CHECK(TerminalScreen::encodeKey(K::Home, 0, false) == "\033[H");
        CHECK(TerminalScreen::encodeKey(K::End, 0, true) == "\033OF");
        CHECK(TerminalScreen::encodeKey(K::Right, TermModCtrl, true) == "\033[1;5C");
        CHECK(TerminalScreen::encodeKey(K::Down, TermModShift | TermModAlt, false) ==
              "\033[1;4B");
        CHECK(TerminalScreen::encodeKey(K::PageUp, 0, false) == "\033[5~");
        CHECK(TerminalScreen::encodeKey(K::PageDown, TermModShift, false) == "\033[6;2~");
        CHECK(TerminalScreen::encodeKey(K::Delete, 0, false) == "\033[3~");
        CHECK(TerminalScreen::encodeKey(K::F1, 0, false) == "\033OP");
        CHECK(TerminalScreen::encodeKey(K::F4, TermModCtrl, false) == "\033[1;5S");
        CHECK(TerminalScreen::encodeKey(K::F5, 0, false) == "\033[15~");
        CHECK(TerminalScreen::encodeKey(K::F12, 0, false) == "\033[24~");
        CHECK(TerminalScreen::encodeKey(K::Enter, 0, false) == "\r");
        CHECK(TerminalScreen::encodeKey(K::KeypadEnter, 0, false, true) == "\033OM");
        CHECK(TerminalScreen::encodeKey(K::KeypadEnter, 0, false, false) == "\r");
        CHECK(TerminalScreen::encodeKey(K::Backspace, 0, false) == "\x7f");
        CHECK(TerminalScreen::encodeKey(K::Backspace, TermModAlt, false) == "\033\x7f");
        CHECK(TerminalScreen::encodeKey(K::Tab, 0, false) == "\t");
        CHECK(TerminalScreen::encodeKey(K::Tab, TermModShift, false) == "\033[Z");
        CHECK(TerminalScreen::encodeKey(K::Escape, 0, false) == "\033");

        CHECK(TerminalScreen::encodeChar('c', TermModCtrl) == "\x03");
        CHECK(TerminalScreen::encodeChar('C', TermModCtrl) == "\x03");
        CHECK(TerminalScreen::encodeChar('[', TermModCtrl) == "\033");
        CHECK(TerminalScreen::encodeChar(' ', TermModCtrl) == std::string(1, '\0'));
        CHECK(TerminalScreen::encodeChar('/', TermModCtrl) == "\x1f");
        CHECK(TerminalScreen::encodeChar('b', TermModAlt) == "\033b");  // Meta
        CHECK(TerminalScreen::encodeChar('x', TermModAlt | TermModCtrl) == "\033\x18");
        CHECK(TerminalScreen::encodeChar(0xE9, 0) == "\xC3\xA9");
        CHECK(TerminalScreen::encodePaste("a\nb\r\nc", false) == "a\rb\rc");
        CHECK(TerminalScreen::encodePaste("x", true) == "\033[200~x\033[201~");
        CHECK(TerminalScreen::encodePaste("x\033[201~y", true) == "\033[200~xy\033[201~");
    }
}

}  // namespace

// --------------------------------------------------------------- settings
namespace {

bool near(double a, double b) { return a - b < 1e-9 && b - a < 1e-9; }

bool colorIs(const Rgba &c, uint32_t rgb, double alpha = 1.0) {
    return c.rgb() == rgb && near(c.a, alpha);
}

Settings parseSettings(const std::string &text,
                       std::vector<SettingsError> *errors = nullptr) {
    return Settings::parse(text, errors);
}

// Every line of `text` that reads "# key = value" with the "# " removed, so
// the documented keys in the default file can be checked against the parser.
std::string uncommentKeys(const std::string &text) {
    std::string out, line;
    for (size_t i = 0; i <= text.size(); i++) {
        if (i < text.size() && text[i] != '\n') { line += text[i]; continue; }
        // "# group.field = value", not prose that happens to contain " = ".
        size_t eq = line.find(" = ");
        std::string key = eq == std::string::npos ? "" : line.substr(2, eq - 2);
        bool isKey = line.rfind("# ", 0) == 0 && key.find('.') != std::string::npos;
        for (char ch : key)
            if (!(ch == '.' || (ch >= 'a' && ch <= 'z'))) isKey = false;
        if (isKey) out += line.substr(2) + "\n";
        line.clear();
    }
    return out;
}

void testSettings() {
    GROUP("settings:colors");
    Rgba c;
    CHECK(Settings::parseColor("#1E1E1E", c) && colorIs(c, 0x1E1E1E));
    CHECK(Settings::parseColor("#1e1e1e", c) && colorIs(c, 0x1E1E1E));
    CHECK(Settings::parseColor("#fff", c) && colorIs(c, 0xFFFFFF));
    CHECK(Settings::parseColor("#0A0", c) && colorIs(c, 0x00AA00));
    CHECK(Settings::parseColor("#00000080", c) && colorIs(c, 0x000000, 128 / 255.0));
    CHECK(Settings::parseColor("#FFFFFF00", c) && near(c.a, 0));
    CHECK(!Settings::parseColor("1E1E1E", c));      // no #
    CHECK(!Settings::parseColor("#12", c));
    CHECK(!Settings::parseColor("#12345", c));
    CHECK(!Settings::parseColor("#1234567", c));
    CHECK(!Settings::parseColor("#GGGGGG", c));
    CHECK(!Settings::parseColor("#", c));
    CHECK(!Settings::parseColor("", c));
    CHECK(!Settings::parseColor("red", c));

    GROUP("settings:opacity");
    double o = -1;
    CHECK(Settings::parseOpacity("0.5", o) && near(o, 0.5));
    CHECK(Settings::parseOpacity(".25", o) && near(o, 0.25));
    CHECK(Settings::parseOpacity("1", o) && near(o, 1));
    CHECK(Settings::parseOpacity("0", o) && near(o, 0));
    CHECK(Settings::parseOpacity("80%", o) && near(o, 0.8));
    CHECK(Settings::parseOpacity("100%", o) && near(o, 1));
    CHECK(Settings::parseOpacity("12.5%", o) && near(o, 0.125));
    CHECK(!Settings::parseOpacity("1.5", o));
    CHECK(!Settings::parseOpacity("80", o));        // a bare 80 is not 80%
    CHECK(!Settings::parseOpacity("-0.1", o));
    CHECK(!Settings::parseOpacity("101%", o));
    CHECK(!Settings::parseOpacity("%", o));
    CHECK(!Settings::parseOpacity("half", o));
    CHECK(!Settings::parseOpacity("0.5.0", o));
    CHECK(!Settings::parseOpacity("", o));

    GROUP("settings:defaults");
    Settings d = parseSettings("");
    CHECK(colorIs(d.background(Surface::Editor), 0x1E1E1E));
    CHECK(colorIs(d.background(Surface::Sidebar), 0x252526));
    CHECK(colorIs(d.background(Surface::Terminal), 0x181818));
    CHECK(colorIs(d.background(Surface::Statusbar), 0x007ACC));
    CHECK(colorIs(d.background(Surface::Browser), 0x2A2A2A));
    CHECK(colorIs(d.text(Surface::Editor), 0xD4D4D4));
    CHECK(colorIs(d.text(Surface::Sidebar), 0xCCCCCC));
    CHECK(colorIs(d.text(Surface::Statusbar), 0xFFFFFF));
    CHECK(colorIs(d.syntax(TokenStyle::Keyword), 0x569CD6));
    CHECK(colorIs(d.syntax(TokenStyle::Comment), 0x6A9955));
    CHECK(colorIs(d.syntax(TokenStyle::Plain), 0xD4D4D4));
    CHECK(colorIs(d.markdown(MarkdownColor::Link), 0x4EA1F7));
    CHECK(colorIs(d.markdownCodeBackground(), 0x2A2A2A));
    CHECK(colorIs(d.terminalInputBackground(), 0x232323));
    CHECK(colorIs(d.terminalInputText(), 0xEDEDED));
    CHECK(near(d.opacity(Surface::Editor), 1));
    CHECK(!d.blur());
    CHECK(d.material() == "under-window");
    CHECK(d.windowIsOpaque());
    CHECK(!d.customTitlebar());
    CHECK(!d.textIsSet(Surface::Browser));
    CHECK(std::string(Settings::surfaceName(Surface::Statusbar)) == "statusbar");

    GROUP("settings:panel-opacity");
    Settings p = parseSettings("editor.opacity = 0.6\nsidebar.opacity = 40%\n");
    CHECK(colorIs(p.background(Surface::Editor), 0x1E1E1E, 0.6));
    CHECK(colorIs(p.background(Surface::Sidebar), 0x252526, 0.4));
    CHECK(colorIs(p.background(Surface::Terminal), 0x181818, 1));  // untouched
    CHECK(colorIs(p.text(Surface::Editor), 0xD4D4D4, 1));          // text stays solid
    CHECK(near(p.markdownCodeBackground().a, 0.6));                // follows editor
    CHECK(near(p.terminalInputBackground().a, 1));
    CHECK(!p.windowIsOpaque());
    CHECK(p.customTitlebar());   // a see-through window draws its own title bar
    CHECK(near(p.background(Surface::Titlebar).a, 1));

    GROUP("settings:window-opacity");
    Settings w = parseSettings("window.opacity = 0.5\nterminal.opacity = 0.9\n");
    CHECK(near(w.opacity(Surface::Editor), 0.5));
    CHECK(near(w.opacity(Surface::Titlebar), 0.5));
    CHECK(near(w.opacity(Surface::Statusbar), 0.5));
    CHECK(near(w.opacity(Surface::Terminal), 0.9));      // its own wins
    CHECK(near(w.terminalInputBackground().a, 0.9));
    // Order doesn't matter: the panel's own opacity wins either way.
    Settings w2 = parseSettings("terminal.opacity = 0.9\nwindow.opacity = 0.5\n");
    CHECK(near(w2.opacity(Surface::Terminal), 0.9));
    // A panel can be fully opaque inside a see-through window.
    Settings w3 = parseSettings("window.opacity = 0.3\neditor.opacity = 1\n");
    CHECK(near(w3.background(Surface::Editor).a, 1));
    CHECK(near(w3.background(Surface::Sidebar).a, 0.3));

    GROUP("settings:background-alpha");
    // A color's own alpha multiplies with the opacity.
    Settings ba = parseSettings("editor.background = #10203080\n"
                                "editor.opacity = 0.5\n");
    CHECK(ba.background(Surface::Editor).rgb() == 0x102030);
    CHECK(near(ba.background(Surface::Editor).a, (128 / 255.0) * 0.5));
    Settings bb = parseSettings("statusbar.background = #333\n");
    CHECK(colorIs(bb.background(Surface::Statusbar), 0x333333));
    CHECK(bb.windowIsOpaque());
    Settings bc = parseSettings("sidebar.background = #25252600\n");
    CHECK(!bc.windowIsOpaque());

    GROUP("settings:text");
    Settings t = parseSettings("window.text = #EEEEEE\neditor.text = #00FF00\n"
                               "sidebar.text = #FF000080\n");
    CHECK(colorIs(t.text(Surface::Editor), 0x00FF00));
    CHECK(colorIs(t.text(Surface::Sidebar), 0xFF0000, 128 / 255.0));
    CHECK(colorIs(t.text(Surface::Terminal), 0xEEEEEE));   // window.text fallback
    CHECK(colorIs(t.text(Surface::Statusbar), 0xEEEEEE));
    CHECK(colorIs(t.syntax(TokenStyle::Plain), 0x00FF00)); // plain = editor.text
    CHECK(colorIs(t.terminalInputText(), 0xEEEEEE));
    CHECK(t.textIsSet(Surface::Browser));
    CHECK(t.windowIsOpaque());          // text alone never makes it see-through
    CHECK(!t.customTitlebar());
    Settings t2 = parseSettings("browser.text = #111111\n");
    CHECK(t2.textIsSet(Surface::Browser));
    CHECK(!t2.textIsSet(Surface::Editor));

    GROUP("settings:syntax-markdown");
    Settings sy = parseSettings("syntax.keyword = #FF0000\nsyntax.function = #00F\n"
                                "markdown.heading = #ABCDEF\nmarkdown.quote = #123\n");
    CHECK(colorIs(sy.syntax(TokenStyle::Keyword), 0xFF0000));
    CHECK(colorIs(sy.syntax(TokenStyle::Function), 0x0000FF));
    CHECK(colorIs(sy.syntax(TokenStyle::String), 0xCE9178));   // default kept
    CHECK(colorIs(sy.markdown(MarkdownColor::Heading), 0xABCDEF));
    CHECK(colorIs(sy.markdown(MarkdownColor::Quote), 0x112233));
    CHECK(colorIs(sy.markdown(MarkdownColor::Code), 0xCE9178));
    std::vector<SettingsError> se;
    parseSettings("syntax.plain = #FFFFFF\n", &se);   // plain is editor.text
    CHECK(se.size() == 1);

    GROUP("settings:window");
    Settings bl = parseSettings("window.blur = true\nwindow.material = HUD\n");
    CHECK(bl.blur());
    CHECK(bl.material() == "hud");
    CHECK(!bl.windowIsOpaque());
    CHECK(bl.customTitlebar());
    for (const char *v : {"on", "yes", "1", "TRUE"})
        CHECK(parseSettings(std::string("window.blur = ") + v).blur());
    for (const char *v : {"off", "no", "0", "false"})
        CHECK(!parseSettings(std::string("window.blur = true\nwindow.blur = ") + v).blur());
    Settings tb = parseSettings("titlebar.background = #000000\n");
    CHECK(tb.customTitlebar());          // any titlebar key takes it over
    CHECK(tb.windowIsOpaque());
    CHECK(colorIs(tb.background(Surface::Titlebar), 0x000000));
    Settings tb2 = parseSettings("titlebar.opacity = 0\n");
    CHECK(tb2.customTitlebar());
    CHECK(!tb2.windowIsOpaque());        // the title bar counts once it's ours

    GROUP("settings:syntax-of-file");
    std::vector<SettingsError> e1;
    Settings f = parseSettings(
        "# a comment\n"
        "   \n"
        "  # indented comment\n"
        "EDITOR.Opacity=0.5\n"                      // case, no spaces
        "sidebar.opacity =\t25%   # trailing comment\n"
        "statusbar.background = #FF0000#not-a-comment-space\n"
        "terminal.text = #00FF00\r\n"               // CRLF
        "editor.opacity = 0.7", &e1);               // no final newline
    CHECK(near(f.opacity(Surface::Editor), 0.7));   // last one wins
    CHECK(near(f.opacity(Surface::Sidebar), 0.25));
    CHECK(colorIs(f.text(Surface::Terminal), 0x00FF00));
    CHECK(e1.size() == 1);   // "#FF0000#not..." is not a color
    CHECK(e1.size() == 1 && e1[0].line == 6);
    CHECK(colorIs(f.background(Surface::Statusbar), 0x007ACC));  // default kept

    GROUP("settings:errors");
    std::vector<SettingsError> e2;
    Settings bad = parseSettings(
        "editor.opacity = 0.5\n"         // 1 ok
        "editor.opacity = 2\n"           // 2 out of range
        "editr.opacity = 0.5\n"          // 3 unknown surface
        "editor.color = #FFFFFF\n"       // 4 unknown field
        "just some words\n"              // 5 no '='
        "= #FFFFFF\n"                    // 6 no key
        "editor.text =\n"                // 7 no value
        "editor.text = #FFF extra\n"     // 8 junk after value
        "sidebar.text = white\n"         // 9 not a color
        "window.blur = maybe\n"          // 10 not a bool
        "window.material = glass\n"      // 11 unknown material
        "editor.opacity = 80\n"          // 12 bare percentage
        "editr.opacity = abc\n",         // 13 unknown beats bad value
        &e2);
    CHECK(e2.size() == 12);
    std::vector<int> lines;
    for (const SettingsError &e : e2) lines.push_back(e.line);
    CHECK((lines == std::vector<int>{2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}));
    CHECK(near(bad.opacity(Surface::Editor), 0.5));      // bad lines ignored
    CHECK(colorIs(bad.text(Surface::Sidebar), 0xCCCCCC));
    CHECK(!bad.blur());
    CHECK(bad.material() == "under-window");
    auto has = [&](size_t i, const char *needle) {
        return i < e2.size() && e2[i].message.find(needle) != std::string::npos;
    };
    CHECK(has(0, "not an opacity"));
    CHECK(has(1, "unknown setting 'editr.opacity'"));
    CHECK(has(2, "unknown setting 'editor.color'"));
    CHECK(has(3, "expected key = value"));
    CHECK(has(7, "not a color"));
    CHECK(has(8, "not true or false"));
    CHECK(has(9, "unknown material"));
    CHECK(has(10, "80%"));                               // hints at the fix
    CHECK(has(11, "unknown setting"));
    parseSettings("editor.opacity = 2\n");              // null errors is fine

    GROUP("settings:default-file");
    std::vector<SettingsError> e3;
    Settings df = parseSettings(Settings::defaultFileText(), &e3);
    CHECK(e3.empty());
    CHECK(df.windowIsOpaque() && !df.customTitlebar() && !df.blur());
    // Every documented key, uncommented, is one the parser accepts, and its
    // documented value is the real default.
    std::string keys = uncommentKeys(Settings::defaultFileText());
    CHECK(std::count(keys.begin(), keys.end(), '\n') == 33);   // 4 window, 18 panel, 7 syntax, 4 markdown
    std::vector<SettingsError> e4;
    Settings un = parseSettings(keys, &e4);
    CHECK(e4.empty());
    for (const SettingsError &e : e4)
        std::printf("    line %d: %s\n", e.line, e.message.c_str());
    for (int i = 0; i < kSurfaceCount; i++) {
        Surface s = (Surface)i;
        CHECK(un.background(s) == d.background(s));
        CHECK(un.text(s) == d.text(s));
    }
    for (int i = 1; i < 8; i++)
        CHECK(un.syntax((TokenStyle)i) == d.syntax((TokenStyle)i));
    for (int i = 0; i < kMarkdownColorCount; i++)
        CHECK(un.markdown((MarkdownColor)i) == d.markdown((MarkdownColor)i));
    CHECK(un.material() == d.material() && un.blur() == d.blur());
}

}  // namespace

// ------------------------------------------------------------ line comments
namespace {

std::string narrow(const std::u16string &s) {
    std::string out;
    for (char16_t c : s) out += c < 0x80 ? (char)c : '?';
    return out;
}

// Toggle with the selection written into the text as [ and ] (or a single |
// for a caret), and return the result written the same way.
std::string toggled(const std::string &marked, const std::string &marker) {
    std::u16string text;
    size_t a = std::string::npos, b = std::string::npos;
    for (char c : marked) {
        if (c == '|') { a = b = text.size(); continue; }
        if (c == '[') { a = text.size(); continue; }
        if (c == ']') { b = text.size(); continue; }
        text += (char16_t)(unsigned char)c;
    }
    LineComments::Result r = LineComments::toggle(text, a, b, marker);
    // The single-replacement form must describe the same edit.
    std::u16string viaReplace = text;
    if (r.changed)
        viaReplace.replace(r.replaceStart, r.replaceLength, r.replacement);
    if (viaReplace != r.text) return "replacement mismatch";
    std::string out = narrow(r.text);
    if (r.selStart == r.selEnd) return out.insert(r.selStart, "|");
    out.insert(r.selEnd, "]");
    return out.insert(r.selStart, "[");
}

void testLineComments() {
    using LineComments::markerFor;
    GROUP("comments:markers");
    CHECK(markerFor("a.py") == "#");
    CHECK(markerFor("settings.conf") == "#");
    CHECK(markerFor("/Users/x/.config/minicode/settings.conf") == "#");
    CHECK(markerFor("main.CPP") == "//");
    CHECK(markerFor("App.mm") == "//");
    CHECK(markerFor("index.tsx") == "//");
    CHECK(markerFor("query.sql") == "--");
    CHECK(markerFor("init.lua") == "--");
    CHECK(markerFor("setup.ini") == ";");
    CHECK(markerFor("Makefile") == "#");
    CHECK(markerFor("src/Dockerfile") == "#");
    CHECK(markerFor(".zshrc") == "#");
    CHECK(markerFor("paper.tex") == "%");
    CHECK(markerFor("data.json") == "");      // JSON has no comments
    CHECK(markerFor("README.md") == "");
    CHECK(markerFor("LICENSE") == "");
    CHECK(markerFor("") == "");

    GROUP("comments:single-line");
    CHECK(toggled("x = 1|", "#") == "# x = 1|");
    CHECK(toggled("x| = 1", "#") == "# x| = 1");
    CHECK(toggled("|x = 1", "#") == "# |x = 1");       // caret follows the text
    CHECK(toggled("# x = 1|", "#") == "x = 1|");
    CHECK(toggled("#x = 1|", "#") == "x = 1|");        // no space after marker
    CHECK(toggled("#  x|", "#") == " x|");             // only one space removed
    CHECK(toggled("    return 0;|", "//") == "    // return 0;|");
    CHECK(toggled("    // return 0;|", "//") == "    return 0;|");
    CHECK(toggled("  // |a", "//") == "  |a");         // caret inside removed marker
    CHECK(toggled("\tint a;|", "//") == "\t// int a;|");
    CHECK(toggled("a\nb|\nc", "#") == "a\n# b|\nc");   // only the caret's line
    CHECK(toggled("|", "#") == "# |");                 // empty line
    CHECK(toggled("a\n|", "#") == "a\n# |");
    CHECK(toggled("   |", "#") == "   # |");
    CHECK(toggled("SELECT 1|", "--") == "-- SELECT 1|");

    GROUP("comments:multi-line");
    CHECK(toggled("[a\nb\nc]", "#") == "[# a\n# b\n# c]");
    CHECK(toggled("[# a\n# b\n# c]", "#") == "[a\nb\nc]");
    // Mixed: anything uncommented means comment them all.
    CHECK(toggled("[# a\nb]", "#") == "[# # a\n# b]");
    // Inserted at the smallest indentation, keeping the block aligned.
    CHECK(toggled("[  if x:\n    y\n  z]", "#") == "[  # if x:\n  #   y\n  # z]");
    CHECK(toggled("[  # if x:\n  #   y\n  # z]", "#") == "[  if x:\n    y\n  z]");
    // Blank lines are skipped, in both directions.
    CHECK(toggled("[a\n\nb]", "//") == "[// a\n\n// b]");
    CHECK(toggled("[// a\n   \n// b]", "//") == "[a\n   \nb]");
    // A selection that ends at the start of a line leaves that line alone.
    CHECK(toggled("[a\nb\n]c", "#") == "[# a\n# b\n]c");
    // Selection starting mid-line keeps its place in the text.
    CHECK(toggled("ab[c\nd]e", "#") == "# ab[c\n# d]e");
    CHECK(toggled("# ab[c\n# d]e", "#") == "ab[c\nd]e");
    // Surrounding lines are untouched.
    CHECK(toggled("keep\n[a\nb]\nkeep", "//") == "keep\n[// a\n// b]\nkeep");
    // Backwards selection is fine.
    std::u16string t = u"a\nb";
    LineComments::Result r = LineComments::toggle(t, 3, 0, "#");
    CHECK(narrow(r.text) == "# a\n# b");

    GROUP("comments:non-ascii");
    // UTF-16 offsets: an emoji is two units, and the caret stays after it.
    std::u16string emoji = u"x = \U0001F600";
    LineComments::Result e = LineComments::toggle(emoji, emoji.size(), emoji.size(), "#");
    CHECK(e.text == u"# x = \U0001F600");
    CHECK(e.selStart == e.text.size());

    GROUP("comments:no-marker");
    LineComments::Result n = LineComments::toggle(u"abc", 1, 1, "");
    CHECK(!n.changed && n.text == u"abc" && n.selStart == 1);
    // Out-of-range selections are clamped rather than crashing.
    LineComments::Result o = LineComments::toggle(u"abc", 99, 99, "#");
    CHECK(o.changed && o.text == u"# abc");
}

void testSettingsColorEditing() {
    GROUP("settings:find-color");
    ColorSpan span;
    CHECK(Settings::findColor(u"editor.text = #D4D4D4", span) &&
          span.start == 14 && span.length == 7 && colorIs(span.color, 0xD4D4D4));
    CHECK(Settings::findColor(u"# editor.text = #D4D4D4", span) && span.start == 16);
    CHECK(Settings::findColor(u"sidebar.background=#12345680  # note", span) &&
          span.start == 19 && span.length == 9 && near(span.color.a, 128 / 255.0));
    CHECK(Settings::findColor(u"x = #abc\r", span) && span.length == 4);
    CHECK(!Settings::findColor(u"editor.opacity = 0.5", span));
    CHECK(!Settings::findColor(u"# Colors are #RRGGBB, or #RRGGBBAA", span));
    CHECK(!Settings::findColor(u"# Each line is key = value, and", span));
    CHECK(!Settings::findColor(u"x = #GGG", span));
    CHECK(!Settings::findColor(u"", span));

    GROUP("settings:format-color");
    CHECK(Settings::formatColor(Rgba::hex(0x0a0b0c)) == "#0A0B0C");
    CHECK(Settings::formatColor(Rgba::hex(0xFFFFFF, 0.5)) == "#FFFFFF80");
    CHECK(Settings::formatColor(Rgba::hex(0x000000, 0)) == "#00000000");
    CHECK(Settings::formatColor(Rgba::hex(0x123456, 0.999)) == "#123456");
    Rgba back;
    CHECK(Settings::parseColor(Settings::formatColor(Rgba::hex(0x336699, 0.25)), back) &&
          back.rgb() == 0x336699 && back.a > 0.24 && back.a < 0.26);

    GROUP("settings:contrast");
    Settings dark = Settings::parse("");
    CHECK(dark.contrastText(Rgba::hex(0xFFFFFF)).rgb() == 0x000000);
    CHECK(dark.contrastText(Rgba::hex(0xD4D4D4)).rgb() == 0x000000);
    CHECK(dark.contrastText(Rgba::hex(0x1E1E1E)).rgb() == 0xFFFFFF);
    CHECK(dark.contrastText(Rgba::hex(0x007ACC)).rgb() == 0xFFFFFF);
    // A see-through white swatch over the dark editor mostly shows the editor.
    CHECK(dark.contrastText(Rgba::hex(0xFFFFFF, 0.1)).rgb() == 0xFFFFFF);
    Settings light = Settings::parse("editor.background = #FFFFFF\n");
    CHECK(light.contrastText(Rgba::hex(0x000000, 0.1)).rgb() == 0x000000);

    GROUP("settings:set-color");
    Rgba red = Rgba::hex(0xFF0000);
    CHECK(Settings::setColor(u"editor.text = #D4D4D4", red) == u"editor.text = #FF0000");
    // A commented-out setting is switched on.
    CHECK(Settings::setColor(u"# editor.text = #D4D4D4", red) == u"editor.text = #FF0000");
    CHECK(Settings::setColor(u"#editor.text = #D4D4D4", red) == u"editor.text = #FF0000");
    CHECK(Settings::setColor(u"  # editor.text = #D4D4D4", red) == u"  editor.text = #FF0000");
    // Trailing comments and alpha survive.
    CHECK(Settings::setColor(u"sidebar.text = #FFF  # mine", Rgba::hex(0x00FF00, 0.5)) ==
          u"sidebar.text = #00FF0080  # mine");
    // Lines without a color, or prose, are left alone.
    CHECK(Settings::setColor(u"editor.opacity = 0.5", red) == u"editor.opacity = 0.5");
    CHECK(Settings::setColor(u"# Colors are #RRGGBB", red) == u"# Colors are #RRGGBB");
    CHECK(Settings::setColor(u"# a note, x y = #FFFFFF", red) == u"# a note, x y = #FF0000");
    // The result parses as the new setting.
    std::u16string line = Settings::setColor(u"# terminal.text = #D4D4D4", red);
    CHECK(colorIs(Settings::parse(narrow(line)).text(Surface::Terminal), 0xFF0000));
}

// ------------------------------------------------------------------- latex

// The span covering `text`, or nullptr.
const LatexSpan *spanWithText(const LatexDoc &doc, const std::string &text) {
    for (const LatexSpan &sp : doc.spans)
        if (sp.display == text) return &sp;
    return nullptr;
}

bool hasSpanText(const LatexDoc &doc, const std::string &text) {
    return spanWithText(doc, text) != nullptr;
}

const char *kSampleTex =
    "\\documentclass{article}\n"                       // 1
    "\\usepackage{amsmath}\n"                          // 2
    "\\title{My Paper}\n"                              // 3
    "\\author{A. Writer}\n"                            // 4
    "\\begin{document}\n"                              // 5
    "\\maketitle\n"                                    // 6
    "\\section{Introduction}\\label{sec:intro}\n"       // 7
    "Some prose with \\textbf{bold} inside.\n"          // 8
    "\n"                                               // 9
    "Another paragraph, $x^2 + 1$ included.\n"          // 10
    "\\begin{itemize}\n"                               // 11
    "  \\item First thing\n"                           // 12
    "  \\item Second thing\n"                          // 13
    "\\end{itemize}\n"                                 // 14
    "\\end{document}\n";                               // 15

void testLatexDoc() {
    std::string src = kSampleTex;
    LatexDoc doc = LatexDoc::parse(src);

    GROUP("latex:fields");
    const LatexSpan *title = spanWithText(doc, "My Paper");
    CHECK(title != nullptr);
    CHECK(title && title->kind == LatexSpanKind::Field);
    CHECK(title && title->command == "title");
    CHECK(title && title->line == 3);
    CHECK(title && src.substr(title->start, title->end - title->start) == "My Paper");
    const LatexSpan *sec = spanWithText(doc, "Introduction");
    CHECK(sec && sec->command == "section" && sec->line == 7);
    CHECK(hasSpanText(doc, "A. Writer"));

    GROUP("latex:skips-machinery");
    // Package options, labels and class names are not prose, so are not offered.
    CHECK(!hasSpanText(doc, "amsmath"));
    CHECK(!hasSpanText(doc, "sec:intro"));
    CHECK(!hasSpanText(doc, "article"));

    GROUP("latex:text-runs");
    // A styling command splits the run; its content is editable on its own.
    CHECK(hasSpanText(doc, "Some prose with"));
    CHECK(hasSpanText(doc, "bold"));
    CHECK(hasSpanText(doc, "inside."));
    // A blank line ends a paragraph.
    const LatexSpan *para = spanWithText(doc, "Another paragraph,");
    CHECK(para != nullptr);
    CHECK(para && para->line == 10);
    // Nothing before \begin{document} is treated as body text.
    for (const LatexSpan &sp : doc.spans)
        if (sp.kind == LatexSpanKind::Text) CHECK(sp.line >= 6);

    GROUP("latex:math");
    const LatexSpan *math = nullptr;
    for (const LatexSpan &sp : doc.spans)
        if (sp.kind == LatexSpanKind::Math) math = &sp;
    CHECK(math != nullptr);
    CHECK(math && src.substr(math->start, math->end - math->start) == "x^2 + 1");
    LatexDoc envMath = LatexDoc::parse(
        "\\begin{document}\\begin{align}\na &= b\n\\end{align}\\end{document}");
    CHECK(envMath.spans.size() == 1);
    CHECK(envMath.spans[0].kind == LatexSpanKind::Math);
    CHECK(envMath.spans[0].display.find("a") != std::string::npos);

    GROUP("latex:lists");
    CHECK(doc.lists.size() == 1);
    const LatexList &list = doc.lists[0];
    CHECK(list.environment == "itemize");
    CHECK(list.itemBodyStart.size() == 2);
    CHECK(list.itemBodyEnd.size() == 2);
    CHECK(list.indent == "  ");
    const LatexSpan *first = spanWithText(doc, "First thing");
    CHECK(first != nullptr);
    CHECK(first && first->listIndex == 0 && first->itemIndex == 0);
    const LatexSpan *second = spanWithText(doc, "Second thing");
    CHECK(second && second->itemIndex == 1);

    GROUP("latex:lines");
    CHECK(LatexDoc::lineAt(src, 0) == 1);
    CHECK(doc.spansOnLine(12).size() == 1);
    CHECK(doc.spansOnLine(999).empty());

    GROUP("latex:display-text");
    CHECK(LatexDoc::displayText("a  b\n  c") == "a b c");
    CHECK(LatexDoc::displayText("50\\% off \\& more") == "50% off & more");
    CHECK(LatexDoc::displayText("a~b") == "a b");
    CHECK(LatexDoc::displayText("The \\emph{fun} part") == "The fun part");
    CHECK(LatexDoc::displayText("x % a comment\ny") == "x y");

    GROUP("latex:replace");
    LatexDoc::Edit e = LatexDoc::replaceSpan(src, *title, "A Better Title");
    CHECK(e.source.find("\\title{A Better Title}") != std::string::npos);
    CHECK(e.source.find("My Paper") == std::string::npos);
    CHECK(e.source.substr(e.start, e.end - e.start) == "A Better Title");
    // The rest of the file is untouched, byte for byte.
    CHECK(e.source.substr(0, title->start) == src.substr(0, title->start));
    CHECK(e.source.substr(e.end) == src.substr(title->end));
    // Re-parsing the result finds the new text where the old one was.
    LatexDoc after = LatexDoc::parse(e.source);
    CHECK(hasSpanText(after, "A Better Title"));

    GROUP("latex:add-item");
    LatexDoc::Edit add = LatexDoc::addItem(src, list, 1, "Third thing");
    CHECK(add.source.find("  \\item Second thing\n  \\item Third thing\n"
                          "\\end{itemize}") != std::string::npos);
    CHECK(add.source.substr(add.start, add.end - add.start) == "Third thing");
    LatexDoc addedDoc = LatexDoc::parse(add.source);
    CHECK(addedDoc.lists.size() == 1 && addedDoc.lists[0].itemBodyStart.size() == 3);
    CHECK(hasSpanText(addedDoc, "Third thing"));
    // Inserting in the middle keeps the order.
    LatexDoc::Edit mid = LatexDoc::addItem(src, list, 0, "Middle");
    CHECK(mid.source.find("First thing\n  \\item Middle\n  \\item Second") !=
          std::string::npos);
    // A list written on one line still gets a well-formed new item.
    std::string oneLine =
        "\\begin{document}\\begin{itemize}\\item one\\end{itemize}\\end{document}";
    LatexDoc oneDoc = LatexDoc::parse(oneLine);
    CHECK(oneDoc.lists.size() == 1);
    LatexDoc::Edit oneAdd = LatexDoc::addItem(oneLine, oneDoc.lists[0], 0, "two");
    CHECK(LatexDoc::parse(oneAdd.source).lists[0].itemBodyStart.size() == 2);
    CHECK(hasSpanText(LatexDoc::parse(oneAdd.source), "two"));

    GROUP("latex:click");
    // SyncTeX names the line a paragraph closed on, one past the prose here.
    const LatexSpan *hit = doc.spanForClick({9}, "prose");
    CHECK(hit && hit->display == "Some prose with");
    // The word decides between two items whose lines both look plausible.
    CHECK(doc.spanForClick({13, 14}, "First")->display == "First thing");
    CHECK(doc.spanForClick({13, 14}, "Second")->display == "Second thing");
    // A heading is found on its own line.
    CHECK(doc.spanForClick({7}, "Introduction")->display == "Introduction");
    // Without a usable word, the nearest span still wins.
    CHECK(doc.spanForClick({12}, "")->display == "First thing");
    CHECK(doc.spanForClick({12}, "nowhere") == nullptr);   // refuses to guess
    CHECK(doc.spanForClick({}, "First") == nullptr);
    // The PDF reads math back without its markup, so matching ignores it.
    CHECK(doc.spanForClick({10}, "x2")->kind == LatexSpanKind::Math);
    CHECK(doc.spanForClick({10}, "paragraph,")->display == "Another paragraph,");
    CHECK(doc.spanForClick({900}, "First") == nullptr);

    GROUP("latex:real-documents");
    // Real documents wrap their text in all sorts of commands, including the
    // author's own macros. Text the reader cannot see is text the preview
    // cannot edit, so unknown commands are looked through, not skipped.
    LatexDoc wild = LatexDoc::parse(
        "\\begin{document}\n"
        "\\normalfont{Post-training of Gemini}\n"
        "\\raisebox{0.5em}{\\textit{Senior Engineer} @ Google}\n"
        "\\makebox[8em][l]{\\includegraphics[scale=0.1]{GDM.png}}\n"
        "\\twemoji{globe} \\href{https://echansen.org}{echansen.org}\n"
        "\\todo{Bullet 2 -- scale and adoption}\n"
        "\\vspace*{-\\baselineskip}\n"
        "\\setlength{\\parskip}{1em}\n"
        "\\multicolumn{2}{c}{Total}\n"
        "\\end{document}\n");
    CHECK(hasSpanText(wild, "Post-training of Gemini"));
    CHECK(hasSpanText(wild, "Senior Engineer"));
    CHECK(hasSpanText(wild, "@ Google"));
    CHECK(hasSpanText(wild, "Bullet 2 -- scale and adoption"));   // a user macro
    CHECK(hasSpanText(wild, "Total"));
    // ... while arguments that are machinery stay out of reach.
    CHECK(!hasSpanText(wild, "GDM.png"));
    CHECK(!hasSpanText(wild, "https://echansen.org"));
    CHECK(hasSpanText(wild, "echansen.org"));     // the link text, not its URL
    CHECK(!hasSpanText(wild, "0.5em"));
    CHECK(!hasSpanText(wild, "1em"));
    CHECK(!hasSpanText(wild, "8em"));
    CHECK(!hasSpanText(wild, "l"));
    CHECK(!hasSpanText(wild, "c"));
    CHECK(!hasSpanText(wild, "2"));
    // A date is text even though it has no letters in it.
    LatexDoc dates = LatexDoc::parse(
        "\\begin{document}\\raisebox{0.5em}{\\textit{2014 - 2019}}"
        "\\normalfont{11/2025 - ...}\\end{document}");
    CHECK(hasSpanText(dates, "2014 - 2019"));
    CHECK(hasSpanText(dates, "11/2025 - ..."));
    CHECK(!hasSpanText(dates, "0.5em"));
    LatexDoc refs = LatexDoc::parse(
        "\\begin{document}\\label{sec:intro}\\cite{knuth1984}"
        "\\includegraphics{fig.pdf}\\input{chapter}Real text\\end{document}");
    CHECK(refs.spans.size() == 1 && refs.spans[0].display == "Real text");

    GROUP("latex:click-refuses");
    // A click on something the reader does not know must not open the nearest
    // text instead: editing the wrong line is worse than editing nothing.
    CHECK(doc.spanForClick({12, 13}, "Photograph") == nullptr);
    CHECK(doc.spanForClick({7}, "Introduction") != nullptr);
    // With nothing to match on (a logo, a glyph the PDF cannot name), the
    // nearest span is still offered.
    CHECK(doc.spanForClick({12}, "") != nullptr);
    // A click that reports a whole line finds the span inside it.
    CHECK(doc.spanForClick({12}, "First thing and more")->display == "First thing");

    GROUP("latex:verbatim");
    LatexDoc verb = LatexDoc::parse(
        "\\begin{document}\\begin{verbatim}\n\\section{no}\n"
        "\\end{verbatim}\nafter\\end{document}");
    CHECK(!hasSpanText(verb, "no"));
    CHECK(hasSpanText(verb, "after"));

    GROUP("latex:tabular");
    LatexDoc tab = LatexDoc::parse(
        "\\begin{document}\\begin{tabular}{|l|r|}\nApples & 3 \\\\\n"
        "\\end{tabular}\\end{document}");
    CHECK(!hasSpanText(tab, "|l|r|"));
    CHECK(hasSpanText(tab, "Apples"));   // each cell is editable on its own
    CHECK(hasSpanText(tab, "3"));

    GROUP("latex:description");
    LatexDoc desc = LatexDoc::parse(
        "\\begin{document}\\begin{description}\n\\item[Term] meaning\n"
        "\\end{description}\\end{document}");
    CHECK(hasSpanText(desc, "Term"));
    CHECK(hasSpanText(desc, "meaning"));
    CHECK(desc.lists.size() == 1 && desc.lists[0].itemBodyStart.size() == 1);

    GROUP("latex:robustness");
    // Unterminated input must not hang or read past the end.
    CHECK(LatexDoc::parse("\\section{unclosed").spans.size() == 1);
    CHECK(LatexDoc::parse("\\begin{itemize}\\item x").lists.size() == 1);
    CHECK(LatexDoc::parse("$x").spans.size() <= 1);
    CHECK(LatexDoc::parse("").spans.empty());
    CHECK(LatexDoc::parse("\\").spans.empty());
}

// ------------------------------------------------------------------ synctex

// Two boxes on page 1: source line 7 near the top, line 12 lower down.
// Values are scaled points (1pt = 65536sp); y is the baseline of the box.
const char *kSampleSyncTex =
    "SyncTeX Version:1\n"
    "Input:1:/tmp/doc.tex\n"
    "Input:2:/tmp/other.tex\n"
    "Output:pdf\n"
    "Magnification:1000\n"
    "Unit:1\n"
    "X Offset:0\n"
    "Y Offset:0\n"
    "Content:\n"
    "!753\n"
    "{1\n"
    "[1,7:4718592,6553600:19660800,655360,0\n"   // x=72 y=100 w=300 h=10
    "h1,7:4718592,6553600:19660800,655360,0\n"
    "[1,12:4718592,19660800:13107200,655360,0\n" // x=72 y=300 w=200 h=10
    "]\n"
    "]\n"
    "}1\n"
    "Postamble:\n";

void testSyncTex() {
    SyncTexIndex idx = SyncTexIndex::parse(kSampleSyncTex);

    GROUP("synctex:parse");
    CHECK(idx.valid());
    CHECK(idx.recordCount() == 3);
    CHECK(idx.pathForTag(1) == "/tmp/doc.tex");
    CHECK(idx.tagForPath("/tmp/doc.tex") == 1);
    CHECK(idx.tagForPath("doc.tex") == 1);          // matched by file name
    CHECK(idx.tagForPath("/elsewhere/other.tex") == 2);
    CHECK(idx.tagForPath("missing.tex") == 0);

    GROUP("synctex:hit");
    // A point inside the first box resolves to its line.
    std::vector<SyncTexHit> hits = idx.hitsAtPoint(1, 100, 95);
    CHECK(!hits.empty());
    CHECK(!hits.empty() && hits[0].line == 7);
    CHECK(!hits.empty() && hits[0].distance == 0.0);
    CHECK(!hits.empty() && hits[0].tag == 1);
    // The box is reported in points, measured from the top-left of the page.
    CHECK(!hits.empty() && std::abs(hits[0].x - 72.0) < 0.01);
    CHECK(!hits.empty() && std::abs(hits[0].width - 300.0) < 0.01);
    CHECK(!hits.empty() && std::abs(hits[0].y - 90.0) < 0.01);

    // Inside the second box.
    hits = idx.hitsAtPoint(1, 100, 295);
    CHECK(!hits.empty() && hits[0].line == 12);

    // Between them: nothing contains the point, so the nearest box wins, and
    // the other line is still offered as a candidate.
    hits = idx.hitsAtPoint(1, 100, 260);
    CHECK(hits.size() == 2);
    CHECK(!hits.empty() && hits[0].line == 12);
    CHECK(hits.size() > 1 && hits[1].line == 7);

    // One hit per source line, and never from another page.
    CHECK(idx.hitsAtPoint(1, 100, 95).size() == 2);
    CHECK(idx.hitsAtPoint(2, 100, 95).empty());

    GROUP("synctex:robustness");
    CHECK(!SyncTexIndex::parse("").valid());
    CHECK(!SyncTexIndex::parse("SyncTeX Version:1\nContent:\n{1\nnonsense\n").valid());
    CHECK(SyncTexIndex::parse(kSampleSyncTex).hitsAtPoint(1, 0, 0, 1).size() == 1);
}

}  // namespace

int main() {
    std::printf("Running MiniCode core tests...\n");
    testSyntax();
    testMarkdown();
    testTerminalStream();
    testTerminalScreen();
    testSettings();
    testSettingsColorEditing();
    testLineComments();
    testLatexDoc();
    testSyncTex();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
