// adjust_settings_tests.inc — linux/PARITY.md item 2.11: what Adjust
// Settings does to the settings text, which the GTK port reads from the
// core (`settings:adjust-addition`, `settings:surface-key-lines`). The Mac
// builds the same in EditorController+Settings.mm, so these hold the core
// to the Mac's wording and order. Included by run_tests.cpp.

static void testAdjustSettings() {
    GROUP("settings:adjust-addition");
    {
        const std::string defaults = Settings::defaultFileText();
        // Nothing missing, nothing added; an unknown surface neither.
        for (const AppearanceSurface &s : Settings::appearanceSurfaces())
            CHECK(Settings::adjustSettingsAddition(defaults, s.id).empty());
        CHECK(Settings::adjustSettingsAddition("", "nothing").empty());
        CHECK(Settings::adjustSettingsAddition("lsp.enabled = false\n", "nothing").empty());

        // An empty file: the heading line, then the lines, no blank line first.
        const std::string missing = Settings::missingSurfaceLines("", "statusbar");
        CHECK(!missing.empty());
        CHECK(Settings::adjustSettingsAddition("", "statusbar") ==
              "# Added by Adjust Settings: the Status Bar keys this file did not have yet.\n" +
                  missing);

        // A file ending in a line break: one blank line before the heading.
        const std::string old = "lsp.enabled = false\n";
        const std::string add = Settings::adjustSettingsAddition(old, "terminal");
        CHECK(add.rfind("\n# Added by Adjust Settings: the Terminal keys this file did not "
                        "have yet.\n", 0) == 0);
        CHECK(add.find("# terminal.background = ") != std::string::npos);
        // A file without one at its end gets it first.
        const std::string bare = "lsp.enabled = false";
        CHECK(Settings::adjustSettingsAddition(bare, "terminal") == "\n" + add);

        // What it adds is what was missing: nothing is missing afterwards,
        // the result parses cleanly, and every line it adds is a comment
        // or blank, so the file means what it meant before.
        CHECK(Settings::missingSurfaceLines(old + add, "terminal").empty());
        CHECK(Settings::adjustSettingsAddition(old + add, "terminal").empty());
        std::vector<SettingsError> errors;
        Settings::parse(old + add, &errors);
        CHECK(errors.empty());
        size_t pos = 0;
        int lines = 0, active = 0;
        while (pos < add.size()) {
            size_t nl = add.find('\n', pos);
            if (nl == std::string::npos) nl = add.size();
            const std::string line = add.substr(pos, nl - pos);
            lines++;
            if (!line.empty() && line[0] != '#') active++;
            pos = nl + 1;
        }
        CHECK(lines > 4);
        CHECK(active == 0);
        // CRLF files: the addition keeps to LF, as the Mac's does, and the
        // CR before the file's last break is no line of its own.
        const std::string crlf = "lsp.enabled = false\r\n";
        CHECK(Settings::adjustSettingsAddition(crlf, "terminal") == add);
    }

    GROUP("settings:surface-key-lines");
    {
        CHECK(Settings::surfaceKeyLines("terminal.text = #FFFFFF\n", "nothing").empty());
        CHECK(Settings::surfaceKeyLines("lsp.enabled = false\n", "terminal").empty());
        // The surface's order (its own keys, then window.*), each line once,
        // a set line before a commented one.
        const std::string text = "# window.opacity = 1\n"        // 0
                                 "window.text = #D4D4D4\n"       // 1
                                 "\n"                            // 2
                                 "# terminal.text = #D4D4D4\n"   // 3
                                 "terminal.background = #181818\n"   // 4
                                 "# terminal.opacity = 1\n"      // 5
                                 "terminal.text = #EEEEEE\n";    // 6
        const std::vector<int> want = {4, 5, 6, 0, 1};
        CHECK(Settings::surfaceKeyLines(text, "terminal") == want);
        // The default file: one line per key of the surface, the first its
        // first key's.
        const std::string defaults = Settings::defaultFileText();
        for (const AppearanceSurface &s : Settings::appearanceSurfaces()) {
            const std::vector<int> got = Settings::surfaceKeyLines(defaults, s.id);
            CHECK(got.size() == s.keys.size());
            CHECK(!got.empty() && got[0] == Settings::findKeyLine(defaults, s.keys[0]));
        }
        // After an addition, the added keys are found below the old text.
        const std::string old = "lsp.enabled = false\n";
        const std::string grown = old + Settings::adjustSettingsAddition(old, "statusbar");
        const std::vector<int> found = Settings::surfaceKeyLines(grown, "statusbar");
        CHECK(found.size() == Settings::appearanceSurface("statusbar")->keys.size());
        for (int line : found) CHECK(line > 1);
    }
}
