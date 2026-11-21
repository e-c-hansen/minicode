// session_write_tests.inc — stage 7's page-side writes: Save As and New File
// from a browser's page, and Replace in Folder from it (SessionFiles.h's
// newFilePath, SessionSearch.h's "replace", SessionHost.h's onSaveAs and
// onReplace, SessionWeb.h's `saveas`, `newfile` and `replace`). The path
// rules as a function over real files, the replace rules over texts, then
// through a host with an app, a guest and a reader, every message through
// the encoding, and last a headless host's page over its socket, and an
// app's page whose window saves as. Included after session_search_tests.inc,
// whose pumpStream and runRead it uses, and the web tests' page helpers.

namespace {

bool pathThere(const std::string& p) {
    struct stat st;
    return lstat(p.c_str(), &st) == 0;
}

// A Find in Folder row of `content`'s line `line` (1-based) as the replace
// gets it back: its display text and its matches' count.
Json replaceRowJson(const std::string& content, int line, std::size_t count, bool on = true) {
    std::string raw;
    FolderSearch::lineAt(content, line, &raw);
    Json r = Json::object();
    r.set("line", Json((int64_t)line));
    r.set("text", Json(FolderSearch::displayText(raw)));
    r.set("count", Json((int64_t)count));
    r.set("on", Json(on));
    return r;
}

// The rows a page's search showed, as the replace request names them:
// every file's rows from a real search of `root`.
Json replaceFilesFrom(const std::vector<Json>& answers, const std::set<std::string>& offRows = {}) {
    std::map<std::string, Json> byFile;
    std::vector<std::string> order;
    for (const Json& a : answers)
        for (const Json& r : a["rows"].elements()) {
            const std::string path = r["path"].asString();
            if (!byFile.count(path)) {
                byFile[path] = Json::array();
                order.push_back(path);
            }
            Json row = Json::object();
            row.set("line", r["line"]);
            row.set("text", r["text"]);
            row.set("count", r["count"]);
            row.set("on", Json(!offRows.count(path + ":" + std::to_string(r["line"].asInt()))));
            byFile[path].push(std::move(row));
        }
    Json files = Json::array();
    for (const std::string& p : order) {
        Json f = Json::object();
        f.set("path", Json(p));
        f.set("rows", byFile[p]);
        files.push(std::move(f));
    }
    return files;
}

Json replaceAsk(const std::string& query, const std::string& replacement, Json files) {
    Json r = Json::object();
    r.set("query", Json(query));
    r.set("replacement", Json(replacement));
    r.set("files", std::move(files));
    return r;
}

std::string skippedOf(const Json& answer) {
    std::string s;
    for (const Json& k : answer["skipped"].elements())
        s += (s.empty() ? "" : " ") + k["path"].asString() + ":" + k["reason"].asString();
    return s;
}

}  // namespace

