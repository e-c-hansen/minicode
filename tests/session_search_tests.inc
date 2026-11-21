// session_search_tests.inc — the `search:*` and `sessionweb:search` groups
// of run_tests.cpp: the session host's heavier reads (SessionSearch.h),
// Quick Open's list of the folder's files and Find in Folder, run on the
// host's workers and answered as they go, held to the session's folder
// and the page's dotfile rule; cancelled, capped in rows and in time; the
// client's streams (SessionClient.h) and a page of the browser's view
// asking for both over the wire (SessionWeb.h). Included there, after
// session_disk_tests.inc (it uses the host tests' Net and fs helpers, the
// server tests' ServerRig and the web tests' PageClient).

#include "SessionSearch.h"

namespace {

// Answers a read gave, in order (emit may come from two threads in turn).
struct Answers {
    std::mutex mu;
    std::vector<Json> all;
    void add(Json&& j) {
        std::lock_guard<std::mutex> lk(mu);
        all.push_back(std::move(j));
    }
    std::vector<Json> copy() {
        std::lock_guard<std::mutex> lk(mu);
        return all;
    }
};

Json searchAsk(const std::string& query, bool hidden = false) {
    Json r = Json::object();
    r.set("op", Json("search"));
    r.set("query", Json(query));
    if (hidden) r.set("hidden", Json(true));
    return r;
}

Json filesAsk(bool hidden = false) {
    Json r = Json::object();
    r.set("op", Json("files"));
    if (hidden) r.set("hidden", Json(true));
    return r;
}

std::vector<Json> runRead(const std::string& root, const Json& request, double seconds = 0,
                          const std::function<void()>& eachFile = nullptr) {
    Answers got;
    std::atomic<bool> cancel{false};
    Shared::runWorkerRead(root, request, cancel, [&](Json&& j) { got.add(std::move(j)); }, seconds, eachFile);
    return got.copy();
}

// Every row of a search's answers as "path:line", in order.
std::string rowsOf(const std::vector<Json>& answers) {
    std::string s;
    for (const Json& a : answers)
        for (const Json& r : a["rows"].elements())
            s += (s.empty() ? "" : " ") + r["path"].asString() + ":" + std::to_string(r["line"].asInt());
    return s;
}

const Json* rowAt(const std::vector<Json>& answers, const std::string& path, int line) {
    for (const Json& a : answers)
        for (const Json& r : a["rows"].elements())
            if (r["path"].asString() == path && r["line"].asInt() == line) return &r;
    return nullptr;
}

std::string joined(const Json& strings) {
    std::string s;
    for (const Json& e : strings.elements()) s += (s.empty() ? "" : " ") + e.asString();
    return s;
}

// The answers a client's stream gave until its last, the host's workers
// pumped meanwhile (a Net's host has no thread of its own).
bool pumpStream(Net& net, Shared::Client& client, std::uint64_t req, std::vector<Json>* out, int ms = 8000) {
    const double end = nowMs() + ms;
    while (nowMs() < end) {
        net.host.waitReads(50);
        client.dispatch();
        if (client.takeStream(req, out)) return true;
    }
    return false;
}

// A folder of the reads' checks, and one outside it that links lead to.
struct SearchFolder {
    std::string root = fsScratch(), elsewhere = fsScratch();
    SearchFolder() {
        fsWrite(root + "/.gitignore", "*.log\nbuild/\n");
        fsWrite(root + "/.env", "needle=secret\n");
        ::mkdir((root + "/.github").c_str(), 0755);
        fsWrite(root + "/.github/ci.yml", "on: push\n");
        fsWrite(root + "/a.py", "print('no')\n");
        fsWrite(root + "/b.log", "needle in a log\n");
        fsWrite(root + "/notes.txt", "alpha\nthe caf\xC3\xA9 needle here\n\xF0\x9F\x98\x80 needle again\nomega\n");
        ::mkdir((root + "/src").c_str(), 0755);
        fsWrite(root + "/src/code.c", "int needle = 1;\nint Needle2;\n");
        fsWrite(root + "/src/.hidden.c", "int needle;\n");
        ::mkdir((root + "/src/deep").c_str(), 0755);
        fsWrite(root + "/src/deep/y.c", "int y;\n");
        ::mkdir((root + "/build").c_str(), 0755);
        fsWrite(root + "/build/out.c", "int needle;\n");
        ::mkdir((root + "/node_modules").c_str(), 0755);
        fsWrite(root + "/node_modules/m.js", "var needle;\n");
        fsWrite(elsewhere + "/secret.txt", "needle outside\n");
        ok = ::symlink((root + "/src/deep").c_str(), (root + "/inlink").c_str()) == 0 &&
             ::symlink(elsewhere.c_str(), (root + "/link-out").c_str()) == 0 &&
             ::symlink((elsewhere + "/secret.txt").c_str(), (root + "/secret-link.txt").c_str()) == 0;
    }
    ~SearchFolder() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::remove_all(elsewhere, ec);
    }
    bool ok = false;
};

