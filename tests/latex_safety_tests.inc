// latex_safety_tests.inc — the `mathtex:refused` and `latex:scratch` groups
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
}
