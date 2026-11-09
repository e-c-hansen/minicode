// ProcessTable.cpp — see ProcessTable.h.
#include "ProcessTable.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <functional>

namespace Procs {

namespace {

const Basic* liveBasic(const std::vector<Basic>& list, const std::map<int, size_t>& byPid,
                       const Key& k) {
    auto it = byPid.find(k.pid);
    if (it == byPid.end()) return nullptr;
    const Basic& b = list[it->second];
    return b.start == k.start ? &b : nullptr;
}

Key keyOf(const Basic& b) { return Key{b.pid, b.start}; }

void takeTerminal(Proc& p, const Terminal& t) {
    p.origin = Origin::Terminal;
    p.terminal = t.id;
    p.terminalTitle = t.title;
    p.window = t.window;
}

// A shell's short name, as the kernel gives it (a login shell's starts
// with "-"): the terminal's shell is no shell after `exec claude`.
bool isShellName(std::string name) {
    if (!name.empty() && name[0] == '-') name.erase(0, 1);
    static const char* const kShells[] = {"zsh", "bash", "sh", "dash", "fish", "ksh", "mksh",
                                          "tcsh", "csh", "nu", "elvish", "xonsh", "pwsh"};
    for (const char* s : kShells)
        if (name == s) return true;
    return false;
}

}  // namespace

bool isInside(const std::string& path, const std::string& folder) {
    if (path.empty() || folder.empty()) return false;
    if (folder == "/") return path[0] == '/';
    if (path.size() < folder.size() || path.compare(0, folder.size(), folder) != 0) return false;
    return path.size() == folder.size() || path[folder.size()] == '/';
}

// ------------------------------------------------------------ sampling
SampleStats Tracker::sample(Source& source, double now, const std::vector<Terminal>& terminals,
                            const std::vector<std::string>& folders, int selfPid, uint32_t uid) {
    SampleStats st;
    std::vector<Basic> list = source.list();
    st.listed = list.size();
    std::map<int, size_t> byPid;
    std::map<int, std::vector<size_t>> kids;
    for (size_t i = 0; i < list.size(); i++) {
        byPid[list[i].pid] = i;
        if (list[i].ppid != list[i].pid) kids[list[i].ppid].push_back(i);
    }

    // Exits, and forgetting what exited a while ago.
    for (auto it = procs_.begin(); it != procs_.end();) {
        Proc& p = it->second;
        if (liveBasic(list, byPid, p.key)) {
            ++it;
            continue;
        }
        if (p.alive) {
            p.alive = false;
            p.exitedAt = now;
            p.ports.clear();
            p.cpu = 0;
        }
        if (now - p.exitedAt >= kForgetAfter) it = procs_.erase(it);
        else ++it;
    }

    std::set<int> shells;
    for (const Terminal& t : terminals)
        if (t.shellPid > 0) shells.insert(t.shellPid);

    // minicode's own tree (its shells, language servers, git): never "not
    // started here".
    std::set<int> appTree;
    {
        std::deque<int> q{selfPid};
        while (!q.empty()) {
            int pid = q.front();
            q.pop_front();
            if (!appTree.insert(pid).second) continue;
            auto k = kids.find(pid);
            if (k == kids.end()) continue;
            for (size_t i : k->second) q.push_back(list[i].pid);
        }
    }

    // Descendants of each terminal's shell, then of every process already
    // tracked from a terminal (one re-parented to launchd keeps its origin,
    // and so do its children).
    std::set<int> fromTerminal;
    auto walk = [&](int rootPid, const std::function<void(Proc&, bool fresh)>& assign) {
        std::deque<int> q;
        auto k = kids.find(rootPid);
        if (k != kids.end())
            for (size_t i : k->second) q.push_back((int)i);
        while (!q.empty()) {
            const Basic& b = list[(size_t)q.front()];
            q.pop_front();
            if (b.pid <= 1 || b.pid == selfPid || shells.count(b.pid)) continue;
            if (!fromTerminal.insert(b.pid).second) continue;
            auto found = procs_.find(keyOf(b));
            const bool fresh = found == procs_.end();
            Proc& p = procs_[keyOf(b)];
            if (fresh) p.key = keyOf(b);
            assign(p, fresh);
            auto c = kids.find(b.pid);
            if (c != kids.end())
                for (size_t i : c->second) q.push_back((int)i);
        }
    };
    // The process each terminal's Claude Code session runs as (R7.4; see
    // SessionRoot), and every pid under it now.
    std::map<int, std::string> sessionOf;
    {
        std::map<int, SessionRoot> kept;
        for (const Terminal& t : terminals) {
            if (t.session.empty() || t.shellPid <= 0 || !byPid.count(t.shellPid)) continue;
            SessionRoot sr = sessions_.count(t.id) ? sessions_[t.id] : SessionRoot();
            const bool rootAlive = sr.root.pid && liveBasic(list, byPid, sr.root);
            if (sr.session != t.session || (!rootAlive && t.sessionNamedAt > sr.namedAt)) {
                sr.session = t.session;
                sr.namedAt = t.sessionNamedAt;
                sr.root = Key();
                const Basic& shell = list[byPid[t.shellPid]];
                if (!isShellName(shell.name)) {
                    sr.root = keyOf(shell);
                } else {
                    const Basic* newest = nullptr;
                    auto k = kids.find(t.shellPid);
                    if (k != kids.end())
                        for (size_t i : k->second)
                            if (list[i].start <= t.sessionNamedAt &&
                                (!newest || list[i].start > newest->start))
                                newest = &list[i];
                    if (newest) sr.root = keyOf(*newest);
                }
            } else if (!rootAlive) {
                sr.root = Key();
            }
            if (sr.root.pid) {
                std::deque<int> q{sr.root.pid};
                while (!q.empty()) {
                    const int pid = q.front();
                    q.pop_front();
                    if (!sessionOf.emplace(pid, sr.session).second) continue;
                    auto k = kids.find(pid);
                    if (k != kids.end())
                        for (size_t i : k->second) q.push_back(list[i].pid);
                }
            }
            kept[t.id] = sr;
        }
        sessions_.swap(kept);
    }
    auto takeSession = [&](Proc& p) {
        if (!p.session.empty()) return;
        auto s = sessionOf.find(p.key.pid);
        if (s != sessionOf.end()) p.session = s->second;
    };
    for (const Terminal& t : terminals) {
        if (t.shellPid <= 0 || !byPid.count(t.shellPid)) continue;
        walk(t.shellPid, [&](Proc& p, bool fresh) {
            if (fresh || p.origin == Origin::Outside) takeTerminal(p, t);
            else if (p.terminal == t.id) p.terminalTitle = t.title;
            takeSession(p);
        });
    }
    std::vector<Key> roots;
    for (const auto& kv : procs_)
        if (kv.second.alive && kv.second.origin == Origin::Terminal &&
            !fromTerminal.count(kv.first.pid))
            roots.push_back(kv.first);
    for (const Key& r : roots) {
        auto it = procs_.find(r);
        if (it == procs_.end() || fromTerminal.count(r.pid)) continue;
        fromTerminal.insert(r.pid);
        const Proc origin = it->second;   // a copy: walk may grow the map
        walk(r.pid, [&](Proc& p, bool fresh) {
            if (fresh || p.origin == Origin::Outside) {
                p.origin = Origin::Terminal;
                p.terminal = origin.terminal;
                p.terminalTitle = origin.terminalTitle;
                p.window = origin.window;
                p.session = origin.session;
            }
        });
    }

    // Processes elsewhere whose working folder is in a watched folder.
    size_t outside = 0;
    for (const auto& kv : procs_)
        if (kv.second.origin == Origin::Outside) outside++;
    for (const Basic& b : list) {
        if (b.uid != uid || b.pid <= 1 || appTree.count(b.pid) || fromTerminal.count(b.pid) ||
            shells.count(b.pid))
            continue;
        auto it = procs_.find(keyOf(b));
        if (it != procs_.end() && it->second.origin == Origin::Terminal) continue;
        if (folders.empty() && it == procs_.end()) continue;
        std::string dir;
        bool in = false;
        if (!folders.empty() && source.cwd(b.pid, dir))
            for (const std::string& f : folders)
                if (isInside(dir, f)) { in = true; break; }
        if (it != procs_.end()) {
            if (!in) {
                procs_.erase(it);
                outside--;
            } else {
                it->second.cwd = dir;
            }
            continue;
        }
        if (!in || outside >= kMaxOutside) continue;
        Proc& p = procs_[keyOf(b)];
        p.key = keyOf(b);
        p.origin = Origin::Outside;
        p.cwd = dir;
        outside++;
    }

    // What each live one is doing now.
    for (auto& kv : procs_) {
        Proc& p = kv.second;
        if (!p.alive) continue;
        const Basic* b = liveBasic(list, byPid, p.key);
        if (!b) continue;
        p.ppid = b->ppid;
        p.pgid = b->pgid;
        p.name = b->name;
        if (p.origin == Origin::Terminal) {
            std::string dir;
            if (source.cwd(p.key.pid, dir)) p.cwd = dir;
        }
        // Read again when the short name changes: a fork seen before its
        // exec showed the shell's command line.
        if (p.command.empty() || p.commandFor != p.name) {
            std::string cmd;
            if (source.commandLine(p.key.pid, cmd) && !cmd.empty()) p.command = cmd;
            else if (p.command.empty() || p.commandFor != p.name) p.command = p.name;
            p.commandFor = p.name;
        }
        Detail d;
        if (!source.detail(p.key.pid, d)) continue;
        st.detailed++;
        p.memory = d.memory;
        p.ports = d.ports;
        if (!p.cpuKnown) {
            p.cpuKnown = true;
            p.cpuNanos = d.cpuNanos;
            p.cpuAt = now;
            p.cpu = 0;
        } else if (now - p.cpuAt >= 0.2) {
            const double dt = now - p.cpuAt;
            const uint64_t dn = d.cpuNanos >= p.cpuNanos ? d.cpuNanos - p.cpuNanos : 0;
            p.cpu = (double)dn / 1e9 / dt * 100.0;
            p.cpuNanos = d.cpuNanos;
            p.cpuAt = now;
        }
    }
    list_ = std::move(list);
    st.tracked = procs_.size();
    return st;
}

std::vector<Proc> Tracker::snapshot() const {
    std::vector<Proc> out;
    out.reserve(procs_.size());
    for (const auto& kv : procs_) out.push_back(kv.second);
    return out;
}

// ---------------------------------------------------------------- view
namespace {

bool byStart(const Proc& a, const Proc& b) {
    return a.key.start != b.key.start ? a.key.start < b.key.start : a.key.pid < b.key.pid;
}

std::vector<Node> buildTree(std::vector<Proc> ps) {
    std::sort(ps.begin(), ps.end(), byStart);
    const size_t n = ps.size();
    std::vector<int> parent(n, -1);
    std::map<int, size_t> byPid;
    for (size_t i = 0; i < n; i++) {
        // A live process holds its pid; an exited one only when no live one does.
        auto it = byPid.find(ps[i].key.pid);
        if (it == byPid.end() || (!ps[it->second].alive && ps[i].alive)) byPid[ps[i].key.pid] = i;
    }
    for (size_t i = 0; i < n; i++) {
        auto it = byPid.find(ps[i].ppid);
        if (it == byPid.end() || it->second == i) continue;
        if (ps[it->second].key.start > ps[i].key.start) continue;
        parent[i] = (int)it->second;
    }
    // Break any cycle the table could show mid-change.
    for (size_t i = 0; i < n; i++) {
        size_t steps = 0;
        int j = parent[i];
        while (j >= 0 && steps <= n) {
            if ((size_t)j == i) { parent[i] = -1; break; }
            j = parent[(size_t)j];
            steps++;
        }
    }
    std::vector<std::vector<size_t>> children(n);
    std::vector<size_t> top;
    for (size_t i = 0; i < n; i++) {
        if (parent[i] >= 0) children[(size_t)parent[i]].push_back(i);
        else top.push_back(i);
    }
    std::function<Node(size_t)> make = [&](size_t i) {
        Node node;
        node.proc = ps[i];
        for (size_t c : children[i]) node.children.push_back(make(c));
        return node;
    };
    std::vector<Node> roots;
    for (size_t i : top) roots.push_back(make(i));
    return roots;
}

void finishGroup(Group& g, std::vector<Proc> members) {
    for (const Proc& p : members)
        if (p.alive) {
            g.memory += p.memory;
            g.count++;
        }
    g.roots = buildTree(std::move(members));
}

}  // namespace

View buildView(const std::vector<Proc>& procs, const std::vector<Terminal>& terminals, int window,
               const std::string& folder) {
    View v;
    std::map<int, const Terminal*> live;
    std::vector<Group> groups;
    std::map<int, size_t> groupOf;
    for (const Terminal& t : terminals) {
        live[t.id] = &t;
        if (window > 0 && t.window == window && !groupOf.count(t.id)) {
            Group g;
            g.kind = GroupKind::Terminal;
            g.terminal = t.id;
            g.title = t.title.empty() ? "Terminal " + std::to_string(t.id) : t.title;
            groupOf[t.id] = groups.size();
            groups.push_back(g);
        }
    }
    std::vector<std::vector<Proc>> members(groups.size());
    std::vector<Proc> orphans, outside;
    for (const Proc& p : procs) {
        if (p.origin == Origin::Terminal) {
            if (live.count(p.terminal)) {
                auto g = groupOf.find(p.terminal);
                if (g != groupOf.end()) members[g->second].push_back(p);
            } else if ((window > 0 && p.window == window) || isInside(p.cwd, folder)) {
                orphans.push_back(p);
            }
        } else if (isInside(p.cwd, folder)) {
            outside.push_back(p);
        }
    }
    for (size_t i = 0; i < groups.size(); i++) {
        if (members[i].empty()) continue;
        finishGroup(groups[i], std::move(members[i]));
        v.groups.push_back(std::move(groups[i]));
    }
    if (!orphans.empty()) {
        Group g;
        g.kind = GroupKind::Orphans;
        g.title = "Orphans";
        finishGroup(g, std::move(orphans));
        v.groups.push_back(std::move(g));
    }
    if (!outside.empty()) {
        Group g;
        g.kind = GroupKind::Outside;
        g.title = "Not started here";
        finishGroup(g, std::move(outside));
        v.groups.push_back(std::move(g));
    }
    for (const Group& g : v.groups) {
        v.memory += g.memory;
        v.count += g.count;
    }
    return v;
}

// ------------------------------------------------------------- words
std::string formatBytes(uint64_t bytes) {
    char buf[32];
    const double kb = 1024.0, mb = kb * 1024, gb = mb * 1024;
    const double b = (double)bytes;
    if (b < mb) std::snprintf(buf, sizeof buf, "%.0f KB", b / kb);
    else if (b < 100 * mb) std::snprintf(buf, sizeof buf, "%.1f MB", b / mb);
    else if (b < gb) std::snprintf(buf, sizeof buf, "%.0f MB", b / mb);
    else std::snprintf(buf, sizeof buf, "%.1f GB", b / gb);
    return buf;
}

std::string summary(uint64_t memory, int count) {
    if (count <= 0) return "No processes";
    return formatBytes(memory) + " in " + std::to_string(count) +
           (count == 1 ? " process" : " processes");
}

std::string formatCpu(double percent) {
    char buf[32];
    if (percent < 0.05) return "0%";
    if (percent < 10) std::snprintf(buf, sizeof buf, "%.1f%%", percent);
    else std::snprintf(buf, sizeof buf, "%.0f%%", percent);
    return buf;
}

std::string formatPorts(const std::vector<Listen>& ports) {
    std::string out;
    std::set<int> seen;
    for (const Listen& l : ports) {
        if (!seen.insert(l.port).second) continue;
        if (!out.empty()) out += ", ";
        out += std::to_string(l.port);
    }
    return out;
}

std::string displayName(const Proc& p) {
    // The first word of the command line, without its folder.
    std::string first = p.command.substr(0, p.command.find(' '));
    size_t slash = first.rfind('/');
    if (slash != std::string::npos) first = first.substr(slash + 1);
    if (!first.empty() && first[0] == '-') first = first.substr(1);   // a login shell
    return first.empty() ? p.name : first;
}

const char* helpText() {
    return "What this window's terminals started, and what other programs run in this "
           "folder. A process that detaches from its terminal between two looks, a few "
           "seconds apart, can be missed. Nothing here is ever stopped unless you ask.";
}

// ---------------------------------------------------------------- JSON
namespace {

const char* kindName(GroupKind k) {
    switch (k) {
    case GroupKind::Terminal: return "terminal";
    case GroupKind::Orphans: return "orphans";
    case GroupKind::Outside: return "outside";
    }
    return "terminal";
}

void flatten(const Node& n, int depth, Json& out) {
    const Proc& p = n.proc;
    Json o = Json::object();
    o.set("pid", p.key.pid);
    o.set("ppid", p.ppid);
    o.set("pgid", p.pgid);
    o.set("start", (double)p.key.start);
    o.set("name", displayName(p));
    o.set("command", p.command);
    o.set("cwd", p.cwd);
    o.set("memory", (double)p.memory);
    o.set("cpu", p.cpu);
    Json ports = Json::array();
    for (const Listen& l : p.ports) {
        Json pj = Json::object();
        pj.set("address", l.address);
        pj.set("port", l.port);
        ports.push(pj);
    }
    o.set("ports", ports);
    o.set("depth", depth);
    o.set("alive", p.alive);
    o.set("origin", p.origin == Origin::Terminal ? "terminal" : "outside");
    if (p.origin == Origin::Terminal) {
        o.set("terminal", p.terminal);
        o.set("terminal_title", p.terminalTitle);
    }
    if (!p.session.empty()) o.set("session", p.session);
    out.push(o);
    for (const Node& c : n.children) flatten(c, depth + 1, out);
}

std::string pad(const std::string& s, size_t width, bool right) {
    // Widths in characters; the columns padded are ASCII.
    if (s.size() >= width) return s;
    return right ? std::string(width - s.size(), ' ') + s : s + std::string(width - s.size(), ' ');
}

}  // namespace

Json toJson(const View& view, const std::string& folder, int window) {
    Json r = Json::object();
    r.set("folder", folder);
    if (window > 0) r.set("window", window);
    r.set("memory", (double)view.memory);
    r.set("count", view.count);
    r.set("summary", summary(view.memory, view.count));
    Json groups = Json::array();
    for (const Group& g : view.groups) {
        Json gj = Json::object();
        gj.set("kind", kindName(g.kind));
        gj.set("title", g.title);
        if (g.kind == GroupKind::Terminal) gj.set("terminal", g.terminal);
        gj.set("memory", (double)g.memory);
        gj.set("count", g.count);
        Json ps = Json::array();
        for (const Node& n : g.roots) flatten(n, 0, ps);
        gj.set("processes", ps);
        groups.push(gj);
    }
    r.set("groups", groups);
    r.set("help", helpText());
    return r;
}

std::string formatTable(const Json& j) {
    std::string out = j["summary"].asString("No processes");
    if (j.has("folder")) out += " in " + j["folder"].asString();
    out += "\n";
    for (const Json& g : j["groups"].elements()) {
        std::string head = g["title"].asString();
        if (g["kind"].asString() == "terminal")
            head += " (terminal " + std::to_string(g["terminal"].asInt()) + ")";
        out += "\n" + head + ", " + formatBytes((uint64_t)g["memory"].asNumber()) + "\n";
        out += "  " + pad("PID", 7, true) + "  " + pad("MEMORY", 9, true) + "  " +
               pad("CPU", 5, true) + "  " + pad("PORTS", 12, false) + "  COMMAND\n";
        for (const Json& p : g["processes"].elements()) {
            const bool alive = p["alive"].asBool(true);
            std::string ports;
            std::set<int> seen;
            for (const Json& l : p["ports"].elements()) {
                const int port = (int)l["port"].asInt();
                if (!seen.insert(port).second) continue;
                if (!ports.empty()) ports += ",";
                ports += std::to_string(port);
            }
            std::string cmd = std::string((size_t)p["depth"].asInt() * 2, ' ') +
                              p["command"].asString();
            if (cmd.size() > 120) cmd = cmd.substr(0, 117) + "...";
            if (!alive) cmd += " (exited)";
            out += "  " + pad(std::to_string(p["pid"].asInt()), 7, true) + "  " +
                   pad(alive ? formatBytes((uint64_t)p["memory"].asNumber()) : "-", 9, true) +
                   "  " + pad(alive ? formatCpu(p["cpu"].asNumber()) : "-", 5, true) + "  " +
                   pad(ports.empty() ? "-" : ports, 12, false) + "  " + cmd + "\n";
        }
    }
    return out;
}

// ----------------------------------------------------------------- Stop
StopPlan planStop(const std::vector<Basic>& table, const Key& target, int selfPid, int selfPgid,
                  const std::vector<int>& shellPids) {
    StopPlan plan;
    const std::set<int> shells(shellPids.begin(), shellPids.end());
    const Basic* t = nullptr;
    std::map<int, std::vector<size_t>> kids;
    for (size_t i = 0; i < table.size(); i++) {
        if (table[i].pid == target.pid && table[i].start == target.start) t = &table[i];
        if (table[i].ppid != table[i].pid) kids[table[i].ppid].push_back(i);
    }
    if (!t) { plan.refusal = "it has already exited"; return plan; }
    if (t->pid <= 1) { plan.refusal = "launchd and the kernel are never stopped"; return plan; }
    if (t->pid == selfPid) { plan.refusal = "that is minicode itself"; return plan; }
    if (shells.count(t->pid)) {
        plan.refusal = "that is a terminal's shell: close the terminal instead";
        return plan;
    }
    // The process and what it started.
    std::set<int> subtree;
    std::vector<Key> subtreeKeys;
    std::deque<const Basic*> q{t};
    while (!q.empty()) {
        const Basic* b = q.front();
        q.pop_front();
        if (b->pid <= 1 || b->pid == selfPid || shells.count(b->pid)) continue;
        if (!subtree.insert(b->pid).second) continue;
        subtreeKeys.push_back(keyOf(*b));
        auto k = kids.find(b->pid);
        if (k != kids.end())
            for (size_t i : k->second) q.push_back(&table[i]);
    }
    bool group = t->pgid > 1 && t->pgid != selfPgid;
    std::vector<Key> members;
    if (group)
        for (const Basic& b : table)
            if (b.pgid == t->pgid) {
                members.push_back(keyOf(b));
                if (!subtree.count(b.pid)) { group = false; break; }
            }
    plan.ok = true;
    plan.group = group;
    plan.pgid = group ? t->pgid : 0;
    plan.targets = subtreeKeys;
    if (group)
        for (const Key& m : members)
            if (std::find(plan.targets.begin(), plan.targets.end(), m) == plan.targets.end())
                plan.targets.push_back(m);
    return plan;
}

}  // namespace Procs
