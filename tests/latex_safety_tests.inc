// latex_safety_tests.inc — the `mathtex:refused` group of run_tests.cpp:
// what a Markdown formula may not hold (MathTex::check). Included there.

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
}
