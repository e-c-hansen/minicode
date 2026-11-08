// procs_tests.inc — the `procs:*` groups of run_tests.cpp (ProcessTable.h):
// processes and ports per folder, against a scripted process table.
#include "ProcessTable.h"
#include <chrono>

namespace {

// A process table the test writes, with each process's details.
struct FakeProcs : Procs::Source {
    struct P {
        Procs::Basic b;
        std::string cwd, cmd;
        Procs::Detail d;
    };
    std::vector<P> ps;
    int cwdCalls = 0;

    P& add(int pid, int ppid, int64_t start, const std::string& name, const std::string& cwd,
           uint64_t memory = 1024 * 1024, uint32_t uid = 501, int pgid = 0) {
        P p;
        p.b.pid = pid;
        p.b.ppid = ppid;
        p.b.pgid = pgid ? pgid : pid;
        p.b.uid = uid;
        p.b.start = start;
        p.b.name = name;
        p.cwd = cwd;
        p.cmd = name;
        p.d.memory = memory;
        ps.push_back(p);
        return ps.back();
    }
    P* find(int pid) {
        for (P& p : ps)
            if (p.b.pid == pid) return &p;
        return nullptr;
    }
    void remove(int pid) {
        for (size_t i = 0; i < ps.size(); i++)
            if (ps[i].b.pid == pid) { ps.erase(ps.begin() + (long)i); return; }
    }
    std::vector<Procs::Basic> list() override {
        std::vector<Procs::Basic> out;
        for (const P& p : ps) out.push_back(p.b);
        return out;
    }
    bool cwd(int pid, std::string& out) override {
        cwdCalls++;
        P* p = find(pid);
        if (!p || p->b.uid != 501) return false;
        out = p->cwd;
        return true;
    }
    bool commandLine(int pid, std::string& out) override {
        P* p = find(pid);
        if (!p) return false;
        out = p->cmd;
        return true;
    }
    bool detail(int pid, Procs::Detail& out) override {
        P* p = find(pid);
        if (!p) return false;
        out = p->d;
        return true;
    }
};

const Procs::Proc* tracked(const Procs::Tracker& t, int pid, bool aliveOnly = true) {
    for (const auto& kv : t.processes())
        if (kv.first.pid == pid && (!aliveOnly || kv.second.alive)) return &kv.second;
    return nullptr;
}

// "name@depth" for every row of a group, in tree order.
void rowsOf(const std::vector<Procs::Node>& nodes, int depth, std::vector<std::string>& out) {
    for (const Procs::Node& n : nodes) {
        out.push_back(Procs::displayName(n.proc) + "@" + std::to_string(depth));
        rowsOf(n.children, depth + 1, out);
    }
}
std::vector<std::string> rowsOf(const Procs::Group& g) {
    std::vector<std::string> out;
    rowsOf(g.roots, 0, out);
    return out;
}

}  // namespace

