// procfs_tests.inc — the `procfs:*` groups of run_tests.cpp (ProcFs.h): the
// /proc parsers on canned text, and the Source over a folder shaped like
// /proc, so the Linux sampler's reading is tested on every CI job.
#include "ProcFs.h"
#include <sys/stat.h>
#include <unistd.h>

static void testProcFs() {
    GROUP("procfs:stat");
    {
        // A name with spaces and parentheses: fields count from the last ')'.
        const std::string line =
            "4242 (my (odd) name) S 100 4242 100 34816 4242 4194560 1 0 0 0 150 50 0 0 20 0 3 0 "
            "987654 1000000 500 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 2 0 0 0 0 0\n";
        ProcFs::Stat s;
        CHECK(ProcFs::parseStat(line, s));
        CHECK(s.pid == 4242 && s.name == "my (odd) name");
        CHECK(s.ppid == 100 && s.pgid == 4242);
        CHECK(s.utime == 150 && s.stime == 50 && s.startTicks == 987654);
        CHECK(!ProcFs::parseStat("garbage", s));
        CHECK(!ProcFs::parseStat("1 (x) S 1 2", s));   // cut short
        CHECK(ProcFs::parseBootTime("cpu 1 2 3\nbtime 1700000000\nprocesses 5\n") == 1700000000);
        CHECK(ProcFs::parseBootTime("cpu 1 2 3\n") == 0);
    }

    GROUP("procfs:words");
    {
        CHECK(ProcFs::joinCommandLine(std::string("python3\0-m\0http.server\0", 24)) ==
              "python3 -m http.server");
        CHECK(ProcFs::joinCommandLine(std::string("a\tb\0", 4)) == "a b");
        CHECK(ProcFs::joinCommandLine("").empty());
        CHECK(ProcFs::joinCommandLine(std::string(10000, 'x')).size() == 4096);
        CHECK(ProcFs::parsePss("Rss:   9000 kB\nPss:    2048 kB\nPss_Anon:  1 kB\n") == 2048u * 1024);
        CHECK(ProcFs::parsePss("nothing") == 0);
        CHECK(ProcFs::parseStatmResident("1000 250 100 1 0 200 0", 4096) == 250u * 4096);
        CHECK(ProcFs::socketInode("socket:[12345]") == 12345);
        CHECK(ProcFs::socketInode("pipe:[12345]") == 0 && ProcFs::socketInode("/dev/null") == 0);
    }

    GROUP("procfs:tcp");
    {
        const std::string t4 =
            "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
            "   0: 0100007F:1F90 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 11111 1 0\n"
            "   1: 00000000:0050 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 22222 1 0\n"
            "   2: 0100007F:D431 0100007F:1F90 01 00000000:00000000 00:00000000 00000000  1000        0 33333 1 0\n";
        std::vector<ProcFs::TcpEntry> v = ProcFs::parseListening(t4, false);
        CHECK(v.size() == 2);   // the established one is no listener
        CHECK(v[0].address == "127.0.0.1" && v[0].port == 8080 && v[0].inode == 11111);
        CHECK(v[1].address == "*" && v[1].port == 80 && v[1].inode == 22222);
        const std::string t6 =
            "  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
            "   0: 00000000000000000000000001000000:2328 00000000000000000000000000000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 44444 1 0\n"
            "   1: 00000000000000000000000000000000:1BBD 00000000000000000000000000000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 55555 1 0\n"
            "   2: 0000000000000000FFFF00000100007F:0016 00000000000000000000000000000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 66666 1 0\n";
        v = ProcFs::parseListening(t6, true);
        CHECK(v.size() == 3);
        CHECK(v[0].address == "::1" && v[0].port == 9000 && v[0].inode == 44444);
        CHECK(v[1].address == "*" && v[1].port == 7101);
        CHECK(v[2].address == "127.0.0.1" && v[2].port == 22);
        CHECK(ProcFs::parseListening("", false).empty());
        CHECK(ProcFs::parseListening("header\n 0: bad\n", false).empty());
    }

    GROUP("procfs:source");
    {
        // A folder shaped like /proc: two of the caller's processes, one with
        // a listening socket, and a non-numeric entry that is no process.
        char tmpl[] = "/tmp/mc-procfs-XXXXXX";
        const char* root = mkdtemp(tmpl);
        CHECK(root != nullptr);
        if (root) {
            const std::string r = root;
            auto put = [&](const std::string& rel, const std::string& text) {
                std::string path = r + "/" + rel;
                for (size_t i = r.size() + 1; i < path.size(); i++)
                    if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0700);
                FILE* f = fopen(path.c_str(), "wb");
                if (f) { fwrite(text.data(), 1, text.size(), f); fclose(f); }
            };
            put("stat", "cpu  1 2 3\nbtime 1700000000\n");
            put("7001/stat", "7001 (python3) S 1 7001 7001 0 -1 0 0 0 0 0 300 100 0 0 20 0 1 0 "
                             "200000 0 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
            put("7001/cmdline", std::string("python3\0-m\0http.server\0", 24));
            put("7001/statm", "100 50 10 1 0 40 0\n");
            put("7001/smaps_rollup", "Rss:  200 kB\nPss:  100 kB\n");
            put("7002/stat", "7002 (sleep) S 7001 7001 7001 0 -1 0 0 0 0 0 1 1 0 0 20 0 1 0 "
                             "200050 0 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
            put("7002/statm", "100 50 10 1 0 40 0\n");   // no smaps_rollup: RSS
            put("self/stat", "not a process");
            put("net/tcp",
                "  sl  local_address rem_address st tx rx tr tm retrnsmt uid timeout inode\n"
                "   0: 0100007F:1F90 00000000:0000 0A 0:0 0:0 0 1000 0 99999 1 0\n");
            put("net/tcp6", "header\n");
            mkdir((r + "/7001/fd").c_str(), 0700);
            CHECK(symlink("socket:[99999]", (r + "/7001/fd/3").c_str()) == 0);
            CHECK(symlink("/dev/null", (r + "/7001/fd/0").c_str()) == 0);
            CHECK(symlink("/tmp", (r + "/7001/cwd").c_str()) == 0);

            ProcFs::Source src(r);
            std::vector<Procs::Basic> l = src.list();
            CHECK(l.size() == 2);
            std::sort(l.begin(), l.end(),
                      [](const Procs::Basic& a, const Procs::Basic& b) { return a.pid < b.pid; });
            if (l.size() == 2) {
                CHECK(l[0].pid == 7001 && l[0].name == "python3" && l[0].ppid == 1);
                CHECK(l[1].pid == 7002 && l[1].ppid == 7001 && l[1].pgid == 7001);
                CHECK(l[0].uid == (uint32_t)geteuid());
                const long hz = sysconf(_SC_CLK_TCK);
                CHECK(l[0].start == 1700000000LL * 1000000 + 200000LL * 1000000 / hz);
                CHECK(l[1].start > l[0].start);
                CHECK(src.startTimeOf(7001) == l[0].start && src.startTimeOf(31999) == 0);
            }
            std::string s;
            CHECK(src.cwd(7001, s) && s == "/tmp");
            CHECK(!src.cwd(7002, s));   // unreadable: skipped
            CHECK(src.commandLine(7001, s) && s == "python3 -m http.server");
            CHECK(!src.commandLine(7002, s));
            Procs::Detail d;
            CHECK(src.detail(7001, d));
            CHECK(d.memory == 100u * 1024);   // Pss
            CHECK(d.cpuNanos == 400ull * (1000000000ull / (uint64_t)sysconf(_SC_CLK_TCK)));
            CHECK(d.ports.size() == 1 && d.ports[0].port == 8080 && d.ports[0].address == "127.0.0.1");
            CHECK(src.detail(7002, d) && d.memory == 50u * (uint64_t)sysconf(_SC_PAGESIZE) &&
                  d.ports.empty());
            CHECK(!src.detail(31999, d));
            std::string rm = "rm -rf '" + r + "'";
            CHECK(system(rm.c_str()) == 0);
        }
        // A missing root lists nothing and does not crash.
        ProcFs::Source none("/tmp/mc-no-such-proc-folder");
        CHECK(none.list().empty());
    }

    GROUP("procfs:live");
    {
        // The real /proc, where there is one: this very process is listed,
        // with its cwd, command line, memory and CPU time.
        struct stat st;
        if (stat("/proc/self/stat", &st) == 0) {
            ProcFs::Source src;
            bool found = false;
            for (const Procs::Basic& b : src.list()) found = found || b.pid == (int)getpid();
            CHECK(found);
            std::string s;
            Procs::Detail d;
            CHECK(src.cwd((int)getpid(), s) && !s.empty());
            CHECK(src.commandLine((int)getpid(), s) && !s.empty());
            CHECK(src.detail((int)getpid(), d) && d.memory > 0);
            CHECK(src.startTimeOf((int)getpid()) > 1600000000LL * 1000000);
        }
    }
}
