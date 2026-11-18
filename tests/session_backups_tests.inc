// session_backups_tests.inc — the `hostbackup:*` and `reattach:*` groups of
// run_tests.cpp: a headless host's backups of the unsaved documents no
// app's window has open (Host::keepBackups, SessionHost.h "Backups"), and
// what a window opening on a host's folder takes of what it holds
// (Shared::reattachFrom, SessionClient.h: untitled buffers, two documents
// of one path), plus the shared helpers in Backups.h they lean on.
// Included in run_tests.cpp after session_server_tests.inc, whose rigs
// (ServerRig, SocketRenderer, waitUntil) and session_host_tests.inc's Net
// and TestMirror it uses.

namespace {

// A host keeping backups in a scratch store, on clocks the test moves.
struct KeptRig {
    std::string store = scratchFolder() + "/Backups";
    std::string root = Shared::resolveFolder(scratchFolder());
    double mono = 100, wall = 1.7e9;
    int ids = 0;
    Backups::Store disk() const {
        Backups::Store s;
        s.dir = store;
        s.fs = Backups::posixFs();
        return s;
    }
    Shared::Host::BackupKeeping keeping() {
        Shared::Host::BackupKeeping k;
        k.store = disk();
        k.root = root;
        k.ownerPid = 4242;
        k.ownerStart = 7.5;
        k.monoNow = [this] { return mono; };
        k.wallNow = [this] { return wall; };
        k.newId = [this] { return "kept-" + std::to_string(++ids); };
        return k;
    }
    std::vector<Backups::Entry> list() const { return disk().list(); }
    // The text of the one backup of `path`, "" (and *found false) for none.
    std::string textOf(const std::string& path, bool* found = nullptr, Backups::Entry* entry = nullptr) const {
        if (found) *found = false;
        for (const Backups::Entry& e : list())
            if (e.path == path) {
                Backups::Entry got;
                std::string text;
                if (!disk().load(e.id, &got, &text)) return "";
                if (found) *found = true;
                if (entry) *entry = got;
                return text;
            }
        return "";
    }
};

void putFile(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
}

// A host state as StateQuery answers it, for reattachFrom.
Json stateDoc(int64_t id, const std::string& path, int64_t length, bool dirty) {
    Json j = Json::object();
    j.set("doc", Json(id));
    j.set("path", Json(path));
    j.set("version", Json((int64_t)1));
    j.set("length", Json(length));
    j.set("dirty", Json(dirty));
    return j;
}
Json stateRecord(int64_t window, int64_t renderer, bool connected, int64_t seen, std::vector<int64_t> docs,
                 std::vector<int64_t> terms = {}) {
    Json tabs = Json::array();
    for (int64_t d : docs) {
        Json t = Json::object();
        t.set("doc", Json(d));
        tabs.push(std::move(t));
    }
    Json group = Json::object();
    group.set("tabs", std::move(tabs));
    Json groups = Json::array();
    groups.push(std::move(group));
    Json termIds = Json::array();
    for (int64_t t : terms) termIds.push(Json(t));
    Json record = Json::object();
    record.set("groups", std::move(groups));
    record.set("terms", std::move(termIds));
    Json j = Json::object();
    j.set("window", Json(window));
    j.set("renderer", Json(renderer));
    j.set("connected", Json(connected));
    j.set("seen", Json(seen));
    j.set("tabs", std::move(record));
    return j;
}
std::vector<uint64_t> docIds(const Shared::Reattach& r) {
    std::vector<uint64_t> out;
    for (const auto& d : r.docs) out.push_back(d.id);
    return out;
}

}  // namespace

