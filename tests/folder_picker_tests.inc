// folder_picker_tests.inc — the `folderpick:*` groups of run_tests.cpp
// (FolderPicker.h: minicode's own folder picker), kept in their own file and
// included there.

namespace {

std::vector<std::string> fpLabels(const std::vector<FolderPicker::Row> &rs) {
    std::vector<std::string> out;
    for (const auto &r : rs) {
        std::string l = toU8(r.label, nullptr);
        if (r.kind == FolderPicker::RowKind::OpenTyped) l = "open " + l;
        out.push_back(l);
    }
    return out;
}

FolderPicker::Listing fpListing(std::vector<std::string> names) {
    FolderPicker::Listing l;
    l.done = l.ok = true;
    l.names = std::move(names);
    return l;
}

}  // namespace

static void testFolderPicker() {
    using namespace FolderPicker;
    const std::string home = "/Users/me", base = "/Users/me/Code/app";

    GROUP("folderpick:normalize");
    CHECK(normalize("/a//b/./c/") == "/a/b/c");
    CHECK(normalize("/a/b/../c") == "/a/c");
    CHECK(normalize("/..") == "/");
    CHECK(normalize("/") == "/");
    CHECK(normalize("/a/b/../../..") == "/");
    CHECK(join("/", "x") == "/x");
    CHECK(join("/a", "x") == "/a/x");
    CHECK(abbreviate("/Users/me", home) == "~");
    CHECK(abbreviate("/Users/me/Code", home) == "~/Code");
    CHECK(abbreviate("/Users/meow", home) == "/Users/meow");
    CHECK(lastComponent("/") == "/");
    CHECK(lastComponent("~") == "~");
    CHECK(lastComponent("/a/bc/") == "bc");

    GROUP("folderpick:parse");
    {
        Input n = parse("mini", home, base);
        CHECK(!n.isPath && n.dir == base && n.partial == "mini" && !n.showHidden);
        CHECK(n.typed == base + "/mini");
        Input e = parse("", home, base);
        CHECK(!e.isPath && e.partial.empty() && e.typed.empty());
        Input h = parse(".con", home, base);
        CHECK(!h.isPath && h.showHidden);
        Input t = parse("~/Code/mi", home, base);
        CHECK(t.isPath && t.dir == "/Users/me/Code" && t.partial == "mi");
        CHECK(t.typed == "/Users/me/Code/mi");
        Input tilde = parse("~", home, base);
        CHECK(tilde.isPath && tilde.dir == home && tilde.partial.empty() && tilde.typed == home);
        Input abs = parse("/Users/", home, base);
        CHECK(abs.isPath && abs.dir == "/Users" && abs.partial.empty() && abs.typed == "/Users");
        Input root = parse("/", home, base);
        CHECK(root.dir == "/" && root.typed == "/");
        Input up = parse("../", home, base);
        CHECK(up.isPath && up.dir == "/Users/me/Code" && up.partial.empty());
        Input upup = parse("../../Do", home, base);
        CHECK(upup.dir == "/Users/me" && upup.partial == "Do");
        Input rel = parse("src/ma", home, base);
        CHECK(rel.isPath && rel.dir == base + "/src" && rel.partial == "ma");
        Input dot = parse("~/.c", home, base);
        CHECK(dot.isPath && dot.showHidden);
        Input lead = parse("  ~/Code/", home, base);
        CHECK(lead.isPath && lead.dir == "/Users/me/Code");
    }

    GROUP("folderpick:complete");
    CHECK(complete("~/Co", "/Users/me/Code", home) == "~/Code/");
    CHECK(complete("~", "/Users/me/Code", home) == "~/Code/");
    CHECK(complete("/Users/me/Co", "/Users/me/Code", home) == "/Users/me/Code/");
    CHECK(complete("../a", "/Users/me/Code/apps", home) == "../apps/");
    CHECK(complete("mini", "/Users/me/Code/minicode", home) == "~/Code/minicode/");
    CHECK(complete("", "/opt/x", home) == "/opt/x/");
    CHECK(complete("ro", "/", home) == "/");
    CHECK(slashCompletes("~/Co"));
    CHECK(slashCompletes("mini"));
    CHECK(!slashCompletes(""));
    CHECK(!slashCompletes("~"));
    CHECK(!slashCompletes("~/Code/"));
    CHECK(!slashCompletes(".."));
    CHECK(!slashCompletes("."));
    CHECK(!slashCompletes("~/Code/.."));

    GROUP("folderpick:rows-name");
    {
        const std::vector<std::string> recent = {"/Users/me/Code/minicode", "/Users/me/Code/app/lib"};
        const Listing l = fpListing({".git", "docs", "lib", "src"});
        // Empty: the recent folders, then the base's subfolders (each once,
        // hidden ones left out), then Choose in Finder.
        auto rs = rows(parse("", home, base), recent, l, home);
        CHECK(fpLabels(rs) == (std::vector<std::string>{
                                 "~/Code/minicode", "~/Code/app/lib", "~/Code/app/docs",
                                 "~/Code/app/src", "Choose in Finder…"}));
        CHECK(rs[0].recent && !rs[2].recent);
        CHECK(rs[0].nameAt == 7);   // "~/Code/" before "minicode"
        CHECK(rs.back().kind == RowKind::ChooseInFinder);
        // A name matches names first.
        auto m = rows(parse("mini", home, base), recent, l, home);
        CHECK(m.size() == 2 && toU8(m[0].label, nullptr) == "~/Code/minicode");
        CHECK(m[0].positions == (std::vector<size_t>{7, 8, 9, 10}));
        // A subfolder of the base named exactly is offered to open as typed.
        auto s = rows(parse("src", home, base), recent, l, home);
        CHECK(fpLabels(s) == (std::vector<std::string>{"~/Code/app/src", "open ~/Code/app/src",
                                                       "Choose in Finder…"}));
        // A leading dot lists hidden folders.
        auto h = rows(parse(".g", home, base), recent, l, home);
        CHECK(!h.empty() && toU8(h[0].label, nullptr) == "~/Code/app/.git");
        // While the base is listed, the recent folders are there already.
        Listing waiting;
        auto w = rows(parse("", home, base), recent, waiting, home);
        CHECK(w.size() == 3);
        CHECK(emptyText(parse("", home, base), waiting, home) == "Listing ~/Code/app…");
    }

    GROUP("folderpick:rows-path");
    {
        const Listing l = fpListing({".config", "Code", "Desktop", "Documents", "Downloads"});
        // Ending in '/': the folder itself first, so Return opens it.
        auto all = rows(parse("~/", home, base), {"/x"}, l, home);
        CHECK(fpLabels(all) == (std::vector<std::string>{
                                  "open ~", "~/Code", "~/Desktop", "~/Documents", "~/Downloads",
                                  "Choose in Finder…"}));
        // A name after it matches names only, prefixes first (Desktop holds
        // the d and the o apart).
        auto d = rows(parse("~/do", home, base), {}, l, home);
        CHECK(fpLabels(d) == (std::vector<std::string>{"~/Documents", "~/Downloads", "~/Desktop",
                                                       "Choose in Finder…"}));
        CHECK(d[0].positions == (std::vector<size_t>{2, 3}));
        // Typed out in full, the folder is offered to open as typed too.
        auto c = rows(parse("~/Code", home, base), {}, l, home);
        CHECK(fpLabels(c) == (std::vector<std::string>{"~/Code", "open ~/Code",
                                                       "Choose in Finder…"}));
        auto hid = rows(parse("~/.co", home, base), {}, l, home);
        CHECK(!hid.empty() && toU8(hid[0].label, nullptr) == "~/.config");
        Listing bad;
        bad.done = true;
        auto b = rows(parse("/nope/", home, base), {}, bad, home);
        CHECK(b.size() == 1 && b[0].kind == RowKind::ChooseInFinder);
        CHECK(emptyText(parse("/nope/", home, base), bad, home) == "Cannot read /nope");
        CHECK(emptyText(parse("~/zz", home, base), l, home) == "No matching folders");
    }

    GROUP("folderpick:list");
    {
        char tmpl[] = "/tmp/mc-folderpick-XXXXXX";
        const char *made = mkdtemp(tmpl);
        CHECK(made != nullptr);
        if (made) {
            const std::string root = made;
            for (const char *d : {"beta", "Alpha", ".hidden", "gamma"})
                mkdir((root + "/" + d).c_str(), 0755);
            FILE *f = std::fopen((root + "/file.txt").c_str(), "w");
            if (f) std::fclose(f);
            symlink((root + "/gamma").c_str(), (root + "/link-to-gamma").c_str());
            symlink((root + "/file.txt").c_str(), (root + "/link-to-file").c_str());
            bool ok = false;
            auto names = listSubfolders(root, nullptr, &ok);
            CHECK(ok);
            CHECK(names == (std::vector<std::string>{".hidden", "Alpha", "beta", "gamma",
                                                     "link-to-gamma"}));
            // Thousands of entries list fast (a home folder full of them).
            const std::string many = root + "/many";
            mkdir(many.c_str(), 0755);
            for (int i = 0; i < 3000; i++)
                mkdir((many + "/d" + std::to_string(i)).c_str(), 0755);
            double t0 = cpuMs();
            auto lots = listSubfolders(many, nullptr, &ok);
            CHECK(ok && lots.size() == 3000);
            Listing ll;
            ll.done = ll.ok = true;
            ll.names = lots;
            auto rs = rows(parse(many + "/d29", "/Users/me", root), {}, ll, "/Users/me");
            CHECK(rs.size() > 2 && toU8(rs[0].label, nullptr) == many + "/d29");
            CHECK_TIME(cpuMs() - t0, 400);
            std::atomic<bool> stop{true};
            auto cut = listSubfolders(many, &stop, &ok);
            CHECK(cut.size() < 3000);
            bool okMissing = true;
            CHECK(listSubfolders(root + "/missing", nullptr, &okMissing).empty() && !okMissing);
            std::filesystem::remove_all(root);
        }
    }
}
