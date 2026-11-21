// session_save_tests.inc — the `host:save*` groups of run_tests.cpp: the
// session's host saving a document itself (stage 7's first write,
// SessionFiles.h's saveDocument and SessionHost.h's DocSave). The rules as
// a function over real files (encodings, line endings, a file changed or
// gone on disk, the folder a browser's save is held to), then through a
// host with an editing renderer, a guest (a browser's page) that hears
// DocDirty, an app, and a reader, every message through the encoding.
// Included after session_host_tests.inc, whose Net, TestMirror, fsScratch
// and fsWrite it uses.

namespace {

// A file's text as the host's saved text would hold it (what an app read).
std::u16string savedOf(const std::string& bytes) {
    std::u16string t;
    FolderSearch::TextEncoding e;
    bool bom = false;
    Model::decodeFileText(bytes, &t, &e, &bom);
    return t;
}

// The ask a document read from `path` (and never written by the host) makes.
Shared::SaveAsk askFor(const std::string& path, const std::u16string& text, const std::u16string& saved) {
    Shared::SaveAsk a;
    a.path = path;
    a.text = text;
    a.saved = saved;
    return a;
}

}  // namespace

static void testSessionSave() {
    namespace fs = std::filesystem;
    using Shared::SaveAsk;
    using Shared::SaveDone;
    using Shared::saveDocument;
    using TE = FolderSearch::TextEncoding;

    // The rules, file by file: the bytes on disk are the apps' (the
    // encoding and its mark kept, line endings the text's own), a file
    // changed or gone since the saved text is never written over unless
    // asked, and a browser's save stays in its folder.
    GROUP("host:save-rules");
    {
        const std::string root = fsScratch(), elsewhere = fsScratch();
        CHECK(!root.empty() && !elsewhere.empty());

        // UTF-8 with CRLF: the text's own line endings, byte for byte.
        const std::string crlf = root + "/crlf.txt";
        fsWrite(crlf, "one\r\ntwo\r\n");
        SaveDone d = saveDocument(askFor(crlf, u"one\r\ntwo\r\nthree\r\n", savedOf("one\r\ntwo\r\n")));
        CHECK(d.ok && d.reason.empty() && d.note.empty() && d.encoding == TE::Utf8 && !d.bom && !d.created);
        CHECK(readFile(crlf.c_str()) == "one\r\ntwo\r\nthree\r\n");
        CHECK(d.written.exists && d.written == FolderSearch::stampOf(crlf));
        if (!d.ok) std::printf("  host:save-rules: %s\n", d.why.c_str());

        // Windows-1252 (Latin-1's bytes): written back in it, é as one byte.
        const std::string latin = root + "/latin.txt";
        fsWrite(latin, "caf\xE9\r\n");
        const std::u16string latinText = savedOf("caf\xE9\r\n");
        CHECK(latinText == u"café\r\n");
        d = saveDocument(askFor(latin, latinText + u"naïve\n", latinText));
        CHECK(d.ok && d.encoding == TE::Windows1252 && d.note.empty());
        CHECK(readFile(latin.c_str()) == "caf\xE9\r\nna\xEFve\n");
        // A character Windows-1252 lacks: UTF-8, said so, never a '?'.
        const std::u16string latin2 = latinText + u"naïve\n";
        d = saveDocument(askFor(latin, latin2 + u"→\n", latin2));
        CHECK(d.ok && d.encoding == TE::Utf8 &&
              d.note == "Saved latin.txt as UTF-8: Windows-1252 cannot store “→”");
        CHECK(readFile(latin.c_str()) == "caf\xC3\xA9\r\nna\xC3\xAFve\n\xE2\x86\x92\n");

        // UTF-16 with its mark, unedited: the same bytes; UTF-8's mark kept.
        const std::string u16 = root + "/u16.txt";
        const std::string u16bytes("\xFF\xFEh\0i\0\r\0\n\0", 10);
        fsWrite(u16, u16bytes);
        d = saveDocument(askFor(u16, u"hi\r\n", savedOf(u16bytes)));
        CHECK(d.ok && d.encoding == TE::Utf16LE && d.bom && readFile(u16.c_str()) == u16bytes);
        const std::string bom8 = root + "/bom8.txt";
        fsWrite(bom8, "\xEF\xBB\xBFx\n");
        d = saveDocument(askFor(bom8, u"xy\n", u"x\n"));
        CHECK(d.ok && d.bom && readFile(bom8.c_str()) == "\xEF\xBB\xBFxy\n");
        // UTF-8 with a stray byte: kept as it was, edited or not.
        const std::string raw = root + "/raw.txt";
        const std::string rawBytes = "na\xC3\xAFve caf\xC3\xA9 \xFF end\n";
        CHECK(FolderSearch::readsAsUtf8WithStrayBytes(rawBytes));
        fsWrite(raw, rawBytes);
        const std::u16string rawText = savedOf(rawBytes);
        d = saveDocument(askFor(raw, rawText, rawText));
        CHECK(d.ok && d.encoding == TE::Utf8Raw && readFile(raw.c_str()) == rawBytes);
        d = saveDocument(askFor(raw, u">" + rawText, rawText));
        CHECK(d.ok && readFile(raw.c_str()) == ">" + rawBytes);

        // Changed on disk since the saved text: refused, the file as it is;
        // a person's overwrite writes it.
        const std::string ch = root + "/changed.txt";
        fsWrite(ch, "theirs\n");
        d = saveDocument(askFor(ch, u"mine\n", u"before\n"));
        CHECK(!d.ok && d.reason == "changed" && d.why.find("changed.txt changed on disk") == 0);
        CHECK(readFile(ch.c_str()) == "theirs\n");
        SaveAsk over = askFor(ch, u"mine\n", u"before\n");
        over.overwrite = true;
        d = saveDocument(over);
        CHECK(d.ok && readFile(ch.c_str()) == "mine\n");
        // Written over after the host wrote it: in the encoding it wrote,
        // not the agent's file's (as Keep Mine writes the tab's).
        const std::string w1252 = root + "/w1252.txt";
        fsWrite(w1252, "\xE9t\xE9\n");
        d = saveDocument(askFor(w1252, u"été!\n", u"été\n"));
        CHECK(d.ok && d.encoding == TE::Windows1252);
        SaveAsk keep = askFor(w1252, u"été!!\n", u"été!\n");
        keep.stamp = d.written;
        keep.encoding = d.encoding;
        fsWrite(w1252, "an agent's edit, longer\n");
        CHECK(saveDocument(keep).reason == "changed");
        keep.overwrite = true;
        d = saveDocument(keep);
        CHECK(d.ok && d.encoding == TE::Windows1252 && readFile(w1252.c_str()) == "\xE9t\xE9!!\n");
        // Deleted since: refused, nothing made; overwrite makes it again.
        const std::string gone = root + "/gone.txt";
        d = saveDocument(askFor(gone, u"mine\n", u"was\n"));
        CHECK(!d.ok && d.reason == "gone" && !fs::exists(gone));
        SaveAsk again = askFor(gone, u"mine\n", u"was\n");
        again.overwrite = true;
        d = saveDocument(again);
        CHECK(d.ok && d.created && readFile(gone.c_str()) == "mine\n");
        // A file never read and never made (no saved text): made.
        d = saveDocument(askFor(root + "/new.txt", u"fresh\n", u""));
        CHECK(d.ok && d.created && readFile((root + "/new.txt").c_str()) == "fresh\n");

        // The stamp the host wrote with: the next save knows the file
        // unchanged without comparing; a writer since is a change.
        SaveAsk known = askFor(crlf, u"1\r\n", u"one\r\ntwo\r\nthree\r\n");
        d = saveDocument(known);
        CHECK(d.ok);
        known.stamp = d.written;
        known.encoding = d.encoding;
        known.saved = u"1\r\n";
        known.text = u"12\r\n";
        d = saveDocument(known);
        CHECK(d.ok && readFile(crlf.c_str()) == "12\r\n");
        known.stamp = d.written;
        known.saved = u"12\r\n";
        fsWrite(crlf, "an agent's\r\n");
        known.text = u"123\r\n";
        d = saveDocument(known);
        CHECK(!d.ok && d.reason == "changed" && readFile(crlf.c_str()) == "an agent's\r\n");

        // Held to a folder (a browser's save): inside it, a subfolder too;
        // never a path outside, a link out, a link to nothing, or a folder.
        ::mkdir((root + "/sub").c_str(), 0755);
        fsWrite(root + "/sub/in.txt", "a\n");
        fsWrite(elsewhere + "/secret.txt", "not yours\n");
        CHECK(::symlink((elsewhere + "/secret.txt").c_str(), (root + "/out-link").c_str()) == 0);
        CHECK(::symlink((root + "/sub/in.txt").c_str(), (root + "/in-link").c_str()) == 0);
        CHECK(::symlink((root + "/nothing").c_str(), (root + "/dangling").c_str()) == 0);
        CHECK(::symlink(elsewhere.c_str(), (root + "/out-dir").c_str()) == 0);
        auto held = [&](const std::string& path, const std::u16string& saved) {
            SaveAsk a = askFor(path, u"written\n", saved);
            a.root = root;
            return saveDocument(a);
        };
        d = held(root + "/sub/in.txt", u"a\n");
        CHECK(d.ok && readFile((root + "/sub/in.txt").c_str()) == "written\n");
        // A link inside to a file inside: written through, still a link.
        d = held(root + "/in-link", u"written\n");
        struct stat st;
        CHECK(d.ok && ::lstat((root + "/in-link").c_str(), &st) == 0 && S_ISLNK(st.st_mode));
        for (const std::string& bad : {elsewhere + "/secret.txt", root + "/out-link", root + "/out-dir/secret.txt",
                                       root + "/missing-folder/x.txt", root + "/../x.txt"}) {
            d = held(bad, u"not yours\n");
            CHECK(!d.ok && d.reason == "outside");
            if (d.ok || d.reason != "outside") std::printf("  host:save-rules: %s -> %s\n", bad.c_str(), d.reason.c_str());
        }
        CHECK(readFile((elsewhere + "/secret.txt").c_str()) == "not yours\n");
        d = held(root + "/dangling", u"");
        CHECK(!d.ok && d.reason == "notfile" && !fs::exists(root + "/nothing"));
        d = held(root + "/sub", u"");
        CHECK(!d.ok && d.reason == "notfile");
        // No path yet, and a text UTF-8 cannot carry.
        d = saveDocument(askFor("", u"x", u""));
        CHECK(!d.ok && d.reason == "nofile");
        d = saveDocument(askFor(root + "/lone.txt", std::u16string(1, char16_t(0xD800)), u""));
        CHECK(!d.ok && d.reason == "encoding" && !fs::exists(root + "/lone.txt"));
        // The answer's shape.
        const Json j = Shared::saveDoneJson(root + "/sub/in.txt", held(root + "/sub/in.txt", u"written\n"));
        CHECK(j["ok"].asBool() && j["encoding"].asString() == "UTF-8" && !j.has("why") &&
              j["path"].asString() == root + "/sub/in.txt");
        const Json r = Shared::saveDoneJson("/x", saveDocument(askFor(ch, u"z\n", u"q\n")));
        CHECK(!r["ok"].asBool() && r["reason"].asString() == "changed" && r.has("why"));

        std::error_code ec;
        for (const std::string& dir : {root, elsewhere}) fs::remove_all(dir, ec);
    }

    // Through the host (every message through the encoding): an editor, a
    // guest that hears DocDirty, an app, and a reader on one folder's host.
    GROUP("host:save");
    {
        const std::string root = fsScratch(), elsewhere = fsScratch();
        Net net(true, true);
        net.host.setFolder(root);
        const std::string path = root + "/f.py";
        fsWrite(path, "def f():\r\n    pass\r\n");
        Net::Link& ed = net.add(Unit::Utf16, "editor");
        Net::Link& guest = net.add(Unit::Utf16, "browser", 2 | 8);
        Net::Link& reader = net.add(Unit::Utf16, "reader", 1 | 8);
        for (Net::Link* l : {&ed, &guest, &reader}) l->client->dispatch();   // the Welcomes
        CHECK(ed.client->hostSaves() && guest.client->hostSaves());
        TestMirror em;
        em.client = ed.client.get();
        em.doc = 1;
        em.text = u"def f():\r\n    pass\r\n";
        ed.client->open(1, path, Model::utf8FromUtf16(em.text), &em);
        TestMirror gm;
        gm.client = guest.client.get();
        gm.doc = 1;
        guest.client->view(1, &gm);
        guest.client->dispatch();
        CHECK(gm.text == em.text);
        auto save = [&](Net::Link& l, Shared::DocId doc, bool overwrite) {
            Shared::Client::SaveAnswer a;
            const std::uint64_t req = l.client->requestSave(doc, 0, overwrite);
            l.client->dispatch();
            if (!req || !l.client->saveAnswer(req, &a)) a.json = Json();
            return a;
        };
        // Typing at the end, every edit acknowledged before the next.
        auto typed = [&](TestMirror& m, const std::u16string& s) {
            m.caret = m.length();
            m.type(s);
            m.client->settle();
        };

        // Typing makes it unsaved, and those that hear it are told once.
        em.caret = em.length();
        typed(em, u"# end\r\n");
        typed(em, u"#");
        guest.client->dispatch();
        reader.client->dispatch();
        CHECK(net.host.dirty(1));
        CHECK(guest.dirties.size() == 1 && guest.dirties[0] == std::make_pair(Shared::DocId(1), true));
        CHECK(reader.dirties.size() == 1);
        CHECK(ed.dirties.empty());   // it did not ask to hear

        // The guest's save: the host's text, in the file's line endings,
        // clean once written, and everyone who hears told so.
        Shared::Client::SaveAnswer a = save(guest, 1, false);
        CHECK(a.ok && a.json["ok"].asBool() && a.version == net.host.version(1));
        if (!a.ok) std::printf("  host:save: %s\n", a.json.dump().c_str());
        CHECK(readFile(path.c_str()) == "def f():\r\n    pass\r\n# end\r\n#");
        CHECK(!net.host.dirty(1));
        reader.client->dispatch();
        CHECK(guest.dirties.size() == 2 && !guest.dirties[1].second && reader.dirties.size() == 2);
        CHECK(!ed.client->sessionState()["documents"][0]["dirty"].asBool());
        // Again, by the stamp it wrote: no question.
        typed(em, u"!");
        a = save(guest, 1, false);
        CHECK(a.ok && readFile(path.c_str()) == "def f():\r\n    pass\r\n# end\r\n#!");

        // Changed on disk since: refused, the file left as the agent wrote
        // it and the document still unsaved; overwrite is the person's.
        fsWrite(path, "an agent's edit\n");
        typed(em, u"?");
        a = save(guest, 1, false);
        CHECK(!a.ok && a.json["reason"].asString() == "changed" && net.host.dirty(1));
        CHECK(a.json["why"].asString() == "f.py changed on disk since it was read: saving would write over that change.");
        CHECK(readFile(path.c_str()) == "an agent's edit\n");
        a = save(guest, 1, true);
        CHECK(a.ok && readFile(path.c_str()) == "def f():\r\n    pass\r\n# end\r\n#!?" && !net.host.dirty(1));

        // An app's window with it open saves it, not the page.
        Net::Link& app = net.add(Unit::Utf16, "app", 4);
        TestMirror am;
        am.client = app.client.get();
        am.doc = 1;
        app.client->open(1, path, "", &am);
        app.client->dispatch();
        typed(em, u"+");
        a = save(guest, 1, false);
        CHECK(!a.ok && a.json["reason"].asString() == "app" &&
              a.json["why"].asString() == "f.py is open in a mutible window: save it there.");
        CHECK(readFile(path.c_str()).back() == '?');
        // The app saves (its own write) and says so: clean, and the host's
        // next save compares the file with that text, finding it unchanged.
        fsWrite(path, Model::utf8FromUtf16(net.host.text(1)));
        am.client->dispatch();
        app.client->saved(1);
        guest.client->dispatch();
        CHECK(!net.host.dirty(1) && !guest.dirties.back().second);
        app.client->detach(1);
        typed(em, u"=");
        a = save(guest, 1, false);
        CHECK(a.ok && readFile(path.c_str()) == Model::utf8FromUtf16(net.host.text(1)));

        // Held to the folder: a document outside it is the editor's to
        // save (an app's renderer is not held), never the guest's.
        const std::string out = elsewhere + "/s.txt";
        fsWrite(out, "s\n");
        TestMirror om;
        om.client = ed.client.get();
        om.doc = 2;
        om.text = u"s\n";
        ed.client->open(2, out, "s\n", &om);
        TestMirror gm2;
        gm2.client = guest.client.get();
        gm2.doc = 2;
        guest.client->view(2, &gm2);
        guest.client->dispatch();
        typed(om, u"t");
        a = save(guest, 2, false);
        CHECK(!a.ok && a.json["reason"].asString() == "outside" && readFile(out.c_str()) == "s\n");
        a = save(ed, 2, false);
        CHECK(a.ok && readFile(out.c_str()) == "s\nt");
        // A document with no file yet; one the host does not have.
        TestMirror um;
        um.client = ed.client.get();
        um.doc = 3;
        ed.client->open(3, "", "draft", &um);
        a = save(ed, 3, false);
        CHECK(!a.ok && a.json["reason"].asString() == "nofile");

        // A reader may not save: the host refuses it, and writes nothing.
        TestMirror rm;
        rm.client = reader.client.get();
        rm.doc = 1;
        reader.client->view(1, &rm);
        reader.client->dispatch();
        typed(em, u"~");
        const std::string before = readFile(path.c_str());
        a = save(reader, 1, false);
        CHECK(!a.ok && a.json.isNull() && readFile(path.c_str()) == before);
        bool refused = false;
        for (const std::string& e : reader.client->takeErrors()) refused = refused || e.find("doc.save") != std::string::npos;
        CHECK(refused);

        // A host from before saving says nothing of it in its Welcome: no
        // DocSave goes to it (it would drop the connection at the line).
        Shared::Client old(Unit::Utf16, "new renderer");
        std::vector<Message> sent;
        old.connect([&](Message&& m) { sent.push_back(std::move(m)); });
        Message w;
        w.kind = Shared::Kind::Welcome;
        w.client = 9;
        w.v = Shared::kIdRange;
        old.deliver(Shared::roundTrip(w));
        old.dispatch();
        TestMirror xm;
        old.open(1, path, "x", &xm);
        CHECK(!old.hostSaves() && old.requestSave(1, 0, false) == 0);
        for (const Message& m : sent) CHECK(m.kind != Shared::Kind::DocSave);
        // The messages on the wire.
        Message s;
        s.kind = Shared::Kind::DocSave;
        s.doc = 4;
        s.req = 7;
        s.window = 5;
        s.flag = 1;
        Message back = Shared::roundTrip(s);
        CHECK(back.kind == Shared::Kind::DocSave && back.doc == 4 && back.req == 7 && back.window == 5 && back.flag == 1);
        CHECK(std::string(Shared::kindName(Shared::Kind::DocSaveDone)) == "doc.save-done" &&
              std::string(Shared::kindName(Shared::Kind::DocDirty)) == "doc.dirty");

        std::error_code ec;
        for (const std::string& dir : {root, elsewhere}) fs::remove_all(dir, ec);
    }
    // An app that saves through the host (the Mac's tabs, Hello flag 16):
    // its tab's encoding kept whatever the file holds, the stamp written in
    // the answer, the others told the host wrote (DocDirty flag 2), and a
    // page's save of what its window shows allowed, which the app follows.
    // An app that saves by itself (GTK's, no flag 16) still holds the page
    // off.
    GROUP("host:save-for-apps");
    {
        const std::string root = fsScratch();
        Net net(true, true);
        net.host.setFolder(root);
        Net::Link& app = net.add(Unit::Utf16, "mac", 4 | 8 | 16);
        Net::Link& guest = net.add(Unit::Utf16, "browser", 2 | 8);
        for (Net::Link* l : {&app, &guest}) l->client->dispatch();
        CHECK(app.client->hostSaves() && app.client->hostSavesForApps());
        auto save = [&](Net::Link& l, Shared::DocId doc, bool overwrite, const std::string& enc) {
            Shared::Client::SaveAnswer a;
            const std::uint64_t req = l.client->requestSave(doc, 0, overwrite, enc);
            l.client->dispatch();
            if (!req || !l.client->saveAnswer(req, &a)) a.json = Json();
            return a;
        };
        auto typed = [&](TestMirror& m, const std::u16string& s) {
            m.caret = m.length();
            m.type(s);
            m.client->settle();
        };

        // A UTF-16 file the app read; an agent rewrites it as UTF-8, and the
        // app (which asked its Keep Mine) saves in its tab's UTF-16.
        const std::string u16 = root + "/u16.txt";
        fsWrite(u16, std::string("\xFF\xFEo\0n\0e\0\n\0", 10));
        TestMirror am;
        am.client = app.client.get();
        am.doc = 1;
        am.text = u"one\n";
        app.client->open(1, u16, "one\n", &am);
        app.client->dispatch();
        TestMirror gm;
        gm.client = guest.client.get();
        gm.doc = 1;
        guest.client->view(1, &gm);
        guest.client->dispatch();
        typed(am, u"two\n");
        fsWrite(u16, "agent\n");
        Shared::Client::SaveAnswer a = save(app, 1, true, "utf16le");
        CHECK(a.ok && a.version == net.host.version(1));
        if (!a.ok) std::printf("  host:save-for-apps: %s\n", a.json.dump().c_str());
        CHECK(readFile(u16.c_str()) == std::string("\xFF\xFEo\0n\0e\0\n\0t\0w\0o\0\n\0", 18));
        CHECK(a.json["encodingName"].asString() == "utf16le");
        CHECK(Shared::stampFromJson(a.json["stamp"]) == FolderSearch::stampOf(u16));
        CHECK(!net.host.dirty(1));
        // The app, which asked, hears the flip but no write; the guest hears
        // the write, with the version written and the same stamp.
        guest.client->dispatch();
        CHECK(app.writes.empty() && !app.dirties.empty() && !app.dirties.back().second);
        CHECK(guest.writes.size() == 1 && guest.writes[0].doc == 1 && guest.writes[0].version == a.version);
        CHECK(guest.writes[0].json["path"].asString() == u16 &&
              guest.writes[0].json["encodingName"].asString() == "utf16le");
        CHECK(Shared::stampFromJson(guest.writes[0].json["stamp"]) == FolderSearch::stampOf(u16));

        // A page's save while the app's window has it open: the host writes,
        // in the encoding last written, and the app is told.
        typed(gm, u"three\n");
        app.client->dispatch();
        CHECK(am.text == u"one\ntwo\nthree\n" && net.host.dirty(1));
        a = save(guest, 1, false, "");
        CHECK(a.ok && readFile(u16.c_str()) ==
                          std::string("\xFF\xFEo\0n\0e\0\n\0t\0w\0o\0\n\0t\0h\0r\0e\0e\0\n\0", 30));
        if (!a.ok) std::printf("  host:save-for-apps (page): %s\n", a.json.dump().c_str());
        app.client->dispatch();
        CHECK(app.writes.size() == 1 && app.writes[0].doc == 1 && app.writes[0].version == a.version &&
              app.writes[0].version == app.client->version(1));
        CHECK(Shared::stampFromJson(app.writes[0].json["stamp"]) == FolderSearch::stampOf(u16));
        CHECK(!app.dirties.back().second && !net.host.dirty(1));
        // Saved again with nothing changed: the others still hear it (the
        // stamp moved), the asker does not.
        const std::size_t heard = app.writes.size(), dirtiesBefore = guest.dirties.size();
        a = save(guest, 1, false, "");
        app.client->dispatch();
        CHECK(a.ok && app.writes.size() == heard + 1 && guest.dirties.size() == dirtiesBefore);

        // An app that saves by itself (GTK's) holds the page off again.
        Net::Link& gtk = net.add(Unit::Utf16, "gtk", 4);
        gtk.client->dispatch();
        TestMirror km;
        km.client = gtk.client.get();
        km.doc = 1;
        gtk.client->open(1, u16, "", &km);
        gtk.client->dispatch();
        typed(gm, u"!");
        a = save(guest, 1, false, "");
        CHECK(!a.ok && a.json["reason"].asString() == "app");
        gtk.client->detach(1);
        a = save(guest, 1, false, "");
        CHECK(a.ok);

        // Windows-1252 with a character it lacks: UTF-8, said so, as the
        // Mac's own save said it.
        const std::string latin = root + "/w.txt";
        fsWrite(latin, "caf\xE9\n");
        TestMirror lm;
        lm.client = app.client.get();
        lm.doc = 2;
        lm.text = u"café\n";
        app.client->open(2, latin, Model::utf8FromUtf16(lm.text), &lm);
        app.client->dispatch();
        typed(lm, u"→");
        a = save(app, 2, true, "windows1252");
        CHECK(a.ok && a.json["encodingName"].asString() == "utf8");
        CHECK(a.json["note"].asString() == "Saved w.txt as UTF-8: Windows-1252 cannot store \xE2\x80\x9C\xE2\x86\x92\xE2\x80\x9D");
        CHECK(readFile(latin.c_str()) == "caf\xC3\xA9\n\xE2\x86\x92");
        // Unedited Windows-1252, kept so even though the host never read it.
        const std::string plain = root + "/p.txt";
        fsWrite(plain, "na\xEFve\n");
        TestMirror pm;
        pm.client = app.client.get();
        pm.doc = 3;
        pm.text = u"naïve\n";
        app.client->open(3, plain, Model::utf8FromUtf16(pm.text), &pm);
        typed(pm, u"x");
        a = save(app, 3, true, "windows1252");
        CHECK(a.ok && readFile(plain.c_str()) == "na\xEFve\nx" && a.json["encodingName"].asString() == "windows1252");
        // A file deleted since: the app asked, so it is made again.
        fsWrite(root + "/gone.txt", "g\n");
        TestMirror dm;
        dm.client = app.client.get();
        dm.doc = 4;
        dm.text = u"g\n";
        app.client->open(4, root + "/gone.txt", "g\n", &dm);
        std::error_code rec;
        fs::remove(root + "/gone.txt", rec);
        typed(dm, u"h");
        a = save(app, 4, true, "utf8");
        CHECK(a.ok && a.json["created"].asBool() && readFile((root + "/gone.txt").c_str()) == "g\nh");

        // The window keeps its buffer over a file changed on disk (Keep
        // Mine): the host counts it unsaved against the file's new text, and
        // clean again once the buffer is that text.
        const std::size_t told = app.dirties.size();
        CHECK(!net.host.dirty(4) && app.client->savedAs(4, u"g\nchanged\n"));
        app.client->settle();
        CHECK(net.host.dirty(4) && app.dirties.size() == told + 1 && app.dirties.back().second);
        CHECK(app.client->savedAs(4, u"g\nh"));
        app.client->settle();
        CHECK(!net.host.dirty(4) && !app.dirties.back().second);
        Message sv;
        sv.kind = Shared::Kind::DocSaved;
        sv.flag = 1;
        sv.text16 = u"x\u00e9";
        Message svBack = Shared::roundTrip(sv);
        CHECK(svBack.flag == 1 && svBack.text16 == u"x\u00e9");

        // The stamp as JSON, exactly, through the wire too.
        FolderSearch::FileStamp st;
        st.exists = st.regular = true;
        st.mtimeSec = 1763700000;
        st.mtimeNsec = 987654321;
        st.device = 0x8000000000000005ull;
        st.inode = 0x1000000000000007ull;
        st.size = 123456789012ull;
        Message wm;
        wm.kind = Shared::Kind::DocDirty;
        wm.doc = 1;
        wm.flag = 2;
        wm.json = Json::object();
        wm.json.set("stamp", Shared::stampJson(st));
        Message back = Shared::roundTrip(wm);
        CHECK(Shared::stampFromJson(back.json["stamp"]) == st && back.flag == 2);
        CHECK(!Shared::stampFromJson(Json()).exists);
        // A DocSave's encoding on the wire.
        Message sm;
        sm.kind = Shared::Kind::DocSave;
        sm.json = Json::object();
        sm.json.set("encoding", Json("utf16be"));
        CHECK(Shared::roundTrip(sm).json["encoding"].asString() == "utf16be");

        // A host that saves only for guests (Welcome flag 1): an app gets
        // no encoding through, and knows not to save through it.
        Shared::Client old(Unit::Utf16, "mac");
        old.setKeepsBackups(true);
        old.setSavesThroughHost(true);
        std::vector<Message> sent;
        old.connect([&](Message&& m) { sent.push_back(std::move(m)); });
        CHECK(!sent.empty() && sent[0].kind == Shared::Kind::Hello && sent[0].flag == (4 | 16));
        Message w;
        w.kind = Shared::Kind::Welcome;
        w.client = 9;
        w.v = Shared::kIdRange;
        w.flag = 1;
        old.deliver(Shared::roundTrip(w));
        old.dispatch();
        CHECK(old.hostSaves() && !old.hostSavesForApps());
        TestMirror xm;
        old.open(1, u16, "x", &xm);
        CHECK(old.requestSave(1, 0, true, "utf16le") != 0);
        CHECK(sent.back().kind == Shared::Kind::DocSave && sent.back().json.isNull());
        const std::size_t sentBefore = sent.size();
        CHECK(!old.savedAs(1, u"y") && sent.size() == sentBefore);

        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // A reload's edit and its DocSaved, with the host's messages taken in
    // the order that let the edit's "unsaved" come after the renderer
    // counted the document clean: the ack comes before the DocDirty the
    // edit caused, and lets the DocSaved out (the save suite's reload in
    // CI's session-host pass, 2026-11-21). The listener is never told
    // "unsaved" for it, and hears the host's state once the DocSaved is
    // taken.
    GROUP("host:dirty-fence");
    {
        Net net(true, false);
        Net::Link& app = net.add(Unit::Utf16, "mac", 4 | 8 | 16);
        auto up = [&] {   // the host takes everything the app sent
            while (!app.up.empty()) {
                Message m = std::move(app.up.front());
                app.up.pop_front();
                net.host.handle(app.id, std::move(m));
            }
        };
        auto down = [&] {   // the app takes everything the host sent, one dispatch
            while (!app.down.empty()) {
                app.client->deliver(std::move(app.down.front()));
                app.down.pop_front();
            }
            app.client->dispatch();
        };
        up();
        down();
        TestMirror& m = net.open(app, 1, u"one\n");
        up();
        down();
        CHECK(!net.host.dirty(1) && app.dirties.empty());

        // The reload: an edit, then saved().
        m.type(u"x");
        app.client->saved(1);
        CHECK(app.client->dirtyFenced(1));
        up();     // the edit only: the DocSaved waits behind it
        CHECK(net.host.dirty(1));
        down();   // the ack (the DocSaved goes now) and the edit's DocDirty
        CHECK(app.dirties.empty());
        if (!app.dirties.empty()) std::printf("  host:dirty-fence: told %d first\n", (int)app.dirties[0].second);
        up();     // the DocSaved and the Ping behind it
        CHECK(!net.host.dirty(1));
        down();
        CHECK(app.dirties.size() == 1 && !app.dirties.back().second);
        CHECK(!app.client->dirtyFenced(1));

        // Unfenced again: typing is told at once.
        m.type(u"y");
        up();
        down();
        CHECK(app.dirties.size() == 2 && app.dirties.back().second);

        // A DocSaved that flips nothing (the buffer kept over a changed
        // file, Keep Mine): the fence ends with nothing to tell.
        CHECK(app.client->savedAs(1, u"other\n"));
        up();
        down();
        CHECK(app.dirties.size() == 2 && !app.client->dirtyFenced(1));

        // Two saves out at once: held until the last is taken.
        app.client->saved(1);
        app.client->savedAs(1, u"other\n");
        up();
        CHECK(app.down.size() == 4);   // clean, Pong, unsaved, Pong
        app.client->deliver(std::move(app.down.front()));
        app.down.pop_front();
        app.client->deliver(std::move(app.down.front()));
        app.down.pop_front();
        app.client->dispatch();
        CHECK(app.dirties.size() == 2 && app.client->dirtyFenced(1));
        down();
        CHECK(app.dirties.size() == 3 && app.dirties.back().second && !app.client->dirtyFenced(1));

        // Another document's edit in flight leaves this one in step (the
        // Mac's tab takes the host's count by unconfirmed(doc)).
        TestMirror& other = net.open(app, 3, u"three\n");
        up();
        down();
        other.type(u"z");
        CHECK(app.client->unconfirmed(3) == 1 && app.client->unconfirmed(1) == 0);
        CHECK(app.client->unconfirmed() == 1);
        up();
        down();
        CHECK(app.client->unconfirmed(3) == 0 && app.client->unconfirmed() == 0);

        // A renderer that does not hear dirty states sends no Ping.
        Net::Link& plain = net.add(Unit::Utf16, "gtk", 4);
        net.open(plain, 2, u"two\n");
        plain.up.clear();
        plain.client->saved(2);
        CHECK(!plain.client->dirtyFenced(2));
        CHECK(plain.up.size() == 1 && plain.up.back().kind == Shared::Kind::DocSaved);
    }
}
