// run_tests.cpp — unit tests for the Linux port's pure-C++ pieces (no GTK, no
// framework, no deps). Build/run with `make test` from linux/, or
// `meson test -C build`. Exits non-zero if any check fails.
//
// What this covers is the byte-offset -> character-offset conversion in
// Utf8Offsets.h. SyntaxHighlighter speaks byte offsets and GtkTextBuffer speaks
// character offsets, so that conversion decides whether syntax colors land on
// the right spans. It is the one part of the GTK editor that can be tested
// without a display, so it is tested hard, including against real token streams
// from the shared lexer.
#include "Utf8Offsets.h"
#include "SyntaxHighlighter.h"
#include "ThemeCss.h"
#include "LineComments.h"
#include "PageWords.h"
#include "LatexDoc.h"
#include "SyncTex.h"
#include "TermLinkPath.h"
#include "TermLinks.h"
#include "WebImageRules.h"
#include "LaunchOptions.h"
#include <algorithm>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
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

namespace {

// A deliberately naive reference implementation to check the cursor against:
// walk the whole string from the start, counting character boundaries.
long refCharOffset(const std::string &s, long byte) {
    long chars = 0;
    for (long i = 0; i < byte && i < (long)s.size(); ++i)
        if (((unsigned char)s[i] & 0xC0) != 0x80) ++chars;
    return chars;
}

// The characters of a UTF-8 string, one string per character. Lets a test
// reconstruct a span from CHARACTER offsets the way GtkTextBuffer would.
std::vector<std::string> chars(const std::string &s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        size_t len = 1;
        unsigned char c = (unsigned char)s[i];
        if (c >= 0xF0)      len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        if (i + len > s.size()) len = s.size() - i;
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}

std::string joinChars(const std::vector<std::string> &cs, long from, long to) {
    std::string out;
    for (long i = from; i < to && i < (long)cs.size(); ++i) out += cs[i];
    return out;
}

} // namespace

// ------------------------------------------------------------ basic conversion
static void testAscii() {
    GROUP("utf8/ascii");
    std::string s = "hello world";
    Utf8OffsetCursor c(s);
    // Pure ASCII: byte offsets and character offsets are identical.
    for (long i = 0; i <= (long)s.size(); ++i) CHECK(c.charOffset(i) == i);
    Utf8OffsetCursor c2(s);
    CHECK(c2.totalChars() == 11);
}

static void testTwoByte() {
    GROUP("utf8/two-byte");
    // "héllo": h(1) é(2) l(1) l(1) o(1) = 6 bytes, 5 characters.
    std::string s = "h\xC3\xA9llo";
    CHECK(s.size() == 6);
    Utf8OffsetCursor c(s);
    CHECK(c.charOffset(0) == 0);
    CHECK(c.charOffset(1) == 1);   // start of é
    CHECK(c.charOffset(3) == 2);   // just past é
    CHECK(c.charOffset(4) == 3);
    CHECK(c.charOffset(6) == 5);
    Utf8OffsetCursor c2(s);
    CHECK(c2.totalChars() == 5);
}

static void testThreeByte() {
    GROUP("utf8/three-byte");
    // "日本語": 3 characters, 9 bytes.
    std::string s = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";
    CHECK(s.size() == 9);
    Utf8OffsetCursor c(s);
    CHECK(c.charOffset(0) == 0);
    CHECK(c.charOffset(3) == 1);
    CHECK(c.charOffset(6) == 2);
    CHECK(c.charOffset(9) == 3);
}

static void testFourByte() {
    GROUP("utf8/four-byte");
    // "a😀b": a(1) emoji(4) b(1) = 6 bytes, 3 characters. GtkTextBuffer counts
    // an astral character as ONE character (unlike the macOS build's UTF-16
    // offsets, where it is two units) — this is the difference that would
    // silently shift every color after an emoji.
    std::string s = "a\xF0\x9F\x98\x80" "b";
    CHECK(s.size() == 6);
    Utf8OffsetCursor c(s);
    CHECK(c.charOffset(0) == 0);
    CHECK(c.charOffset(1) == 1);   // start of the emoji
    CHECK(c.charOffset(5) == 2);   // just past the emoji
    CHECK(c.charOffset(6) == 3);
}

static void testEdges() {
    GROUP("utf8/edges");
    std::string empty;
    Utf8OffsetCursor e(empty);
    CHECK(e.charOffset(0) == 0);
    CHECK(e.charOffset(50) == 0);      // clamps past the end
    CHECK(e.charOffset(-3) == 0);      // clamps before the start

    std::string s = "\xE6\x97\xA5" "abc";   // 日abc: 6 bytes, 4 characters
    Utf8OffsetCursor c(s);
    CHECK(c.charOffset(100) == 4);     // clamped to the end
    CHECK(c.charOffset(3) == 1);       // a backward request still resolves
    CHECK(c.charOffset(0) == 0);
    CHECK(c.charOffset(6) == 4);       // and forward again afterwards
}

static void testMatchesReference() {
    GROUP("utf8/vs-reference");
    // Mixed scripts, emoji, and ASCII interleaved. Every ascending offset must
    // agree with the naive full-rescan implementation.
    std::string s = "int x = 1; // caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC "
                    "\xF0\x9F\x98\x80 done";
    Utf8OffsetCursor c(s);
    bool allMatch = true;
    for (long i = 0; i <= (long)s.size(); ++i)
        if (c.charOffset(i) != refCharOffset(s, i)) allMatch = false;
    CHECK(allMatch);
}

