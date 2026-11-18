// Rename.h — header only, plain C++ (POSIX). minicode was renamed mutible
// in 1.6.0; this file holds every old name the app still reads, and what a
// first launch copies from the old places to the new.
//
// The old names are kept here, and only here, so that the mechanical rename
// (scripts/rename-mutible.sh, which skips this file) leaves them alone:
//
//   environment  MINICODE_X is read when MUTIBLE_X is unset: a program
//                calls adoptOldEnvironment() first thing, and core code
//                the Linux app reaches reads getenvOrOld()
//   commands     "minicode.x" ids, as users' keys.conf files have them,
//                name the same command as "mutible.x" (Commands::find)
//   sockets      the control channel's client also looks in an old
//                copy's socket folder while one may still run
//                (ControlPaths.h)
//   hooks        `minicode notify --from-hook`, as Claude Code's settings
//                files keep it, counts as installed (ClaudeHooks.cpp)
//   IDE link     a lock file an old copy left behind may be removed
//                (IdeLink.cpp)
//
// The migration (Mac: mac/src/Migration.mm, at startup): the folders below
// are copied to their new names when the new one is missing, the old
// defaults domain is copied when the new one is empty, and a marker file
// says it ran, so it runs once. The old copies are left where they were.
#pragma once
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#ifdef __APPLE__
#include <crt_externs.h>
#else
extern "C" char **environ;   // as glibc's unistd.h declares it
#endif

