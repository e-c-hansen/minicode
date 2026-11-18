// Bench.cpp — see Bench.h.
#include "Bench.h"

#include <glib.h>

#include <cstdio>
#include <cstdlib>

namespace Bench {

static std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? v : "";
}

bool active() { return !env("MUTIBLE_BENCH").empty(); }

namespace {
struct Step {
    Hooks hooks;
    int n = 0;
};

gboolean run(gpointer p) {
    Step* s = static_cast<Step*>(p);
    const std::string cpp = env("MUTIBLE_BENCH_CPP"), tex = env("MUTIBLE_BENCH_TEX"),
                      url = env("MUTIBLE_BENCH_URL");
    switch (s->n++) {
        case 0: if (!cpp.empty()) s->hooks.openFile(cpp); return G_SOURCE_CONTINUE;
        case 1: s->hooks.showTerminal(); return G_SOURCE_CONTINUE;
        case 2: if (!url.empty()) s->hooks.showAddress(url); return G_SOURCE_CONTINUE;
        case 3: if (!tex.empty()) s->hooks.openInNewWindow(tex); return G_SOURCE_CONTINUE;
        default:
            std::fprintf(stderr, "mutible bench: ready\n");
            delete s;
            return G_SOURCE_REMOVE;
    }
}
}  // namespace

void start(const Hooks& hooks) {
    static bool started = false;
    if (!active() || started) return;
    started = true;
    g_timeout_add(2000, run, new Step{hooks, 0});
}

}  // namespace Bench
