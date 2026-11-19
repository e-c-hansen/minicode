// text_scan_tests.inc — the `textscan:*` groups of run_tests.cpp
// (TextScan.h: the find bar's search of a large file by its needle, over
// UTF-8 bytes or UTF-16 read in blocks), kept in their own file and
// included there.
#include "TextScan.h"

namespace {

// UTF-16 to UTF-8 (valid input).
std::string scanUtf8(const std::u16string &u) {
    std::string out;
    for (size_t i = 0; i < u.size(); i++) {
        char32_t c = u[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < u.size()) {
            c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

TextScan::Text scanReader(const std::u16string &text) {
    return TextScan::Text::utf16(text.size(), [&text](size_t start, size_t n, char16_t *out) {
        std::copy(text.begin() + (long)start, text.begin() + (long)(start + n), out);
    });
}

std::vector<TextFind::Match> scanAll(const TextScan::Text &t, const TextFind::Query &q,
                                     size_t from = 0, size_t to = SIZE_MAX, size_t block = 1 << 20) {
    std::vector<TextFind::Match> out;
    TextScan::Options o;
    o.blockUnits = block;
    TextScan::scan(t, q, from, to, [&](const TextFind::Match &m) {
        out.push_back(m);
        return true;
    }, o);
    return out;
}

bool sameMatches(const std::vector<TextFind::Match> &a, const std::vector<TextFind::Match> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i].start != b[i].start || a[i].length != b[i].length || a[i].groups != b[i].groups)
            return false;
    return true;
}

TextFind::Query scanQuery(const std::string &p, bool cs, bool word = false, bool re = false) {
    TextFind::Options o;
    o.caseSensitive = cs;
    o.wholeWord = word;
    o.regex = re;
    return TextFind::Query::make(p, o);
}

}  // namespace

static void testTextScan() {
    using TextScan::Needle;
    using TextScan::Text;
    const size_t npos = std::string::npos;

    GROUP("textscan:needle-ascii-case");
    {
        // Ignoring case, ASCII letters fold both ways in bytes and in units.
        const Needle nd = Needle::make(U"needle-4242", false);
        CHECK(nd.folds() && nd.whole());
        const std::string s = "x NEEDLE_4242 y NeEdLe-4242 z";
        CHECK(nd.find(s.data(), s.size(), 0) == 16);
        const std::u16string u = toU16(s);
        CHECK(nd.find(u.data(), u.size(), 0) == 16);
        CHECK(nd.find(s.data(), s.size(), 17) == npos);
        // Matching case, the same needle does not match other capitals.
        const Needle cs = Needle::make(U"needle-4242", true);
        CHECK(!cs.folds() && cs.find(s.data(), s.size(), 0) == npos);
        // No letters: nothing to fold, found by memchr.
        const Needle digits = Needle::make(U"2026", false);
        CHECK(!digits.folds() && digits.whole());
        // Not ASCII, ignoring case: its longest ASCII run, and not whole.
        const Needle cafe = Needle::make(U"cafés", false);
        CHECK(cafe.byteLength() == 3 && !cafe.whole());
        // A line break: no line holds it.
        CHECK(Needle::make(U"a\nb", true).never());
        // Only ASCII folds to ASCII, which is what makes an ASCII needle
        // enough for a text that is not.
        bool onlyAscii = true;
        for (char32_t c = 0x80; c < 0x10000; c++)
            if (TextFind::foldCase(c) < 0x80) onlyAscii = false;
        CHECK(onlyAscii);
    }

    GROUP("textscan:utf8-boundaries");
    {
        // é is C3 A9; ラ (U+30E9) is E3 83 A9 and ends in the same byte, ©
        // (U+00A9) is C2 A9: no needle matches inside another character.
        const Needle e = Needle::make(U"é", true);
        const std::string s = "\xE3\x83\xA9\xC3\xA9";
        CHECK(e.find(s.data(), s.size(), 0) == 3);
        const Needle copy = Needle::make(U"©", true);
        CHECK(copy.find(s.data(), s.size(), 0) == npos);
        // Ignoring case, É (C3 89) is found by verifying the line: the
        // needle is "caf", and TextFind's folding matches É to é.
        const std::u16string text = u"aé\U0001F600 x NEEDLE\nb needle CAFÉ\r\nCAFE café";
        const std::string bytes = scanUtf8(text);
        const Text t8 = Text::utf8(bytes.data(), bytes.size(), false);
        CHECK(t8.length == text.size());
        const auto q = scanQuery("needle", false);
        const auto want = TextFind::findAll(text, q);
        CHECK(want.size() == 2 && want[0].start == 7 && want[1].start == 16);
        CHECK(sameMatches(scanAll(t8, q), want));
        CHECK(sameMatches(scanAll(scanReader(text), q), want));
        const auto cafe = scanQuery("caf\xC3\xA9", false);
        const auto wantCafe = TextFind::findAll(text, cafe);
        CHECK(wantCafe.size() == 2);
        CHECK(sameMatches(scanAll(t8, cafe), wantCafe));
        CHECK(sameMatches(scanAll(t8, cafe, 0, SIZE_MAX, 3), wantCafe));
        CHECK(sameMatches(scanAll(scanReader(text), cafe, 0, SIZE_MAX, 3), wantCafe));
        // Units, not bytes: 😀 is two, é one.
        TextFind::Match m;
        CHECK(TextScan::findFirst(t8, scanQuery("x", true), 0, SIZE_MAX, &m) && m.start == 5);
        CHECK(TextScan::findLast(t8, q, 0, SIZE_MAX, &m) && m.start == 16);
        CHECK(TextScan::findLast(t8, q, 0, 16, &m) && m.start == 7);
        CHECK(!TextScan::findLast(t8, q, 0, 7, &m));
    }

    GROUP("textscan:same-as-findall");
    {
        // Random texts against TextFind::findAll on the whole: bytes in
        // ASCII and in UTF-8, units read in blocks; blocks smaller than
        // lines; every kind of query (the needle is the match, a line
        // verified, a regex with no needle); a range and the first and last
        // match in it.
        std::mt19937 rng(41);
        const char16_t *pieces[] = {u"a", u"A", u"b", u"B", u"ab", u"-", u" ", u"\n", u"\r",
                                    u"\r\n", u" ", u"é", u"É", u"\U0001F600", u"x"};
        const char *patterns[] = {"ab", "a", "AB", "a-b", "b a", "\xC3\xA9", "a\xC3\xA9", "x",
                                  "^a", "b$", "a+b", "\\bab\\b", "[ab]{2}", "^$", ".", "a\\nb"};
        int bad = 0, checked = 0;
        for (int round = 0; round < 160; round++) {
            std::u16string text;
            const size_t n = rng() % 120;
            const bool ascii = round % 3 == 0;
            for (size_t i = 0; i < n; i++) {
                const char16_t *p = pieces[rng() % (ascii ? 10 : 15)];
                text += p;
            }
            const std::string bytes = scanUtf8(text);
            for (const char *p : patterns) {
                const bool re = std::strpbrk(p, "^$\\[+.") != nullptr;
                const auto q = scanQuery(p, round % 2, round % 5 == 0, re);
                if (!q.usable()) continue;
                const auto whole = TextFind::findAll(text, q);
                const size_t block = 1 + rng() % 16;
                const size_t from = text.empty() ? 0 : rng() % (text.size() + 1);
                const size_t to = rng() % 3 == 0 ? SIZE_MAX : from + rng() % (text.size() + 2);
                std::vector<TextFind::Match> want;
                for (const auto &m : whole)
                    if (m.start >= from && (to >= text.size() || m.start < to)) want.push_back(m);
                const Text t8 = Text::utf8(bytes.data(), bytes.size(), ascii);
                const Text t16 = scanReader(text);
                bool ok = sameMatches(scanAll(t8, q), whole) &&
                          sameMatches(scanAll(t16, q, 0, SIZE_MAX, block), whole) &&
                          sameMatches(scanAll(t8, q, from, to, block), want) &&
                          sameMatches(scanAll(t16, q, from, to, block), want);
                TextScan::Options o;
                o.blockUnits = block;
                TextFind::Match first, last;
                const bool gotFirst = TextScan::findFirst(t16, q, from, to, &first, o);
                const bool gotLast = TextScan::findLast(t8, q, from, to, &last, o);
                ok = ok && gotFirst == !want.empty() && gotLast == !want.empty();
                if (ok && !want.empty())
                    ok = first.start == want.front().start && last.start == want.back().start;
                if (!ok) {
                    if (bad < 3) std::printf("  textscan: \"%s\" differs (round %d)\n", p, round);
                    bad++;
                }
                checked++;
            }
        }
        CHECK(bad == 0 && checked > 2000);
    }

    GROUP("textscan:cancel-and-speed");
    {
        // A 32 MB log with its one needle on the last line: found through
        // the bytes in a few ms (TextFind a line at a time took about 2 s
        // for 259 MB in the round 9 grading).
        std::string log;
        log.reserve(32u << 20);
        char line[160];
        for (unsigned i = 0; log.size() < (32u << 20); i++) {
            const int k = std::snprintf(line, sizeof line,
                                        "2026-11-18T%02u:%02u:%02u.%03uZ INFO  worker-%02u "
                                        "request id=%08u path=/api/items/%u\n",
                                        (i / 3600) % 24, (i / 60) % 60, i % 60, i % 1000, i % 32, i,
                                        i % 9973);
            log.append(line, (size_t)k);
        }
        log += "2026-11-18T23:59:59.999Z ERROR worker-07 NEEDLE-4242 payment failed\n";
        const Text t = Text::utf8(log.data(), log.size(), true);
        TextFind::Match m;
        double c0 = cpuMs();
        const bool found = TextScan::findFirst(t, scanQuery("needle-4242", false), 0, SIZE_MAX, &m);
        const double folded = cpuMs() - c0;
        CHECK(found && m.start == log.rfind("NEEDLE-4242"));
        c0 = cpuMs();
        size_t count = 0;
        TextScan::scan(t, scanQuery("2026", false), 0, SIZE_MAX, [&](const TextFind::Match &) {
            count++;
            return true;
        });
        const double counted = cpuMs() - c0;
        CHECK(count > 300000);
        std::printf("  textscan: 32 MB, the last line's needle %.1f ms, %zu matches counted %.1f ms\n",
                    folded, count, counted);
        CHECK_TIME(folded, 150);
        CHECK_TIME(counted, 600);
        // A raised flag stops it, and says so.
        std::atomic<bool> stop{true};
        TextScan::Options o;
        o.cancel = &stop;
        bool cancelled = false;
        CHECK(!TextScan::findFirst(t, scanQuery("needle-4242", false), 0, SIZE_MAX, &m, o, &cancelled));
        CHECK(cancelled);
    }
}
