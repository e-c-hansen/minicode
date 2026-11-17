// A reader/writer lock around the process environment.
//
// setenv reallocates `environ` and frees strings that another thread may be
// reading. Starting a process reads it: a GSubprocessLauncher copies it when
// it is made (g_get_environ), g_spawn_* with no environment reads it, and
// g_getenv and g_find_program_in_path read PATH. CI's GTK job aborted twice
// with "realloc(): invalid next size"; the stack was
//   realloc <- g_realloc <- g_environ_setenv <- g_subprocess_launcher_setenv
//   <- runGitWithin <- runGit <- (the git panel's worker thread)
// while the GUI harness's first step called g_setenv on the main thread.
//
// The rule: any spawn or environment read off the main thread holds an
// EnvRead while it reads (never while it waits for a child), and everything
// that changes the environment goes through envSet and envUnset.
#pragma once

#include <shared_mutex>

namespace EnvLock {

std::shared_mutex& envMutex();

// Held while reading the environment; any number of readers at once.
class EnvRead {
public:
    EnvRead() : lock_(envMutex()) {}
private:
    std::shared_lock<std::shared_mutex> lock_;
};

// g_setenv and g_unsetenv under the exclusive lock.
bool envSet(const char* name, const char* value, bool overwrite = true);
void envUnset(const char* name);

}  // namespace EnvLock
