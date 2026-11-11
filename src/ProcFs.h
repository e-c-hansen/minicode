// ProcFs.h — Procs::Source over Linux's /proc (PARITY.md item 3.8), the
// seam ProcessTable.h leaves for each port. POSIX only, no GTK; it compiles
// on the Mac too (where /proc is absent and nothing uses it) so the parsers
// are tested by every build from canned /proc-shaped text.
//
// What each field is, against the Mac's libproc:
//   start      field 22 of /proc/<pid>/stat (clock ticks since boot) turned
//              into microseconds since 1970 with /proc/stat's btime, read
//              once per source: btime moves when the clock is stepped, and a
//              key (pid, start) must not change under a process.
//   memory     Pss from smaps_rollup (the proportional share of shared
//              pages), else RSS from statm. The Mac's number is the physical
//              footprint, which counts a process's own dirty pages and
//              charges shared ones to nobody in particular; Pss is the
//              closest Linux has (RSS counts every shared library page in
//              full, so six processes sharing a runtime sum to more than the
//              machine holds). smaps_rollup costs the kernel a page-table
//              walk, a few ms for a big process, so it is read only for
//              processes the Tracker asks details of.
//   cpuNanos   (utime + stime) / sysconf(_SC_CLK_TCK).
//   ports      sockets in LISTEN state in /proc/net/tcp and tcp6, matched
//              to the process by the socket inodes under /proc/<pid>/fd.
// Only the caller's own processes are listed (the owner of /proc/<pid>);
// one whose cwd cannot be read is simply asked for no cwd.
#pragma once
#include "ProcessTable.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ProcFs {

// ----- parsers over text already read (tests feed them canned text)

struct Stat {
    int pid = 0, ppid = 0, pgid = 0;
    std::string name;          // comm, which may hold spaces and parentheses
    uint64_t utime = 0, stime = 0, startTicks = 0;
};
bool parseStat(const std::string& text, Stat& out);

// /proc/stat's "btime" line, seconds since 1970; 0 when absent.
int64_t parseBootTime(const std::string& text);

// cmdline: NUL-separated arguments to one line, controls blanked, at most
// 4096 bytes, like the Mac's.
std::string joinCommandLine(const std::string& raw);

// smaps_rollup's "Pss:" in bytes (0 when absent); statm's resident pages
// times the page size.
uint64_t parsePss(const std::string& text);
uint64_t parseStatmResident(const std::string& text, uint64_t pageSize);

struct TcpEntry {
    std::string address;   // "127.0.0.1", "::1", "*", or the textual address
    int port = 0;
    uint64_t inode = 0;
};
// Listening sockets (state 0A) of /proc/net/tcp (v6 false) or tcp6.
std::vector<TcpEntry> parseListening(const std::string& text, bool v6);

// "socket:[12345]" (a /proc/<pid>/fd link), else 0.
uint64_t socketInode(const std::string& link);

// ----- the source

class Source : public Procs::Source {
public:
    // `root` is "/proc", or a folder shaped like it (the tests').
    explicit Source(const std::string& root = "/proc");
    std::vector<Procs::Basic> list() override;
    bool cwd(int pid, std::string& out) override;
    bool commandLine(int pid, std::string& out) override;
    bool detail(int pid, Procs::Detail& out) override;
    // Start time of `pid` now in microseconds since 1970, 0 when gone: the
    // check before a signal.
    int64_t startTimeOf(int pid);

private:
    std::string root_;
    int64_t boot_ = 0;      // microseconds since 1970
    long ticks_ = 100;
    uint64_t page_ = 4096;
    bool tcpRead_ = false;
    std::vector<TcpEntry> listening_;   // read once per list()
};

}  // namespace ProcFs
