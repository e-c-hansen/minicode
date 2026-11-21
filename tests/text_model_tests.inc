// text_model_tests.inc — the `text:*`, `document:*` and `lsp:sync-code-points`
// groups of run_tests.cpp: the core's document model (Text.h, Document.h,
// SESSION-DESIGN.md stage 1). Text's code points and apply() are held to a
// plain std::u16string under random edits; Document's rules to the Mac's
// (dirty means differs from the saved text, stamps, encodings and marks);
// and the client's code point didChange, which the GTK port sends, to a
// server applying what it is sent. Included there, after lsp_sync_tests.inc.

#include "Document.h"

namespace {

// Code point `point` of a plain string, in units (as Text::unitOf).
size_t plainUnitOf(const std::u16string &s, size_t point) {
    return EditSpans::unitsAt(s, point);
}

// Unit `unit` of a plain string as a code point (as Text::codePointOf).
size_t plainCodePointOf(const std::u16string &s, size_t unit) {
    return EditSpans::codePoints(s, {std::min(unit, s.size())})[0];
}

// The edits applied to a plain string, last first, in units.
std::u16string plainApply(std::u16string s, const std::vector<Model::Text::Edit> &edits,
                          Model::Unit u) {
    std::vector<std::pair<size_t, size_t>> at;
    for (const Model::Text::Edit &e : edits) {
        size_t a = e.start, b = e.start + e.length;
        if (u == Model::Unit::CodePoint) a = plainUnitOf(s, a), b = plainUnitOf(s, b);
        at.push_back({a, b});
    }
    for (size_t i = edits.size(); i-- > 0;) s.replace(at[i].first, at[i].second - at[i].first, edits[i].text);
    return s;
}

FolderSearch::FileStamp stampAt(int64_t sec, uint64_t inode, uint64_t size) {
    FolderSearch::FileStamp s;
    s.exists = s.regular = true;
    s.mtimeSec = sec;
    s.inode = inode;
    s.size = size;
    return s;
}

}  // namespace

