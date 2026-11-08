// snippet_tests.inc — the `snippet:*` groups of run_tests.cpp: the LSP
// snippet grammar (Snippet::parse), expansion with variables, mirrors,
// indentation and line breaks (Snippet::expand), regex transforms, and the
// tab stops following edits (Snippet::Session). Included there.
#include "Snippet.h"

namespace {

std::string snipU8(const std::u16string &s) {
    std::string out;
    for (char16_t c : s) {
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// The expansion as text with each stop's ranges marked: [1:i] for a
// range of stop 1, {0} for the empty final stop.
std::string snipShown(const Snippet::Expansion &e) {
    struct Mark { size_t at; int order; std::string text; };
    std::vector<Mark> marks;
    int k = 0;
    for (const Snippet::Stop &s : e.stops)
        for (const Snippet::Range &r : s.ranges) {
            if (r.start == r.end) {
                marks.push_back({r.start, k++, "{" + std::to_string(s.index) + "}"});
            } else {
                marks.push_back({r.start, k++, "[" + std::to_string(s.index) + ":"});
                marks.push_back({r.end, -(k++), "]"});
            }
        }
    // Closers before openers at one place; otherwise as found.
    std::stable_sort(marks.begin(), marks.end(), [](const Mark &a, const Mark &b) {
        if (a.at != b.at) return a.at < b.at;
        return (a.order < 0) > (b.order < 0);
    });
    std::string out;
    size_t m = 0;
    for (size_t i = 0; i <= e.text.size(); i++) {
        while (m < marks.size() && marks[m].at == i) out += marks[m++].text;
        if (i < e.text.size()) out += snipU8(e.text.substr(i, 1));
    }
    return out;
}

Snippet::Context snipContext() {
    Snippet::Context c;
    c.filePath = "/work/proj/src/main.py";
    c.workspaceFolder = "/work/proj";
    c.currentLine = "    x = foo";
    c.currentWord = "foo";
    c.lineIndex = 41;
    c.clipboard = "copied";
    c.lineComment = "#";
    c.blockCommentStart = c.blockCommentEnd = "\"\"\"";
    c.time.tm_year = 126;   // 2026
    c.time.tm_mon = 10;     // November
    c.time.tm_mday = 8;
    c.time.tm_wday = 0;     // Sunday
    c.time.tm_hour = 9;
    c.time.tm_min = 5;
    c.time.tm_sec = 7;
    c.unixSeconds = 1794042307;
    c.timezoneOffsetSeconds = -5 * 3600 - 30 * 60;
    uint32_t seed = 1;
    c.random = [seed]() mutable { seed = seed * 1664525u + 1013904223u; return seed; };
    return c;
}

std::string expandShown(const std::string &snippet,
                        const Snippet::Context &ctx = snipContext()) {
    return snipShown(Snippet::expand(snippet, ctx));
}

std::string plainOf(const std::string &snippet) {
    return snipU8(Snippet::expand(snippet, snipContext()).text);
}

}  // namespace

static void testSnippets() {
    using namespace Snippet;

    GROUP("snippet:parse");
    {
        std::vector<Node> n = parse("for ${1:i} in ${2:range(10)}:\n\t$0");
        CHECK(n.size() == 6);
        CHECK(n[0].kind == Node::Kind::Text && n[0].text == "for ");
        CHECK(n[1].kind == Node::Kind::Placeholder && n[1].index == 1 &&
              n[1].children.size() == 1 && n[1].children[0].text == "i");
        CHECK(n[3].kind == Node::Kind::Placeholder && n[3].index == 2);
        CHECK(n[5].kind == Node::Kind::Tabstop && n[5].index == 0);
        // Every form of a tab stop and variable.
        n = parse("$1${2}$NAME${NAME}${NAME:def}${3|a,b|}");
        CHECK(n.size() == 6);
        CHECK(n[0].kind == Node::Kind::Tabstop && n[0].index == 1);
        CHECK(n[1].kind == Node::Kind::Tabstop && n[1].index == 2);
        CHECK(n[2].kind == Node::Kind::Variable && n[2].text == "NAME" && !n[2].hasDefault);
        CHECK(n[3].kind == Node::Kind::Variable && n[3].text == "NAME");
        CHECK(n[4].kind == Node::Kind::Variable && n[4].hasDefault &&
              n[4].children.size() == 1 && n[4].children[0].text == "def");
        CHECK(n[5].kind == Node::Kind::Choice && n[5].index == 3 &&
              n[5].choices == std::vector<std::string>({"a", "b"}));
        // Nested placeholders.
        n = parse("${1:outer ${2:inner ${3:deep}}}");
        CHECK(n.size() == 1 && n[0].children.size() == 2);
        CHECK(n[0].children[1].kind == Node::Kind::Placeholder && n[0].children[1].index == 2);
        CHECK(n[0].children[1].children[1].index == 3);
        // Escapes: \$ \} \\ in text; \, \| in a choice; other backslashes stay.
        CHECK(plainText(parse("a\\$1 \\} \\\\ \\n")) == "a$1 } \\ \\n");
        CHECK(parse("${1|x\\,y,z\\|w|}")[0].choices ==
              std::vector<std::string>({"x,y", "z|w"}));
        CHECK(plainText(parse("${1:a\\}b}")) == "a}b");
        // Malformed: as written.
        CHECK(plainText(parse("${1:open")) == "${1:open");
        CHECK(plainText(parse("${1|a,b}")) == "${1|a,b}");
        CHECK(plainText(parse("$ and ${ and ${}")) == "$ and ${ and ${}");
        CHECK(plainText(parse("${1:a ${2:b}")) == "${1:a b");
        CHECK(parse("${1:a ${2:b}")[1].kind == Node::Kind::Placeholder);
        CHECK(plainText(parse("price: $5.00")) == "price: .00");   // $5 is a tab stop
        CHECK(plainText(parse("${-1}")) == "${-1}");
        CHECK(plainText(parse("}")) == "}");
        CHECK(plainText(parse("")) == "");
        // Transforms.
        n = parse("${TM_FILENAME/(.*)\\.py$/${1:/upcase}/}");
        CHECK(n.size() == 1 && n[0].transform);
        CHECK(n[0].transform->regex == "(.*)\\.py$");
        CHECK(n[0].transform->format.size() == 1 &&
              n[0].transform->format[0].kind == FormatItem::Kind::Upcase);
        n = parse("${1/a\\/b/x${1:+yes}${2:?t:f}${3:-no}${4:other}$5\\/z/gi}");
        CHECK(n.size() == 1 && n[0].kind == Node::Kind::Tabstop && n[0].transform);
        const Transform &t = *n[0].transform;
        CHECK(t.regex == "a/b" && t.options == "gi");
        CHECK(t.format.size() == 7);
        CHECK(t.format[1].hasIf && !t.format[1].hasElse && t.format[1].ifText == "yes");
        CHECK(t.format[2].hasIf && t.format[2].hasElse && t.format[2].elseText == "f");
        CHECK(!t.format[3].hasIf && t.format[3].elseText == "no");
        CHECK(t.format[4].elseText == "other");
        CHECK(t.format[5].kind == FormatItem::Kind::Group && t.format[5].group == 5);
        CHECK(t.format[6].text == "/z");
        // An unclosed transform is text.
        CHECK(plainText(parse("${1/a/b}")) == "${1/a/b}");
    }

    GROUP("snippet:expand");
    {
        Context flat = snipContext();
        CHECK(expandShown("for ${1:i} in ${2:range(10)}:\n\t$0", flat) ==
              "for [1:i] in [2:range(10)]:\n\t{0}");
        // No $0: the final stop goes at the end.
        CHECK(expandShown("foo($1)") == "foo({1}){0}");
        CHECK(expandShown("plain") == "plain{0}");
        CHECK(!expand("plain", flat).hasStops());
        CHECK(expand("f($1)", flat).hasStops());
        // Stops in index order, then the final.
        Expansion e = expand("${2:b} ${1:a} $0 ${3:c}", flat);
        CHECK(e.stops.size() == 4);
        CHECK(e.stops[0].index == 1 && e.stops[1].index == 2 && e.stops[2].index == 3 &&
              e.stops[3].index == 0);
        // Mirrors take the placeholder's text, before or after it.
        CHECK(expandShown("$1 = ${1:name}; use($1)") == "[1:name] = [1:name]; use([1:name]){0}");
        e = expand("$1 = ${1:name}", flat);
        CHECK(e.stops[0].ranges.size() == 2);
        // Nested: the outer range holds the inner.
        CHECK(expandShown("${1:a ${2:b}}") == "[1:a [2:b]]{0}");
        // A choice shows its first option.
        e = expand("${1|one,two,three|}", flat);
        CHECK(snipU8(e.text) == "one");
        CHECK(e.stops[0].choices.size() == 3 && snipU8(e.stops[0].choices[2]) == "three");
        // Variables.
        CHECK(plainOf("$TM_FILENAME ${TM_FILENAME_BASE} $TM_DIRECTORY") ==
              "main.py main /work/proj/src");
        CHECK(plainOf("$TM_FILEPATH $RELATIVE_FILEPATH") ==
              "/work/proj/src/main.py src/main.py");
        CHECK(plainOf("$WORKSPACE_NAME $WORKSPACE_FOLDER") == "proj /work/proj");
        CHECK(plainOf("$TM_LINE_INDEX $TM_LINE_NUMBER $TM_CURRENT_WORD|$TM_CURRENT_LINE") ==
              "41 42 foo|    x = foo");
        CHECK(plainOf("$CLIPBOARD $LINE_COMMENT $BLOCK_COMMENT_START") == "copied # \"\"\"");
        CHECK(plainOf("$CURRENT_YEAR $CURRENT_YEAR_SHORT $CURRENT_MONTH $CURRENT_DATE") ==
              "2026 26 11 08");
        CHECK(plainOf("$CURRENT_MONTH_NAME $CURRENT_MONTH_NAME_SHORT") == "November Nov");
        CHECK(plainOf("$CURRENT_DAY_NAME $CURRENT_DAY_NAME_SHORT") == "Sunday Sun");
        CHECK(plainOf("$CURRENT_HOUR:$CURRENT_MINUTE:$CURRENT_SECOND") == "09:05:07");
        CHECK(plainOf("$CURRENT_SECONDS_UNIX $CURRENT_TIMEZONE_OFFSET") == "1794042307 -05:30");
        CHECK(plainOf("$RANDOM").size() == 6 && plainOf("$RANDOM_HEX").size() == 6);
        const std::string uuid = plainOf("$UUID");
        CHECK(uuid.size() == 36 && uuid[8] == '-' && uuid[14] == '4');
        // An empty value takes the default; an unknown name becomes a
        // placeholder holding its name.
        CHECK(expandShown("${TM_SELECTED_TEXT:${1:pick}}") == "[1:pick]{0}");
        CHECK(plainOf("[$TM_SELECTED_TEXT]") == "[]");
        CHECK(expandShown("${1:a} $NOPE") == "[1:a] [2:NOPE]{0}");
        CHECK(expandShown("${NOPE:dflt}") == "[1:dflt]{0}");
        // Variable transforms.
        CHECK(plainOf("${TM_FILENAME/(.*)\\.py$/${1:/upcase}/}") == "MAIN");
        CHECK(plainOf("${TM_FILENAME_BASE/(.*)/${1:/capitalize}Test/}") == "MainTest");
        // Indentation: every line after the first gets the line's, a tab at
        // a line's start the unit, and line breaks the file's.
        Context py = snipContext();
        py.indent = u"    ";
        py.unit = u"    ";
        CHECK(expandShown("if ${1:x}:\n\t${2:pass}\n$0", py) ==
              "if [1:x]:\n        [2:pass]\n    {0}");
        Context crlf = snipContext();
        crlf.eol = u"\r\n";
        crlf.indent = u"\t";
        CHECK(expandShown("{\n\t$0\n}", crlf) == "{\r\n\t\t{0}\r\n\t}");
        CHECK(expandShown("a\r\nb", crlf) == "a\r\n\tb{0}");
        // A tab inside a line stays a tab; a mirror's text is indented too.
        CHECK(expandShown("a\tb\n\tc", py) == "a\tb\n        c{0}");
        CHECK(snipU8(expand("${1:x\ny} $1", py).text) == "x\n    y x\n    y");
        // Non-ASCII text counts in UTF-16 units.
        e = expand("é${1:ü}€", flat);
        CHECK(e.stops[0].ranges[0].start == 1 && e.stops[0].ranges[0].end == 2);
        // A mirror loop does not run forever.
        CHECK(snipU8(expand("${1:$2} ${2:$1}", flat).text).size() < 100);
    }

    GROUP("snippet:transform");
    {
        auto tr = [](const std::string &spec, const std::string &value) {
            std::vector<Node> n = parse("${1" + spec + "}");
            if (n.empty() || !n[0].transform) return std::string("(no transform)");
            std::u16string v(value.begin(), value.end());
            return snipU8(applyTransform(*n[0].transform, v));
        };
        CHECK(tr("/(\\w+)/<$1>/", "ab cd") == "<ab> cd");
        CHECK(tr("/(\\w+)/<$1>/g", "ab cd") == "<ab> <cd>");
        CHECK(tr("/AB/x/i", "zabz") == "zxz");
        CHECK(tr("/AB/x/", "zabz") == "zabz");
        CHECK(tr("/(.*)/${1:/upcase}/", "mixed Case") == "MIXED CASE");
        CHECK(tr("/(.*)/${1:/downcase}/", "MiXeD") == "mixed");
        CHECK(tr("/(.*)/${1:/capitalize}/", "word here") == "Word here");
        CHECK(tr("/(.*)/${1:/camelcase}/", "some-file_name") == "someFileName");
        CHECK(tr("/(.*)/${1:/pascalcase}/", "some-file_name") == "SomeFileName");
        CHECK(tr("/(a)?b/${1:+had a}/", "ab") == "had a");
        CHECK(tr("/(a)?b/${1:+had a}/", "b") == "");
        CHECK(tr("/(a)?b/${1:?yes:no}/", "b") == "no");
        CHECK(tr("/(a)?b/${1:-none}/", "ab") == "a");
        CHECK(tr("/(a)?b/${1:-none}/", "b") == "none");
        CHECK(tr("/(a)?b/${1:none}/", "b") == "none");
        CHECK(tr("/x*/-/g", "ab") == "-a-b-");
        CHECK(tr("/\\./\\//g", "a.b.c") == "a/b/c");
        // A pattern the linear engine refuses leaves the value as it is.
        CHECK(tr("/(a)\\1/x/", "aa") == "aa");
    }

    GROUP("snippet:stops-after-edit");
    {
        Context flat = snipContext();
        // "x = for i in range(10):\n\t" with the snippet at 4.
        Expansion e = expand("for ${1:i} in ${2:range(10)}:\n\t$0", flat);
        Session s(e, 4);
        CHECK(s.active() && s.current() == 0);
        CHECK(s.caretRanges().size() == 1 && s.caretRanges()[0] == (Range{8, 9}));
        // Typing over the first stop: it holds what was typed, and grows.
        s.edited(8, 1, 1);   // "i" -> "k"
        CHECK(s.caretRanges()[0] == (Range{8, 9}));
        s.edited(9, 0, 2);   // "k" + "ey"
        CHECK(s.caretRanges()[0] == (Range{8, 11}));
        CHECK(s.holds({{11, 11}}) && s.holds({{8, 8}}) && !s.holds({{12, 12}}));
        // Stop 2 moved along.
        CHECK(s.stops()[1].ranges[0] == (Range{15, 24}));
        CHECK(s.next() && s.current() == 1);
        CHECK(s.caretRanges()[0] == (Range{15, 24}));
        // Typing at the end of stop 2 grows it; the final stop after the
        // colon moves along.
        const Range finalBefore = s.stops()[2].ranges[0];
        s.edited(24, 0, 1);
        CHECK(s.caretRanges()[0] == (Range{15, 25}));
        CHECK(s.stops()[2].ranges[0].start == finalBefore.start + 1);
        // An edit before the snippet moves it all; one after leaves it.
        s.edited(0, 0, 3);
        CHECK(s.caretRanges()[0] == (Range{18, 28}));
        s.edited(100, 0, 5);
        CHECK(s.caretRanges()[0] == (Range{18, 28}));
        CHECK(s.next() && s.atFinal());
        CHECK(!s.next());
        CHECK(s.prev() && s.current() == 1);
        CHECK(s.prev() && s.current() == 0);
        CHECK(!s.prev() && s.current() == 0);

        // An inactive stop never grows at its edges: typing at the end of
        // stop 1 right before stop 2 leaves stop 2 out of it.
        e = expand("${1:a}${2:b}", flat);
        s = Session(e, 0);
        s.edited(1, 0, 1);
        CHECK(s.stops()[0].ranges[0] == (Range{0, 2}));
        CHECK(s.stops()[1].ranges[0] == (Range{2, 3}));
        // The final stop right after the stop being typed in follows it.
        e = expand("$1$0", flat);
        s = Session(e, 10);
        s.edited(10, 0, 3);
        CHECK(s.stops()[0].ranges[0] == (Range{10, 13}));
        CHECK(s.stops()[1].ranges[0] == (Range{13, 13}));
        // Typing over a placeholder skips the one nested in it, and the
        // outer grows as the inner is typed in.
        e = expand("${1:a ${2:b}} ${3:c}", flat);
        s = Session(e, 0);
        s.edited(0, 3, 1);   // "a b" -> "x"
        CHECK(s.dead(1) && !s.dead(0) && s.active());
        CHECK(s.caretRanges()[0] == (Range{0, 1}));
        CHECK(s.next() && s.stops()[s.current()].index == 3);
        e = expand("${1:a ${2:b}} ${3:c}", flat);
        s = Session(e, 0);
        CHECK(s.next() && s.current() == 1);
        s.edited(3, 0, 2);   // "b" + "yy"
        CHECK(s.stops()[1].ranges[0] == (Range{2, 5}));
        CHECK(s.stops()[0].ranges[0] == (Range{0, 5}));
        CHECK(s.stops()[2].ranges[0] == (Range{6, 7}));
        // ${1:${2:x}}: typing over 1 leaves 2 dead (same range, nested).
        e = expand("${1:${2:x}}", flat);
        s = Session(e, 0);
        s.edited(0, 1, 1);
        CHECK(s.dead(1) && !s.dead(0));
        // Mirrors: each follows its own typing.
        e = expand("${1:n} = $1", flat);
        s = Session(e, 0);
        CHECK(s.caretRanges().size() == 2);
        s.edited(4, 1, 3);   // the mirror, last first as replaceRanges does
        s.edited(0, 1, 3);
        CHECK(s.caretRanges()[0] == (Range{0, 3}) && s.caretRanges()[1] == (Range{6, 9}));
        CHECK(s.holds({{3, 3}, {9, 9}}));
        // Backspace to empty and typing again.
        e = expand("f(${1:x})", flat);
        s = Session(e, 0);
        s.edited(2, 1, 0);
        CHECK(s.caretRanges()[0] == (Range{2, 2}));
        s.edited(2, 0, 2);
        CHECK(s.caretRanges()[0] == (Range{2, 4}));
        // A replacement bigger than the snippet ends the session.
        s.edited(0, 6, 1);
        CHECK(!s.active());
        // A selection of more than the stop, typed over, ends it too.
        e = expand("f(${1:x}) + ${2:y}", flat);
        s = Session(e, 5);
        s.edited(6, 3, 1);
        CHECK(!s.active());
        // Transformed copies: written from the stop's text.
        e = expand("${1:name} ${1/(.*)/${1:/upcase}/}", flat);
        s = Session(e, 0);
        CHECK(s.caretRanges().size() == 1);
        std::vector<Session::Replacement> reps = s.transformsFor(u"value");
        CHECK(reps.size() == 1 && reps[0].start == 5 && snipU8(reps[0].text) == "VALUE");
    }
}

static void testSnippetCompletionItems() {
    GROUP("snippet:completion-items");
    // insertTextFormat 2 marks a snippet; the list's itemDefaults stand in
    // for an item's own field; insertTextMode 1 keeps the text as it is.
    Json list;
    CHECK(Json::parse(R"({"isIncomplete": false,
        "itemDefaults": {"insertTextFormat": 2},
        "items": [
          {"label": "for", "insertText": "for ${1:i} in ${2:x}:\n\t$0"},
          {"label": "plain", "insertText": "plain", "insertTextFormat": 1},
          {"label": "asis", "insertText": "a\n$0", "insertTextMode": 1}]})", list));
    std::vector<Lsp::CompletionItem> items = Lsp::parseCompletion(list);
    CHECK(items.size() == 3);
    for (const Lsp::CompletionItem &it : items) {
        if (it.label == "for") CHECK(it.isSnippet && !it.insertAsIs);
        if (it.label == "plain") CHECK(!it.isSnippet);
        if (it.label == "asis") CHECK(it.isSnippet && it.insertAsIs);
    }
    Json bare;
    CHECK(Json::parse(R"j([{"label": "f", "insertText": "f($1)"}])j", bare));
    CHECK(!Lsp::parseCompletion(bare)[0].isSnippet);
}