static void testSessionWrites() {
    using Shared::NewPath;
    using Shared::newFilePath;

    GROUP("host:new-path");
    {
        const std::string root = fsScratch(), elsewhere = fsScratch();
        mkdir((root + "/src").c_str(), 0755);
        mkdir((root + "/dir").c_str(), 0755);
        fsWrite(root + "/src/a.py", "a\n");
        fsWrite(elsewhere + "/out.txt", "out\n");
        symlink((root + "/src/a.py").c_str(), (root + "/inlink.py").c_str());
        symlink((elsewhere + "/out.txt").c_str(), (root + "/outlink.txt").c_str());
        symlink((root + "/nothing.txt").c_str(), (root + "/dangling.txt").c_str());
        symlink(elsewhere.c_str(), (root + "/outdir").c_str());
        NewPath p = newFilePath(root, "notes.txt", false);
        CHECK(p.ok && !p.exists && p.abs == root + "/notes.txt");
        p = newFilePath(root, "src/new.py", false);
        CHECK(p.ok && !p.exists && p.abs == root + "/src/new.py");
        p = newFilePath(root, "src/a.py", false);
        CHECK(p.ok && p.exists);
        p = newFilePath(root, "inlink.py", false);   // a link inside to a file
        CHECK(p.ok && p.exists);
        // Refused, each for its reason, nothing made.
        const std::pair<const char*, const char*> refused[] = {
            {"", "name"},           {"src/", "name"},       {"/etc/passwd", "name"},  {"../x.txt", "name"},
            {"src/../x", "name"},   {"./x", "name"},        {"a//b", "name"},         {".env", "dotfile"},
            {"src/.x/y", "dotfile"}, {"none/x.txt", "nofolder"}, {"src/a.py/x", "nofolder"},
            {"outdir/x.txt", "nofolder"}, {"outlink.txt", "outside"}, {"dangling.txt", "notfile"},
            {"dir", "notfile"},
        };
        for (const auto& [rel, reason] : refused) {
            p = newFilePath(root, rel, false);
            if (p.ok || p.reason != reason)
                std::printf("  host:new-path: %s -> %s (%s)\n", rel, p.reason.c_str(), p.why.c_str());
            CHECK(!p.ok && p.reason == reason && !p.why.empty());
        }
        CHECK(newFilePath(root, std::string("a\0b", 3), false).reason == "name");
        CHECK(newFilePath(root, std::string(300, 'x'), false).reason == "name");
        // Dotfiles when the page shows them; no folder at all.
        CHECK(newFilePath(root, ".env", true).ok);
        CHECK(newFilePath("", "x.txt", false).reason == "nofolder");
        CHECK(readFile((elsewhere + "/out.txt").c_str()) == "out\n");
    }

    GROUP("search:replace-rows");
    {
        using Shared::ReplaceCheck;
        using Shared::ReplaceRow;
        const std::string text = "foo = 1\n  bar(foo, foo)\nbaz\nfoo\r\n";
        FolderSearchOptions o = Shared::searchOptions(Json::object());
        auto rowsOf = [&](const std::string& t, std::vector<bool> on) {
            std::vector<ReplaceRow> rows;
            const int lines[] = {1, 2, 4};
            const std::size_t counts[] = {1, 2, 1};
            for (int i = 0; i < 3; i++) {
                std::string raw;
                FolderSearch::lineAt(t, lines[i], &raw);
                rows.push_back({lines[i], FolderSearch::displayText(raw), counts[i], on[(std::size_t)i]});
            }
            return rows;
        };
        std::string out;
        std::size_t n = 0;
        CHECK(Shared::replaceRows(text, rowsOf(text, {true, true, true}), "foo", "qux", o, &out, &n) ==
              ReplaceCheck::Replaced);
        CHECK(n == 4 && out == "qux = 1\n  bar(qux, qux)\nbaz\nqux\r\n");   // line endings kept
        // A row left out keeps its line; the others are replaced.
        CHECK(Shared::replaceRows(text, rowsOf(text, {true, false, true}), "foo", "qux", o, &out, &n) ==
              ReplaceCheck::Replaced);
        CHECK(n == 2 && out == "qux = 1\n  bar(foo, foo)\nbaz\nqux\r\n");
        CHECK(Shared::replaceRows(text, rowsOf(text, {false, false, false}), "foo", "qux", o, &out, &n) ==
              ReplaceCheck::Nothing);
        // Changed since the search: a row's line reads otherwise, a match
        // added on a line no row shows, a line gone.
        const std::vector<ReplaceRow> rows = rowsOf(text, {true, true, true});
        CHECK(Shared::replaceRows("foo = 2\n  bar(foo, foo)\nbaz\nfoo\r\n", rows, "foo", "qux", o, &out, &n) ==
              ReplaceCheck::Changed);
        CHECK(Shared::replaceRows(text + "foo again\n", rows, "foo", "qux", o, &out, &n) == ReplaceCheck::Changed);
        CHECK(Shared::replaceRows("foo = 1\n  bar(foo, foo)\n", rows, "foo", "qux", o, &out, &n) ==
              ReplaceCheck::Changed);
        // A regex with a group, as the search found it.
        Json rq = Json::object();
        rq.set("regex", Json(true));
        o = Shared::searchOptions(rq);
        const std::string calls = "call(a)\ncall(bc)\n";
        std::vector<ReplaceRow> rr = {{1, "call(a)", 1, true}, {2, "call(bc)", 1, true}};
        CHECK(Shared::replaceRows(calls, rr, "call\\((\\w+)\\)", "run($1)", o, &out, &n) == ReplaceCheck::Replaced);
        CHECK(n == 2 && out == "run(a)\nrun(bc)\n");
        // Rows from a page, checked: not rows, a line 0, and the cap.
        std::vector<ReplaceRow> got;
        Json bad = Json::array();
        bad.push(Json("x"));
        CHECK(!Shared::replaceRowsFrom(bad, 10, &got));
        Json zero = Json::array();
        zero.push(replaceRowJson("a\n", 1, 1));
        zero.push(Json::object());
        CHECK(!Shared::replaceRowsFrom(zero, 10, &got));
        Json many = Json::array();
        for (int i = 1; i <= 5; i++) many.push(replaceRowJson("a\nb\nc\nd\ne\n", i, 1, i != 3));
        CHECK(Shared::replaceRowsFrom(many, 3, &got) && got.size() == 3 && !got[2].on && got[0].text == "a");
        CHECK(Shared::replaceHidesPath(".env") && Shared::replaceHidesPath("src/.git/x") &&
              !Shared::replaceHidesPath("src/a.env"));
    }

    GROUP("host:save-as");
    {
        const std::string root = fsScratch();
        mkdir((root + "/sub").c_str(), 0755);
        Net net(true, true);
        net.host.setFolder(root);
        const std::string path = root + "/a.txt";
        fsWrite(path, "alpha\n");
        fsWrite(root + "/taken.txt", "taken\n");
        Net::Link& ed = net.add(Unit::Utf16, "editor");
        Net::Link& guest = net.add(Unit::Utf16, "browser", 2 | 8);
        Net::Link& reader = net.add(Unit::Utf16, "reader", 1);
        for (Net::Link* l : {&ed, &guest, &reader}) l->client->dispatch();
        CHECK(guest.client->hostWritesForPages() && reader.client->hostWritesForPages());
        TestMirror em;
        em.client = ed.client.get();
        em.doc = 1;
        em.text = u"alpha\n";
        ed.client->open(1, path, "alpha\n", &em);
        TestMirror gm;
        gm.client = guest.client.get();
        gm.doc = 1;
        guest.client->view(1, &gm);
        guest.client->dispatch();
        auto saveAs = [&](Net::Link& l, Shared::DocId doc, const std::string& rel, bool overwrite, bool hidden = false) {
            Shared::Client::SaveAnswer a;
            const std::uint64_t req = l.client->requestSaveAs(doc, 0, rel, hidden, overwrite);
            l.client->dispatch();
            if (!req || !l.client->saveAnswer(req, &a)) a.json = Json();
            return a;
        };
        auto pathOf = [&](Shared::DocId doc) {
            const Json st = net.host.state();   // kept: the loop reads into it
            for (const Json& d : st["documents"].elements())
                if ((Shared::DocId)d["doc"].asInt() == doc) return d["path"].asString();
            return std::string("?");
        };
        em.caret = em.length();
        em.type(u"beta\n");
        ed.client->settle();
        CHECK(net.host.dirty(1));

        // Saved as a new file: written, the document's path that file for
        // every view, clean; the old file left as it was.
        Shared::Client::SaveAnswer a = saveAs(guest, 1, "sub/b.txt", false);
        if (!a.ok) std::printf("  host:save-as: %s\n", a.json.dump().c_str());
        CHECK(a.ok && a.json["path"].asString() == root + "/sub/b.txt" && a.json["created"].asBool());
        CHECK(readFile((root + "/sub/b.txt").c_str()) == "alpha\nbeta\n" && readFile(path.c_str()) == "alpha\n");
        if (pathOf(1) != root + "/sub/b.txt" || net.host.dirty(1))
            std::printf("  host:save-as: path %s, dirty %d\n", pathOf(1).c_str(), (int)net.host.dirty(1));
        CHECK(pathOf(1) == root + "/sub/b.txt" && !net.host.dirty(1));
        CHECK(!guest.dirties.empty() && !guest.dirties.back().second);
        // Its next save by the stamp it wrote.
        em.type(u"gamma\n");
        ed.client->settle();
        Shared::Client::SaveAnswer s;
        const std::uint64_t sreq = guest.client->requestSave(1, 0, false);
        guest.client->dispatch();
        CHECK(sreq && guest.client->saveAnswer(sreq, &s) && s.ok);
        CHECK(readFile((root + "/sub/b.txt").c_str()) == "alpha\nbeta\ngamma\n");

        // A file there already: refused with the sentence until the person
        // says to replace it.
        a = saveAs(guest, 1, "taken.txt", false);
        CHECK(!a.ok && a.json["reason"].asString() == "exists" &&
              a.json["why"].asString() == "taken.txt already exists: saving would replace it.");
        CHECK(readFile((root + "/taken.txt").c_str()) == "taken\n" && pathOf(1) == root + "/sub/b.txt");
        a = saveAs(guest, 1, "taken.txt", true);
        CHECK(a.ok && readFile((root + "/taken.txt").c_str()) == "alpha\nbeta\ngamma\n" &&
              pathOf(1) == root + "/taken.txt");
        // Paths a page may not name, nothing written.
        CHECK(saveAs(guest, 1, "../out.txt", false).json["reason"].asString() == "name");
        CHECK(saveAs(guest, 1, "none/x.txt", false).json["reason"].asString() == "nofolder");
        CHECK(saveAs(guest, 1, ".env", false).json["reason"].asString() == "dotfile");
        a = saveAs(guest, 1, ".env", false, true);   // the tree shows dotfiles
        CHECK(a.ok && readFile((root + "/.env").c_str()) == "alpha\nbeta\ngamma\n");
        // Its own file again: a save, not a refusal.
        a = saveAs(guest, 1, ".env", false, true);
        CHECK(a.ok);

        // Never two documents of one path.
        TestMirror om;
        om.client = ed.client.get();
        om.doc = 2;
        om.text = u"alpha\n";
        ed.client->open(2, path, "alpha\n", &om);
        ed.client->settle();
        a = saveAs(guest, 1, "a.txt", true);
        CHECK(!a.ok && a.json["reason"].asString() == "open" && readFile(path.c_str()) == "alpha\n");
        // An app's window with it open names its own files.
        Net::Link& app = net.add(Unit::Utf16, "app", 4 | 16);
        app.client->dispatch();
        TestMirror am;
        am.client = app.client.get();
        am.doc = 2;
        app.client->open(2, path, "", &am);
        app.client->dispatch();
        TestMirror gm2;
        gm2.client = guest.client.get();
        gm2.doc = 2;
        guest.client->view(2, &gm2);
        guest.client->dispatch();
        a = saveAs(guest, 2, "c.txt", false);
        CHECK(!a.ok && a.json["reason"].asString() == "app" &&
              a.json["why"].asString() == "a.txt is open in a mutible window: use Save As there.");
        CHECK(!pathThere(root + "/c.txt"));

        // New File: an untitled document on the host, the guest's seat a view.
        const Shared::DocId nid = guest.client->newId();
        TestMirror nm;
        nm.client = guest.client.get();
        nm.doc = nid;
        CHECK(guest.client->openUntitled(nid, &nm));
        guest.client->dispatch();
        CHECK(net.host.hasDocument(nid) && pathOf(nid).empty() && nm.text.empty());
        // A plain save names it nothing: Save As does.
        Shared::Client::SaveAnswer u;
        const std::uint64_t ureq = guest.client->requestSave(nid, 0, false);
        guest.client->dispatch();
        CHECK(ureq && guest.client->saveAnswer(ureq, &u) && !u.ok && u.json["reason"].asString() == "nofile");
        nm.type(u"print('hi')\n");
        guest.client->settle();
        CHECK(net.host.text(nid) == u"print('hi')\n" && net.host.busyReason() != "");
        a = saveAs(guest, nid, "sub/new.py", false);
        CHECK(a.ok && readFile((root + "/sub/new.py").c_str()) == "print('hi')\n" &&
              pathOf(nid) == root + "/sub/new.py" && !net.host.dirty(nid));
        // One nobody typed into goes with the page; one with text stays.
        const Shared::DocId empty = guest.client->newId();
        TestMirror xm;
        xm.client = guest.client.get();
        xm.doc = empty;
        CHECK(guest.client->openUntitled(empty, &xm));
        guest.client->dispatch();
        const Shared::DocId kept = guest.client->newId();
        TestMirror km;
        km.client = guest.client.get();
        km.doc = kept;
        CHECK(guest.client->openUntitled(kept, &km));
        guest.client->dispatch();
        km.type(u"draft");
        guest.client->settle();
        CHECK(net.host.hasDocument(empty) && net.host.hasDocument(kept));
        // A reader makes nothing: its DocOpen of a new id is a view, gone.
        const Shared::DocId rid = reader.client->newId();
        TestMirror rm;
        CHECK(reader.client->openUntitled(rid, &rm));
        reader.client->dispatch();
        CHECK(!net.host.hasDocument(rid));
        // Nor saves as.
        TestMirror rv;
        rv.client = reader.client.get();
        rv.doc = 1;
        reader.client->view(1, &rv);
        reader.client->dispatch();
        a = saveAs(reader, 1, "r.txt", false);
        CHECK(!a.ok && !pathThere(root + "/r.txt"));
        net.host.disconnect(guest.id);
        CHECK(!net.host.hasDocument(empty) && net.host.hasDocument(kept) && net.host.hasDocument(nid));
        // Through the encoding: DocSave's saveAs and DocOpen's flag 2.
        Message m;
        m.kind = Shared::Kind::DocSave;
        m.json = Json::object();
        m.json.set("saveAs", Json("x/y.txt"));
        m.json.set("hidden", Json(true));
        m.flag = 1;
        const Message back = Shared::roundTrip(m);
        CHECK(back.json["saveAs"].asString() == "x/y.txt" && back.json["hidden"].asBool() && back.flag == 1);
        m = Message();
        m.kind = Shared::Kind::DocOpen;
        m.flag = 2;
        CHECK(Shared::roundTrip(m).flag == 2);
    }

    GROUP("host:replace");
    {
        const std::string root = fsScratch();
        mkdir((root + "/src").c_str(), 0755);
        fsWrite(root + "/open.py", "foo = 1\nprint(foo)\n");
        fsWrite(root + "/closed.py", "foo()\nbar()\nfoo(foo)\n");
        fsWrite(root + "/later.py", "foo\n");
        fsWrite(root + "/dirty.py", "foo\n");
        fsWrite(root + "/src/wide.txt", std::string("\xFF\xFE" "f\0o\0o\0\n\0", 10));   // UTF-16 LE
        fsWrite(root + "/latin.txt", "caf\xE9 foo\n");                                   // Windows-1252
        fsWrite(root + "/.hidden.py", "foo\n");
        Net net(true, true);
        net.host.setFolder(root);
        Net::Link& app = net.add(Unit::Utf16, "app", 4 | 16);
        Net::Link& guest = net.add(Unit::Utf16, "browser", 2 | 8);
        Net::Link& reader = net.add(Unit::Utf16, "reader", 1);
        for (Net::Link* l : {&app, &guest, &reader}) l->client->dispatch();
        // A window has open.py clean and dirty.py with typing in it.
        TestMirror om;
        om.client = app.client.get();
        om.doc = 1;
        om.text = u"foo = 1\nprint(foo)\n";
        app.client->open(1, root + "/open.py", Model::utf8FromUtf16(om.text), &om);
        TestMirror dm;
        dm.client = app.client.get();
        dm.doc = 2;
        dm.text = u"foo\n";
        app.client->open(2, root + "/dirty.py", "foo\n", &dm);
        dm.caret = dm.length();
        dm.type(u"# mine\n");
        app.client->settle();
        CHECK(!net.host.dirty(1) && net.host.dirty(2));

        // The page's search, as the host ran it.
        const std::vector<Json> found = runRead(root, searchAsk("foo"));
        CHECK(rowsOf(found) ==
              "closed.py:1 closed.py:3 dirty.py:1 later.py:1 latin.txt:1 open.py:1 open.py:2 src/wide.txt:1");
        // Then someone adds a match to later.py; the page leaves closed.py's
        // line 1 out.
        fsWrite(root + "/later.py", "foo\nfoo\n");
        Json files = replaceFilesFrom(found, {"closed.py:1"});
        // A crafted row for a dotfile the page never showed.
        Json hid = Json::object();
        hid.set("path", Json(".hidden.py"));
        Json hrows = Json::array();
        hrows.push(replaceRowJson("foo\n", 1, 1));
        hid.set("rows", hrows);
        files.push(hid);
        std::vector<Json> got;
        std::uint64_t req = guest.client->requestReplace(0, replaceAsk("foo", "qux", files));
        CHECK(req && pumpStream(net, *guest.client, req, &got));
        CHECK(got.size() == 1);
        const Json ans = got.empty() ? Json() : got[0];
        std::printf("  host:replace: %s\n", ans.dump().c_str());
        // The open document edited on the host, one step, the window's
        // mirror with it, the file on disk as it was.
        app.client->dispatch();
        CHECK(net.host.text(1) == u"qux = 1\nprint(qux)\n" && om.text == net.host.text(1));
        CHECK(readFile((root + "/open.py").c_str()) == "foo = 1\nprint(foo)\n" && net.host.dirty(1));
        CHECK(net.host.undoInfo(1).undoName == "Replace in Folder");
        // The closed file rewritten but for the row left out.
        CHECK(readFile((root + "/closed.py").c_str()) == "foo()\nbar()\nqux(qux)\n");
        // UTF-16 written back in it, with its mark.
        CHECK(readFile((root + "/src/wide.txt").c_str()) == std::string("\xFF\xFE" "q\0u\0x\0\n\0", 10));
        // Left alone and named: unsaved changes, a change since the search,
        // an encoding that cannot hold the replacement (not here: qux is
        // ASCII, so latin.txt is written in Windows-1252), a dotfile.
        CHECK(readFile((root + "/latin.txt").c_str()) == "caf\xE9 qux\n");
        CHECK(readFile((root + "/later.py").c_str()) == "foo\nfoo\n" && readFile((root + "/dirty.py").c_str()) == "foo\n");
        CHECK(net.host.text(2) == u"foo\n# mine\n");
        CHECK(readFile((root + "/.hidden.py").c_str()) == "foo\n");
        CHECK(skippedOf(ans) == "dirty.py:unsaved later.py:changed .hidden.py:dotfile");
        CHECK(ans["replaced"].asInt() == 6 && ans["files"].asInt() == 3 && ans["edited"].asInt() == 1 &&
              ans["editedMatches"].asInt() == 2);
        // Undo on the host takes the document's back.
        const std::uint64_t ureq = app.client->requestUndo(1, false);
        app.client->dispatch();
        CHECK(ureq && net.host.text(1) == u"foo = 1\nprint(foo)\n" && om.text == net.host.text(1));

        // A replacement Windows-1252 cannot store: the file left alone.
        got.clear();
        const std::vector<Json> again = runRead(root, searchAsk("caf"));
        req = guest.client->requestReplace(0, replaceAsk("caf", "caf→", replaceFilesFrom(again)));
        CHECK(req && pumpStream(net, *guest.client, req, &got) && got.size() == 1);
        CHECK(got.size() == 1 && skippedOf(got[0]) == "latin.txt:encoding" && got[0]["replaced"].asInt() == 0);
        CHECK(readFile((root + "/latin.txt").c_str()) == "caf\xE9 qux\n");

        // A reader replaces nothing.
        got.clear();
        const std::vector<Json> bars = runRead(root, searchAsk("bar"));
        reader.client->dispatch();
        req = reader.client->requestReplace(0, replaceAsk("bar", "baz", replaceFilesFrom(bars)));
        CHECK(req && pumpStream(net, *reader.client, req, &got) && got.size() == 1);
        CHECK(got.size() == 1 && got[0]["error"].asString() == "A page that only reads cannot replace.");
        CHECK(readFile((root + "/closed.py").c_str()) == "foo()\nbar()\nqux(qux)\n");
        // A query too short for a search replaces nothing either.
        got.clear();
        req = guest.client->requestReplace(0, replaceAsk("b", "z", replaceFilesFrom(bars)));
        CHECK(req && pumpStream(net, *guest.client, req, &got) && got.size() == 1 && got[0].has("error"));
        CHECK(readFile((root + "/closed.py").c_str()) == "foo()\nbar()\nqux(qux)\n");
    }

    GROUP("sessionweb:writes");
    {
        ServerRig rig;
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        fsWrite(rig.proj + "/notes.txt", "one foo\ntwo foo\n");
        fsWrite(rig.proj + "/taken.txt", "taken\n");
        Web::Server web;
        Web::Options o;
        o.hostSocket = rig.path();
        o.folder = rig.proj;
        o.pollMs = 60000;
        o.editable = true;
        std::string error;
        CHECK(web.start(o, &error));
        std::thread webThread([&] { web.run(); });
        Web::Server ro;
        Web::Options ro_o = o;
        ro_o.editable = false;
        CHECK(ro.start(ro_o, &error));
        std::thread roThread([&] { ro.run(); });
        if (web.port() > 0) {
            PageClient p;
            CHECK(p.open(web));
            PageEditor page;
            page.p = &p;
            Json hello, state, made, found;
            std::vector<Json> saved, replaced;
            auto take = [&](const Json& m) {
                const std::string t = m["t"].asString();
                if (t == "hello") hello = m;
                else if (t == "state") state = m;
                else if (t == "newfile") made = m;
                else if (t == "savedas") saved.push_back(m);
                else if (t == "replaced") replaced.push_back(m);
                else if (t == "found" && m["done"].asBool()) found = m;
                page.take(m);
            };
            auto docPath = [&](Shared::DocId id) {
                for (const Json& d : state["docs"].elements())
                    if ((Shared::DocId)d["id"].asInt() == id) return d["path"].asString();
                return std::string("?");
            };
            CHECK(p.pump(take, [&] { return state.isObject(); }));
            CHECK(hello["canNew"].asBool() && hello["canSaveAs"].asBool() && hello["canReplace"].asBool());

            // New File: the host's untitled document, which the page shows.
            p.sendText("{\"t\":\"newfile\"}");
            CHECK(p.pump(take, [&] { return made.isObject(); }, 4000));
            const Shared::DocId id = (Shared::DocId)made["doc"].asInt();
            CHECK(id > 0);
            CHECK(p.pump(take, [&] { return docPath(id).empty(); }, 4000));
            page.id = id;
            p.sendText("{\"t\":\"show\",\"doc\":" + std::to_string(id) + "}");
            CHECK(p.pump(take, [&] { return page.docs >= 1; }, 4000));
            page.edit(Ot::Ops{Ot::Op{0, 0, u"hello\n"}});
            CHECK(p.pump(take, [&] { return page.synced(); }, 4000));
            // Saved as a name of the folder's, and the tab is that file.
            p.sendText("{\"t\":\"saveas\",\"doc\":" + std::to_string(id) + ",\"path\":\"hello.txt\"}");
            CHECK(p.pump(take, [&] { return saved.size() == 1; }, 4000));
            CHECK(saved.size() == 1 && saved[0]["ok"].asBool() && saved[0]["path"].asString() == rig.proj + "/hello.txt");
            CHECK(readFile((rig.proj + "/hello.txt").c_str()) == "hello\n");
            CHECK(p.pump(take, [&] { return docPath(id) == rig.proj + "/hello.txt"; }, 4000));
            // A file there already: "exists", then written over when asked.
            p.sendText("{\"t\":\"saveas\",\"doc\":" + std::to_string(id) + ",\"path\":\"taken.txt\"}");
            CHECK(p.pump(take, [&] { return saved.size() == 2; }, 4000));
            CHECK(saved.size() == 2 && !saved[1]["ok"].asBool() && saved[1]["reason"].asString() == "exists");
            CHECK(readFile((rig.proj + "/taken.txt").c_str()) == "taken\n");
            p.sendText("{\"t\":\"saveas\",\"doc\":" + std::to_string(id) + ",\"path\":\"taken.txt\",\"overwrite\":true}");
            CHECK(p.pump(take, [&] { return saved.size() == 3; }, 4000));
            CHECK(saved.size() == 3 && saved[2]["ok"].asBool() && readFile((rig.proj + "/taken.txt").c_str()) == "hello\n");
            // Out of the folder: refused by its name.
            p.sendText("{\"t\":\"saveas\",\"doc\":" + std::to_string(id) + ",\"path\":\"../x.txt\"}");
            CHECK(p.pump(take, [&] { return saved.size() == 4; }, 4000));
            CHECK(saved.size() == 4 && saved[3]["reason"].asString() == "name");

            // Replace in Folder: the page searches, then replaces the rows
            // it showed but one.
            p.sendText("{\"t\":\"search\",\"seq\":1,\"query\":\"foo\"}");
            CHECK(p.pump(take, [&] { return found.isObject(); }, 4000));
            const std::string notes = "one foo\ntwo foo\n";
            Json f = Json::object();
            f.set("path", Json("notes.txt"));
            Json rows = Json::array();
            rows.push(replaceRowJson(notes, 1, 1, false));
            rows.push(replaceRowJson(notes, 2, 1, true));
            f.set("rows", rows);
            Json ask = replaceAsk("foo", "bar", Json::array());
            Json list = Json::array();
            list.push(f);
            ask.set("files", list);
            ask.set("t", Json("replace"));
            ask.set("seq", Json(7));
            p.sendText(ask.dump());
            CHECK(p.pump(take, [&] { return replaced.size() == 1; }, 4000));
            CHECK(replaced.size() == 1 && replaced[0]["seq"].asInt() == 7 && replaced[0]["replaced"].asInt() == 1 &&
                  replaced[0]["files"].asInt() == 1);
            CHECK(readFile((rig.proj + "/notes.txt").c_str()) == "one foo\ntwo bar\n");

            // A read-only page has none of these, and its asks do nothing.
            PageClient q;
            CHECK(q.open(ro));
            Json rhello, rstate, rreplaced;
            std::vector<Json> rnotes;
            auto rtake = [&](const Json& m) {
                const std::string t = m["t"].asString();
                if (t == "hello") rhello = m;
                if (t == "state") rstate = m;
                if (t == "replaced") rreplaced = m;
                if (t == "note") rnotes.push_back(m);
            };
            CHECK(q.pump(rtake, [&] { return rstate.isObject(); }));
            CHECK(!rhello["canNew"].asBool() && !rhello["canSaveAs"].asBool() && !rhello["canReplace"].asBool());
            q.sendText("{\"t\":\"newfile\"}");
            ask.set("seq", Json(8));
            q.sendText(ask.dump());
            CHECK(q.pump(rtake, [&] { return rreplaced.isObject() && !rnotes.empty(); }, 4000));
            CHECK(rreplaced["error"].asString() == "This page only reads: it cannot replace.");
            CHECK(readFile((rig.proj + "/notes.txt").c_str()) == "one foo\ntwo bar\n");
        }
        web.stop();
        ro.stop();
        webThread.join();
        roThread.join();
    }

    GROUP("sessionweb:writes-app");
    {
        // An app's page: its window saves as and makes new files
        // (Options::saveAs, newFile); the core holds the path first.
        const std::string root = Shared::resolveFolder(fsScratch());
        fsWrite(root + "/taken.txt", "taken\n");
        fsWrite(root + "/f.txt", "f\n");
        std::mutex routesMu;
        std::map<Shared::ClientId, Shared::Client*> routes;
        Shared::SessionHost host([&](Shared::ClientId to, Message&& m) {
            std::lock_guard<std::mutex> lk(routesMu);
            auto it = routes.find(to);
            if (it != routes.end()) it->second->deliver(std::move(m));
        });
        Shared::Client app(Model::Unit::Utf16, "app");
        const Shared::ClientId appId = host.connect();
        {
            std::lock_guard<std::mutex> lk(routesMu);
            routes[appId] = &app;
        }
        app.connect([&](Message&& m) { host.post(appId, std::move(m)); });
        app.open(1, root + "/f.txt", "f\n", nullptr);
        Json w = Json::object();
        w.set("root", Json(root));
        w.set("focused", Json(0));
        Json tab = Json::object();
        tab.set("doc", Json((int64_t)1));
        tab.set("path", Json(root + "/f.txt"));
        Json tabs = Json::array();
        tabs.push(std::move(tab));
        Json g = Json::object();
        g.set("tabs", std::move(tabs));
        g.set("active", Json((int64_t)1));
        Json groups = Json::array();
        groups.push(std::move(g));
        w.set("groups", std::move(groups));
        app.windowTabs(5, w);
        CHECK(app.settle());
        std::mutex mu;
        std::vector<std::pair<Shared::DocId, std::string>> asked;
        std::vector<std::uint64_t> asks;
        int news = 0;
        Web::Server web, plain;
        auto options = [&](bool withWrites) {
            Web::Options o;
            o.folder = root;
            o.window = 5;
            o.pollMs = 100;
            o.editable = true;
            o.connect = [&](Shared::Client& c) -> std::function<void()> {
                const Shared::ClientId id = host.connect();
                {
                    std::lock_guard<std::mutex> lk(routesMu);
                    routes[id] = &c;
                }
                c.connect([&host, id](Message&& m) { host.post(id, std::move(m)); });
                return [&, id] {
                    {
                        std::lock_guard<std::mutex> lk(routesMu);
                        routes.erase(id);
                    }
                    host.disconnect(id);
                };
            };
            o.save = [](Shared::DocId) { return true; };
            if (withWrites) {
                o.newFile = [&] {
                    std::lock_guard<std::mutex> lk(mu);
                    news++;
                    return true;
                };
                o.saveAs = [&](Shared::DocId d, const std::string& path, std::uint64_t ask) {
                    std::lock_guard<std::mutex> lk(mu);
                    asked.push_back({d, path});
                    asks.push_back(ask);
                };
            }
            return o;
        };
        std::string error;
        CHECK(web.start(options(true), &error));
        CHECK(plain.start(options(false), &error));
        std::thread t1([&] { web.run(); }), t2([&] { plain.run(); });
        {
            PageClient p;
            CHECK(p.open(web));
            Json hello, state;
            std::vector<Json> saved;
            auto take = [&](const Json& m) {
                const std::string t = m["t"].asString();
                if (t == "hello") hello = m;
                else if (t == "state") state = m;
                else if (t == "savedas") saved.push_back(m);
            };
            CHECK(p.pump(take, [&] { return state.isObject() && state["docs"].size() == 1; }));
            CHECK(hello["canNew"].asBool() && hello["canSaveAs"].asBool() && hello["canReplace"].asBool());
            p.sendText("{\"t\":\"show\",\"doc\":1}");
            p.sendText("{\"t\":\"newfile\"}");
            // A file there already: asked before the window hears of it.
            p.sendText("{\"t\":\"saveas\",\"doc\":1,\"path\":\"taken.txt\"}");
            CHECK(p.pump(take, [&] { return saved.size() == 1; }, 4000));
            CHECK(saved.size() == 1 && saved[0]["reason"].asString() == "exists");
            // The window is handed the path, and its answer reaches the page.
            p.sendText("{\"t\":\"saveas\",\"doc\":1,\"path\":\"g.txt\"}");
            CHECK(waitUntil([&] {
                std::lock_guard<std::mutex> lk(mu);
                return asked.size() == 1 && news == 1;
            }, 4000));
            std::uint64_t ask = 0;
            {
                std::lock_guard<std::mutex> lk(mu);
                CHECK(asked.size() == 1 && news == 1);
                if (!asked.empty()) CHECK(asked[0].first == 1 && asked[0].second == root + "/g.txt");
                if (!asks.empty()) ask = asks[0];
            }
            web.savedAs(ask, false, "g.txt could not be saved: no room.");
            CHECK(p.pump(take, [&] { return saved.size() == 2; }, 4000));
            CHECK(saved.size() == 2 && !saved[1]["ok"].asBool() &&
                  saved[1]["why"].asString() == "g.txt could not be saved: no room." &&
                  saved[1]["path"].asString() == root + "/g.txt");
            web.savedAs(ask, true, "");         // an answer told once is not told twice
            web.savedAs(ask + 100, true, "");   // nor one never asked
            // A path the page may not name never reaches the window.
            p.sendText("{\"t\":\"saveas\",\"doc\":1,\"path\":\"../g.txt\"}");
            CHECK(p.pump(take, [&] { return saved.size() == 3; }, 4000));
            CHECK(saved.size() == 3 && saved[2]["reason"].asString() == "name");
            {
                std::lock_guard<std::mutex> lk(mu);
                CHECK(asked.size() == 1);
            }
            // An app's page without them: no New File, no Save As.
            PageClient q;
            CHECK(q.open(plain));
            Json qhello, qstate;
            auto qtake = [&](const Json& m) {
                if (m["t"].asString() == "hello") qhello = m;
                if (m["t"].asString() == "state") qstate = m;
            };
            CHECK(q.pump(qtake, [&] { return qstate.isObject(); }));
            CHECK(!qhello["canNew"].asBool() && !qhello["canSaveAs"].asBool() && qhello["canReplace"].asBool());
        }
        web.stop();
        plain.stop();
        t1.join();
        t2.join();
    }
}
