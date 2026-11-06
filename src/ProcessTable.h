// ProcessTable.h — pure C++. Processes and ports per folder (spec feature 5):
// what is still running, what it costs, and what started it.
//
// The platform supplies a Source: the process table (pid, parent, process
// group, owner, start time, short name), and for one process its working
// folder, command line, memory, CPU time and the TCP ports it listens on.
// The Mac's is libproc and sysctl (ProcessMonitor.mm); the tests' is a fake.
//
// A Tracker samples a Source now and then and keeps what it found, keyed by
// (pid, start time), so a pid used again is never taken for the old process:
//   - every descendant of a minicode terminal's shell, with that terminal as
//     its origin, kept after it is re-parented to launchd (a daemon, or a job
//     left behind when its terminal closed); its own children follow it.
//     A process that detaches between two samples is missed;
//   - every process of the user's not started from minicode (nor by minicode
//     itself: language servers, git) whose working folder is inside a
//     watched folder ("not started here"), while it stays there;
//   - for each: memory (the physical footprint Activity Monitor shows), CPU
//     over the last two samples, command line, working folder, listening
//     ports. A process that exited is kept, marked, for kForgetAfter seconds.
//
// buildView groups one window's processes as its panel shows them: a group
// per terminal of that window (in the order given), then orphans (their
// terminal closed), then "not started here", each a tree by parent.
// planStop says what Stop signals. Nothing here stops anything by itself.
#pragma once
#include "Json.h"
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Procs {

// A process for as long as it lives: a pid alone can be used again.
struct Key {
    int pid = 0;
    int64_t start = 0;   // start time, microseconds since 1970
    bool operator<(const Key& o) const { return pid != o.pid ? pid < o.pid : start < o.start; }
    bool operator==(const Key& o) const { return pid == o.pid && start == o.start; }
    bool operator!=(const Key& o) const { return !(*this == o); }
};

// One row of the process table.
struct Basic {
    int pid = 0, ppid = 0, pgid = 0;
    uint32_t uid = 0;
    int64_t start = 0;   // microseconds since 1970
    std::string name;    // the kernel's short name (p_comm)
};

struct Listen {
    std::string address;   // "127.0.0.1", "::1", "*"
    int port = 0;
};

struct Detail {
    uint64_t memory = 0;     // bytes, the physical footprint
    uint64_t cpuNanos = 0;   // user + system time so far
    std::vector<Listen> ports;
};

class Source {
public:
    virtual ~Source() = default;
    virtual std::vector<Basic> list() = 0;
    virtual bool cwd(int pid, std::string& out) = 0;
    virtual bool commandLine(int pid, std::string& out) = 0;
    virtual bool detail(int pid, Detail& out) = 0;
};

// A minicode terminal, as the app sees it now.
struct Terminal {
    int id = 0;               // `minicode state`'s terminal id
    std::string title;
    int shellPid = 0;         // 0 while it has no shell
    int window = 0;           // `minicode state`'s window id
    std::string session;      // the Claude session running in it, when known
};

enum class Origin { Terminal, Outside };

struct Proc {
    Key key;
    int ppid = 0, pgid = 0;
    std::string name, command, cwd;
    uint64_t memory = 0;
    double cpu = 0;           // percent of one core
    std::vector<Listen> ports;
    Origin origin = Origin::Terminal;
    int terminal = 0;         // the terminal it came from (Origin::Terminal)
    std::string terminalTitle;
    int window = 0;           // that terminal's window
    std::string session;
    bool alive = true;
    double exitedAt = 0;      // when a sample first missed it
    // CPU bookkeeping.
    uint64_t cpuNanos = 0;
    double cpuAt = 0;
    bool cpuKnown = false;
    std::string commandFor;   // the short name the command line was read under
};

struct SampleStats {
    size_t listed = 0;    // processes on the machine
    size_t tracked = 0;   // kept, exited ones included
    size_t detailed = 0;  // asked for details this time
};

class Tracker {
public:
    static constexpr double kForgetAfter = 60;   // seconds after it exits
    static constexpr size_t kMaxOutside = 2000;  // "not started here" kept at most

    // One look at the table. `folders` are the watched folders (every open
    // window's), resolved; `selfPid` is minicode's own; `uid` the user's.
    SampleStats sample(Source& source, double now, const std::vector<Terminal>& terminals,
                       const std::vector<std::string>& folders, int selfPid, uint32_t uid);

    const std::map<Key, Proc>& processes() const { return procs_; }
    std::vector<Proc> snapshot() const;
    // The process table as last listed (for planStop).
    const std::vector<Basic>& lastList() const { return list_; }

private:
    std::map<Key, Proc> procs_;
    std::vector<Basic> list_;
};

// ------------------------------------------------------------ the panel
struct Node {
    Proc proc;
    std::vector<Node> children;
};

enum class GroupKind { Terminal, Orphans, Outside };

struct Group {
    GroupKind kind = GroupKind::Terminal;
    int terminal = 0;          // GroupKind::Terminal
    std::string title;         // "zsh", "Orphans", "Not started here"
    uint64_t memory = 0;       // of the live processes
    int count = 0;             // live processes
    std::vector<Node> roots;   // by start time, children likewise
};

struct View {
    std::vector<Group> groups;
    uint64_t memory = 0;
    int count = 0;
};

// `window` 0: no window (a folder only), so no terminal groups. `terminals`
// is every live terminal of every window, the window's in tab order.
View buildView(const std::vector<Proc>& procs, const std::vector<Terminal>& terminals,
               int window, const std::string& folder);

// "1.8 GB in 14 processes", "34.2 MB in 1 process", "No processes".
std::string summary(uint64_t memory, int count);
// 1024-based: "512 KB", "34.2 MB", "412 MB", "1.8 GB".
std::string formatBytes(uint64_t bytes);
// "0%", "0.4%", "12%", "250%".
std::string formatCpu(double percent);
// "8123", "8123, 5173" (each port once, in order).
std::string formatPorts(const std::vector<Listen>& ports);
// What a row's first line shows: the program's name from its command line
// (python3 for "/usr/bin/python3 -m http.server"), else the short name.
std::string displayName(const Proc& p);

// Is `path` the folder or inside it (both resolved, no trailing slash)?
bool isInside(const std::string& path, const std::string& folder);

// For `minicode procs`: the view as JSON (groups with their processes
// flattened in tree order, each with a depth), and the text the command
// prints without --json.
Json toJson(const View& view, const std::string& folder, int window);
std::string formatTable(const Json& procs);
// The panel's help text (R5.2).
const char* helpText();

// ----------------------------------------------------------------- Stop
// What Stop signals: the process group when every live member of it is
// the process or one of its descendants (so a group the process shares
// with whatever started it, such as Claude Code, is never signalled), else
// the process and its descendants one by one. Never minicode itself, its
// process group, a terminal's shell, launchd or the kernel.
struct StopPlan {
    bool ok = false;
    std::string refusal;          // why not, when !ok
    bool group = false;           // kill(-pgid)
    int pgid = 0;
    std::vector<Key> targets;     // every process it should end (the group's members too)
};
StopPlan planStop(const std::vector<Basic>& table, const Key& target, int selfPid, int selfPgid,
                  const std::vector<int>& shellPids);

}  // namespace Procs
