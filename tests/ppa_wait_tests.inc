// ppa_wait_tests.inc — the `ppa:*` and `ci:*` groups of run_tests.cpp:
// scripts/ppa-wait.sh, which ppa.sh and release.sh run after an upload to
// wait for Launchpad's build and fail unless it built, played against
// canned answers of Launchpad's getBuildRecords (MINICODE_PPA_ANSWERS: no
// network, and time passes by the poll interval per look without sleeping);
// and the CI workflow's job time limits. Run from the repo root; skipped
// where the script, bash or python3 is missing (the Debian package's build
// on Launchpad has no python3).

#include <sys/wait.h>

namespace {

namespace pfs = std::filesystem;

// One build record as getBuildRecords lists it (the fields the script reads).
std::string lpRecord(const std::string &version, const std::string &state,
                     const std::string &arch = "amd64", const std::string &deps = "") {
    return "{\"source_package_name\": \"minicode\", \"source_package_version\": \"" + version +
           "\", \"arch_tag\": \"" + arch + "\", \"buildstate\": \"" + state +
           "\", \"dependencies\": " + (deps.empty() ? "null" : "\"" + deps + "\"") +
           ", \"build_log_url\": \"https://launchpad.test/log/" + version + "-" + arch +
           "\", \"web_link\": \"https://launchpad.test/build/" + arch + "\"}";
}

std::string lpAnswer(const std::vector<std::string> &records) {
    std::string s = "{\"start\": 0, \"total_size\": " + std::to_string(records.size()) +
                    ", \"entries\": [";
    for (size_t i = 0; i < records.size(); i++) s += (i ? ", " : "") + records[i];
    return s + "]}";
}

struct PpaWait {
    int status = -1;
    std::string out;
};

// Runs scripts/ppa-wait.sh `args` with the answers given, one a look ("FAIL"
// for a look that cannot read Launchpad), the last repeating.
PpaWait runPpaWait(const pfs::path &root, const std::string &name,
                   const std::vector<std::string> &answers, const std::string &args,
                   const std::string &env = "") {
    const pfs::path dir = root / name;
    pfs::create_directories(dir);
    for (size_t i = 0; i < answers.size(); i++) {
        const bool fail = answers[i] == "FAIL";
        std::ofstream(dir / (std::to_string(i + 1) + (fail ? ".fail" : ".json"))) << answers[i];
    }
    const pfs::path out = root / (name + ".out");
    const std::string cmd = "env -u MINICODE_PPA -u MINICODE_PPA_SERIES -u MINICODE_PPA_WAIT_MINUTES"
                            " -u MINICODE_PPA_ACCEPT_MINUTES -u MINICODE_PPA_POLL_SECONDS"
                            " MINICODE_PPA_ANSWERS='" + dir.string() + "' " + env +
                            " bash scripts/ppa-wait.sh " + args + " > '" + out.string() + "' 2>&1";
    const int rc = std::system(cmd.c_str());
    PpaWait r;
    if (rc != -1 && WIFEXITED(rc)) r.status = WEXITSTATUS(rc);
    r.out = readFile(out.c_str());
    return r;
}

bool ppaHas(const std::string &s, const std::string &what) {
    return s.find(what) != std::string::npos;
}

}  // namespace

