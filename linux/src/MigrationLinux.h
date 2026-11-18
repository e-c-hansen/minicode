// MigrationLinux.h — the first launch as mutible copies what minicode, its
// name before 1.6.0, kept in the XDG folders (src/Rename.h says which; the
// Mac's is mac/src/Migration.mm). Settings and keys (config), sessions,
// recent folders and the zoom (state), and the Backups and the downloaded
// tectonic (data) are copied, each only when its new folder is missing, so
// nothing the new name already has is overwritten. The old folders are
// left where they were. A marker in the new data folder says it ran and
// what it copied; while it is there nothing runs again. Plain C++17, no
// GLib, so linux/tests/run_tests.cpp covers it.
#pragma once

#include "Rename.h"

#include <string>
#include <vector>

namespace MigrationLinux {

struct Result {
    bool ran = false;                 // false: the marker was there already
    std::vector<Rename::Copy> done;   // copied
    std::vector<Rename::Copy> failed; // tried, and left as it was
};

// The copies for `home` and the XDG values (null or relative: the
// default, as Rename::linuxItems reads them), then the marker with `when`
// in it. A copy goes to a temporary name beside its target first and is
// renamed into place, so a failed one leaves nothing half made.
Result migrate(const std::string& home, const char* xdgConfig, const char* xdgState,
               const char* xdgData, const std::string& when);

// migrate() for this process: $HOME (else the password file's) and the
// XDG variables, the time now. Run once at startup, before anything reads
// settings or sessions. Not under MUTIBLE_BENCH, whose world is made for
// it.
Result migrateFromOldName();

}  // namespace MigrationLinux
