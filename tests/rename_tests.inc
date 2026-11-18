// rename_tests.inc — the `rename:*` groups of run_tests.cpp: what still
// reads the names minicode had before it became mutible in 1.6.0, and what
// a first launch copies (src/Rename.h). Included there. The old names are
// spelled out here on purpose; scripts/rename-mutible.sh skips this file.
#include "Rename.h"
#include "ControlPaths.h"

static void testRename() {
    GROUP("rename:env");
    {
        CHECK(Rename::oldEnvName("MUTIBLE_SETTINGS") == "MINICODE_SETTINGS");
        CHECK(Rename::oldEnvName("MUTIBLE_") == "");
        CHECK(Rename::oldEnvName("MINICODE_SETTINGS") == "");
        CHECK(Rename::oldEnvName("PATH") == "");

        auto a = Rename::envAdoptions({"PATH=/bin", "MINICODE_SETTINGS=/a/s.conf", "MINICODE_PID=7",
                                       "MUTIBLE_PID=9", "MINICODE_=x", "MINICODE_EMPTY=",
                                       "MINICODE_SETTINGS=/later"});
        CHECK(a.size() == 2);
        CHECK(a.size() == 2 && a[0].first == "MUTIBLE_SETTINGS" && a[0].second == "/a/s.conf");
        CHECK(a.size() == 2 && a[1].first == "MUTIBLE_EMPTY" && a[1].second.empty());
        CHECK(Rename::envAdoptions({"MUTIBLE_X=1", "MINICODE_X=2"}).empty());

        ProcessEnv::set("MUTIBLE_RENAME_TEST", nullptr);
        ProcessEnv::set("MINICODE_RENAME_TEST", "old");
        CHECK(Rename::getenvOrOld("MUTIBLE_RENAME_TEST") != nullptr &&
              std::string(Rename::getenvOrOld("MUTIBLE_RENAME_TEST")) == "old");
        ProcessEnv::set("MUTIBLE_RENAME_TEST", "new");
        CHECK(std::string(Rename::getenvOrOld("MUTIBLE_RENAME_TEST")) == "new");
        ProcessEnv::set("MUTIBLE_RENAME_TEST", nullptr);
        ProcessEnv::set("MINICODE_RENAME_TEST", nullptr);
        CHECK(Rename::getenvOrOld("MUTIBLE_RENAME_TEST") == nullptr);
        CHECK(Rename::getenvOrOld("RENAME_TEST_UNSET_ANYWHERE") == nullptr);
    }

    GROUP("rename:commands");
    {
        CHECK(Rename::otherCommandId("minicode.toggleBrowser") == "mutible.toggleBrowser");
        CHECK(Rename::otherCommandId("mutible.toggleBrowser") == "minicode.toggleBrowser");
        CHECK(Rename::otherCommandId("editor.action.formatDocument") == "");
        // Every command of the app's own is found by either spelling, and
        // a keys.conf written before the rename still binds its keys.
        int own = 0;
        for (const Commands::Command &c : Commands::all()) {
            const std::string other = Rename::otherCommandId(c.id);
            if (other.empty()) continue;
            own++;
            CHECK(Commands::find(other) == &c);
            CHECK(Commands::find(c.id) == &c);
        }
        CHECK(own > 50);
        std::vector<Commands::Error> errors;
        Commands::Keymap m = Commands::resolve(
            "ctrl+alt+shift+f9 = minicode.toggleBrowser\nctrl+alt+shift+f10 = mutible.toggleBrowser\n",
            &errors);
        CHECK(errors.empty());
        const Commands::Command *browser = Commands::find("mutible.toggleBrowser");
        CHECK(browser != nullptr);
        if (browser) {
            Commands::Key f9, f10;
            CHECK(Commands::parseKey("ctrl+alt+shift+f9", &f9));
            CHECK(Commands::parseKey("ctrl+alt+shift+f10", &f10));
            const auto &keys = m.keysOf(browser->id);
            auto has = [&](const Commands::Key &k) {
                for (const Commands::Key &x : keys)
                    if (Commands::format(x) == Commands::format(k)) return true;
                return false;
            };
            CHECK(has(f9));
            CHECK(has(f10));
            CHECK(m.keysOf("minicode.toggleBrowser").size() == keys.size());
        }
    }

    GROUP("rename:cli");
    {
        CHECK(Rename::sayRenamed("/opt/homebrew/bin/minicode", {"open", "."}, true));
        CHECK(Rename::sayRenamed("minicode", {}, true));
        CHECK(!Rename::sayRenamed("minicode", {}, false));                 // no terminal
        CHECK(!Rename::sayRenamed("/usr/bin/mutible", {}, true));
        CHECK(!Rename::sayRenamed("mbl", {}, true));
        CHECK(!Rename::sayRenamed("minicode", {"notify", "--from-hook"}, true));
        CHECK(!Rename::sayRenamed("minicode", {"notify", "--pid", "3", "--from-hook"}, true));
        CHECK(Rename::sayRenamed("minicode", {"notify", "--ping"}, true));
    }

    GROUP("rename:sockets");
    {
        CHECK(Rename::oldSocketFolder("/var/folders/x/T/Mutible") == "/var/folders/x/T/MiniCode");
        CHECK(Rename::oldSocketFolder("/run/user/1000/mutible") == "/run/user/1000/minicode");
        CHECK(Rename::oldSocketFolder("/tmp/mutible-501") == "/tmp/minicode-501");
        CHECK(Rename::oldSocketFolder("/tmp/mutible-") == "");
        CHECK(Rename::oldSocketFolder("/x/forced") == "");
        CHECK(Rename::oldSocketFolder("relative") == "");
        // The client looks in an old copy's folder after the new one; a
        // forced folder (the checks) is the only one.
        const char *forced = getenv("MUTIBLE_CONTROL_DIR");
        const char *oldForced = getenv("MINICODE_CONTROL_DIR");
        if ((!forced || !*forced) && (!oldForced || !*oldForced)) {
            const std::vector<std::string> dirs = ControlChannel::socketFolders();
            CHECK(dirs.size() >= 2);
            if (dirs.size() >= 2) {
                CHECK(Rename::oldSocketFolder(dirs[0]) != "");
                CHECK(std::find(dirs.begin(), dirs.end(), Rename::oldSocketFolder(dirs[0])) !=
                      dirs.end());
            }
        }
    }

    GROUP("rename:migration");
    {
        const std::vector<Rename::Copy> mac = Rename::macItems("/Users/u");
        CHECK(mac.size() == 5);
        CHECK(mac[0] == (Rename::Copy{"/Users/u/Library/Application Support/MiniCode",
                                      "/Users/u/Library/Application Support/Mutible"}));
        CHECK(mac[1] == (Rename::Copy{"/Users/u/.config/minicode", "/Users/u/.config/mutible"}));
        CHECK(mac[2] == (Rename::Copy{"/Users/u/Library/WebKit/com.minicode.editor",
                                      "/Users/u/Library/WebKit/ai.ible.mutible"}));
        CHECK(mac[4].to == "/Users/u/Library/HTTPStorages/ai.ible.mutible.binarycookies");
        CHECK(Rename::macMarker("/Users/u") ==
              "/Users/u/Library/Application Support/Mutible/.migrated-from-minicode");

        const auto lin = Rename::linuxItems("/home/u", nullptr, "", "/data");
        CHECK(lin.size() == 3);
        CHECK(lin[0] == (Rename::Copy{"/home/u/.config/minicode", "/home/u/.config/mutible"}));
        CHECK(lin[1] == (Rename::Copy{"/home/u/.local/state/minicode", "/home/u/.local/state/mutible"}));
        CHECK(lin[2] == (Rename::Copy{"/data/minicode", "/data/mutible"}));
        CHECK(Rename::linuxItems("/h", "rel", nullptr, nullptr)[0].from == "/h/.config/minicode");
        CHECK(Rename::linuxMarker("/h", nullptr) == "/h/.local/share/mutible/.migrated-from-minicode");

        // Only what the old name has and the new one lacks is copied.
        std::set<std::string> there = {mac[0].from, mac[1].from, mac[1].to, mac[3].to};
        auto exists = [&](const std::string &p) { return there.count(p) > 0; };
        const auto todo = Rename::pending(mac, exists);
        CHECK(todo.size() == 1);
        CHECK(todo.size() == 1 && todo[0] == mac[0]);
        CHECK(Rename::pending(mac, [](const std::string &) { return false; }).empty());

        CHECK(Rename::shouldCopyDefaults(0, 12));
        CHECK(!Rename::shouldCopyDefaults(3, 12));
        CHECK(!Rename::shouldCopyDefaults(0, 0));

        const std::string text = Rename::markerText("2026-11-18T10:00:00Z", todo, {mac[2]}, true);
        CHECK(text.find("2026-11-18T10:00:00Z") != std::string::npos);
        CHECK(text.find("defaults com.minicode.editor -> ai.ible.mutible\n") != std::string::npos);
        CHECK(text.find(mac[0].from + " -> " + mac[0].to + "\n") != std::string::npos);
        CHECK(text.find(mac[2].from + " -> " + mac[2].to + " failed\n") != std::string::npos);
        CHECK(Rename::markerText("t", {}, {}, false).find("(nothing to copy)") != std::string::npos);
    }
}