// `n` files of `lines` matching lines each, for the slow checks.
std::string manyFiles(int n, int lines) {
    const std::string root = fsScratch();
    for (int i = 0; i < n; i++) {
        std::string text;
        for (int k = 0; k < lines; k++) text += "a needle on line " + std::to_string(k + 1) + "\n";
        char name[32];
        std::snprintf(name, sizeof name, "/f%03d.txt", i);
        fsWrite(root + name, text);
    }
    return root;
}

}  // namespace

static void testSessionSearch() {
    // Quick Open's list, as the host walks it: Find in Folder's skips and
    // the ignore files, nearer files first, held to the folder (a link out
    // neither listed nor entered, one inside followed once), and the page's
    // dotfile rule.
    GROUP("search:files");
    {
        SearchFolder f;
        CHECK(f.ok);
        std::vector<Json> a = runRead(f.root, filesAsk());
        CHECK(a.size() == 1 && a[0]["done"].asBool() && a[0]["op"].asString() == "files");
        CHECK(joined(a[0]["files"]) == "a.py notes.txt inlink/y.c src/code.c");
        if (joined(a[0]["files"]) != "a.py notes.txt inlink/y.c src/code.c")
            std::printf("  search:files: %s\n", a[0].dump().c_str());
        CHECK(!a[0]["truncated"].asBool());
        a = runRead(f.root, filesAsk(true));
        CHECK(joined(a[0]["files"]) ==
              ".env .gitignore a.py notes.txt .github/ci.yml inlink/y.c src/.hidden.c src/code.c");
        if (joined(a[0]["files"]).find("secret") != std::string::npos)
            std::printf("  search:files: the outside is listed: %s\n", a[0].dump().c_str());
        // The Mac's Quick Open (no confine, dotfiles) is as it was: it
        // follows a link out of the folder.
        const std::vector<std::string> mac = CommandPalette::listFiles(f.root);
        CHECK(std::find(mac.begin(), mac.end(), "secret-link.txt") != mac.end());
        CHECK(std::find(mac.begin(), mac.end(), ".env") != mac.end());
        // Ranked as the palette ranks (quickOpenJson): the name before the
        // path, positions in UTF-16 units of the whole path, kinds and views.
        const std::vector<std::string> paths = {"docs/index.html", "src/x.c", "a.py", "README.md", "lib/axc.py"};
        std::vector<std::u16string> paths16;
        for (const std::string& p : paths) paths16.push_back(Model::utf16FromUtf8(p));
        Json q = Shared::Web::quickOpenJson(paths, paths16, "xc", {});
        CHECK(q["rows"].size() == 2 && q["rows"][0]["path"].asString() == "src/x.c" &&
              q["rows"][1]["path"].asString() == "lib/axc.py");
        CHECK(q["rows"][0]["pos"].dump() == "[4,6]" && q["rows"][0]["kind"].asString() == "code" &&
              !q["rows"][0].has("view") && !q["more"].asBool());
        q = Shared::Web::quickOpenJson(paths, paths16, "", {"README.md"}, 2);
        CHECK(q["rows"].size() == 2 && q["rows"][0]["path"].asString() == "README.md" && q["more"].asBool());
        q = Shared::Web::quickOpenJson(paths, paths16, "index", {});
        CHECK(q["rows"].size() == 1 && q["rows"][0]["view"].asString() == "html");
        // On the server's thread for every key typed: 20,000 paths ranked
        // in a few milliseconds.
        std::vector<std::string> big;
        std::vector<std::u16string> big16;
        for (int i = 0; i < 20000; i++) {
            big.push_back("src/module" + std::to_string(i % 97) + "/part" + std::to_string(i) + "/file_" +
                          std::to_string(i) + ".cpp");
            big16.push_back(Model::utf16FromUtf8(big.back()));
        }
        // A name typed: most paths are passed over before any alignment.
        double t0 = cpuMs();
        q = Shared::Web::quickOpenJson(big, big16, "file_1999", {});
        const double typed = cpuMs() - t0;
        CHECK(q["rows"].size() > 0 && q["rows"][0]["path"].asString().find("/file_1999.cpp") != std::string::npos);
        // The worst: a query every path holds, each aligned.
        t0 = cpuMs();
        q = Shared::Web::quickOpenJson(big, big16, "mod12fil", {});
        const double worst = cpuMs() - t0;
        std::printf("  search:files: 20,000 paths ranked in %.1f ms CPU (%.1f ms when every path matches)\n", typed,
                    worst);
        CHECK(q["rows"].size() == Shared::Web::kQuickOpenRows);
        CHECK_TIME(typed, 100);
        CHECK_TIME(worst, 600);
    }

    // Find in Folder's rows: the core's search with the Mac's options, held
    // to the folder and the dotfile rule, matches marked in UTF-16 units of
    // the display text, a line of context either side.
    GROUP("search:rows");
    {
        SearchFolder f;
        CHECK(f.ok);
        std::vector<Json> a = runRead(f.root, searchAsk("needle"));
        CHECK(!a.empty() && a.back()["done"].asBool());
        for (std::size_t i = 0; i + 1 < a.size(); i++) CHECK(!a[i]["done"].asBool());
        CHECK(rowsOf(a) == "notes.txt:2 notes.txt:3 src/code.c:1 src/code.c:2");
        if (rowsOf(a) != "notes.txt:2 notes.txt:3 src/code.c:1 src/code.c:2")
            std::printf("  search:rows: %s\n", rowsOf(a).c_str());
        const Json* cafe = rowAt(a, "notes.txt", 2);
        CHECK(cafe && (*cafe)["text"].asString() == "the caf\xC3\xA9 needle here" &&
              (*cafe)["hl"].dump() == "[[9,6]]" && (*cafe)["column"].asInt() == 10);
        CHECK(cafe && joined((*cafe)["before"]) == "alpha" && (*cafe)["after"].size() == 0);
        const Json* smile = rowAt(a, "notes.txt", 3);
        CHECK(smile && (*smile)["hl"].dump() == "[[3,6]]" && (*smile)["before"].size() == 0 &&
              joined((*smile)["after"]) == "omega");
        const Json& last = a.back();
        CHECK(last["filesMatched"].asInt() == 2 && last["totalMatches"].asInt() == 4 &&
              last["rowCount"].asInt() == 4 && !last["truncated"].asBool() && !last["timedOut"].asBool() &&
              !last["cancelled"].asBool() && !last.has("error"));
        // Nothing outside: not through the folder's link, not the file
        // link, not the ignored log, the build folder or node_modules.
        CHECK(rowsOf(a).find("secret") == std::string::npos && rowsOf(a).find("link-out") == std::string::npos &&
              rowsOf(a).find("b.log") == std::string::npos && rowsOf(a).find("build") == std::string::npos &&
              rowsOf(a).find("node_modules") == std::string::npos);
        // Dotfiles when the tree shows them.
        a = runRead(f.root, searchAsk("needle", true));
        CHECK(rowsOf(a) == ".env:1 notes.txt:2 notes.txt:3 src/.hidden.c:1 src/code.c:1 src/code.c:2");
        // The toggles: case, whole word, a regex; a regex that does not
        // compile; a query too short to search.
        Json ask = searchAsk("needle");
        ask.set("caseSensitive", Json(true));
        CHECK(rowsOf(runRead(f.root, ask)) == "notes.txt:2 notes.txt:3 src/code.c:1");
        ask = searchAsk("needle");
        ask.set("wholeWord", Json(true));
        CHECK(rowsOf(runRead(f.root, ask)) == "notes.txt:2 notes.txt:3 src/code.c:1");
        ask = searchAsk("need(le)\\s+\\w+");
        ask.set("regex", Json(true));
        a = runRead(f.root, ask);
        CHECK(rowsOf(a) == "notes.txt:2 notes.txt:3");
        CHECK(rowAt(a, "notes.txt", 2) && (*rowAt(a, "notes.txt", 2))["hl"].dump() == "[[9,11]]");
        ask = searchAsk("(");
        ask.set("regex", Json(true));
        a = runRead(f.root, ask);
        CHECK(a.size() == 1 && a[0]["done"].asBool() && !a[0]["error"].asString().empty());
        a = runRead(f.root, searchAsk("n"));
        CHECK(a.size() == 1 && a[0]["done"].asBool() && rowsOf(a).empty());
        // Files to include.
        ask = searchAsk("needle");
        ask.set("include", Json("src"));
        CHECK(rowsOf(runRead(f.root, ask)) == "src/code.c:1 src/code.c:2");
        // An op the workers do not have.
        Json bad = Json::object();
        bad.set("op", Json("format-disk"));
        a = runRead(f.root, bad);
        CHECK(a.size() == 1 && a[0]["done"].asBool() && a[0]["error"].asString().find("no read") == 0);
    }

    // Rows go as they are found: batches at most every kBatchMs, in the
    // walk's order, the last with the counts.
    GROUP("search:batches");
    {
        const std::string root = manyFiles(12, 25);
        std::vector<Json> a =
            runRead(root, searchAsk("needle"), 0, [] { std::this_thread::sleep_for(std::chrono::milliseconds(120)); });
        std::size_t batches = 0, rows = 0;
        std::string lastPath;
        bool ordered = true;
        for (const Json& x : a) {
            if (!x["done"].asBool()) batches++;
            for (const Json& r : x["rows"].elements()) {
                rows++;
                if (r["path"].asString() < lastPath) ordered = false;
                lastPath = r["path"].asString();
            }
        }
        std::printf("  search:batches: %zu rows in %zu batches\n", rows, batches);
        CHECK(batches >= 2 && rows == 300 && ordered);
        CHECK(a.back()["done"].asBool() && a.back()["totalMatches"].asInt() == 300 &&
              a.back()["filesMatched"].asInt() == 12);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    // Capped as the app caps it: 20,000 rows; and in time, the rows found
    // so far kept.
    GROUP("search:caps");
    {
        const std::string root = manyFiles(1, (int)Shared::kMaxSearchRows + 50);
        std::vector<Json> a = runRead(root, searchAsk("needle"));
        std::size_t rows = 0;
        for (const Json& x : a) rows += x["rows"].size();
        CHECK(rows == Shared::kMaxSearchRows && a.back()["truncated"].asBool() &&
              a.back()["rowCount"].asInt() == (int64_t)Shared::kMaxSearchRows);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        const std::string slow = manyFiles(40, 2);
        const double t0 = nowMs();
        a = runRead(slow, searchAsk("needle"), 0.15, [] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); });
        const double took = nowMs() - t0;
        std::printf("  search:caps: stopped after %.0f ms, %lld files searched\n", took,
                    (long long)a.back()["filesSearched"].asInt());
        CHECK(a.back()["done"].asBool() && a.back()["timedOut"].asBool() && !a.back()["cancelled"].asBool());
        CHECK(a.back()["filesSearched"].asInt() < 40);
        CHECK(took < 3000);
        std::filesystem::remove_all(slow, ec);
    }

    // Cancelled mid-walk: the last answer says so, what was found kept.
    GROUP("search:cancel");
    {
        const std::string root = manyFiles(30, 1);
        Answers got;
        std::atomic<bool> cancel{false};
        std::thread t([&] {
            Shared::runWorkerRead(root, searchAsk("needle"), cancel, [&](Json&& j) { got.add(std::move(j)); }, 0,
                                  [] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); });
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        cancel = true;
        t.join();
        const std::vector<Json> a = got.copy();
        std::size_t dones = 0;
        for (const Json& x : a) dones += x["done"].asBool() ? 1 : 0;
        CHECK(dones == 1 && a.back()["done"].asBool() && a.back()["cancelled"].asBool());
        CHECK(a.back()["filesSearched"].asInt() < 30);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    // The host: its Welcome offers the reads (flag 8), they run on its
    // workers and answer as they go, a reader may ask, a cancel stops one
    // and the client drops what still comes, a renderer that goes has its
    // reads stopped, eight at once and no more, and none without a folder.
    GROUP("search:host");
    {
        SearchFolder f;
        CHECK(f.ok);
        Net net(true, true);
        net.host.setFolder(f.root);
        Net::Link& g = net.add(Unit::Utf16, "browser", 2);
        Net::Link& r = net.add(Unit::Utf16, "reader", 1);
        g.client->dispatch();   // the Welcome
        r.client->dispatch();
        CHECK(g.client->hostSearches() && r.client->hostSearches());
        std::vector<Json> got;
        std::uint64_t req = g.client->requestStream(0, filesAsk());
        CHECK(req && pumpStream(net, *g.client, req, &got));
        CHECK(got.size() == 1 && joined(got[0]["files"]) == "a.py notes.txt inlink/y.c src/code.c");
        got.clear();
        req = r.client->requestStream(0, searchAsk("needle"));
        CHECK(req && pumpStream(net, *r.client, req, &got));
        CHECK(rowsOf(got) == "notes.txt:2 notes.txt:3 src/code.c:1 src/code.c:2");
        CHECK(r.client->takeErrors().empty() && r.client->streams() == 0);

        // Cancelled: the client takes nothing more, and forgets the request
        // once its last answer comes.
        const std::string many = manyFiles(30, 1);
        Net slow(true, true);
        slow.host.setFolder(many);
        slow.host.readWorkers().eachFile = [] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); };
        Net::Link& s = slow.add(Unit::Utf16, "browser", 2);
        s.client->dispatch();
        req = s.client->requestStream(0, searchAsk("needle"));
        CHECK(req && slow.host.readsRunning() == 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        s.client->cancelStream(req, 0);
        CHECK(waitUntil([&] {
            slow.host.waitReads(20);
            s.client->dispatch();
            return slow.host.readsRunning() == 0 && s.client->streams() == 0;
        }, 5000));
        got.clear();
        CHECK(!s.client->takeStream(req, &got) && got.empty());
        // A renderer that goes: its read stops, and nothing is sent to it.
        Net::Link& gone = slow.add(Unit::Utf16, "browser", 2);
        gone.client->dispatch();
        req = gone.client->requestStream(0, searchAsk("needle"));
        CHECK(slow.host.readsRunning() == 1);
        slow.host.disconnect(gone.id);
        const double t0 = nowMs();
        CHECK(waitUntil([&] {
            slow.host.waitReads(20);
            return slow.host.readsRunning() == 0;
        }, 5000));
        std::printf("  search:host: a gone renderer's search stopped in %.0f ms\n", nowMs() - t0);
        // Eight at once, then a refusal at once.
        std::vector<std::uint64_t> reqs;
        for (std::size_t i = 0; i < Shared::kMaxReadJobs; i++) reqs.push_back(s.client->requestStream(0, searchAsk("needle")));
        CHECK(slow.host.readsRunning() == Shared::kMaxReadJobs);
        got.clear();
        req = s.client->requestStream(0, searchAsk("needle"));
        s.client->dispatch();
        CHECK(s.client->takeStream(req, &got) && got.size() == 1 &&
              got[0]["error"].asString().find("too many") == 0);
        for (std::uint64_t q : reqs) s.client->cancelStream(q, 0);
        CHECK(waitUntil([&] {
            slow.host.waitReads(20);
            s.client->dispatch();
            return slow.host.readsRunning() == 0 && s.client->streams() == 0;
        }, 8000));
        std::error_code ec;
        std::filesystem::remove_all(many, ec);
        // An app's own host with no folder for the window named.
        Net app(true, true);
        Net::Link& w = app.add(Unit::Utf16, "browser", 2);
        w.client->dispatch();
        got.clear();
        req = w.client->requestStream(6, searchAsk("needle"));
        w.client->dispatch();
        CHECK(w.client->takeStream(req, &got) && got.size() == 1 &&
              got[0]["error"].asString() == "no folder to read in");
        // A host from before them (no Welcome flag 8): nothing is sent.
        std::vector<Message> sent;
        Shared::Client old(Unit::Utf16, "page");
        old.connect([&](Message&& m) { sent.push_back(std::move(m)); });
        Message welcome;
        welcome.kind = Shared::Kind::Welcome;
        welcome.client = 1;
        welcome.v = Shared::kIdRange;
        welcome.flag = 1 | 2 | 4;
        old.deliver(std::move(welcome));
        old.dispatch();
        const std::size_t before = sent.size();
        CHECK(!old.hostSearches() && old.requestStream(0, searchAsk("needle")) == 0 && sent.size() == before);
        // The answers through the encoding, several for one request.
        Message d;
        d.kind = Shared::Kind::FsData;
        d.req = 9;
        d.json = searchAsk("x");
        d.json.set("done", Json(false));
        Message back;
        CHECK(Shared::decode(Shared::encode(d), &back) && back.kind == Shared::Kind::FsData && back.req == 9 &&
              back.json == d.json);
    }

    // A page of the browser's view over the wire, read-only: Quick Open's
    // files ranked from the host's listing, a search's rows as they come,
    // a new search replacing the last, a short query, and nothing outside.
    GROUP("sessionweb:search");
    {
        ServerRig rig;
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        const std::string root = rig.proj, elsewhere = scratchFolder();
        writeText(root + "/.gitignore", "*.log\n");
        writeText(root + "/main.py", "def needle():\n    return 1\n");
        writeText(root + "/run.log", "needle\n");
        ::mkdir((root + "/src").c_str(), 0755);
        writeText(root + "/src/util.c", "int alpha;\nint needle;\n");
        writeText(elsewhere + "/secret.txt", "needle outside\n");
        CHECK(::symlink(elsewhere.c_str(), (root + "/away").c_str()) == 0);
        Web::Server web;
        Web::Options o;
        o.hostSocket = rig.path();
        o.folder = root;
        o.pollMs = 100;
        std::string error;
        CHECK(web.start(o, &error));
        std::thread webThread([&] { web.run(); });
        {
            PageClient p;
            CHECK(p.open(web));
            Json state, hello;
            std::map<std::int64_t, Json> files;
            std::map<std::int64_t, std::vector<Json>> found;
            auto take = [&](const Json& m) {
                const std::string t = m["t"].asString();
                if (t == "state") state = m;
                else if (t == "hello") hello = m;
                else if (t == "files") files[m["seq"].asInt()] = m;
                else if (t == "found") found[m["seq"].asInt()].push_back(m);
            };
            CHECK(p.pump(take, [&] { return state.isObject(); }));
            CHECK(hello["canSearch"].asBool() && !hello["editable"].asBool());
            auto listed = [&](std::int64_t seq) {
                return files.count(seq) && !files[seq]["listing"].asBool() && files[seq]["rows"].size() > 0;
            };
            p.sendText("{\"t\":\"files\",\"seq\":1,\"query\":\"\",\"fresh\":true}");
            CHECK(p.pump(take, [&] { return listed(1); }));
            std::string names;
            for (const Json& r : files[1]["rows"].elements()) names += (names.empty() ? "" : " ") + r["path"].asString();
            CHECK(names == "main.py src/util.c");
            p.sendText("{\"t\":\"files\",\"seq\":2,\"query\":\"utc\"}");
            CHECK(p.pump(take, [&] { return files.count(2) > 0; }));
            CHECK(files[2]["rows"].size() == 1 && files[2]["rows"][0]["path"].asString() == "src/util.c" &&
                  files[2]["query"].asString() == "utc");
            auto finished = [&](std::int64_t seq) {
                return found.count(seq) && !found[seq].empty() && found[seq].back()["done"].asBool();
            };
            p.sendText("{\"t\":\"search\",\"seq\":1,\"query\":\"needle\"}");
            CHECK(p.pump(take, [&] { return finished(1); }));
            CHECK(rowsOf(found[1]) == "main.py:1 src/util.c:2");
            const Json* row = rowAt(found[1], "src/util.c", 2);
            CHECK(row && joined((*row)["before"]) == "int alpha;" && (*row)["hl"].dump() == "[[4,6]]");
            // A new search replaces the last one; the page asks with its
            // own numbers.
            p.sendText("{\"t\":\"search\",\"seq\":2,\"query\":\"needle\",\"caseSensitive\":true}");
            p.sendText("{\"t\":\"search\",\"seq\":3,\"query\":\"alpha\"}");
            CHECK(p.pump(take, [&] { return finished(3); }));
            CHECK(rowsOf(found[3]) == "src/util.c:1");
            CHECK(!finished(2) || rowsOf(found[2]) == "main.py:1 src/util.c:2");
            p.sendText("{\"t\":\"search\",\"seq\":4,\"query\":\"n\"}");
            CHECK(p.pump(take, [&] { return finished(4); }));
            CHECK(found[4].back()["short"].asBool() && rowsOf(found[4]).empty());
            p.sendText("{\"t\":\"search\",\"seq\":5,\"cancel\":true}");
            // Nothing outside the folder or ignored, in either.
            for (const auto& [seq, answers] : found) CHECK(rowsOf(answers).find("away") == std::string::npos &&
                                                           rowsOf(answers).find("run.log") == std::string::npos);
            p.close();
            CHECK(waitUntil([&] { return web.browsers() == 0; }));
        }
        web.stop();
        webThread.join();
        std::error_code ec;
        std::filesystem::remove_all(elsewhere, ec);
    }
}
