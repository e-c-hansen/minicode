// mathtex_isolation_tests.inc — the `mathtex:isolation` group: a Markdown
// formula cannot change the formulas typeset after it in its batch, nor
// pass for one of them in the log (MathTex.h: what check() refuses, and how
// document() keeps each formula to its box). Included by run_tests.cpp.
// What TeX itself makes of the batch was checked with tectonic by hand
// (a formula escaping its box, one leaving a group open, a forged log
// line); the GUI's markdown_math suite does it with MC_GUI_REAL_TECTONIC.
#include "MarkdownParser.h"
#include "MathTex.h"
#include <cstdio>

static void testMathTexIsolation() {
    GROUP("mathtex:isolation");
    {
        using MathTex::check;
        // What outlives the formula's group, or reaches the batch's own
        // machinery, is refused.
        for (const char *tex : {
                 "\\gdef\\alpha{\\beta}\\alpha",
                 "\\xdef\\alpha{\\beta}",
                 "\\global\\let\\alpha\\beta",
                 "\\globaldefs=1 \\def\\alpha{\\beta}",
                 "\\aftergroup\\x",
                 "\\afterassignment\\x \\def\\y{}",
                 "x\\egroup y",
                 "x\\endgroup y",
                 "\\begingroup x",
                 "\\bgroup x",
                 "\\mcship{3}",
                 "\\let\\mcship\\relax",
                 "\\mcbox",
                 "\\setcounter{equation}{4}",
                 "\\stepcounter{equation}",
                 "\\addtocounter{x}{1}",
                 "\\xglobal\\definecolor{red}{rgb}{0,1,0}",
                 "\\fontdimen22\\textfont2=9pt",
                 "\\hyphenchar\\font=-1",
                 "\\skewchar\\textfont1=0",
                 "\\interactionmode=0",
                 "\\batchmode x",
                 "\\errorstopmode",
                 "\\XeTeXpicfile \"/etc/x.png\"",
                 "\\XeTeXinterchartokenstate=1",
                 "\\DeclareMathOperator{\\x}{x}",
                 "\\DeclareFontShape{OML}{cmm}{m}{it}{<->cmbx10}{}",
                 "\\NewDocumentCommand{\\x}{}{y}",
                 "\\RenewDocumentCommand{\\x}{}{y}",
                 "\\AddToHook{shipout/background}{x}",
                 "\\AtBeginDocument{x}",
                 "\\AtEndDocument{x}",
                 "\\SetSymbolFont{letters}{bold}{OML}{cmm}{b}{it}",
             }) {
            const std::string why = check(tex);
            CHECK(!why.empty());
            if (why.empty()) std::printf("    not refused: %s\n", tex);
        }
        CHECK(check("\\mcship{3}") == "\\mcship is not allowed in a formula");
        CHECK(check("\\gdef\\a{b}") == "\\gdef is not allowed in a formula");
        CHECK(check("x \\NewDocumentCommand").find("\\NewDocumentCommand") != std::string::npos);
        // Local definitions end with the box and stay allowed; look-alikes
        // and ordinary math pass.
        for (const char *tex : {"\\def\\alpha{\\beta}\\alpha", "\\newcommand{\\x}{y}\\x",
                                "\\mathcal{A}", "\\max_i x_i", "\\begin{aligned}a&=b\\end{aligned}",
                                "\\egroupx", "\\globally", "\\text{mc}", "\\left( x \\right)",
                                "\\boldsymbol{x}", "\\mu_c", "\\m", "\\textcolor{red}{x}",
                                "\\nexists", "\\atop", "\\Re z"})
            CHECK(check(tex).empty());

        // Every formula of tests/markdown/math.md still passes, unless for
        // a reason it had before.
        FILE *f = std::fopen("tests/markdown/math.md", "rb");
        if (f) {
            std::string text;
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            std::fclose(f);
            int formulas = 0, refused = 0;
            for (const MdRun &r : MarkdownParser::parse(text)) {
                if (r.math == 0) continue;
                formulas++;
                const std::string why = check(r.text);
                if (why.empty() || why.find("{") != std::string::npos ||
                    why.find("$") != std::string::npos || why.find("^^") != std::string::npos)
                    continue;
                refused++;
                std::printf("    refused: %s (%s)\n", r.text.c_str(), why.c_str());
            }
            CHECK(formulas > 20);
            CHECK(refused == 0);
        }

        // The batch: each box opened and closed with names, watched as it
        // closes, with the sentinel after each and at the end, shipped only
        // at the document's own group level.
        const std::string doc = MathTex::document({{"\\def\\alpha{\\beta}\\alpha", false},
                                                   {"\\alpha", true}},
                                                  "0123abcd4567ef89");
        size_t at = 0;
        int boxes = 0;
        while ((at = doc.find("\\setbox\\mcbox\\hbox", at)) != std::string::npos) {
            const size_t end = doc.find("$\\mcegroup\\mcsentinel\n\\mcship{", at);
            CHECK(end != std::string::npos);
            const std::string open = "\\setbox\\mcbox\\hbox\\mcbgroup\\aftergroup\\mcafter$";
            CHECK(doc.compare(at, open.size(), open) == 0);
            // Nothing between the box's start and end but the formula.
            CHECK(doc.substr(at, end - at).find("\\mcsentinel") == std::string::npos);
            at = end + 1;
            boxes++;
        }
        CHECK(boxes == 2);
        CHECK(doc.find("\\mcsentinel\\mcstop\n\\typeout{MC:0123abcd4567ef89:END}") !=
              std::string::npos);
        CHECK(doc.find("\\mclevel=\\currentgrouplevel") > doc.find("\\begin{document}"));
        // The guards use only names a formula cannot write.
        const size_t ship = doc.find("\\def\\mcship#1{\\mcifnum");
        CHECK(ship != std::string::npos);
        const std::string guard = doc.substr(ship, doc.find("\\mcfi}", ship) - ship);
        CHECK(guard.find("\\mcifnum\\mcgrouplevel>\\mclevel") != std::string::npos);
        CHECK(guard.find("\\mcexpandafter\\mcskip") != std::string::npos);
        CHECK(doc.find("\\long\\def\\mcskip#1\\mcstop{}") != std::string::npos);

        // The log: only lines with the run's nonce count, so a formula's
        // own \typeout{MC:1:...} is not a box.
        CHECK(doc.find("\\typeout{MC:0123abcd4567ef89:BEGIN:1}") != std::string::npos);
        CHECK(doc.find("\\typeout{MC:0123abcd4567ef89:#1:") != std::string::npos);
        CHECK(MathTex::logMark("0123abcd4567ef89") == "MC:0123abcd4567ef89:");
        CHECK(MathTex::logMark("") == "MC:");
        const std::string log =
            "MC:0123abcd4567ef89:BEGIN:0\n"
            "MC:1:1:100.0pt:10.0pt:0.0pt\n"                 // forged by formula 0
            "MC:0123abcd4567ef89:0:1:6.43404pt:4.30554pt:0.0pt\n"
            "MC:BEGIN:1\n"                                  // forged too
            "! Undefined control sequence.\n"               // still formula 0's
            "MC:0123abcd4567ef89:BEGIN:1\n"
            "MC:0123abcd4567ef89:END\n";
        const std::vector<MathTex::Box> boxesRead = MathTex::readLog(log, 2, "0123abcd4567ef89");
        CHECK(boxesRead[0].measured && boxesRead[0].page == 1 && boxesRead[0].width > 6.4 &&
              boxesRead[0].width < 6.5);
        CHECK(boxesRead[0].error == "Undefined control sequence");
        CHECK(boxesRead[1].reached && !boxesRead[1].measured && boxesRead[1].error.empty());
        // Without the nonce the forged lines would have counted.
        CHECK(MathTex::readLog(log, 2)[1].measured);

        CHECK(MathTex::kVersion == 3);   // pictures made by the old batch are not reused
    }
}