static void testProcs() {
    using namespace Procs;
    const int self = 100;
    const std::string folder = "/Users/me/proj";
    const uint64_t MB = 1024 * 1024;

    // launchd, minicode, its two shells and a language server, a dev server
    // under shell 1 (npm, then node), an editor's process elsewhere.
    auto world = [&](FakeProcs& f) {
        f.add(1, 0, 1, "launchd", "/");
        f.add(self, 1, 10, "MiniCode", "/");
        f.add(200, self, 20, "zsh", folder);
        f.add(300, self, 30, "zsh", folder);
        f.add(210, 200, 40, "npm", folder, 50 * MB).cmd = "npm run dev";
        f.add(211, 210, 41, "node", folder, 200 * MB, 501, 210).cmd =
            "/opt/homebrew/bin/node /Users/me/proj/node_modules/.bin/vite";
        f.add(150, self, 15, "pyright", folder, 300 * MB, 501, self);   // minicode's own
        f.add(400, 1, 50, "Code Helper", "/");                          // elsewhere
    };
    const std::vector<Terminal> terms = {{1, "npm run dev", 200, 7, ""}, {2, "zsh", 300, 7, ""}};

    GROUP("procs:inside");
    CHECK(isInside("/Users/me/proj", folder));
    CHECK(isInside("/Users/me/proj/src/a", folder));
    CHECK(!isInside("/Users/me/project", folder));
    CHECK(!isInside("/Users/me", folder));
    CHECK(!isInside("", folder) && !isInside("/x", ""));
    CHECK(isInside("/x", "/"));

    GROUP("procs:words");
    CHECK(formatBytes(512 * 1024) == "512 KB");
    CHECK(formatBytes((uint64_t)(34.2 * MB)) == "34.2 MB");
    CHECK(formatBytes(412 * MB) == "412 MB");
    CHECK(formatBytes((uint64_t)(1.8 * 1024 * MB)) == "1.8 GB");
    CHECK(summary((uint64_t)(1.8 * 1024 * MB), 14) == "1.8 GB in 14 processes");
    CHECK(summary(3 * MB, 1) == "3.0 MB in 1 process");
    CHECK(summary(0, 0) == "No processes");
    CHECK(formatCpu(0) == "0%" && formatCpu(0.42) == "0.4%" && formatCpu(12.4) == "12%" &&
          formatCpu(250) == "250%");
    CHECK(formatPorts({{"127.0.0.1", 8123}, {"::1", 8123}, {"*", 5173}}) == "8123, 5173");
    {
        Proc p;
        p.name = "Python";
        p.command = "/usr/bin/python3 -m http.server 8123";
        CHECK(displayName(p) == "python3");
        p.command = "-zsh";
        CHECK(displayName(p) == "zsh");
        p.command = "";
        CHECK(displayName(p) == "Python");
    }
    CHECK(std::string(helpText()).find("can be missed") != std::string::npos);

    GROUP("procs:terminal-descendants");
    {
        FakeProcs f;
        world(f);
        Tracker t;
        SampleStats st = t.sample(f, 1000, terms, {folder}, self, 501);
        CHECK(st.listed == 8);
        // The dev server and its node, from terminal 1; not the shells,
        // not minicode, not its language server, not the other app.
        CHECK(tracked(t, 210) && tracked(t, 211));
        CHECK(!tracked(t, 200) && !tracked(t, 300) && !tracked(t, self) && !tracked(t, 150) &&
              !tracked(t, 400) && !tracked(t, 1));
        CHECK(t.processes().size() == 2);
        const Proc* npm = tracked(t, 210);
        CHECK(npm->origin == Origin::Terminal && npm->terminal == 1 && npm->window == 7 &&
              npm->terminalTitle == "npm run dev");
        CHECK(npm->command == "npm run dev" && npm->cwd == folder && npm->memory == 50 * MB);
        CHECK(tracked(t, 211)->terminal == 1 && tracked(t, 211)->pgid == 210);
    }

    GROUP("procs:reparented");
    {
        FakeProcs f;
        world(f);
        Tracker t;
        t.sample(f, 1000, terms, {folder}, self, 501);
        // npm exits; node goes to launchd and starts a child of its own.
        f.remove(210);
        f.find(211)->b.ppid = 1;
        f.add(212, 211, 60, "esbuild", "/tmp");
        t.sample(f, 1002, terms, {folder}, self, 501);
        const Proc* node = tracked(t, 211);
        CHECK(node && node->origin == Origin::Terminal && node->terminal == 1 && node->ppid == 1);
        const Proc* child = tracked(t, 212);
        CHECK(child && child->origin == Origin::Terminal && child->terminal == 1);
        const Proc* npm = tracked(t, 210, false);
        CHECK(npm && !npm->alive && npm->exitedAt == 1002);
        // Forgotten a minute after it exited, not before.
        t.sample(f, 1061, terms, {folder}, self, 501);
        CHECK(tracked(t, 210, false));
        t.sample(f, 1062.5, terms, {folder}, self, 501);
        CHECK(!tracked(t, 210, false) && tracked(t, 211));
    }

    GROUP("procs:pid-reused");
    {
        FakeProcs f;
        world(f);
        Tracker t;
        t.sample(f, 1000, terms, {folder}, self, 501);
        // 211 ends, and the pid comes back as something under launchd.
        f.remove(211);
        f.add(211, 1, 99, "cron", "/");
        t.sample(f, 1002, terms, {folder}, self, 501);
        CHECK(!tracked(t, 211));                 // the new 211 is not ours
        const Proc* old = tracked(t, 211, false);
        CHECK(old && !old->alive && old->key.start == 41);
    }

    GROUP("procs:outside");
    {
        FakeProcs f;
        world(f);
        f.add(500, 1, 70, "node", folder + "/web", 80 * MB).cmd = "node server.js";   // Terminal.app's
        f.add(501, 500, 71, "node", "/elsewhere");
        f.add(600, 1, 72, "postgres", folder, 10 * MB, 70);   // another user's
        Tracker t;
        t.sample(f, 1000, terms, {folder}, self, 501);
        const Proc* web = tracked(t, 500);
        CHECK(web && web->origin == Origin::Outside && web->cwd == folder + "/web");
        CHECK(!tracked(t, 501) && !tracked(t, 600));
        CHECK(!tracked(t, 150));   // minicode's own language server, though in the folder
        // It moves out of the folder: dropped, not kept as exited.
        f.find(500)->cwd = "/tmp";
        t.sample(f, 1002, terms, {folder}, self, 501);
        CHECK(!tracked(t, 500, false));
        // No folders: no working folders asked for at all.
        FakeProcs g;
        world(g);
        Tracker t2;
        t2.sample(g, 1000, terms, {}, self, 501);
        CHECK(g.cwdCalls == 2);   // only the two terminal processes'
    }

    GROUP("procs:cpu");
    {
        FakeProcs f;
        world(f);
        Tracker t;
        f.find(211)->d.cpuNanos = 5000000000ull;
        t.sample(f, 1000, terms, {folder}, self, 501);
        CHECK(tracked(t, 211)->cpu == 0);
        f.find(211)->d.cpuNanos += 1000000000ull;   // one second of CPU in two
        t.sample(f, 1002, terms, {folder}, self, 501);
        CHECK(tracked(t, 211)->cpu > 49.9 && tracked(t, 211)->cpu < 50.1);
        // A look 0.1 s later keeps the figure rather than dividing by nothing.
        f.find(211)->d.cpuNanos += 100000000ull;
        t.sample(f, 1002.1, terms, {folder}, self, 501);
        CHECK(tracked(t, 211)->cpu > 49.9 && tracked(t, 211)->cpu < 50.1);
    }

    GROUP("procs:command-after-exec");
    {
        FakeProcs f;
        world(f);
        // A fork of the shell seen before its exec.
        FakeProcs::P& c = f.add(220, 200, 80, "zsh", folder);
        c.cmd = "-zsh";
        Tracker t;
        t.sample(f, 1000, terms, {folder}, self, 501);
        CHECK(tracked(t, 220)->command == "-zsh");
        f.find(220)->b.name = "python3.12";
        f.find(220)->cmd = "python3 -m http.server 8123";
        f.find(220)->d.ports = {{"*", 8123}};
        t.sample(f, 1002, terms, {folder}, self, 501);
        CHECK(tracked(t, 220)->command == "python3 -m http.server 8123");
        CHECK(formatPorts(tracked(t, 220)->ports) == "8123");
    }

    GROUP("procs:view");
    {
        FakeProcs f;
        world(f);
        f.add(500, 1, 70, "node", folder, 80 * MB).cmd = "node server.js";
        f.add(230, 300, 90, "sleep", folder, 1 * MB).cmd = "sleep 100";
        Tracker t;
        t.sample(f, 1000, terms, {folder}, self, 501);
        View v = buildView(t.snapshot(), terms, 7, folder);
        CHECK(v.groups.size() == 3);
        CHECK(v.groups[0].kind == GroupKind::Terminal && v.groups[0].terminal == 1 &&
              v.groups[0].title == "npm run dev");
        CHECK(rowsOf(v.groups[0]) == (std::vector<std::string>{"npm@0", "node@1"}));
        CHECK(v.groups[0].memory == 250 * MB && v.groups[0].count == 2);
        CHECK(v.groups[1].terminal == 2 && rowsOf(v.groups[1]) == (std::vector<std::string>{"sleep@0"}));
        CHECK(v.groups[2].kind == GroupKind::Outside && v.groups[2].title == "Not started here");
        CHECK(v.count == 4 && v.memory == 331 * MB);

        // Terminal 1 closes: its processes are orphans, the rest stay put.
        const std::vector<Terminal> one = {terms[1]};
        f.remove(200);
        f.find(210)->b.ppid = 1;
        t.sample(f, 1002, one, {folder}, self, 501);
        v = buildView(t.snapshot(), one, 7, folder);
        CHECK(v.groups.size() == 3 && v.groups[0].terminal == 2);
        CHECK(v.groups[1].kind == GroupKind::Orphans && v.groups[1].title == "Orphans");
        CHECK(rowsOf(v.groups[1]) == (std::vector<std::string>{"npm@0", "node@1"}));

        // Another window on the same folder sees no terminal groups of this
        // one's, but the orphans (by folder) and the outside process.
        View other = buildView(t.snapshot(), one, 8, folder);
        CHECK(other.groups.size() == 2 && other.groups[0].kind == GroupKind::Orphans);
        // A folder elsewhere sees nothing.
        CHECK(buildView(t.snapshot(), one, 9, "/srv").groups.empty());

        // Exited rows show but do not count.
        f.remove(230);
        t.sample(f, 1004, one, {folder}, self, 501);
        v = buildView(t.snapshot(), one, 7, folder);
        CHECK(v.groups[0].count == 0 && v.groups[0].roots.size() == 1 &&
              !v.groups[0].roots[0].proc.alive);
    }

    GROUP("procs:json");
    {
        FakeProcs f;
        world(f);
        f.find(211)->d.ports = {{"127.0.0.1", 5173}};
        Tracker t;
        t.sample(f, 1000, terms, {folder}, self, 501);
        Json j = toJson(buildView(t.snapshot(), terms, 7, folder), folder, 7);
        CHECK(j["summary"].asString() == "250 MB in 2 processes");
        CHECK(j["window"].asInt() == 7 && j["count"].asInt() == 2);
        const Json& g = j["groups"][0];
        CHECK(g["kind"].asString() == "terminal" && g["terminal"].asInt() == 1);
        CHECK(g["processes"].size() == 2);
        const Json& node = g["processes"][1];
        CHECK(node["pid"].asInt() == 211 && node["depth"].asInt() == 1 &&
              node["ports"][0]["port"].asInt() == 5173 && node["name"].asString() == "node" &&
              node["terminal"].asInt() == 1 && node["origin"].asString() == "terminal");
        std::string text = formatTable(j);
        CHECK(text.find("250 MB in 2 processes in /Users/me/proj\n") == 0);
        CHECK(text.find("npm run dev (terminal 1), 250 MB") != std::string::npos);
        CHECK(text.find("    211") != std::string::npos);
        CHECK(text.find("5173") != std::string::npos);
        CHECK(text.find("  /opt/homebrew/bin/node") != std::string::npos);   // indented under npm
        Json empty = toJson(View{}, folder, 0);
        CHECK(formatTable(empty) == "No processes in /Users/me/proj\n");
        CHECK(!empty.has("window"));
    }

    GROUP("procs:stop");
    {
        FakeProcs f;
        world(f);
        // Claude in terminal 2 started a server in its own process group.
        f.add(310, 300, 100, "claude", folder, 1, 501, 310);
        f.add(311, 310, 101, "python3", folder, 1, 501, 310);
        f.add(312, 311, 102, "python3", folder, 1, 501, 310);
        std::vector<Basic> table = f.list();
        // npm leads a group of npm and node: the group.
        StopPlan p = planStop(table, Key{210, 40}, self, self, {200, 300});
        CHECK(p.ok && p.group && p.pgid == 210 && p.targets.size() == 2);
        // node alone shares npm's group, which is more than node and its
        // children: node only.
        p = planStop(table, Key{211, 41}, self, self, {200, 300});
        CHECK(p.ok && !p.group && p.targets.size() == 1 && p.targets[0].pid == 211);
        // The server Claude started: never Claude's group.
        p = planStop(table, Key{311, 101}, self, self, {200, 300});
        CHECK(p.ok && !p.group && p.targets.size() == 2);
        // Claude itself: its group is all its own.
        p = planStop(table, Key{310, 100}, self, self, {200, 300});
        CHECK(p.ok && p.group && p.pgid == 310 && p.targets.size() == 3);
        // Refusals.
        CHECK(!planStop(table, Key{300, 30}, self, self, {200, 300}).ok);
        CHECK(!planStop(table, Key{self, 10}, self, self, {200, 300}).ok);
        CHECK(!planStop(table, Key{1, 1}, self, self, {200, 300}).ok);
        CHECK(planStop(table, Key{211, 999}, self, self, {200, 300}).refusal == "it has already exited");
        // A group shared with minicode (a language server): never the group.
        p = planStop(table, Key{150, 15}, self, self, {200, 300});
        CHECK(p.ok && !p.group && p.targets.size() == 1);
    }

    GROUP("procs:command-line");
    {
        using namespace ControlChannel;
        auto none = [](const std::string&) { return false; };
        CliCommand c = parseCommandLine({"procs"}, "/w", "/h", none);
        CHECK(c.error.empty() && c.cmd == "procs" && c.output == Output::Procs && c.args.size() == 0);
        c = parseCommandLine({"procs", "--json", "--folder", "proj"}, "/w", "/h", none);
        CHECK(c.error.empty() && c.output == Output::Json && c.args["folder"].asString() == "/w/proj" &&
              !c.args.has("json") && c.launchFolder == "/w/proj");
        std::string err;
        CHECK(validateArgs(*findCommand("procs"), c.args, err));
        c = parseCommandLine({"procs", "--window", "3"}, "/w", "/h", none);
        CHECK(c.error.empty() && c.args["window"].asInt() == 3);
        CHECK(!parseCommandLine({"procs", "x"}, "/w", "/h", none).error.empty());
        CHECK(!parseCommandLine({"procs", "--all"}, "/w", "/h", none).error.empty());
        CHECK(isSubcommand("procs") && !launchesApp("procs"));
        CHECK(usageText("procs").find("kill <pid>") != std::string::npos);
        CHECK(usageText().find("minicode procs") != std::string::npos);
        Request r;
        CHECK(parseRequest(R"({"v":1,"id":1,"cmd":"procs","args":{"folder":"/x"}})", r, err));
        CHECK(!parseRequest(R"({"v":1,"id":1,"cmd":"procs","args":{"json":true}})", r, err));
    }

    GROUP("procs:speed");
    {
        // 2,000 processes, 200 of them under a terminal: a sample of the
        // core's part stays well inside the budget.
        FakeProcs f;
        f.add(1, 0, 1, "launchd", "/");
        f.add(self, 1, 10, "MiniCode", "/");
        f.add(200, self, 20, "zsh", folder);
        for (int i = 0; i < 200; i++) f.add(1000 + i, i ? 1000 + i - 1 : 200, 100 + i, "w", folder);
        for (int i = 0; i < 1800; i++) f.add(5000 + i, 1, 500 + i, "x", "/Applications");
        Tracker t;
        const double t0 = cpuMs();
        for (int k = 0; k < 10; k++) t.sample(f, 1000 + k * 2, {{1, "zsh", 200, 1, ""}}, {folder}, self, 501);
        const double ms = (cpuMs() - t0) / 10;
        CHECK(t.processes().size() == 200);
        std::printf("  procs: a sample of 2,000 processes (200 tracked): %.2f ms\n", ms);
        CHECK_TIME(ms, 150);   // unoptimized: about 10 ms on an M3, 30 on its efficiency cores
    }
}