namespace Rename {

// The names before 1.6.0, and the ones after.
inline const char *const kOldEnvPrefix = "MINICODE_";
inline const char *const kNewEnvPrefix = "MUTIBLE_";
inline const char *const kOldCommandPrefix = "minicode.";
inline const char *const kNewCommandPrefix = "mutible.";
inline const char *const kOldBundleId = "com.minicode.editor";
inline const char *const kNewBundleId = "ai.ible.mutible";
inline const char *const kOldName = "minicode";      // the command, ideName, XDG folders
inline const char *const kNewName = "mutible";
inline const char *const kOldMacFolder = "MiniCode";  // Application Support, $TMPDIR
inline const char *const kNewMacFolder = "Mutible";

inline bool startsWith(const std::string &s, const char *prefix) {
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

// ------------------------------------------------------------ environment

// "MUTIBLE_X" -> "MINICODE_X"; "" for a name without the new prefix.
inline std::string oldEnvName(const std::string &name) {
    if (!startsWith(name, kNewEnvPrefix) || name.size() == std::strlen(kNewEnvPrefix)) return "";
    return kOldEnvPrefix + name.substr(std::strlen(kNewEnvPrefix));
}

// getenv(name), else the value of its old name.
inline const char *getenvOrOld(const char *name) {
    if (const char *v = std::getenv(name)) return v;
    const std::string old = oldEnvName(name);
    return old.empty() ? nullptr : std::getenv(old.c_str());
}

// "NAME=value" entries -> the (new name, value) pairs to set: one for every
// MINICODE_X whose MUTIBLE_X is not among the entries.
inline std::vector<std::pair<std::string, std::string>> envAdoptions(
    const std::vector<std::string> &entries) {
    std::vector<std::pair<std::string, std::string>> out;
    auto has = [&](const std::string &name) {
        for (const std::string &e : entries)
            if (e.size() > name.size() && e[name.size()] == '=' && startsWith(e, name.c_str()))
                return true;
        return false;
    };
    for (const std::string &e : entries) {
        const size_t eq = e.find('=');
        if (eq == std::string::npos || !startsWith(e, kOldEnvPrefix)) continue;
        const std::string rest = e.substr(std::strlen(kOldEnvPrefix), eq - std::strlen(kOldEnvPrefix));
        if (rest.empty()) continue;
        const std::string name = kNewEnvPrefix + rest;
        bool given = has(name);
        for (const auto &p : out) given = given || p.first == name;
        if (!given) out.emplace_back(name, e.substr(eq + 1));
    }
    return out;
}

// Sets MUTIBLE_X from MINICODE_X wherever MUTIBLE_X is unset, so a script,
// a hook or a terminal of an old copy that still speaks the old names is
// understood. Only at the very start of a program, before any thread
// starts: it calls setenv (ProcessEnv.h says why that matters).
inline void adoptOldEnvironment() {
#ifdef __APPLE__
    char **env = *_NSGetEnviron();
#else
    char **env = ::environ;
#endif
    std::vector<std::string> entries;
    for (char **e = env; e && *e; e++) entries.emplace_back(*e);
    for (const auto &p : envAdoptions(entries)) setenv(p.first.c_str(), p.second.c_str(), 0);
}

// ------------------------------------------------------------ commands

// A command id's other spelling, "minicode.x" <-> "mutible.x"; "" for an id
// with neither prefix.
inline std::string otherCommandId(const std::string &id) {
    if (startsWith(id, kOldCommandPrefix))
        return kNewCommandPrefix + id.substr(std::strlen(kOldCommandPrefix));
    if (startsWith(id, kNewCommandPrefix))
        return kOldCommandPrefix + id.substr(std::strlen(kNewCommandPrefix));
    return "";
}

// ------------------------------------------------------------ the CLI

// Whether `minicode` should say it is now `mutible`: when run under its old
// name, with a terminal to read the line, and never for a Claude Code hook
// (`minicode notify --from-hook`, which must stay silent).
inline bool sayRenamed(const std::string &invokedAs, const std::vector<std::string> &args,
                       bool stderrIsTerminal) {
    std::string base = invokedAs;
    const size_t slash = base.rfind('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    if (base != kOldName || !stderrIsTerminal) return false;
    if (!args.empty() && args[0] == "notify")
        for (const std::string &a : args)
            if (a == "--from-hook") return false;
    return true;
}

inline const char *const kRenamedNotice =
    "minicode is now called mutible (or mbl); the minicode command will go in a later release";

// ------------------------------------------------------------ sockets

// The folder an old copy would use instead of `folder`, one of
// socketFolders()' (ControlPaths.h): ".../Mutible" -> ".../MiniCode",
// ".../mutible" -> ".../minicode", ".../mutible-501" -> ".../minicode-501";
// "" for any other folder.
inline std::string oldSocketFolder(const std::string &folder) {
    const size_t slash = folder.rfind('/');
    if (slash == std::string::npos) return "";
    const std::string parent = folder.substr(0, slash + 1), last = folder.substr(slash + 1);
    if (last == kNewMacFolder) return parent + kOldMacFolder;
    if (last == kNewName) return parent + kOldName;
    const std::string dash = std::string(kNewName) + "-";
    if (startsWith(last, dash.c_str()) && last.size() > dash.size())
        return parent + kOldName + "-" + last.substr(dash.size());
    return "";
}

// ------------------------------------------------------------ migration

struct Copy {
    std::string from, to;
    bool operator==(const Copy &o) const { return from == o.from && to == o.to; }
};

// The Mac's: Application Support (sessions.json, Backups, the downloaded
// tectonic and mermaid, Request a Change's source), the settings folder
// (settings.conf, keys.conf), and the browser's site data and cookies,
// which WebKit keeps under the bundle id. The caches are left to refill.
inline std::vector<Copy> macItems(const std::string &home) {
    const std::string lib = home + "/Library/";
    const std::string oldId = kOldBundleId, newId = kNewBundleId;
    return {
        {lib + "Application Support/" + kOldMacFolder, lib + "Application Support/" + kNewMacFolder},
        {home + "/.config/" + kOldName, home + "/.config/" + kNewName},
        {lib + "WebKit/" + oldId, lib + "WebKit/" + newId},
        {lib + "HTTPStorages/" + oldId, lib + "HTTPStorages/" + newId},
        {lib + "HTTPStorages/" + oldId + ".binarycookies",
         lib + "HTTPStorages/" + newId + ".binarycookies"},
    };
}

// The marker the Mac's migration writes, in the new Application Support
// folder; while it is there the migration does not run again.
inline std::string macMarker(const std::string &home) {
    return home + "/Library/Application Support/" + kNewMacFolder + "/.migrated-from-minicode";
}

// The XDG folders on Linux, for the GTK port (an empty or relative XDG
// variable means its default, as the spec says): settings, state
// (sessions, recent folders, zoom), data (Backups, tectonic). The cache is
// left to refill.
inline std::vector<Copy> linuxItems(const std::string &home, const char *xdgConfig,
                                    const char *xdgState, const char *xdgData) {
    auto base = [&](const char *v, const char *fallback) {
        return v && v[0] == '/' ? std::string(v) : home + fallback;
    };
    const std::string config = base(xdgConfig, "/.config"), state = base(xdgState, "/.local/state"),
                      data = base(xdgData, "/.local/share");
    return {
        {config + "/" + kOldName, config + "/" + kNewName},
        {state + "/" + kOldName, state + "/" + kNewName},
        {data + "/" + kOldName, data + "/" + kNewName},
    };
}

inline std::string linuxMarker(const std::string &home, const char *xdgData) {
    const std::string data = xdgData && xdgData[0] == '/' ? std::string(xdgData) : home + "/.local/share";
    return data + "/" + kNewName + "/.migrated-from-minicode";
}

// The copies still to make: the old one is there and the new one is not
// (so nothing the new name already has is overwritten).
inline std::vector<Copy> pending(const std::vector<Copy> &items,
                                 const std::function<bool(const std::string &)> &exists) {
    std::vector<Copy> out;
    for (const Copy &c : items)
        if (exists(c.from) && !exists(c.to)) out.push_back(c);
    return out;
}

// Whether the old defaults domain is copied to the new: the new one has
// nothing yet and the old one has something.
inline bool shouldCopyDefaults(size_t newKeys, size_t oldKeys) { return newKeys == 0 && oldKeys > 0; }

// The marker's text: when, and what was copied (each copy's two paths, and
// "failed" after one that did not copy).
inline std::string markerText(const std::string &when, const std::vector<Copy> &done,
                              const std::vector<Copy> &failed, bool copiedDefaults) {
    std::string s = "mutible copied these from minicode, its name before 1.6.0, on " + when +
                    ". The old copies were left where they were.\n";
    if (copiedDefaults)
        s += std::string("defaults ") + kOldBundleId + " -> " + kNewBundleId + "\n";
    for (const Copy &c : done) s += c.from + " -> " + c.to + "\n";
    for (const Copy &c : failed) s += c.from + " -> " + c.to + " failed\n";
    if (done.empty() && failed.empty() && !copiedDefaults) s += "(nothing to copy)\n";
    return s;
}

}  // namespace Rename
