// change_review_tests.inc — the `review:*` groups of run_tests.cpp: the
// review view of uncommitted changes (ChangeReview.h). Blocks split where
// git's context would merge them; what staging, unstaging and discarding
// one block or a few lines makes of the file, CRLF, a UTF-8 BOM, UTF-16
// and a missing final newline among them; files that go only whole; the
// page's rows, folded stretches, words and syntax colors; F7 across files;
// keeping the eye's place after an action; and git's outputs read.
// Included there.

static void testChangeReview() {
    using namespace ChangeReview;
    auto u = [](const char *s) { return QuickDiff::fromUtf8(s); };
    auto s8 = [](const std::u16string &s) { return QuickDiff::toUtf8(s); };
    auto file = [&](const char *oldText, const char *newText,
                    Section section = Section::Changes) {
        File f;
        f.path = "src/f.txt";
        f.section = section;
        f.oldText = u(oldText);
        f.newText = u(newText);
        return f;
    };
    auto expect = [&](const std::u16string &got, const char *want) {
        CHECK(s8(got) == want);
        if (s8(got) != want)
            fprintf(stderr, "    got \"%s\"\n    want \"%s\"\n", s8(got).c_str(), want);
    };

    GROUP("review:blocks");
    {
        // Three edits a line apart: git makes them one hunk, the view three.
        FileDiff d = diffFile(file("a\nb\nc\nd\ne\n", "A\nb\nC\nd\nE\n"), "txt");
        CHECK(d.blocks.size() == 3);
        CHECK(d.added == 3 && d.removed == 3);
        CHECK(s8(fileHeaderText(d)) == "src/f.txt  +3 -3");
        CHECK(wholeOnlyReason(d.file).empty());
        // The middle one staged: only it goes into the index.
        expect(blockResult(d, 1, Op::Stage), "a\nb\nC\nd\ne\n");
        // Discarded: the other two stay in the work tree.
        expect(blockResult(d, 1, Op::Discard), "A\nb\nc\nd\nE\n");
        expect(blockResult(d, 0, Op::Discard), "a\nb\nC\nd\nE\n");
    }
    {
        // Unstage in the staged list: old HEAD, new the index.
        FileDiff d = diffFile(file("1\n2\n3\n4\n5\n", "1\nTWO\n3\n4\nFIVE\n", Section::Staged), "");
        CHECK(d.blocks.size() == 2);
        expect(blockResult(d, 0, Op::Unstage), "1\n2\n3\n4\nFIVE\n");
        expect(blockResult(d, 1, Op::Unstage), "1\nTWO\n3\n4\n5\n");
        CHECK(opsFor(Section::Staged) == std::vector<Op>{Op::Unstage});
        CHECK((opsFor(Section::Changes) == std::vector<Op>{Op::Stage, Op::Discard}));
        CHECK(opsFor(Section::Plain).empty());
        CHECK(!plan(d, Op::Stage, false).refusal.empty());
        CHECK(plan(d, Op::Unstage, false).refusal.empty());
        CHECK(plan(d, Op::Unstage, false).doneText == "Unstaged a change to f.txt.");
    }
    {
        // Lines added and removed, sizes unequal.
        FileDiff d = diffFile(file("a\nb\nc\n", "a\nx\ny\nz\nc\n"), "");
        CHECK(d.blocks.size() == 1 && d.added == 3 && d.removed == 1);
        expect(blockResult(d, 0, Op::Stage), "a\nx\ny\nz\nc\n");
        expect(blockResult(d, 0, Op::Discard), "a\nb\nc\n");
    }

    GROUP("review:lines");
    {
        FileDiff d = diffFile(file("a\nb\nc\n", "a\nB1\nB2\nc\n"), "");
        // Only the second added line (added alone) staged: the removed b stays.
        expect(linesResult(d, {}, {2}, Op::Stage), "a\nb\nB2\nc\n");
        // b and B1 are a pair: either side chosen takes the change b -> B1.
        expect(linesResult(d, {1}, {}, Op::Stage), "a\nB1\nc\n");
        expect(linesResult(d, {}, {1}, Op::Stage), "a\nB1\nc\n");
        // B1 discarded: b comes back, B2 (added on its own) stays.
        expect(linesResult(d, {}, {1}, Op::Discard), "a\nb\nB2\nc\n");
        // Unstaging a pair from the staged list puts the old line back.
        FileDiff st = diffFile(file("a\nb\nc\n", "a\nB1\nB2\nc\n", Section::Staged), "");
        expect(linesResult(st, {}, {1}, Op::Unstage), "a\nb\nB2\nc\n");
        // Unchanged lines chosen do nothing.
        CHECK(!anyChanged(d, {0}, {0, 3}));
        CHECK(anyChanged(d, {}, {2}));
        CHECK(plan(d, Op::Discard, true).question == "Discard the selected lines of f.txt?");
        CHECK(plan(d, Op::Discard, false).question == "Discard this change to f.txt?");
        CHECK(plan(d, Op::Discard, false).button == "Discard Change");
    }

    GROUP("review:line-endings");
    {
        // The work tree in CRLF, the index in LF (core.autocrlf): only the
        // real change is a block, staged in LF, discarded in CRLF.
        FileDiff d = diffFile(file("a\nb\nc\n", "a\r\nB\r\nc\r\n"), "");
        CHECK(d.blocks.size() == 1 && d.added == 1);
        expect(blockResult(d, 0, Op::Stage), "a\nB\nc\n");
        expect(blockResult(d, 0, Op::Discard), "a\r\nb\r\nc\r\n");
    }
    {
        // No final newline in the old text, a line added after it.
        FileDiff d = diffFile(file("a\nb", "a\nb\nc\n"), "");
        CHECK(d.blocks.size() == 1 && d.lastNewlineChanged);
        expect(blockResult(d, 0, Op::Stage), "a\nb\nc\n");
        expect(blockResult(d, 0, Op::Discard), "a\nb\n");
        // The new last line without one.
        FileDiff e = diffFile(file("a\nb\n", "a\nb\nc"), "");
        expect(blockResult(e, 0, Op::Stage), "a\nb\nc");
        expect(blockResult(e, 0, Op::Discard), "a\nb\n");
        Page p = layout({e}, {});
        CHECK(p.rows.back().kind == RowKind::Note);
    }
    {
        // A UTF-8 BOM the work tree keeps apart, the index holding it as a
        // first U+FEFF: read as the change bars read them, line 1 is no
        // change, and the staged blob gets its mark back.
        std::string work = "\xEF\xBB\xBF" "one\ntwo\n";
        FolderSearch::TextEncoding enc;
        bool bom = false;
        CHECK(FolderSearch::decodeFileText(&work, &enc, &bom) && bom);
        const QuickDiff::Original idx =
            QuickDiff::readOriginal("\xEF\xBB\xBF" "one\nTWO\n", enc, bom);
        File f;
        f.path = "b.txt";
        f.oldText = idx.text;
        f.newText = u(work.c_str());
        FileDiff d = diffFile(f, "");
        CHECK(d.blocks.size() == 1 && d.rows[0].kind == SideBySide::Kind::Same);
        const std::string blob = QuickDiff::stagedBlob(blockResult(d, 0, Op::Stage), idx);
        CHECK(blob == "\xEF\xBB\xBF" "one\ntwo\n");
    }
    {
        // UTF-16 little-endian, as the work tree and the index both hold it.
        std::string bytes = std::string("\xFF\xFE", 2) + std::string("a\0\n\0b\0\n\0", 8);
        std::string work = bytes;
        FolderSearch::TextEncoding enc;
        bool bom = false;
        CHECK(FolderSearch::decodeFileText(&work, &enc, &bom) &&
              enc == FolderSearch::TextEncoding::Utf16LE);
        const QuickDiff::Original idx =
            QuickDiff::readOriginal(std::string("\xFF\xFE", 2) + std::string("a\0\n\0c\0\n\0", 8),
                                    enc, bom);
        File f;
        f.path = "w.txt";
        f.oldText = idx.text;
        f.newText = u(work.c_str());
        FileDiff d = diffFile(f, "");
        CHECK(d.blocks.size() == 1);
        CHECK(QuickDiff::stagedBlob(blockResult(d, 0, Op::Stage), idx) == bytes);
        CHECK(!looksBinary(bytes));
    }

    GROUP("review:whole-files");
    {
        File n = file("", "x\ny\n");
        n.oldExists = false;
        n.letter = 'A';
        n.section = Section::Staged;
        FileDiff d = diffFile(n, "");
        CHECK(d.added == 2 && d.blocks.size() == 1);
        CHECK(wholeOnlyReason(d.file) == "is new");
        CHECK(plan(d, Op::Unstage, false).refusal == "f.txt is new: use Unstage File for all of it.");
        CHECK(s8(fileHeaderText(d)) == "src/f.txt  (new)  +2");
        File un = n;
        un.section = Section::Changes;
        un.untracked = true;
        CHECK(wholeOnlyReason(un) == "is not tracked yet");
        File del = file("x\n", "");
        del.newExists = false;
        del.letter = 'D';
        CHECK(wholeOnlyReason(del) == "was deleted");
        File bin = file("", "");
        bin.binary = true;
        FileDiff bd = diffFile(bin, "");
        CHECK(bd.rows.empty());
        CHECK(plan(bd, Op::Discard, false).refusal ==
              "f.txt is a binary file: use Discard File for all of it.");
        Page p = layout({bd}, {});
        CHECK(p.rows.size() == 3 && p.rows[2].kind == RowKind::Note);
        File sub = file("", "");
        sub.submodule = true;
        CHECK(wholeOnlyReason(sub) == "is a submodule");
        File link = file("old/target", "new/target");
        link.symlink = true;
        CHECK(wholeOnlyReason(link) == "is a symbolic link" && diffFile(link, "").rows.empty());
        File con = file("a\n", "b\n");
        con.conflict = true;
        CHECK(wholeOnlyReason(con) == "has a conflict");
        CHECK(diffFile(con, "").blocks.empty());
        File plain = file("a\n", "b\n", Section::Plain);
        CHECK(!plan(diffFile(plain, ""), Op::Stage, false).refusal.empty());
        CHECK(looksBinary(std::string("ab\0cd", 5)) && !looksBinary("abcd"));
        CHECK(staleText("src/f.txt", true).rfind("The staged version of f.txt changed", 0) == 0);
        CHECK(staleText("src/f.txt", false).rfind("f.txt changed since", 0) == 0);
    }

    GROUP("review:layout");
    {
        std::string a, b;
        for (int i = 1; i <= 20; i++) {
            a += "line " + std::to_string(i) + "\n";
            b += (i == 10 ? std::string("LINE 10 changed") : "line " + std::to_string(i)) + "\n";
        }
        File f = file(a.c_str(), b.c_str());
        f.path = "a.py";
        FileDiff d = diffFile(f, "py");
        Page p = layout({d}, {});
        std::vector<RowKind> kinds;
        for (const PageRow &r : p.rows) kinds.push_back(r.kind);
        const std::vector<RowKind> want = {
            RowKind::Section, RowKind::File, RowKind::Gap,     RowKind::Context,
            RowKind::Context, RowKind::Context, RowKind::Removed, RowKind::Added,
            RowKind::Context, RowKind::Context, RowKind::Context, RowKind::Gap};
        CHECK(kinds == want);
        CHECK(p.rows[2].hidden == 6 && p.rows[11].hidden == 7);
        auto rowText = [&](size_t r) {
            const size_t end = r + 1 < p.starts.size() ? p.starts[r + 1] - 1 : p.text.size();
            return s8(p.text.substr(p.starts[r], end - p.starts[r]));
        };
        CHECK(rowText(0) == "Changes  1");
        CHECK(rowText(1) == "a.py  +1 -1");
        CHECK(rowText(2) == "6 unchanged lines");
        CHECK(rowText(6) == "line 10" && rowText(7) == "LINE 10 changed");
        CHECK(p.rows[6].blockStart && !p.rows[7].blockStart && p.rows[6].block == 0);
        CHECK(p.rows[3].oldLine == 6 && p.rows[3].newLine == 6);
        CHECK(p.blockRows == std::vector<size_t>{6});
        // The changed words: "line" against "LINE", and " changed" added.
        CHECK(!p.words.empty());
        bool removedWord = false, addedWord = false;
        for (const SideBySide::Span &w : p.words) {
            if (w.style == 0 && w.start >= p.starts[6] && w.start < p.starts[7]) removedWord = true;
            if (w.style == 1 && w.start >= p.starts[7]) addedWord = true;
        }
        CHECK(removedWord && addedWord);
        // Python's colors: none in these lines but the page asks no more
        // of them than the lines it shows.
        for (const SideBySide::Span &s : p.syntax) CHECK(s.start + s.length <= p.text.size());
        // Opened, the first gap shows its lines.
        Opened open;
        open[d.file.key()].insert(p.rows[2].gap);
        Page q = layout({d}, open);
        CHECK(q.rows.size() == p.rows.size() + 5 && q.rows[2].kind == RowKind::Context);
        CHECK(gapText(1) == "1 unchanged line");
    }

    GROUP("review:button-labels");
    // What the Accessibility API names each drawn button: the file, and
    // the change by its place in the file, counted from 1.
    CHECK(buttonLabel("Stage", "app.py", 0) == "Stage change 1 in app.py");
    CHECK(buttonLabel("Discard", "src/a.py", 2) == "Discard change 3 in src/a.py");
    CHECK(buttonLabel("Unstage", "app.py", 1) == "Unstage change 2 in app.py");
    CHECK(buttonLabel("Keep Current", "a.py", 1, true) == "Keep Current for conflict 2 in a.py");
    CHECK(buttonLabel("Stage File", "app.py", -1) == "Stage File app.py");
    CHECK(buttonLabel("Side by Side", "b c.txt", -1) == "Side by Side b c.txt");
    {
        // Syntax colors placed on the rows: a keyword in a Python line.
        FileDiff d = diffFile([&] {
            File f = file("x = 1\n", "def f():\n    return 1\n");
            f.path = "m.py";
            return f;
        }(), "py");
        Page p = layout({d}, {});
        bool keyword = false;
        for (const SideBySide::Span &s : p.syntax)
            if (s8(p.text.substr(s.start, s.length)) == "def") keyword = true;
        CHECK(keyword);
    }
    {
        // Two sections and F7 across files, wrapping.
        FileDiff s = diffFile(file("a\n", "b\n", Section::Staged), "");
        FileDiff w1 = diffFile(file("a\nb\nc\nd\ne\nf\ng\nh\n", "A\nb\nc\nd\ne\nf\ng\nH\n"), "");
        File other = file("x\n", "y\n");
        other.path = "z.txt";
        FileDiff w2 = diffFile(other, "");
        Page p = layout({s, w1, w2}, {});
        size_t sections = 0;
        for (const PageRow &r : p.rows) sections += r.kind == RowKind::Section;
        CHECK(sections == 2);
        CHECK(p.blockRows.size() == 4);
        CHECK(stepBlock(p, -1, true) == (long)p.blockRows[0]);
        CHECK(stepBlock(p, (long)p.blockRows[1], true) == (long)p.blockRows[2]);
        CHECK(stepBlock(p, (long)p.blockRows[3], true) == (long)p.blockRows[0]);
        CHECK(stepBlock(p, (long)p.blockRows[0], false) == (long)p.blockRows[3]);
        CHECK(fileRow(p, {s, w1, w2}, "W:z.txt") > fileRow(p, {s, w1, w2}, "W:src/f.txt"));
        CHECK(fileRow(p, {s, w1, w2}, "S:src/f.txt") == 1);
        CHECK(stepBlock(Page(), 0, true) == -1);
    }

    GROUP("review:anchor");
    {
        // The eye on file 2's last block; file 1's only change staged and
        // gone from the list: the eye stays on that block.
        FileDiff f1 = diffFile(file("a\n", "b\n"), "");
        File g = file("1\n2\n3\n4\n5\n6\n7\n8\n9\n", "1\nX\n3\n4\n5\n6\n7\n8\nY\n");
        g.path = "g.txt";
        FileDiff f2 = diffFile(g, "");
        std::vector<FileDiff> before = {f1, f2};
        Page p = layout(before, {});
        const size_t eye = p.blockRows.back() + 1;   // the "Y" row
        Anchor a = anchorAt(p, before, eye);
        CHECK(a.valid && a.fileKey == "W:g.txt");
        std::vector<FileDiff> after = {f2};
        Page q = layout(after, {});
        const size_t r = rowFor(q, after, a);
        CHECK(q.rows[r].newLine == 8 && q.rows[r].kind == RowKind::Added);
        // A file that went: the one now at its place.
        Anchor gone = anchorAt(p, before, 1);
        CHECK(gone.fileKey == "W:src/f.txt");
        CHECK(q.rows[rowFor(q, after, gone)].kind == RowKind::File);
        // The block above the eye staged within the same file: still on Y.
        File g2 = g;
        g2.oldText = u("1\nX\n3\n4\n5\n6\n7\n8\n9\n");
        std::vector<FileDiff> staged = {diffFile(g2, "")};
        Page s = layout(staged, {});
        CHECK(s.rows[rowFor(s, staged, a)].newLine == 8);
    }

    GROUP("review:button-row");
    {
        // A press the Accessibility API queued finds its button again in a
        // later layout of the same changes: here the stretch above g.txt's
        // second block opened, so every row after it moved down.
        File f1 = file("a\n", "b\n");
        std::string base, edited;
        for (int i = 1; i <= 30; i++) {
            base += std::to_string(i) + "\n";
            edited += (i == 2 ? std::string("X") : i == 20 ? std::string("Y") : std::to_string(i)) + "\n";
        }
        File g = file(base.c_str(), edited.c_str());
        g.path = "g.txt";
        File gs = file("p\n", "q\n", Section::Staged);
        gs.path = "g.txt";
        std::vector<FileDiff> files = {diffFile(gs, ""), diffFile(f1, ""), diffFile(g, "")};
        Page p = layout(files, {});
        const long gHeader = fileRow(p, files, "W:g.txt");
        CHECK(gHeader > 0);
        CHECK(buttonRow(p, files, "W:g.txt", -1) == gHeader);
        // The staged g.txt is another file: its own header.
        CHECK(buttonRow(p, files, "S:g.txt", -1) == fileRow(p, files, "S:g.txt"));
        CHECK(buttonRow(p, files, "S:g.txt", -1) != gHeader);
        std::vector<size_t> gBlocks;
        for (size_t r : p.blockRows)
            if ((long)r > gHeader) gBlocks.push_back(r);
        CHECK(gBlocks.size() == 2);
        if (gBlocks.size() == 2) {
            CHECK(buttonRow(p, files, "W:g.txt", 0) == (long)gBlocks[0]);
            CHECK(buttonRow(p, files, "W:g.txt", 1) == (long)gBlocks[1]);
            long gap = -1;
            for (size_t r = gBlocks[0]; r < gBlocks[1]; r++)
                if (p.rows[r].kind == RowKind::Gap) gap = p.rows[r].gap;
            CHECK(gap >= 0);
            Opened open;
            open[files[2].file.key()].insert((int)gap);
            Page q = layout(files, open);
            const long moved = buttonRow(q, files, "W:g.txt", 1);
            CHECK(moved > (long)gBlocks[1]);
            CHECK(moved >= 0 && q.rows[(size_t)moved].blockStart && q.rows[(size_t)moved].block == 1 &&
                  q.rows[(size_t)moved].file == 2);
            CHECK(buttonRow(q, files, "W:g.txt", 0) == (long)gBlocks[0]);
        }
        // No such block, no such file: none.
        CHECK(buttonRow(p, files, "W:g.txt", 2) == -1);
        CHECK(buttonRow(p, files, "W:gone.txt", -1) == -1);
        CHECK(buttonRow(p, files, "W:gone.txt", 0) == -1);
        CHECK(buttonRow(Page(), {}, "W:g.txt", 0) == -1);
    }

    GROUP("review:acted-place");
    {
        // Stage on the first of g.txt's two blocks: the second takes its
        // place, though a staged list now stands above.
        FileDiff f1 = diffFile(file("a\n", "b\n"), "");
        auto lines = [](int n, std::initializer_list<std::pair<int, const char *>> changes) {
            std::string t;
            for (int i = 1; i <= n; i++) {
                std::string line = std::to_string(i);
                for (const auto &c : changes)
                    if (c.first == i) line = c.second;
                t += line + "\n";
            }
            return t;
        };
        const std::string base = lines(30, {});
        const std::string edited = lines(30, {{2, "X"}, {20, "Y"}});
        File g = file(base.c_str(), edited.c_str());
        g.path = "g.txt";
        File h = file("p\n", "q\n");
        h.path = "h.txt";
        std::vector<FileDiff> before = {f1, diffFile(g, ""), diffFile(h, "")};
        Page p = layout(before, {});
        const long gHeader = fileRow(p, before, "W:g.txt");
        size_t firstBlock = 0;
        for (size_t r : p.blockRows)
            if ((long)r > gHeader) { firstBlock = r; break; }
        ActedPlace a = actedPlace(p, before, firstBlock, Op::Stage);
        CHECK(a.valid && a.fileKey == "W:g.txt" && a.newSide && a.line == 1);
        CHECK((a.after == std::vector<std::string>{"W:h.txt"}));
        CHECK((a.before == std::vector<std::string>{"W:src/f.txt"}));
        // After: g.txt's line 2 is in the index; the staged list above.
        File gs = file(base.c_str(), lines(30, {{2, "X"}}).c_str(), Section::Staged);
        gs.path = "g.txt";
        File gw = file(lines(30, {{2, "X"}}).c_str(), edited.c_str());
        gw.path = "g.txt";
        std::vector<FileDiff> after = {diffFile(gs, ""), f1, diffFile(gw, ""), diffFile(h, "")};
        Page q = layout(after, {});
        const size_t r = rowAfterAction(q, after, a);
        CHECK(q.rows[r].blockStart && q.rows[r].kind == RowKind::Removed && q.rows[r].oldLine == 19);
        CHECK(after[q.rows[r].file].file.key() == "W:g.txt");
        // Its last block staged too: the eye goes to what followed (h.txt).
        File gs2 = file(base.c_str(), edited.c_str(), Section::Staged);
        gs2.path = "g.txt";
        std::vector<FileDiff> allStaged = {diffFile(gs2, ""), f1, diffFile(h, "")};
        Page s = layout(allStaged, {});
        ActedPlace last = actedPlace(q, after, r, Op::Stage);
        const size_t r2 = rowAfterAction(s, allStaged, last);
        CHECK(s.rows[r2].kind == RowKind::File && allStaged[s.rows[r2].file].file.key() == "W:h.txt");
        // A refused action changed nothing: the same block again.
        CHECK(rowAfterAction(q, after, last) == r);
        // Discard counts in the index's lines, which it leaves alone.
        ActedPlace d = actedPlace(p, before, firstBlock, Op::Discard);
        CHECK(!d.newSide && d.line == 1);
        File gd = file(base.c_str(), lines(30, {{20, "Y"}}).c_str());
        gd.path = "g.txt";
        std::vector<FileDiff> discarded = {f1, diffFile(gd, ""), diffFile(h, "")};
        Page t = layout(discarded, {});
        const size_t r3 = rowAfterAction(t, discarded, d);
        CHECK(t.rows[r3].blockStart && t.rows[r3].oldLine == 19);
        // A whole file staged from its header: the next file in its place;
        // the last file gone, the one before it.
        ActedPlace whole = actedPlace(p, before, (size_t)gHeader, Op::Stage);
        CHECK(whole.line == -1);
        std::vector<FileDiff> wholeStaged = {diffFile(gs2, ""), f1, diffFile(h, "")};
        Page w = layout(wholeStaged, {});
        CHECK(wholeStaged[w.rows[rowAfterAction(w, wholeStaged, whole)].file].file.key() == "W:h.txt");
        ActedPlace lastFile = actedPlace(p, before, (size_t)fileRow(p, before, "W:h.txt"), Op::Discard);
        std::vector<FileDiff> noH = {f1, diffFile(g, "")};
        Page n = layout(noH, {});
        CHECK(noH[n.rows[rowAfterAction(n, noH, lastFile)].file].file.key() == "W:g.txt");
        CHECK(rowAfterAction(Page(), {}, a) == 0);
    }
    {
        // A block of removed lines only: its place is the line after it.
        File g = file("1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n",
                      "1\n3\n4\n5\n6\n7\n8\n9\n10\n11\nB\n12\n");
        std::vector<FileDiff> before = {diffFile(g, "")};
        Page p = layout(before, {});
        CHECK(p.blockRows.size() == 2);
        ActedPlace a = actedPlace(p, before, p.blockRows[0], Op::Stage);
        CHECK(a.line == 1);   // "3", new line 1
        File g2 = file("1\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n",
                       "1\n3\n4\n5\n6\n7\n8\n9\n10\n11\nB\n12\n");
        std::vector<FileDiff> after = {diffFile(g2, "")};
        Page q = layout(after, {});
        const size_t r = rowAfterAction(q, after, a);
        CHECK(q.rows[r].blockStart && q.rows[r].newLine == 10);
    }

    GROUP("review:images");
    {
        File f;
        f.path = "art/logo.PNG";
        f.section = Section::Plain;   // a commit's: no section row
        f.binary = true;
        CHECK(imagePath(f.path) && imagePath("a.webp") && imagePath("x/y.jpeg"));
        CHECK(!imagePath("icon.svg") && !imagePath("data.bin") && !imagePath("png") &&
              !imagePath("dir.png/file"));
        // A modified picture: both versions, side by side.
        ImageSide o{false, 4, 3, 75}, n{false, 8, 600, 2048};
        CHECK(showAsImages(f, o, n, 16));
        CHECK(f.image && f.oldImage.shown && f.newImage.shown);
        CHECK(imageLabel(f, false) == "Before: 4 \xC3\x97 3 pixels, 75 bytes");
        CHECK(imageLabel(f, true) == "After: 8 \xC3\x97 600 pixels, 2.0 KB");
        CHECK(imageHeight(f.oldImage) == 3 && imageHeight(f.newImage) == kImageMaxHeight);
        CHECK(f.imageRows == (size_t)std::ceil((kImageLabelHeight + kImageMaxHeight + 8) / 16));
        // Laid out as its header and empty rows for the pictures, no note.
        std::vector<FileDiff> files = {diffFile(f, "png")};
        Page p = layout(files, {});
        CHECK(p.rows.size() == 1 + f.imageRows);
        CHECK(p.rows[0].kind == RowKind::File);
        for (size_t r = 1; r < p.rows.size(); r++) CHECK(p.rows[r].kind == RowKind::Image);
        CHECK(s8(p.text).find("binary") == std::string::npos);
        // Still whole only: a picture is staged as a file.
        CHECK(wholeOnlyReason(files[0].file) == "is a binary file");
        // A new picture: one version, no Before or After.
        File added;
        added.path = "new.gif";
        added.binary = true;
        added.oldExists = false;
        CHECK(showAsImages(added, ImageSide(), ImageSide{false, 1, 1, 1}, 16));
        CHECK(!added.oldImage.shown && added.newImage.shown);
        CHECK(imageLabel(added, true) == "1 \xC3\x97 1 pixels, 1 byte");
        CHECK(imageLabel(added, false).empty());
        CHECK(added.imageRows >= 2);
        // Not pictures: bytes that did not read as one, too many pixels,
        // not binary, too large, or another kind of file.
        File bad = f;
        bad.image = false;
        CHECK(!showAsImages(bad, ImageSide(), n, 16));
        CHECK(!showAsImages(bad, o, ImageSide{false, 100000, 100000, 10}, 16));
        File text = bad;
        text.binary = false;
        CHECK(!showAsImages(text, o, n, 16));
        File large = bad;
        large.tooLarge = true;
        CHECK(!showAsImages(large, o, n, 16));
        File other = bad;
        other.path = "a.zip";
        CHECK(!showAsImages(other, o, n, 16));
        std::vector<FileDiff> plain = {diffFile(other, "")};
        CHECK(s8(layout(plain, {}).text).find("A binary file: its bytes are not shown.") != std::string::npos);
        CHECK(byteText(0) == "0 bytes" && byteText(1023) == "1023 bytes" &&
              byteText(1536) == "1.5 KB" && byteText(3u << 20) == "3.0 MB");
    }

    GROUP("review:counts");
    {
        // One file with a staged change and another not: in both lists,
        // one file.
        FileDiff staged = diffFile(file("a\nb\n", "A\nb\n", Section::Staged), "");
        FileDiff changed = diffFile(file("A\nb\n", "A\nB\n"), "");
        File other = file("x\n", "y\n");
        other.path = "g.txt";
        FileDiff g = diffFile(other, "");
        CHECK(fileCount({staged, changed}) == 1);
        CHECK(fileCount({staged, changed, g}) == 2);
        CHECK(fileCount({}) == 0);
        // The same read twice: the same fingerprint; a commit (the staged
        // list gone), a changed text, or a file moved between lists: not.
        const size_t both = fingerprint({staged, changed});
        CHECK(fingerprint({staged, changed}) == both);
        CHECK(fingerprint({changed}) != both);
        FileDiff edited = diffFile(file("A\nb\n", "A\nBB\n"), "");
        CHECK(fingerprint({staged, edited}) != both);
        FileDiff moved = diffFile(file("A\nb\n", "A\nB\n", Section::Staged), "");
        CHECK(fingerprint({moved}) != fingerprint({changed}));
    }

    GROUP("review:action-note");
    {
        // "Staged g.py." over a page with g.py and f.txt changed.
        File gs = file("def f():\n    return 1\n", "def f():\n    return 2\n", Section::Staged);
        gs.path = "g.py";
        File gc = gs;
        gc.section = Section::Changes;
        FileDiff gStaged = diffFile(gs, ""), gChanged = diffFile(gc, "");
        FileDiff f = diffFile(file("a\n", "b\n"), "");
        FileDiff fEdited = diffFile(file("a\n", "bb\n"), "");
        CHECK(fingerprintOf({gStaged, f}, {"g.py"}) == fingerprintOf({gStaged, fEdited}, {"g.py"}));
        CHECK(fingerprintOf({gStaged, f}, {"g.py"}) != fingerprintOf({gChanged, f}, {"g.py"}));
        CHECK(fingerprintOf({gStaged, f}, {}) == fingerprint({gStaged, f}));
        File renamed = gs;
        renamed.path = "h.py";
        renamed.origPath = "g.py";
        CHECK(fingerprintOf({diffFile(renamed, "")}, {"g.py"}) != fingerprintOf({}, {"g.py"}));
        auto note = [](bool headKnown, const std::string& head) {
            ActionNote n;
            n.paths = {"g.py"};
            n.headKnown = headKnown;
            n.head = head;
            return n;
        };
        // The first read after the action is its result; it stays while
        // g.py shows so, whatever else moves.
        ActionNote n = note(true, "H1");
        CHECK(!n.goesWith({gStaged, f}, true, "H1"));
        CHECK(!n.goesWith({gStaged, f}, true, "H1"));
        CHECK(!n.goesWith({gStaged, fEdited}, true, "H1"));
        // Unstaged, discarded or edited from outside: gone.
        CHECK(n.goesWith({gChanged, f}, true, "H1"));
        n = note(true, "H1");
        CHECK(!n.goesWith({gStaged, f}, true, "H1"));
        CHECK(n.goesWith({f}, true, "H1"));
        // A commit, in a terminal or the panel, after the first read:
        // HEAD moved (and the staged change is gone).
        n = note(true, "H1");
        CHECK(!n.goesWith({gStaged, f}, true, "H1"));
        CHECK(n.goesWith({f}, true, "H2"));
        // The order CI met (main, 69ff43b): the commit lands before the
        // first read after the action, so that read already shows g.py
        // committed. HEAD as the action ran says so.
        n = note(true, "H1");
        CHECK(n.goesWith({f}, true, "H2"));
        // A first commit: HEAD unborn ("") as the action ran.
        n = note(true, "");
        CHECK(n.goesWith({}, true, "H1"));
        // HEAD unknown (a worktree's page): the files alone decide.
        n = note(false, "");
        CHECK(!n.goesWith({gStaged}, false, ""));
        CHECK(!n.goesWith({gStaged}, true, "H9"));
        CHECK(n.goesWith({gChanged}, false, ""));
        // A refusal changed nothing: settled on the page at once, so the
        // next read that shows g.py otherwise takes it away.
        n = note(true, "H1");
        n.settle({gChanged, f});
        CHECK(!n.goesWith({gChanged, fEdited}, true, "H1"));
        CHECK(n.goesWith({gStaged, f}, true, "H1"));
        // Every file when no path is named (Apply to Main).
        n = ActionNote();
        n.settle({gStaged, f});
        CHECK(!n.goesWith({gStaged, f}, false, ""));
        CHECK(n.goesWith({gStaged, fEdited}, false, ""));
    }

    GROUP("review:syntax-by-line");
    {
        // Each line's colors from its own start; a "\r\n" file's lines as
        // pendingDiff splits them; a string across the break colored on
        // both lines.
        auto lines = syntaxByLine(u("x = 1\r\ndef f():\n    return 'a'\n"), "py");
        CHECK(lines.size() == 3);
        bool defAtStart = false, stringOnThird = false;
        for (const SideBySide::Span &sp : lines.size() > 1 ? lines[1] : std::vector<SideBySide::Span>())
            if (sp.start == 0 && sp.length == 3 && sp.style == (int)TokenStyle::Keyword) defAtStart = true;
        for (const SideBySide::Span &sp : lines.size() > 2 ? lines[2] : std::vector<SideBySide::Span>()) {
            if (sp.style == (int)TokenStyle::String && sp.start == 11 && sp.length == 3) stringOnThird = true;
            CHECK(sp.start + sp.length <= 14);
        }
        CHECK(defAtStart && stringOnThird);
        for (const SideBySide::Span &sp : lines.empty() ? std::vector<SideBySide::Span>() : lines[0])
            CHECK(sp.start + sp.length <= 5);   // never over the "\r"
        auto doc = syntaxByLine(u("s = \"\"\"one\ntwo\"\"\"\n"), "py");
        CHECK(doc.size() == 2 && !doc[1].empty() && doc[1][0].start == 0 &&
              doc[1][0].style == (int)TokenStyle::String);
        CHECK(syntaxByLine(u("plain words\n"), "").empty());
        CHECK(syntaxByLine(u(""), "py").empty());
    }

    GROUP("review:git-output");
    {
        CHECK((lsFilesArgs({"a b.txt"}) ==
               std::vector<std::string>{"--literal-pathspecs", "ls-files", "--stage", "-z", "--",
                                        "a b.txt"}));
        CHECK((lsTreeArgs("HEAD", {"x"}) ==
               std::vector<std::string>{"--literal-pathspecs", "ls-tree", "-r", "-z", "HEAD", "--",
                                        "x"}));
        CHECK((batchArgs() == std::vector<std::string>{"cat-file", "--batch"}));
        const std::string oid1(40, 'a'), oid2(40, 'b'), oid3(40, 'c');
        std::string ls = "100644 " + oid1 + " 0\ta b.txt" + std::string(1, '\0') + "100755 " + oid2 +
                         " 0\tbin/run" + std::string(1, '\0') + "100644 " + oid3 + " 1\tc.txt" +
                         std::string(1, '\0') + "100644 " + oid3 + " 2\tc.txt" + std::string(1, '\0');
        std::map<std::string, Entry> m = parseLsFiles(ls);
        CHECK(m.size() == 2 && m["a b.txt"].object == oid1 && m["bin/run"].mode == "100755");
        CHECK(!m.count("c.txt"));
        std::string tree = "100644 blob " + oid1 + "\td/x.txt" + std::string(1, '\0') + "160000 commit " +
                           oid2 + "\tlib" + std::string(1, '\0');
        std::map<std::string, Entry> t = parseLsTree(tree);
        CHECK(t.size() == 2 && t["d/x.txt"].object == oid1 && t["lib"].mode == "160000");
        std::string batch = oid1 + " blob 3\nab\n\n" + oid2 + " missing\n" + oid3 + " blob 0\n\n";
        std::map<std::string, std::string> b = parseBatch(batch);
        CHECK(b.size() == 2 && b[oid1] == "ab\n" && b[oid3].empty() && !b.count(oid2));
        // diff-tree's raw records: a change, a rename, an add, a delete.
        const std::string z(40, '0');
        auto rec = [](const std::string &t) { return t + std::string(1, (char)0); };
        std::string raw = rec(":100644 100644 " + oid1 + " " + oid2 + " M") + rec("src/a.py") +
                          rec(":100644 100644 " + oid1 + " " + oid1 + " R100") + rec("old name.txt") +
                          rec("new name.txt") + rec(":000000 100644 " + z + " " + oid3 + " A") +
                          rec("n.txt") + rec(":100644 000000 " + oid1 + " " + z + " D") + rec("d.txt");
        std::vector<RawChange> rc = parseRawDiff(raw);
        CHECK(rc.size() == 4);
        if (rc.size() == 4) {
            CHECK(rc[0].letter == 'M' && rc[0].path == "src/a.py" && rc[0].newObject == oid2);
            CHECK(rc[1].letter == 'R' && rc[1].origPath == "old name.txt" && rc[1].path == "new name.txt");
            CHECK(rc[2].letter == 'A' && rc[2].oldMode == "000000" && nullObject(rc[2].oldObject));
            CHECK(rc[3].letter == 'D' && rc[3].newMode == "000000" && !nullObject(rc[3].oldObject));
        }
        CHECK(parseRawDiff("").empty());
        CHECK((commitDiffArgs("abc", "") ==
               std::vector<std::string>{"-c", "core.quotePath=false", "diff-tree", "-r", "-z", "-M",
                                        "--no-commit-id", "--no-ext-diff", "--no-abbrev", "--root", "abc",
                                        "--"}));
        CHECK(commitDiffArgs("abc", "def")[10] == "abc" && commitDiffArgs("abc", "def")[9] == "def");
        CHECK(commitHeaderArgs("abc")[1] == "-s");
        auto day = [](long long) { return std::string("Sep 23, 2026 at 14:02"); };
        CHECK(commitByline("Ann", 1000, oid1, 1, 1000 + 3 * 86400, day) ==
              "Ann, 3 days ago (Sep 23, 2026 at 14:02)  aaaaaaa");
        CHECK(commitByline("Ann", 1000, oid1, 2, 1000, day).find("first parent") != std::string::npos);
        CHECK(mainWorktreePath("worktree /a/b c\nHEAD 123\nbranch refs/heads/main\n\nworktree /a/b c/.claude/x\n") ==
              "/a/b c");
        CHECK(mainWorktreePath("").empty());
        CHECK(worktreeRawArgs("abc")[8] == "abc" && worktreeRawArgs("abc")[3] == "--raw" &&
              worktreeRawArgs("abc")[4] == "--no-abbrev");
        CHECK((nulPaths(std::string("a\0b c\0", 6)) == std::vector<std::string>{"a", "b c"}));
        // Bytes holding a newline in the middle of the header's count.
        std::string nul = oid1 + " blob 3\n" + std::string("a\0b", 3) + "\n";
        CHECK(parseBatch(nul)[oid1] == std::string("a\0b", 3));
    }
}
