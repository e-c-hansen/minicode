// session_disk_tests.inc — the `diskwatch:*` groups of run_tests.cpp: the
// session's host watching the disk (stage 7, DiskWatch.h and SessionHost.h's
// "The disk"). The pure parts first (paths, the coalescer, the filter, the
// rules a document follows its file by), then a Host with renderers through
// the encoding (the trees' listings, Source Control, documents following,
// marked changed or gone, Reload, the protocol), the system's own watcher
// on a scratch folder (inotify on Linux, FSEvents on the Mac), a headless
// host over its socket that writes files and sees the notices, and a
// browser's page of it. Included after session_web_tests.inc, whose
// PageClient and PageEditor it uses, and session_server_tests.inc's
// ServerRig and waitUntil, and session_host_tests.inc's Net, TestMirror,
// fsScratch and fsWrite.

#include "DiskWatch.h"

namespace {

// A listing's names in FsChanged (or "" when it holds none for `path`).
std::string changedNames(const Json& changed, const std::string& path) {
    for (const Json& l : changed["listings"].elements())
        if (l["path"].asString() == path) return fsNames(l);
    return "";
}

bool hasListing(const Json& changed, const std::string& path) {
    for (const Json& l : changed["listings"].elements())
        if (l["path"].asString() == path) return true;
    return false;
}

// A scratch folder by its real place (/private/tmp on the Mac), as a
// headless host names its folder.
std::string realScratch() { return Shared::resolveFolder(fsScratch()); }

}  // namespace

