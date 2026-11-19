// backups_tests.inc — the `backups:*` group of run_tests.cpp (Backups.h),
// kept in its own file and included there.

#include <ctime>

static void testBackups() {
    using namespace Backups;
    namespace fsys = std::filesystem;

    GROUP("backups:line-endings");
    CHECK(lineEndingsOf("") == "none");
    CHECK(lineEndingsOf("abc") == "none");
    CHECK(lineEndingsOf("a\nb\n") == "lf");
    CHECK(lineEndingsOf("a\r\nb\r\n") == "crlf");
    CHECK(lineEndingsOf("a\rb\r") == "cr");
    CHECK(lineEndingsOf("a\r\nb\n") == "mixed");
    CHECK(lineEndingsOf("a\r\n") == "crlf");

    GROUP("backups:ids");
    CHECK(validId("3F2A-11-abc"));
    CHECK(!validId(""));
    CHECK(!validId("../x"));
    CHECK(!validId("a/b"));
    CHECK(!validId("a.b"));
    CHECK(!validId(std::string(65, 'a')));
    CHECK(fileName("abc") == "abc.mcbackup");

    GROUP("backups:format");
    {
        Entry e;
        e.id = "A1-B2";
        e.path = "/p/odd name\nwith%newline.txt";
        e.root = "/p";
        e.diskExisted = true;
        e.diskMtime = 1730000000.25;
        e.diskInode = 123456789012ull;
        e.saved = 1730000100.5;
        e.ownerPid = 4242;
        e.ownerStart = 1729999999.0;
        const std::string text = "line one\r\nline two\r\n\n\nmutible-backup 1\n";
        const std::string data = encode(e, text);
        CHECK(data.rfind("mutible-backup 1\n", 0) == 0);
        Entry back;
        std::string t;
        CHECK(decode(data, &back, &t));
        CHECK(t == text);
        CHECK(back.id == e.id);
        CHECK(back.path == e.path);
        CHECK(back.root == "/p");
        CHECK(back.diskExisted);
        CHECK(std::fabs(back.diskMtime - e.diskMtime) < 1e-6);
        CHECK(back.diskInode == e.diskInode);
        CHECK(back.encoding == "utf-8");
        CHECK(back.fileEncoding.empty());   // none given, none written
        CHECK(data.find("file-encoding") == std::string::npos);
        Entry wide = e;
        wide.fileEncoding = "utf16be";
        CHECK(decode(encode(wide, text), &back, &t) && back.fileEncoding == "utf16be");
        CHECK(back.lineEndings == "mixed");
        CHECK(std::fabs(back.saved - e.saved) < 1e-6);
        CHECK(back.ownerPid == 4242);
        CHECK(back.bytes == text.size());
        // The header alone, from a prefix of the file.
        Entry head;
        size_t start = 0;
        CHECK(decodeHeader(data.substr(0, data.size() - text.size()), &head, &start));
        CHECK(start == data.size() - text.size());
        CHECK(head.path == e.path);
        // A file cut short, or with more than it says, is refused.
        CHECK(!decode(data.substr(0, data.size() - 1), &back, &t));
        CHECK(!decode(data + "x", &back, &t));
        CHECK(!decode("not a backup\n\nhello", &back, &t));
        // A backup from before 1.6.0, with the old magic, as the first launch copies it.
        const std::string old = "minicode-backup 1\n" + data.substr(std::string("mutible-backup 1\n").size());  // rename-mutible: keep
        CHECK(decode(old, &back, &t) && t == text && back.path == e.path);
        CHECK(decodeHeader(old.substr(0, old.size() - text.size()), &head, &start));
        CHECK(start == old.size() - text.size());
        CHECK(!decodeHeader("mutible-backup 1\nid: x\n", &head));   // no blank line
        CHECK(!decodeHeader("mutible-backup 1\nid: ../x\npath: /a\nbytes: 0\n\n", &head));
        CHECK(!decodeHeader("mutible-backup 1\nid: x\nbytes: 0\n\n", &head));   // no path
        // Empty text, and unknown keys skipped.
        Entry z;
        z.id = "z";
        z.path = "/z";
        std::string zd = encode(z, "");
        zd.insert(zd.find("bytes:"), "future-key: whatever\n");
        CHECK(decode(zd, &back, &t) && t.empty() && back.lineEndings == "none");
        CHECK(!back.diskExisted);
    }

    GROUP("backups:compare");
    {
        Entry e;
        e.diskExisted = true;
        e.diskMtime = 100.5;
        e.diskInode = 7;
        Stat s;
        s.exists = true;
        s.regular = true;
        s.mtime = 100.5;
        s.inode = 7;
        CHECK(compare(e, s) == Disk::Same);
        s.mtime = 100.5004;   // within NSDate's rounding
        CHECK(compare(e, s) == Disk::Same);
        s.mtime = 101;
        CHECK(compare(e, s) == Disk::Changed);
        s.mtime = 100.5;
        s.inode = 8;          // replaced by an atomic save elsewhere
        CHECK(compare(e, s) == Disk::Changed);
        s.exists = false;
        CHECK(compare(e, s) == Disk::Gone);
        e.diskExisted = false;
        CHECK(compare(e, s) == Disk::NeverSaved);
        s.exists = true;
        CHECK(compare(e, s) == Disk::Appeared);
    }

    // A scratch folder through the real POSIX Fs.
    std::random_device rd;
    const fsys::path base = fsys::temp_directory_path() /
                            ("mutible-backups-" + std::to_string(rd()));
    const std::string dir = (base / "Backups").string();   // made on first write

    GROUP("backups:store");
    {
        Store st{dir, posixFs()};
        CHECK(st.list().empty());                 // no folder yet
        Entry e;
        e.id = "tab-1";
        e.path = "/proj/a.py";
        e.root = "/proj";
        e.saved = 1000;
        CHECK(st.save(e, "print(1)\n"));
        struct stat sb;
        CHECK(::stat(dir.c_str(), &sb) == 0 && (sb.st_mode & 0777) == 0700);
        CHECK(::stat(st.pathFor("tab-1").c_str(), &sb) == 0 && (sb.st_mode & 0777) == 0600);
        std::vector<Entry> l = st.list();
        CHECK(l.size() == 1 && l[0].path == "/proj/a.py" && l[0].bytes == 9);
        Entry got;
        std::string text;
        CHECK(st.load("tab-1", &got, &text) && text == "print(1)\n");
        // Written again over itself, atomically, with no temporary left.
        CHECK(st.save(e, "print(2)\n"));
        CHECK(st.load("tab-1", &got, &text) && text == "print(2)\n");
        int names = 0;
        for (const auto &ent : fsys::directory_iterator(dir)) { (void)ent; names++; }
        CHECK(names == 1);
        // A large buffer, byte for byte.
        std::string big(5 * 1024 * 1024, 'x');
        for (size_t i = 0; i < big.size(); i += 81) big[i] = '\n';
        e.id = "tab-big";
        CHECK(st.save(e, big));
        CHECK(st.load("tab-big", &got, &text) && text == big);
        CHECK(st.list().size() == 2);             // headers only, still two
        CHECK(st.discard("tab-big"));
        CHECK(st.discard("tab-big"));             // already gone is fine
        CHECK(!st.load("tab-big", &got, &text));
        CHECK(!st.discard("../escape"));
        CHECK(!st.save(Entry{}, "x"));            // no id, nothing written
        // Debris: a stale temporary, a backup that does not read, others kept.
        const std::string tmp = dir + "/.tab-1.mcbackup.1.1.tmp";
        const std::string bad = dir + "/bad.mcbackup";
        { std::ofstream(tmp) << "half"; }
        { std::ofstream(bad) << "garbage"; }
        { std::ofstream(dir + "/notes.txt") << "not ours"; }
        CHECK(st.list().size() == 1);
        const double now = (double)std::time(nullptr);
        CHECK(st.sweepDebris(now, 3600, 3600) == 0);    // all fresh
        CHECK(st.sweepDebris(now + 7200, 3600, 1e9) == 1);   // the temporary
        CHECK(!fsys::exists(tmp) && fsys::exists(bad));
        CHECK(st.sweepDebris(now + 7200, 3600, 3600) == 1);  // the bad one
        CHECK(!fsys::exists(bad) && fsys::exists(dir + "/notes.txt") &&
              fsys::exists(st.pathFor("tab-1")));
        // A header in a file cut short still lists (the text is checked on load).
        { std::ofstream(dir + "/cut.mcbackup") << encode(e, "abcdef").substr(0, 200); }
        bool listed = false;
        for (const Entry &x : st.list()) listed = listed || x.id == "cut";
        CHECK(!listed);   // its id inside says tab-big, not cut
        std::string cut = encode(Entry{"cut", "/proj/c", "/proj"}, "abcdef");
        { std::ofstream(dir + "/cut.mcbackup") << cut.substr(0, cut.size() - 2); }
        listed = false;
        for (const Entry &x : st.list()) listed = listed || x.id == "cut";
        CHECK(listed);
        CHECK(!st.load("cut", &got, &text));
        // Disk state through the real stat.
        const std::string file = (base / "file.txt").string();
        { std::ofstream(file) << "on disk"; }
        Stat s = st.fs.stat(file);
        CHECK(s.exists && s.regular && s.inode != 0 && s.mtime > 0);
        Entry d;
        d.diskExisted = true;
        d.diskMtime = s.mtime;
        d.diskInode = s.inode;
        CHECK(compare(d, st.fs.stat(file)) == Disk::Same);
        fsys::remove(file);
        CHECK(compare(d, st.fs.stat(file)) == Disk::Gone);
    }
    std::error_code ec;
    fsys::remove_all(base, ec);

    GROUP("backups:fake-fs");
    {
        // Failures from below are reported, not hidden.
        std::map<std::string, std::string> files;
        bool failWrites = false;
        Fs f;
        f.list = [&](const std::string &d) {
            std::vector<std::string> v;
            for (auto &kv : files)
                if (kv.first.rfind(d + "/", 0) == 0) v.push_back(kv.first.substr(d.size() + 1));
            return v;
        };
        f.read = [&](const std::string &p, std::string *out, size_t max) {
            auto it = files.find(p);
            if (it == files.end()) return false;
            *out = max ? it->second.substr(0, max) : it->second;
            return true;
        };
        f.writeAtomic = [&](const std::string &p, const std::string &data) {
            if (failWrites) return false;
            files[p] = data;
            return true;
        };
        f.remove = [&](const std::string &p) { files.erase(p); return true; };
        f.stat = [&](const std::string &p) {
            Stat s;
            s.exists = files.count(p) > 0;
            s.regular = s.exists;
            return s;
        };
        Store st{"/b", f};
        Entry e{"one", "/r/x.txt", "/r"};
        CHECK(st.save(e, "hello"));
        CHECK(files.count("/b/one.mcbackup") == 1);
        failWrites = true;
        CHECK(!st.save(e, "changed"));
        std::string text;
        Entry got;
        CHECK(st.load("one", &got, &text) && text == "hello");   // the old one stands
    }

    GROUP("backups:plan");
    {
        auto mk = [](std::string id, std::string path, std::string root, double saved,
                     int64_t pid = 1) {
            Entry e;
            e.id = id;
            e.path = path;
            e.root = root;
            e.saved = saved;
            e.ownerPid = pid;
            return e;
        };
        const IsLive dead = [](const Entry &) { return false; };
        const IsLive pid9 = [](const Entry &e) { return e.ownerPid == 9; };
        std::vector<Entry> all = {
            mk("a", "/r/a.txt", "/r", 10),
            mk("b", "/r/b.txt", "/r", 30),
            mk("c", "/r/c.txt", "/r", 20),       // not in the session
            mk("d", "/r/d.txt", "/r", 40),       // not in the session, newer
            mk("o", "/other/x.txt", "/other", 50),
            mk("b2", "/r/b.txt", "/r", 5),       // an older backup of b
            mk("l", "/r/live.txt", "/r", 60, 9), // held by a running process
        };
        Restore r = plan(all, "/r", {"/r/b.txt", "/r/gone.txt", "/r/a.txt", "/r/b.txt"}, dead);
        CHECK(r.items.size() == 5);
        if (r.items.size() == 5) {
            CHECK(r.items[0].path == "/r/b.txt" && r.items[0].id == "b" && r.items[0].inSession);
            CHECK(r.items[1].path == "/r/a.txt" && r.items[1].inSession);
            CHECK(r.items[2].path == "/r/live.txt" && !r.items[2].inSession);   // dead here
            CHECK(r.items[3].path == "/r/d.txt" && !r.items[3].inSession);
            CHECK(r.items[4].path == "/r/c.txt");
        }
        // The older backup of b is not deleted: a draft of its own.
        CHECK(r.conflicts.size() == 1 && r.conflicts[0].id == "b2" &&
              r.conflicts[0].path == "/r/b.txt");
        r = plan(all, "/r", {}, pid9);
        CHECK(r.items.size() == 4);   // live.txt left to its owner
        for (auto &it : r.items) CHECK(it.path != "/r/live.txt" && !it.inSession);
        CHECK(plan(all, "/nowhere", {"/r/a.txt"}, dead).items.empty());
        // Ties between two backups of one file: one in the tab, the other
        // a conflict, neither lost.
        r = plan({mk("x1", "/r/t", "/r", 1), mk("x2", "/r/t", "/r", 1)}, "/r", {}, dead);
        CHECK(r.items.size() == 1 && r.conflicts.size() == 1 &&
              r.items[0].id != r.conflicts[0].id);

        GROUP("backups:plan-windows");
        {
            // Round 10's workspace item 1 (probe-core/p3): two windows on
            // one folder, a.txt unsaved in both with other text, quit with
            // hot exit. The first window to come back took the newer
            // backup and deleted the other, and took the second window's
            // b.txt too. Now each window takes its own.
            auto in = [&](std::string id, std::string path, double saved, std::string w) {
                Entry e = mk(id, path, "/proj", saved);
                e.window = w;
                return e;
            };
            const std::string k1 = "11111111-1111-1111-1111-111111111111";
            const std::string k2 = "22222222-2222-2222-2222-222222222222";
            const std::vector<Entry> quit = {in("A1", "/proj/a.txt", 1, k1),
                                             in("B2", "/proj/a.txt", 2, k2),
                                             in("C3", "/proj/b.txt", 3, k2)};
            // Window 1 comes back while window 2 is still to: its own a.txt,
            // though older; window 2's backups are left for it.
            Restore w1 = plan(quit, "/proj", {"/proj/a.txt"}, dead, k1, {k1, k2});
            CHECK(w1.items.size() == 1 && w1.items[0].id == "A1" && w1.conflicts.empty());
            // Window 2 then (window 1's now held, so live).
            const IsLive held = [](const Entry &e) { return e.id == "A1"; };
            Restore w2 = plan(quit, "/proj", {"/proj/a.txt", "/proj/b.txt"}, held, k2, {});
            CHECK(w2.items.size() == 2 && w2.items[0].id == "B2" && w2.items[1].id == "C3" &&
                  w2.conflicts.empty());
            // A window not among those coming back (a launch with a folder
            // named, the record gone) takes every backup of the folder:
            // the newest a.txt in the tab, the other as a conflict.
            Restore any = plan(quit, "/proj", {}, dead, "", {});
            CHECK(any.items.size() == 2 && any.conflicts.size() == 1);
            for (const auto &it : any.items)
                if (it.path == "/proj/a.txt") CHECK(it.id == "B2");
            CHECK(any.conflicts.size() == 1 && any.conflicts[0].id == "A1");
            // Its own comes first even when another's is newer.
            Restore own = plan(quit, "/proj", {}, dead, k1, {});
            CHECK(own.conflicts.size() == 1 && own.conflicts[0].id == "B2");
            for (const auto &it : own.items)
                if (it.path == "/proj/a.txt") CHECK(it.id == "A1");
            // Backups from before the key (no window) go to the first window.
            Restore old = plan({in("O1", "/proj/a.txt", 1, ""), in("O2", "/proj/a.txt", 2, k2)},
                               "/proj", {}, dead, k1, {k1, k2});
            CHECK(old.items.size() == 1 && old.items[0].id == "O1" && old.conflicts.empty());
            // The key survives the header; a bad one is dropped.
            Entry e = in("id-1", "/proj/a.txt", 1, k2);
            Entry back;
            std::string text;
            CHECK(decode(encode(e, "x"), &back, &text) && back.window == k2);
            e.window = "../x";
            CHECK(decode(encode(e, "x"), &back, &text) && back.window.empty());
        }

        GROUP("backups:draft-path");
        {
            std::set<std::string> there = {"/p/a (unsaved copy).txt"};
            auto taken = [&there](const std::string &p) { return there.count(p) > 0; };
            CHECK(draftPath("/p/a.txt", nullptr) == "/p/a (unsaved copy).txt");
            CHECK(draftPath("/p/a.txt", taken) == "/p/a (unsaved copy 2).txt");
            CHECK(draftPath("/p/Makefile", taken) == "/p/Makefile (unsaved copy)");
            CHECK(draftPath("/p/.env", taken) == "/p/.env (unsaved copy)");
            CHECK(draftPath("/p.d/x", taken) == "/p.d/x (unsaved copy)");
        }

        GROUP("backups:reopen");
        std::vector<std::string> f = foldersToReopen(all, {"/r"}, dead);
        CHECK(f == std::vector<std::string>{"/other"});
        f = foldersToReopen(all, {}, pid9);
        CHECK(f == (std::vector<std::string>{"/other", "/r"}));   // newest first
        f = foldersToReopen({mk("l", "/q/l", "/q", 1, 9)}, {}, pid9);
        CHECK(f.empty());
        CHECK(foldersToReopen({mk("e", "/q/l", "", 1)}, {}, dead).empty());

        GROUP("backups:expired");
        const double day = 24 * 3600;
        std::vector<Entry> aged = {mk("old", "/r/1", "/r", 0), mk("new", "/r/2", "/r", 29 * day),
                                   mk("oldlive", "/r/3", "/r", 0, 9)};
        std::vector<std::string> x = expired(aged, 31 * day, kMaxAge, pid9);
        CHECK(x == std::vector<std::string>{"old"});
        CHECK(expired(aged, 29.5 * day, kMaxAge, dead).empty());
    }
}
