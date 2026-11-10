// edit_batch_tests.inc — the `editbatch:*` groups of run_tests.cpp: a batch
// of edits (typing at many carets, Replace All) given to the incremental
// highlighter at once (IncrementalHighlighter::editMany, EditBatch.h)
// leaves it exactly as edit() of each in turn would, and as a fresh lex of
// the result, re-lexing only around each edit. Included there.

static void testEditBatch() {
    // Random changes over a text: ascending, apart, some touching, some at
    // the same place, insertions, deletions and replacements, now and then
    // with line breaks, quotes and comment openers in them.
    auto randomChanges = [](std::mt19937 &rng, const std::string &text, size_t count) {
        static const char *pieces[] = {"x",  "ab", "\n",  "\"", "'", "/*", "*/", "#",  "(",
                                       ")",  "\r\n", "\"\"\"", "é", "  ", "\t", "}", "{", "//",
                                       "\r", "`", "${", "\n\n"};
        std::vector<size_t> at;
        for (size_t i = 0; i < count; i++) at.push_back(text.empty() ? 0 : rng() % (text.size() + 1));
        std::sort(at.begin(), at.end());
        std::vector<EditBatch::Change> out;
        size_t end = 0;
        for (size_t p : at) {
            if (p < end) continue;
            // Never inside a UTF-8 sequence.
            while (p < text.size() && ((unsigned char)text[p] & 0xC0) == 0x80) p++;
            if (p < end) continue;
            EditBatch::Change c;
            c.pos = p;
            const unsigned kind = rng() % 4;
            if (kind != 0 && p < text.size()) {
                size_t len = 1 + rng() % 4;
                len = std::min(len, text.size() - p);
                while (p + len < text.size() && ((unsigned char)text[p + len] & 0xC0) == 0x80) len++;
                c.oldLen = len;
            }
            if (kind != 1) c.text = pieces[rng() % (sizeof pieces / sizeof *pieces)];
            out.push_back(c);
            end = c.pos + c.oldLen;
        }
        return out;
    };
    auto randomText = [](std::mt19937 &rng, const std::string &ext, size_t lines) {
        static const char *py[] = {"def f(x):\n", "    return x + 1  # one\n", "s = 'it''s'\n",
                                   "\"\"\"doc\nstring\"\"\"\n", "x = f\"{a}\"\n", "\n", "\r\n",
                                   "class C:\n", "    pass\r\n", "t = \"é\"\n"};
        static const char *cpp[] = {"int f(int x) {\n", "  return x; // c\n", "}\n", "/* a\n",
                                    " b */\n", "const char *s = \"x\\\"y\";\n", "\n", "#include <a>\n",
                                    "auto r = R\"(raw\n)\";\n", "x = 'c';\r\n"};
        static const char *js[] = {"const a = `t ${b}`;\n", "let r = /x\\/y/g;\n", "// c\n",
                                   "function f() {\n", "}\n", "x = a / b / c;\n", "\n",
                                   "s = \"é\";\n", "o = {a: 1};\r\n", "`multi\nline`;\n"};
        const char **table = ext == "py" ? py : ext == "cpp" ? cpp : js;
        std::string s;
        for (size_t i = 0; i < lines; i++) s += table[rng() % 10];
        return s;
    };
    // Every line's start, end state and tokens, as a highlighter holds them.
    auto snapshot = [](IncrementalHighlighter<char> &h, const std::string &text) {
        std::vector<std::string> out;
        StringSource<char> src(text);
        std::vector<Token> toks;
        h.lineTokens(src, 0, h.lineCount(), toks);
        for (size_t k = 0; k < h.lineCount(); k++) {
            const LexState st = h.endState(k);
            out.push_back("L" + std::to_string(h.lineStart(k)) + ":" +
                          std::to_string((int)st.kind) + "/" + std::to_string((int)st.quote));
        }
        for (const Token &t : toks)
            out.push_back(std::to_string(t.start) + "+" + std::to_string(t.length) + "=" +
                          std::to_string((int)t.style));
        return out;
    };
    // The text's lines as the highlighter cuts them.
    auto linesOf = [](const IncrementalHighlighter<char> &h, const std::string &text) {
        std::vector<std::string> out;
        for (size_t k = 0; k < h.lineCount(); k++) {
            const size_t a = h.lineStart(k), b = k + 1 < h.lineCount() ? h.lineStart(k + 1) : text.size();
            out.push_back(text.substr(a, b - a));
        }
        return out;
    };

    GROUP("editbatch:applied");
    {
        CHECK(EditBatch::applied("abcdef", {{1, 2, "X"}, {3, 0, "Y"}, {6, 0, "Z"}}) == "aXYdefZ");
        CHECK(EditBatch::applied("abc", {}) == "abc");
        // valid() refuses overlaps, a change past the end and disorder.
        CHECK(!EditBatch::valid({{4, 2, ""}, {5, 0, "x"}}, 10));
        CHECK(!EditBatch::valid({{5, 0, "x"}, {4, 0, "y"}}, 10));
        CHECK(!EditBatch::valid({{9, 2, ""}}, 10));
        CHECK(EditBatch::valid({{4, 2, ""}, {6, 0, "x"}, {6, 0, "y"}, {10, 0, "z"}}, 10));
    }

    GROUP("editbatch:highlighter");
    {
        // After a batch the highlighter is exactly what edit() of each
        // change, last first, leaves, and a fresh lex of the result: line
        // starts, end states and every token, in Python, C++ and JavaScript
        // (strings across lines, block comments, templates, raw strings,
        // CRLF and lone CR). The ranges come ascending and apart, hold every
        // change, and taken in order with the old lines each replaced, turn
        // the old lines into the new (what the GTK port's bracket depths do
        // with them).
        std::mt19937 rng(11);
        bool same = true, sequential = true, text = true, ranges = true, replay = true;
        for (int round = 0; round < 600 && same && sequential; round++) {
            const char *ext = round % 3 == 0 ? "py" : round % 3 == 1 ? "cpp" : "js";
            const std::string base = randomText(rng, ext, 1 + rng() % 40);
            const std::vector<EditBatch::Change> changes = randomChanges(rng, base, rng() % 15);
            IncrementalHighlighter<char> h(ext);
            h.reset(StringSource<char>(base), nullptr);
            std::vector<std::string> model = linesOf(h, base);
            std::vector<std::pair<size_t, size_t>> seen;
            std::vector<size_t> befores;
            const std::string out = EditBatch::apply(
                h, base, changes,
                [&](const TextSource<char> &now, const IncrementalHighlighter<char>::Range &r,
                    size_t linesBefore, const std::vector<Token> &) {
                    if (now.length() != h.length() || r.endLine <= r.firstLine) ranges = false;
                    seen.push_back({r.firstLine, r.endLine});
                    befores.push_back(linesBefore);
                });
            if (out != EditBatch::applied(base, changes)) text = false;
            // Ranges ascending and apart.
            for (size_t i = 1; i < seen.size(); i++)
                if (seen[i].first < seen[i - 1].second) ranges = false;
            // Every change's place in the new text is in a range.
            long long shift = 0;
            for (const auto &c : changes) {
                const size_t at = (size_t)((long long)c.pos + shift);
                shift += (long long)c.text.size() - (long long)c.oldLen;
                const size_t line = h.lineOf(std::min(at, out.size()));
                bool in = false;
                for (const auto &r : seen) in = in || (r.first <= line && line < r.second);
                if (!in) ranges = false;
            }
            // Replayed in order: each range's lines replace the old ones it
            // says, as edit()'s caller computes them from the count after.
            const std::vector<std::string> now = linesOf(h, out);
            for (size_t i = 0; i < seen.size(); i++) {
                const size_t lines = seen[i].second - seen[i].first;
                const size_t replaced = befores[i] + lines - h.lineCount();
                if (seen[i].first + replaced > model.size()) {
                    replay = false;
                    break;
                }
                model.erase(model.begin() + (long)seen[i].first,
                            model.begin() + (long)(seen[i].first + replaced));
                model.insert(model.begin() + (long)seen[i].first, now.begin() + (long)seen[i].first,
                             now.begin() + (long)seen[i].second);
            }
            if (model != now) replay = false;
            // edit() one change at a time, last first, over the text as it
            // then is.
            IncrementalHighlighter<char> one(ext);
            one.reset(StringSource<char>(base), nullptr);
            std::string cur = base;
            std::vector<Token> scratch;
            for (size_t k = changes.size(); k-- > 0;) {
                cur.replace(changes[k].pos, changes[k].oldLen, changes[k].text);
                one.edit(StringSource<char>(cur), changes[k].pos, changes[k].oldLen,
                         changes[k].text.size(), scratch);
            }
            IncrementalHighlighter<char> fresh(ext);
            fresh.reset(StringSource<char>(out), nullptr);
            const std::vector<std::string> mine = snapshot(h, out);
            if (mine != snapshot(fresh, out)) {
                same = false;
                std::printf("  editbatch: %s round %d differs from a fresh lex\n", ext, round);
            }
            if (mine != snapshot(one, out)) {
                sequential = false;
                std::printf("  editbatch: %s round %d differs from edit() in turn\n", ext, round);
            }
        }
        CHECK(same);
        CHECK(sequential);
        CHECK(text);
        CHECK(ranges);
        CHECK(replay);
        // Nothing to do, and edits that do not describe the text (a reset).
        IncrementalHighlighter<char> h("py");
        const std::string t = "a = 1\nb = 2\n";
        h.reset(StringSource<char>(t), nullptr);
        std::vector<Token> toks;
        CHECK(h.editMany(StringSource<char>(t), {}, toks).empty());
        const std::string longer = "a = 1\nb = 22\n";
        const auto rs = h.editMany(StringSource<char>(longer), {{0, 0, 5}}, toks);
        CHECK(rs.size() == 1 && rs[0].range.firstLine == 0 && rs[0].range.endLine == h.lineCount() &&
              rs[0].oldLines == 3);
    }

    GROUP("editbatch:reads-near");
    {
        // A thousand carets typing over 100,000 lines, and Replace All of
        // 20,000 matches: each edit re-lexes and reads only around itself,
        // where one edit from the first change to the last re-lexed every
        // line between, and the line table moves once rather than once an
        // edit (50 million starts moved for the thousand).
        std::string base;
        for (int i = 0; i < 100000; i++) base += "    value = compute(i) + 1\n";
        struct Counting : TextSource<char> {
            const std::string &text;
            mutable size_t bytes = 0;
            explicit Counting(const std::string &s) : text(s) {}
            size_t length() const override { return text.size(); }
            void read(size_t pos, size_t len, char *out) const override {
                bytes += len;
                text.copy(out, len, pos);
            }
        };
        for (int spread : {100, 1, 5}) {
            const int count = spread == 5 ? 20000 : 1000;
            std::vector<EditBatch::Change> changes;
            for (int i = 0; i < count; i++)
                changes.push_back({(size_t)(i * spread + (spread == 1 ? 50000 : 0)) * 27 + 4,
                                   spread == 5 ? (size_t)5 : 0, spread == 5 ? "total" : "x"});
            IncrementalHighlighter<char> h("py");
            h.reset(StringSource<char>(base), nullptr);
            const std::string out = EditBatch::applied(base, changes);
            std::vector<IncrementalHighlighter<char>::Edit> edits;
            for (const auto &c : changes) edits.push_back({c.pos, c.oldLen, c.text.size()});
            Counting counting(out);
            std::vector<Token> toks;
            const double t0 = cpuMs();
            const auto ranges = h.editMany(counting, edits, toks);
            const double took = cpuMs() - t0;
            size_t relexed = 0;
            for (const auto &r : ranges) relexed += r.range.endLine - r.range.firstLine;
            CHECK(relexed <= 3 * (size_t)count);
            CHECK(counting.bytes < 200 * (size_t)count);
            // A few milliseconds for a thousand on the ThinkPad; edit() in
            // turn took 25 to 50.
            CHECK_TIME(took, spread == 5 ? 300.0 : 50.0);
            IncrementalHighlighter<char> fresh("py");
            fresh.reset(StringSource<char>(out), nullptr);
            CHECK(snapshot(h, out) == snapshot(fresh, out));
        }
    }
}
