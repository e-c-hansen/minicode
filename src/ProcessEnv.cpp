// ProcessEnv.cpp — see ProcessEnv.h.
#include "ProcessEnv.h"
#include <cstdlib>
#include <mutex>
#include <set>
#ifdef __APPLE__
#include <crt_externs.h>
#define MC_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#define MC_ENVIRON environ
#endif

namespace ProcessEnv {

namespace {

std::mutex gLock;
std::shared_ptr<const Snapshot> gCurrent;
unsigned long gGeneration = 0;

std::string nameOf(const std::string& entry) {
    const size_t eq = entry.find('=');
    return eq == std::string::npos ? entry : entry.substr(0, eq);
}

// Reads environ. Called with gLock held, on the thread that changes it.
std::shared_ptr<const Snapshot> take() {
    auto s = std::make_shared<Snapshot>();
    std::set<std::string> seen;
    if (char** e = MC_ENVIRON)
        for (; *e; e++) {
            std::string entry = *e;
            if (entry.find('=') == std::string::npos) continue;
            if (!seen.insert(nameOf(entry)).second) continue;
            s->entries.push_back(std::move(entry));
        }
    s->generation = ++gGeneration;
    return s;
}

}  // namespace

std::optional<std::string> Snapshot::get(const std::string& name) const {
    for (const std::string& e : entries)
        if (e.size() > name.size() && e[name.size()] == '=' && e.compare(0, name.size(), name) == 0)
            return e.substr(name.size() + 1);
    return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> Snapshot::pairs() const {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(entries.size());
    for (const std::string& e : entries) {
        const size_t eq = e.find('=');
        out.emplace_back(e.substr(0, eq), e.substr(eq + 1));
    }
    return out;
}

std::shared_ptr<const Snapshot> current() {
    std::lock_guard<std::mutex> hold(gLock);
    if (!gCurrent) gCurrent = take();
    return gCurrent;
}

void refresh() {
    std::lock_guard<std::mutex> hold(gLock);
    gCurrent = take();
}

bool set(const std::string& name, const char* value) {
    // The lock is held across the change too, so a reader that came in
    // before it gets the old snapshot whole and one after it the new.
    std::lock_guard<std::mutex> hold(gLock);
    const int rc = value ? ::setenv(name.c_str(), value, 1) : ::unsetenv(name.c_str());
    gCurrent = take();
    return rc == 0;
}

Block::Block(const Snapshot& base, const std::vector<Change>& changes) {
    std::set<std::string> changed;
    for (const Change& c : changes) changed.insert(c.first);
    for (const std::string& e : base.entries)
        if (!changed.count(nameOf(e))) strings_.push_back(e);
    std::set<std::string> done;
    // The last change of a name wins.
    for (auto c = changes.rbegin(); c != changes.rend(); ++c) {
        if (!done.insert(c->first).second) continue;
        if (c->second) strings_.push_back(c->first + "=" + *c->second);
    }
    pointers_.reserve(strings_.size() + 1);
    for (std::string& s : strings_) pointers_.push_back(&s[0]);
    pointers_.push_back(nullptr);
}

std::optional<std::string> Block::get(const std::string& name) const {
    for (const std::string& e : strings_)
        if (e.size() > name.size() && e[name.size()] == '=' && e.compare(0, name.size(), name) == 0)
            return e.substr(name.size() + 1);
    return std::nullopt;
}

}  // namespace ProcessEnv
