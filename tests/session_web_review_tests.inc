// session_web_review_tests.inc — the `sessionweb:pictures` and
// `sessionweb:review-pages` groups of run_tests.cpp (R3's last slice): the
// pictures of a review (their sizes from the header, their addresses), the
// window's commit and worktree pages as it reports them and the server
// checks them, read against a real git in a scratch repository with an
// agent's worktree, then over the wire with a host in this process: the
// page's ids, the reads, the pictures' bytes over HTTP, and the refusals.
// Uses session_web_tests.inc's PageClient, writeText and helpers. Included
// there, after it.

static void testSessionWebReviewPages() {
    using Shared::Message;
    GROUP("sessionweb:pictures");
    auto be32 = [](std::uint32_t v) {
        std::string s(4, '\0');
        for (int i = 0; i < 4; i++) s[(std::size_t)i] = (char)((v >> (24 - 8 * i)) & 0xFF);
        return s;
    };
    // A PNG's header, then bytes no reader looks at.
    auto png = [&](std::uint32_t w, std::uint32_t h, const std::string& rest) {
        return std::string("\x89PNG\r\n\x1a\n", 8) + std::string("\0\0\0\x0D", 4) + "IHDR" + be32(w) + be32(h) +
               std::string("\x08\x02\0\0\0", 5) + rest;
    };
    {
        auto size = [](const std::string& b) {
            const ChangeReview::ImageSide s = Web::pictureSize(b);
            return std::to_string(s.width) + "x" + std::to_string(s.height);
        };
        CHECK(size(png(640, 480, "x")) == "640x480");
        CHECK(size(png(640, 480, "x").substr(0, 20)) == "0x0");   // cut inside the header
        CHECK(size(png(0, 480, "x")) == "0x0");
        CHECK(size(png(0x80000000u, 2, "x")) == "0x0");
        CHECK(size(std::string("GIF89a\x20\x00\x10\x00", 10) + "rest") == "32x16");
        CHECK(size(std::string("GIF87a\x01\x00\x01\x00", 10)) == "1x1");
        // A JPEG: an APP0 segment, then a baseline start of frame.
        const std::string jpeg = std::string("\xFF\xD8\xFF\xE0\x00\x04\x4A\x46", 8) +
                                 std::string("\xFF\xC0\x00\x0B\x08\x00\x64\x00\xC8\x01\x01\x11\x00", 13);
        CHECK(size(jpeg) == "200x100");
        CHECK(size(std::string("\xFF\xD8\xFF\xDA\x00\x04xx", 8)) == "0x0");   // scan data first
        std::string vp8x = "RIFF" + std::string(4, '\0') + "WEBPVP8X" + std::string(8, '\0');
        vp8x += std::string("\x0F\x00\x00\x07\x00\x00", 6);   // 16 by 8, less one each
        vp8x += std::string(8, '\0');
        CHECK(size(vp8x) == "16x8");
        const std::string bmp = "BM" + std::string(12, '\0') + std::string("\x28\0\0\0", 4) +
                                std::string("\x03\0\0\0", 4) + std::string("\xFE\xFF\xFF\xFF", 4) + std::string(8, '\0');
        CHECK(size(bmp) == "3x2");   // stored top-down: a negative height
        const std::string ico = std::string("\0\0\1\0\2\0", 6) + std::string("\x10\x10", 2) + std::string(14, '\0') +
                                std::string("\0\0", 2) + std::string(14, '\0');
        CHECK(size(ico) == "256x256");   // the largest entry; 0 is 256
        CHECK(size("not a picture at all, just words") == "0x0");
        CHECK(size("") == "0x0");
        CHECK(size(std::string("II*\0", 4) + std::string(40, 'x')) == "0x0");   // a TIFF: not shown here
    }
    {
        const std::string base = "/v/0123456789abcdef/";
        CHECK(Web::pictureTarget(base, 3, 17, 2, true) == base + "picture/3/17/2/new");
        int page = -1;
        std::uint64_t version = 0;
        std::size_t file = 0;
        bool isNew = false;
        CHECK(Web::pictureParts("3/17/2/new", &page, &version, &file, &isNew) && page == 3 && version == 17 &&
              file == 2 && isNew);
        CHECK(Web::pictureParts("0/1/0/old", &page, &version, &file, &isNew) && page == 0 && !isNew);
        for (const char* bad : {"3/17/2/mid", "3/17/2/new/x", "3/17/new", "a/1/2/old", "-1/1/2/old", "3//2/old",
                                "../1/2/old", "3/17/2/", "", "3/17/2/old?x", "99999999999999999999/1/1/old",
                                "3/1/2/NEW"})
            CHECK(!Web::pictureParts(bad, &page, &version, &file, &isNew));
        // The answer for /picture/: the credential's cookie, as for /file/.
        Web::Access access;
        const Web::Request r =
            Web::readRequest("GET " + access.base() + "picture/0/1/0/old HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
        Web::Caller from;
        const Web::Answer a = Web::answer(r, access, "page", from);
        CHECK(a.kind == Web::Answer::Refused && a.status == 403);
    }

    GROUP("sessionweb:review-pages");
    {
        // The window's report, and the server's check of it.
        std::vector<Web::ReviewPage> pages;
        Web::ReviewPage c;
        c.kind = "commit";
        c.hash = std::string(40, 'a');
        c.title = "Fix the thing";
        c.onShow = true;
        pages.push_back(c);
        Web::ReviewPage w;
        w.kind = "worktree";
        w.path = "/tmp/wt";
        w.title = "wt vs main";
        pages.push_back(w);
        Web::ReviewPage bad = c;
        bad.hash = "HEAD";
        pages.push_back(bad);
        const Json report = Web::reviewsReportJson(pages);
        CHECK(report.size() == 2 && report[0]["hash"].asString() == c.hash && !report[0].has("path"));
        CHECK(report[1]["path"].asString() == "/tmp/wt" && report[0]["onShow"].asBool());
        std::vector<Web::ReviewPage> back = Web::reviewPagesFrom(report);
        CHECK(back.size() == 2 && back[0].id() == "commit:" + c.hash && back[1].id() == "worktree:/tmp/wt");
        Json hostile;
        CHECK(Json::parse("[{\"kind\":\"commit\",\"hash\":\"--output=/tmp/x\"},"
                          "{\"kind\":\"commit\",\"hash\":\"HEAD~1\"},"
                          "{\"kind\":\"commit\",\"hash\":\"abc\"},"
                          "{\"kind\":\"commit\",\"hash\":\"" + std::string(65, 'a') + "\"},"
                          "{\"kind\":\"worktree\",\"path\":\"../elsewhere\"},"
                          "{\"kind\":\"worktree\",\"path\":\"/tmp/a\\nb\"},"
                          "{\"kind\":\"shell\",\"path\":\"/bin/sh\"},7,\"x\","
                          "{\"kind\":\"commit\",\"hash\":\"abcd1234\",\"path\":\"/etc\",\"title\":\"" +
                              std::string(400, 'y') + "\"},"
                          "{\"kind\":\"commit\",\"hash\":\"abcd1234\"}]",
                          hostile));
        back = Web::reviewPagesFrom(hostile);
        CHECK(back.size() == 1 && back[0].hash == "abcd1234" && back[0].path.empty() && back[0].title.size() == 300);
        Json many = Json::array();
        for (int i = 0; i < 40; i++) {
            Json p = Json::object();
            p.set("kind", Json("commit"));
            char h[16];
            std::snprintf(h, sizeof h, "%08x", i);
            p.set("hash", Json(std::string(h)));
            many.push(p);
        }
        CHECK(Web::reviewPagesFrom(many).size() == Web::kMaxReviewPages);
        const Json view = Web::reviewsViewJson(back);
        CHECK(view.size() == 1 && view[0]["id"].asString() == "commit:abcd1234" && !view[0].has("hash"));
        CHECK(Web::commitName("abcd") && !Web::commitName("abc") && !Web::commitName("abcg") &&
              Web::commitName(std::string(64, 'F')) && !Web::commitName(std::string(65, 'f')));
    }
    if (std::system("git --version >/dev/null 2>&1") != 0) return;
    const std::string root = Shared::resolveFolder(scratchFolder()), elsewhere = Shared::resolveFolder(scratchFolder());
    auto shq = [](const std::string& s) {
        std::string o = "'";
        for (char ch : s) o += ch == '\'' ? std::string("'\\''") : std::string(1, ch);
        return o + "'";
    };
    auto sh = [&](const std::string& cmd) { return std::system(("cd " + shq(root) + " && " + cmd).c_str()) == 0; };
    const ChangeReview::GitRunner run = [](const std::string& dir, const std::vector<std::string>& args,
                                           const std::string& input) { return Web::runGit(dir, args, input); };
    const std::string pic1 = png(2, 1, "one"), pic2 = png(4, 3, "two!"), shot = png(5, 5, "shot");
    ::mkdir((root + "/sub").c_str(), 0755);
    writeText(root + "/a.txt", "one\ntwo\nthree\n");
    writeText(root + "/pic.png", pic1);
    writeText(root + "/.dot.txt", "dot\n");
    writeText(root + "/sub/s.txt", "sub\n");
    CHECK(sh("git init -q . && git symbolic-ref HEAD refs/heads/main && git config user.email a@b && "
             "git config user.name Ann && git config commit.gpgsign false && git add -A && git commit -q -m first"));
    writeText(root + "/a.txt", "one\nTWO\nthree\n");
    writeText(root + "/pic.png", pic2);
    writeText(root + "/.dot.txt", "dot two\n");
    writeText(root + "/sub/s.txt", "sub two\n");
    CHECK(sh("git add -A && git commit -q -m 'Second subject' -m 'The body line.'"));
    const std::string head = Web::runGit(root, {"rev-parse", "HEAD"}, "").out.substr(0, 40);
    // An agent's worktree outside the folder: a change, a new picture, a
    // dotfile, and an untracked link to a secret elsewhere.
    const std::string wt = elsewhere + "/agent-wt";
    CHECK(sh("git worktree add -q -b agent-x " + shq(wt) + " HEAD"));
    writeText(wt + "/a.txt", "one\nTWO\nthree\nagent line\n");
    writeText(wt + "/shot.png", shot);
    writeText(wt + "/.env", "TOKEN\n");
    writeText(elsewhere + "/secret.txt", "SECRET\n");
    CHECK(::symlink((elsewhere + "/secret.txt").c_str(), (wt + "/leak.txt").c_str()) == 0);
    auto fileIn = [](const ChangeReview::Read& r, const std::string& path) -> long {
        for (std::size_t i = 0; i < r.diffs.size(); i++)
            if (r.diffs[i].file.path == path) return (long)i;
        return -1;
    };
    Web::ReviewPage commitPage;
    commitPage.kind = "commit";
    commitPage.hash = head;
    Web::ReviewPage wtPage;
    wtPage.kind = "worktree";
    wtPage.path = wt;
    const long long now = (long long)std::time(nullptr);
    {
        // A commit: its header, its files under the folder, its pictures.
        Web::ReviewRead c = Web::readReviewPage(root, false, commitPage, run, now);
        CHECK(c.read.error.empty());
        CHECK(c.title == "Second subject" && c.body == "The body line.");
        if (c.detail.find("Ann") == std::string::npos || c.detail.find("3 files") == std::string::npos)
            std::printf("  sessionweb:review-pages: commit detail %s\n", c.detail.c_str());
        CHECK(c.detail.find("Ann") != std::string::npos && c.detail.find("3 files") != std::string::npos);
        CHECK(fileIn(c.read, ".dot.txt") < 0 && fileIn(c.read, "a.txt") >= 0 && fileIn(c.read, "sub/s.txt") >= 0);
        const long p = fileIn(c.read, "pic.png");
        CHECK(p >= 0);
        if (p >= 0) {
            const std::size_t pi = (std::size_t)p;
            const ChangeReview::File& f = c.read.diffs[pi].file;
            CHECK(f.image && f.oldImage.width == 2 && f.oldImage.height == 1 && f.newImage.width == 4);
            CHECK(c.read.extras[pi].oldImage == pic1 && c.read.extras[pi].newImage == pic2);
            // The page's review of it: Before and After at their addresses,
            // no picture note among its rows.
            const Json j = Web::reviewFileJson(
                c.read.diffs[pi], {}, false,
                [](std::size_t file, bool isNew) { return "/p/" + std::to_string(file) + (isNew ? "/new" : "/old"); },
                pi);
            CHECK(j["images"].size() == 2 && j["images"][0]["side"].asString() == "old");
            CHECK(j["images"][0]["label"].asString() ==
                  "Before: 2 \xC3\x97 1 pixels, " + std::to_string(pic1.size()) + " bytes");
            CHECK(j["images"][1]["url"].asString() == "/p/" + std::to_string(pi) + "/new");
            CHECK(j["images"][1]["width"].asInt() == 4 && j["images"][1]["height"].asInt() == 3);
            CHECK(j["rows"].size() == 0 && !j["canSide"].asBool());
            // Without addresses, the note the review had.
            const Json plain = Web::reviewFileJson(c.read.diffs[pi], {}, false);
            CHECK(!plain.has("images") && plain["rows"].size() == 1 && plain["rows"][0][0].asString() == "n");
        }
        // Dotfiles with Show Hidden Files; a folder below the top level.
        CHECK(fileIn(Web::readReviewPage(root, true, commitPage, run, now).read, ".dot.txt") >= 0);
        Web::ReviewRead s = Web::readReviewPage(root + "/sub", false, commitPage, run, now);
        CHECK(s.read.error.empty() && s.read.diffs.size() == 1 && fileIn(s.read, "sub/s.txt") == 0);
        // Names that are no commit, or not names at all.
        Web::ReviewPage odd = commitPage;
        odd.hash = std::string(40, 'e');
        CHECK(!Web::readReviewPage(root, false, odd, run, now).read.error.empty());
        odd.hash = "HEAD";
        CHECK(Web::readReviewPage(root, false, odd, run, now).read.error == "This page cannot be shown here.");
        odd.hash = "--output=" + elsewhere + "/x";
        CHECK(!Web::readReviewPage(root, false, odd, run, now).read.error.empty());
        CHECK(::access((elsewhere + "/x").c_str(), F_OK) != 0);
        // Another folder that is no repository: nothing of it.
        CHECK(!Web::readReviewPage(elsewhere, false, commitPage, run, now).read.error.empty());
    }
    {
        // A worktree: against main at the branch point, held to its folder.
        Web::ReviewRead r = Web::readReviewPage(root, false, wtPage, run, now);
        CHECK(r.read.error.empty());
        if (!r.read.error.empty()) std::printf("  sessionweb:review-pages: worktree %s\n", r.read.error.c_str());
        CHECK(r.title == "agent-wt compared with main");
        CHECK(r.detail.find("against main") != std::string::npos && r.detail.find("agent-x") != std::string::npos);
        const long a = fileIn(r.read, "a.txt"), s = fileIn(r.read, "shot.png"), leak = fileIn(r.read, "leak.txt");
        CHECK(a >= 0 && s >= 0 && leak >= 0 && fileIn(r.read, ".env") < 0);
        if (s >= 0) {
            const ChangeReview::File& f = r.read.diffs[(std::size_t)s].file;
            CHECK(f.image && !f.oldImage.shown && f.newImage.shown && f.newImage.width == 5);
            CHECK(r.read.extras[(std::size_t)s].newImage == shot);
            CHECK(ChangeReview::imageLabel(f, true).find("After") == std::string::npos);
        }
        if (leak >= 0) {
            const ChangeReview::FileDiff& d = r.read.diffs[(std::size_t)leak];
            CHECK(d.file.symlink && d.file.newText.empty() && d.rows.empty() && !d.file.image);
            CHECK(r.read.extras[(std::size_t)leak].workBytes.empty());
        }
        const std::string j = Web::reviewJson(r.read, 1, {}, {}).dump();
        CHECK(j.find("SECRET") == std::string::npos && j.find("TOKEN") == std::string::npos);
        // Only a worktree the folder's repository lists: not the main
        // checkout, not another folder, not a path that is gone.
        Web::ReviewPage other = wtPage;
        other.path = root;
        CHECK(!Web::readReviewPage(root, false, other, run, now).read.error.empty());
        other.path = elsewhere;
        CHECK(Web::readReviewPage(root, false, other, run, now).read.error ==
              "That worktree is not one of this repository's any more.");
        other.path = elsewhere + "/nope";
        CHECK(!Web::readReviewPage(root, false, other, run, now).read.error.empty());
    }
    {
        // Reads on a thread of their own: a picture that changed alone is
        // a new version.
        std::atomic<int> woke{0};
        Web::GitReads reads([&] { return Web::readReviewPage(root, false, wtPage, run, now); }, [&] { woke++; });
        reads.ask();
        CHECK(waitUntil([&] { return reads.latest().finished == 1; }, 20000));
        const Web::GitReads::Latest l = reads.latest();
        CHECK(l.read && l.page && l.version == 1 && l.page->title == "agent-wt compared with main");
        CHECK(l.read.get() == &l.page->read);
        writeText(wt + "/shot.png", png(5, 5, "SHOT"));   // the same size, other pixels
        reads.ask();
        CHECK(waitUntil([&] { return reads.latest().finished == 2; }, 20000));
        CHECK(reads.latest().version == 2);
        writeText(wt + "/shot.png", shot);
    }

    // Over the wire: a host in this process whose window has both pages
    // open (and reports some that do not hold up), a read-only page.
    std::mutex routesMu;
    std::map<Shared::ClientId, Shared::Client*> routes;
    Shared::SessionHost host([&](Shared::ClientId to, Message&& m) {
        std::lock_guard<std::mutex> lk(routesMu);
        auto it = routes.find(to);
        if (it != routes.end()) it->second->deliver(std::move(m));
    });
    Shared::Client editor(Model::Unit::Utf16, "editor");
    const Shared::ClientId editorId = host.connect();
    {
        std::lock_guard<std::mutex> lk(routesMu);
        routes[editorId] = &editor;
    }
    editor.connect([&](Message&& m) { host.post(editorId, std::move(m)); });
    auto windowWith = [&](bool withCommit) {
        Json w = Json::object();
        w.set("root", Json(root));
        w.set("focused", Json(0));
        w.set("groups", Json::array());
        Json reviews = Json::array();
        auto add = [&](const std::string& kind, const std::string& key, const std::string& value,
                       const std::string& title) {
            Json p = Json::object();
            p.set("kind", Json(kind));
            p.set(key, Json(value));
            p.set("title", Json(title));
            reviews.push(p);
        };
        if (withCommit) add("commit", "hash", head, "Second subject");
        add("worktree", "path", wt, "agent-wt vs main");
        add("commit", "hash", "HEAD", "not a name");
        add("worktree", "path", "relative/wt", "not absolute");
        w.set("reviews", reviews);
        return w;
    };
    editor.windowTabs(5, windowWith(true));
    // Another window's page is not this one's.
    Json other = windowWith(false);
    Json otherReviews = Json::array();
    Json otherPage = Json::object();
    otherPage.set("kind", Json("commit"));
    otherPage.set("hash", Json(std::string(40, 'b')));
    otherReviews.push(otherPage);
    other.set("reviews", otherReviews);
    editor.windowTabs(6, other);
    CHECK(editor.settle());
    auto connectTo = [&](Shared::Client& cl) -> std::function<void()> {
        const Shared::ClientId id = host.connect();
        {
            std::lock_guard<std::mutex> lk(routesMu);
            routes[id] = &cl;
        }
        cl.connect([&host, id](Message&& m) { host.post(id, std::move(m)); });
        return [&, id] {
            {
                std::lock_guard<std::mutex> lk(routesMu);
                routes.erase(id);
            }
            host.disconnect(id);
        };
    };
    Web::Server web;
    Web::Options o;
    o.folder = root;
    o.window = 5;
    o.pollMs = 100;
    o.connect = connectTo;
    std::string error;
    CHECK(web.start(o, &error));
    std::thread webThread([&] { web.run(); });
    const int port = web.port();
    PageClient p;
    CHECK(p.open(web));
    auto get = [&](const std::string& target, bool cookie = true) {
        PageClient cl;
        return cl.exchange(port, "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n" +
                                     (cookie ? "Cookie: " + p.cookie + "\r\n" : "") + "\r\n");
    };
    auto bodyOf = [](const std::string& r) {
        const std::size_t e = r.find("\r\n\r\n");
        return e == std::string::npos ? std::string() : r.substr(e + 4);
    };
    auto headOf = [](const std::string& r) { return r.substr(0, r.find("\r\n\r\n") + 2); };
    auto ask = [&](const std::string& page) {
        Json m = Json::object();
        m.set("t", Json("review"));
        m.set("page", Json(page));
        return m;
    };
    Json state;
    std::vector<Json> reviews, files, reads;
    std::string all;
    auto take = [&](const Json& m) {
        const std::string t = m["t"].asString();
        if (t == "state") state = m;
        if (t == "review" || t == "reviewfile" || t == "reviewread") all += m.dump();
        if (t == "review") reviews.push_back(m);
        if (t == "reviewfile") files.push_back(m);
        if (t == "reviewread") reads.push_back(m);
    };
    CHECK(p.pump(take, [&] { return state.isObject() && state["windows"].size() == 1; }));
    const std::string commitId = "commit:" + head, wtId = "worktree:" + wt;
    {
        const Json& rv = state["windows"][0]["reviews"];
        CHECK(rv.size() == 2 && rv[0]["id"].asString() == commitId && rv[1]["id"].asString() == wtId);
        CHECK(rv[0]["title"].asString() == "Second subject" && rv[0]["kind"].asString() == "commit");
        const std::string text = state.dump();
        CHECK(text.find(std::string(40, 'b')) == std::string::npos);   // window 6's
        CHECK(text.find("relative/wt") == std::string::npos && text.find("not a name") == std::string::npos);
    }
    // A review page, asked for: pending until read, then the review with
    // its header.
    auto readPage = [&](const std::string& id) {
        reviews.clear();
        reads.clear();
        p.sendText(ask(id).dump());
        CHECK(p.pump(take, [&] { return !reviews.empty(); }));
        if (!reviews.empty() && reviews.back()["pending"].asBool()) {
            CHECK(p.pump(take, [&] { return !reads.empty(); }, 20000));
            CHECK(!reads.empty() && reads.back()["page"].asString() == id && reads.back()["version"].asInt() >= 1);
            p.sendText(ask(id).dump());
            CHECK(p.pump(take, [&] { return reviews.size() == 2; }));
        }
        return reviews.empty() ? Json() : reviews.back();
    };
    const Json commitReview = readPage(commitId);
    CHECK(commitReview["page"].asString() == commitId);
    CHECK(commitReview["title"].asString() == "Second subject" && commitReview["body"].asString() == "The body line.");
    std::string picOld, picNew, textKey;
    std::size_t textIndex = 0;
    for (std::size_t i = 0; i < commitReview["files"].size(); i++) {
        const Json& f = commitReview["files"][i];
        CHECK(f["section"].asString() == "plain");
        if (f["path"].asString() == "a.txt") {
            textKey = f["key"].asString();
            textIndex = i;
        }
        if (f["path"].asString() != "pic.png") continue;
        CHECK(f["images"].size() == 2);
        picOld = f["images"][0]["url"].asString();
        picNew = f["images"][1]["url"].asString();
    }
    CHECK(picOld.find(web.base() + "picture/") == 0 && picNew.size() > 4 &&
          picNew.compare(picNew.size() - 4, 4, "/new") == 0);
    // Its pictures over HTTP: the commit's bytes, as /file/ serves a
    // picture; the credential needed; a stale or made-up address refused.
    {
        std::string r = get(picOld);
        CHECK(r.compare(0, 15, "HTTP/1.1 200 OK") == 0 && bodyOf(r) == pic1);
        CHECK(headOf(r).find("Content-Type: image/png\r\n") != std::string::npos);
        CHECK(headOf(r).find("X-Content-Type-Options: nosniff\r\n") != std::string::npos);
        CHECK(headOf(r).find("sandbox") != std::string::npos);
        r = get(picNew);
        CHECK(r.compare(0, 15, "HTTP/1.1 200 OK") == 0 && bodyOf(r) == pic2);
        CHECK(get(picOld, false).compare(0, 12, "HTTP/1.1 403") == 0);
        // The same picture under another read's version: refused.
        const std::string prefix = web.base() + "picture/";
        std::string stale = picOld;
        const std::size_t v = stale.find('/', prefix.size());   // after the page's number
        if (v != std::string::npos) stale.insert(v + 1, "9");
        CHECK(stale != picOld && get(stale).compare(0, 12, "HTTP/1.1 404") == 0);
        for (const std::string& badPath :
             {std::string("picture/1/1/999/old"), std::string("picture/77/1/0/old"), std::string("picture/1/1/0/mid"),
              std::string("picture/../../etc/passwd"), std::string("picture/0/1/0/old")})
            CHECK(get(web.base() + badPath).compare(0, 12, "HTTP/1.1 404") == 0);
        // A text file of the review has no picture.
        std::string text = picOld.substr(0, picOld.rfind('/'));
        text = text.substr(0, text.rfind('/') + 1) + std::to_string(textIndex) + "/new";
        CHECK(get(text).compare(0, 12, "HTTP/1.1 404") == 0);
    }
    // One file of it, by key; pages the window does not have.
    {
        Json m = ask(commitId);
        m.set("file", Json(textKey));
        m.set("side", Json(true));
        p.sendText(m.dump());
        CHECK(p.pump(take, [&] { return !files.empty(); }));
        CHECK(!files.empty() && files[0]["page"].asString() == commitId && files[0]["file"]["side"].asBool());
    }
    reviews.clear();
    p.sendText(ask("commit:" + std::string(40, 'b')).dump());
    p.sendText(ask("worktree:/etc").dump());
    CHECK(p.pump(take, [&] { return reviews.size() == 2; }));
    CHECK(reviews.size() == 2 && reviews[0]["error"].asString() == "This page is no longer open in the mutible window." &&
          !reviews[0].has("files") && reviews[1].has("error"));
    // The worktree page: the link shown as a link, nothing of the secret;
    // an edit in the worktree reaches the page that shows it.
    const Json wtReview = readPage(wtId);
    CHECK(wtReview["title"].asString() == "agent-wt compared with main");
    bool sawLeak = false, sawShot = false;
    for (const Json& f : wtReview["files"].elements()) {
        if (f["path"].asString() == "leak.txt") sawLeak = !f.has("images");
        if (f["path"].asString() == "shot.png") sawShot = f["images"].size() == 1;
    }
    CHECK(sawLeak && sawShot);
    reads.clear();
    const std::int64_t wtVersion = wtReview["version"].asInt();
    writeText(wt + "/a.txt", "one\nTWO\nthree\nagent line two\n");
    web.gitChanged();
    CHECK(p.pump(take, [&] { return !reads.empty(); }, 20000));
    CHECK(!reads.empty() && reads.back()["page"].asString() == wtId && reads.back()["version"].asInt() > wtVersion);
    // The window closes its commit page: the state lists it no more, its
    // pictures go, and the page's ask is refused.
    editor.windowTabs(5, windowWith(false));
    CHECK(editor.settle());
    CHECK(p.pump(take, [&] { return state["windows"][0]["reviews"].size() == 1; }));
    reviews.clear();
    p.sendText(ask(commitId).dump());
    CHECK(p.pump(take, [&] { return !reviews.empty(); }));
    CHECK(!reviews.empty() && reviews[0].has("error"));
    CHECK(get(picOld).compare(0, 12, "HTTP/1.1 404") == 0);
    CHECK(all.find("SECRET") == std::string::npos && all.find("TOKEN") == std::string::npos);
    p.close();
    CHECK(waitUntil([&] { return web.browsers() == 0; }));
    web.stop();
    webThread.join();
    // Nothing in either checkout changed: no stage, no commit.
    CHECK(Web::runGit(root, {"rev-parse", "HEAD"}, "").out.substr(0, 40) == head);
    CHECK(Web::runGit(root, {"status", "--porcelain"}, "").out.empty());
    CHECK(Web::runGit(wt, {"diff", "--cached", "--name-only"}, "").out.empty());
    // The page asks for the pages by id and draws with textContent.
    CHECK(Web::pageHtml().find("case \"reviewread\"") != std::string::npos);
    CHECK(Web::pageHtml().find("innerHTML") == std::string::npos);
}
