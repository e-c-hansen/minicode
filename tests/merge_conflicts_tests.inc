// merge_conflicts_tests.inc — the `conflicts:*` groups of run_tests.cpp:
// git's conflict blocks read from text (MergeConflicts.h) in all three
// marker styles (merge, diff3, zdiff3), with labels, CRLF, marker-like text
// inside a side, and unfinished blocks left alone; what keeping the
// current side, the incoming one or both makes of the text; blocks
// following edits; stepping between them; and a conflicted file on the
// review page (ChangeReview::conflictFile). Included there.

static void testMergeConflicts() {
    using namespace MergeConflicts;
    auto u = [](const char *s) { return QuickDiff::fromUtf8(s); };
    auto s8 = [](const std::u16string &s) { return QuickDiff::toUtf8(s); };
    auto keep = [&](const char *text, size_t block, Keep k) {
        const std::u16string t = u(text);
        const std::vector<Block> bs = parse(t);
        if (block >= bs.size()) return std::string("<no block>");
        return s8(resolved(t, bs[block], k));
    };
    auto expect = [&](const std::string &got, const char *want) {
        CHECK(got == want);
        if (got != want) fprintf(stderr, "    got \"%s\"\n    want \"%s\"\n", got.c_str(), want);
    };

    GROUP("conflicts:parse");
    {
        const char *t = "top\n<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> feature\nend\n";
        const std::u16string text = u(t);
        const std::vector<Block> bs = parse(text);
        CHECK(bs.size() == 1);
        if (bs.size() == 1) {
            const Block &b = bs[0];
            CHECK(b.start == 4 && b.end == text.size() - 4);
            CHECK(s8(text.substr(b.currentStart, b.currentEnd - b.currentStart)) == "ours\n");
            CHECK(s8(text.substr(b.incomingStart, b.incomingEnd - b.incomingStart)) == "theirs\n");
            CHECK(!b.hasBase());
            CHECK(b.currentLabel == "HEAD" && b.incomingLabel == "feature");
            CHECK(text.substr(b.separator, 7) == u"=======");
            CHECK(text.substr(b.endMarker, 7) == u">>>>>>>");
        }
        CHECK(mayHold(text) && !mayHold(u("no marks <<<<<< here")));
        // The UTF-8 reading finds the same block at byte offsets.
        const std::vector<Block> b8 = parse(std::string(t));
        CHECK(b8.size() == 1 && b8[0].start == 4 && b8[0].currentLabel == "HEAD");
        // No label at all, and an empty side.
        const std::vector<Block> bare = parse(u("<<<<<<<\n=======\nx\n>>>>>>>\n"));
        CHECK(bare.size() == 1 && bare[0].currentLabel.empty() &&
              bare[0].currentStart == bare[0].currentEnd);
        // Two blocks, one right after the other.
        CHECK(parse(u("<<<<<<< a\n1\n=======\n2\n>>>>>>> b\n<<<<<<< a\n3\n=======\n4\n>>>>>>> b\n"))
                  .size() == 2);
        // A label with spaces and parentheses, as a rebase writes, and a
        // non-ASCII one.
        const std::vector<Block> r = parse(
            u("<<<<<<< HEAD\nx\n=======\ny\n>>>>>>> 1a2b3c4 (Fix the thing)  \n"));
        CHECK(r.size() == 1 && r[0].incomingLabel == "1a2b3c4 (Fix the thing)");
        const std::vector<Block> nb = parse(u("<<<<<<< ветка\nx\n=======\ny\n>>>>>>> b\n"));
        CHECK(nb.size() == 1 && nb[0].currentLabel == "ветка");
    }

    GROUP("conflicts:diff3");
    {
        // diff3 and zdiff3 write the common ancestor between "|||||||" and
        // "=======".
        const char *t = "<<<<<<< HEAD\nours\n||||||| merged common ancestors\nbase\n=======\n"
                        "theirs\n>>>>>>> topic\n";
        const std::u16string text = u(t);
        const std::vector<Block> bs = parse(text);
        CHECK(bs.size() == 1);
        if (bs.size() == 1) {
            CHECK(bs[0].hasBase() && bs[0].baseLabel == "merged common ancestors");
            CHECK(s8(text.substr(bs[0].baseStart, bs[0].baseEnd - bs[0].baseStart)) == "base\n");
            CHECK(s8(text.substr(bs[0].currentStart, bs[0].currentEnd - bs[0].currentStart)) ==
                  "ours\n");
        }
        // The ancestor goes with the markers, whichever side is kept.
        expect(keep(t, 0, Keep::Current), "ours\n");
        expect(keep(t, 0, Keep::Incoming), "theirs\n");
        expect(keep(t, 0, Keep::Both), "ours\ntheirs\n");
        // zdiff3: shared lines outside the block, the same markers inside.
        const char *z = "same\n<<<<<<< ours\nA\n||||||| base\nB\n=======\nC\n>>>>>>> theirs\n";
        expect(keep(z, 0, Keep::Incoming), "same\nC\n");
    }

    GROUP("conflicts:keep");
    {
        const char *t = "a\n<<<<<<< HEAD\n1\n2\n=======\n3\n>>>>>>> b\nz\n";
        expect(keep(t, 0, Keep::Current), "a\n1\n2\nz\n");
        expect(keep(t, 0, Keep::Incoming), "a\n3\nz\n");
        expect(keep(t, 0, Keep::Both), "a\n1\n2\n3\nz\n");
        // One splice: only the block's range is replaced.
        const std::u16string text = u(t);
        const Splice sp = resolve(text, parse(text)[0], Keep::Incoming);
        CHECK(sp.start == 2 && sp.start + sp.length == text.size() - 2 && s8(sp.text) == "3\n");
        // The second of two blocks, the first left as it is.
        const char *two = "<<<<<<< a\n1\n=======\n2\n>>>>>>> b\nmid\n<<<<<<< a\n3\n=======\n4\n>>>>>>> b\n";
        expect(keep(two, 1, Keep::Current),
               "<<<<<<< a\n1\n=======\n2\n>>>>>>> b\nmid\n3\n");
        // Every block at once, as Side by Side shows the two sides.
        CHECK(s8(resolvedAll(u(two), parse(u(two)), Keep::Current)) == "1\nmid\n3\n");
        CHECK(s8(resolvedAll(u(two), parse(u(two)), Keep::Incoming)) == "2\nmid\n4\n");
        // The text ending at ">>>>>>>" with no line break: neither does
        // what is kept.
        expect(keep("x\n<<<<<<< a\n1\n=======\n2\n>>>>>>> b", 0, Keep::Current), "x\n1");
        expect(keep("x\n<<<<<<< a\n1\n=======\n2\n>>>>>>> b", 0, Keep::Both), "x\n1\n2");
        // An empty side kept: the block goes, nothing in its place.
        expect(keep("a\n<<<<<<< a\n=======\n2\n>>>>>>> b\nz\n", 0, Keep::Current), "a\nz\n");
    }

    GROUP("conflicts:crlf");
    {
        const char *t = "a\r\n<<<<<<< HEAD\r\nours\r\n=======\r\ntheirs\r\n>>>>>>> b\r\nz\r\n";
        CHECK(parse(u(t)).size() == 1);
        CHECK(parse(u(t))[0].incomingLabel == "b");   // no "\r" in the label
        expect(keep(t, 0, Keep::Current), "a\r\nours\r\nz\r\n");
        expect(keep(t, 0, Keep::Both), "a\r\nours\r\ntheirs\r\nz\r\n");
        expect(keep("<<<<<<< a\r\n1\r\n=======\r\n2\r\n>>>>>>> b", 0, Keep::Incoming), "2");
    }

    GROUP("conflicts:not-markers");
    {
        // Eight or more characters, an indented marker, a marker glued to
        // text, and "=======" with text after it are lines of a side.
        const char *t = "<<<<<<< HEAD\n<<<<<<<< eight\n  <<<<<<< indented\n<<<<<<<x\n======= no\n"
                        "========\n=======\n>>>>>>>> eight\nreal\n>>>>>>> b\n";
        const std::u16string text = u(t);
        const std::vector<Block> bs = parse(text);
        CHECK(bs.size() == 1);
        if (bs.size() == 1) {
            CHECK(s8(text.substr(bs[0].currentStart, bs[0].currentEnd - bs[0].currentStart)) ==
                  "<<<<<<<< eight\n  <<<<<<< indented\n<<<<<<<x\n======= no\n========\n");
            CHECK(s8(text.substr(bs[0].incomingStart, bs[0].incomingEnd - bs[0].incomingStart)) ==
                  ">>>>>>>> eight\nreal\n");
        }
        // A second "=======" in the incoming side is its text (a heading's
        // underline), and so is "|||||||" there.
        const std::vector<Block> under =
            parse(u("<<<<<<< a\nx\n=======\nTitle\n=======\n||||||| y\n>>>>>>> b\n"));
        CHECK(under.size() == 1);
        // Nothing marked: no blocks.
        CHECK(parse(u("plain text\n=======\n>>>>>>> lone end\n")).empty());
    }

    GROUP("conflicts:unterminated");
    {
        // No end: left alone.
        CHECK(parse(u("<<<<<<< a\nx\n=======\ny\n")).empty());
        // No separator before the end: left alone.
        CHECK(parse(u("<<<<<<< a\nx\n>>>>>>> b\n")).empty());
        // A second start before the first finished: the first was not a
        // block, the second is.
        const std::u16string t = u("<<<<<<< a\nstray\n<<<<<<< c\n1\n=======\n2\n>>>>>>> d\n");
        const std::vector<Block> bs = parse(t);
        CHECK(bs.size() == 1 && bs.size() == 1 && bs[0].start == 16 && bs[0].currentLabel == "c");
        // A start inside the incoming side starts again too.
        CHECK(parse(u("<<<<<<< a\n1\n=======\n<<<<<<< c\n2\n=======\n3\n>>>>>>> d\n")).size() == 1);
        // A finished block before an unfinished one is still a block.
        CHECK(parse(u("<<<<<<< a\n1\n=======\n2\n>>>>>>> b\n<<<<<<< c\n3\n")).size() == 1);
        // The empty text and one line.
        CHECK(parse(u("")).empty() && parse(u("<<<<<<<")).empty());
    }

    GROUP("conflicts:follow-edits");
    {
        const std::u16string t = u("aa\n<<<<<<< x\n1\n=======\n2\n>>>>>>> y\nzz\n");
        std::vector<Block> bs = parse(t);
        CHECK(bs.size() == 1);
        // Typing before the block moves it.
        std::vector<Block> moved = bs;
        followEdit(moved, 1, 0, 3);
        std::u16string t2 = t;
        t2.insert(1, u"abc");
        CHECK(moved.size() == 1 && moved[0].start == bs[0].start + 3);
        const std::vector<Block> again = parse(t2);
        if (moved.size() == 1 && again.size() == 1) {
            const Block &a = moved[0], &b = again[0];
            CHECK(a.start == b.start && a.end == b.end && a.separator == b.separator &&
                  a.currentStart == b.currentStart && a.incomingEnd == b.incomingEnd &&
                  a.endMarker == b.endMarker);
        }
        // Typing after it leaves it.
        std::vector<Block> after = bs;
        followEdit(after, t.size() - 1, 1, 0);
        CHECK(after.size() == 1 && after[0].start == bs[0].start);
        // At its end (the next line's start) too.
        after = bs;
        followEdit(after, bs[0].end, 0, 4);
        CHECK(after.size() == 1 && after[0].end == bs[0].end);
        // Inside it, or right at its start: dropped until parsed again.
        std::vector<Block> inside = bs;
        followEdit(inside, bs[0].currentStart, 0, 1);
        CHECK(inside.empty());
        std::vector<Block> atStart = bs;
        followEdit(atStart, bs[0].start, 0, 1);
        CHECK(atStart.empty());
        std::vector<Block> joined = bs;
        followEdit(joined, bs[0].start - 1, 1, 0);   // the break before "<<<<<<<"
        CHECK(joined.empty());
    }

    GROUP("conflicts:step");
    {
        const std::u16string t =
            u("<<<<<<< a\n1\n=======\n2\n>>>>>>> b\nmid\n<<<<<<< a\n3\n=======\n4\n>>>>>>> b\nend\n");
        const std::vector<Block> bs = parse(t);
        CHECK(bs.size() == 2);
        if (bs.size() == 2) {
            CHECK(step(bs, 0, true) == 1);                 // in the first: on to the second
            CHECK(step(bs, bs[1].start, true) == 0);       // past the last: wraps
            CHECK(step(bs, bs[1].start + 3, false) == 0);  // in the second: back to the first
            CHECK(step(bs, 0, false) == 1);                // before the first: wraps
            CHECK(step(bs, t.size(), false) == 1);
            CHECK(blockAt(bs, bs[0].separator) == 0 && blockAt(bs, bs[0].end) == -1);
        }
        CHECK(step(std::vector<Block>(), 0, true) == -1);
    }

    GROUP("conflicts:words");
    {
        CHECK(buttonTitle(Keep::Current) == "Keep Current" &&
              buttonTitle(Keep::Incoming) == "Keep Incoming" && buttonTitle(Keep::Both) == "Keep Both");
        CHECK(actionName(Keep::Both) == "Keep Both Changes" &&
              actionName(Keep::Current) == "Keep Current Change");
        CHECK(leftText(0) == "No conflicts left" && leftText(1) == "1 conflict left" &&
              leftText(3) == "3 conflicts left");
        CHECK(positionText(1, 3) == "Conflict 2 of 3");
        Block b;
        b.currentLabel = "HEAD";
        b.incomingLabel = "feature";
        CHECK(tooltip(Keep::Incoming, b) == "Keep the lines from feature and drop those from HEAD");
        CHECK(resolvedNote("a.txt", 2) == "2 conflicts left in a.txt.");
        CHECK(resolvedNote("a.txt", 0).find("No conflicts left in a.txt.") == 0);
    }

    GROUP("conflicts:review");
    {
        using namespace ChangeReview;
        File f;
        f.path = "a.py";
        f.section = Section::Changes;
        f.conflict = true;
        f.letter = 'U';
        f.newText = u("one\ntwo\nthree\nfour\nfive\nsix\n<<<<<<< HEAD\nours\n||||||| base\nold\n"
                      "=======\ntheirs\n>>>>>>> topic\nseven\n");
        const FileDiff d = conflictFile(f, "py");
        CHECK(d.conflicts.size() == 1 && d.blocks.size() == 1);
        CHECK(s8(fileHeaderText(d)) == "a.py  1 conflict left");
        CHECK(!canMarkResolved(d));
        CHECK(!wholeOnlyReason(d.file).empty());   // no Stage or Discard on its blocks
        const Page p = layout({d}, Opened());
        // Header, a fold of the first lines, three of context, the block's
        // seven lines, and the line after.
        std::vector<RowKind> kinds;
        size_t conflictRows = 0;
        for (const PageRow &r : p.rows) {
            kinds.push_back(r.kind);
            if (r.kind == RowKind::Conflict) conflictRows++;
        }
        CHECK(conflictRows == 7);
        CHECK(p.blockRows.size() == 1);
        if (p.blockRows.size() == 1) {
            const PageRow &first = p.rows[p.blockRows[0]];
            CHECK(first.kind == RowKind::Conflict && first.blockStart &&
                  first.part == ConflictPart::CurrentMarker && first.newLine == 6);
            const std::vector<ConflictPart> want = {
                ConflictPart::CurrentMarker, ConflictPart::Current, ConflictPart::BaseMarker,
                ConflictPart::Base, ConflictPart::Separator, ConflictPart::Incoming,
                ConflictPart::IncomingMarker};
            bool same = true;
            for (size_t k = 0; k < want.size(); k++)
                if (p.rows[p.blockRows[0] + k].part != want[k]) same = false;
            CHECK(same);
        }
        CHECK(kinds.size() >= 2 && kinds[0] == RowKind::Section && kinds[1] == RowKind::File);
        // Keeping a side gives the file's new text, and once none is left
        // the file can be marked resolved.
        const std::u16string kept = conflictResult(d, 0, MergeConflicts::Keep::Incoming);
        CHECK(s8(kept) == "one\ntwo\nthree\nfour\nfive\nsix\ntheirs\nseven\n");
        File g = f;
        g.newText = kept;
        const FileDiff done = conflictFile(g, "py");
        CHECK(canMarkResolved(done) && s8(fileHeaderText(done)) == "a.py  no conflicts left");
        const Page dp = layout({done}, Opened());
        bool noted = false;
        for (const PageRow &r : dp.rows)
            if (r.kind == RowKind::Note) noted = true;
        CHECK(noted && dp.blockRows.empty());
        // A binary conflict or one gone from the work tree keeps the old note.
        File bin = f;
        bin.binary = true;
        const FileDiff bd = conflictFile(bin, "py");
        CHECK(bd.conflicts.empty() && !canMarkResolved(bd) &&
              s8(fileHeaderText(bd)) == "a.py  (conflict)");
        File gone = f;
        gone.newExists = false;
        CHECK(!canMarkResolved(conflictFile(gone, "py")));
        // F7 steps between the blocks of two conflicts.
        File two = f;
        two.newText = u("<<<<<<< a\n1\n=======\n2\n>>>>>>> b\n<<<<<<< a\n3\n=======\n4\n>>>>>>> b\n");
        const FileDiff td = conflictFile(two, "");
        const Page tp = layout({td}, Opened());
        CHECK(td.blocks.size() == 2 && tp.blockRows.size() == 2);
        if (tp.blockRows.size() == 2)
            CHECK(stepBlock(tp, (long)tp.blockRows[0], true) == (long)tp.blockRows[1]);
    }
}
