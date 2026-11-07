// auto_close_tests.inc — the `autoclose:*` groups of run_tests.cpp:
// brackets and quotes closing as they are typed (AutoClose), with type-over
// and pair Backspace, by each language's rules, at one caret and several,
// against the real highlighter. Included there.

namespace {

// A small editor over AutoClose: a text with carets (marked "|" in the
// strings), typing and Backspace as the Mac's editor does them (AutoClose
// first, then MultiCaret), and the Tracker told about every change.
struct AutoCloseSim {
    std::u16string text;
    std::vector<MultiCaret::Caret> carets;
    AutoClose::Tracker tracker;
    std::string path;
    bool closing = true;
    AutoClose::Typed::Kind last = AutoClose::Typed::Kind::None;

    AutoCloseSim(const std::u16string &marked, const std::string &file) : path(file) {
        for (char16_t c : marked) {
            if (c == u'|') carets.push_back({text.size(), text.size()});
            else text += c;
        }
        if (carets.empty()) carets.push_back({text.size(), text.size()});
    }

    std::u16string shown() const {
        std::u16string out;
        size_t k = 0;
        for (size_t i = 0; i <= text.size(); i++) {
            while (k < carets.size() && carets[k].head == i) {
                out += u'|';
                k++;
            }
            if (i < text.size()) out += text[i];
        }
        return out;
    }

    void apply(const MultiCaret::Edit &e) {
        for (auto it = e.changes.rbegin(); it != e.changes.rend(); ++it) {
            text.replace(it->start, it->length, it->text);
            tracker.edited(it->start, it->length, it->text.size());
        }
        setCarets(e.carets);
    }

    void setCarets(std::vector<MultiCaret::Caret> c) {
        carets = MultiCaret::normalize(std::move(c), text.size());
        tracker.validate(MultiCaret::Text(text), carets);
    }

    void key(char16_t ch) {
        const MultiCaret::Text t(text);
        const std::string ext = path.substr(path.find_last_of('.') + 1);
        const AutoClose::Language &lang = AutoClose::languageFor(path);
        last = AutoClose::Typed::Kind::None;
        if (ch == u'\b') {
            MultiCaret::Edit e = AutoClose::deletePair(t, carets, lang, tracker);
            if (e.changes.empty()) e = MultiCaret::backspace(t, carets);
            apply(e);
            return;
        }
        AutoClose::Typed typed;
        if (SyntaxHighlighter::supports(ext)) {
            IncrementalHighlighter<char16_t> hl(ext);
            StringSource<char16_t> whole(text);
            hl.reset(whole, nullptr);
            AutoClose::HighlighterContext ctx(
                hl, std::unique_ptr<TextSource<char16_t>>(new StringSource<char16_t>(text)));
            typed = AutoClose::type(t, carets, ch, lang, ctx, tracker, closing);
        } else {
            typed = AutoClose::type(t, carets, ch, lang, AutoClose::Context(), tracker, closing);
        }
        last = typed.kind;
        if (typed.kind == AutoClose::Typed::Kind::None) {
            apply(MultiCaret::insert(carets, {std::u16string(1, ch)}));
            return;
        }
        apply(typed.edit);
        tracker.add(typed.closed);
    }

    AutoCloseSim &type(const std::u16string &keys) {
        for (char16_t c : keys) key(c);
        return *this;
    }
};

std::u16string typed(const std::u16string &marked, const std::u16string &keys,
                     const std::string &path = "a.py") {
    AutoCloseSim s(marked, path);
    s.type(keys);
    return s.shown();
}

}  // namespace

