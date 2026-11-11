// ProcessMonitor.h — the sampler behind `minicode procs` and, later, the
// Processes panel (PARITY.md item 3.8; the Mac's is src/ProcessMonitor.mm).
//
// A thread of its own reads /proc through ProcFs::Source into the core's
// Procs::Tracker every 10 s (2 s while a panel shows), and hands copied rows
// to the main loop. The main thread gathers the windows' folders (resolved
// with realpath) and terminals (each by its shell's pid, the VTE child)
// before each look, as the Mac's gatherTerminals does. Stop is the core's
// planStop: SIGTERM now, SIGKILL 3 s later, each only if the pid still has
// the start time it was planned with.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ProcessTable.h"

class ProcessMonitor {
public:
    // What the main thread knows: filled by the app on each look.
    using Gatherer = std::function<void(std::vector<Procs::Terminal>& terminals,
                                        std::vector<std::string>& folders)>;

    static ProcessMonitor& shared();

    // Main thread. Starts the thread and the 2 s timer that decides when a
    // look is due; once.
    void start(Gatherer gatherer);
    // A look now; `done` runs on the main thread once the rows arrived.
    void sampleNow(std::function<void()> done);
    // The last look's rows, for one window.
    Procs::View view(int window, const std::string& folder) const;
    // Stops a process as Stop in the panel does; `done` gets "" or the
    // reason it was refused.
    void stop(const Procs::Key& key, std::function<void(const std::string&)> done);
    // A panel on show makes looks come every 2 s.
    void setPanelShown(bool shown) { panelShown_ = shown; }
    bool panelShown() const { return panelShown_; }
    // The terminals of the last look (for the checks).
    const std::vector<Procs::Terminal>& terminals() const { return terms_; }

    // For the checks: the CPU time the sampling thread spent on its last
    // look (CLOCK_THREAD_CPUTIME_ID), and how many looks arrived.
    double lastSampleCpuMilliseconds() const { return lastCpuMs_; }
    unsigned samples() const { return samples_; }
    size_t lastListed() const { return lastListed_; }

private:
    ProcessMonitor() = default;
    struct Impl;
    Impl* impl_ = nullptr;
    Gatherer gatherer_;
    std::vector<Procs::Proc> procs_;
    std::vector<Procs::Terminal> terms_;
    bool panelShown_ = false;
    bool inFlight_ = false;
    double lastLookAt_ = 0;
    double lastCpuMs_ = 0;
    unsigned samples_ = 0;
    size_t lastListed_ = 0;
    std::vector<std::function<void()>> waiting_;

    void post(std::function<void()> work);
    static int tick(void*);
    friend struct Impl;
};
