// ProcessEnv.h — pure C++ (POSIX). The process's environment as an
// immutable snapshot, so threads that start programs never read `environ`.
//
// Why (2026-11-17): libc's setenv reallocates the `environ` array and can
// rewrite a value in place, with no lock a reader of `environ` takes. A
// thread that walked `environ` (to copy it for a child, through
// NSProcessInfo.environment, GSubprocessLauncher or a plain loop) while
// another set a variable read a freed array or a torn string; on Linux that
// corrupted the heap in the GUI checks, which set and unset variables while
// git ran on worker threads. The apps never set variables after startup,
// but their checks do, and anything may in future.
//
// The rule: the snapshot is taken from `environ` only by `refresh` and
// `set`, which run on the one thread that changes the environment (the main
// thread; at startup, before any other thread starts programs). Every other
// reader asks `current()`, a shared immutable copy, and builds a child's
// environment with `Block`, an explicit envp for posix_spawn or execve.
// Variables are changed after startup only through `set`.
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ProcessEnv {

// "NAME=value" strings, in environ's order, one per name (the first wins).
struct Snapshot {
    std::vector<std::string> entries;
    // The value of `name`, or nullopt when it is not set.
    std::optional<std::string> get(const std::string& name) const;
    bool has(const std::string& name) const { return get(name).has_value(); }
    // The entries as name and value.
    std::vector<std::pair<std::string, std::string>> pairs() const;
    // Counts each refresh, so a caller can cache what it built from one.
    unsigned long generation = 0;
};

// The snapshot as it is now. Any thread; never reads `environ`. The first
// call takes one when nothing has (the apps call refresh() at startup, so
// that happens only in tools and tests).
std::shared_ptr<const Snapshot> current();

// Reads `environ` again. Only on the thread that changes the environment.
void refresh();

// Sets `name` to `value` (nullptr: unsets it) with setenv or unsetenv, then
// refreshes. The only way the apps and their checks change the environment
// after startup. Returns false when setenv refused the name.
bool set(const std::string& name, const char* value);

// An explicit environment for one child: a snapshot's entries with some
// names changed (a value) or removed (nullopt). envp() stays valid for the
// Block's life and is what posix_spawn and execve take.
class Block {
public:
    using Change = std::pair<std::string, std::optional<std::string>>;
    explicit Block(const Snapshot& base, const std::vector<Change>& changes = {});
    char* const* envp() { return pointers_.data(); }
    const std::vector<std::string>& entries() const { return strings_; }
    std::optional<std::string> get(const std::string& name) const;

private:
    std::vector<std::string> strings_;
    std::vector<char*> pointers_;
};

}  // namespace ProcessEnv
