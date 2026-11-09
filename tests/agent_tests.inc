// agent_tests.inc — the `agent:*` group of run_tests.cpp (InlineAgent.h),
// kept in its own file and included there.

static void testInlineAgent() {
    using namespace InlineAgent;

    GROUP("agent:edit-prompt");
    {
        EditRequest r;
        r.request = "  add a docstring \n";
        r.path = "src/util.py";
        r.text = "def f(x):\n    return x * 2\n";
        r.selStart = r.selEnd = 14;   // in "return"
        std::string p = editPrompt(r);
        CHECK(p.find("the file src/util.py and asked:\n\nadd a docstring\n") != std::string::npos);
        CHECK(p.find("The cursor is on line 2, column 5, which reads:\n    return x * 2\n") !=
              std::string::npos);
        CHECK(p.find("```\ndef f(x):\n    return x * 2\n```\n") != std::string::npos);
        CHECK(p.find("<<<<<<< SEARCH") != std::string::npos);
        CHECK(p.find("Earlier") == std::string::npos);
        // A selection, and earlier requests.
        r.selStart = 0;
        r.selEnd = 10;   // "def f(x):\n"
        r.earlier = {"rename x to n"};
        p = editPrompt(r);
        CHECK(p.find("selected part of line 1:\n```\ndef f(x):\n```") != std::string::npos);
        CHECK(p.find("they are already done:\n- rename x to n\n") != std::string::npos);
        r.selEnd = r.text.size();
        CHECK(editPrompt(r).find("selected lines 1 to 2:") != std::string::npos);
        // A file holding ``` gets a longer fence.
        r.text = "x = '```'\n";
        r.selStart = r.selEnd = 0;
        CHECK(editPrompt(r).find("````\nx = '```'\n````") != std::string::npos);
        // An empty line at the cursor; an empty file.
        r.text = "a\n\nb\n";
        r.selStart = r.selEnd = 2;
        CHECK(editPrompt(r).find("line 2, column 1, an empty line.") != std::string::npos);
        r.text = "";
        r.selStart = r.selEnd = 0;
        CHECK(editPrompt(r).find("line 1, column 1, an empty line.") != std::string::npos);
        // A big file: only the lines around the cursor.
        std::string big;
        for (int i = 1; i <= 20000; i++) big += "line number " + std::to_string(i) + " of the file\n";
        r.text = big;
        r.selStart = r.selEnd = big.find("line number 10000 ");
        p = editPrompt(r);
        CHECK(p.size() < kMaxFileBytes + 4000);
        CHECK(p.find("Lines 9600 to 10400 of the 20001 in the file") != std::string::npos);
        CHECK(p.find("line number 9600 of") != std::string::npos);
        CHECK(p.find("line number 9599 of") == std::string::npos);
    }

    GROUP("agent:parse");
    {
        Reply r = parseReply("<<<<<<< SEARCH\n    return x * 2\n=======\n    \"\"\"Doubles.\"\"\"\n"
                             "    return x * 2\n>>>>>>> REPLACE\n");
        CHECK(r.edits.size() == 1 && r.answer.empty());
        CHECK(r.edits[0].search == "    return x * 2");
        CHECK(r.edits[0].replace == "    \"\"\"Doubles.\"\"\"\n    return x * 2");
        // Fences and chatter around the blocks are ignored; two blocks; CRLF.
        r = parseReply("Here:\r\n```python\r\n<<<<<<< SEARCH\r\na\r\n=======\r\nb\r\n>>>>>>> REPLACE"
                       "\r\n<<<<<<< SEARCH\r\nc\r\n=======\r\n>>>>>>> REPLACE\r\n```\r\n");
        CHECK(r.edits.size() == 2);
        CHECK(r.edits[0].search == "a" && r.edits[0].replace == "b");
        CHECK(r.edits[1].search == "c" && !r.edits[1].hasReplace);
        // An empty SEARCH; an unfinished block; a plain answer.
        r = parseReply("<<<<<<< SEARCH\n=======\nnew\n>>>>>>> REPLACE\n");
        CHECK(r.edits.size() == 1 && !r.edits[0].hasSearch && r.edits[0].replace == "new");
        r = parseReply("<<<<<<< SEARCH\na\n=======\nb\n");
        CHECK(r.edits.empty() && !r.answer.empty());
        r = parseReply("\n  It doubles its argument.  \n\n");
        CHECK(r.edits.empty() && r.answer == "It doubles its argument.");
    }

    GROUP("agent:apply");
    {
        auto edit = [](const char *s, const char *rep) {
            Edit e;
            e.search = s;
            e.replace = rep;
            return e;
        };
        const std::string t = "def f(x):\n    return x * 2\n\ndef g(x):\n    return x * 2\n";
        // Whole lines; of two matches, the one the cursor is in.
        const size_t inG = t.rfind("return");
        Applied a = applyEdits(t, {edit("    return x * 2", "    return x * 3")}, inG);
        CHECK(a.ok && a.text == "def f(x):\n    return x * 2\n\ndef g(x):\n    return x * 3\n");
        a = applyEdits(t, {edit("    return x * 2", "    return x * 3")}, t.find("return"));
        CHECK(a.ok && a.text == "def f(x):\n    return x * 3\n\ndef g(x):\n    return x * 2\n");
        // Several blocks, each on the result of the last.
        a = applyEdits(t, {edit("def f(x):", "def f(n):"), edit("def f(n):\n    return x * 2",
                                                               "def f(n):\n    return n * 2")},
                       0);
        CHECK(a.ok && a.text.rfind("def f(n):\n    return n * 2\n\ndef g", 0) == 0);
        // Part of a line.
        a = applyEdits("x = foo(1)\n", {edit("foo(", "bar(")}, 0);
        CHECK(a.ok && a.text == "x = bar(1)\n");
        // Whitespace the model got wrong: trailing, then indentation.
        a = applyEdits("if a:\n    b()   \n", {edit("    b()", "    c()")}, 0);
        CHECK(a.ok && a.text == "if a:\n    c()   \n");   // found as text
        a = applyEdits("if a:\n    b()   \n    d()\n", {edit("    b()\n    d()", "    c()")}, 0);
        CHECK(a.ok && a.text == "if a:\n    c()\n");      // as lines, trailing blanks aside
        a = applyEdits("if a:\n\tb()\n", {edit("  b()", "\tc()")}, 0);
        CHECK(a.ok && a.text == "if a:\n\tc()\n");
        // Deleting lines, in the middle and at the end.
        Edit del = edit("b", "");
        del.hasReplace = false;
        a = applyEdits("a\nb\nc\n", {del}, 0);
        CHECK(a.ok && a.text == "a\nc\n");
        a = applyEdits("a\nb", {del}, 0);
        CHECK(a.ok && a.text == "a");
        // A blank line put in place of one.
        a = applyEdits("a\nb\nc\n", {edit("b", "")}, 0);
        CHECK(a.ok && a.text == "a\n\nc\n");
        // CRLF stays CRLF.
        a = applyEdits("a\r\nb\r\n", {edit("a", "x\ny")}, 0);
        CHECK(a.ok && a.text == "x\r\ny\r\nb\r\n");
        // An empty SEARCH inserts at the cursor: into an empty file, and on
        // an empty last line.
        Edit ins;
        ins.hasSearch = false;
        ins.replace = "print(1)";
        a = applyEdits("", {ins}, 0);
        CHECK(a.ok && a.text == "print(1)");
        a = applyEdits("x = 1\n", {ins}, 6);
        CHECK(a.ok && a.text == "x = 1\nprint(1)");
        a = applyEdits("x = 1", {ins}, 5);
        CHECK(a.ok && a.text == "x = 1\nprint(1)");
        // No match: nothing changes, and which block is named.
        a = applyEdits(t, {edit("def f(x):", "def f(y):"), edit("nowhere", "x")}, 0);
        CHECK(!a.ok && a.text.empty() && a.error.find("edit 2 of 2") != std::string::npos);
        a = applyEdits(t, {edit("nowhere", "x")}, 0);
        CHECK(!a.ok && a.error == "The agent's edit did not match the file, so nothing was changed.");
        // Parse then apply, as the bar does.
        Reply r = parseReply("<<<<<<< SEARCH\ndef g(x):\n=======\n@cache\ndef g(x):\n>>>>>>> REPLACE\n");
        a = applyEdits(t, r.edits, 0);
        CHECK(a.ok && a.text.find("\n\n@cache\ndef g(x):\n") != std::string::npos);
        CHECK(lineOf("a\nb\nc", 4) == 3 && lineOf("a\nb", 0) == 1 && lineOf("ab", 99) == 1);
    }

    GROUP("inline:ambiguous");
    {
        auto edit = [](const char *s, const char *rep) {
            Edit e;
            e.search = s;
            e.replace = rep;
            return e;
        };
        const std::string t = "def f(x):\n    return x * 2\n\ndef g(x):\n    return x * 2\n";
        // Two places and the cursor in neither: nothing changes, and why.
        Applied a = applyEdits(t, {edit("    return x * 2", "    return x * 3")}, 0);
        CHECK(!a.ok && a.text.empty());
        CHECK(a.error == "The agent's edit matches 2 places in the file, so nothing was changed. "
                         "Select the part to change and ask again.");
        a = applyEdits(t, {edit("    return x * 2", "    return x * 3")}, t.size());
        CHECK(!a.ok && a.error.find("matches 2 places") != std::string::npos);
        // A selection that touches one of them picks it; one over both does not.
        const size_t g = t.find("def g");
        a = applyEdits(t, {edit("    return x * 2", "    return x * 3")}, g, t.size());
        CHECK(a.ok && a.text == "def f(x):\n    return x * 2\n\ndef g(x):\n    return x * 3\n");
        a = applyEdits(t, {edit("    return x * 2", "    return x * 3")}, 0, t.size());
        CHECK(!a.ok);
        // A caret at a line's very end still holds it.
        a = applyEdits(t, {edit("    return x * 2", "    return x * 3")}, t.find("\n\ndef g"));
        CHECK(a.ok && a.text.rfind("def f(x):\n    return x * 3\n", 0) == 0);
        // As text inside lines: "x * 2" is in two places too.
        a = applyEdits(t, {edit("x * 2", "x + x")}, 0);
        CHECK(!a.ok && a.error.find("matches 2 places") != std::string::npos);
        a = applyEdits(t, {edit("x * 2", "x + x")}, t.rfind("x * 2") + 1);
        CHECK(a.ok && a.text.find("    return x + x\n") == t.rfind("    return"));
        // Ignoring whitespace as well.
        const std::string ws = "if a:\n  b()\nif c:\n  b()\n";
        a = applyEdits(ws, {edit("    b()", "    d()")}, 0);
        CHECK(!a.ok && a.error.find("matches 2 places") != std::string::npos);
        a = applyEdits(ws, {edit("    b()", "    d()")}, ws.rfind("b()"));
        CHECK(a.ok && a.text == "if a:\n  b()\nif c:\n  d()\n");
        // Which of several blocks is named, and the selection follows an
        // earlier block's change of length.
        a = applyEdits(t, {edit("def f(x):", "def f(x, y=1):"), edit("    return x * 2", "    return 0")},
                       0);
        CHECK(!a.ok && a.error.find("edit 2 of 2 matches 2 places") != std::string::npos);
        a = applyEdits(t, {edit("def f(x):", "def f(x, unused=1):"),
                           edit("    return x * 2", "    return 0")},
                       t.rfind("return"));
        CHECK(a.ok && a.text == "def f(x, unused=1):\n    return x * 2\n\ndef g(x):\n    return 0\n");
        // One match anywhere is taken wherever the cursor is.
        a = applyEdits(t, {edit("def g(x):", "def h(x):")}, 0);
        CHECK(a.ok && a.text.find("def h(x):") != std::string::npos);
    }

    GROUP("inline:reindent");
    {
        auto edit = [](const char *s, const char *rep) {
            Edit e;
            e.search = s;
            e.replace = rep;
            return e;
        };
        // The model dropped the method's indentation: its lines go in at
        // the file's.
        const std::string py = "class A:\n    def f(self):\n        return 1\n";
        Applied a = applyEdits(py, {edit("def f(self):\n    return 1",
                                         "def f(self):\n    # one\n    return 2\n\ndef g(self):\n    pass")},
                               0);
        CHECK(a.ok && a.text == "class A:\n    def f(self):\n        # one\n        return 2\n\n"
                                "    def g(self):\n        pass\n");
        // Too deep: taken back, and a line less indented than the SEARCH
        // part's first moves by as much.
        a = applyEdits(py, {edit("            return 1", "            if x:\n                return 2\n        return 3")},
                       0);
        CHECK(a.ok && a.text == "class A:\n    def f(self):\n        if x:\n            return 2\n    return 3\n");
        // Spaces from the model where the file has tabs, nested levels too.
        const std::string go = "func f() {\n\tif x {\n\t\treturn 1\n\t}\n}\n";
        a = applyEdits(go, {edit("    if x {\n        return 1\n    }",
                                 "    if x {\n        y()\n        return 2\n    }")},
                       0);
        CHECK(a.ok && a.text == "func f() {\n\tif x {\n\t\ty()\n\t\treturn 2\n\t}\n}\n");
        // And tabs where the file has spaces.
        const std::string sp = "def f():\n    if x:\n        return 1\n";
        a = applyEdits(sp, {edit("\tif x:\n\t\treturn 1", "\tif x:\n\t\treturn 2")}, 0);
        CHECK(a.ok && a.text == "def f():\n    if x:\n        return 2\n");
        // An exact match is left as the model wrote it.
        a = applyEdits(py, {edit("        return 1", "      return 1")}, 0);
        CHECK(a.ok && a.text == "class A:\n    def f(self):\n      return 1\n");
        // Trailing blanks only: no change of indentation.
        a = applyEdits("if a:\n    b()  \n    c()\n", {edit("    b()\n    c()", "    d()")}, 0);
        CHECK(a.ok && a.text == "if a:\n    d()\n");
        // Blank lines stay blank, CRLF stays CRLF.
        a = applyEdits("x:\r\n    y\r\n", {edit("y", "y\n\nz")}, 0);
        CHECK(a.ok && a.text == "x:\r\n    y\r\n\r\n    z\r\n");
    }

    GROUP("inline:stream");
    {
        auto slurp = [](const char *path) {
            std::ifstream f(path, std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        };
        const std::string edit = slurp("tests/fixtures/claude/stream/edit.jsonl");
        const std::string answer = slurp("tests/fixtures/claude/stream/answer.jsonl");
        const std::string error = slurp("tests/fixtures/claude/stream/error.jsonl");
        CHECK(!edit.empty() && !answer.empty() && !error.empty());
        std::string want = "<<<<<<< SEARCH\n    return x * 2\n=======\n    return x * 3\n>>>>>>> REPLACE\n";
        // Fed a byte at a time: the text grows, the first turn's text gives way
        // to the tool call and then to the second turn's.
        {
            Stream st;
            std::vector<std::string> texts, acts;
            for (char c : edit)
                if (st.feed(std::string(1, c))) {
                    texts.push_back(st.text());
                    acts.push_back(st.activity());
                }
            st.finish();
            CHECK(st.sawJson() && st.done() && !st.failed());
            CHECK(st.reply() == want);
            CHECK(std::find(texts.begin(), texts.end(), "I'll look at ") != texts.end());
            CHECK(std::find(texts.begin(), texts.end(), "I'll look at the helper first.") != texts.end());
            CHECK(std::find(acts.begin(), acts.end(), "Reading a file") != acts.end());
            CHECK(std::find(acts.begin(), acts.end(), "Reading util.py") != acts.end());
            CHECK(std::find(texts.begin(), texts.end(), "<<<<<<< SEARCH\n    return x") != texts.end());
            CHECK(st.text() == want && st.activity().empty());
            CHECK(parseReply(st.reply()).edits.size() == 1);
        }
        // In one piece, and in pieces of a line or more.
        {
            Stream st;
            CHECK(st.feed(edit));
            CHECK(st.reply() == want && st.done());
            Stream st2;
            for (size_t i = 0; i < edit.size(); i += 333) st2.feed(edit.substr(i, 333));
            CHECK(st2.reply() == want);
        }
        {
            Stream st;
            st.feed(answer);
            CHECK(st.done() && st.reply() == "It doubles its argument." && st.error().empty());
        }
        {
            Stream st;
            st.feed(error);
            CHECK(st.done() && st.failed() && st.error() == "Credit balance is too low");
        }
        // A last line without its "\n" is read at the end.
        {
            Stream st;
            std::string cut = answer.substr(0, answer.size() - 1);
            st.feed(cut);
            CHECK(!st.done());
            st.finish();
            CHECK(st.done() && st.reply() == "It doubles its argument.");
        }
        // Stopped before the result: the text so far, and why.
        {
            Stream st;
            st.feed(answer.substr(0, answer.rfind("{\"type\":\"result\"")));
            st.finish();
            CHECK(!st.done() && st.reply() == "It doubles its argument.");
            CHECK(st.error() == "Claude Code stopped before it finished its answer.");
        }
        // Whole messages only (no partial messages): text from them, a
        // message's blocks joined.
        {
            Stream st;
            st.feed("{\"type\":\"assistant\",\"message\":{\"id\":\"m1\",\"content\":[{\"type\":\"text\",\"text\":\"Hel\"}]}}\n");
            CHECK(st.text() == "Hel");
            st.feed("{\"type\":\"assistant\",\"message\":{\"id\":\"m1\",\"content\":[{\"type\":\"text\",\"text\":\"lo\"}]}}\n");
            CHECK(st.text() == "Hello");
            st.feed("{\"type\":\"assistant\",\"message\":{\"id\":\"m2\",\"content\":[{\"type\":\"tool_use\",\"name\":\"Grep\",\"input\":{\"pattern\":\"parseReply\"}}]}}\n");
            CHECK(st.text().empty() && st.activity() == "Searching for parseReply");
            CHECK(st.reply() == "Hello");   // the last text a turn had
            st.feed("{\"type\":\"result\",\"subtype\":\"error_max_turns\",\"is_error\":false}\n");
            CHECK(st.failed() && st.error() == "Claude Code stopped: it used all its turns.");
        }
        // Not JSON at all (a stand-in agent, or another tool): the output is
        // the reply, as it came; a shell's greeting before JSON is skipped.
        {
            Stream st;
            CHECK(!st.feed("<<<<<<< SEARCH\nalpha\n=======\nALPHA\n>>>>>>> REPLACE\n"));
            st.finish();
            CHECK(!st.sawJson() && st.reply() == "<<<<<<< SEARCH\nalpha\n=======\nALPHA\n>>>>>>> REPLACE\n");
            Stream st2;
            st2.feed("Welcome back!\n");
            st2.feed(answer);
            CHECK(st2.sawJson() && st2.reply() == "It doubles its argument.");
            Stream st3;
            st3.feed("{not json}\n{\"no\":\"type\"}\n");
            st3.finish();
            CHECK(!st3.sawJson());
        }
        // Once a line is JSON the output as it came is never the reply, so
        // it is not kept: a run that reads big files (each a tool result
        // line with --verbose) holds only the line being read (AG-D3).
        {
            Stream st;
            st.feed("Welcome back!\n");
            CHECK(st.heldBytes() == 14);
            st.feed("{\"type\":\"system\"}\n");
            CHECK(st.heldBytes() == 0);
            const std::string big =
                "{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"content\":\"" +
                std::string(4 << 20, 'x') + "\"}]}}\n";
            for (int i = 0; i < 4; i++) st.feed(big);
            CHECK(st.heldBytes() == 0);
            st.feed(big.substr(0, 1000));
            CHECK(st.heldBytes() == 1000);
            st.feed(big.substr(1000));
            st.feed(answer);
            CHECK(st.heldBytes() == 0 && st.reply() == "It doubles its argument.");
        }
        CHECK(std::string(kPrintFlags).find("--output-format stream-json --verbose") != std::string::npos);
    }

    GROUP("inline:preview");
    {
        CHECK(preview("") == "");
        CHECK(preview("\nIt doubles its argument.\n\n") == "It doubles its argument.");
        // Edit blocks: the new lines, under a line saying what they replace.
        CHECK(preview("<<<<<<< SEARCH\n    return x * 2\n=======\n    return x * 3\n>>>>>>> REPLACE\n") ==
              "Edit 1, in place of 1 line:\n    return x * 3");
        CHECK(preview("<<<<<<< SEARCH\na\nb\n=======\nA\n") == "Edit 1, in place of 2 lines:\nA");
        CHECK(preview("<<<<<<< SEARCH\na\nb") == "Edit 1:");             // still in SEARCH
        CHECK(preview("<<<<<<< SEA") == "");                             // a marker on its way
        CHECK(preview("<<<<<<< SEARCH\n=======\nprint(1)\n>>>>>>> REPLACE\n"
                      "<<<<<<< SEARCH\nx\n=======\ny\n>>>>>>> RE") ==
              "Edit 1, new lines:\nprint(1)\n\nEdit 2, in place of 1 line:\ny");
        // Only the newest lines.
        std::string many;
        for (int i = 1; i <= 30; i++) many += "line " + std::to_string(i) + "\n";
        CHECK(preview(many, 3) == "line 28\nline 29\nline 30");
    }

    GROUP("inline:pending-diff");
    {
        const std::u16string before = u"a\nb\nc\nd\ne\nf\ng\nh\ni\nj\n";
        std::vector<DiffLine> d = pendingDiff(before, before);
        CHECK(d.empty());
        CHECK(changedLines(before, before) == 0);
        // One line changed: a line of context each side, old then new.
        const std::u16string one = u"a\nb\nc\nD\ne\nf\ng\nh\ni\nj\n";
        d = pendingDiff(before, one);
        CHECK(d.size() == 4);
        if (d.size() == 4) {
            CHECK(d[0].kind == DiffLine::Context && d[0].text == u"c" && d[0].oldLine == 3 && d[0].newLine == 3);
            CHECK(d[1].kind == DiffLine::Removed && d[1].text == u"d" && d[1].oldLine == 4 && d[1].newLine == 0);
            CHECK(d[2].kind == DiffLine::Added && d[2].text == u"D" && d[2].newLine == 4 && d[2].oldLine == 0);
            CHECK(d[3].kind == DiffLine::Context && d[3].text == u"e");
        }
        CHECK(changedLines(before, one) == 1);
        // Two runs far apart: a gap between them; close ones run together.
        const std::u16string two = u"A\nb\nc\nd\ne\nf\ng\nh\ni\nJ\n";
        d = pendingDiff(before, two);
        size_t gaps = 0;
        for (const DiffLine &l : d) gaps += l.kind == DiffLine::Gap;
        CHECK(gaps == 1 && d.front().kind == DiffLine::Removed && d.back().kind == DiffLine::Added);
        const std::u16string near = u"A\nb\nC\nd\ne\nf\ng\nh\ni\nj\n";
        d = pendingDiff(before, near);
        gaps = 0;
        for (const DiffLine &l : d) gaps += l.kind == DiffLine::Gap;
        CHECK(gaps == 0 && d.size() == 6);   // A's run, b, C's run, d
        // Words that changed inside a changed line.
        d = pendingDiff(u"    return x * 2\n", u"    return x * 3\n");
        CHECK(d.size() == 2);
        if (d.size() == 2) {
            CHECK(d[0].words.size() == 1 && d[0].words[0] == std::make_pair((size_t)15, (size_t)1));
            CHECK(d[1].words.size() == 1 && d[1].words[0] == std::make_pair((size_t)15, (size_t)1));
        }
        // Lines added and lines removed, CRLF read as lines.
        d = pendingDiff(u"a\r\nb\r\n", u"a\r\nx\r\ny\r\nb\r\n");
        CHECK(d.size() == 4 && d[1].kind == DiffLine::Added && d[1].text == u"x" && d[2].text == u"y");
        CHECK(changedLines(u"a\r\nb\r\n", u"a\r\nx\r\ny\r\nb\r\n") == 2);
        d = pendingDiff(u"a\nb\nc\n", u"a\nc\n");
        CHECK(d.size() == 3 && d[1].kind == DiffLine::Removed && d[1].text == u"b");
        // A long change is cut, the rest counted.
        std::u16string big, bigger;
        for (int i = 0; i < 100; i++) {
            big += u"line\n";
            bigger += u"LINE\n";
        }
        d = pendingDiff(big, bigger, 1, 50);
        CHECK(d.size() == 51 && d.back().kind == DiffLine::Gap && d.back().more == 150);
        CHECK(changedLines(big, bigger) == 100);
    }

    // Keep after the user changed the buffer (AG-D1): the selection the
    // blocks were placed by moves with the user's edits, and a change that
    // lands anywhere but where the bar showed it is caught.
    GROUP("inline:keep-rebase");
    {
        auto u = [](const std::string &s) { return std::u16string(s.begin(), s.end()); };
        auto keep = [&](const std::string &base, size_t sel, const std::string &now,
                        const std::vector<Edit> &edits, std::string *next) {
            const size_t moved = moveOffset(u(base), u(now), sel);
            Applied a = applyEdits(now, edits, moved, moved);
            *next = a.ok ? a.text : now;
            return a.ok;
        };
        const std::string base =
            "def double(x):\n    return x * 2\n\n\ndef twice(x):\n    return x * 2\n";
        Edit e;
        e.search = "    return x * 2";
        e.replace = "    return x * 3";
        const size_t sel = base.rfind("return");          // in the second copy
        Applied shown = applyEdits(base, {e}, sel, sel);
        CHECK(shown.ok && shown.text.rfind("x * 3") > base.find("twice"));
        const std::string pending = shown.text;
        // A line typed at the end, the caret moved into the first copy:
        // with the caret of the moment the block went to the first copy, a
        // place the bar never showed; moved, it is the second again.
        std::string now = base + "print(double(2))\n", next;
        const size_t caretNow = now.find("return");
        Applied old = applyEdits(now, {e}, caretNow, caretNow);
        CHECK(old.ok && !sameChange(u(base), u(pending), u(now), u(old.text)));
        CHECK(keep(base, sel, now, {e}, &next));
        CHECK(next == pending + "print(double(2))\n");
        CHECK(sameChange(u(base), u(pending), u(now), u(next)));
        // A line added right above the block, and one far above: still
        // the change shown.
        now = base;
        now.insert(now.rfind("    return"), "    # twice\n");
        now.insert(0, "import math\n");
        CHECK(keep(base, sel, now, {e}, &next));
        CHECK(next.find("# twice\n    return x * 3\n") != std::string::npos &&
              next.find("return x * 2") != std::string::npos);
        CHECK(sameChange(u(base), u(pending), u(now), u(next)));
        // The second function deleted: the only match left is the first,
        // which the bar never showed changing.
        now = "def double(x):\n    return x * 2\n";
        CHECK(keep(base, sel, now, {e}, &next));
        CHECK(next == "def double(x):\n    return x * 3\n");
        CHECK(!sameChange(u(base), u(pending), u(now), u(next)));
        // The user changed the lines the block replaces (found loosely):
        // what it takes out is not what the bar showed.
        now = base;
        now.replace(now.rfind("    return x * 2"), 16, "  return x * 2");
        CHECK(keep(base, sel, now, {e}, &next));
        CHECK(!sameChange(u(base), u(pending), u(now), u(next)));
        // Nothing changed: the same.
        CHECK(sameChange(u(base), u(pending), u(base), u(pending)));
        CHECK(moveOffset(u(base), u(base), sel) == sel);
    }

    GROUP("agent:command");
    {
        CommandRequest c;
        c.request = "find large files";
        c.directory = "/Users/me/proj";
        c.screen = "$ ls\nREADME.md  src";
        std::string p = commandPrompt(c);
        CHECK(p.find("in the folder /Users/me/proj, and asked:\n\nfind large files\n") !=
              std::string::npos);
        CHECK(p.find("```\n$ ls\nREADME.md  src\n```") != std::string::npos);
        c.screen = "  \n";
        CHECK(commandPrompt(c).find("last lines") == std::string::npos);
        std::string note;
        CHECK(commandFrom("find . -size +10M\n", &note) == "find . -size +10M" && note.empty());
        CHECK(commandFrom("```sh\n$ git log --oneline -5\n```\n", &note) == "git log --oneline -5");
        CHECK(commandFrom("Run this:\n```bash\ncd src && make\n```\nThen look.", &note) ==
              "cd src && make");
        CHECK(commandFrom("\n\nls -la\npwd\n\n", &note) == "ls -la\npwd");
        CHECK(commandFrom("# That needs a GUI app, not a command.\n", &note).empty() &&
              note == "That needs a GUI app, not a command.");
        // A comment line above a command is part of the command.
        CHECK(commandFrom("# list\nls\n", &note) == "# list\nls" && note.empty());
        CHECK(commandFrom("  \n", &note).empty() && note.empty());
    }

    GROUP("agent:task");
    {
        TaskRequest t;
        CHECK(taskPrompt(t).empty());
        t.request = "  explain this folder\n";
        t.where = "the file tree, with src/ selected";
        CHECK(taskPrompt(t) ==
              "explain this folder\n\n(Asked from minicode with Cmd+I, in the file tree, with src/ "
              "selected.)\n");
        t.where = "the editor";
        t.path = "src/a.cpp";
        t.selection = "int x;\n";
        t.firstLine = t.lastLine = 4;
        std::string p = taskPrompt(t);
        CHECK(p.find("in the editor. The file open in the editor is src/a.cpp.)") != std::string::npos);
        CHECK(p.find("The selected text, on line 4 of src/a.cpp:\n```\nint x;\n```\n") !=
              std::string::npos);
        t.where = "src/a.cpp in the editor";
        CHECK(taskPrompt(t).find("The file open") == std::string::npos);
        t.selection.clear();
        t.screen = "make: *** [all] Error 1";
        CHECK(taskPrompt(t).find("The last lines in that terminal:\n```\nmake: ***") !=
              std::string::npos);
    }
}
