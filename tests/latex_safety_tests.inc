// latex_safety_tests.inc — the `mathtex:refused` and `latex:scratch*` groups
// of run_tests.cpp: what a Markdown formula may not hold (MathTex::check),
// and the LaTeX preview's output folders and sibling records
// (LatexScratch.h). Included there.

static void testLatexSafety() {
    GROUP("mathtex:refused");
    {
        using MathTex::check;
        // Each way round the words check() refuses, and the words themselves.
        for (const char *tex : {
                 "\\csname input\\endcsname{/etc/passwd}",
                 "\\expandafter\\csname end\\endcsname",
                 "\\catcode`\\Q=0 Qinput{x}",
                 "x \\catcode 64=11",
                 "\\scantokens{\\in put}",
                 "\\everyeof{x}",
                 "\\makeatletter\\@@input x",
                 "\\ExplSyntaxOn \\file_input:n {x}",
                 "\\openin1=x",
                 "\\read1 to\\x",
                 "\\readline1 to\\x",
                 "a \\stop",
                 "^^5cinput{x}",
                 "\\^^69nput{x}",
                 "x^^2",
                 "% ^^5c in a comment\nx",
             }) {
            const std::string why = check(tex);
            CHECK(!why.empty());
            if (why.empty()) std::printf("    not refused: %s\n", tex);
        }
        CHECK(check("\\csname x\\endcsname").find("\\csname") != std::string::npos);
        CHECK(check("^^41").find("^^") != std::string::npos);
        // Words that only start like them, and ordinary math, still pass.
        for (const char *tex : {"\\readable", "\\stopwatch", "\\catcodes", "\\csnamex",
                                "x^2", "x^{2^3}", "a_i^2", "\\hat{x}", "\\widehat{xy}",
                                "\\text{read and stop}", "\\mathrm{catcode}",
                                "\\frac{a}{b} \\leq \\sqrt[3]{c}", "\\\\catcode"})
            CHECK(check(tex).empty());
        CHECK(MathTex::kVersion >= 2);   // the cache keys changed with the rules
    }

    GROUP("latex:scratch");
    {
        using namespace LatexScratch;
        const std::string hex =
            "0123456789ABCDEFfedcba98765432100123456789abcdef0123456789abcdef";
        CHECK(folderName(hex) == "0123456789abcdef");
        CHECK(folderName("") == "untitled");
        CHECK(folderName("0123") == "untitled");
        CHECK(folderName("zz23456789abcdef0000") == "untitled");
        CHECK(folderName("../../etc/passwd/xx") == "untitled");

        Record r;
        r.sibling = "/Users/me/paper/.paper.minicode.tex";
        r.pid = 4242;
        r.start = 1762400000.25;
        r.size = 1234;
        r.sha256 = std::string(64, 'a');
        Record back;
        CHECK(parse(format(r), &back));
        CHECK(back.sibling == r.sibling && back.pid == r.pid && back.size == r.size &&
              back.sha256 == r.sha256 && back.start > r.start - 0.01 &&
              back.start < r.start + 0.01);
        // A path holding a newline reads back whole.
        r.sibling = "/tmp/odd\nname/.p.minicode.tex";
        CHECK(parse(format(r), &back) && back.sibling == r.sibling);
        // Anything else is not a record.
        CHECK(!parse("", &back));
        CHECK(!parse("minicode-latex-sibling 2\n1\n0\n1\n" + std::string(64, 'a') + "\n/x\n",
                     &back));
        CHECK(!parse("minicode-latex-sibling 1\nx\n0\n1\n" + std::string(64, 'a') + "\n/x\n",
                     &back));
        CHECK(!parse("minicode-latex-sibling 1\n1\n0\n1\nabc\n/x\n", &back));
        CHECK(!parse("minicode-latex-sibling 1\n1\n0\n1\n" + std::string(64, 'a') + "\nrel\n",
                     &back));
        CHECK(!parse("minicode-latex-sibling 1\n1\n0\n1\n" + std::string(64, 'a') + "\n",
                     &back));

        CHECK(isPreviewCopyName("/a/.paper.minicode.tex"));
        CHECK(isPreviewCopyName("/w/untitled.minicode.tex"));
        CHECK(!isPreviewCopyName("/a/paper.tex"));
        CHECK(!isPreviewCopyName("/a/paper.minicode.tex"));
        CHECK(!isPreviewCopyName("/a/..minicode.tex"));
        CHECK(!isPreviewCopyName("/a/.paper.minicode.tex/x"));

        r.sibling = "/a/.paper.minicode.tex";
        const std::string sha = r.sha256;
        CHECK(decide(r, true, true, r.size, sha) == Action::Keep);
        CHECK(decide(r, false, true, r.size, sha) == Action::RemoveSibling);
        CHECK(decide(r, false, true, r.size, std::string(64, 'A')) == Action::RemoveSibling);
        CHECK(decide(r, false, false, 0, "") == Action::ForgetRecord);
        CHECK(decide(r, false, true, r.size + 1, sha) == Action::ForgetRecord);   // edited
        CHECK(decide(r, false, true, r.size, std::string(64, 'b')) == Action::ForgetRecord);
        Record mine = r;
        mine.sibling = "/a/paper.tex";   // never a file named as the user's are
        CHECK(decide(mine, false, true, r.size, sha) == Action::ForgetRecord);
    }

    GROUP("latex:scratch-two-views");
    {
        using namespace LatexScratch;
        const std::string hex =
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        // Two previews of one file in one process (two windows): a folder
        // and a sibling each.
        Owner one{4242, 1762400000, 1}, two{4242, 1762400000, 2};
        CHECK(folderName(hex, one) == "0123456789abcdef-4242-1762400000-1");
        CHECK(folderName(hex, one) != folderName(hex, two));
        CHECK(siblingName("paper", one) == ".paper.4242-1.minicode.tex");
        CHECK(siblingName("paper", one) != siblingName("paper", two));
        CHECK(siblingName("paper", Owner{}) == ".paper.minicode.tex");
        // Both are preview copies by name, so a crash's record of either can
        // still be acted on, and nothing else is.
        CHECK(isPreviewCopyName("/a/" + siblingName("paper", one)));
        CHECK(isPreviewCopyName("/a/" + siblingName("my.notes", two)));
        Record r;
        r.sibling = "/a/" + siblingName("paper", one);
        r.size = 3;
        r.sha256 = std::string(64, 'c');
        CHECK(decide(r, false, true, 3, r.sha256) == Action::RemoveSibling);
        CHECK(decide(r, true, true, 3, r.sha256) == Action::Keep);
        // Another view's copy is never this record's: the names differ.
        CHECK(r.sibling != "/a/" + siblingName("paper", two));

        // The owner reads back from the folder's name; other names have none.
        Owner back;
        CHECK(parseFolderName(folderName(hex, two), &back));
        CHECK(back.pid == 4242 && back.start == 1762400000 && back.view == 2);
        CHECK(parseFolderName(folderName("", one), &back) && back.view == 1);   // untitled
        CHECK(!parseFolderName("0123456789abcdef", &back));          // an older minicode's
        CHECK(!parseFolderName("0123456789abcdef-1-2", &back));
        CHECK(!parseFolderName("0123456789abcdef-x-2-3", &back));
        CHECK(!parseFolderName("0123456789abcdef-1-2-3-4", &back));
        CHECK(!parseFolderName("0123456789abcdef-0-2-3", &back));    // no pid 0
        CHECK(!parseFolderName("0123456789abcdef-1-2-0", &back));    // views count from 1
        CHECK(!parseFolderName("0123456789abcdeg-1-2-3", &back));
        CHECK(!parseFolderName("../../x-1-2-3", &back));
        CHECK(!parseFolderName("", &back));

        // Gone: no such process, or the pid now another process's.
        CHECK(ownerGone(one, 0));
        CHECK(!ownerGone(one, 1762400000.4));
        CHECK(!ownerGone(one, 1762399999.6));
        CHECK(ownerGone(one, 1762400100));
    }
}
