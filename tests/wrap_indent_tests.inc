// wrap_indent_tests.inc — included by run_tests.cpp. Where a wrapped line's
// continuation starts (WrapIndent.h, editor.wrapping-indent): the leading
// whitespace's width in columns and in points, and VS Code's fall back to
// no indent past half the width.
#include "WrapIndent.h"

void testWrapIndent() {
    using namespace WrapIndent;
    GROUP("wrap-indent:modes");
    {
        Mode m = Mode::None;
        CHECK(parseMode("same", m) && m == Mode::Same);
        CHECK(parseMode("Indent", m) && m == Mode::Indent);
        CHECK(parseMode("NONE", m) && m == Mode::None);
        m = Mode::Same;
        CHECK(!parseMode("deep", m) && m == Mode::Same);   // left as it was
        CHECK(!parseMode("", m));
        CHECK(std::string(modeName(Mode::Same)) == "same");
        CHECK(std::string(modeName(Mode::Indent)) == "indent");
        CHECK(std::string(modeName(Mode::None)) == "none");
    }
    GROUP("wrap-indent:columns");
    {
        CHECK(leadingColumns(u"", 4) == 0);
        CHECK(leadingColumns(u"x = 1", 4) == 0);
        CHECK(leadingColumns(u"    return x", 4) == 4);
        CHECK(leadingColumns(u"\treturn x", 4) == 4);
        CHECK(leadingColumns(u"\treturn x", 8) == 8);
        CHECK(leadingColumns(u"  \tx", 4) == 4);      // a tab runs to the next stop
        CHECK(leadingColumns(u"    \tx", 4) == 8);    // from a stop, a whole step
        CHECK(leadingColumns(u"\t  x", 4) == 6);
        CHECK(leadingColumns(u"\t\t", 2) == 4);       // a blank line counts whole
        CHECK(leadingColumns(u"   ", 4) == 3);
        CHECK(leadingColumns(u"\t", 0) == 1);         // no tab size: one column
        // Only spaces and tabs: a no-break space or a form feed is text.
        CHECK(leadingColumns(u"  x", 4) == 0);
        CHECK(leadingColumns(u"\fx", 4) == 0);
        CHECK(leadingLength(u"  \t x", 5) == 4);
    }
    GROUP("wrap-indent:points");
    {
        const char16_t *s = u"\t  x";
        // AppKit's default stops: every 28 points, twelve of them.
        std::vector<double> stops;
        for (int i = 1; i <= 12; i++) stops.push_back(28.0 * i);
        CHECK(leadingWidth(s, 4, 7.5, stops, 0) == 28 + 15);
        CHECK(leadingWidth(u"   \t", 4, 10, stops, 0) == 56);   // 30 then the stop at 56
        // Past the last stop: the interval, or one space without one.
        CHECK(leadingWidth(u"\t\t", 2, 10, {28}, 0) == 38);
        CHECK(leadingWidth(u"\t\t", 2, 10, {28}, 40) == 40);
        CHECK(leadingWidth(u"\t\t", 2, 10, {}, 40) == 80);
        // A limit stops the reading: past it the answer only needs to be past it.
        std::u16string many(100000, u' ');
        many += u"x";
        const double w = leadingWidth(many.data(), many.size(), 7, {}, 28, 300);
        CHECK(w >= 300 && w < 310);
        CHECK(leadingWidth(many.data(), many.size(), 7, {}, 28) == 700000);
    }
    GROUP("wrap-indent:continuation");
    {
        // Same: under the first non-blank character.
        CHECK(continuationIndent(32, Mode::Same, 32, 600) == 32);
        // Indent: one level further.
        CHECK(continuationIndent(32, Mode::Indent, 32, 600) == 64);
        CHECK(continuationIndent(0, Mode::Indent, 4, 80) == 4);
        // None, and an unindented line in Same: the edge.
        CHECK(continuationIndent(32, Mode::None, 32, 600) == 0);
        CHECK(continuationIndent(0, Mode::Same, 4, 80) == 0);
        // Past half the room: the edge, as VS Code falls back.
        CHECK(continuationIndent(40, Mode::Same, 4, 80) == 40);
        CHECK(continuationIndent(41, Mode::Same, 4, 80) == 0);
        CHECK(continuationIndent(38, Mode::Indent, 4, 80) == 0);
        CHECK(continuationIndent(36, Mode::Indent, 4, 80) == 40);
        // In columns, from a line: 8 columns in a 30-column view.
        CHECK(continuationIndent(leadingColumns(u"        pass", 4), Mode::Same, 4, 30) == 8);
        CHECK(continuationIndent(leadingColumns(u"\t\t\t\tpass", 4), Mode::Same, 4, 30) == 0);
    }
}
