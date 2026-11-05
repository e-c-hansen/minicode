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
        // Whole lines; of two matches, the one nearest the cursor.
        Applied a = applyEdits(t, {edit("    return x * 2", "    return x * 3")}, t.size());
        CHECK(a.ok && a.text == "def f(x):\n    return x * 2\n\ndef g(x):\n    return x * 3\n");
        a = applyEdits(t, {edit("    return x * 2", "    return x * 3")}, 0);
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
