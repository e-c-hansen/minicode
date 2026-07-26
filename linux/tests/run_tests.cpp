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
int main() {
    std::printf("Running MiniCode Linux port tests...\n\n");
    testAscii();
    testTwoByte();
    testThreeByte();
    testFourByte();
    testEdges();
    testMatchesReference();
    testRealSources();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
