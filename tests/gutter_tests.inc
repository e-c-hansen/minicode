// gutter_tests.inc — included by run_tests.cpp. The line index behind the
// editor's line-number gutter, and bracket matching.

namespace {

std::vector<size_t> startsOf(const LineIndex &ix) {
    std::vector<size_t> v;
    for (size_t i = 0; i < ix.lineCount(); i++) v.push_back(ix.lineStart(i));
    return v;
}

LineIndex indexOf(const std::u16string &t) {
    LineIndex ix;
    ix.reset(t.data(), t.size());
    return ix;
}

// Apply an edit to both the text and the index.
void editBoth(std::u16string &t, LineIndex &ix, size_t pos, size_t oldLen,
              const std::u16string &ins) {
    t.replace(pos, oldLen, ins);
    const size_t end = pos + ins.size();
    ix.edit(pos, oldLen, ins.data(), ins.size(), pos > 0 ? t[pos - 1] : 0,
            end < t.size() ? t[end] : 0);
}

}  // namespace

static void testLineIndex() {
    GROUP("lineindex:basics");
    {
        LineIndex empty;
        CHECK(empty.lineCount() == 1 && empty.lineOf(0) == 0 && empty.length() == 0);
        std::u16string t = u"ab\ncd\n\nef";
        LineIndex ix = indexOf(t);
        CHECK(startsOf(ix) == (std::vector<size_t>{0, 3, 6, 7}));
        CHECK(ix.lineOf(0) == 0 && ix.lineOf(2) == 0 && ix.lineOf(3) == 1);
        CHECK(ix.lineOf(6) == 2 && ix.lineOf(7) == 3 && ix.lineOf(9) == 3);
        CHECK(ix.lineOf(500) == 3);                       // past the end: the last line
        CHECK(ix.lineEnd(0) == 3 && ix.lineEnd(3) == 9);
        CHECK(ix.isLineStart(3) && !ix.isLineStart(4) && ix.isLineStart(0));
        // A text ending in a newline has an empty last line.
        CHECK(indexOf(u"x\n").lineCount() == 2);
        // CRLF is one break, after the \n.
        CHECK(startsOf(indexOf(u"a\r\nb\r\n")) == (std::vector<size_t>{0, 3, 6}));
        // A '\r' alone (an old Mac file) and U+2029 end lines too, as they
        // do for NSTextView and TextFind.
        CHECK(startsOf(indexOf(u"a\rb\rc")) == (std::vector<size_t>{0, 2, 4}));
        CHECK(startsOf(indexOf(u"a\r")) == (std::vector<size_t>{0, 2}));
        CHECK(startsOf(indexOf(u"a\u2029b")) == (std::vector<size_t>{0, 2}));
        CHECK(startsOf(indexOf(u"a\r\r\nb")) == (std::vector<size_t>{0, 2, 4}));
    }

    GROUP("lineindex:cr-edits");
    {
        // A '\n' typed after a '\r' joins them into one break, and deleting
        // it splits them again; so does text typed or deleted between them.
        std::u16string t = u"a\rb\rc";
        LineIndex ix = indexOf(t);
        editBoth(t, ix, 2, 0, u"\n");                     // a\r\nb\rc
        CHECK(startsOf(ix) == (std::vector<size_t>{0, 3, 5}));
        editBoth(t, ix, 2, 1, u"");                       // a\rb\rc again
        CHECK(startsOf(ix) == (std::vector<size_t>{0, 2, 4}));
        editBoth(t, ix, 2, 0, u"x");                      // a\rxb\rc
        CHECK(startsOf(ix) == startsOf(indexOf(t)));
        t = u"a\rx\nb";
        ix = indexOf(t);
        editBoth(t, ix, 2, 1, u"");                       // a\r\nb: one break
        CHECK(startsOf(ix) == (std::vector<size_t>{0, 3}));
        editBoth(t, ix, 1, 0, u"\r");                     // a\r\r\nb
        CHECK(startsOf(ix) == startsOf(indexOf(t)));
        // Random edits heavy in \r and \n against a full index.
        std::mt19937 rng(11);
        const char16_t alphabet[] = u"a\r\n\r\n\u2029b";
        int mismatches = 0;
        t = u"x\ry\r\nz";
        ix = indexOf(t);
        for (int i = 0; i < 20000; i++) {
            size_t pos = t.empty() ? 0 : rng() % (t.size() + 1);
            size_t del = pos < t.size() ? rng() % std::min<size_t>(t.size() - pos + 1, 4) : 0;
            std::u16string ins;
            for (size_t n = rng() % 4; n > 0; n--) ins += alphabet[rng() % 7];
            if (t.size() > 300) del = std::min<size_t>(t.size() - pos, 30);
            editBoth(t, ix, pos, del, ins);
            if (startsOf(ix) != startsOf(indexOf(t)) || ix.length() != t.size()) mismatches++;
        }
        CHECK(mismatches == 0);
    }

    GROUP("lineindex:edits");
    {
        std::u16string t = u"one\ntwo\nthree\n";
        LineIndex ix = indexOf(t);
        editBoth(t, ix, 4, 0, u"X\nY\n");                // lines added inside
        CHECK(startsOf(ix) == startsOf(indexOf(t)) && ix.length() == t.size());
        editBoth(t, ix, 2, 7, u"");                       // breaks deleted
        CHECK(startsOf(ix) == startsOf(indexOf(t)));
        editBoth(t, ix, 0, t.size(), u"a\nb\nc\nd");      // everything replaced
        CHECK(startsOf(ix) == (std::vector<size_t>{0, 2, 4, 6}));
        editBoth(t, ix, t.size(), 0, u"\n");              // typing at the end
        CHECK(ix.lineCount() == 5 && ix.lineStart(4) == t.size());
        editBoth(t, ix, 1, 1, u"\n\n");                   // a break replaced by two
        CHECK(startsOf(ix) == startsOf(indexOf(t)));
        // Random edits against a full index of the result.
        std::mt19937 rng(7);
        const char16_t alphabet[] = u"ab\n\n\r x";
        int mismatches = 0;
        for (int i = 0; i < 20000; i++) {
            size_t pos = t.empty() ? 0 : rng() % (t.size() + 1);
            size_t del = pos < t.size() ? rng() % std::min<size_t>(t.size() - pos + 1, 6) : 0;
            std::u16string ins;
            for (size_t n = rng() % 5; n > 0; n--) ins += alphabet[rng() % 7];
            if (t.size() > 400) del = std::min<size_t>(t.size() - pos, 40);
            editBoth(t, ix, pos, del, ins);
            if (startsOf(ix) != startsOf(indexOf(t)) || ix.length() != t.size()) mismatches++;
        }
        CHECK(mismatches == 0);
    }

    GROUP("lineindex:utf8");
    {
        // The GTK port's index: one unit per code point, GtkTextIter's
        // offsets. The same breaks as UTF-16 ('\n', a lone '\r', U+2029;
        // never U+2028), wherever the text has multi-byte characters.
        auto utf8Of = [](const std::string &t) {
            LineIndex ix;
            ix.resetUtf8(t.data(), t.size());
            return ix;
        };
        CHECK(startsOf(utf8Of("ab\ncd\n\nef")) == (std::vector<size_t>{0, 3, 6, 7}));
        CHECK(startsOf(utf8Of("a\r\nb\rc")) == (std::vector<size_t>{0, 3, 5}));
        CHECK(startsOf(utf8Of("\xC3\xA9\xE2\x80\xA9" "b\xE2\x80\xA8" "c")) ==
              (std::vector<size_t>{0, 2}));                          // é U+2029 b U+2028 c
        CHECK(utf8Of("\xF0\x9F\x98\x80\n\xF0\x9F\x98\x80").length() == 3);   // emoji, 1 unit each
        CHECK(startsOf(utf8Of("\xF0\x9F\x98\x80\nx")) == (std::vector<size_t>{0, 2}));
        CHECK(utf8Of("").lineCount() == 1 && utf8Of("x\n").lineCount() == 2);
        // A BMP-only text indexes as its UTF-16 form does.
        const std::string bmp = "caf\xC3\xA9\r\nna\xC3\xAFve\rx\xE2\x80\xA9y\n";
        const std::u16string bmp16 = u"café\r\nnaïve\rx y\n";
        CHECK(startsOf(utf8Of(bmp)) == startsOf(indexOf(bmp16)));

        // Edits as a GtkTextBuffer reports them (insert-text and
        // delete-range, character offsets, the neighbours as code points),
        // heavy in '\r', '\n' and multi-byte characters, against a full index.
        std::vector<char32_t> text = {U'x', U'\r', U'y', U'\r', U'\n', U'z'};
        auto encode = [](const std::vector<char32_t> &cps, size_t from, size_t to) {
            std::string out;
            for (size_t i = from; i < to; i++) {
                const char32_t c = cps[i];
                if (c < 0x80) out += (char)c;
                else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
                else if (c < 0x10000) {
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
        };
        LineIndex ix;
        {
            const std::string all = encode(text, 0, text.size());
            ix.resetUtf8(all.data(), all.size());
        }
        std::mt19937 rng(23);
        const char32_t alphabet[] = {U'a', U'\r', U'\n', U'\r', U'\n', 0x2029, 0x2028,
                                     0xE9, 0x1F600, U' '};
        int mismatches = 0;
        for (int i = 0; i < 20000; i++) {
            const size_t pos = text.empty() ? 0 : rng() % (text.size() + 1);
            size_t del = pos < text.size() ? rng() % std::min<size_t>(text.size() - pos + 1, 4) : 0;
            if (text.size() > 300) del = std::min<size_t>(text.size() - pos, 30);
            std::vector<char32_t> ins;
            for (size_t n = rng() % 4; n > 0; n--) ins.push_back(alphabet[rng() % 10]);
            // A replacement comes as a deletion, then an insertion.
            if (del) {
                text.erase(text.begin() + (ptrdiff_t)pos, text.begin() + (ptrdiff_t)(pos + del));
                ix.editUtf8(pos, del, nullptr, 0, pos ? text[pos - 1] : 0,
                            pos < text.size() ? text[pos] : 0);
            }
            if (!ins.empty()) {
                const std::string bytes = encode(ins, 0, ins.size());
                const char32_t before = pos ? text[pos - 1] : 0;
                const char32_t after = pos < text.size() ? text[pos] : 0;
                text.insert(text.begin() + (ptrdiff_t)pos, ins.begin(), ins.end());
                ix.editUtf8(pos, 0, bytes.data(), bytes.size(), before, after);
            }
            LineIndex full;
            const std::string all = encode(text, 0, text.size());
            full.resetUtf8(all.data(), all.size());
            // And independently: the code points' own breaks.
            std::vector<size_t> want{0};
            for (size_t k = 0; k < text.size(); k++) {
                const char32_t c = text[k], next = k + 1 < text.size() ? text[k + 1] : 0;
                if (c == U'\n' || c == 0x2029 || (c == U'\r' && next != U'\n')) want.push_back(k + 1);
            }
            if (startsOf(ix) != startsOf(full) || startsOf(ix) != want || ix.length() != text.size())
                mismatches++;
        }
        CHECK(mismatches == 0);
        // A whole replacement, as gtk_text_buffer_set_text makes one.
        const std::string fresh = "one\ntwo\r\nthree\rfour";
        ix.editUtf8(0, ix.length(), nullptr, 0, 0, 0);
        ix.editUtf8(0, 0, fresh.data(), fresh.size(), 0, 0);
        CHECK(startsOf(ix) == (std::vector<size_t>{0, 4, 9, 15}) && ix.length() == 19);
    }

    GROUP("lineindex:speed");
    {
        // 100,000 lines: one keystroke near the top shifts every start after
        // it once, which stays well under a millisecond.
        std::u16string big;
        for (int i = 0; i < 100000; i++) big += u"    value = compute(i) + 1\n";
        LineIndex ix = indexOf(big);
        CHECK(ix.lineCount() == 100001);
        auto t0 = std::chrono::steady_clock::now();
        const int reps = 200;
        for (int i = 0; i < reps; i++) {
            const char16_t c = u'\n';
            ix.edit(50, 0, &c, 1, big[49], big[50]);
            ix.edit(50, 1, nullptr, 0, big[49], big[50]);
        }
        double us = std::chrono::duration<double, std::micro>(
                        std::chrono::steady_clock::now() - t0).count() / (2 * reps);
        std::printf("  line index: %.1f us per edit on 100,000 lines\n", us);
        CHECK(ix.lineCount() == 100001 && ix.lineOf(big.size() - 1) == 99999);
    }
}

namespace {

// Spans of a text that are not code: here, anything between quotes and from
// "//" to the line's end, enough to stand in for the highlighter's tokens.
std::vector<Brackets::Span> quoted(const std::u16string &t) {
    std::vector<Brackets::Span> spans;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == u'"') {
            size_t e = t.find(u'"', i + 1);
            e = e == std::u16string::npos ? t.size() : e + 1;
            spans.push_back({i, e});
            i = e - 1;
        } else if (t.compare(i, 2, u"//") == 0) {
            size_t e = t.find(u'\n', i);
            e = e == std::u16string::npos ? t.size() : e;
            spans.push_back({i, e});
            i = e;
        }
    }
    return spans;
}

Brackets::Pair matchAt(const std::u16string &t, size_t caret, size_t scan = 20000) {
    return Brackets::match(t.data(), t.size(), caret, quoted(t), scan);
}

size_t jumpFrom(const std::u16string &t, size_t caret) {
    return Brackets::jumpTarget(t.data(), t.size(), caret, quoted(t));
}

}  // namespace

