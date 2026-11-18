// MigrationLinux.cpp — see MigrationLinux.h.
#include "MigrationLinux.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <pwd.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace MigrationLinux {

namespace {

// There, as a file, a folder or a link (a dangling one too): never copied
// over.
bool present(const std::string& p) {
    std::error_code ec;
    return fs::symlink_status(p, ec).type() != fs::file_type::not_found && !ec;
}

bool copyTree(const Rename::Copy& c) {
    std::error_code ec;
    const fs::path to(c.to);
    fs::create_directories(to.parent_path(), ec);
    if (ec) return false;
    // Beside the target, so the rename stays on one file system.
    const fs::path tmp = to.parent_path() / (to.filename().string() + ".migrating");
    fs::remove_all(tmp, ec);
    ec.clear();
    fs::copy(c.from, tmp, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (!ec) fs::rename(tmp, to, ec);
    if (ec) {
        std::fprintf(stderr, "mutible: could not copy %s to %s: %s\n", c.from.c_str(),
                     c.to.c_str(), ec.message().c_str());
        std::error_code ignored;
        fs::remove_all(tmp, ignored);
        return false;
    }
    return true;
}

}  // namespace

Result migrate(const std::string& home, const char* xdgConfig, const char* xdgState,
               const char* xdgData, const std::string& when) {
    Result r;
    const std::string marker = Rename::linuxMarker(home, xdgData);
    if (present(marker)) return r;
    r.ran = true;
    // Which copies, decided before any is made: the data folder's copy
    // makes the folder the marker goes in.
    for (const Rename::Copy& c : Rename::pending(Rename::linuxItems(home, xdgConfig, xdgState, xdgData),
                                                 present))
        (copyTree(c) ? r.done : r.failed).push_back(c);
    std::error_code ec;
    fs::create_directories(fs::path(marker).parent_path(), ec);
    std::ofstream out(marker, std::ios::binary | std::ios::trunc);
    out << Rename::markerText(when, r.done, r.failed, false);
    return r;
}

Result migrateFromOldName() {
    const char* bench = std::getenv("MUTIBLE_BENCH");
    if (bench && *bench) return Result();
    std::string home;
    if (const char* h = std::getenv("HOME"); h && *h) home = h;
    else if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) home = pw->pw_dir;
    if (home.empty()) return Result();
    char when[32];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    std::strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return migrate(home, std::getenv("XDG_CONFIG_HOME"), std::getenv("XDG_STATE_HOME"),
                   std::getenv("XDG_DATA_HOME"), when);
}

}  // namespace MigrationLinux