static void testSessionBackups() {
    GROUP("hostbackup:helpers");
    {
        CHECK(Backups::defaultDir("/x/b/", "/home/u", "/xdg", false) == "/x/b");
        CHECK(Backups::defaultDir(nullptr, "/home/u", "/xdg", false) == "/xdg/mutible/Backups");
        CHECK(Backups::defaultDir("", "/home/u", "", false) == "/home/u/.local/share/mutible/Backups");
        CHECK(Backups::defaultDir(nullptr, "/home/u", "relative", false) == "/home/u/.local/share/mutible/Backups");
        CHECK(Backups::defaultDir(nullptr, "/Users/u", "/xdg", true) ==
              "/Users/u/Library/Application Support/Mutible/Backups");
        CHECK(Backups::defaultDir(nullptr, nullptr, nullptr, false).empty());
        const std::string a = Backups::newId(), b = Backups::newId();
        CHECK(Backups::validId(a) && a.size() == 36 && a != b);
        // This process is running, from when it says; no such process is 0.
        const double me = Backups::processStart(getpid());
        CHECK(me > 0 && Backups::processStart(getpid()) == me);
        CHECK(Backups::processStart(0) == 0 && Backups::processStart(-3) == 0);
        std::set<std::string> taken = {"/p/Untitled-1", "/p/Untitled-2"};
        CHECK(Backups::untitledPath("/p/", [&](const std::string& s) { return taken.count(s) > 0; }) ==
              "/p/Untitled-3");
        CHECK(Backups::untitledPath("/p", nullptr) == "/p/Untitled-1");
    }

    GROUP("hostbackup:detach-unsaved");
    {
        // An app's window has the file open, unsaved: its own backup is the
        // one, so the host keeps none. It closes keeping the file unsaved on
        // the host: the host's backup is on disk at once, in the apps' store
        // and format, held by the host's process.
        KeptRig rig;
        const std::string file = rig.root + "/notes.txt";
        putFile(file, "saved\n");
        Net net(true, true);
        net.host.keepBackups(rig.keeping());
        CHECK(net.host.keepsBackups());
        Net::Link& app = net.add(Unit::CodePoint, "gtk", 4);
        CHECK(app.client->settle());
        TestMirror& ma = net.open(app, 41, u"saved\n");
        app.client->setPath(41, file);
        ma.caret = ma.length();
        ma.type(u"typed");
        CHECK(app.client->settle());
        CHECK(net.host.dirty(41) && rig.list().empty() && net.host.backupId(41).empty());
        rig.mono += 10;   // nothing planned for a document an app shows
        net.host.tick();
        CHECK(rig.list().empty());
        app.client->detach(41);
        CHECK(app.client->settle());
        bool found = false;
        Backups::Entry e;
        CHECK(rig.textOf(file, &found, &e) == "saved\ntyped" && found);
        CHECK(net.host.backupId(41) == "kept-1" && e.id == "kept-1");
        CHECK(e.root == rig.root && e.ownerPid == 4242 && e.ownerStart == 7.5 && e.saved == rig.wall);
        CHECK(e.fileEncoding == "utf8");
        // The file as it was: unchanged since, as a restore will compare.
        CHECK(Backups::compare(e, Backups::posixFs().stat(file)) == Backups::Disk::Same);

        // A guest (a browser's view that edits) types: not at once, but a
        // second after typing stops, as the apps time theirs.
        Net::Link& guest = net.add(Unit::Utf16, "web", 2);
        CHECK(guest.client->settle());
        TestMirror& mg = net.open(guest, 41, u"");
        CHECK(guest.client->settle() && mg.text == u"saved\ntyped");
        mg.caret = mg.length();
        mg.type(u"!");
        CHECK(guest.client->settle() && net.host.text(41) == u"saved\ntyped!");
        CHECK(rig.textOf(file) == "saved\ntyped");
        const int wait = net.host.tick();
        CHECK(wait > 900 && wait <= 1000);
        rig.mono += 0.5;
        net.host.tick();
        CHECK(rig.textOf(file) == "saved\ntyped");
        rig.mono += 0.5;
        net.host.tick();
        CHECK(rig.textOf(file) == "saved\ntyped!");
        CHECK(rig.list().size() == 1 && net.host.backupId(41) == "kept-1");   // the same file, rewritten
        // Typing on and on is still on disk every five seconds.
        for (int i = 0; i < 12; i++) {
            mg.type(u".");
            CHECK(guest.client->settle());
            rig.mono += 0.5;
            net.host.tick();
        }
        CHECK(rig.textOf(file).size() > std::string("saved\ntyped!").size());

        // An app's window opens it again: its own backup takes over, the
        // host's goes at once.
        TestMirror again;
        again.client = app.client.get();
        again.doc = 41;
        again.unit = Unit::CodePoint;
        app.client->open(41, file, "", &again);
        CHECK(app.client->settle());
        CHECK(rig.list().empty() && net.host.backupId(41).empty());
        // The app goes (its connection lost): the host's, at once again.
        net.host.disconnect(app.id);
        CHECK(rig.textOf(file, &found) == u8(net.host.text(41)) && found);
        // Changed on disk since the buffer was based on it: a restore will
        // say so, and ask.
        putFile(file, "someone else\n");
        mg.type(u"?");
        CHECK(guest.client->settle());
        rig.mono += 2;
        net.host.tick();
        Backups::Entry changed;
        rig.textOf(file, &found, &changed);
        CHECK(found && Backups::compare(changed, Backups::posixFs().stat(file)) == Backups::Disk::Changed);

        // Saved (by a renderer that keeps no backups, as `mutible attach`
        // would be): nothing unsaved, so the backup goes.
        Net::Link& other = net.add(Unit::Utf16, "other");
        CHECK(other.client->settle());
        net.open(other, 41, u"");
        CHECK(other.client->settle());
        other.client->saved(41);
        CHECK(other.client->settle());
        CHECK(!net.host.dirty(41) && rig.list().empty());
        // Unsaved again, then closed by the last renderer that edits it.
        other.client->detach(41);
        CHECK(other.client->settle());
        mg.type(u"#");
        CHECK(guest.client->settle());
        rig.mono += 2;
        net.host.tick();
        CHECK(rig.list().size() == 1);
        TestMirror last;
        last.client = other.client.get();
        last.doc = 41;
        other.client->open(41, file, "", &last);
        CHECK(other.client->settle());
        other.client->close(41);
        CHECK(other.client->settle());
        CHECK(!net.host.hasDocument(41) && rig.list().empty());
        CHECK(app.client->takeErrors().empty() && guest.client->takeErrors().empty());
    }

    GROUP("hostbackup:untitled");
    {
        // A buffer with no file holding text is unsaved whatever its history
        // says: the lifetime keeps the host for it, and its backup names a
        // file in the folder that is not there (Untitled-N).
        KeptRig rig;
        putFile(rig.root + "/Untitled-2", "a file of that name\n");
        Net net(true, true);
        net.host.keepBackups(rig.keeping());
        Net::Link& r = net.add(Unit::Utf16, "cli");
        CHECK(r.client->settle());
        TestMirror empty;
        empty.client = r.client.get();
        empty.doc = 70;
        r.client->open(70, "", "", &empty);   // a document before it has a file: nothing kept
        CHECK(r.client->settle() && net.host.busyReason().empty());
        TestMirror draft;
        draft.client = r.client.get();
        draft.doc = 71;
        draft.text = u"draft";
        r.client->open(71, "", "draft", &draft);
        TestMirror second;
        second.client = r.client.get();
        second.doc = 72;
        second.text = u"second";
        r.client->open(72, "", "second", &second);
        CHECK(r.client->settle());
        CHECK(!net.host.dirty(71) && net.host.busyReason() == "a document with no file holds text");
        rig.mono += 1;
        net.host.tick();
        CHECK(net.host.backupPath(71) == rig.root + "/Untitled-1");
        CHECK(net.host.backupPath(72) == rig.root + "/Untitled-3");   // -2 is a file
        CHECK(net.host.backupId(70).empty());
        bool found = false;
        Backups::Entry e;
        CHECK(rig.textOf(rig.root + "/Untitled-1", &found, &e) == "draft" && found);
        CHECK(Backups::compare(e, Backups::posixFs().stat(e.path)) == Backups::Disk::NeverSaved);
        // The host's document keeps no path; the name is the backup's.
        Json st = r.client->sessionState();
        bool pathless = true;
        for (const Json& d : st["documents"].elements()) pathless = pathless && d["path"].asString().empty();
        CHECK(pathless);
        // Named by a window once it is saved there: a path, and saved.
        r.client->setPath(71, rig.root + "/Untitled-1");
        r.client->saved(71);
        CHECK(r.client->settle());
        CHECK(net.host.backupId(71).empty() && rig.list().size() == 1);
    }

    GROUP("hostbackup:restore");
    {
        // Over the socket, as `mutible host` runs it: an app's window types,
        // then detaches with the file unsaved; the backup is the host
        // process's (this one here). Stopped as a signal stops it, with a
        // guest's edit not yet written: written first. The backup is then
        // what a window restores once the host's process is gone.
        ServerRig rig;
        const std::string backups = scratchFolder() + "/Backups";
        Shared::HostOptions o;
        rig.server = std::make_unique<Shared::HostServer>();
        o.folder = rig.proj;
        o.socketDir = rig.dir;
        o.idleMs = 60000;
        o.backupDir = backups;
        std::string why;
        CHECK(rig.server->start(o, &why) == Shared::HostServer::Start::Ok);
        rig.thread = std::thread([&rig] { rig.result = rig.server->run() ? 1 : 0; });
        const std::string file = rig.proj + "/main.c";
        putFile(file, "int x;\n");
        Backups::Store store;
        store.dir = backups;
        store.fs = Backups::posixFs();

        // A renderer over the socket, its Hello's flags set before it.
        auto connect = [&rig](SocketRenderer& r, const char* name, bool app, bool guest) {
            std::string error;
            r.link = Shared::HostLink::open(rig.path(), &error);
            if (!r.link) return false;
            r.client = std::make_unique<Shared::Client>(Model::Unit::Utf16, name);
            r.client->setKeepsBackups(app);
            r.client->setGuest(guest);
            r.link->connect(*r.client);
            r.mirror = TestMirror();
            r.mirror.client = r.client.get();
            return r.client->settle();
        };
        SocketRenderer app;
        CHECK(connect(app, "gtk", true, false));
        const Shared::DocId id = app.client->newId();
        app.mirror.doc = id;
        app.mirror.text = u"int x;\n";
        app.client->open(id, file, "int x;\n", &app.mirror);
        app.mirror.caret = app.mirror.length();
        app.mirror.type(u"int y;\n");
        CHECK(app.client->settle());
        CHECK(store.list().empty());
        app.client->detach(id);
        CHECK(app.client->settle());
        CHECK(waitUntil([&] { return store.list().size() == 1; }));
        std::vector<Backups::Entry> all = store.list();
        CHECK(all.size() == 1);
        if (all.size() == 1) {
            const Backups::Entry& e = all[0];
            CHECK(e.path == file && e.root == rig.proj && e.ownerPid == (int64_t)getpid());
            CHECK(e.ownerStart == Backups::processStart(getpid()));
            // Held by a live process: no window restores it meanwhile.
            const Backups::IsLive live = [](const Backups::Entry& x) {
                return Backups::processStart(x.ownerPid) == x.ownerStart;
            };
            CHECK(Backups::plan(all, rig.proj, {file}, live).items.empty());
            CHECK(Backups::foldersToReopen(all, {}, live).empty());
        }
        // The app goes; a guest types, and the host is stopped before the
        // second has passed.
        app.detach();
        SocketRenderer guest;
        CHECK(connect(guest, "web", false, true));
        if (guest.client) {
            guest.mirror.doc = id;
            guest.client->open(id, file, "", &guest.mirror);
            CHECK(guest.client->settle() && guest.mirror.text == u"int x;\nint y;\n");
            guest.mirror.caret = 0;
            guest.mirror.type(u"// ");
            CHECK(guest.client->settle());
        }
        rig.stop();   // as SIGTERM would: the backup written before the host ends
        guest.detach();
        Backups::Entry got;
        std::string text;
        all = store.list();
        CHECK(all.size() == 1 && store.load(all.empty() ? "" : all[0].id, &got, &text));
        CHECK(text == "// int x;\nint y;\n");
        // Its process gone (as after a kill or a reboot), the folder's next
        // window brings it back, and the folder gets a window at launch.
        const Backups::IsLive gone = [](const Backups::Entry&) { return false; };
        const Backups::Restore r = Backups::plan(all, rig.proj, {}, gone);
        CHECK(r.items.size() == 1 && r.items[0].path == file && !r.items[0].inSession);
        CHECK(Backups::foldersToReopen(all, {}, gone) == std::vector<std::string>{rig.proj});
        CHECK(Backups::compare(got, Backups::posixFs().stat(file)) == Backups::Disk::Same);
    }

    GROUP("reattach:two-of-one-path");
    {
        // Two windows on one file both closed (records 501 and 502, the
        // second the latest), the same file once more with no record, a
        // file no record names, an untitled buffer with text, and a
        // renderer's document from before it had a file.
        Json st = Json::object();
        Json docs = Json::array();
        docs.push(stateDoc(11, "/p/a.txt", 5, true));
        docs.push(stateDoc(12, "/p/a.txt", 6, true));
        docs.push(stateDoc(13, "/p/a.txt", 7, false));
        docs.push(stateDoc(14, "/p/b.txt", 3, false));
        docs.push(stateDoc(15, "", 5, false));
        docs.push(stateDoc(16, "", 0, false));
        docs.push(stateDoc(17, "", 0, true));
        st.set("documents", std::move(docs));
        Json wins = Json::array();
        wins.push(stateRecord(501, 9, false, 3, {11}));
        wins.push(stateRecord(502, 9, false, 4, {12}));
        // This renderer's own report of a window before it took a record:
        // the latest, but naming nothing, so never the one taken.
        wins.push(stateRecord(503, 1, true, 9, {}));
        st.set("windows", std::move(wins));
        st.set("terminals", Json::array());
        std::set<uint64_t> open;
        auto openHere = [&open](Shared::DocId d) { return open.count(d) > 0; };
        // The first window: the latest record and its own a.txt; never the
        // other two of that path; the rest no record names.
        Shared::Reattach first = Shared::reattachFrom(st, 1, {}, openHere, nullptr, false);
        CHECK(first.window == 502);
        CHECK((docIds(first) == std::vector<uint64_t>{12, 14, 15, 17}));
        // The same answer every time.
        CHECK(docIds(Shared::reattachFrom(st, 1, {}, openHere, nullptr, false)) == docIds(first));
        for (uint64_t d : docIds(first)) open.insert(d);
        // The second: the other record, with its own a.txt.
        Shared::Reattach second = Shared::reattachFrom(st, 1, {502}, openHere, nullptr, false);
        CHECK(second.window == 501 && (docIds(second) == std::vector<uint64_t>{11}));
        for (uint64_t d : docIds(second)) open.insert(d);
        // The third: what is left, the copy no record named.
        Shared::Reattach third = Shared::reattachFrom(st, 1, {501, 502}, openHere, nullptr, false);
        CHECK(third.window == 0 && (docIds(third) == std::vector<uint64_t>{13}));
        // A record still open in another renderer keeps its documents.
        Json st2 = st;
        Json wins2 = Json::array();
        wins2.push(stateRecord(501, 9, true, 3, {11}));
        wins2.push(stateRecord(502, 9, false, 4, {12}));
        st2.set("windows", std::move(wins2));
        Shared::Reattach other = Shared::reattachFrom(st2, 1, {}, nullptr, nullptr, false);
        CHECK(other.window == 502 && (docIds(other) == std::vector<uint64_t>{12, 14, 15, 17}));
        // A check's cleanup: everything free, seconds of a path too, never
        // the pathless empty one.
        Shared::Reattach all = Shared::reattachFrom(st, 1, {}, nullptr, nullptr, true);
        CHECK((docIds(all) == std::vector<uint64_t>{11, 12, 13, 14, 15, 17}));
        CHECK((all.records == std::vector<uint64_t>{501, 502, 503}));
    }

    GROUP("reattach:untitled");
    {
        // Through a host: an untitled buffer a renderer left comes back to
        // the next one once, with its text, and never to a second.
        Net net(true, true);
        Net::Link& a = net.add(Unit::Utf16, "cli");
        CHECK(a.client->settle());
        TestMirror m;
        m.client = a.client.get();
        m.doc = 90;
        m.text = u"scratch";
        a.client->open(90, "", "scratch", &m);
        CHECK(a.client->settle());
        a.client->detach(90);
        CHECK(a.client->settle());
        net.host.disconnect(a.id);
        Net::Link& b = net.add(Unit::CodePoint, "gtk", 4);
        CHECK(b.client->settle());
        const Shared::Reattach got = Shared::reattachFrom(
            b.client->sessionState(), b.client->clientId(), {},
            [&b](Shared::DocId d) { return b.client->isOpen(d); }, nullptr, false);
        CHECK(got.docs.size() == 1 && got.docs[0].id == 90 && got.docs[0].path.empty());
        TestMirror back;
        back.client = b.client.get();
        back.doc = 90;
        back.unit = Unit::CodePoint;
        b.client->open(90, "", "", &back);
        CHECK(b.client->settle() && back.text == u"scratch");
        const Shared::Reattach again = Shared::reattachFrom(
            b.client->sessionState(), b.client->clientId(), {},
            [&b](Shared::DocId d) { return b.client->isOpen(d); }, nullptr, false);
        CHECK(again.docs.empty());
    }
}