static void testBrackets() {
    using Brackets::npos;
    GROUP("brackets:match");
    {
        //                    0123456789012
        const std::u16string t = u"f(a[1], {b})";
        // The bracket before the caret first, then the one after it.
        CHECK(matchAt(t, 2).open == 1 && matchAt(t, 2).close == 11);   // after "("
        CHECK(matchAt(t, 1).open == 1 && matchAt(t, 1).close == 11);   // before "("
        CHECK(matchAt(t, 12).open == 1 && matchAt(t, 12).close == 11); // after ")"
        CHECK(matchAt(t, 4).open == 3 && matchAt(t, 4).close == 5);    // after "["
        CHECK(matchAt(t, 9).open == 8 && matchAt(t, 9).close == 10);   // after "{"
        CHECK(matchAt(t, 11).open == 8);    // after "}" beats before ")"
        CHECK(!matchAt(t, 0).found());      // before "f"
        CHECK(!matchAt(t, 7).found());      // between ", " and " "
        // Nesting of one kind.
        const std::u16string n = u"((()))";
        CHECK(matchAt(n, 1).close == 5 && matchAt(n, 2).close == 4 && matchAt(n, 3).close == 3);
        CHECK(matchAt(n, 6).open == 0);
        // Unmatched: nothing to highlight.
        CHECK(!matchAt(u"(()", 1).found());
        CHECK(!matchAt(u"a)", 2).found());
        // Kinds are counted apart, so a stray of another kind between
        // partners does not separate them.
        CHECK(matchAt(u"(a]b)", 1).close == 4);
        CHECK(matchAt(u"{ ( }", 1).close == 4);
    }

    GROUP("brackets:strings-and-comments");
    {
        //                         0          1
        //                         0123456789 0123456789
        const std::u16string t = u"f(\")\", x) // (\n)";
        // The ")" in the string is skipped: "(" at 1 pairs with ")" at 8.
        CHECK(matchAt(t, 2).open == 1 && matchAt(t, 2).close == 8);
        // A bracket inside a string or comment is no bracket.
        CHECK(!matchAt(t, 4).found() && !matchAt(t, 3).found());
        CHECK(Brackets::touching(t.data(), t.size(), 4, quoted(t)) == npos);
        CHECK(Brackets::touching(t.data(), t.size(), 13, quoted(t)) == npos);  // "// ("
        // The ")" on the last line is unmatched, the comment's "(" not counting.
        CHECK(!matchAt(t, t.size()).found());
        CHECK(Brackets::skipped(quoted(t), 3) && !Brackets::skipped(quoted(t), 1));
        CHECK(!Brackets::skipped({}, 0));
    }

    GROUP("brackets:bounded");
    {
        // A partner further away than the scan is not found, and the search
        // stops there.
        std::u16string far = u"(" + std::u16string(30000, u'x') + u")";
        CHECK(!matchAt(far, 1).found());
        CHECK(matchAt(far, 1, 40000).close == far.size() - 1);
        CHECK(!matchAt(far, far.size()).found());
        CHECK(Brackets::partner(far.data(), far.size(), 0, {}, 30001) == far.size() - 1);
        CHECK(Brackets::partner(far.data(), far.size(), 0, {}, 30000) == npos);
        // A large unbalanced file answers quickly.
        std::u16string many(200000, u'(');
        const double t0 = cpuMs();
        for (int i = 0; i < 100; i++) matchAt(many, 100000);
        const double ms = (cpuMs() - t0) / 100;
        std::printf("  bracket match: %.3f ms for a 20,000-unit scan\n", ms);
        CHECK_TIME(ms, 150);
    }

    GROUP("brackets:jump");
    {
        //                         012345678
        const std::u16string t = u"f(a, (b))";
        // VS Code's rules: from an opener to before its closer, from a closer
        // to before its opener, so pressing again comes back.
        CHECK(jumpFrom(t, 1) == 8);      // |( ... )  -> before ")"
        CHECK(jumpFrom(t, 8) == 1);      // (b)|): the ")" after the caret wins
        // Closers back to back: there and back again.
        //                       0123456
        const std::u16string nested = u"f({x})";
        CHECK(jumpFrom(nested, 2) == 5 && jumpFrom(nested, 5) == 1);
        CHECK(matchAt(nested, 5).open == 2);   // the highlight takes the "}" before
        CHECK(jumpFrom(t, 9) == 1);      // after ")"
        CHECK(jumpFrom(t, 2) == 8);      // after "(" -> before ")"
        CHECK(jumpFrom(t, 6) == 7);      // (|b): after "(" of the inner pair
        CHECK(jumpFrom(t, 7) == 5);      // (b|): before its ")"
        // An empty pair: "(|)" goes to the opener, then back.
        CHECK(jumpFrom(u"()", 1) == 0 && jumpFrom(u"()", 0) == 1);
        // Inside a pair, touching nothing: before its closer.
        CHECK(jumpFrom(u"(abc)", 2) == 4);
        CHECK(jumpFrom(u"{ x(1) y }", 7) == 9);
        // Outside any pair: the next bracket.
        CHECK(jumpFrom(u"ab (c)", 1) == 3);
        CHECK(jumpFrom(u"abc", 1) == npos);
        // Strings are skipped for the enclosing pair as for matches.
        CHECK(jumpFrom(u"(\"(\" x )", 6) == 7);
        Brackets::Pair e = Brackets::enclosing(t.data(), t.size(), 3, {});
        CHECK(e.open == 1 && e.close == 8);
    }
}
