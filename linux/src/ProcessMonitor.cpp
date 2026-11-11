// ProcessMonitor.cpp — see ProcessMonitor.h.
#include "ProcessMonitor.h"

#include <glib.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <ctime>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>

#include "ProcFs.h"

namespace {

double monotonicSeconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

double threadCpuMs() {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

}  // namespace

// Everything the sampling thread owns: the source, the tracker, its queue.
struct ProcessMonitor::Impl {
    struct Task {
        double due;
        std::function<void()> fn;
    };
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Task> tasks;
    ProcFs::Source source;
    Procs::Tracker tracker;

    void add(double due, std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            tasks.push_back({due, std::move(fn)});
        }
        wake.notify_one();
    }

    void run() {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            auto next = std::min_element(tasks.begin(), tasks.end(),
                                         [](const Task& a, const Task& b) { return a.due < b.due; });
            if (next == tasks.end()) {
                wake.wait(lock);
                continue;
            }
            const double wait = next->due - monotonicSeconds();
            if (wait > 0) {
                wake.wait_for(lock, std::chrono::duration<double>(wait));
                continue;
            }
            std::function<void()> fn = std::move(next->fn);
            tasks.erase(next);
            lock.unlock();
            fn();
            lock.lock();
        }
    }
};

ProcessMonitor& ProcessMonitor::shared() {
    static ProcessMonitor* m = new ProcessMonitor;   // lives to the end of the process
    return *m;
}

// Runs `work` on the main loop.
void ProcessMonitor::post(std::function<void()> work) {
    auto* box = new std::function<void()>(std::move(work));
    g_idle_add(
        +[](gpointer p) -> gboolean {
            auto* f = static_cast<std::function<void()>*>(p);
            (*f)();
            delete f;
            return G_SOURCE_REMOVE;
        },
        box);
}

int ProcessMonitor::tick(void*) {
    ProcessMonitor& m = shared();
    const double interval = m.panelShown_ ? 2.0 : 10.0;
    if (!m.inFlight_ && monotonicSeconds() - m.lastLookAt_ >= interval) m.sampleNow(nullptr);
    return G_SOURCE_CONTINUE;
}

void ProcessMonitor::start(Gatherer gatherer) {
    if (impl_) return;
    gatherer_ = std::move(gatherer);
    impl_ = new Impl;
    std::thread([impl = impl_] { impl->run(); }).detach();
    g_timeout_add_seconds(2, &ProcessMonitor::tick, nullptr);
    sampleNow(nullptr);
}

void ProcessMonitor::sampleNow(std::function<void()> done) {
    if (!impl_) {
        if (done) done();
        return;
    }
    if (done) waiting_.push_back(std::move(done));
    if (inFlight_) return;   // its answer serves every waiter
    inFlight_ = true;
    lastLookAt_ = monotonicSeconds();
    auto terms = std::make_shared<std::vector<Procs::Terminal>>();
    auto folders = std::make_shared<std::vector<std::string>>();
    if (gatherer_) gatherer_(*terms, *folders);
    impl_->add(0, [this, terms, folders] {
        const double c0 = threadCpuMs();
        Procs::SampleStats st = impl_->tracker.sample(impl_->source, monotonicSeconds(), *terms,
                                                       *folders, (int)getpid(), (uint32_t)geteuid());
        auto snap = std::make_shared<std::vector<Procs::Proc>>(impl_->tracker.snapshot());
        const double cpu = threadCpuMs() - c0;
        post([this, snap, terms, st, cpu] {
            procs_ = std::move(*snap);
            terms_ = *terms;
            lastCpuMs_ = cpu;
            lastListed_ = st.listed;
            samples_++;
            inFlight_ = false;
            std::vector<std::function<void()>> waiters;
            waiters.swap(waiting_);
            for (auto& w : waiters) w();
        });
    });
}

Procs::View ProcessMonitor::view(int window, const std::string& folder) const {
    return Procs::buildView(procs_, terms_, window, folder);
}

void ProcessMonitor::stop(const Procs::Key& key, std::function<void(const std::string&)> done) {
    if (!impl_) {
        if (done) done("not running");
        return;
    }
    // Shells of terminals opened since the last look count too.
    std::vector<int> shells;
    for (const Procs::Terminal& t : terms_)
        if (t.shellPid > 0) shells.push_back(t.shellPid);
    std::vector<Procs::Terminal> now;
    std::vector<std::string> folders;
    if (gatherer_) gatherer_(now, folders);
    for (const Procs::Terminal& t : now)
        if (t.shellPid > 0) shells.push_back(t.shellPid);
    impl_->add(0, [this, key, shells, done] {
        Impl* I = impl_;
        Procs::StopPlan plan =
            Procs::planStop(I->source.list(), key, (int)getpid(), (int)getpgrp(), shells);
        if (plan.ok) {
            // Each target is still the process it was (its start time).
            bool all = I->source.startTimeOf(key.pid) == key.start;
            for (const Procs::Key& k : plan.targets)
                if (I->source.startTimeOf(k.pid) != k.start) all = false;
            if (plan.group && all) {
                kill(-plan.pgid, SIGTERM);
            } else {
                for (const Procs::Key& k : plan.targets)
                    if (I->source.startTimeOf(k.pid) == k.start) kill(k.pid, SIGTERM);
            }
            const std::vector<Procs::Key> targets = plan.targets;
            I->add(monotonicSeconds() + 3, [this, targets] {
                for (const Procs::Key& k : targets)
                    if (impl_->source.startTimeOf(k.pid) == k.start) kill(k.pid, SIGKILL);
                post([this] { sampleNow(nullptr); });
            });
        }
        const std::string refusal = plan.ok ? std::string() : plan.refusal;
        post([this, done, refusal] {
            if (done) done(refusal);
            sampleNow(nullptr);
        });
    });
}
