// TEMPORARY (PR "CI: one retry for a failed GUI suite, and a nightly soak"):
// fails on its first run and passes when the wrapper retries it alone in a
// fresh run (MC_GUI_RETRY_OF), so CI's run shows a FLAKY line and still
// passes. It passes under the soak (MC_GUI_SHUFFLE set), which retries
// nothing. Removed before the pull request merges.
#include "Harness.h"
#include <cstdlib>

MC_GUI_SUITE(temp_retry_proof) {
    MCGuiStep("fails until retried (temporary, proves the retry)", [] {
        GCHECK(getenv("MC_GUI_RETRY_OF") != nullptr || getenv("MC_GUI_SHUFFLE") != nullptr);
    });
}