static void testAutoClose() {
    using AutoClose::Typed;

    GROUP("autoclose:brackets");
    {
        CHECK(typed(u"print|", u"(") == u"print(|)");
        CHECK(typed(u"print|", u"()") == u"print()|");         // typed over
        CHECK(typed(u"print|", u"(a)") == u"print(a)|");
        CHECK(typed(u"print|", u"(\b") == u"print|");          // the pair goes
        CHECK(typed(u"x = |", u"[") == u"x = [|]");
        CHECK(typed(u"x = |;", u"{") == u"x = {|};");          // before ; as VS Code
        CHECK(typed(u"f|, g", u"(") == u"f(|), g");
        CHECK(typed(u"|foo", u"(") == u"(|foo");               // never before a word
        CHECK(typed(u"f|", u"(())") == u"f(())|");             // nested, each typed over
        CHECK(typed(u"print(|)", u"(") == u"print((|))");      // balanced: closes
        // A closer already waiting for an opener gets one, not a pair.
        CHECK(typed(u"foo|)", u"(") == u"foo(|)");
        CHECK(typed(u"foo|)", u"()") == u"foo()|)");
        // Each kind is counted on its own; strings and comments do not count.
        CHECK(typed(u"f|]", u"(") == u"f(|)]");
        CHECK(typed(u"f| \")\"", u"(") == u"f(|) \")\"");
        CHECK(typed(u"f| # )", u"(") == u"f(|) # )");
        // A closer typed by hand is not typed over, nor deleted with its
        // opener.
        CHECK(typed(u"f(|)", u")") == u"f()|)");
        CHECK(typed(u"f(|)", u"\b") == u"f|)");
        // The caret leaving the pair ends it.
        {
            AutoCloseSim s(u"f|", "a.py");
            s.type(u"(");
            CHECK(s.tracker.actions() == 1);
            s.setCarets({{1, 1}});   // before the opener
            CHECK(s.tracker.actions() == 0);
            s.setCarets({{2, 2}});
            s.type(u")");
            CHECK(s.shown() == u"f()|)");
        }
        // So does a pair spanning lines (Return inside it).
        {
            AutoCloseSim s(u"f|", "a.c");
            s.type(u"{");
            s.apply(MultiCaret::insert(s.carets, {u"\n"}));
            CHECK(s.tracker.actions() == 0);
        }
        // Typing inside keeps it.
        {
            AutoCloseSim s(u"f|", "a.py");
            s.type(u"(abc");
            CHECK(s.tracker.actions() == 1);
            s.type(u")");
            CHECK(s.shown() == u"f(abc)|" && s.last == Typed::Kind::Overtype);
        }
        // Backspace of a single character inside, then the pair.
        CHECK(typed(u"f|", u"(a\b\b") == u"f|");
    }

    GROUP("autoclose:quotes");
    {
        CHECK(typed(u"x = |", u"\"") == u"x = \"|\"");
        CHECK(typed(u"x = |", u"\"\"") == u"x = \"\"|");
        CHECK(typed(u"x = |", u"'a'") == u"x = 'a'|");
        CHECK(typed(u"x = |", u"'\b") == u"x = |");
        CHECK(typed(u"don|", u"'") == u"don'|");                // after a word character
        CHECK(typed(u"don|", u"\"") == u"don\"|");
        CHECK(typed(u"|abc", u"\"") == u"\"|abc");              // before a word
        // Python's prefixes close: the opener is longer than the quote.
        CHECK(typed(u"x = f|", u"\"") == u"x = f\"|\"");
        CHECK(typed(u"x = rb|", u"'") == u"x = rb'|'");
        CHECK(typed(u"x = F|", u"'") == u"x = F'|'");
        // A quote that ends a string closes nothing.
        CHECK(typed(u"x = \"abc|", u"\"") == u"x = \"abc\"|");
        CHECK(typed(u"x = 'it is|", u"'") == u"x = 'it is'|");
        // In a comment " pairs in Python and ' does not (VS Code's rules).
        CHECK(typed(u"# say |", u"'") == u"# say '|");
        CHECK(typed(u"# say |", u"\"") == u"# say \"|\"");
        // Inside a string a quote of the other kind is plain.
        CHECK(typed(u"x = \"a |", u"'") == u"x = \"a '|");
        // Not typed over after a backslash: an escaped quote goes in.
        CHECK(typed(u"x = |", u"\"\\\"") == u"x = \"\\\"|\"");
        // The triple quote closes on its third quote (the plan's, not VS
        // Code's), and closing a docstring closes nothing more.
        CHECK(typed(u"def f():\n    |", u"\"\"\"") == u"def f():\n    \"\"\"|\"\"\"");
        CHECK(typed(u"x = |", u"'''") == u"x = '''|'''");
        CHECK(typed(u"\"\"\"doc\"\"|", u"\"") == u"\"\"\"doc\"\"\"|");
        CHECK(typed(u"x = \"\"\"a\nb\"\"|", u"\"") == u"x = \"\"\"a\nb\"\"\"|");
        CHECK(typed(u"# \"\"|", u"\"") == u"# \"\"\"|");
        // Typing the closing quote of a prefixed string types over it.
        CHECK(typed(u"x = |", u"f\"{a}\"") == u"x = f\"{a}\"|");
        // A backtick pairs in Python (VS Code's configuration), not in a
        // string.
        CHECK(typed(u"x = |", u"`") == u"x = `|`");
        CHECK(typed(u"x = \"a |", u"`") == u"x = \"a `|");
    }

    GROUP("autoclose:languages");
    {
        // Markdown, LaTeX and plain text: brackets only (Markdown's < too,
        // as VS Code's markdown configuration has it).
        CHECK(typed(u"see |", u"(", "a.md") == u"see (|)");
        CHECK(typed(u"see |", u"'", "a.md") == u"see '|");
        CHECK(typed(u"see |", u"\"", "a.md") == u"see \"|");
        CHECK(typed(u"see |", u"`", "a.md") == u"see `|");
        CHECK(typed(u"see |", u"*", "a.md") == u"see *|");
        CHECK(typed(u"see |", u"<", "a.md") == u"see <|>");
        CHECK(typed(u"\\section|", u"{", "a.tex") == u"\\section{|}");
        CHECK(typed(u"a |", u"$", "a.tex") == u"a $|");
        CHECK(typed(u"a |", u"\"", "a.tex") == u"a \"|");
        CHECK(typed(u"a |", u"`", "a.tex") == u"a `|");
        CHECK(typed(u"a |", u"[", "notes.txt") == u"a [|]");
        CHECK(typed(u"a |", u"\"", "notes.txt") == u"a \"|");
        CHECK(typed(u"a |", u"'", "README") == u"a '|");
        // JavaScript: templates and ${ } inside them.
        CHECK(typed(u"x = |", u"`", "a.ts") == u"x = `|`");
        CHECK(typed(u"x = `a |`", u"${", "a.ts") == u"x = `a ${|}`");
        CHECK(typed(u"x = |", u"'it'", "a.js") == u"x = 'it'|");
        // C and C++: a block comment, and a doc comment from inside it.
        CHECK(typed(u"|", u"/*", "a.cpp") == u"/*|*/");
        CHECK(typed(u"|", u"/**", "a.cpp") == u"/**| */");
        CHECK(typed(u"char c = |", u"'", "a.c") == u"char c = '|'");
        // Rust's ' is a lifetime: plain.
        CHECK(typed(u"fn f<|", u"'", "a.rs") == u"fn f<'|");
        CHECK(typed(u"let s = |", u"\"", "a.rs") == u"let s = \"|\"");
        // Go's raw strings; JSON's strings; YAML's quotes anywhere.
        CHECK(typed(u"s := |", u"`", "a.go") == u"s := `|`");
        CHECK(typed(u"{|}", u"\"", "a.json") == u"{\"|\"}");
        CHECK(typed(u"key: |", u"'", "a.yaml") == u"key: '|'");
        // Shell.
        CHECK(typed(u"echo |", u"'", "run.sh") == u"echo '|'");
        CHECK(typed(u"echo |", u"$(", "run.sh") == u"echo $(|)");
        // A language with a grammar but no table here: the common pairs.
        CHECK(typed(u"x = |", u"\"", "a.lua") == u"x = \"|\"");
        CHECK(&AutoClose::languageFor("/x/Makefile") == &AutoClose::languageFor("b.mk"));
        CHECK(AutoClose::languageFor("/x/y/Z.PY").id == "python");
        CHECK(AutoClose::languageFor("/x/y/notes").id == "plaintext");
    }

    GROUP("autoclose:surround");
    {
        using MultiCaret::Caret;
        auto wrap = [](const std::u16string &text, std::vector<Caret> carets, char16_t ch,
                       const std::string &path) {
            AutoClose::Tracker none;
            AutoClose::Typed t = AutoClose::type(MultiCaret::Text(text), carets, ch,
                                                 AutoClose::languageFor(path), AutoClose::Context(),
                                                 none);
            return t.kind == Typed::Kind::Surround ? MultiCaret::applyEdit(text, t.edit)
                                                   : u"(none)";
        };
        CHECK(wrap(u"a word", {{2, 6}}, u'(', "a.py") == u"a (word)");
        CHECK(wrap(u"a word", {{2, 6}}, u'"', "a.py") == u"a \"word\"");
        // Markdown surrounds with its own characters.
        CHECK(wrap(u"a word", {{2, 6}}, u'*', "a.md") == u"a *word*");
        CHECK(wrap(u"a word", {{2, 6}}, u'_', "a.md") == u"a _word_");
        CHECK(wrap(u"a word", {{2, 6}}, u'$', "a.md") == u"a $word$");
        CHECK(wrap(u"a word", {{2, 6}}, u'*', "a.py") == u"(none)");
        // JSON and Makefiles surround nothing; JavaScript's $ only prepends.
        CHECK(wrap(u"a word", {{2, 6}}, u'(', "a.json") == u"(none)");
        CHECK(wrap(u"a word", {{2, 6}}, u'"', "Makefile") == u"(none)");
        CHECK(wrap(u"a word", {{2, 6}}, u'$', "a.js") == u"a $word");
        // Not a selection of blanks, nor a quote typed over a quote.
        CHECK(wrap(u"a  word", {{1, 3}}, u'(', "a.py") == u"(none)");
        CHECK(wrap(u"a \"word", {{2, 3}}, u'\'', "a.py") == u"(none)");
        // Every caret needs a selection.
        CHECK(wrap(u"ab\ncd", {{0, 1}, {4, 4}}, u'(', "a.py") == u"(none)");
        CHECK(wrap(u"ab\ncd", {{0, 1}, {3, 4}}, u'[', "a.py") == u"[a]b\n[c]d");
    }

    GROUP("autoclose:carets");
    {
        CHECK(typed(u"a|\nb|", u"(") == u"a(|)\nb(|)");
        CHECK(typed(u"a|\nb|", u"()") == u"a()|\nb()|");
        CHECK(typed(u"a|\nb|", u"(\b") == u"a|\nb|");
        CHECK(typed(u"x = |\ny = |", u"\"hi\"") == u"x = \"hi\"|\ny = \"hi\"|");
        // All or nothing: one caret before a word, so no pair anywhere.
        CHECK(typed(u"a|x\nb|", u"(") == u"a(|x\nb(|");
        CHECK(typed(u"don|\nx = |", u"'") == u"don'|\nx = '|");
        // A caret that leaves its pair ends the pairs of the others too (one
        // keystroke, one action), as in VS Code.
        {
            AutoCloseSim s(u"a|\nb|", "a.py");
            s.type(u"(");
            s.setCarets({{2, 2}});
            CHECK(s.tracker.actions() == 1);   // fewer carets: still inside
            s.setCarets({{2, 2}, {6, 6}});
            s.setCarets({{0, 0}, {6, 6}});
            CHECK(s.tracker.actions() == 0);
        }
        // Three carets on one line.
        CHECK(typed(u"f| g| h|", u"[") == u"f[|] g[|] h[|]");
        // Many carets far apart share one balance window per stretch.
        {
            std::u16string marked;
            for (int i = 0; i < 2000; i++) marked += u"call|\n";
            marked += u")";   // one unmatched closer at the very end
            AutoCloseSim s(marked, "a.py");
            s.type(u"(");
            // Carets within reach of the closer see it and get no pair, so
            // none does.
            CHECK(s.last == Typed::Kind::None);
            AutoCloseSim s2(marked.substr(0, marked.size() - 1), "a.py");
            s2.type(u"(");
            CHECK(s2.last == Typed::Kind::Close);
        }
    }

    GROUP("autoclose:off");
    {
        AutoCloseSim s(u"print|", "a.py");
        s.closing = false;
        s.type(u"(\"");
        CHECK(s.shown() == u"print(\"|");
        // Surround stays.
        AutoClose::Tracker none;
        AutoClose::Typed t = AutoClose::type(MultiCaret::Text(u"ab"), {{0, 2}}, u'(',
                                             AutoClose::languageFor("a.py"),
                                             AutoClose::Context(), none, false);
        CHECK(t.kind == Typed::Kind::Surround);
    }

    GROUP("autoclose:edit-shape");
    {
        // The typed character and its closer are two changes at one place,
        // the typed one first, so it goes in last when applied last first.
        AutoClose::Tracker none;
        AutoClose::Typed t = AutoClose::type(MultiCaret::Text(u"f"), {{1, 1}}, u'(',
                                             AutoClose::languageFor("a.py"),
                                             AutoClose::Context(), none);
        CHECK(t.kind == Typed::Kind::Close && t.edit.changes.size() == 2);
        CHECK(t.edit.changes[0].text == u"(" && t.edit.changes[1].text == u")");
        CHECK(t.edit.changes[0].start == 1 && t.edit.changes[1].start == 1);
        CHECK(t.closed.size() == 1 && t.closed[0].pairStart == 1 && t.closed[0].closeStart == 2 &&
              t.closed[0].closeEnd == 3);
        // Overtype moves the carets and changes nothing.
        AutoClose::Tracker tr;
        tr.add(t.closed);
        AutoClose::Typed o = AutoClose::type(MultiCaret::Text(u"f()"), {{2, 2}}, u')',
                                             AutoClose::languageFor("a.py"),
                                             AutoClose::Context(), tr);
        CHECK(o.kind == Typed::Kind::Overtype && o.edit.changes.empty() &&
              o.edit.carets[0] == (MultiCaret::Caret{3, 3}));
        // Nothing to do: None, and an empty edit.
        AutoClose::Typed n = AutoClose::type(MultiCaret::Text(u"f"), {{1, 1}}, u'x',
                                             AutoClose::languageFor("a.py"),
                                             AutoClose::Context(), none);
        CHECK(n.kind == Typed::Kind::None && n.edit.changes.empty());
    }

    GROUP("autoclose:tracker");
    {
        // Closer [5, 6) of pair [4, 6); edits before, at the edges, inside
        // and over it.
        auto after = [](size_t pos, size_t oldLength, size_t newLength, size_t at) {
            AutoClose::Tracker t;
            t.add({{4, 5, 6}});
            t.edited(pos, oldLength, newLength);
            return t.closerAt(at) && t.actions() == 1;
        };
        CHECK(after(0, 0, 3, 8));    // typed before: moves
        CHECK(after(5, 0, 2, 7));    // typed just before the closer (inside the pair)
        CHECK(after(6, 0, 2, 5));    // typed after it: stays
        CHECK(after(4, 0, 1, 6));    // typed before the opener: moves
        CHECK(after(0, 2, 0, 3));    // deleted before: moves back
        CHECK(!after(5, 1, 0, 5));   // the closer deleted: gone
        CHECK(!after(5, 1, 1, 5));   // or replaced
        CHECK(!after(0, 10, 2, 0));  // everything replaced (a reload)
        AutoClose::Tracker t;
        t.add({{4, 5, 6}});
        t.add({{0, 1, 2}});
        CHECK(t.actions() == 2);
        t.edited(5, 1, 0);
        CHECK(t.actions() == 1 && t.closerAt(1));
        t.clear();
        CHECK(t.empty());
    }

    GROUP("autoclose:highlighter-context");
    {
        // What a character typed at a place would be: VS Code's neutral
        // character test, from the highlighter's line state.
        const std::u16string s = u"x = \"abc\ny = '''doc\nmore\n";
        IncrementalHighlighter<char16_t> hl("py");
        StringSource<char16_t> src(s);
        hl.reset(src, nullptr);
        CHECK(hl.styleIfInserted(src, 8, u'0') == TokenStyle::String);    // in the open string
        CHECK(hl.styleIfInserted(src, 4, u'0') == TokenStyle::Number);    // before it
        CHECK(hl.styleIfInserted(src, s.size() - 1, u'0') == TokenStyle::String);   // in '''
        AutoClose::HighlighterContext ctx(
            hl, std::unique_ptr<TextSource<char16_t>>(new StringSource<char16_t>(s)));
        CHECK(ctx.at(5) == AutoClose::Scope::String && ctx.at(0) == AutoClose::Scope::Code);
        CHECK(ctx.ifTyped(16, u'0') == AutoClose::Scope::String);
        const std::vector<AutoClose::Span> spans = ctx.nonCode(0, s.size());
        CHECK(!spans.empty() && spans[0].start == 4 && spans[0].scope == AutoClose::Scope::String);
        // A line too long to lex closes nothing.
        std::u16string longLine(kMaxTokenizationLineLength + 10, u'a');
        longLine += u" ";
        AutoCloseSim sim(longLine + u"|", "a.py");
        sim.type(u"(");
        CHECK(sim.last == Typed::Kind::None);
    }
}
