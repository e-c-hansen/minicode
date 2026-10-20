// AutoIndentTests.cpp — included by run_tests.cpp (uses its CHECK, toU16 and
// toU8). The cases are written as text with the caret as '|', or a selection
// between ‹ and ›, before and after the key.
#include "AutoIndent.h"

namespace {

struct AiText {
    std::u16string text;
    size_t a = 0, b = 0;
};

AiText aiParse(const std::string &s) {
    AiText r;
    std::u16string u = toU16(s);
    size_t open = u.find(u'\u2039'), close = u.find(u'\u203A');
    if (open != std::u16string::npos && close != std::u16string::npos) {
        u.erase(close, 1);
        u.erase(open, 1);
        r.a = open;
        r.b = close - 1;
    } else {
        size_t bar = u.find(u'|');
        if (bar != std::u16string::npos) u.erase(bar, 1);
        r.a = r.b = bar == std::u16string::npos ? u.size() : bar;
    }
    r.text = u;
    return r;
}

std::string aiShow(const std::u16string &text, const AutoIndent::Edit &e) {
    if (!e.changed) return "(unchanged)";
    std::u16string t = text;
    t.replace(e.replaceStart, e.replaceLength, e.replacement);
    if (e.selStart == e.selEnd) {
        t.insert(e.selStart, u"|");
    } else {
        t.insert(e.selEnd, u"\u203A");
        t.insert(e.selStart, u"\u2039");
    }
    return toU8(t, nullptr);
}

std::string aiRet(const std::string &file, const std::string &s) {
    AiText p = aiParse(s);
    return aiShow(p.text, AutoIndent::newline(p.text, p.a, p.b, file));
}
std::string aiRetU(const std::string &file, const std::string &s, AutoIndent::Unit u) {
    AiText p = aiParse(s);
    return aiShow(p.text, AutoIndent::newline(p.text, p.a, p.b, file, u));
}
// Types the character before the caret: the text is given with it already in.
std::string aiTyped(const std::string &file, const std::string &s) {
    AiText p = aiParse(s);
    return aiShow(p.text, AutoIndent::typed(p.text, p.a, file));
}
std::string aiTab(const std::string &file, const std::string &s) {
    AiText p = aiParse(s);
    return aiShow(p.text, AutoIndent::tab(p.text, p.a, p.b, file));
}
std::string aiBacktab(const std::string &file, const std::string &s) {
    AiText p = aiParse(s);
    return aiShow(p.text, AutoIndent::backtab(p.text, p.a, p.b, file));
}
std::string aiBackspace(const std::string &file, const std::string &s) {
    AiText p = aiParse(s);
    return aiShow(p.text, AutoIndent::backspace(p.text, p.a, p.b, file));
}

#define AI_EQ(got, want)                                                       \
    do {                                                                       \
        std::string g_ = (got), w_ = (want);                                   \
        CHECK(g_ == w_);                                                       \
        if (g_ != w_)                                                          \
            std::printf("      got:  \"%s\"\n      want: \"%s\"\n", g_.c_str(), \
                        w_.c_str());                                           \
    } while (0)

void testAutoIndentLanguages() {
    using AutoIndent::Lang;
    GROUP("indent:languages");
    CHECK(AutoIndent::languageFor("a.py") == Lang::Python);
    CHECK(AutoIndent::languageFor("/x/y/Tool.PYI") == Lang::Python);
    CHECK(AutoIndent::languageFor("main.cpp") == Lang::CLike);
    CHECK(AutoIndent::languageFor("app.tsx") == Lang::CLike);
    CHECK(AutoIndent::languageFor("go.mod.go") == Lang::CLike);
    CHECK(AutoIndent::languageFor("package.json") == Lang::CLike);
    CHECK(AutoIndent::languageFor("site.css") == Lang::CLike);
    CHECK(AutoIndent::languageFor("ci.yml") == Lang::Yaml);
    CHECK(AutoIndent::languageFor("a.yaml") == Lang::Yaml);
    CHECK(AutoIndent::languageFor("run.sh") == Lang::Generic);
    CHECK(AutoIndent::languageFor("Makefile") == Lang::Generic);
    CHECK(AutoIndent::languageFor("init.lua") == Lang::Generic);
    CHECK(AutoIndent::languageFor("notes.md") == Lang::Plain);
    CHECK(AutoIndent::languageFor("notes.txt") == Lang::Plain);
    CHECK(AutoIndent::languageFor("README") == Lang::Plain);
    CHECK(AutoIndent::languageFor("") == Lang::Plain);
}

void testAutoIndentDetect() {
    GROUP("indent:detect");
    auto d = [](const std::string &file, const std::string &text) {
        return AutoIndent::detect(toU16(text), file);
    };
    AutoIndent::Unit u = d("a.py", "");
    CHECK(!u.tabs && u.width == 4 && !u.fromFile);
    u = d("a.yaml", "a: 1\n");
    CHECK(!u.tabs && u.width == 2 && !u.fromFile);
    u = d("a.go", "package a\n");
    CHECK(u.tabs && !u.fromFile);
    u = d("Makefile", "all:\n");
    CHECK(u.tabs);
    u = d("a.py", "def f():\n  if x:\n    y\n  z\n");
    CHECK(!u.tabs && u.width == 2 && u.fromFile);
    u = d("a.py", "def f():\n    if x:\n        y\n    z\n");
    CHECK(!u.tabs && u.width == 4 && u.fromFile);
    u = d("a.c", "int f() {\n\treturn 1;\n}\n");
    CHECK(u.tabs && u.fromFile);
    // Mostly tabs, with one aligned line of spaces.
    u = d("a.c", "f() {\n\ta;\n\tb;\n      c;\n}\n");
    CHECK(u.tabs);
    // Continuation lines aligned deep do not outvote the real steps.
    u = d("a.py", "def f(a,\n      b):\n    x = 1\n    if x:\n        y\n    if z:\n        w\n");
    CHECK(u.width == 4);
    // Arguments lined up under "foo(" do not set the width.
    u = d("a.py", "x = foo(a,\n        b)\nif y:\n  z\n");
    CHECK(u.width == 2);
    u = d("a.py", "def f(a,\n      b):\n    pass\n");
    CHECK(u.width == 4 && u.fromFile);
    // Allman braces.
    u = d("a.c", "void f()\n{\n  x;\n}\n");
    CHECK(u.width == 2);
    // Shell has no block opener to look for: any step counts.
    u = d("a.sh", "if x; then\n  y\nfi\n");
    CHECK(u.width == 2);
    // An 8-wide step only.
    u = d("a.c", "a {\n        b;\n}\n");
    CHECK(u.width == 8);
    // Spaces with no deeper step: the language's width, but from the file.
    u = d("a.py", "    x\n    y\n");
    CHECK(!u.tabs && u.width == 4 && u.fromFile);
    // CRLF and blank lines with spaces do not count.
    u = d("a.py", "if x:\r\n  y\r\n   \r\nz\r\n");
    CHECK(!u.tabs && u.width == 2);
    CHECK((AutoIndent::Unit{true, 4, true}.text() == u"\t"));
    CHECK((AutoIndent::Unit{false, 2, true}.text() == u"  "));
}

void testAutoIndentPython() {
    GROUP("indent:python colon");
    AI_EQ(aiRet("a.py", "if x:|"), "if x:\n    |");
    AI_EQ(aiRet("a.py", "def f(a, b):|"), "def f(a, b):\n    |");
    AI_EQ(aiRet("a.py", "    for i in range(3):|"), "    for i in range(3):\n        |");
    AI_EQ(aiRet("a.py", "class A(Base):  # the base|"), "class A(Base):  # the base\n    |");
    AI_EQ(aiRet("a.py", "else:   |"), "else:\n    |");             // trailing spaces go
    AI_EQ(aiRet("a.py", "if x:  # note:|"), "if x:  # note:\n    |");
    AI_EQ(aiRet("a.py", "x = 1  # note:|"), "x = 1  # note:\n|");    // ':' in a comment
    AI_EQ(aiRet("a.py", "s = 'a:'|"), "s = 'a:'\n|");                // ':' in a string
    AI_EQ(aiRet("a.py", "s = \"if x:\"|"), "s = \"if x:\"\n|");
    AI_EQ(aiRet("a.py", "d = {'a': 1}|"), "d = {'a': 1}\n|");         // dict literal
    AI_EQ(aiRet("a.py", "y = x[1:]|"), "y = x[1:]\n|");               // slice
    AI_EQ(aiRet("a.py", "f(lambda v: v)|"), "f(lambda v: v)\n|");     // lambda in parens
    AI_EQ(aiRet("a.py", "def f(a: int) -> Dict[str, int]:|"),
          "def f(a: int) -> Dict[str, int]:\n    |");
    // A two-line condition: the colon on the second line indents from the first.
    AI_EQ(aiRetU("a.py", "    if (a and\n            b):|", (AutoIndent::Unit{false, 4, true})),
          "    if (a and\n            b):\n        |");
    // The caret before the colon: no indent.
    AI_EQ(aiRet("a.py", "if x|:"), "if x\n|:");
    // Return mid-line after the colon takes the rest down, indented.
    AI_EQ(aiRet("a.py", "if x:|  pass"), "if x:\n    |pass");
    // Indentation from the file: two spaces.
    AI_EQ(aiRet("a.py", "def f():\n  x = 1\n  if x:|"), "def f():\n  x = 1\n  if x:\n    |");
    // Tabs.
    AI_EQ(aiRet("a.py", "def f():\n\tif x:|"), "def f():\n\tif x:\n\t\t|");

    GROUP("indent:python statements");
    AI_EQ(aiRet("a.py", "    x = 1|"), "    x = 1\n    |");
    AI_EQ(aiRet("a.py", "    return x|"), "    return x\n|");
    AI_EQ(aiRet("a.py", "        return|"), "        return\n    |");
    AI_EQ(aiRet("a.py", "    pass|"), "    pass\n|");
    AI_EQ(aiRet("a.py", "    pass  # done|"), "    pass  # done\n|");
    AI_EQ(aiRet("a.py", "        break|"), "        break\n    |");
    AI_EQ(aiRet("a.py", "        continue|"), "        continue\n    |");
    AI_EQ(aiRet("a.py", "    raise ValueError('x')|"), "    raise ValueError('x')\n|");
    AI_EQ(aiRet("a.py", "    return_value = 3|"), "    return_value = 3\n    |");
    AI_EQ(aiRet("a.py", "    passed = True|"), "    passed = True\n    |");
    AI_EQ(aiRet("a.py", "    if x: return|"), "    if x: return\n    |");
    AI_EQ(aiRet("a.py", "return x|"), "return x\n|");                // nothing to dedent
    // Return before "return"'s value keeps the line where it is.
    AI_EQ(aiRet("a.py", "    return |x"), "    return\n    |x");
    // After a statement spread over lines, back to its first line.
    AI_EQ(aiRet("a.py", "result = some_function(arg1,\n                       arg2)|"),
          "result = some_function(arg1,\n                       arg2)\n|");
    AI_EQ(aiRet("a.py", "    x = f(\n        a,\n    )|"), "    x = f(\n        a,\n    )\n    |");
    AI_EQ(aiRet("a.py", "    return f(a,\n             b)|"), "    return f(a,\n             b)\n|");
    // A comment line keeps its indentation.
    AI_EQ(aiRet("a.py", "    # a comment|"), "    # a comment\n    |");

    GROUP("indent:python brackets");
    // Hanging indent when the bracket ends the line.
    AI_EQ(aiRet("a.py", "x = foo(|"), "x = foo(\n    |");
    AI_EQ(aiRet("a.py", "    x = [|"), "    x = [\n        |");
    AI_EQ(aiRet("a.py", "x = foo(  # why|"), "x = foo(  # why\n    |");
    AI_EQ(aiRet("a.py", "x = foo(\n    a,|"), "x = foo(\n    a,\n    |");
    // Aligned with the first thing after the bracket.
    AI_EQ(aiRet("a.py", "x = foo(a,|"), "x = foo(a,\n        |");
    AI_EQ(aiRet("a.py", "    x = foo( a,|"), "    x = foo( a,\n             |");
    AI_EQ(aiRet("a.py", "x = foo(a,\n        b,|"), "x = foo(a,\n        b,\n        |");
    AI_EQ(aiRet("a.py", "x = {'a': [1,|"), "x = {'a': [1,\n           |");
    AI_EQ(aiRet("a.py", "x = {'a': [1, 2],|"), "x = {'a': [1, 2],\n     |");
    // A colon inside brackets does not indent a level.
    AI_EQ(aiRet("a.py", "d = {\n    'a':|"), "d = {\n    'a':\n    |");
    // Between a pair: the closer on its own line.
    AI_EQ(aiRet("a.py", "x = foo(|)"), "x = foo(\n    |\n)");
    AI_EQ(aiRet("a.py", "    d = {|}"), "    d = {\n        |\n    }");
    AI_EQ(aiRet("a.py", "x = [|  ]"), "x = [\n    |\n]");
    // A closer moved down goes to the opener's line indentation.
    AI_EQ(aiRet("a.py", "    x = foo(a,|)"), "    x = foo(a,\n    |)");
    AI_EQ(aiRet("a.py", "x = foo(\n    a,|)"), "x = foo(\n    a,\n|)");
    // The caret right after the bracket with text after it: hanging.
    AI_EQ(aiRet("a.py", "foo(|a, b)"), "foo(\n    |a, b)");
    // Brackets in strings and comments do not count.
    AI_EQ(aiRet("a.py", "s = '('|"), "s = '('\n|");
    AI_EQ(aiRet("a.py", "x = 1  # (|"), "x = 1  # (\n|");
    AI_EQ(aiRet("a.py", "s = \"it's (\"|"), "s = \"it's (\"\n|");
    // A closing bracket in a string inside brackets.
    AI_EQ(aiRet("a.py", "f(')',|"), "f(')',\n  |");

    GROUP("indent:python continuation");
    AI_EQ(aiRet("a.py", "x = a + \\|"), "x = a + \\\n    |");
    AI_EQ(aiRet("a.py", "    x = a + \\\n        b + \\|"), "    x = a + \\\n        b + \\\n        |");
    AI_EQ(aiRet("a.py", "x = a + \\\n    b|"), "x = a + \\\n    b\n|");
    AI_EQ(aiRetU("a.py", "if a and \\\n        b:|", (AutoIndent::Unit{false, 4, true})),
          "if a and \\\n        b:\n    |");
    AI_EQ(aiRet("a.py", "s = 'a\\\\'|"), "s = 'a\\\\'\n|");          // an escaped backslash

    GROUP("indent:python strings");
    // Inside a triple-quoted string only the line's indentation is kept, and
    // nothing in the string is trimmed.
    AI_EQ(aiRet("a.py", "def f():\n    \"\"\"Doc:|"), "def f():\n    \"\"\"Doc:\n    |");
    AI_EQ(aiRet("a.py", "x = '''\n  (a:|"), "x = '''\n  (a:\n  |");
    AI_EQ(aiRet("a.py", "x = '''\n  a  |  b"), "x = '''\n  a  \n  |  b");
    AI_EQ(aiRet("a.py", "s = '''a''' + (|"), "s = '''a''' + (\n    |");   // closed again
    AI_EQ(aiRet("a.py", "s = \"\"\"\n(\n\"\"\"\nif x:|"), "s = \"\"\"\n(\n\"\"\"\nif x:\n    |");
    // Inside a one-line string: the indentation is kept.
    AI_EQ(aiRet("a.py", "    s = 'ab|cd'"), "    s = 'ab\n    |cd'");
    // f-strings and prefixes read as strings.
    AI_EQ(aiRet("a.py", "s = f'{x}:'|"), "s = f'{x}:'\n|");
    AI_EQ(aiRet("a.py", "s = rb'\\(':|"), "s = rb'\\(':\n    |");

    GROUP("indent:python mid-line");
    // The moved text loses its leading spaces.
    AI_EQ(aiRet("a.py", "    x = 1|   ; y = 2"), "    x = 1\n    |; y = 2");
    // Caret at the start of a line: the line moves down with its indent.
    AI_EQ(aiRet("a.py", "    x = 1\n|    y = 2"), "    x = 1\n\n    |y = 2");
    AI_EQ(aiRet("a.py", "def f():\n|    y = 2"), "def f():\n\n    |y = 2");
    // In the leading whitespace.
    AI_EQ(aiRet("a.py", "  |  y = 2"), "\n    |y = 2");
    // A blank line of spaces is emptied, and the indentation carried on.
    AI_EQ(aiRet("a.py", "def f():\n    x\n    |"), "def f():\n    x\n\n    |");
    AI_EQ(aiRet("a.py", "if x:\n    |"), "if x:\n\n    |");
    // A selection is replaced.
    AI_EQ(aiRet("a.py", "if x:\u2039 pass\u203A"), "if x:\n    |");
    AI_EQ(aiRet("a.py", "a = 1\u2039\nb = 2\nc\u203A = 3"), "a = 1\n|= 3");
    AI_EQ(aiRet("a.py", "if x:\u2039abc\ndef\u203A y"), "if x:\n    |y");

    GROUP("indent:python edges");
    AI_EQ(aiRet("a.py", "|"), "\n|");
    AI_EQ(aiRet("a.py", "|x = 1"), "\n|x = 1");
    AI_EQ(aiRet("a.py", "x = 1|"), "x = 1\n|");
    AI_EQ(aiRet("a.py", "if x:\r\n    y = 1|"), "if x:\r\n    y = 1\r\n    |");
    AI_EQ(aiRet("a.py", "if x:\r\n    y = 1|\r\nz"), "if x:\r\n    y = 1\r\n    |\r\nz");
    AI_EQ(aiRet("a.py", "if x:|\r\n"), "if x:\r\n    |\r\n");
    // Non-ASCII before the caret: alignment counts characters.
    AI_EQ(aiRet("a.py", "é = foo(a,|"), "é = foo(a,\n        |");
    AI_EQ(aiRet("a.py", "x = '\U0001F600' + f(a,|"), "x = '\U0001F600' + f(a,\n            |");
    AI_EQ(aiRet("a.py", "naïve = {|"), "naïve = {\n    |");
    AI_EQ(aiRet("a.py", "if café:|"), "if café:\n    |");
    // A given unit.
    AI_EQ(aiRetU("a.py", "if x:|", (AutoIndent::Unit{false, 2, true})), "if x:\n  |");
    AI_EQ(aiRetU("a.py", "if x:|", (AutoIndent::Unit{true, 4, true})), "if x:\n\t|");
    AI_EQ(aiRetU("a.py", "\tif x:\n\t\treturn|", (AutoIndent::Unit{true, 4, true})), "\tif x:\n\t\treturn\n\t|");
    AI_EQ(aiRetU("a.py", "    if x:\n        return|", (AutoIndent::Unit{true, 4, true})), "    if x:\n        return\n\t|");
    // A string ending in the middle of a later line is followed through.
    AI_EQ(aiRet("a.py", "x = \"\"\"a\nb\"\"\" + (1,|"), "x = \"\"\"a\nb\"\"\" + (1,\n        |");
}

void testAutoIndentElectricPython() {
    GROUP("indent:python electric");
    AI_EQ(aiTyped("a.py", "if x:\n    y\n    else:|"), "if x:\n    y\nelse:|");
    AI_EQ(aiTyped("a.py", "    if x:\n        y\n        elif z:|"), "    if x:\n        y\n    elif z:|");
    AI_EQ(aiTyped("a.py", "try:\n    y\n    except ValueError as e:|"),
          "try:\n    y\nexcept ValueError as e:|");
    AI_EQ(aiTyped("a.py", "try:\n    y\n    except:|"), "try:\n    y\nexcept:|");
    AI_EQ(aiTyped("a.py", "try:\n    y\nexcept E:\n    z\n    finally:|"),
          "try:\n    y\nexcept E:\n    z\nfinally:|");
    AI_EQ(aiTyped("a.py", "for a in b:\n    y\n    else:|"), "for a in b:\n    y\nelse:|");
    AI_EQ(aiTyped("a.py", "while a:\n    y\n    else:|"), "while a:\n    y\nelse:|");
    AI_EQ(aiTyped("a.py", "async def f():\n    async for a in b:\n        y\n        else:|"),
          "async def f():\n    async for a in b:\n        y\n    else:|");
    AI_EQ(aiTyped("a.py", "match x:\n    case 1:\n        y\n        case 2:|"),
          "match x:\n    case 1:\n        y\n    case 2:|");
    // The first case under match stays.
    AI_EQ(aiTyped("a.py", "match x:\n    case 1:|"), "(unchanged)");
    // Nested: one level back, to the nearest shallower statement.
    AI_EQ(aiTyped("a.py", "if a:\n    if b:\n        x\n        else:|"),
          "if a:\n    if b:\n        x\n    else:|");
    // Already in place.
    AI_EQ(aiTyped("a.py", "if x:\n    y\nelse:|"), "(unchanged)");
    // Comments, blank lines and deeper blocks in between are skipped.
    AI_EQ(aiTyped("a.py", "if x:\n    for i in y:\n        z\n\n  # note\n    else:|"),
          "if x:\n    for i in y:\n        z\n\n  # note\nelse:|");
    // The shallower statement is not one else can follow.
    AI_EQ(aiTyped("a.py", "def f():\n    x\n    else:|"), "(unchanged)");
    AI_EQ(aiTyped("a.py", "y = 1\n    else:|"), "(unchanged)");
    // Not the block keywords.
    AI_EQ(aiTyped("a.py", "if x:\n    y\n    elsewhere:|"), "(unchanged)");
    AI_EQ(aiTyped("a.py", "if x:\n    y\n    else_ = {1:|"), "(unchanged)");
    AI_EQ(aiTyped("a.py", "if x:\n    y\n    d[1:|"), "(unchanged)");
    // The colon in a string or comment.
    AI_EQ(aiTyped("a.py", "if x:\n    y\n    else  # :|"), "(unchanged)");
    AI_EQ(aiTyped("a.py", "if x:\n    y\n    else = ':|"), "(unchanged)");
    // A colon typed mid-line.
    AI_EQ(aiTyped("a.py", "if x:\n    y\n    else:| z"), "(unchanged)");
    AI_EQ(aiTyped("a.py", "if x:\n    y\n    else:|  # c"), "if x:\n    y\nelse:|  # c");
    // An if inside brackets is not a statement.
    AI_EQ(aiTyped("a.py", "x = (a if b\n     else:|"), "(unchanged)");
    // Inside a triple-quoted string.
    AI_EQ(aiTyped("a.py", "if x:\n    '''\n    else:|"), "(unchanged)");
    // Tabs.
    AI_EQ(aiTyped("a.py", "\tif x:\n\t\ty\n\t\telse:|"), "\tif x:\n\t\ty\n\telse:|");
    // Not a colon.
    AI_EQ(aiTyped("a.py", "if x:\n    y\n    else|"), "(unchanged)");
    AI_EQ(aiTyped("a.py", ""), "(unchanged)");

    GROUP("indent:python electric closers");
    AI_EQ(aiTyped("a.py", "x = foo(\n    a,\n    )|"), "x = foo(\n    a,\n)|");
    AI_EQ(aiTyped("a.py", "    d = {\n        'a': 1,\n        }|"), "    d = {\n        'a': 1,\n    }|");
    // Lined up after the bracket: left alone.
    AI_EQ(aiTyped("a.py", "x = foo(a,\n        )|"), "(unchanged)");
    // Not alone on the line.
    AI_EQ(aiTyped("a.py", "x = foo(\n    a)|"), "(unchanged)");
}

void testAutoIndentBraces() {
    GROUP("indent:braces");
    AI_EQ(aiRet("a.c", "int f() {|"), "int f() {\n    |");
    AI_EQ(aiRet("a.c", "int f() {|}"), "int f() {\n    |\n}");
    AI_EQ(aiRet("a.js", "  const a = [|]"), "  const a = [\n      |\n  ]");
    AI_EQ(aiRet("a.js", "foo(|)"), "foo(\n    |\n)");
    AI_EQ(aiRet("a.js", "foo({|})"), "foo({\n    |\n})");
    AI_EQ(aiRet("a.c", "    x = 1;|"), "    x = 1;\n    |");
    AI_EQ(aiRet("a.c", "    if (x) {  // why|"), "    if (x) {  // why\n        |");
    AI_EQ(aiRet("a.c", "    if (x) { /* why */|"), "    if (x) { /* why */\n        |");
    AI_EQ(aiRet("a.c", "    } else {|"), "    } else {\n        |");
    AI_EQ(aiRet("a.c", "    }|"), "    }\n    |");
    // Back to the line that opened what this line closes.
    AI_EQ(aiRet("a.c", "    foo(a,\n        b);|"), "    foo(a,\n        b);\n    |");
    AI_EQ(aiRetU("a.js", "  call(1,\n       2).then(x => {|", (AutoIndent::Unit{false, 2, true})), "  call(1,\n       2).then(x => {\n         |");
    // Text after the bracket on its line: keep the line's indentation.
    AI_EQ(aiRet("a.c", "    if (a &&|"), "    if (a &&\n    |");
    // A closer moved down goes to its opener's indentation.
    AI_EQ(aiRet("a.c", "  void f() {\n    x();|}"), "  void f() {\n    x();\n  |}");
    // Brackets in strings, characters and comments do not count.
    AI_EQ(aiRet("a.c", "    s = \"{\";|"), "    s = \"{\";\n    |");
    AI_EQ(aiRet("a.c", "    c = '{';|"), "    c = '{';\n    |");
    AI_EQ(aiRet("a.c", "    x = 1; // {|"), "    x = 1; // {\n    |");
    AI_EQ(aiRet("a.c", "/* { */ x = 1;|"), "/* { */ x = 1;\n|");
    AI_EQ(aiRet("a.js", "const s = `\n{`;|"), "const s = `\n{`;\n|");
    AI_EQ(aiRet("a.js", "const s = `a ${b} {`|"), "const s = `a ${b} {`\n|");
    AI_EQ(aiRet("a.rs", "fn f<'a>(x: &'a str) {|"), "fn f<'a>(x: &'a str) {\n    |");
    AI_EQ(aiRet("a.rs", "let c = '{';|"), "let c = '{';\n|");
    AI_EQ(aiRet("a.go", "s := `{\\`\nfunc f() {|"), "s := `{\\`\nfunc f() {\n\t|");
    AI_EQ(aiRet("a.kt", "val s = \"\"\"\n{\n\"\"\"\nfun f() {|"), "val s = \"\"\"\n{\n\"\"\"\nfun f() {\n    |");
    // Inside a string: keep the indentation.
    AI_EQ(aiRet("a.js", "  s = `a|b`"), "  s = `a\n  |b`");
    // CSS: a URL's // is not a comment.
    AI_EQ(aiRet("a.css", "a { background: url(http://x/y.png); |}"),
          "a { background: url(http://x/y.png);\n|}");
    AI_EQ(aiRet("a.css", "a {|"), "a {\n    |");
    AI_EQ(aiRet("a.json", "{\n  \"a\": [|"), "{\n  \"a\": [\n    |");
    AI_EQ(aiRet("a.json", "{\n  \"a\": \"http://x\"|"), "{\n  \"a\": \"http://x\"\n  |");
    // Go indents with tabs by default.
    AI_EQ(aiRet("a.go", "func f() {|"), "func f() {\n\t|");
    AI_EQ(aiRet("a.c", "int f() {\r\n    |"), "int f() {\r\n\r\n    |");

    GROUP("indent:block comments");
    AI_EQ(aiRet("a.c", "/**|"), "/**\n * |");
    AI_EQ(aiRet("a.c", "    /** Docs.|"), "    /** Docs.\n     * |");
    AI_EQ(aiRet("a.c", "/**\n * Docs.|"), "/**\n * Docs.\n * |");
    AI_EQ(aiRet("a.c", "/**|*/"), "/**\n * |\n */");
    AI_EQ(aiRet("a.c", "/*\n  plain|"), "/*\n  plain\n  |");
    AI_EQ(aiRet("a.c", "/** a */|"), "/** a */\n|");

    GROUP("indent:braces electric");
    AI_EQ(aiTyped("a.c", "int f() {\n    x();\n    }|"), "int f() {\n    x();\n}|");
    AI_EQ(aiTyped("a.c", "  if (a) {\n      }|"), "  if (a) {\n  }|");
    AI_EQ(aiTyped("a.c", "int f() {\n}|"), "(unchanged)");
    AI_EQ(aiTyped("a.c", "int f() {\n    x(); }|"), "(unchanged)");
    AI_EQ(aiTyped("a.c", "int f() {\n    \"\n    }|"), "int f() {\n    \"\n}|");
    AI_EQ(aiTyped("a.c", "int f() {\n    /*\n    }|"), "(unchanged)");
    AI_EQ(aiTyped("a.c", "int f(\n    }|"), "(unchanged)");      // the wrong closer
    AI_EQ(aiTyped("a.js", "foo(\n  a,\n  )|"), "foo(\n  a,\n)|");
    AI_EQ(aiTyped("a.js", "foo(a,\n    )|"), "(unchanged)");
    AI_EQ(aiTyped("a.c", "int f() {  // c\n    }|"), "int f() {  // c\n}|");
    AI_EQ(aiTyped("a.c", "\tint f() {\n\t\t}|"), "\tint f() {\n\t}|");
    AI_EQ(aiTyped("a.c", "int f() {\n    }|  "), "int f() {\n}|  ");
    AI_EQ(aiTyped("a.c", "x;|"), "(unchanged)");
    AI_EQ(aiTyped("a.txt", "a {\n  }|"), "(unchanged)");
}

void testAutoIndentOthers() {
    GROUP("indent:yaml");
    AI_EQ(aiRet("a.yml", "jobs:|"), "jobs:\n  |");
    AI_EQ(aiRet("a.yml", "jobs:\n  build:|"), "jobs:\n  build:\n    |");
    AI_EQ(aiRet("a.yml", "  name: x|"), "  name: x\n  |");
    AI_EQ(aiRet("a.yml", "key: value  # a: comment|"), "key: value  # a: comment\n|");
    AI_EQ(aiRet("a.yml", "url: \"http://a:b\"|"), "url: \"http://a:b\"\n|");
    AI_EQ(aiRet("a.yml", "- name:|"), "- name:\n    |");
    AI_EQ(aiRet("a.yml", "  - run: |\u2039\u203A"), "  - run: |\n      |");
    AI_EQ(aiRet("a.yml", "text: >-|"), "text: >-\n  |");
    AI_EQ(aiRet("a.yml", "a: x | y\u2039\u203A"), "a: x | y\n|");
    AI_EQ(aiRet("a.yml", "msg: it's done|"), "msg: it's done\n|");
    AI_EQ(aiRet("a.yml", "a:\n    b:|"), "a:\n    b:\n        |");   // the file's 4

    GROUP("indent:generic");
    AI_EQ(aiRet("a.sh", "f() {|"), "f() {\n    |");
    AI_EQ(aiRet("a.sh", "echo $# {|"), "echo $# {\n    |");          // $# is no comment
    AI_EQ(aiRet("a.sh", "echo x # {|"), "echo x # {\n|");
    AI_EQ(aiRet("a.sh", "  echo \\{|"), "  echo \\{\n  |");
    AI_EQ(aiRet("a.lua", "t = {|}"), "t = {\n    |\n}");
    AI_EQ(aiRet("a.lua", "x = 1 -- {|"), "x = 1 -- {\n|");
    AI_EQ(aiRet("a.toml", "a = [|"), "a = [\n    |");
    AI_EQ(aiRet("a.el", "(defun f ()\n  'x|"), "(defun f ()\n  'x\n  |");
    AI_EQ(aiTyped("a.lua", "t = {\n    1,\n    }|"), "t = {\n    1,\n}|");

    GROUP("indent:plain");
    // Plain text keeps the indentation up to the caret, as before.
    AI_EQ(aiRet("a.txt", "  a|"), "  a\n  |");
    AI_EQ(aiRet("a.txt", "  a |b"), "  a \n  |b");
    AI_EQ(aiRet("a.md", "    code {|"), "    code {\n    |");
    AI_EQ(aiRet("a.txt", "  |  a"), "  \n  |  a");
    AI_EQ(aiRet("README", "\tx|"), "\tx\n\t|");
    AI_EQ(aiRet("a.txt", "x\r\ny|"), "x\r\ny\r\n|");
    AI_EQ(aiTyped("a.md", "x:|"), "(unchanged)");
}

void testAutoIndentKeys() {
    GROUP("indent:tab");
    AI_EQ(aiTab("a.py", "|x"), "    |x");
    AI_EQ(aiTab("a.py", "ab|x"), "ab  |x");
    AI_EQ(aiTab("a.py", "def f():\n  if x:\n    |"), "def f():\n  if x:\n      |");            // the file's 2
    AI_EQ(aiTab("a.py", "a\u2039b\u203Ac"), "a   |c");
    AI_EQ(aiTab("a.go", "|x"), "(unchanged)");                            // tabs: a tab
    AI_EQ(aiTab("a.txt", "|x"), "(unchanged)");                           // no evidence
    AI_EQ(aiTab("a.txt", "  a\n|x"), "  a\n    |x");
    AI_EQ(aiTab("a.tsv", "  a\n|x"), "(unchanged)");
    AI_EQ(aiTab("a.py", "é|"), "é   |");
    // Several lines: indent them, blank ones left alone.
    AI_EQ(aiTab("a.py", "\u2039a\n\nb\u203A"), "\u2039    a\n\n    b\u203A");
    AI_EQ(aiTab("a.py", "x\u2039a\nb\u203A\nc"), "    x\u2039a\n    b\u203A\nc");
    AI_EQ(aiTab("a.py", "\u2039a\nb\n\u203Ac"), "\u2039    a\n    b\n\u203Ac");
    AI_EQ(aiTab("a.go", "\u2039a\nb\u203A"), "\u2039\ta\n\tb\u203A");
    AI_EQ(aiTab("a.py", "\u2039a\r\nb\u203A\r\n"), "\u2039    a\r\n    b\u203A\r\n");

    GROUP("indent:backtab");
    AI_EQ(aiBacktab("a.py", "    x|"), "x|");
    AI_EQ(aiBacktab("a.py", "      x|"), "    x|");
    AI_EQ(aiBacktab("a.py", "  |    x"), "|    x");
    AI_EQ(aiBacktab("a.py", "  x|"), "x|");
    AI_EQ(aiBacktab("a.py", "x|"), "(unchanged)");
    AI_EQ(aiBacktab("a.go", "\t\tx|"), "\tx|");
    AI_EQ(aiBacktab("a.py", "\u2039    a\n  b\nc\u203A"), "\u2039a\nb\nc\u203A");
    AI_EQ(aiBacktab("a.py", "    a\u2039b\n    c\u203Ad"), "a\u2039b\nc\u203Ad");
    AI_EQ(aiBacktab("a.py", "\u2039    a\n\u203A    b"), "\u2039a\n\u203A    b");

    GROUP("indent:backspace");
    AI_EQ(aiBackspace("a.py", "    |x"), "|x");
    AI_EQ(aiBackspace("a.py", "        |"), "    |");
    AI_EQ(aiBackspace("a.py", "      |"), "    |");
    AI_EQ(aiBackspace("a.py", "     |"), "(unchanged)");   // one space: the editor's own
    AI_EQ(aiBackspace("a.py", "  x |"), "(unchanged)");
    AI_EQ(aiBackspace("a.py", "|x"), "(unchanged)");
    AI_EQ(aiBackspace("a.py", "\u2039    \u203A"), "(unchanged)");
    AI_EQ(aiBackspace("a.go", "    |"), "(unchanged)");                 // tabs
    AI_EQ(aiBackspace("a.txt", "  a\n    |"), "  a\n|");   // spaces in the file
    AI_EQ(aiBackspace("a.txt", "    |"), "(unchanged)");      // no evidence
    AI_EQ(aiBackspace("a.py", "a:\n  if x:\n    |"), "a:\n  if x:\n  |");
}

void testAutoIndentFuzz() {
    GROUP("indent:fuzz");
    // Random text full of what the rules look at: every result must be a
    // valid replacement with the selection inside the new text.
    const char16_t alphabet[] = u"ab :#\\\"'`([{}])\n\n\n\r\t    /*-|>,\u00E9\U0001F600";
    const size_t alphaLen = sizeof(alphabet) / sizeof(alphabet[0]) - 1;
    const char *files[] = {"a.py", "a.c", "a.js", "a.yml", "a.sh", "a.txt", "a.go", "a.rs",
                           "a.css", "a.lua", "a.md", "a.json", "a.kt"};
    std::mt19937 rng(7);
    int bad = 0, calls = 0;
    auto valid = [&](const std::u16string &t, const AutoIndent::Edit &e) {
        ++calls;
        if (!e.changed) return true;
        if (e.replaceStart > t.size() || e.replaceLength > t.size() - e.replaceStart) return false;
        size_t n = t.size() - e.replaceLength + e.replacement.size();
        return e.selStart <= e.selEnd && e.selEnd <= n;
    };
    for (int round = 0; round < 3000; ++round) {
        std::u16string t;
        size_t len = rng() % 60;
        for (size_t i = 0; i < len; ++i) t += alphabet[rng() % alphaLen];
        size_t a = t.empty() ? 0 : rng() % (t.size() + 1);
        size_t b = rng() % 4 == 0 && !t.empty() ? rng() % (t.size() + 1) : a;
        if (a > b) std::swap(a, b);
        // Never between the halves of a surrogate pair.
        if (a > 0 && a < t.size() && t[a] >= 0xDC00 && t[a] < 0xE000) --a;
        if (b > 0 && b < t.size() && t[b] >= 0xDC00 && t[b] < 0xE000) --b;
        if (a > b) a = b;
        const char *f = files[rng() % (sizeof(files) / sizeof(files[0]))];
        AutoIndent::Edit e = AutoIndent::newline(t, a, b, f);
        if (!valid(t, e) || !e.changed || e.replacement.find(u'\n') == std::u16string::npos) ++bad;
        if (!valid(t, AutoIndent::typed(t, a, f))) ++bad;
        if (!valid(t, AutoIndent::tab(t, a, b, f))) ++bad;
        if (!valid(t, AutoIndent::backtab(t, a, b, f))) ++bad;
        if (!valid(t, AutoIndent::backspace(t, a, b, f))) ++bad;
    }
    CHECK(bad == 0);
    CHECK(calls == 15000);

    // A big file: Return at its end reads all of it.
    std::string big;
    for (int i = 0; i < 20000; ++i)
        big += "def f" + std::to_string(i) + "(a, b):\n    s = \"x: (\" + str(a)  # c\n"
               "    return [a,\n            b]\n\n";
    big += "class A:";
    std::u16string bt = toU16(big);
    auto t0 = std::chrono::steady_clock::now();
    AutoIndent::Edit e = AutoIndent::newline(bt, bt.size(), bt.size(), "big.py");
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(e.changed && e.replacement == u"\n    ");
    std::printf("  auto-indent: Return at the end of an 80,000-line Python file in %.1f ms\n", ms);
}

void testAutoIndent() {
    testAutoIndentLanguages();
    testAutoIndentDetect();
    testAutoIndentPython();
    testAutoIndentElectricPython();
    testAutoIndentBraces();
    testAutoIndentOthers();
    testAutoIndentKeys();
    testAutoIndentFuzz();
}

}  // namespace
