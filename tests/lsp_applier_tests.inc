// lsp_applier_tests.inc — the `lsp:edits-hold`, `lsp:point-edits` and
// `lsp:quick-fix-range` groups of run_tests.cpp: what the GTK port's
// workspace edit applier and Quick Fix take from the core (linux/PARITY.md
// item 2.1). Whether a file's edits still fit the text the editor has, a
// file's edits as code points (what a GtkTextIter counts) for MultiEdit,
// and the range Quick Fix asks about. Included there, after the Json
// helpers.

namespace {

Lsp::TextEdit ApplierEdit(int l0, int c0, int l1, int c1, const char *text) {
    Lsp::TextEdit e;
    e.range = {{l0, c0}, {l1, c1}};
    e.newText = text;
    return e;
}

// A UTF-16 text as code points, a pair one.
std::u32string ApplierPoints(const std::u16string &u) {
    std::u32string out;
    for (size_t i = 0; i < u.size(); i++) {
        char32_t c = u[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < u.size() && u[i + 1] >= 0xDC00 &&
            u[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        }
        out += c;
    }
    return out;
}

}  // namespace

static void testLspApplier() {
    using Lsp::TextEdit;
    GROUP("lsp:edits-hold");
    {
        const std::u16string base = u"def helper():\n    return 1\n\nhelper()\n";
        const std::vector<TextEdit> rename = {ApplierEdit(0, 4, 0, 10, "assist"),
                                              ApplierEdit(3, 0, 3, 6, "assist")};
        CHECK(Lsp::editsHold(base, base, rename));
        // Typed on a line below the edits: they still hold.
        CHECK(Lsp::editsHold(base + u"# more\n", base, rename));
        // Typed elsewhere on an edit's line, after it: it holds.
        CHECK(Lsp::editsHold(u"def helper(): # x\n    return 1\n\nhelper()\n", base, rename));
        // Typed inside a range: refused.
        CHECK(!Lsp::editsHold(u"def helpXer():\n    return 1\n\nhelper()\n", base, rename));
        // A line added above: the ranges now name other text.
        CHECK(!Lsp::editsHold(u"# new\n" + base, base, rename));
        // A range on a line the text no longer has.
        CHECK(!Lsp::editsHold(u"def helper():\n", base, rename));
        // An insertion holds where the text before it is the same.
        CHECK(Lsp::editsHold(base + u"# more\n", base, {ApplierEdit(0, 0, 0, 0, "import os\n")}));
        // No edits: nothing to hold.
        CHECK(Lsp::editsHold(u"other", base, {}));
    }

    GROUP("lsp:point-edits");
    {
        // An emoji is two UTF-16 units and one code point.
        const std::u16string t = u"a\U0001F600b = 1\nb + b\n";
        const std::vector<TextEdit> e = {ApplierEdit(0, 3, 0, 4, "cc"), ApplierEdit(1, 0, 1, 1, "cc"),
                                         ApplierEdit(1, 4, 1, 5, "\xC3\xA9")};
        std::vector<Lsp::PointEdit> p = Lsp::pointEdits(t, e);
        CHECK(p.size() == 3);
        if (p.size() == 3) {
            CHECK(p[0].start == 2 && p[0].length == 1 && p[0].text == "cc");
            CHECK(p[1].start == 8 && p[1].length == 1 && p[1].text == "cc");
            CHECK(p[2].start == 12 && p[2].length == 1 && p[2].text == "\xC3\xA9");
        }
        // An offset inside the pair counts as its start.
        std::vector<Lsp::PointEdit> q = Lsp::pointEdits(t, {ApplierEdit(0, 2, 0, 2, "x")});
        CHECK(q.size() == 1 && q[0].start == 1 && q[0].length == 0);
        // Edits already in units (a completion's), ascending: the same answer.
        const std::vector<Lsp::OffsetEdit> units = {{3, 1, u"cc"}, {9, 1, u"cc"}, {13, 1, u"é"}};
        std::vector<Lsp::PointEdit> r = Lsp::pointEdits(t, units);
        CHECK(r.size() == 3);
        if (r.size() == 3) {
            CHECK(r[0].start == 2 && r[0].length == 1 && r[0].text == "cc");
            CHECK(r[1].start == 8 && r[1].length == 1 && r[1].text == "cc");
            CHECK(r[2].start == 12 && r[2].length == 1 && r[2].text == "\xC3\xA9");
        }
        CHECK(Lsp::pointEdits(t, std::vector<Lsp::OffsetEdit>{}).empty());
        // Applied as code points, last first, they make what applyTextEdits
        // makes, on random texts and edits.
        unsigned seed = 11;
        auto next = [&] { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7fff; };
        bool same = true, ascending = true;
        for (int round = 0; round < 300; round++) {
            std::u16string u;
            const int n = (int)(next() % 40);
            for (int k = 0; k < n; k++) {
                unsigned r = next() % 9;
                if (r == 0) u += u'\n';
                else if (r == 1) u += u"\U0001F600";
                else if (r == 2) u += u"é";
                else u += (char16_t)(u'a' + r);
            }
            const Lsp::PositionIndex index(u);
            // Ascending offsets, never between a pair's halves, paired up
            // into edits that touch at most.
            std::vector<size_t> at;
            for (int k = 0; k < 6; k++) {
                size_t o = u.empty() ? 0 : next() % (u.size() + 1);
                if (o > 0 && o < u.size() && u[o] >= 0xDC00 && u[o] <= 0xDFFF) o--;
                at.push_back(o);
            }
            std::sort(at.begin(), at.end());
            std::vector<TextEdit> edits;
            for (size_t k = 0; k + 1 < at.size(); k += 2) {
                TextEdit te;
                te.range = {index.position(at[k]), index.position(at[k + 1])};
                te.newText = next() % 2 ? "X\xF0\x9F\x98\x80" : "";
                edits.push_back(te);
            }
            const std::vector<Lsp::PointEdit> pe = Lsp::pointEdits(u, edits);
            std::u32string pts = ApplierPoints(u);
            for (size_t k = 1; k < pe.size(); k++)
                if (pe[k].start < pe[k - 1].start + pe[k - 1].length) ascending = false;
            for (size_t k = pe.size(); k-- > 0;) {
                if (pe[k].start + pe[k].length > pts.size()) {
                    same = false;
                    break;
                }
                pts.replace(pe[k].start, pe[k].length, ApplierPoints(Lsp::toUtf16(pe[k].text)));
            }
            if (pts != ApplierPoints(Lsp::applyTextEdits(u, edits))) same = false;
        }
        CHECK(same);
        CHECK(ascending);
    }

    GROUP("lsp:quick-fix-range");
    {
        // clang's missing ';' at the next token: the caret at the line's start.
        Json params;
        CHECK(Json::parse(R"({"uri": "file:///p/a.c", "diagnostics": [
            {"range": {"start": {"line": 2, "character": 10}, "end": {"line": 2, "character": 11}},
             "severity": 1, "message": "expected ';'"},
            {"range": {"start": {"line": 5, "character": 0}, "end": {"line": 5, "character": 4}},
             "severity": 2, "message": "unused"}]})", params));
        const std::vector<Lsp::Diagnostic> d = Lsp::parseDiagnostics(params);
        CHECK(d.size() == 2);
        Lsp::Range r = Lsp::quickFixRange(d, {{2, 0}, {2, 0}});
        CHECK(r.start == (Lsp::Position{2, 10}) && r.end == (Lsp::Position{2, 11}));
        // On the problem: the caret as it is.
        r = Lsp::quickFixRange(d, {{5, 2}, {5, 2}});
        CHECK(r.start == (Lsp::Position{5, 2}) && r.end == (Lsp::Position{5, 2}));
        // A selection is asked about as it is.
        r = Lsp::quickFixRange(d, {{2, 0}, {2, 3}});
        CHECK(r.start == (Lsp::Position{2, 0}) && r.end == (Lsp::Position{2, 3}));
        // A line with no problem: the caret.
        r = Lsp::quickFixRange(d, {{3, 1}, {3, 1}});
        CHECK(r.start == (Lsp::Position{3, 1}) && r.end == (Lsp::Position{3, 1}));
        // And what the context sends is the diagnostic as it came.
        Json ctx = Lsp::diagnosticsInRange(d, Lsp::quickFixRange(d, {{2, 0}, {2, 0}}));
        CHECK(ctx.size() == 1 && ctx[0]["message"].asString() == "expected ';'");
    }
}