// ------------------------------------------- real token streams from the lexer
// The end-to-end check that matters: take source containing multi-byte text,
// run the actual highlighter, convert each token's byte span to a character
// span, and confirm the character span selects exactly the same text. If the
// conversion were wrong, colors would land on shifted spans.
static void testTokenSpansRoundTrip(const std::string &src,
                                    const std::string &ext) {
    std::vector<Token> tokens = SyntaxHighlighter::highlight(src, ext);
    std::vector<std::string> cs = chars(src);
    Utf8OffsetCursor cur(src);
    bool allMatch = true;
    bool ascending = true;
    long prevStart = -1;
    for (const Token &t : tokens) {
        if ((long)t.start < prevStart) ascending = false;
        prevStart = (long)t.start;
        long cstart = cur.charOffset((long)t.start);
        long cend = cur.charOffset((long)(t.start + t.length));
        if (joinChars(cs, cstart, cend) != src.substr(t.start, t.length))
            allMatch = false;
    }
    CHECK(tokens.size() > 0);
    // The forward-only cursor is only valid because the lexer emits tokens in
    // ascending order. Assert that, so the optimization cannot silently rot.
    CHECK(ascending);
    CHECK(allMatch);
}

static void testRealSources() {
    GROUP("lexer/utf8-spans");
    // C++ with accented and CJK text inside strings and comments.
    testTokenSpansRoundTrip(
        "// caf\xC3\xA9 na\xC3\xAFve\n"
        "#include <string>\n"
        "int main() {\n"
        "    const char* s = \"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\";\n"
        "    int n = 42;  // \xF0\x9F\x98\x80 emoji comment\n"
        "    return 0;\n"
        "}\n", "cpp");

    // Python with an emoji in a string, decorators, and numbers.
    testTokenSpansRoundTrip(
        "@decorator\n"
        "def greet(name):\n"
        // \xBC must end the literal: otherwise "be" is swallowed as more hex
        // digits and the escape silently becomes something else.
        "    msg = \"hi \xF0\x9F\x91\x8B \xC3\xBC" "ber\"\n"
        "    count = 0xFF\n"
        "    return msg  # \xE6\xB3\xA8\xE9\x87\x88\n", "py");

    // Pure ASCII source must round-trip too (the common case).
    testTokenSpansRoundTrip(
        "function add(a, b) {\n"
        "    // sum them\n"
        "    return a + b;  /* done */\n"
        "}\n", "js");
}

// ------------------------------------------------------------------------ main
// ------------------------------------------------ UTF-16 <-> characters
void testUtf16() {
    GROUP("utf16:decode-encode");
    CHECK(utf16::fromUtf8("abc") == u"abc");
    CHECK(utf16::fromUtf8("h\xC3\xA9llo") == u"héllo");           // é
    CHECK(utf16::fromUtf8("\xE4\xB8\xAD") == u"中");                 // 中
    CHECK(utf16::fromUtf8("\xF0\x9F\x98\x80") == u"\U0001F600");         // 😀, a pair
    CHECK(utf16::fromUtf8("\xF0\x9F\x98\x80").size() == 2);
    CHECK(utf16::fromUtf8("a\xFF" "b") == u"a�" u"b");              // bad byte
    CHECK(utf16::fromUtf8("\xE4\xB8") == u"��");               // truncated
    const std::string mixed = "x \xC3\xA9 \xE4\xB8\xAD \xF0\x9F\x98\x80 end";
    CHECK(utf16::toUtf8(utf16::fromUtf8(mixed)) == mixed);                // round trip
    CHECK(utf16::toUtf8(u"\xD800") == "\xEF\xBF\xBD");                    // lone surrogate

    GROUP("utf16:offsets");
    const std::u16string t = u"a\U0001F600béc";   // a, 😀 (2 units), b, é, c
    CHECK(utf16::toCharOffset(t, 0) == 0);
    CHECK(utf16::toCharOffset(t, 1) == 1);
    CHECK(utf16::toCharOffset(t, 2) == 1);   // between the halves: the emoji's start
    CHECK(utf16::toCharOffset(t, 3) == 2);
    CHECK(utf16::toCharOffset(t, 4) == 3);
    CHECK(utf16::toCharOffset(t, 6) == 5);
    CHECK(utf16::toCharOffset(t, 99) == 5);  // clamped
    CHECK(utf16::fromCharOffset(t, 0) == 0);
    CHECK(utf16::fromCharOffset(t, 1) == 1);
    CHECK(utf16::fromCharOffset(t, 2) == 3);
    CHECK(utf16::fromCharOffset(t, 5) == 6);
    CHECK(utf16::fromCharOffset(t, 99) == 6);
    for (long c = 0; c <= 5; ++c)
        CHECK(utf16::toCharOffset(t, utf16::fromCharOffset(t, c)) == c);

    GROUP("utf16:comment-toggle-through-gtk-offsets");
    // What Editor::toggleComment does: GTK character offsets in, through the
    // UTF-16 core, character offsets out.
    const std::string buf = "\xF0\x9F\x98\x80 = 1\nx = 2";   // "😀 = 1\nx = 2"
    std::u16string u = utf16::fromUtf8(buf);
    long caretChars = 5;                                     // end of line 1
    size_t caret = utf16::fromCharOffset(u, caretChars);
    CHECK(caret == 6);
    LineComments::Result r = LineComments::toggle(u, caret, caret, "#");
    CHECK(utf16::toUtf8(r.text) == "# \xF0\x9F\x98\x80 = 1\nx = 2");
    CHECK(utf16::toCharOffset(r.text, r.selStart) == 7);     // caret kept its place
    CHECK(utf16::toCharOffset(u, r.replaceStart) == 0);
    CHECK(utf16::toCharOffset(u, r.replaceStart + r.replaceLength) == 5);
}

