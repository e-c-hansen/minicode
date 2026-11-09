// ppa_wait_tests.inc — the `ci:*` group of run_tests.cpp: the CI
// workflow's job time limits. Run from the repo root; skipped where the
// workflow file is missing (the Debian package's build).

#include <sys/wait.h>

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