static void testTextModel() {
    GROUP("text:code-points");
    {
        // Code points counted through random replacements, small texts and
        // texts over many chunks, pairs cut in two by an edit and joined
        // again, lone surrogates, a pair across a chunk's edge: the count,
        // and both ways between units and code points at every offset (or
        // many, for the large ones), agree with a plain string's.
        std::mt19937 rng(16112026);
        const std::u16string pieces[] = {u"a", u"\U0001F600", u"\n", u"\r\n", u"é",
                                         std::u16string(1, (char16_t)0xD83D),   // a lone high half
                                         std::u16string(1, (char16_t)0xDE00),   // a lone low half
                                         u"xyz"};
        auto randomText = [&](size_t n) {
            std::u16string s;
            while (s.size() < n) s += pieces[rng() % 8];
            return s;
        };
        bool same = true;
        for (int round = 0; round < 600 && same; round++) {
            const bool big = round % 10 == 0;
            std::u16string mirror = randomText(big ? 1 + rng() % 9000 : rng() % 30);
            Model::Text t(mirror);
            for (int step = 0; step < (big ? 30 : 10) && same; step++) {
                const size_t a = rng() % (mirror.size() + 1);
                const size_t span = big && rng() % 4 == 0 ? 3000 : 6;
                const size_t b = a + rng() % (std::min(span, mirror.size() - a) + 1);
                const std::u16string with =
                    big && rng() % 8 == 0 ? randomText(rng() % 4000) : randomText(rng() % 5);
                t.replace(a, b, with);
                mirror.replace(a, b - a, with);
                same = t == mirror && t.codePoints() == Model::countCodePoints(mirror);
                std::vector<size_t> at;
                if (mirror.size() < 80) {
                    for (size_t k = 0; k <= mirror.size() + 1; k++) at.push_back(k);
                } else {
                    for (int k = 0; k < 120; k++) at.push_back(rng() % (mirror.size() + 1));
                    for (size_t e = 0; e <= mirror.size(); e += Model::Text::kChunk / 4)
                        for (size_t d = 0; d < 3; d++)
                            if (e + d >= 1) at.push_back(std::min(mirror.size(), e + d - 1));
                }
                for (size_t k : at) {
                    const size_t unit = std::min(k, mirror.size());
                    same = same && t.codePointOf(k) == plainCodePointOf(mirror, unit);
                    if (k <= t.codePoints())
                        same = same && t.unitOf(k) == plainUnitOf(mirror, k);
                }
                same = same && t.unitOf(t.codePoints() + 3) == mirror.size() &&
                       t.codePointOf(mirror.size() + 3) == t.codePoints();
            }
        }
        CHECK(same);
        // A pair cut by a chunk's edge counts once, and as its start.
        std::u16string edge(Model::Text::kChunk - 1, u'a');
        edge += u"\U0001F600b";
        Model::Text e(edge);
        CHECK(e.chunks() == 2 && e.codePoints() == Model::Text::kChunk + 1);
        CHECK(e.codePointOf(Model::Text::kChunk) == Model::Text::kChunk - 1 &&
              e.unitOf(Model::Text::kChunk) == Model::Text::kChunk + 1);
        e.replace(Model::Text::kChunk - 1, Model::Text::kChunk, u"");   // the high half goes
        CHECK(e.codePoints() == Model::Text::kChunk + 1 && e.size() == Model::Text::kChunk + 1);
        e.replace(Model::Text::kChunk - 1, Model::Text::kChunk - 1,
                  std::u16string(1, (char16_t)0xD83D));   // and comes back
        CHECK(e.codePoints() == Model::Text::kChunk + 1 && e == edge);
        CHECK(Model::utf8FromUtf16(u"\U0001F600é") == "\xF0\x9F\x98\x80\xC3\xA9" &&
              Model::utf16FromUtf8("\xF0\x9F\x98\x80\xC3\xA9") == u"\U0001F600é" &&
              Model::hasLoneSurrogate(std::u16string(1, (char16_t)0xDE00)) &&
              !Model::hasLoneSurrogate(u"\U0001F600"));
    }

    GROUP("text:apply-oracle");
    {
        // Random batches of edits, in units and in code points, against a
        // plain string applying them last first: the text, its lines and
        // code points agree; each batch moves the version on once; a batch
        // out of order, overlapping or past the end changes nothing.
        std::mt19937 rng(20261116);
        const std::u16string pieces[] = {u"ab", u"\n", u"\r", u"\r\n", u"\U0001F42A", u"é", u" "};
        auto randomText = [&](size_t n) {
            std::u16string s;
            while (s.size() < n) s += pieces[rng() % 7];
            return s;
        };
        size_t batches = 0, wrong = 0, refusedRight = 0, refused = 0;
        for (int round = 0; round < 3000; round++) {
            std::u16string mirror = randomText(round % 20 == 0 ? 5000 : rng() % 60);
            Model::Text t(mirror);
            for (int step = 0; step < 6; step++) {
                const Model::Unit u = rng() % 2 ? Model::Unit::CodePoint : Model::Unit::Utf16;
                const size_t total = t.length(u);
                std::vector<Model::Text::Edit> edits;
                size_t at = 0;
                const int n = (int)(rng() % 6);
                for (int i = 0; i < n && at <= total; i++) {
                    Model::Text::Edit e;
                    e.start = at + rng() % (std::min<size_t>(total - at, 40) + 1);
                    e.length = rng() % (std::min<size_t>(total - e.start, 5) + 1);
                    e.text = randomText(rng() % 4);
                    at = e.start + e.length;
                    edits.push_back(e);
                }
                const bool spoil = rng() % 8 == 0 && edits.size() >= 2;
                if (spoil) std::swap(edits[0], edits[edits.size() - 1]);
                const bool apart = [&] {
                    size_t last = 0;
                    for (const auto &e : edits) {
                        if (e.start < last) return false;
                        last = e.start + e.length;
                    }
                    return true;
                }();
                const uint64_t v = t.version();
                const std::u16string before = mirror;
                const bool ok = t.apply(edits, u);
                if (!apart) {
                    refused++;
                    if (!ok && t == before && t.version() == v) refusedRight++;
                    continue;
                }
                mirror = plainApply(mirror, edits, u);
                batches++;
                const bool changed = mirror != before;
                if (!ok || !(t == mirror) || t.codePoints() != Model::countCodePoints(mirror) ||
                    !(lspPos(t.position(mirror.size())) ==
                      Lsp::PositionIndex(mirror).position(mirror.size())) ||
                    (changed && t.version() != v + 1) || (!changed && t.version() > v + 1))
                    wrong++;
            }
        }
        CHECK(batches > 10000 && wrong == 0 && refused > 100 && refusedRight == refused);
        if (wrong) std::printf("  %zu batches left the text wrong\n", wrong);
        // Past the end, and in EditSpans' shape.
        Model::Text t(u"héllo \U0001F600 world");
        CHECK(!t.apply({{18, 1, u"x"}}) && !t.apply({{17, 1, u""}}, Model::Unit::CodePoint));
        CHECK(t.apply({{0, 1, u"H"}, {6, 1, u"\U0001F642"}}, Model::Unit::CodePoint) &&
              t == u"Héllo \U0001F642 world" && t.version() == 1);
        CHECK(t.apply(std::vector<EditSpans::Span>{{0, 1, 2}, {8, 0, 1}},
                      std::vector<std::u16string>{u"JJ", u"!"}) &&
              t == u"JJéllo \U0001F642! world");
        // A new text's length must be the span's newLength, in the unit given.
        CHECK(!t.apply(std::vector<EditSpans::Span>{{0, 0, 2}},
                       std::vector<std::u16string>{u"\U0001F600"}, Model::Unit::CodePoint));
        CHECK(t.apply(std::vector<EditSpans::Span>{{0, 0, 1}},
                      std::vector<std::u16string>{u"\U0001F600"}, Model::Unit::CodePoint) &&
              t.slice(0, 3) == u"\U0001F600J" && t.slice(t.size() - 2, 9) == u"ld");
    }

    GROUP("text:speed");
    {
        // Code points cost what a lookup costs, not the file: 10,000 of
        // each way in a 16 MB text with a character outside the BMP on every
        // line (so no chunk takes the short way of a chunk without pairs),
        // against the same lookups in a 64 KB text of the same lines, timed
        // in the same run, so a loaded machine slows both alike. A cost that
        // grew with the file would be about 256 times the small one.
        auto lookups = [](size_t units, size_t* sum) {
            std::u16string text;
            while (text.size() < units) text += u"    let \U0001F600 = é(42);\n";
            Model::Text t(text);
            std::mt19937 rng(7);
            const double t0 = cpuMs();
            for (int i = 0; i < 10000; i++) {
                *sum += t.unitOf(rng() % t.codePoints());
                *sum += t.codePointOf(rng() % t.size());
            }
            return cpuMs() - t0;
        };
        size_t sum = 0;
        const double small = lookups(64u << 10, &sum);
        const double spent = lookups(16u << 20, &sum);
        std::printf("  text: 20,000 lookups in 16 MB, %.2f ms of CPU (%.2f in 64 KB)\n", spent,
                    small);
        CHECK(sum > 0);
        CHECK_TIME(spent, std::max(400.0, 8 * small));
    }

    GROUP("document:dirty");
    {
        // Dirty means differs from the saved text: typed and deleted, or
        // undone by hand, leaves none; a file gone is dirty; a save makes
        // the text the saved one.
        Model::Document d;
        CHECK(!d.dirty());
        CHECK(d.load("alpha\nbeta\n", stampAt(100, 7, 11)) && !d.dirty() &&
              d.text() == u"alpha\nbeta\n" && d.savedText() == u"alpha\nbeta\n");
        const uint64_t v = d.version();
        d.replace(5, 5, u"x");
        CHECK(d.dirty() && d.version() == v + 1);
        d.replace(5, 6, u"");
        CHECK(!d.dirty() && d.version() == v + 2);
        CHECK(d.apply({{0, 1, u"A"}}) && d.dirty());
        d.markSaved(stampAt(101, 7, 11));
        CHECK(!d.dirty() && d.savedText() == u"Alpha\nbeta\n" && d.stamp().mtimeSec == 101);
        d.setGone(true);
        CHECK(d.dirty());
        // A reload is one change of the text, saved; the version moves on.
        const uint64_t w = d.version();
        CHECK(d.reload("new\n", stampAt(102, 8, 4)) && !d.dirty() && !d.gone() &&
              d.text() == u"new\n" && d.version() == w + 1);
        // Bytes that are not text change nothing.
        CHECK(!d.reload(std::string("\xFF\xFE\x00", 3), stampAt(103, 8, 3)) && d.text() == u"new\n" &&
              d.stamp().mtimeSec == 102);
        CHECK(!d.load(std::string("a\xC3(\0", 4), stampAt(104, 8, 4)) && d.text() == u"new\n");
        // Not UTF-8 and no NUL: Windows-1252 text (since 2026-11-18, C3b).
        CHECK(d.load("a\xC3(", stampAt(104, 8, 3)) && d.text() == u"a\u00C3(" &&
              d.encoding() == FolderSearch::TextEncoding::Windows1252);
    }

    GROUP("document:stamp");
    {
        // The Mac's rule (tab:changedOnDisk:): the date, the inode or the
        // size moved; missing and back; two missing files are the same.
        const FolderSearch::FileStamp a = stampAt(100, 7, 11), none;
        FolderSearch::FileStamp later = a, other = a, bigger = a, nsec = a;
        later.mtimeSec = 101;
        other.inode = 8;
        bigger.size = 12;
        nsec.mtimeNsec = 5;
        CHECK(!Model::stampChanged(a, a) && !Model::stampChanged(none, none));
        CHECK(Model::stampChanged(a, later) && Model::stampChanged(a, other) &&
              Model::stampChanged(a, bigger) && Model::stampChanged(a, nsec));
        CHECK(Model::stampChanged(a, none) && Model::stampChanged(none, a));
        Model::Document d;
        CHECK(d.load("x", a) && !d.changedOnDisk(a) && d.changedOnDisk(later));
    }

    GROUP("document:encoding");
    {
        // What was read is what is written, byte for byte: UTF-8 with and
        // without its mark, UTF-16 in either order with its mark; an edit
        // keeps the encoding; a lone surrogate is not written; UTF-16 that
        // does not decode exactly is not text.
        const std::string files[] = {
            "plain \xC3\xA9\r\n",
            "\xEF\xBB\xBFmarked\n",
            std::string("\xFF\xFE" "a\0\x3D\xD8\x00\xDE\n\0", 10),   // LE: a, U+1F600, \n
            std::string("\xFE\xFF\0b\0\n", 6),                       // BE: b, \n
        };
        bool roundTrips = true;
        for (const std::string &f : files) {
            Model::Document d;
            std::string out, why;
            roundTrips = roundTrips && d.load(f, stampAt(1, 1, f.size())) &&
                         d.bytesToWrite(&out, &why) && out == f;
        }
        CHECK(roundTrips);
        Model::Document le;
        CHECK(le.load(files[2], stampAt(1, 1, 10)) &&
              le.encoding() == FolderSearch::TextEncoding::Utf16LE && le.bom() &&
              le.text() == u"a\U0001F600\n" && le.text().codePoints() == 3);
        le.replace(0, 1, u"z");
        std::string out, why;
        CHECK(le.bytesToWrite(&out, &why) && out == std::string("\xFF\xFE" "z\0\x3D\xD8\x00\xDE\n\0", 10));
        le.replace(1, 2, u"");   // half a pair left behind
        CHECK(!le.bytesToWrite(&out, &why) && !why.empty());
        Model::Document marked;
        CHECK(marked.load(files[1], stampAt(1, 1, 10)) && marked.bom() &&
              marked.encoding() == FolderSearch::TextEncoding::Utf8 && marked.text() == u"marked\n");
        Model::Document odd;
        CHECK(!odd.load(std::string("\xFF\xFE" "a\0b", 5), stampAt(1, 1, 5)));
    }

    GROUP("document:large");
    {
        // VS Code's thresholds, as LargeFile.h has them.
        Model::Document small, many;
        CHECK(small.load("a\nb\n", stampAt(1, 1, 4)) && !small.large());
        std::string lines;
        for (int i = 0; i < 300001; i++) lines += "\n";
        CHECK(many.load(lines, stampAt(1, 1, lines.size())) && many.large());
        // Counted over the text's chunks: a \r ending one chunk and a \n
        // starting the next are one break, as in the whole string. "x" puts
        // every \r at an odd unit, and each chunk ends at one.
        static_assert(Model::Text::kChunk % 2 == 0, "a \r ends each chunk below");
        for (size_t breaks : {LargeFile::kLineCountThreshold - 1, LargeFile::kLineCountThreshold}) {
            std::u16string t = u"x";
            for (size_t i = 0; i < breaks; i++) t += u"\r\n";
            const Model::Document d(t);
            CHECK(d.large() == LargeFile::isLargeText(t.data(), t.size()));
            CHECK(d.large() == (breaks == LargeFile::kLineCountThreshold));
        }
    }

    GROUP("text:from-utf8");
    {
        // Decoded straight into chunks, unit for unit as utf16FromUtf8:
        // bytes that are not UTF-8, a sequence cut short at the end, and a
        // pair or a three-byte character across a chunk's edge.
        std::mt19937 rng(23);
        const std::string bits[] = {"a", "\n", "\r", "\r\n", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80",
                                    "\xFF", "\xC3", "\xE2\x82", "\x80", "\xED\xA0\x80"};
        bool same = true;
        for (int round = 0; round < 300; round++) {
            std::string s;
            const int n = round < 100 ? (int)(rng() % 40) : (int)(rng() % 3000);
            for (int i = 0; i < n; i++) s += bits[rng() % (sizeof bits / sizeof bits[0])];
            const std::u16string want = Model::utf16FromUtf8(s);
            const Model::Text t = Model::Text::fromUtf8(s), u(want);
            same = same && t == want && t.size() == want.size() && t.lines() == u.lines() &&
                   t.codePoints() == u.codePoints() && t.position(t.size()) == u.position(u.size());
        }
        CHECK(same);
        for (size_t at : {Model::Text::kChunk - 1, Model::Text::kChunk - 2}) {
            std::string s(at, 'a');
            s += "\xF0\x9F\x98\x80\xE2\x82\xAC" "b";
            const Model::Text t = Model::Text::fromUtf8(s);
            CHECK(t == Model::utf16FromUtf8(s) && t.codePoints() == at + 3 && t.chunks() == 2);
        }
        CHECK(Model::Text::fromUtf8("").size() == 0 && Model::Text::fromUtf8("").chunks() == 0);
    }

    GROUP("text:frozen");
    {
        // A text frozen keeps what it was, whatever is done to the text
        // after, and compares with it exactly: chunks still shared are
        // skipped, and a text edited back to the same units is equal again
        // though no chunk is shared any more.
        std::u16string plain;
        for (int i = 0; i < 5000; i++) plain += u"line " + Model::utf16FromUtf8(std::to_string(i)) + u"\n";
        Model::Text t(plain);
        const Model::Frozen f = t.freeze();
        CHECK(t == f && f == plain && f.size() == plain.size() && f.str() == plain);
        t.replace(10, 11, u"Z");
        CHECK(!(t == f) && f == plain && f.str() == plain);
        t.replace(10, 11, plain.substr(10, 1));
        CHECK(t == f);
        // Same length, a unit changed far from the first edit.
        t.replace(plain.size() - 3, plain.size() - 2, u"?");
        CHECK(!(t == f));
        t.replace(plain.size() - 3, plain.size() - 2, plain.substr(plain.size() - 3, 1));
        CHECK(t == f);
        // Cut in other places: everything moved by a unit at the start.
        t.replace(0, 0, u"Q");
        t.replace(0, 1, u"");
        CHECK(t == f && t.str() == plain);
        // Its own text, and an empty one.
        const Model::Frozen own(plain), none;
        CHECK(t == own && own == plain && none.empty() && none == u"" && Model::Text() == none);
        CHECK(!(Model::Text(u"a") == none) && !(Model::Text() == own));
        // A copy of a Text shares its chunks and is changed on its own.
        Model::Text copy = t;
        copy.replace(0, 1, u"#");
        CHECK(t == plain && copy != plain && copy.str().substr(1) == plain.substr(1));
    }

    GROUP("document:saved-shared");
    {
        // The saved text is the text's chunks: dirty compares exactly, a
        // save or a reload makes the text the saved one again, and
        // markSavedAs keeps a text of its own only when it differs.
        std::u16string plain;
        for (int i = 0; i < 3000; i++) plain += u"0123456789abcdef\n";
        Model::Document d(plain);
        CHECK(!d.dirty() && d.savedIs(plain) && d.savedSize() == plain.size() && d.savedText() == plain);
        d.replace(4000, 4001, u"!");
        CHECK(d.dirty() && d.differsFromSaved() && d.savedIs(plain));
        d.replace(4000, 4001, plain.substr(4000, 1));
        CHECK(!d.dirty() && !d.differsFromSaved());
        d.replace(0, 0, u"new ");
        d.markSaved(stampAt(5, 1, 1));
        CHECK(!d.dirty() && d.savedIs(u"new " + plain));
        d.markSavedAs(plain);
        CHECK(d.dirty() && d.savedIs(plain) && d.savedText() == plain);
        d.markSavedAs(u"new " + plain);
        CHECK(!d.dirty() && d.savedIs(u"new " + plain));
        d.setGone(true);
        CHECK(d.dirty() && !d.differsFromSaved());
    }

    GROUP("lsp:sync-code-points");
    {
        // An editor that counts code points (the GTK port's buffer), with
        // characters outside the BMP, CRLF and lone CRs, flushed a few
        // times after random edits: the server applying what it is sent
        // holds the editor's text, and so does the client's copy. Until
        // stage 1 the GTK port sent the whole text whenever a character
        // outside the BMP was in it.
        std::mt19937 rng(1611);
        const std::u16string pieces[] = {u"a", u"de", u"\n", u"\r\n", u"\r", u"\U0001F42A",
                                         u"\U0001F600\U0001F600", u"é", u" "};
        auto randomText = [&](int n) {
            std::u16string s;
            for (int i = 0; i < n; i++) s += pieces[rng() % 9];
            return s;
        };
        size_t wrong = 0, flushes = 0;
        size_t ranged = 0, full = 0;
        for (int round = 0; round < 3000; round++) {
            FakeServer srv;
            Lsp::Client client([&](const std::string &b) { srv.take(b); });
            client.initialize("/p", 1);
            client.receive(reply(1, "{\"capabilities\":{\"textDocumentSync\":2}}"));
            const std::string uri = "file:///p/a.py";
            std::u16string buffer = randomText(40);
            for (int k = 0; k < 12; k++)
                buffer += u"# padding \U0001F600 line " +
                          std::u16string(k % 2 ? u"\r\n" : u"\n") + randomText(3);
            client.didOpen(uri, "python", Lsp::toUtf8(buffer));
            std::u16string server = buffer;
            for (int flush = 0; flush < 4; flush++) {
                Lsp::ChangeTracker t;
                const int edits = 1 + (int)(rng() % 5);
                for (int e = 0; e < edits; e++) {
                    const size_t points = Model::countCodePoints(buffer);
                    const size_t a = rng() % (points + 1);
                    const size_t removed = std::min<size_t>(rng() % 5, points - a);
                    const std::u16string with = randomText((int)(rng() % 4));
                    const size_t ua = plainUnitOf(buffer, a), ub = plainUnitOf(buffer, a + removed);
                    buffer.replace(ua, ub - ua, with);
                    t.edit(a, removed, Model::countCodePoints(with));
                }
                const size_t before = srv.sent.size();
                client.didChange(
                    uri, Model::countCodePoints(buffer), t,
                    [&](size_t a, size_t n) {
                        const size_t ua = plainUnitOf(buffer, a), ub = plainUnitOf(buffer, a + n);
                        return buffer.substr(ua, ub - ua);
                    },
                    Model::Unit::CodePoint);
                for (size_t m = before; m < srv.sent.size(); m++)
                    for (const Json &c : srv.sent[m]["params"]["contentChanges"].elements()) {
                        if (c.has("range")) {
                            const Json &r = c["range"];
                            server = Lsp::applyContentChanges(
                                server, {{{{(int)r["start"]["line"].asInt(),
                                            (int)r["start"]["character"].asInt()},
                                           {(int)r["end"]["line"].asInt(),
                                            (int)r["end"]["character"].asInt()}},
                                          c["text"].asString()}});
                        } else {
                            server = Lsp::toUtf16(c["text"].asString());
                        }
                    }
                auto copy = client.textFor(uri);
                flushes++;
                if (server != buffer || !copy || *copy != buffer) wrong++;
            }
            ranged += client.rangedChangesSent();
            full += client.fullChangesSent();
        }
        CHECK(flushes == 12000 && wrong == 0);
        if (wrong) std::printf("  %zu code point flushes left the server's copy wrong\n", wrong);
        // Many go as ranges, characters outside the BMP or not (random
        // offsets between \r and \n, and ranges costing as much as these
        // short texts, send the whole text, as in lsp:sync-oracle).
        CHECK(ranged > 4000);
        std::printf("  sync code points: %zu ranged, %zu whole\n", ranged, full);

        // A pause after typing beside an emoji in a 2.5 MB file: ranges, the
        // regions' text alone read.
        FakeServer srv;
        Lsp::Client client([&](const std::string &b) { srv.take(b); });
        client.initialize("/p", 1);
        client.receive(reply(1, "{\"capabilities\":{\"textDocumentSync\":2}}"));
        std::u16string buffer;
        while (buffer.size() < 2500000) buffer += u"x = \U0001F600 # é\n";
        client.didOpen("file:///p/big.py", "python", Lsp::toUtf8(buffer));
        Lsp::ChangeTracker t;
        const size_t mid = Model::countCodePoints(buffer) / 2;
        const size_t umid = plainUnitOf(buffer, mid);
        buffer.insert(umid, u"\U0001F642");
        t.edit(mid, 0, 1);
        // Where each code point starts, so reading costs what GTK's does.
        std::vector<uint32_t> starts;
        for (size_t i = 0; i < buffer.size(); i++)
            if (!(buffer[i] >= 0xDC00 && buffer[i] <= 0xDFFF && i > 0 && buffer[i - 1] >= 0xD800 &&
                  buffer[i - 1] <= 0xDBFF))
                starts.push_back((uint32_t)i);
        starts.push_back((uint32_t)buffer.size());
        size_t readUnits = 0;
        const double t0 = cpuMs();
        client.didChange(
            "file:///p/big.py", starts.size() - 1, t,
            [&](size_t a, size_t n) {
                const size_t ua = starts[a], ub = starts[a + n];
                readUnits += ub - ua;
                return buffer.substr(ua, ub - ua);
            },
            Model::Unit::CodePoint);
        const double spent = cpuMs() - t0;
        CHECK(client.rangedChangesSent() == 1 && client.fullChangesSent() == 0 &&
              *client.textFor("file:///p/big.py") == buffer && readUnits == 2);
        std::printf("  sync code points: a 2.5 MB flush %.2f ms of CPU\n", spent);
        CHECK_TIME(spent, 20);
    }
}