void testPpaWait() {
    GROUP("ppa:wait");
    if (!pfs::exists("scripts/ppa-wait.sh") ||
        std::system("command -v python3 >/dev/null 2>&1 && command -v bash >/dev/null 2>&1") != 0) {
        std::printf("    skipped: no scripts/ppa-wait.sh here, or no bash or python3\n");
        return;
    }
    std::random_device rd;
    const pfs::path root = pfs::temp_directory_path() / ("minicode-ppa-" + std::to_string(rd()));
    pfs::create_directories(root);
    const std::string v15 = "1.5.15-1~ppa1~resolute1", v13 = "1.5.13-1~ppa1~resolute1";
    // What the PPA held on 2026-11-08: 1.5.13 failed, older releases built.
    const std::string failed13 = lpRecord(v13, "Failed to build");
    const std::string built14 = lpRecord("1.4.14-1~ppa1~resolute1", "Successfully built");

    {   // Accepted after a while, queued, built: success, each state said once.
        PpaWait r = runPpaWait(root, "built",
            {lpAnswer({failed13, built14}), lpAnswer({failed13, built14}),
             lpAnswer({lpRecord(v15, "Needs building"), failed13}),
             lpAnswer({lpRecord(v15, "Currently building"), failed13}),
             lpAnswer({lpRecord(v15, "Currently building"), failed13}),
             lpAnswer({lpRecord(v15, "Uploading build"), failed13}),
             lpAnswer({lpRecord(v15, "Successfully built"), failed13})},
            "1.5.15");
        CHECK(r.status == 0);
        CHECK(ppaHas(r.out, "Launchpad built " + v15));
        CHECK(ppaHas(r.out, "amd64: Needs building"));
        CHECK(ppaHas(r.out, "2 min  amd64: Needs building"));
        size_t building = r.out.find("amd64: Currently building");
        CHECK(building != std::string::npos &&
              r.out.find("amd64: Currently building", building + 1) == std::string::npos);
        // Another version's failure is not this one's.
        CHECK(!ppaHas(r.out, "error"));
        if (r.status != 0) std::printf("    output:\n%s\n", r.out.c_str());
    }
    {   // Failed to build: exit 1, loudly, with the log's address.
        PpaWait r = runPpaWait(root, "failed",
            {lpAnswer({lpRecord(v13, "Currently building")}), lpAnswer({failed13, built14})},
            "1.5.13");
        CHECK(r.status == 1);
        CHECK(ppaHas(r.out, "error: Launchpad's amd64 build of " + v13 +
                              " ended as \"Failed to build\""));
        CHECK(ppaHas(r.out, "https://launchpad.test/log/" + v13 + "-amd64"));
    }
    {   // Every other end fails too, and a state the script does not know is
        // an end rather than something to wait on for hours.
        int n = 0;
        for (const char *state : {"Dependency wait", "Chroot problem", "Failed to upload",
                                  "Cancelled build", "Build for superseded Source",
                                  "A state Launchpad adds later"}) {
            PpaWait r = runPpaWait(root, "end-" + std::to_string(++n),
                                   {lpAnswer({lpRecord(v15, state, "amd64",
                                                       std::strcmp(state, "Dependency wait") == 0
                                                           ? "libgtk-4-dev (>= 4.20)" : "")})},
                                   "1.5.15");
            CHECK(r.status == 1);
            CHECK(ppaHas(r.out, std::string("ended as \"") + state + "\""));
            if (std::strcmp(state, "Dependency wait") == 0)
                CHECK(ppaHas(r.out, "waiting for: libgtk-4-dev (>= 4.20)"));
        }
    }
    {   // Two architectures: done only when both are, failed if either failed.
        PpaWait both = runPpaWait(root, "two-built",
            {lpAnswer({lpRecord(v15, "Successfully built"), lpRecord(v15, "Currently building", "arm64")}),
             lpAnswer({lpRecord(v15, "Successfully built"), lpRecord(v15, "Successfully built", "arm64")})},
            "1.5.15");
        CHECK(both.status == 0);
        CHECK(ppaHas(both.out, "arm64: Currently building"));
        CHECK(ppaHas(both.out, "(amd64, arm64)"));
        PpaWait one = runPpaWait(root, "two-one-failed",
            {lpAnswer({lpRecord(v15, "Successfully built"), lpRecord(v15, "Currently building", "arm64")}),
             lpAnswer({lpRecord(v15, "Successfully built"), lpRecord(v15, "Failed to build", "arm64")})},
            "1.5.15");
        CHECK(one.status == 1);
        CHECK(ppaHas(one.out, "arm64 build of " + v15));
        CHECK(!ppaHas(one.out, "amd64 build of " + v15 + " ended"));
    }
    {   // Never accepted (a rejected upload has no record): gives up after
        // MINICODE_PPA_ACCEPT_MINUTES, 30 by default, with exit 3.
        PpaWait r = runPpaWait(root, "never", {lpAnswer({failed13, built14})}, "1.5.15");
        CHECK(r.status == 3);
        CHECK(ppaHas(r.out, "no build of " + v15 + " after 30 min"));
        PpaWait shorter = runPpaWait(root, "never-5", {lpAnswer({built14})}, "1.5.15",
                                     "MINICODE_PPA_ACCEPT_MINUTES=5");
        CHECK(shorter.status == 3);
        CHECK(ppaHas(shorter.out, "after 5 min"));
    }
    {   // Never ends: gives up after MINICODE_PPA_WAIT_MINUTES (240) with
        // exit 3, saying how to wait again.
        PpaWait r = runPpaWait(root, "stuck", {lpAnswer({lpRecord(v15, "Currently building")})},
                               "1.5.15");
        CHECK(r.status == 3);
        CHECK(ppaHas(r.out, "has not ended after 240 min"));
        CHECK(ppaHas(r.out, "scripts/ppa-wait.sh 1.5.15,"));
    }
    {   // Launchpad unreadable now and then: kept waiting; ten looks in a
        // row: exit 3. An answer that is not the records counts as one.
        PpaWait r = runPpaWait(root, "blips",
            {"FAIL", "not json at all", "FAIL", lpAnswer({lpRecord(v15, "Successfully built")})},
            "1.5.15");
        CHECK(r.status == 0);
        PpaWait down = runPpaWait(root, "down", {"FAIL"}, "1.5.15");
        CHECK(down.status == 3);
        CHECK(ppaHas(down.out, "ten times in a row"));
    }
    {   // The revision and series pick the version: ~ppa2's record, not ~ppa1's.
        PpaWait r = runPpaWait(root, "rev2",
            {lpAnswer({lpRecord("1.5.13-1~ppa2~resolute1", "Successfully built"), failed13})},
            "1.5.13 2");
        CHECK(r.status == 0);
        CHECK(ppaHas(r.out, "scripts/ppa-wait.sh 1.5.13 2 waits again"));
        PpaWait series = runPpaWait(root, "series",
            {lpAnswer({lpRecord("1.5.15-1~ppa1~questing1", "Failed to build"),
                       lpRecord(v15, "Successfully built")})},
            "1.5.15", "MINICODE_PPA_SERIES=questing");
        CHECK(series.status == 1);
    }
    {   // Usage.
        CHECK(runPpaWait(root, "usage", {lpAnswer({})}, "1.5").status == 2);
        CHECK(runPpaWait(root, "usage-rev", {lpAnswer({})}, "1.5.15 0").status == 2);
        CHECK(runPpaWait(root, "usage-poll", {lpAnswer({})}, "1.5.15",
                         "MINICODE_PPA_POLL_SECONDS=soon").status == 2);
    }
    std::error_code ec;
    pfs::remove_all(root, ec);
}

