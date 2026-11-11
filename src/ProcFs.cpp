// ProcFs.cpp — see ProcFs.h.
#include "ProcFs.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace ProcFs {

namespace {

bool readFile(const std::string& path, std::string& out, size_t limit = 8u << 20) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
        if (out.size() >= limit) break;
    }
    fclose(f);
    return true;
}

bool allDigits(const char* s) {
    if (!*s) return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return false;
    return true;
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> w;
    std::istringstream in(s);
    std::string x;
    while (in >> x) w.push_back(x);
    return w;
}

unsigned hexByte(const std::string& s, size_t at) {
    return (unsigned)strtoul(s.substr(at, 2).c_str(), nullptr, 16);
}

std::string decodeAddress(const std::string& hex, bool v6) {
    if (!v6 && hex.size() == 8) {
        // One host-order word, little-endian on every machine this runs on.
        unsigned char b[4];
        for (int i = 0; i < 4; i++) b[3 - i] = (unsigned char)hexByte(hex, i * 2);
        if (!b[0] && !b[1] && !b[2] && !b[3]) return "*";
        char out[32];
        snprintf(out, sizeof out, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        return out;
    }
    if (v6 && hex.size() == 32) {
        // Four host-order words.
        unsigned char b[16];
        for (int w = 0; w < 4; w++)
            for (int i = 0; i < 4; i++) b[w * 4 + 3 - i] = (unsigned char)hexByte(hex, w * 8 + i * 2);
        bool zero = true;
        for (unsigned char c : b) zero = zero && c == 0;
        if (zero) return "*";
        const bool mapped = [&] {
            for (int i = 0; i < 10; i++)
                if (b[i]) return false;
            return b[10] == 0xff && b[11] == 0xff;
        }();
        char out[INET6_ADDRSTRLEN] = {0};
        if (mapped) {
            snprintf(out, sizeof out, "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
            return out;
        }
        if (inet_ntop(AF_INET6, b, out, sizeof out)) return out;
    }
    return hex;
}

}  // namespace

bool parseStat(const std::string& text, Stat& out) {
    const size_t open = text.find('(');
    const size_t close = text.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return false;
    out.pid = atoi(text.c_str());
    out.name = text.substr(open + 1, close - open - 1);
    const std::vector<std::string> f = words(text.substr(close + 1));
    // f[0] is field 3 (state), so field n is f[n - 3].
    if (f.size() < 20) return false;
    out.ppid = atoi(f[1].c_str());
    out.pgid = atoi(f[2].c_str());
    out.utime = strtoull(f[11].c_str(), nullptr, 10);
    out.stime = strtoull(f[12].c_str(), nullptr, 10);
    out.startTicks = strtoull(f[19].c_str(), nullptr, 10);
    return out.pid > 0;
}

int64_t parseBootTime(const std::string& text) {
    const size_t at = text.find("btime ");
    if (at == std::string::npos || (at && text[at - 1] != '\n')) return 0;
    return strtoll(text.c_str() + at + 6, nullptr, 10);
}

std::string joinCommandLine(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        if (out.size() >= 4096) break;
        out += (c == 0 || (unsigned char)c < 0x20 || c == 0x7f) ? ' ' : c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

uint64_t parsePss(const std::string& text) {
    size_t at = text.find("Pss:");
    if (at == std::string::npos) return 0;
    return strtoull(text.c_str() + at + 4, nullptr, 10) * 1024;   // kB
}

uint64_t parseStatmResident(const std::string& text, uint64_t pageSize) {
    const std::vector<std::string> f = words(text);
    return f.size() < 2 ? 0 : strtoull(f[1].c_str(), nullptr, 10) * pageSize;
}

std::vector<TcpEntry> parseListening(const std::string& text, bool v6) {
    std::vector<TcpEntry> out;
    std::istringstream in(text);
    std::string line;
    std::getline(in, line);   // the header
    while (std::getline(in, line)) {
        const std::vector<std::string> f = words(line);
        // sl local rem st tx:rx tr:when retrnsmt uid timeout inode
        if (f.size() < 10 || f[3] != "0A") continue;
        const size_t colon = f[1].rfind(':');
        if (colon == std::string::npos) continue;
        TcpEntry e;
        e.address = decodeAddress(f[1].substr(0, colon), v6);
        e.port = (int)strtol(f[1].c_str() + colon + 1, nullptr, 16);
        e.inode = strtoull(f[9].c_str(), nullptr, 10);
        if (e.port > 0 && e.inode) out.push_back(e);
    }
    return out;
}

uint64_t socketInode(const std::string& link) {
    if (link.compare(0, 8, "socket:[") != 0 || link.back() != ']') return 0;
    return strtoull(link.c_str() + 8, nullptr, 10);
}

// ------------------------------------------------------------ the source

Source::Source(const std::string& root) : root_(root) {
    std::string text;
    int64_t btime = 0;
    if (readFile(root_ + "/stat", text)) btime = parseBootTime(text);
    boot_ = btime * 1000000;
    ticks_ = sysconf(_SC_CLK_TCK);
    if (ticks_ <= 0) ticks_ = 100;
    const long ps = sysconf(_SC_PAGESIZE);
    if (ps > 0) page_ = (uint64_t)ps;
}

std::vector<Procs::Basic> Source::list() {
    tcpRead_ = false;   // the listening sockets are read again for this look
    std::vector<Procs::Basic> out;
    DIR* d = opendir(root_.c_str());
    if (!d) return out;
    const uid_t me = geteuid();
    while (dirent* e = readdir(d)) {
        if (!allDigits(e->d_name)) continue;
        const std::string dir = root_ + "/" + e->d_name;
        struct stat st;
        if (stat(dir.c_str(), &st) != 0 || st.st_uid != me) continue;
        std::string text;
        Stat s;
        if (!readFile(dir + "/stat", text) || !parseStat(text, s)) continue;
        Procs::Basic b;
        b.pid = s.pid;
        b.ppid = s.ppid;
        b.pgid = s.pgid;
        b.uid = st.st_uid;
        b.start = boot_ + (int64_t)(s.startTicks * 1000000ull / (uint64_t)ticks_);
        b.name = s.name;
        out.push_back(b);
    }
    closedir(d);
    return out;
}

bool Source::cwd(int pid, std::string& out) {
    char buf[4096];
    const std::string link = root_ + "/" + std::to_string(pid) + "/cwd";
    const ssize_t n = readlink(link.c_str(), buf, sizeof buf - 1);
    if (n <= 0) return false;
    out.assign(buf, (size_t)n);
    return true;
}

bool Source::commandLine(int pid, std::string& out) {
    std::string raw;
    if (!readFile(root_ + "/" + std::to_string(pid) + "/cmdline", raw, 8192)) return false;
    out = joinCommandLine(raw);
    return !out.empty();
}

bool Source::detail(int pid, Procs::Detail& out) {
    const std::string dir = root_ + "/" + std::to_string(pid);
    std::string text;
    Stat s;
    if (!readFile(dir + "/stat", text) || !parseStat(text, s)) return false;
    out.cpuNanos = (s.utime + s.stime) * (1000000000ull / (uint64_t)ticks_);
    out.memory = 0;
    if (readFile(dir + "/smaps_rollup", text)) out.memory = parsePss(text);
    if (!out.memory && readFile(dir + "/statm", text)) out.memory = parseStatmResident(text, page_);
    out.ports.clear();
    if (!tcpRead_) {
        listening_.clear();
        std::string t4, t6;
        if (readFile(root_ + "/net/tcp", t4)) listening_ = parseListening(t4, false);
        if (readFile(root_ + "/net/tcp6", t6)) {
            std::vector<TcpEntry> more = parseListening(t6, true);
            listening_.insert(listening_.end(), more.begin(), more.end());
        }
        tcpRead_ = true;
    }
    if (!listening_.empty()) {
        const std::string fdDir = dir + "/fd";
        if (DIR* d = opendir(fdDir.c_str())) {
            while (dirent* e = readdir(d)) {
                if (!allDigits(e->d_name)) continue;
                char buf[128];
                const ssize_t n = readlink((fdDir + "/" + e->d_name).c_str(), buf, sizeof buf - 1);
                if (n <= 0) continue;
                buf[n] = 0;
                const uint64_t inode = socketInode(buf);
                if (!inode) continue;
                for (const TcpEntry& t : listening_) {
                    if (t.inode != inode) continue;
                    bool dup = false;
                    for (const Procs::Listen& l : out.ports)
                        dup = dup || (l.port == t.port && l.address == t.address);
                    if (!dup) out.ports.push_back({t.address, t.port});
                }
            }
            closedir(d);
        }
    }
    return true;
}

int64_t Source::startTimeOf(int pid) {
    std::string text;
    Stat s;
    if (!readFile(root_ + "/" + std::to_string(pid) + "/stat", text) || !parseStat(text, s)) return 0;
    return boot_ + (int64_t)(s.startTicks * 1000000ull / (uint64_t)ticks_);
}

}  // namespace ProcFs
