// See EnvLock.h.
#include "EnvLock.h"

#include <glib.h>
#include <mutex>

namespace EnvLock {

std::shared_mutex& envMutex() {
    static std::shared_mutex m;
    return m;
}

bool envSet(const char* name, const char* value, bool overwrite) {
    std::unique_lock<std::shared_mutex> lock(envMutex());
    return g_setenv(name, value, overwrite ? TRUE : FALSE) != FALSE;
}

void envUnset(const char* name) {
    std::unique_lock<std::shared_mutex> lock(envMutex());
    g_unsetenv(name);
}

}  // namespace EnvLock