static void testSessionDisk() {
    namespace fs = std::filesystem;
    using Model::Unit;

    GROUP("diskwatch:paths");
    {
        CHECK(DiskWatch::under("/a/b", "/a") && DiskWatch::under("/a", "/a") && !DiskWatch::under("/ab", "/a"));
        CHECK(DiskWatch::under("/a/b", "/") && !DiskWatch::under("/a", "/a/b"));
        std::string rel = "x";
        CHECK(DiskWatch::relativeTo("/r", "/r", &rel) && rel.empty());
        CHECK(DiskWatch::relativeTo("/r", "/r/s/t", &rel) && rel == "s/t");
        CHECK(!DiskWatch::relativeTo("/r", "/rs/t", &rel));
        CHECK(DiskWatch::cleanRelative("") && DiskWatch::cleanRelative("a/b.c") && DiskWatch::cleanRelative(".git"));
        for (const char* bad : {"/a", "a//b", "a/", "./a", "a/../b", "..", "a/."})
            CHECK(!DiskWatch::cleanRelative(bad));
        CHECK(!DiskWatch::cleanRelative(std::string("a\0b", 3)));
        CHECK(!DiskWatch::cleanRelative(std::string(5000, 'a')));
    }

    // Bursts: due once nothing came for the quiet time, or the most after
    // the first, whichever is first; each folder once, none under one
    // changed below; past the cap, each root below.
    GROUP("diskwatch:coalesce");
    {
        DiskWatch::Coalescer c(0.15, 1.0, 5);
        CHECK(c.wait(0) == -1 && !c.due(10));
        c.add({"/r/a", false}, 10.0);
        CHECK(std::fabs(c.wait(10.0) - 0.15) < 1e-9 && !c.due(10.1) && c.due(10.15));
        // Changes keep coming: the quiet time moves, the most does not.
        for (double t = 10.1; t < 11.2; t += 0.1) c.add({"/r/a", false}, t);
        CHECK(c.due(11.0) && c.waiting() == 1);
        c.add({"/r/b/x", false}, 11.0);
        c.add({"/r/b", true}, 11.0);
        c.add({"/r/b/y", false}, 11.0);
        c.add({"/r/bb", false}, 11.0);
        c.add({"/r/a/", false}, 11.0);   // a trailing slash is the same folder
        std::vector<DiskWatch::Change> got = c.take();
        CHECK(got.size() == 3);
        if (got.size() == 3)
            CHECK(got[0] == (DiskWatch::Change{"/r/a", false}) && got[1] == (DiskWatch::Change{"/r/b", true}) &&
                  got[2] == (DiskWatch::Change{"/r/bb", false}));
        CHECK(c.waiting() == 0 && c.wait(12) == -1);
        // A folder changed then changed below is below.
        c.add({"/r/c", false}, 20);
        c.add({"/r/c", true}, 20);
        got = c.take();
        CHECK(got.size() == 1 && got[0].below);
        // Past the cap: each root that holds any, below; what comes after
        // is named again until the cap.
        c.setRoots({"/r", "/s", "/t"});
        for (int i = 0; i < 6; i++) c.add({"/r/f" + std::to_string(i), false}, 30);
        c.add({"/s/x", false}, 30);
        c.add({"/r/f9", false}, 30);   // under /r, below already
        got = c.take();
        CHECK(got.size() == 2 && got[0] == (DiskWatch::Change{"/r", true}) && got[1] == (DiskWatch::Change{"/s/x", false}));
        for (int i = 0; i < 6; i++) c.add({"/s/f" + std::to_string(i), false}, 40);
        c.add({"/r/a", false}, 40);
        got = c.take();
        CHECK(got.size() == 2 && got[0] == (DiskWatch::Change{"/r/a", false}) && got[1] == (DiskWatch::Change{"/s", true}));
    }

    // Which folders count: not .git's insides, not what the ignore files
    // ignore or a node_modules, unless a tree has it open (then its
    // listing, never Source Control).
    GROUP("diskwatch:filter");
    {
        const std::string root = realScratch();
        for (const char* d : {"src/deep", "build/obj", "node_modules/pkg", ".git/objects/ab", ".git/refs/heads",
                              "logs", "lib/node_modules/x"})
            fs::create_directories(root + "/" + d);
        fsWrite(root + "/.gitignore", "build/\n*.log\nlogs/\n");
        fsWrite(root + "/src/.gitignore", "deep/\n");
        using S = DiskWatch::Seen;
        DiskWatch::Filter f(root, {});
        CHECK(f.root() == root);
        CHECK(f.seen("") == S::Yes && f.seen("src") == S::Yes && f.seen(".git") == S::Yes);
        CHECK(f.seen(".git/refs") == S::Yes && f.seen(".git/refs/heads") == S::Yes);
        CHECK(f.seen(".git/objects") == S::No && f.seen(".git/objects/ab") == S::No);
        CHECK(f.seen("build") == S::No && f.seen("build/obj") == S::No && f.seen("logs") == S::No);
        CHECK(f.seen("node_modules") == S::No && f.seen("node_modules/pkg") == S::No);
        CHECK(f.seen("lib/node_modules") == S::No && f.seen("lib/node_modules/x") == S::No);
        CHECK(f.seen("src/deep") == S::No);   // the subfolder's own ignore file
        CHECK(f.seen("never/made") == S::Yes);   // a folder not there yet counts by name
        // Open in a tree: the ignored folder's listing follows, and what is
        // below it only when open too.
        DiskWatch::Filter g(root, {"build", "node_modules", "node_modules/pkg", "src/deep", ".git/objects"});
        CHECK(g.seen("build") == S::OpenOnly && g.seen("build/obj") == S::No);
        CHECK(g.seen("node_modules") == S::OpenOnly && g.seen("node_modules/pkg") == S::OpenOnly);
        CHECK(g.seen("src/deep") == S::OpenOnly && g.seen(".git/objects") == S::OpenOnly);
        CHECK(g.seen("src") == S::Yes && g.seen("logs") == S::No);
        // Malformed open folders are left out.
        DiskWatch::Filter h(root, {"../x", "/abs", "build/"});
        CHECK(h.seen("build") == S::No);
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // The rules a document follows its file by (the Mac's for a tab).
    GROUP("diskwatch:reload-rules");
    {
        using A = Shared::DiskAction;
        Shared::DiskLook l;
        l.hadFile = l.exists = l.text = true;
        CHECK(Shared::diskAction(l) == A::Follow);   // clean, the file changed
        l.edited = true;
        CHECK(Shared::diskAction(l) == A::Changed);   // never written over
        l.sameAsSaved = true;
        CHECK(Shared::diskAction(l) == A::Same);   // a write that changed nothing
        l.edited = false;
        CHECK(Shared::diskAction(l) == A::Same);
        l.sameAsSaved = false;
        l.text = false;
        CHECK(Shared::diskAction(l) == A::Changed);   // no longer text: never followed
        l.exists = false;
        CHECK(Shared::diskAction(l) == A::Gone);
        l.wasGone = true;
        CHECK(Shared::diskAction(l) == A::Nothing);   // told once
        l.wasGone = false;
        l.hadFile = false;
        CHECK(Shared::diskAction(l) == A::Nothing);   // a file not made yet
        // The file back after it went: followed when nothing was typed.
        l = Shared::DiskLook();
        l.hadFile = l.exists = l.text = l.wasGone = true;
        CHECK(Shared::diskAction(l) == A::Follow);
        // An app's window follows its own tab.
        l.appHasIt = true;
        CHECK(Shared::diskAction(l) == A::Nothing);
        l.exists = false;
        CHECK(Shared::diskAction(l) == A::Nothing);

        // The file as the host reads it to follow it.
        const std::string root = realScratch();
        fsWrite(root + "/u16.txt", std::string("\xFF\xFE" "h\0i\0", 6));
        Shared::DiskText t = Shared::readDiskText(root + "/u16.txt");
        CHECK(t.exists && t.regular && t.text && t.body == u"hi" && t.bom &&
              t.encoding == FolderSearch::TextEncoding::Utf16LE && t.stamp.exists);
        t = Shared::readDiskText(root + "/missing.txt");
        CHECK(!t.exists && !t.text && !t.stamp.exists);
        t = Shared::readDiskText(root);
        CHECK(t.exists && !t.regular && !t.text);
        fsWrite(root + "/bin.dat", std::string("\x80\x81\0\xFE", 4));
        t = Shared::readDiskText(root + "/bin.dat");
        CHECK(t.exists && t.regular && (t.text == (t.encoding == FolderSearch::TextEncoding::Windows1252)));
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // A Host with renderers through the encoding: the trees' listings,
    // Source Control, documents following their files, Reload.
    GROUP("diskwatch:host");
    {
        const std::string root = realScratch();
        for (const char* d : {"sub", "build", ".git/objects/ab", "node_modules"}) fs::create_directories(root + "/" + d);
        fsWrite(root + "/.gitignore", "build/\n");
        Net net(true, true);
        net.host.setFolder(root);
        net.host.setWatching(true);
        Net::Link& guest = net.add(Unit::Utf16, "browser", 2 | 8 | 32);
        Net::Link& reader = net.add(Unit::Utf16, "reader", 1 | 32);
        Net::Link& old = net.add(Unit::Utf16, "older page", 2);   // hears no disk
        Net::Link& ed = net.add(Unit::Utf16, "editor", 8);
        for (Net::Link* l : {&guest, &reader, &old, &ed}) l->client->dispatch();
        CHECK(guest.client->hostWatches() && old.client->hostWatches());

        // The trees say what they have open.
        const std::uint64_t req = guest.client->watchFolders(0, {"", "sub", "build"}, false);
        guest.client->dispatch();
        Json a;
        CHECK(req && guest.client->readAnswer(req, &a) && a["watching"].asInt() == 3 &&
              a["root"].asString() == root);
        reader.client->watchFolders(0, {""}, false);
        old.client->watchFolders(0, {"sub"}, false);
        for (Net::Link* l : {&reader, &old}) l->client->dispatch();
        std::vector<DiskWatch::Root> roots = net.host.watchRoots();
        CHECK(roots.size() == 1 && roots[0].path == root &&
              roots[0].open == (std::vector<std::string>{"", "build", "sub"}));
        // A path that is not the folder's own is left out.
        const std::uint64_t bad = guest.client->watchFolders(0, {"", "sub", "build", "../out", "/etc", "a//b"}, false);
        guest.client->dispatch();
        CHECK(bad && guest.client->readAnswer(bad, &a) && a["watching"].asInt() == 3);

        // A file made in sub: the guest's tree has sub open and hears its
        // listing again, with Source Control; the reader has only the top
        // open; the older page hears nothing.
        fsWrite(root + "/sub/new.txt", "n\n");
        net.host.diskChanged({{root + "/sub", false}});
        for (Net::Link* l : {&guest, &reader, &old}) l->client->dispatch();
        CHECK(guest.changed.size() == 1 && reader.changed.size() == 1 && old.changed.empty());
        if (guest.changed.size() == 1) {
            CHECK(changedNames(guest.changed[0], "sub") == "new.txt");
            CHECK(guest.changed[0]["git"].asBool() && guest.changed[0]["root"].asString() == root);
            CHECK(!hasListing(guest.changed[0], "") && !hasListing(guest.changed[0], "build"));
        }
        if (reader.changed.size() == 1)
            CHECK(reader.changed[0]["git"].asBool() && reader.changed[0]["listings"].size() == 0);
        // The ignored build, open in the guest's tree: its listing, no git.
        fsWrite(root + "/build/out.o", "o");
        net.host.diskChanged({{root + "/build", false}});
        for (Net::Link* l : {&guest, &reader}) l->client->dispatch();
        CHECK(guest.changed.size() == 2 && reader.changed.size() == 1);
        if (guest.changed.size() == 2)
            CHECK(changedNames(guest.changed[1], "build") == "out.o" && !guest.changed[1]["git"].asBool());
        // .git's insides and a node_modules nobody opened: nothing; .git
        // itself (an index written through its lock): Source Control only.
        net.host.diskChanged({{root + "/.git/objects/ab", false}, {root + "/node_modules", false}});
        guest.client->dispatch();
        CHECK(guest.changed.size() == 2);
        net.host.diskChanged({{root + "/.git", false}});
        guest.client->dispatch();
        CHECK(guest.changed.size() == 3 && guest.changed.back()["git"].asBool() &&
              guest.changed.back()["listings"].size() == 0);
        // Everything below the top (the system dropped events): every open
        // folder listed again.
        net.host.diskChanged({{root, true}});
        guest.client->dispatch();
        CHECK(guest.changed.size() == 4 && guest.changed.back()["listings"].size() == 3);
        // A folder that went: its listing says so (the page closes it).
        fs::remove_all(root + "/sub");
        net.host.diskChanged({{root + "/sub", true}});
        guest.client->dispatch();
        CHECK(guest.changed.size() == 5);
        if (guest.changed.size() == 5) {
            bool err = false;
            for (const Json& l : guest.changed.back()["listings"].elements())
                err = err || (l["path"].asString() == "sub" && l.has("error"));
            CHECK(err);
        }
        fs::create_directories(root + "/sub");

        // A document no app's window has open, clean: it follows its file,
        // one step of the host's every renderer showing it gets.
        const std::string path = root + "/f.txt";
        fsWrite(path, "one\n");
        TestMirror em;
        em.client = ed.client.get();
        em.doc = 1;
        em.text = u"one\n";
        ed.client->open(1, path, "one\n", &em);
        TestMirror gm;
        gm.client = guest.client.get();
        gm.doc = 1;
        guest.client->view(1, &gm);
        guest.client->dispatch();
        CHECK(gm.text == u"one\n");
        fsWrite(path, "zero\none\ntwo\n");
        net.host.diskChanged({{root, false}});
        ed.client->dispatch();
        guest.client->dispatch();
        CHECK(em.text == u"zero\none\ntwo\n" && gm.text == em.text && net.host.text(1) == em.text);
        CHECK(!net.host.dirty(1) && net.host.diskState(1) == 0);
        CHECK(ed.client->undoInfo(1).undoName == "Reload from Disk");
        // Nothing new on disk since: no edit.
        const int edits = em.hostEdits;
        net.host.diskChanged({{root, false}});
        ed.client->dispatch();
        CHECK(em.hostEdits == edits);

        // Unsaved changes: never written over; marked changed on disk, the
        // renderers that hear the disk told, the state says it, and a save
        // asks first.
        em.caret = em.length();
        em.type(u"mine\n");
        ed.client->settle();
        CHECK(net.host.dirty(1));
        fsWrite(path, "an agent's\n");
        net.host.diskChanged({{root, false}});
        for (Net::Link* l : {&ed, &guest, &reader, &old}) l->client->dispatch();
        CHECK(em.text == u"zero\none\ntwo\nmine\n" && net.host.diskState(1) == 1);
        CHECK(!guest.disks.empty() && guest.disks.back() == std::make_pair(Shared::DocId(1), 1));
        CHECK(!reader.disks.empty() && old.disks.empty() && ed.disks.empty());
        CHECK(ed.client->sessionState()["documents"][0]["disk"].asString() == "changed");
        Shared::Client::SaveAnswer s;
        std::uint64_t sr = guest.client->requestSave(1, 0, false);
        guest.client->dispatch();
        CHECK(sr && guest.client->saveAnswer(sr, &s) && !s.ok && s.json["reason"].asString() == "changed");
        CHECK(readFile(path.c_str()) == "an agent's\n");
        // Reload, the guest's: the file's text, one step, clean and in step.
        const std::size_t heard = guest.disks.size();
        std::uint64_t rr = guest.client->requestReload(1);
        guest.client->dispatch();
        ed.client->dispatch();
        CHECK(rr && guest.client->reloadAnswer(rr, &s) && s.ok && s.version == net.host.version(1));
        CHECK(em.text == u"an agent's\n" && gm.text == em.text && !net.host.dirty(1) && net.host.diskState(1) == 0);
        CHECK(guest.disks.size() == heard + 1 && guest.disks.back().second == 0);
        CHECK(!ed.client->sessionState()["documents"][0].has("disk"));
        // Undo takes it back: the typing is there again, unsaved.
        CHECK(ed.client->undo(1, false) == Shared::Outcome::Done);
        CHECK(em.text == u"zero\none\ntwo\nmine\n" && net.host.dirty(1));
        rr = guest.client->requestReload(1);
        guest.client->dispatch();
        ed.client->dispatch();
        CHECK(guest.client->reloadAnswer(rr, &s) && s.ok && em.text == u"an agent's\n");

        // Deleted: kept, unsaved, gone; back with the saved text: in step.
        fs::remove(path);
        net.host.diskChanged({{root, false}});
        guest.client->dispatch();
        CHECK(net.host.diskState(1) == 2 && net.host.dirty(1) && em.text == u"an agent's\n");
        CHECK(!guest.dirties.empty() && guest.dirties.back().second);
        rr = guest.client->requestReload(1);
        guest.client->dispatch();
        CHECK(guest.client->reloadAnswer(rr, &s) && !s.ok && s.json["reason"].asString() == "gone");
        fsWrite(path, "an agent's\n");
        net.host.diskChanged({{root, false}});
        guest.client->dispatch();
        CHECK(net.host.diskState(1) == 0 && !net.host.dirty(1) && !guest.dirties.back().second);
        CHECK(guest.disks.back().second == 0);
        // Gone, then back with other text and nothing typed: followed.
        fs::remove(path);
        net.host.diskChanged({{root, false}});
        fsWrite(path, "new life\n");
        net.host.diskChanged({{root, false}});
        ed.client->dispatch();
        CHECK(em.text == u"new life\n" && !net.host.dirty(1) && net.host.diskState(1) == 0);

        // An app's window has it open: the app follows its own tab; the
        // host leaves it, and a page's Reload is refused.
        Net::Link& app = net.add(Unit::Utf16, "app", 4);
        app.client->dispatch();
        TestMirror am;
        am.client = app.client.get();
        am.doc = 1;
        app.client->open(1, path, "", &am);
        app.client->dispatch();
        fsWrite(path, "the app's to follow\n");
        net.host.diskChanged({{root, false}});
        ed.client->dispatch();
        CHECK(em.text == u"new life\n" && net.host.diskState(1) == 0);
        rr = guest.client->requestReload(1);
        guest.client->dispatch();
        CHECK(guest.client->reloadAnswer(rr, &s) && !s.ok && s.json["reason"].asString() == "app" &&
              s.json["why"].asString() == "f.txt is open in a mutible window: reload it there.");
        // Once it lets go, the host follows.
        app.client->detach(1);
        app.client->dispatch();
        net.host.diskChanged({{root, false}});
        ed.client->dispatch();
        CHECK(em.text == u"the app's to follow\n");

        // A document's folder is watched whatever the ignore files say.
        TestMirror bm;
        bm.client = ed.client.get();
        bm.doc = 2;
        fsWrite(root + "/build/gen.c", "int x;\n");
        bm.text = u"int x;\n";
        reader.client->watchFolders(0, {}, false);
        reader.client->dispatch();
        guest.client->watchFolders(0, {""}, false);
        guest.client->dispatch();
        ed.client->open(2, root + "/build/gen.c", "int x;\n", &bm);
        ed.client->settle();
        roots = net.host.watchRoots();
        // (The older page still has sub open.)
        CHECK(roots.size() == 1 && roots[0].open == (std::vector<std::string>{"", "build", "sub"}));
        fsWrite(root + "/build/gen.c", "int y;\n");
        net.host.diskChanged({{root + "/build", false}});
        ed.client->dispatch();
        guest.client->dispatch();
        CHECK(bm.text == u"int y;\n");
        // ...but its listing goes only to a tree that has it open.
        CHECK(!guest.changed.empty() && !hasListing(guest.changed.back(), "build"));
        // Renderers going take their trees' folders with them.
        const std::uint64_t gen = net.host.watchGeneration();
        net.host.disconnect(reader.id);
        CHECK(net.host.watchGeneration() != gen);

        // A host that does not watch: no flag, nothing sent, and a raw
        // "watch" read answered with why.
        Net plain(true, true);
        plain.host.setFolder(root);
        Net::Link& pg = plain.add(Unit::Utf16, "browser", 2 | 32);
        pg.client->dispatch();
        CHECK(!pg.client->hostWatches() && pg.client->watchFolders(0, {""}, false) == 0);
        CHECK(plain.host.watchRoots().empty());
        Json w = Json::object();
        w.set("op", Json("watch"));
        const Json wa = pg.client->read(0, w, 2000);
        CHECK(wa["error"].asString() == "this host does not watch the disk");

        // The new messages through the encoding.
        Shared::Message m;
        m.kind = Shared::Kind::FsChanged;
        m.window = 7;
        m.json = guest.changed.front();
        Shared::Message back = Shared::roundTrip(m);
        CHECK(back.kind == Shared::Kind::FsChanged && back.window == 7 && back.json.dump() == m.json.dump());
        m = Shared::Message();
        m.kind = Shared::Kind::DocDisk;
        m.doc = 3;
        m.flag = 2;
        m.v = 9;
        back = Shared::roundTrip(m);
        CHECK(back.kind == Shared::Kind::DocDisk && back.doc == 3 && back.flag == 2 && back.v == 9);
        m = Shared::Message();
        m.kind = Shared::Kind::DocReload;
        m.doc = 4;
        m.req = 5;
        back = Shared::roundTrip(m);
        CHECK(back.kind == Shared::Kind::DocReload && back.doc == 4 && back.req == 5);
        m.kind = Shared::Kind::DocReloadDone;
        m.outcome = Shared::Outcome::Refused;
        back = Shared::roundTrip(m);
        CHECK(back.kind == Shared::Kind::DocReloadDone && back.outcome == Shared::Outcome::Refused);
        CHECK(std::string(Shared::kindName(Shared::Kind::FsChanged)) == "fs.changed" &&
              std::string(Shared::kindName(Shared::Kind::DocReloadDone)) == "doc.reload-done");
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // The system's watcher on a scratch folder: a change in a folder it
    // watches comes; one in a node_modules nobody opened counts for nothing
    // (inotify never watched it; FSEvents names it and the filter drops it);
    // opened in a tree, it comes too.
    GROUP("diskwatch:system");
    {
        std::unique_ptr<DiskWatch::Watcher> w = DiskWatch::Watcher::make();
        if (!w) {
            std::printf("  diskwatch:system: no watcher on this system, skipped\n");
        } else {
            const std::string root = realScratch();
            for (const char* d : {"src", "node_modules/pkg"}) fs::create_directories(root + "/" + d);
            w->watch({DiskWatch::Root{root, {""}}});
            CHECK(waitUntil([&] { return w->settled(); }, 10000));
            CHECK(w->folders() >= 1);
            std::vector<DiskWatch::Change> got;
            auto drain = [&] {
                std::vector<DiskWatch::Change> more;
                w->take(&more);
                got.insert(got.end(), more.begin(), more.end());
            };
            auto came = [&](const std::string& path) {
                drain();
                for (const auto& c : got)
                    if (c.path == path || (c.below && DiskWatch::under(path, c.path))) return true;
                return false;
            };
            // The folders' own making may still come (FSEvents' journal
            // trails a little): let it, and forget it.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));   // time is the condition
            drain();
            got.clear();
            // node_modules first: after a while, nothing there the filter
            // counts (inotify sends none; FSEvents names it, and the filter
            // drops it).
            fsWrite(root + "/node_modules/pkg/index.js", "x");
            std::this_thread::sleep_for(std::chrono::milliseconds(700));   // a negative check: time is the condition
            drain();
            DiskWatch::Filter f(root, {""});
            int counted = 0;
            for (const auto& c : got) {
                std::string rel;
                if (DiskWatch::relativeTo(root + "/node_modules", c.path, nullptr) &&
                    DiskWatch::relativeTo(root, c.path, &rel) && f.seen(rel) != DiskWatch::Seen::No)
                    counted++;
            }
            CHECK(counted == 0);
            if (counted)
                for (const auto& c : got) std::printf("  diskwatch:system: %s\n", c.path.c_str());
            got.clear();
            fsWrite(root + "/src/a.c", "int a;\n");
            CHECK(waitUntil([&] { return came(root + "/src"); }, 10000));
            // A folder made after the watch began is watched too.
            fs::create_directories(root + "/src/new");
            CHECK(waitUntil([&] { return w->settled() && came(root + "/src"); }, 10000));
            got.clear();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));   // the folder's own watch made
            fsWrite(root + "/src/new/b.c", "int b;\n");
            CHECK(waitUntil([&] { return came(root + "/src/new"); }, 10000));
            // Opened in a tree: node_modules comes too.
            w->watch({DiskWatch::Root{root, {"", "node_modules"}}});
            CHECK(waitUntil([&] { return w->settled(); }, 10000));
            got.clear();
            fsWrite(root + "/node_modules/top.js", "y");
            CHECK(waitUntil([&] { return came(root + "/node_modules"); }, 10000));
            // No root: nothing watched.
            w->watch({});
            CHECK(waitUntil([&] { return w->settled() && w->folders() == 0; }, 10000));
            w.reset();
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    }

    // A headless host over its socket that watches the disk: files written
    // in the folder reach a renderer as listings and Source Control, a
    // clean document follows its file, unsaved changes are marked, Reload,
    // a deleted file.
    GROUP("diskwatch:headless");
    if (!DiskWatch::Watcher::make()) {
        std::printf("  diskwatch:headless: no watcher on this system, skipped\n");
    } else {
        ServerRig rig;
        rig.watch = true;
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        fs::create_directories(rig.proj + "/sub");
        std::string error;
        std::unique_ptr<Shared::HostLink> link = Shared::HostLink::open(rig.path(), &error);
        CHECK(link != nullptr);
        if (link) {
            Shared::Client client(Unit::Utf16, "page");
            std::vector<Json> changed;
            std::vector<std::pair<Shared::DocId, int>> disks;
            client.hearDisk([&](std::uint64_t, const Json& j) { changed.push_back(j); },
                            [&](Shared::DocId d, int disk) { disks.push_back({d, disk}); });
            link->connect(client);
            CHECK(client.settle() && client.hostWatches());
            auto wait = [&](const std::function<bool()>& done) {
                return waitUntil(
                    [&] {
                        client.dispatch();
                        return done();
                    },
                    10000);
            };
            const std::uint64_t req = client.watchFolders(0, {"", "sub"}, false);
            Json a;
            CHECK(wait([&] { return client.readAnswer(req, &a); }) && a["watching"].asInt() == 2);
            std::size_t folders = 0;
            CHECK(rig.server->host().diskWatchSettled(10000, &folders) && folders >= 1);

            fsWrite(rig.proj + "/sub/made.txt", "m\n");
            CHECK(wait([&] {
                for (const Json& c : changed)
                    if (changedNames(c, "sub") == "made.txt") return true;
                return false;
            }));
            bool git = false;
            for (const Json& c : changed) git = git || c["git"].asBool();
            CHECK(git);

            // A document (this renderer is no app: the host follows it).
            const std::string path = rig.proj + "/notes.txt";
            fsWrite(path, "first\n");
            TestMirror mirror;
            mirror.client = &client;
            mirror.doc = client.newId();
            mirror.text = u"first\n";
            client.open(mirror.doc, path, "first\n", &mirror);
            CHECK(client.settle());
            CHECK(rig.server->host().diskWatchSettled(10000));
            fsWrite(path, "second\n");
            CHECK(wait([&] { return mirror.text == u"second\n"; }));
            CHECK(!client.hostDocument(mirror.doc).dirty);
            mirror.caret = 0;
            mirror.type(u"# ");
            CHECK(client.settle());
            fsWrite(path, "third\n");
            CHECK(wait([&] { return !disks.empty() && disks.back() == std::make_pair(mirror.doc, 1); }));
            CHECK(mirror.text == u"# second\n" && client.hostDocument(mirror.doc).dirty);
            const std::uint64_t rr = client.requestReload(mirror.doc);
            Shared::Client::SaveAnswer s;
            CHECK(rr && wait([&] { return client.reloadAnswer(rr, &s); }) && s.ok);
            CHECK(mirror.text == u"third\n" && !client.hostDocument(mirror.doc).dirty);
            CHECK(wait([&] { return disks.back().second == 0; }));
            fs::remove(path);
            CHECK(wait([&] { return disks.back() == std::make_pair(mirror.doc, 2); }));
            CHECK(client.hostDocument(mirror.doc).dirty && mirror.text == u"third\n");
            link->close();
        }
        rig.stop();
    }

    // `mutible host --stdio` as a window over SSH runs it (stage 8): it
    // watches the disk, so the window's tree (an app that hears the disk,
    // Hello flag 32) gets the folders it has open listed again as they
    // change there; and the address's new file is named "new" in the
    // Welcome and made by the window's first save through the host.
    GROUP("diskwatch:stdio");
    {
        signal(SIGPIPE, SIG_IGN);
        const std::string proj = realScratch();
        fs::create_directories(proj + "/src");
        fsWrite(proj + "/src/a.c", "int a;\n");
        int toHost[2], fromHost[2];
        CHECK(pipe(toHost) == 0 && pipe(fromHost) == 0);
        auto server = std::make_unique<Shared::HostServer>();
        Shared::HostOptions o;
        o.folder = proj;
        o.watchDisk = true;
        o.askedFile = proj + "/src/new.txt";
        o.askedFileNew = true;
        std::string error;
        CHECK(server->startStdio(o, toHost[0], fromHost[1], &error) == Shared::HostServer::Start::Ok);
        std::atomic<int> result{-1};
        std::thread run([&] { result = server->run() ? 1 : 0; });
        std::mutex mu;
        std::vector<Json> changed;
        PipeRenderer r;
        r.connect(toHost[1], fromHost[0], [&](Shared::Client& c) {
            c.hearDisk(
                [&](std::uint64_t, const Json& j) {
                    std::lock_guard<std::mutex> lk(mu);
                    changed.push_back(j);
                },
                [](Shared::DocId, int) {});
        });
        CHECK(r.client->settle(5000));
        CHECK(r.client->hostWatches() && r.client->hostSearches());
        CHECK(r.client->welcomeJson()["file"].asString() == proj + "/src/new.txt");
        CHECK(r.client->welcomeJson()["new"].asBool());

        // The tree's folders, as the window says them (its root and src).
        const std::uint64_t req = r.client->watchFolders(0, {"", "src"}, false);
        Json a;
        CHECK(req != 0 && r.wait([&] { return r.client->readAnswer(req, &a); }));
        CHECK(a["watching"].asInt() == 2);
        // Listed through the host as the window's tree lists a folder.
        Json ask = Json::object();
        ask.set("op", Json("list"));
        ask.set("path", Json("src"));
        CHECK(fsNames(r.client->read(0, ask, 5000)) == "a.c");
        if (DiskWatch::Watcher::make()) {
            CHECK(server->host().diskWatchSettled(10000));
            fsWrite(proj + "/src/b.c", "int b;\n");
            CHECK(r.wait([&] {
                std::lock_guard<std::mutex> lk(mu);
                for (const Json& c : changed)
                    if (changedNames(c, "src") == "a.c b.c") return true;
                return false;
            }, 10000));
        } else {
            std::printf("  diskwatch:stdio: no watcher on this system, its notices not checked\n");
        }

        // The new file: an untitled document, given its path, saved by the
        // host, which makes it.
        const Shared::DocId d = r.client->newId();
        r.mirror.doc = d;
        r.mirror.text = u"";
        r.client->open(d, "", "", &r.mirror);
        r.mirror.type(u"made over ssh\n");
        CHECK(r.client->settle());
        r.client->setPath(d, proj + "/src/new.txt");
        const std::uint64_t save = r.client->requestSave(d, 0, true, "utf8");
        Shared::Client::SaveAnswer s;
        CHECK(save != 0 && r.wait([&] { return r.client->saveAnswer(save, &s); }));
        CHECK(s.ok);
        {
            std::ifstream f(proj + "/src/new.txt");
            std::stringstream ss;
            ss << f.rdbuf();
            CHECK(ss.str() == "made over ssh\n");
        }

        r.closeInput();
        CHECK(waitUntil([&] { return result.load() != -1; }, 5000));
        if (run.joinable()) run.join();
        server.reset();
    }

    // A browser's page of a headless host that watches the disk: its tree
    // follows the disk with no ask, a document changed on disk under the
    // page's typing is marked in the state, and the page's Reload.
    GROUP("diskwatch:page");
    if (!DiskWatch::Watcher::make()) {
        std::printf("  diskwatch:page: no watcher on this system, skipped\n");
    } else {
        ServerRig rig;
        rig.watch = true;
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        fs::create_directories(rig.proj + "/src");
        fsWrite(rig.proj + "/src/a.c", "int a;\n");
        const std::string file = rig.proj + "/notes.txt";
        fsWrite(file, "one\n");
        SocketRenderer ed;
        CHECK(ed.attach(rig.path(), "editor"));
        Web::Server web;
        Web::Options o;
        o.hostSocket = rig.path();
        o.folder = rig.proj;
        o.pollMs = 60000;
        o.editable = true;
        std::string error;
        CHECK(web.start(o, &error));
        std::thread webThread([&] { web.run(); });
        if (ed.client && web.port() > 0) {
            ed.mirror.doc = 1;
            ed.mirror.text = u"one\n";
            ed.client->open(1, file, "one\n", &ed.mirror);
            CHECK(ed.client->settle());
            PageClient p;
            CHECK(p.open(web));
            PageEditor page;
            page.p = &p;
            page.id = 1;
            Json hello, state;
            std::vector<Json> trees, reloads;
            auto take = [&](const Json& m) {
                const std::string t = m["t"].asString();
                if (t == "hello") hello = m;
                else if (t == "state") state = m;
                else if (t == "tree") trees.push_back(m);
                else if (t == "reloaded") reloads.push_back(m);
                page.take(m);
            };
            auto diskOf = [&] {
                for (const Json& d : state["docs"].elements())
                    if (d["id"].asInt() == 1) return d["disk"].asString();
                return std::string();
            };
            auto treeHas = [&](const std::string& path, const std::string& name) {
                for (const Json& t : trees) {
                    if (t["path"].asString() != path) continue;
                    for (const Json& e : t["entries"].elements())
                        if (e["name"].asString() == name) return true;
                }
                return false;
            };
            CHECK(p.pump(take, [&] { return state.isObject(); }));
            CHECK(hello["canReload"].asBool());
            p.sendText("{\"t\":\"watch\",\"paths\":[\"\",\"src\"]}");
            p.sendText("{\"t\":\"list\",\"path\":\"src\"}");
            CHECK(p.pump(take, [&] { return treeHas("src", "a.c"); }));
            CHECK(rig.server->host().diskWatchSettled(10000));
            trees.clear();
            fsWrite(rig.proj + "/src/b.c", "int b;\n");
            CHECK(p.pump(take, [&] { return treeHas("src", "b.c"); }, 10000));   // nobody asked
            // The tree's open folders are the page's: closing src stops it.
            p.sendText("{\"t\":\"watch\",\"paths\":[\"\"]}");
            p.sendText("{\"t\":\"list\",\"path\":\"\"}");
            CHECK(p.pump(take, [&] { return treeHas("", "src"); }));

            // The page types; an agent writes the file: marked, not followed.
            p.sendText("{\"t\":\"show\",\"doc\":1}");
            CHECK(p.pump(take, [&] { return page.docs == 1; }));
            page.edit(Ot::Ops{Ot::Op{0, 0, u"// "}});
            CHECK(p.pump(take, [&] { return page.synced(); }, 4000));
            fsWrite(file, "an agent's\n");
            CHECK(p.pump(take, [&] { return diskOf() == "changed"; }, 10000));
            CHECK(page.text() == u"// one\n");
            // Reload: the file's text in the page, and the mark gone.
            p.sendText("{\"t\":\"reload\",\"doc\":1}");
            CHECK(p.pump(take, [&] { return !reloads.empty() && page.text() == u"an agent's\n" && diskOf().empty(); },
                         10000));
            CHECK(!reloads.empty() && reloads[0]["ok"].asBool());
            p.close();
            CHECK(waitUntil([&] { return web.browsers() == 0; }));
        }
        web.stop();
        webThread.join();
        ed.detach();
        rig.stop();
    }
}