// ------------------------------------------------------------- theme css
bool has(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

void testThemeCss() {
    GROUP("theme:css-color");
    CHECK(theme::cssColor(Rgba::hex(0x1E1E1E)) == "rgba(30,30,30,1.000)");
    CHECK(theme::cssColor(Rgba::hex(0x007ACC, 0.5)) == "rgba(0,122,204,0.500)");
    CHECK(theme::cssColor(Rgba::hex(0xFFFFFF, 0.0)) == "rgba(255,255,255,0.000)");
    CHECK(theme::cssColor(Rgba::hex(0, 0.0625)) == "rgba(0,0,0,0.063)");
    CHECK(theme::cssColor(Rgba::hex(0, 0.25 * (128 / 255.0))) == "rgba(0,0,0,0.125)");

    GROUP("theme:defaults");
    const std::string d = theme::stylesheet(Settings::parse(""));
    CHECK(has(d, ".minicode-editor text { background-color: rgba(30,30,30,1.000); color: rgba(212,212,212,1.000); }"));
    CHECK(has(d, ".minicode-sidebar { background-color: rgba(37,37,38,1.000); }"));
    CHECK(has(d, ".minicode-tree-label { color: rgba(204,204,204,1.000); }"));
    CHECK(has(d, ".minicode-status { background-color: rgba(0,122,204,1.000)"));
    CHECK(has(d, ".minicode-browser-bar { background-color: rgba(42,42,42,1.000)"));
    CHECK(!has(d, "background-color: transparent; }window"));   // sanity
    CHECK(!has(d, "window.minicode-window.background"));        // opaque window
    CHECK(!has(d, "menubar"));                                  // theme's title bar
    CHECK(!has(d, ".minicode-browser-bar entry"));
    // A translucent color must be painted exactly once per panel.
    CHECK(has(d, ".minicode-editor { background-color: transparent; color: rgba(212,212,212,1.000); }"));
    CHECK(has(d, ".minicode-tree, .minicode-tree > row { background-color: transparent; }"));
    CHECK(has(d, ".minicode-terminal { background-color: transparent; }"));
    // No locale-dependent decimal commas anywhere.
    CHECK(!has(d, ",000)") || has(d, "1.000)"));

    GROUP("theme:transparency");
    const std::string t = theme::stylesheet(Settings::parse(
        "editor.opacity = 0.5\nsidebar.opacity = 25%\nstatusbar.text = #000\n"
        "browser.text = #ABCDEF\n"));
    CHECK(has(t, ".minicode-editor text { background-color: rgba(30,30,30,0.500)"));
    CHECK(has(t, ".minicode-sidebar { background-color: rgba(37,37,38,0.250); }"));
    CHECK(has(t, "window.minicode-window.background { background-color: transparent; }"));
    CHECK(has(t, ".minicode-status label { color: rgba(0,0,0,1.000); }"));
    CHECK(has(t, ".minicode-browser-bar entry { color: rgba(171,205,239,1.000); }"));
    // A see-through window takes over the title and menu bar too.
    CHECK(has(t, "window.minicode-window menubar {"));
    CHECK(has(t, "background-color: rgba(50,50,51,1.000); background-image: none;"));

    GROUP("theme:titlebar");
    const std::string tb = theme::stylesheet(Settings::parse(
        "titlebar.opacity = 0.2\ntitlebar.text = #FF0000\n"));
    CHECK(has(tb, "background-color: rgba(50,50,51,0.200); background-image: none;"
                  " box-shadow: none; color: rgba(255,0,0,1.000); }"));
    CHECK(has(tb, "window.minicode-window menubar > item, window.minicode-window"
                  " headerbar label { color: rgba(255,0,0,1.000); }"));
    CHECK(has(tb, "window.minicode-window.background { background-color: transparent; }"));

    GROUP("theme:light");
    // A light theme (View > Theme): its colors, and the selection and
    // scroll bars no key sets made light too; a dark one keeps Dark+'s.
    CHECK(has(d, ".minicode-editor text selection { background-color: #264F78; }"));
    CHECK(has(d, "scrollbar slider { background-color: rgba(255,255,255,0.22);"));
    const std::string lp = theme::stylesheet(Settings::parse("theme = light-plus\n"));
    CHECK(has(lp, ".minicode-editor text { background-color: rgba(255,255,255,1.000)"));
    CHECK(has(lp, ".minicode-editor text selection { background-color: #ADD6FF; }"));
    CHECK(has(lp, ".minicode-git-message text selection { background-color: #ADD6FF; }"));
    CHECK(has(lp, "scrollbar slider { background-color: rgba(0,0,0,0.22);"));
    CHECK(has(lp, "scrollbar slider:hover { background-color: rgba(0,0,0,0.38); }"));
    CHECK(!has(lp, "#264F78"));
    const std::string sl = theme::stylesheet(Settings::parse("theme = solarized-light\n"));
    CHECK(has(sl, ".minicode-sidebar { background-color: rgba(238,232,213,1.000); }"));
    CHECK(has(sl, ".minicode-editor text { background-color: rgba(253,246,227,1.000)"));
    const std::string mk = theme::stylesheet(Settings::parse("theme = monokai\n"));
    CHECK(has(mk, "#264F78") && !has(mk, "#ADD6FF"));

    GROUP("theme:text");
    const std::string tx = theme::stylesheet(Settings::parse("window.text = #00FF00\n"));
    CHECK(has(tx, "color: rgba(0,255,0,1.000); }"));
    CHECK(has(tx, ".minicode-tree-label { color: rgba(0,255,0,1.000); }"));
    CHECK(!has(tx, "window.minicode-window.background"));       // text alone: opaque
}

// ------------------------------------------------------------ PageWords

// Lay text out as poppler would report it: one box per character, 6 points
// wide and 10 tall, each line 14 points below the last; a newline gets a
// zero-width box at the end of its line.
static PageText layOut(const std::string &utf8) {
    std::vector<PageBox> boxes;
    double x = 72, y = 72;
    for (const std::string &ch : chars(utf8)) {
        PageBox b;
        if (ch == "\n") {
            b.x1 = b.x2 = x; b.y1 = y; b.y2 = y + 10;
            boxes.push_back(b);
            x = 72; y += 14;
            continue;
        }
        b.x1 = x; b.x2 = x + 6; b.y1 = y; b.y2 = y + 10;
        boxes.push_back(b);
        x += 6;
    }
    return PageText(utf8, boxes);
}

// The middle of character i's box.
static PageClick clickChar(const PageText &t, size_t i) {
    const PageBox &b = t.box(i);
    return t.clickAt((b.x1 + b.x2) / 2, (b.y1 + b.y2) / 2);
}

static void testPageWords() {
    GROUP("pagewords:basic");
    const PageText t = layOut("Hello, world of text\nsecond line");
    CHECK(t.size() == 32);
    CHECK(t.charAt(72 + 3, 77) == 0);
    CHECK(t.charAt(10, 10) == std::string::npos);          // the margin
    PageClick c = clickChar(t, 8);                            // the "o" of world
    CHECK(c.kind == PageClick::Kind::Word);
    CHECK(c.word == "world");
    CHECK(c.before == "Hello, ");
    CHECK(c.after == " of text\nsecond line");
    CHECK(c.start == 7 && c.end == 12);
    c = clickChar(t, 5);                                      // the comma
    CHECK(c.kind == PageClick::Kind::Line);
    CHECK(c.word == "Hello, world of text");
    c = clickChar(t, 6);                                      // the space
    CHECK(c.kind == PageClick::Kind::Line);
    CHECK(t.clickAt(10, 10).kind == PageClick::Kind::Nothing);
    const auto w = t.words();
    CHECK(w.size() == 6);
    CHECK(t.slice(w[0].first, w[0].second) == "Hello");
    CHECK(t.slice(w[5].first, w[5].second) == "line");

    GROUP("pagewords:context");
    std::string longLine;
    for (int i = 0; i < 20; ++i) longLine += "abc ";
    longLine += "target";
    for (int i = 0; i < 20; ++i) longLine += " xyz";
    const PageText l = layOut(longLine);
    c = clickChar(l, 80);
    CHECK(c.word == "target");
    CHECK(c.before.size() == PageText::kContext);
    CHECK(c.after.size() == PageText::kContext);

    GROUP("pagewords:unicode");
    // Ligatures, accents and other scripts are word characters; the word
    // and its context come back as UTF-8.
    const PageText u = layOut("e\xEF\xAC\x83" "cient r\xC3\xA9sum\xC3\xA9 \xCE\xB1\xCE\xB2 \xE2\x80\xA2 end");
    c = clickChar(u, 1);
    CHECK(c.word == "e\xEF\xAC\x83" "cient");
    c = clickChar(u, 8);
    CHECK(c.word == "r\xC3\xA9sum\xC3\xA9");
    CHECK(c.before == "e\xEF\xAC\x83" "cient ");
    c = clickChar(u, 15);
    CHECK(c.word == "\xCE\xB1\xCE\xB2");
    c = clickChar(u, 18);                                     // the bullet
    CHECK(c.kind == PageClick::Kind::Line);

    GROUP("pagewords:apostrophe");
    const PageText a = layOut("don't 'quoted' l\xE2\x80\x99" "\xC3\xA9t\xC3\xA9");
    CHECK(clickChar(a, 0).word == "don't");
    CHECK(clickChar(a, 4).word == "don't");
    CHECK(clickChar(a, 8).word == "quoted");                  // quotes are not letters
    CHECK(clickChar(a, 6).kind == PageClick::Kind::Line);
    CHECK(clickChar(a, 15).word == "l\xE2\x80\x99\xC3\xA9t\xC3\xA9");

    GROUP("pagewords:hyphenation");
    const PageText h = layOut("the counterrevolu-\ntionaries marched");
    c = clickChar(h, 6);                                      // first half
    CHECK(c.word == "counterrevolutionaries");
    CHECK(c.before == "the ");
    CHECK(c.after == " marched");
    c = clickChar(h, 20);                                     // second half
    CHECK(c.word == "counterrevolutionaries");
    CHECK(c.before == "the ");
    CHECK(c.after == " marched");
    // A hyphen inside a line joins nothing.
    const PageText k = layOut("well-known words");
    c = clickChar(k, 1);
    CHECK(c.word == "well");
    CHECK(c.after == "-known words");
    std::string wd = "abc", be = "x-\n", af = "";
    joinHyphenation(&wd, &be, &af);
    CHECK(wd == "xabc" && be.empty());
    wd = "abc"; be = "-\n"; af = "-";                         // nothing to join
    joinHyphenation(&wd, &be, &af);
    CHECK(wd == "abc" && be == "-\n" && af == "-");

    GROUP("pagewords:span");
    // Without SyncTeX lines nothing is near, so every click refuses; a click
    // on no text refuses before the matcher is even asked.
    const LatexDoc doc = LatexDoc::parse("\\documentclass{article}\n\\begin{document}\n"
                                         "Hello world of text\n\\end{document}\n");
    const SyncTexIndex none = SyncTexIndex::parse("");
    PageClick seen;
    CHECK(latexSpanAtPoint(doc, none, 0, t, 1, 10, 10, nullptr, &seen) == nullptr);
    CHECK(seen.kind == PageClick::Kind::Nothing);
    CHECK(latexSpanAtPoint(doc, none, 0, t, 1, 72 + 8 * 6 + 3, 77, nullptr, &seen) == nullptr);
    CHECK(seen.word == "world");
    // Boxes and characters that disagree in number are cut to the shorter.
    const PageText cut("abc", {PageBox{0, 0, 5, 5}});
    CHECK(cut.size() == 1);
}

// ------------------------------------------------------------ terminal links
// TermLinkPath: where a file reference in the terminal's output points. Real
// files in a scratch directory, since the whole point is that only paths
// that exist resolve.
static void touch(const std::string &p) {
    FILE *f = std::fopen(p.c_str(), "w");
    if (f) std::fclose(f);
}

static void testTermLinkPath() {
    GROUP("TermLinkPath");
    char tmpl[] = "/tmp/minicode-links-XXXXXX";
    const char *made = mkdtemp(tmpl);
    CHECK(made != nullptr);
    if (!made) return;
    const std::string base = made;
    const std::string root = base + "/proj", shell = base + "/proj/build";
    mkdir(root.c_str(), 0700);
    mkdir(shell.c_str(), 0700);
    mkdir((root + "/src").c_str(), 0700);
    touch(root + "/src/main.cpp");
    touch(shell + "/gen.cpp");
    touch(base + "/outside.txt");
    // The project reached through a symlink, as the shell might spell it.
    const std::string alias = base + "/alias";
    CHECK(symlink(root.c_str(), alias.c_str()) == 0);

    TermLinkPath::Target t;
    // Relative to the shell's directory first...
    CHECK(TermLinkPath::resolve("gen.cpp", shell, root, "", &t));
    CHECK(t.path == shell + "/gen.cpp" && !t.isDir && t.insideRoot);
    // ...then the project folder.
    CHECK(TermLinkPath::resolve("src/main.cpp", shell, root, "", &t));
    CHECK(t.path == root + "/src/main.cpp" && t.insideRoot);
    CHECK(TermLinkPath::resolve("../src/main.cpp", shell, root, "", &t));
    CHECK(t.path == root + "/src/main.cpp");
    // Nothing there: no link.
    CHECK(!TermLinkPath::resolve("nope.cpp", shell, root, "", &t));
    CHECK(!TermLinkPath::resolve("e.g", shell, root, "", &t));
    CHECK(!TermLinkPath::resolve("", shell, root, "", &t));
    // Absolute, inside and outside the project.
    CHECK(TermLinkPath::resolve(base + "/outside.txt", shell, root, "", &t));
    CHECK(t.path == base + "/outside.txt" && !t.insideRoot);
    // A folder.
    CHECK(TermLinkPath::resolve("src", shell, root, "", &t));
    CHECK(t.isDir && t.insideRoot && t.path == root + "/src");
    CHECK(TermLinkPath::resolve(".", root, root, "", &t));
    CHECK(t.isDir && t.path == root);
    // Through the symlink, a file inside comes back in the root's spelling;
    // with the root itself opened through the symlink, likewise.
    CHECK(TermLinkPath::resolve(alias + "/src/main.cpp", "", root, "", &t));
    CHECK(t.path == root + "/src/main.cpp" && t.insideRoot);
    CHECK(TermLinkPath::resolve(root + "/src/main.cpp", "", alias, "", &t));
    CHECK(t.path == alias + "/src/main.cpp" && t.insideRoot);
    // "~" is the home directory given, and nothing without one.
    CHECK(TermLinkPath::resolve("~/outside.txt", "", "", base, &t));
    CHECK(t.path == base + "/outside.txt");
    CHECK(!TermLinkPath::resolve("~/outside.txt", "", "", "", &t));
    // With neither a shell directory nor a root, a relative path is nothing.
    CHECK(!TermLinkPath::resolve("src/main.cpp", "", "", "", &t));

    // What the terminal does with a line: TermLinks finds the reference, and
    // this resolves it.
    const std::string line = "src/main.cpp:12:5: error: expected ';'";
    const auto links = TermLinks::find(line);
    const TermLinks::Link *l = TermLinks::at(links, 3);
    CHECK(l && l->kind == TermLinks::Link::File && l->line == 12 && l->column == 5);
    CHECK(l && TermLinkPath::resolve(l->target, shell, root, "", &t) &&
          t.path == root + "/src/main.cpp");

    std::remove(alias.c_str());
    std::remove((root + "/src/main.cpp").c_str());
    std::remove((shell + "/gen.cpp").c_str());
    std::remove((base + "/outside.txt").c_str());
    rmdir((root + "/src").c_str());
    rmdir(shell.c_str());
    rmdir(root.c_str());
    rmdir(base.c_str());
}

// UTF-16 column -> byte offset, the conversion byteColumn() in Lsp.cpp uses
// for a definition read from disk, which may not be valid UTF-8.
static void testByteOffsetOfUtf16() {
    GROUP("utf16-to-byte");
    using utf16::byteOffsetOfUtf16;
    CHECK(byteOffsetOfUtf16("abc", 0) == 0);
    CHECK(byteOffsetOfUtf16("abc", 2) == 2);
    CHECK(byteOffsetOfUtf16("abc", 99) == 3);                 // clamped
    CHECK(byteOffsetOfUtf16("caf\xC3\xA9 x", 4) == 5);        // é is 2 bytes, 1 unit
    CHECK(byteOffsetOfUtf16("a\xF0\x9F\x98\x80z", 3) == 5);   // an emoji is 2 units
    CHECK(byteOffsetOfUtf16("a\xF0\x9F\x98\x80z", 2) == 1);   // mid-pair: its start
    // Invalid UTF-8 stays inside the string, one unit per bad byte, the way
    // utf16::fromUtf8 counts it. Before, "x\xF0" at column 2 gave byte 5.
    CHECK(byteOffsetOfUtf16("x\xF0", 2) == 2);
    CHECK(byteOffsetOfUtf16("x\xF0", 9) == 2);
    CHECK(byteOffsetOfUtf16("/* caf\xE9 */ int foo;", 15) == 15);
    CHECK(byteOffsetOfUtf16("\xE2\x82", 5) == 2);              // cut-off sequence
    // Agrees with fromUtf8 on every prefix of a mixed string.
    const std::string mixed = "a\xC3\xA9\xFF\xF0\x9F\x98\x80\xE2\x82z";
    const std::u16string u = utf16::fromUtf8(mixed);
    bool agrees = true;
    for (size_t col = 0; col <= u.size(); ++col) {
        size_t b = byteOffsetOfUtf16(mixed, col);
        if (b > mixed.size()) agrees = false;
        else if (utf16::fromUtf8(mixed.substr(0, b)).size() > col) agrees = false;
    }
    CHECK(agrees);
}

// ------------------------------------------------ web pictures (WebImageRules)

static std::vector<unsigned char> bytes(std::initializer_list<int> v) {
    std::vector<unsigned char> out;
    for (int b : v) out.push_back((unsigned char)b);
    return out;
}

static void testWebImageRules() {
    GROUP("web images: addresses");
    using namespace WebImageRules;
    CHECK(isFetchable("https://img.shields.io/badge/a-b-green.svg"));
    CHECK(isFetchable("HTTPS://Example.com/x.png"));
    CHECK(isFetchable("https://127.0.0.1:8443/p.png?x=1#f"));
    CHECK(!isFetchable("http://example.com/x.png"));
    CHECK(!isFetchable("//example.com/x.png"));
    CHECK(!isFetchable("data:image/png;base64,AAAA"));
    CHECK(!isFetchable("images/x.png"));
    CHECK(!isFetchable("file:///tmp/x.png"));
    CHECK(!isFetchable("https://"));
    CHECK(!isFetchable("https:///x.png"));
    CHECK(!isFetchable("https://a b/x.png"));
    CHECK(!isFetchable("https://example.com/\nx.png"));
    CHECK(!isFetchable("ftp://example.com/x.png"));

    GROUP("web images: header sizes");
    int w = 0, h = 0;
    // PNG: signature, IHDR length, "IHDR", width 640, height 480.
    auto png = bytes({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                      0, 0, 2, 0x80, 0, 0, 1, 0xE0, 8, 6, 0, 0, 0});
    CHECK(headerSize(png.data(), png.size(), &w, &h) && w == 640 && h == 480);
    CHECK(!headerSize(png.data(), 20, &w, &h));   // cut short
    // A PNG claiming 100000 x 100000: read, and refused by sizeAllowed.
    auto bomb = png;
    bomb[16] = 0; bomb[17] = 0x01; bomb[18] = 0x86; bomb[19] = 0xA0;
    bomb[20] = 0; bomb[21] = 0x01; bomb[22] = 0x86; bomb[23] = 0xA0;
    CHECK(headerSize(bomb.data(), bomb.size(), &w, &h) && w == 100000 && h == 100000);
    CHECK(!sizeAllowed(w, h));
    // GIF: logical screen 160 x 100, little-endian.
    auto gif = bytes({'G', 'I', 'F', '8', '9', 'a', 160, 0, 100, 0, 0xF7, 0, 0});
    CHECK(isGif(gif.data(), gif.size()));
    CHECK(headerSize(gif.data(), gif.size(), &w, &h) && w == 160 && h == 100);
    auto gif87 = bytes({'G', 'I', 'F', '8', '7', 'a', 1, 1, 2, 0});
    CHECK(isGif(gif87.data(), gif87.size()));
    CHECK(headerSize(gif87.data(), gif87.size(), &w, &h) && w == 257 && h == 2);
    CHECK(!isGif(png.data(), png.size()));
    // JPEG: SOI, an APP0 segment to skip, a fill byte, then SOF0 with
    // height 300 and width 400.
    auto jpg = bytes({0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0,
                      0xFF, 0xFF, 0xC0, 0, 17, 8, 1, 0x2C, 1, 0x90, 3, 1, 0x22, 0});
    CHECK(headerSize(jpg.data(), jpg.size(), &w, &h) && w == 400 && h == 300);
    // A progressive JPEG (SOF2) after a DHT (C4) that must not be taken for
    // a frame.
    auto prog = bytes({0xFF, 0xD8, 0xFF, 0xC4, 0, 4, 0, 0, 0xFF, 0xC2, 0, 17, 8, 0, 10, 0, 20, 3});
    CHECK(headerSize(prog.data(), prog.size(), &w, &h) && w == 20 && h == 10);
    // Scan data before any frame: no size.
    auto noframe = bytes({0xFF, 0xD8, 0xFF, 0xDA, 0, 8, 1, 2, 3, 4, 5, 6});
    CHECK(!headerSize(noframe.data(), noframe.size(), &w, &h));
    // WebP, the three kinds.
    auto vp8x = bytes({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'X',
                       10, 0, 0, 0, 0x10, 0, 0, 0, 0x1F, 0x03, 0, 0xC7, 0, 0});   // 800 x 200
    CHECK(headerSize(vp8x.data(), vp8x.size(), &w, &h) && w == 800 && h == 200);
    // VP8L: 14-bit width-1 and height-1 packed after the 0x2F signature:
    // width 100 (99 = 0x63), height 50 (49 << 14).
    const unsigned long bits = 99ul | (49ul << 14);
    auto vp8l = bytes({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'L',
                       0, 0, 0, 0, 0x2F, (int)(bits & 0xFF), (int)((bits >> 8) & 0xFF),
                       (int)((bits >> 16) & 0xFF), (int)((bits >> 24) & 0xFF), 0, 0, 0, 0, 0});
    CHECK(headerSize(vp8l.data(), vp8l.size(), &w, &h) && w == 100 && h == 50);
    auto vp8 = bytes({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' ',
                      0, 0, 0, 0, 0, 0, 0, 0x9D, 0x01, 0x2A, 0x40, 0x01, 0xF0, 0x00});   // 320 x 240
    CHECK(headerSize(vp8.data(), vp8.size(), &w, &h) && w == 320 && h == 240);
    // SVG and plain garbage say nothing.
    const std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"90\" height=\"20\"/>";
    CHECK(!headerSize((const unsigned char*)svg.data(), svg.size(), &w, &h));
    auto junk = bytes({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
                       21, 22, 23, 24, 25, 26, 27, 28, 29, 30});
    CHECK(!headerSize(junk.data(), junk.size(), &w, &h));
    CHECK(!headerSize(nullptr, 0, &w, &h));

    GROUP("web images: limits");
    CHECK(sizeAllowed(1, 1));
    CHECK(sizeAllowed(8192, 8192));
    CHECK(!sizeAllowed(8192, 8193));
    CHECK(!sizeAllowed(0, 10) && !sizeAllowed(10, -1));
    CHECK(decodedCost(100, 50, false, 999) == 20000);
    CHECK(decodedCost(100, 50, true, 999) == 40999);
    CHECK(kMaxBytes == 20u * 1024 * 1024 && kCacheBytes == 64u * 1024 * 1024);

    GROUP("web images: cache");
    ByteLru<std::string> lru(100);
    CHECK(lru.put("a", "A", 40));
    CHECK(lru.put("b", "B", 40));
    CHECK(lru.bytes() == 80 && lru.size() == 2);
    CHECK(lru.get("a") && *lru.get("a") == "A");   // a is now the most recent
    CHECK(lru.put("c", "C", 40));                   // over 100: b, the least recent, goes
    CHECK(!lru.get("b") && lru.get("a") && lru.get("c"));
    CHECK(lru.bytes() == 80);
    CHECK(!lru.put("huge", "H", 101));              // over the whole limit: not kept
    CHECK(!lru.get("huge") && lru.size() == 2);
    CHECK(lru.put("a", "A2", 10));                  // replacing counts the new cost only
    CHECK(lru.bytes() == 50 && *lru.get("a") == "A2");
    CHECK(lru.put("d", "D", 100));                  // exactly the limit: all else goes
    CHECK(lru.size() == 1 && lru.bytes() == 100 && lru.get("d"));
    lru.erase("d");
    CHECK(lru.size() == 0 && lru.bytes() == 0);

    GROUP("web images: failures");
    FailureMemory fails(60);
    CHECK(!fails.recent("x", 0));
    fails.remember("x", 10);
    CHECK(fails.recent("x", 10) && fails.recent("x", 69.9));
    CHECK(!fails.recent("x", 70));
    CHECK(fails.size() == 0);   // forgotten once expired
    // Many failures: the expired ones are swept out as new ones come in.
    for (int i = 0; i < 600; i++) fails.remember("u" + std::to_string(i), i < 300 ? 0 : 100);
    CHECK(fails.size() == 300 && fails.recent("u599", 100) && !fails.recent("u0", 100));
}

// What `minicode` makes of its command line, and when it leaves the
// terminal (PARITY.md R2; the detach GUI suite runs the real binary).
static void testLaunchOptions() {
    using namespace LaunchOptions;
    typedef std::vector<std::string> Args;

    GROUP("launch: options");
    Parsed p = parse(Args{"minicode"});
    CHECK(!p.help && !p.version && !p.foreground && p.error.empty());
    CHECK(p.args == Args{"minicode"});
    p = parse(Args{"/usr/bin/minicode", "notes.md"});
    CHECK(p.args == (Args{"/usr/bin/minicode", "notes.md"}));
    CHECK(parse(Args{"minicode", "-h"}).help && parse(Args{"minicode", "--help"}).help);
    CHECK(parse(Args{"minicode", "-v"}).version && parse(Args{"minicode", "--version"}).version);
    // --foreground anywhere, and taken out of what GApplication gets, so a
    // running minicode is handed the path alone.
    p = parse(Args{"minicode", "--foreground", "src"});
    CHECK(p.foreground && p.args == (Args{"minicode", "src"}));
    p = parse(Args{"minicode", "src", "--foreground"});
    CHECK(p.foreground && p.args == (Args{"minicode", "src"}));
    // An option the port does not have is a usage error, as the Mac's
    // launcher says it; -n and -r are the Mac's and not the port's.
    for (const char* o : {"-n", "-r", "--new-window", "--reuse-window", "-x", "-hv", "--bogus"}) {
        p = parse(Args{"minicode", o});
        CHECK(p.error == std::string("minicode: unknown option ") + o + "\nTry 'minicode --help'.\n");
    }
    // The first unknown option is the one named.
    CHECK(parse(Args{"minicode", "-x", "-y"}).error.find("-x") != std::string::npos);
    // "--" ends the options: what follows is a path, an option-like one
    // made relative so nothing after reads it as an option.
    p = parse(Args{"minicode", "--", "--foreground"});
    CHECK(!p.foreground && p.error.empty() && p.args == (Args{"minicode", "./--foreground"}));
    p = parse(Args{"minicode", "--", "-notes.md", "/abs/-x"});
    CHECK(p.args == (Args{"minicode", "./-notes.md", "/abs/-x"}));
    // "-" alone is a name, as for most programs; an empty argument stays.
    CHECK(parse(Args{"minicode", "-"}).args == (Args{"minicode", "-"}));
    CHECK(parse(Args{"minicode", ""}).args == (Args{"minicode", ""}));
    // GApplication's own option goes through to it.
    p = parse(Args{"minicode", "--gapplication-service"});
    CHECK(p.error.empty() && p.args == (Args{"minicode", "--gapplication-service"}));
    // No argv[0] at all.
    CHECK(parse(Args{}).args == Args{"minicode"});

    GROUP("launch: when to leave the terminal");
    const Parsed plain = parse(Args{"minicode", "src"});
    CHECK(shouldDetach(plain, true, nullptr));
    CHECK(!shouldDetach(plain, false, nullptr));   // the desktop's launch: no terminal
    CHECK(!shouldDetach(parse(Args{"minicode", "--foreground"}), true, nullptr));
    CHECK(!shouldDetach(plain, true, "1"));
    CHECK(!shouldDetach(plain, true, "yes"));
    CHECK(shouldDetach(plain, true, ""));
    CHECK(shouldDetach(plain, true, "0"));
    // What prints and returns never forks.
    CHECK(!shouldDetach(parse(Args{"minicode", "-h"}), true, nullptr));
    CHECK(!shouldDetach(parse(Args{"minicode", "--version"}), true, nullptr));
    CHECK(!shouldDetach(parse(Args{"minicode", "-q"}), true, nullptr));

    GROUP("launch: help and version");
    const std::string help = helpText();
    CHECK(help.rfind("Usage: minicode [options] [path]\n", 0) == 0);
    for (const char* line : {"      --foreground", "MINICODE_FOREGROUND=1", "  -v, --version",
                             "  -h, --help", "returns"})
        CHECK(help.find(line) != std::string::npos);
    // Only what the port has.
    CHECK(help.find("--new-window") == std::string::npos);
    CHECK(help.find("--reuse-window") == std::string::npos);
    // Within 80 columns, for a terminal.
    size_t start = 0, widest = 0;
    for (size_t nl; (nl = help.find('\n', start)) != std::string::npos; start = nl + 1)
        widest = std::max(widest, nl - start);
    CHECK(widest <= 80);
    CHECK(help.back() == '\n');
    CHECK(versionText("1.5.17") == "minicode 1.5.17\n");
    CHECK(versionText("") == "minicode unknown\n");

    GROUP("launch: scope name");
    CHECK(scopeName("org.minicode.Editor", 4242) == "app-minicode-org.minicode.Editor-4242.scope");
    // systemd's escaping, as in app-gnome-org.gnome.Evolution\x2dalarm\x2dnotify-5346.scope.
    CHECK(scopeName("org.gnome.Evolution-alarm-notify", 7) ==
          "app-minicode-org.gnome.Evolution\\x2dalarm\\x2dnotify-7.scope");
    CHECK(scopeName(".x y", 1) == "app-minicode-\\x2ex\\x20y-1.scope");
}

int main() {
    std::printf("Running MiniCode Linux port tests...\n\n");
    testAscii();
    testTwoByte();
    testThreeByte();
    testFourByte();
    testEdges();
    testMatchesReference();
    testRealSources();
    testUtf16();
    testByteOffsetOfUtf16();
    testThemeCss();
    testPageWords();
    testTermLinkPath();
    testWebImageRules();
    testLaunchOptions();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