// Every CI job has a time limit, so a hang fails the job instead of holding a
// runner and main's status for GitHub's six hours (TSan's once did).
void testCiTimeouts() {
    GROUP("ci:timeouts");
    std::ifstream in(".github/workflows/ci.yml");
    if (!in) {
        std::printf("    skipped: no .github/workflows/ci.yml here\n");
        return;
    }
    std::vector<std::string> jobs, limited;
    bool inJobs = false;
    std::string line;
    while (std::getline(in, line)) {
        if (line == "jobs:") { inJobs = true; continue; }
        if (!inJobs) continue;
        if (!line.empty() && line[0] != ' ' && line[0] != '#') inJobs = false;
        // A job is a key at two spaces; its own settings are at four.
        if (line.size() > 3 && line.compare(0, 2, "  ") == 0 && line[2] != ' ' && line[2] != '#' &&
            line.back() == ':')
            jobs.push_back(line.substr(2, line.size() - 3));
        else if (line.compare(0, 20, "    timeout-minutes:") == 0 && !jobs.empty())
            limited.push_back(jobs.back());
    }
    CHECK(jobs.size() >= 6);
    for (const auto &job : jobs) {
        const bool has = std::count(limited.begin(), limited.end(), job) == 1;
        CHECK(has);
        if (!has) std::printf("    the CI job %s has no timeout-minutes of its own\n", job.c_str());
    }
}
