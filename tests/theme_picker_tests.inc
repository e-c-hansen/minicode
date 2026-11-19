// theme_picker_tests.inc — the `themepick:*` groups of run_tests.cpp
// (ThemePicker.h: the color theme picker's list and its preview, choose
// and cancel), kept in their own file and included there.

static void testThemePicker() {
    using namespace ThemePicker;

    GROUP("themepick:rows");
    {
        std::vector<Row> r = rows();
        CHECK(r.size() == Settings::themes().size());
        // Dark ones first, then light, each group by title; the first row
        // of each group says which it is.
        std::vector<std::string> ids;
        for (const Row &x : r) ids.push_back(x.id);
        CHECK((ids == std::vector<std::string>{"dark-modern", "dark-plus", "monokai",
                                               "solarized-dark", "tomorrow-night-blue",
                                               "light-modern", "light-plus", "quiet-light",
                                               "solarized-light"}));
        bool sawLight = false, ordered = true;
        for (size_t i = 0; i < r.size(); i++) {
            if (!r[i].dark) sawLight = true;
            else if (sawLight) ordered = false;   // a dark row after a light one
            const bool first = i == 0 || r[i].dark != r[i - 1].dark;
            CHECK(r[i].group == (first ? (r[i].dark ? "dark themes" : "light themes") : ""));
            if (i && r[i].dark == r[i - 1].dark) {
                std::string a = r[i - 1].title, b = r[i].title;
                for (char &c : a) c = (char)std::tolower((unsigned char)c);
                for (char &c : b) c = (char)std::tolower((unsigned char)c);
                CHECK(a < b);
            }
            const Theme *t = Settings::findTheme(r[i].id);
            CHECK(t && r[i].title == t->title && r[i].dark == t->dark);
        }
        CHECK(ordered && sawLight);
        CHECK(rowOf(r, "monokai") == 2);
        CHECK(rowOf(r, "solarized-light") == 8);
        CHECK(rowOf(r, "no-such-theme") == -1);
        CHECK(rowOf({}, "monokai") == -1);
    }

    GROUP("themepick:initial");
    {
        CHECK(Session("monokai").initialRow() == 2);
        CHECK(Session("light-plus").initialRow() == 6);
        // An id the list lacks (a file from a later version) opens on the top.
        Session odd("no-such-theme");
        CHECK(odd.initialRow() == 0);
        CHECK(odd.showing() == "no-such-theme" && !odd.previewing() && odd.isOpen());
    }

    GROUP("themepick:preview-cancel");
    {
        Session s("dark-modern");
        CHECK(s.showing() == "dark-modern" && !s.previewing());
        // The row on show already, and rows out of range, do nothing.
        CHECK(s.preview(0) == "");
        CHECK(s.preview(-1) == "" && s.preview(99) == "");
        // Down twice: each row shows once.
        CHECK(s.preview(1) == "dark-plus");
        CHECK(s.preview(2) == "monokai");
        CHECK(s.preview(2) == "");   // the pointer staying on the row
        CHECK(s.showing() == "monokai" && s.previewing());
        // Esc puts the set theme back, once.
        CHECK(s.cancel() == "dark-modern");
        CHECK(!s.isOpen() && s.showing() == "dark-modern" && !s.previewing());
        CHECK(s.cancel() == "");
        CHECK(s.preview(3) == "" && s.choose(3) == "");
        // Back on the set theme before Esc: nothing to put back.
        Session t("dark-modern");
        CHECK(t.preview(5) == "light-modern");
        CHECK(t.preview(0) == "dark-modern" && !t.previewing());
        CHECK(t.cancel() == "");
    }

    GROUP("themepick:choose");
    {
        Session s("dark-modern");
        CHECK(s.preview(1) == "dark-plus" && s.preview(2) == "monokai");
        CHECK(s.choose(2) == "monokai");
        CHECK(!s.isOpen() && s.showing() == "monokai");
        // Over: nothing more previews, chooses, or cancels.
        CHECK(s.preview(4) == "" && s.choose(4) == "" && s.cancel() == "");
        // Choosing the set theme writes nothing, wherever the list had been.
        Session same("quiet-light");
        CHECK(same.preview(0) == "dark-modern");
        CHECK(same.choose(same.initialRow()) == "");
        CHECK(!same.isOpen() && same.showing() == "quiet-light");
        // Return with no row (nothing matches what was typed) keeps it open.
        Session none("dark-modern");
        CHECK(none.choose(-1) == "" && none.isOpen());
        // A row chosen without a preview first (a click) is written too.
        Session click("dark-modern");
        CHECK(click.choose(8) == "solarized-light");
    }

    GROUP("themepick:settings");
    {
        // A preview changes every color the theme gives and leaves the
        // file's own: Settings::setTheme over a parsed file.
        Settings st = Settings::parse("theme = dark-modern\nsyntax.comment = #112233\n");
        const Theme *m = Settings::findTheme("monokai");
        CHECK(m != nullptr);
        if (!m) return;
        st.setTheme(*m);
        CHECK(std::string(st.theme().id) == "monokai");
        CHECK(st.background(Surface::Editor).r == Rgba::hex(m->background[(int)Surface::Editor]).r);
        CHECK(st.text(Surface::Terminal).g == Rgba::hex(m->text[(int)Surface::Terminal]).g);
        CHECK(st.syntax(TokenStyle::Comment).r == 0x11 && st.syntax(TokenStyle::Comment).b == 0x33);
        CHECK(st.isDark() == m->dark);
        st.setTheme(*Settings::findTheme("light-plus"));
        CHECK(!st.isDark());
        // What the picker then writes is View > Theme's line, comments kept.
        const std::string file = "# mine\nlsp.enabled = false  # quiet\n# theme = dark-modern\n";
        const std::string out = Settings::withTheme(file, "monokai", false);
        CHECK(out == "# mine\nlsp.enabled = false  # quiet\ntheme = monokai\n");
    }
}
