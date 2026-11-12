// commands_linux_tests.inc — the registry's Linux keys (CommandsLinux.cpp,
// linux/PARITY.md D1): the table, keys.conf on Linux, the terminal's rule,
// GTK's accelerator strings, and README's Linux column. Included by
// run_tests.cpp after commands_tests.inc, whose K, HasKey and ReadmeKeys it
// uses.

static void testLinuxCommandTable() {
    using namespace Commands;
    GROUP("commands:linux-table");
    std::set<std::string> ids;
    size_t lastIndex = 0;
    bool ordered = true;
    for (const LinuxRow &r : linuxRows()) {
        const Command *c = find(r.id);
        if (!c) std::printf("  CommandsLinux.cpp: no command %s in the registry\n", r.id);
        CHECK(c != nullptr);
        CHECK(ids.insert(r.id).second);
        if (!c) continue;
        CHECK(linuxRow(r.id) == &r);
        CHECK(available(*c, Platform::Linux) && available(*c, Platform::Mac));
        // In the registry's order, so the file keys.conf writes and the menus
        // read as the Mac's do.
        const size_t at = (size_t)(c - all().data());
        if (at < lastIndex) {
            ordered = false;
            std::printf("  CommandsLinux.cpp: %s is out of the registry's order\n", r.id);
        }
        lastIndex = at;
        // Moved menus are top-level ones the port has, or a submenu of one
        // ("View > Editor Layout").
        if (r.menu) {
            const std::string whole = r.menu;
            const std::string m = whole.substr(0, whole.find(" > "));
            CHECK(m == "File" || m == "Edit" || m == "View" || m == "Navigate" || m == "Terminal");
            CHECK(m == whole || whole.size() > m.size() + 3);
        }
        CHECK(!r.title || *r.title);
        CHECK((r.flags & ~(unsigned)Global) == 0);
        // Every key parses, is written as format() writes it, and is one a
        // GTK accelerator can be; none has cmd, and none is GNOME's.
        std::string word, list = r.keys;
        size_t n = 0;
        for (size_t i = 0; i <= list.size(); i++) {
            if (i < list.size() && list[i] != ' ') { word += list[i]; continue; }
            if (word.empty()) continue;
            Key k;
            std::string why;
            const bool ok = parseKey(word, &k, &why);
            if (!ok) std::printf("  %s: bad Linux key %s (%s)\n", r.id, word.c_str(), why.c_str());
            CHECK(ok);
            if (ok && format(k) != word)
                std::printf("  %s: write %s as %s\n", r.id, word.c_str(), format(k).c_str());
            CHECK(!ok || format(k) == word);
            CHECK(!(k.mods & Cmd));
            CHECK(!linuxAccelerators(k).empty());
            // A key that types needs a language (Alt+C in Markdown) or the
            // file tree, which takes no text (Delete, as in VS Code).
            if (!(c->flags & Fixed) && !c->language && c->when != When::Tree)
                CHECK(!typesText(k));
            // Ctrl+Alt+arrows switch workspaces and Ctrl+Shift+Alt+arrows
            // move a window to another (VS Code's Linux Copy Line keys).
            const bool arrow = k.name == "up" || k.name == "down" || k.name == "left" ||
                               k.name == "right";
            const bool gnome = (k.mods == (Ctrl | Alt) &&
                                (arrow || k.name == "t" || k.name == "delete")) ||
                               (k.mods == (Ctrl | Alt | Shift) && arrow);
            if (gnome) std::printf("  %s: %s is GNOME's\n", r.id, word.c_str());
            CHECK(!gnome);
            n++;
            word.clear();
        }
        CHECK(defaultKeys(*c, Platform::Linux).size() == n);
    }
    CHECK(ordered);
    CHECK(linuxRows().size() >= 40);
    CHECK(linuxRow("nope") == nullptr);
    // The palette is there (PARITY.md 1.5), and so is the inline agent
    // (3.3). Neither is the Mac's key.
    CHECK(linuxRow("workbench.action.showCommands") != nullptr);
    CHECK(available(*find("workbench.action.showCommands"), Platform::Linux));
    CHECK(linuxRow("inlineChat.start") != nullptr);
    CHECK(available(*find("inlineChat.start"), Platform::Linux));
    CHECK(defaultKeys(*find("inlineChat.start"), Platform::Linux).size() == 1);
    CHECK(linuxRow("workbench.action.showAllSymbols") != nullptr);
    CHECK(available(*find("workbench.action.showAllSymbols"), Platform::Linux));
    // Linux's words where the Mac's do not fit, the registry's elsewhere.
    CHECK(std::string(titleOf(*find("revealFileInOS"), Platform::Linux)) == "Open Containing Folder");
    CHECK(std::string(titleOf(*find("revealFileInOS"), Platform::Mac)) == "Reveal in Finder");
    CHECK(std::string(menuOf(*find("workbench.action.quit"), Platform::Linux)) == "File");
    CHECK(std::string(menuOf(*find("workbench.action.quit"), Platform::Mac)) == "minicode");
    CHECK(std::string(titleOf(*find("actions.find"), Platform::Linux)) == "Find…");
    // Global: the Mac's, and Linux's keys out of the terminal.
    const Command &toggle = *find("workbench.action.terminal.toggleTerminal");
    const Command &tree = *find("workbench.action.focusSideBar");
    CHECK((flagsOf(toggle, Platform::Mac) & Global) && (flagsOf(toggle, Platform::Linux) & Global));
    CHECK(!(flagsOf(tree, Platform::Mac) & Global) && (flagsOf(tree, Platform::Linux) & Global));
}

static void testLinuxCommandDefaults() {
    using namespace Commands;
    GROUP("commands:linux-defaults");
    const Keymap d = defaults(Platform::Linux);
    std::vector<std::string> clashes = d.conflicts();
    for (const std::string &s : clashes) std::printf("  Linux default conflict: %s\n", s.c_str());
    CHECK(clashes.empty());
    // The Mac's are unchanged, and the Platform-less functions are the Mac's.
    CHECK(defaults().keys == defaults(Platform::Mac).keys);
    CHECK(HasKey(defaults(), "markdown.showPreview", "shift+cmd+v"));
    CHECK(defaultFileText() == defaultFileText(Platform::Mac));
    // The keys D1 moves.
    CHECK(d.keysOf("markdown.showPreview").size() == 1 &&
          HasKey(d, "markdown.showPreview", "ctrl+shift+v"));
    CHECK(d.keysOf("minicode.markdown.toggleTask").size() == 2 &&
          d.keysOf("minicode.markdown.toggleTask")[0] == K("ctrl+alt+l") &&
          HasKey(d, "minicode.markdown.toggleTask", "alt+c"));
    CHECK(d.keysOf("minicode.showTodos").size() == 1 &&
          HasKey(d, "minicode.showTodos", "ctrl+shift+d"));
    CHECK(d.keysOf("explorer.newFile").size() == 1 && HasKey(d, "explorer.newFile", "ctrl+n"));
    CHECK(d.keysOf("workbench.action.newWindow").size() == 1 &&
          HasKey(d, "workbench.action.newWindow", "ctrl+shift+n"));
    CHECK(HasKey(d, "explorer.newFolder", "ctrl+alt+n"));
    CHECK(d.keysOf("workbench.action.closeWindow").size() == 1 &&
          HasKey(d, "workbench.action.closeWindow", "ctrl+shift+w"));
    CHECK(d.keysOf("editor.action.showHover").size() == 1 &&
          HasKey(d, "editor.action.showHover", "ctrl+alt+i"));
    CHECK(d.keysOf("editor.action.nextMatchFindAction").size() == 1 &&
          HasKey(d, "editor.action.nextMatchFindAction", "f3"));
    CHECK(d.keysOf("minicode.toggleHiddenFiles").size() == 1 &&
          HasKey(d, "minicode.toggleHiddenFiles", "ctrl+shift+."));
    // Go to Symbol in Workspace is VS Code's Ctrl+T (PARITY.md 2.12).
    CHECK(d.keysOf("workbench.action.showAllSymbols").size() == 1 &&
          HasKey(d, "workbench.action.showAllSymbols", "ctrl+t"));
    // Ctrl+I is the inline agent's (PARITY.md 3.3); the keys the rest leave
    // are free until the commands that take them come (the panel's Ctrl+J).
    CHECK(HasKey(d, "inlineChat.start", "ctrl+i"));
    for (const char *k : {"ctrl+j", "ctrl+alt+t"}) {
        const bool free = d.commandsFor(K(k)).empty();
        if (!free) std::printf("  Linux: %s is %s\n", k, d.commandsFor(K(k))[0]->id);
        CHECK(free);
    }
    // Several carets and the line commands (PARITY.md 1.9): VS Code's Linux
    // keys, but Add Cursor's first (Shift+Alt+Up and Down, Copy Line's
    // here, since VS Code's Linux Copy Line keys are GNOME's) and Copy
    // Line's (Ctrl+Shift+Alt+Up and Down). Ctrl+L and Ctrl+Shift+L, which
    // D1 freed, are Select Line's and Select All Occurrences'.
    CHECK(d.keysOf("expandLineSelection").size() == 1 && HasKey(d, "expandLineSelection", "ctrl+l"));
    CHECK(d.keysOf("editor.action.selectHighlights").size() == 1 &&
          HasKey(d, "editor.action.selectHighlights", "ctrl+shift+l"));
    CHECK(d.keysOf("editor.action.addSelectionToNextFindMatch").size() == 1 &&
          HasKey(d, "editor.action.addSelectionToNextFindMatch", "ctrl+d"));
    CHECK(HasKey(d, "editor.action.insertCursorAbove", "ctrl+shift+up") &&
          HasKey(d, "editor.action.insertCursorBelow", "ctrl+shift+down"));
    CHECK(HasKey(d, "editor.action.deleteLines", "ctrl+shift+k"));
    CHECK(HasKey(d, "editor.action.moveLinesUpAction", "alt+up") &&
          HasKey(d, "editor.action.moveLinesDownAction", "alt+down"));
    CHECK(HasKey(d, "editor.action.copyLinesUpAction", "alt+shift+up") &&
          HasKey(d, "editor.action.copyLinesDownAction", "alt+shift+down"));
    CHECK(HasKey(d, "cursorTop", "ctrl+home") && HasKey(d, "cursorBottomSelect", "ctrl+shift+end"));
    CHECK(std::string(menuOf(*find("editor.action.moveLinesUpAction"), Platform::Linux)) == "Edit");
    CHECK(std::string(menuOf(*find("editor.action.moveLinesUpAction"), Platform::Mac)) == "Editor");
    // Editor commands: in the terminal the shell keeps Ctrl+D and Ctrl+L.
    CHECK(d.commandFor(K("ctrl+d"), When::Terminal) == nullptr);
    CHECK(d.commandFor(K("ctrl+l"), When::Terminal) == nullptr);
    // The palette (PARITY.md 1.5): VS Code's Linux keys.
    CHECK(d.keysOf("workbench.action.showCommands").size() == 2 &&
          d.keysOf("workbench.action.showCommands")[0] == K("ctrl+shift+p") &&
          HasKey(d, "workbench.action.showCommands", "f1"));
    CHECK(d.keysOf("workbench.action.quickOpen").size() == 1 &&
          HasKey(d, "workbench.action.quickOpen", "ctrl+p"));
    CHECK(d.keysOf("workbench.action.gotoLine").size() == 1 &&
          HasKey(d, "workbench.action.gotoLine", "ctrl+g"));
    CHECK(d.keysOf("workbench.action.gotoSymbol").size() == 1 &&
          HasKey(d, "workbench.action.gotoSymbol", "ctrl+shift+o"));
    CHECK(d.commandFor(K("ctrl+shift+p"), When::Editor) == find("workbench.action.showCommands"));
    CHECK(d.commandFor(K("ctrl+g"), When::Tree) == find("workbench.action.gotoLine"));
    // The find bar and Replace in Folder (PARITY.md 1.6): VS Code's Linux
    // keys, but Replace in Folder's Ctrl+Shift+H, which is the shortcut
    // hints' (minicode's own Shift key).
    CHECK(d.keysOf("editor.action.startFindReplaceAction").size() == 1 &&
          HasKey(d, "editor.action.startFindReplaceAction", "ctrl+h"));
    CHECK(d.keysOf("editor.action.replaceOne").size() == 1 &&
          HasKey(d, "editor.action.replaceOne", "ctrl+shift+1"));
    CHECK(d.keysOf("editor.action.replaceAll").size() == 1 &&
          HasKey(d, "editor.action.replaceAll", "ctrl+alt+enter"));
    CHECK(HasKey(d, "toggleFindCaseSensitive", "alt+c") && HasKey(d, "toggleFindWholeWord", "alt+w") &&
          HasKey(d, "toggleFindRegex", "alt+r") && HasKey(d, "togglePreserveCase", "alt+p") &&
          HasKey(d, "toggleFindInSelection", "alt+l"));
    CHECK(d.keysOf("workbench.action.replaceInFiles").size() == 1 &&
          HasKey(d, "workbench.action.replaceInFiles", "ctrl+alt+shift+f"));
    CHECK(d.commandFor(K("ctrl+shift+h"), When::Editor) == find("minicode.toggleShortcutHints"));
    // Alt+C is Match Case's, but the task's in a Markdown file's editor.
    CHECK(d.commandFor(K("alt+c"), When::Editor) == find("toggleFindCaseSensitive"));
    CHECK(d.commandFor(K("alt+c"), When::Any) == find("toggleFindCaseSensitive"));
    // The tabs (PARITY.md 1.1): VS Code's Linux keys, but Reopen Closed Tab,
    // whose Ctrl+Shift+T stays Toggle Terminal's (minicode's own Shift key),
    // and Save All, whose VS Code key is a chord.
    CHECK(d.keysOf("workbench.action.closeActiveEditor").size() == 2 &&
          d.keysOf("workbench.action.closeActiveEditor")[0] == K("ctrl+w") &&
          HasKey(d, "workbench.action.closeActiveEditor", "ctrl+f4"));
    CHECK(d.keysOf("workbench.action.nextEditor").size() == 2 &&
          d.keysOf("workbench.action.nextEditor")[0] == K("ctrl+pagedown") &&
          HasKey(d, "workbench.action.nextEditor", "ctrl+tab"));
    CHECK(d.keysOf("workbench.action.previousEditor").size() == 2 &&
          d.keysOf("workbench.action.previousEditor")[0] == K("ctrl+pageup") &&
          HasKey(d, "workbench.action.previousEditor", "ctrl+shift+tab"));
    CHECK(d.commandFor(K("ctrl+shift+t"), When::Editor) ==
          find("workbench.action.terminal.toggleTerminal"));
    CHECK(d.keysOf("workbench.action.reopenClosedEditor").size() == 1 &&
          HasKey(d, "workbench.action.reopenClosedEditor", "ctrl+alt+shift+t"));
    CHECK(d.keysOf("workbench.action.files.saveAll").size() == 1 &&
          HasKey(d, "workbench.action.files.saveAll", "ctrl+alt+s"));
    for (int n = 1; n <= 8; n++) {
        const std::string id = "workbench.action.openEditorAtIndex" + std::to_string(n);
        CHECK(d.keysOf(id).size() == 1 && HasKey(d, id.c_str(), ("alt+" + std::to_string(n)).c_str()));
    }
    CHECK(d.keysOf("workbench.action.lastEditorInGroup").size() == 2 &&
          d.keysOf("workbench.action.lastEditorInGroup")[0] == K("alt+9") &&
          HasKey(d, "workbench.action.lastEditorInGroup", "alt+0"));
    for (const char *id : {"workbench.action.closeOtherEditors", "workbench.action.closeAllEditors",
                           "workbench.action.closeEditorsToTheRight", "workbench.action.keepEditor"})
        CHECK(available(*find(id), Platform::Linux) && d.keysOf(id).empty());
    // No key of the Mac's anywhere.
    for (const auto &[id, keys] : d.keys)
        for (const Key &k : keys) CHECK(!(k.mods & Cmd));
    // The tree's keys are the tree's; Alt+C is Markdown's.
    CHECK(d.commandFor(K("f2"), When::Tree) == find("renameFile"));
    CHECK(d.commandFor(K("delete"), When::Tree) == find("moveFileToTrash"));
    CHECK(d.commandFor(K("delete"), When::Editor) == nullptr);
    CHECK(d.commandFor(K("alt+c"), When::Editor, "markdown") == find("minicode.markdown.toggleTask"));
    CHECK(d.commandFor(K("alt+c"), When::Editor, "python") == find("toggleFindCaseSensitive"));
    CHECK(d.commandFor(K("ctrl+alt+l"), When::Editor, "markdown") ==
          find("minicode.markdown.toggleTask"));
    // The keymap gives Ctrl+Shift+V to the preview in the terminal too; the
    // port leaves it to VTE's paste there (linuxTerminalTakes, below).
    CHECK(d.commandFor(K("ctrl+shift+v"), When::Terminal) == find("markdown.showPreview"));
    CHECK(d.commandFor(K("ctrl+0"), When::Terminal) == find("workbench.action.focusSideBar"));
}

static void testLinuxTerminalKeys() {
    using namespace Commands;
    GROUP("commands:linux-terminal");
    auto takes = [](const char *id, const char *key) {
        return linuxTerminalTakes(*find(id), K(key));
    };
    // Plain Ctrl keys and unmodified keys are the program's...
    CHECK(takes("explorer.newFile", "ctrl+n"));
    CHECK(takes("workbench.action.toggleSidebarVisibility", "ctrl+b"));
    CHECK(takes("workbench.action.quit", "ctrl+q"));
    CHECK(takes("editor.action.revealDefinition", "f12"));
    CHECK(takes("minicode.sortLines", "f9"));
    CHECK(takes("editor.action.nextMatchFindAction", "f3"));
    // The palette's plain keys too (readline's Ctrl+P, Emacs's Ctrl+G); its
    // Shift keys open it from the terminal.
    CHECK(takes("workbench.action.quickOpen", "ctrl+p"));
    CHECK(takes("workbench.action.gotoLine", "ctrl+g"));
    CHECK(takes("workbench.action.showAllSymbols", "ctrl+t"));   // fzf's, readline's
    CHECK(takes("workbench.action.showCommands", "f1"));
    CHECK(!takes("workbench.action.showCommands", "ctrl+shift+p"));
    CHECK(!takes("workbench.action.gotoSymbol", "ctrl+shift+o"));
    // ... unless Global: the ways out of the terminal.
    CHECK(!takes("workbench.action.terminal.toggleTerminal", "ctrl+`"));
    CHECK(!takes("workbench.action.focusSideBar", "ctrl+0"));
    CHECK(!takes("workbench.action.focusFirstEditorGroup", "ctrl+1"));
    CHECK(!takes("workbench.action.nextEditor", "ctrl+tab"));
    // Shift and Alt stay the app's...
    CHECK(!takes("workbench.action.terminal.toggleTerminal", "ctrl+shift+t"));
    CHECK(!takes("minicode.showTodos", "ctrl+shift+d"));
    CHECK(!takes("workbench.action.newWindow", "ctrl+shift+n"));
    CHECK(!takes("explorer.newFolder", "ctrl+alt+n"));
    CHECK(!takes("editor.action.previousMatchFindAction", "shift+f3"));
    CHECK(!takes("minicode.toggleHiddenFiles", "ctrl+shift+."));
    // ... but for the terminal's copy and paste, and the two a shell reads
    // as Ctrl+G and Ctrl+Space.
    CHECK(takes("markdown.showPreview", "ctrl+shift+v"));
    CHECK(takes("workbench.view.scm", "ctrl+shift+g"));
    CHECK(takes("minicode.togglePlayback", "ctrl+shift+space"));
    CHECK(linuxTerminalOwnKey(K("ctrl+shift+c")) && linuxTerminalOwnKey(K("ctrl+shift+v")));
    CHECK(!linuxTerminalOwnKey(K("ctrl+c")) && !linuxTerminalOwnKey(K("ctrl+shift+p")));
    // Even a Global command leaves the terminal its own keys.
    CHECK(linuxTerminalTakes(*find("workbench.action.terminal.toggleTerminal"), K("ctrl+shift+v")));
    // Of the Linux keys, these are all that stay bound in the terminal with
    // no Shift or Alt.
    std::set<std::string> plain;
    const Keymap d = defaults(Platform::Linux);
    for (const LinuxRow &r : linuxRows()) {
        const Command &c = *find(r.id);
        if (!appliesIn(c.when, When::Terminal)) continue;
        for (const Key &k : d.keysOf(r.id))
            if (!linuxTerminalTakes(c, k) && !(k.mods & (Shift | Alt))) plain.insert(format(k));
    }
    // The terminals' own zoom keeps its keys there (PARITY.md 1.11), the
    // inline agent's Ctrl+I is Global (an idle terminal asks for a command,
    // PARITY.md 3.3), and Ctrl+2 focuses the editor's second group from
    // there as Ctrl+1 the first (PARITY.md 4.6).
    CHECK((plain == std::set<std::string>{"ctrl+`", "ctrl+0", "ctrl+1", "ctrl+2", "ctrl+tab",
                                           "ctrl+pagedown", "ctrl+pageup", "ctrl+=", "ctrl+-",
                                           "ctrl+numpad0", "ctrl+i"}));
    // Alt with a letter alone is Meta to the program: the find bar's
    // options leave it its word keys; Alt with a digit (the tabs) and with
    // Ctrl stays the app's.
    CHECK(takes("toggleFindCaseSensitive", "alt+c"));
    CHECK(takes("toggleFindWholeWord", "alt+w"));
    CHECK(takes("toggleFindRegex", "alt+r"));
    CHECK(takes("togglePreserveCase", "alt+p"));
    CHECK(takes("toggleFindInSelection", "alt+l"));
    CHECK(!takes("editor.action.replaceAll", "ctrl+alt+enter"));
    CHECK(!takes("workbench.action.replaceInFiles", "ctrl+alt+shift+f"));
    CHECK(!takes("editor.action.replaceOne", "ctrl+shift+1"));
    CHECK(takes("editor.action.startFindReplaceAction", "ctrl+h"));
    // Close Tab's keys are the shell's there (Ctrl+W deletes a word).
    CHECK(takes("workbench.action.closeActiveEditor", "ctrl+w"));
    CHECK(takes("workbench.action.closeActiveEditor", "ctrl+f4"));
    CHECK(!takes("workbench.action.nextEditor", "ctrl+pagedown"));
    CHECK(!takes("workbench.action.openEditorAtIndex1", "alt+1"));
    // Go to Bracket (PARITY.md 1.7) is VS Code's Linux Ctrl+Shift+\, an
    // editor command: never bound while the terminal has the keyboard, so
    // the program there gets the key; its accelerator is the character
    // Shift makes (HANDOFF.md, "Traps already known").
    const Command &bracket = *find("editor.action.jumpToBracket");
    CHECK(d.keysOf(bracket.id).size() == 1 && format(d.keysOf(bracket.id)[0]) == "ctrl+shift+\\");
    CHECK(!appliesIn(bracket.when, When::Terminal));
    CHECK(linuxAccelerators(K("ctrl+shift+\\")) == std::vector<std::string>{"<Ctrl>bar"});
}

static void testLinuxAccelerators() {
    using namespace Commands;
    GROUP("commands:linux-accelerators");
    using V = std::vector<std::string>;
    CHECK(linuxAccelerators(K("ctrl+s")) == V{"<Ctrl>s"});
    CHECK(linuxAccelerators(K("ctrl+shift+p")) == V{"<Ctrl><Shift>p"});
    CHECK(linuxAccelerators(K("ctrl+alt+l")) == V{"<Ctrl><Alt>l"});
    CHECK(linuxAccelerators(K("alt+c")) == V{"<Alt>c"});
    // GTK matches the keyval a key produced: Shift with punctuation is the
    // character it types (HANDOFF.md, the <Ctrl>greater trap).
    CHECK(linuxAccelerators(K("ctrl+shift+.")) == V{"<Ctrl>greater"});
    CHECK(linuxAccelerators(K("ctrl+shift+1")) == V{"<Ctrl>exclam"});
    CHECK(linuxAccelerators(K("ctrl+shift+`")) == V{"<Ctrl>asciitilde"});
    CHECK(linuxAccelerators(K("ctrl+=")) == V{"<Ctrl>equal"});
    CHECK((linuxAccelerators(K("ctrl+shift+=")) == V{"<Ctrl>plus", "<Ctrl>KP_Add"}));
    CHECK((linuxAccelerators(K("ctrl+-")) == V{"<Ctrl>minus", "<Ctrl>KP_Subtract"}));
    CHECK(linuxAccelerators(K("ctrl+`")) == V{"<Ctrl>grave"});
    CHECK(linuxAccelerators(K("ctrl+,")) == V{"<Ctrl>comma"});
    CHECK(linuxAccelerators(K("ctrl+/")) == V{"<Ctrl>slash"});
    CHECK(linuxAccelerators(K("ctrl+0")) == V{"<Ctrl>0"});
    CHECK(linuxAccelerators(K("ctrl+alt+0")) == V{"<Ctrl><Alt>0"});
    CHECK(linuxAccelerators(K("f3")) == V{"F3"});
    CHECK(linuxAccelerators(K("shift+f3")) == V{"<Shift>F3"});
    CHECK(linuxAccelerators(K("ctrl+shift+space")) == V{"<Ctrl><Shift>space"});
    CHECK(linuxAccelerators(K("ctrl+tab")) == V{"<Ctrl>Tab"});
    CHECK(linuxAccelerators(K("delete")) == V{"Delete"});
    // The keypad's key with Num Lock on and with it off.
    CHECK((linuxAccelerators(K("ctrl+numpad0")) == V{"<Ctrl>KP_0", "<Ctrl>KP_Insert"}));
    CHECK((linuxAccelerators(K("alt+numpad9")) == V{"<Alt>KP_9", "<Alt>KP_Prior"}));
    CHECK(linuxAccelerators(K("shift+pageup")) == V{"<Shift>Page_Up"});
    CHECK(linuxAccelerators(K("cmd+s")).empty());
    CHECK(linuxAccelerators(Key()).empty());

    CHECK(linuxLabel(K("ctrl+shift+p")) == "Ctrl Shift P");
    CHECK(linuxLabel(K("ctrl+alt+l")) == "Ctrl Alt L");
    CHECK(linuxLabel(K("ctrl+`")) == "Ctrl `");
    CHECK(linuxLabel(K("ctrl+shift+.")) == "Ctrl Shift .");
    CHECK(linuxLabel(K("shift+f3")) == "Shift F3");
    CHECK(linuxLabel(K("delete")) == "Delete");
    CHECK(linuxLabel(K("ctrl+shift+space")) == "Ctrl Shift Space");
    CHECK(linuxLabel(K("ctrl+shift+h"), "+") == "Ctrl+Shift+H");
    CHECK(linuxLabel(K("shift+pagedown")) == "Shift Page Down");
    CHECK(linuxLabel(K("ctrl+numpad0")) == "Ctrl Keypad 0");
}

static void testLinuxKeymapFile() {
    using namespace Commands;
    GROUP("commands:linux-keymap");
    std::vector<Error> errs;
    // A key added goes first; the default stays.
    Keymap m = resolve("ctrl+alt+j = minicode.toggleShortcutHints\n", &errs, Platform::Linux);
    CHECK(errs.empty());
    CHECK(m.keysOf("minicode.toggleShortcutHints").size() == 2 &&
          m.keysOf("minicode.toggleShortcutHints")[0] == K("ctrl+alt+j"));
    // Taken away, from every command or one.
    m = resolve("ctrl+shift+h = -\nctrl+b = -workbench.action.toggleSidebarVisibility\n", &errs,
                Platform::Linux);
    CHECK(errs.empty());
    CHECK(m.keysOf("minicode.toggleShortcutHints").empty());
    CHECK(m.keysOf("workbench.action.toggleSidebarVisibility").empty());
    // A key given to a command that applies everywhere leaves the other.
    m = resolve("ctrl+shift+v = minicode.toggleBrowser\n", &errs, Platform::Linux);
    CHECK(errs.empty() && m.keysOf("markdown.showPreview").empty());
    CHECK(m.commandFor(K("ctrl+shift+v"), When::Editor) == find("minicode.toggleBrowser"));
    // A Mac's line: no Command key here.
    m = resolve("cmd+k = minicode.toggleBrowser\n", &errs, Platform::Linux);
    CHECK(errs.size() == 1 && errs[0].line == 1 &&
          errs[0].message.find("no Command key") != std::string::npos);
    CHECK(m.keys == defaults(Platform::Linux).keys);
    errs.clear();
    m = resolve("cmd+k = -\n", &errs, Platform::Linux);
    CHECK(errs.size() == 1);
    // A command Linux does not have yet (the registry's Mac-only rows).
    errs.clear();
    m = resolve("ctrl+alt+p = minicode.requestChange\n", &errs, Platform::Linux);
    CHECK(errs.size() == 1 && errs[0].message.find("not in minicode for Linux yet") != std::string::npos);
    CHECK(m.commandsFor(K("ctrl+alt+p")).empty());
    // The same line on the Mac is fine, and its keys are the Mac's.
    errs.clear();
    m = resolve("ctrl+alt+p = minicode.requestChange\n", &errs);
    CHECK(errs.empty() && HasKey(m, "minicode.requestChange", "shift+cmd+k"));
    // The palette's own keys move like any other.
    errs.clear();
    m = resolve("ctrl+alt+p = workbench.action.showCommands\n", &errs, Platform::Linux);
    CHECK(errs.empty() && m.keysOf("workbench.action.showCommands")[0] == K("ctrl+alt+p"));
    // The tree takes no text, so its commands may have a typing key on
    // Linux; the Mac still refuses it.
    errs.clear();
    m = resolve("shift+delete = moveFileToTrash\n", &errs, Platform::Linux);
    CHECK(errs.empty() && m.commandFor(K("shift+delete"), When::Tree) == find("moveFileToTrash"));
    m = resolve("delete = -moveFileToTrash\n", &errs, Platform::Linux);
    CHECK(errs.empty() && m.keysOf("moveFileToTrash").empty());
    m = resolve("shift+delete = moveFileToTrash\n", &errs);
    CHECK(errs.size() == 1);
    // Two lines, one key, two commands in one place.
    errs.clear();
    m = resolve("ctrl+alt+j = minicode.toggleBrowser\nctrl+alt+j = minicode.toggleEditor\n", &errs,
                Platform::Linux);
    CHECK(errs.size() == 1 && errs[0].line == 2);
    // An editor command and a tree command may share one.
    errs.clear();
    m = resolve("ctrl+alt+j = editor.action.commentLine\nctrl+alt+j = renameFile\n", &errs,
                Platform::Linux);
    CHECK(errs.empty() && m.conflicts().empty());

    GROUP("commands:linux-file-text");
    // Keyboard Shortcuts' file on Linux: as written, nothing changes, and
    // uncommented every line reads back as Linux's defaults.
    const std::string text = defaultFileText(Platform::Linux);
    errs.clear();
    Keymap same = resolve(text, &errs, Platform::Linux);
    CHECK(errs.empty());
    CHECK(same.keys == defaults(Platform::Linux).keys);
    std::string uncommented, line;
    std::istringstream in(text);
    size_t bindings = 0;
    bool listing = false;
    while (std::getline(in, line)) {
        if (line.rfind("# ---- ", 0) == 0) listing = true;
        if (listing && line.rfind("# ", 0) == 0 && line.find(" = ") != std::string::npos &&
            line.find("(no key)") == std::string::npos) {
            uncommented += line.substr(2) + "\n";
            bindings++;
        }
    }
    errs.clear();
    Keymap back = resolve(uncommented, &errs, Platform::Linux);
    for (const Error &e : errs) std::printf("  Linux file-text line %d: %s\n", e.line, e.message.c_str());
    CHECK(errs.empty());
    CHECK(bindings >= 40);
    bool sameKeys = true;
    for (const Command &c : all()) {
        std::vector<Key> a = back.keysOf(c.id), b = defaults(Platform::Linux).keysOf(c.id);
        auto byName = [](const Key &x, const Key &y) { return format(x) < format(y); };
        std::sort(a.begin(), a.end(), byName);
        std::sort(b.begin(), b.end(), byName);
        if (!(a == b)) { sameKeys = false; std::printf("  Linux file-text changes %s\n", c.id); }
    }
    CHECK(sameKeys);
    // Every row but a view's own keys (Fixed: Move Line, Ctrl+Home), which
    // keys.conf cannot change yet and the file leaves out, as the Mac's does.
    for (const LinuxRow &r : linuxRows()) {
        if (find(r.id)->flags & Fixed) {
            CHECK(text.find(std::string(" = ") + r.id) == std::string::npos);
            continue;
        }
        CHECK(text.find(std::string(" = ") + r.id) != std::string::npos);
    }
    // Only Linux's commands, in Linux's words, with no Mac key.
    CHECK(text.find("minicode.requestChange") == std::string::npos);
    CHECK(text.find("# ctrl+shift+p = workbench.action.showCommands") != std::string::npos);
    CHECK(text.find("cmd+") == std::string::npos);
    CHECK(text.find("File > Open Containing Folder") != std::string::npos);
    CHECK(text.find("Finder") == std::string::npos);
    CHECK(text.find("# ---- File") != std::string::npos && text.find("# ---- minicode") == std::string::npos);
}

// README's Linux column against the table: every key in the table is in it,
// and every key in it is the table's or one the editor and the tree handle
// themselves, so a key that moves cannot stay behind there.
static void testLinuxCommandsReadme() {
    using namespace Commands;
    GROUP("commands:linux-readme");
    std::ifstream f("README.md");
    if (!f) {
        std::printf("  (README.md not found here; README checks skipped)\n");
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string readme = ss.str();
    const size_t start = readme.find("| Action | macOS | Linux | Android |");
    CHECK(start != std::string::npos);
    if (start == std::string::npos) return;
    const size_t end = readme.find("\n\n", start);
    std::set<std::string> listed;
    std::istringstream rows(readme.substr(start, end - start));
    std::string row;
    while (std::getline(rows, row)) {
        size_t a = row.find('|', 1);
        size_t b = a == std::string::npos ? a : row.find('|', a + 1);
        size_t c = b == std::string::npos ? b : row.find('|', b + 1);
        if (c == std::string::npos) continue;
        for (const Key &k : ReadmeKeys(row.substr(b + 1, c - b - 1), true)) listed.insert(format(k));
    }
    std::set<std::string> table;
    const Keymap d = defaults(Platform::Linux);
    for (const LinuxRow &r : linuxRows())
        for (const Key &k : d.keysOf(r.id)) {
            table.insert(format(k));
            const bool found = listed.count(format(k)) > 0;
            if (!found) std::printf("  README's Linux column lacks %s (%s)\n", format(k).c_str(), r.id);
            CHECK(found);
        }
    // The text view's undo and indent keys and the tree's arrows, which no
    // command has; and since item 1.9 its clipboard keys, word moves and
    // scrolling by a line, Escape back to one caret, and the find bar's
    // Alt+Enter (Carets.h), which act on the view's own signals; and since
    // item 2.5 the completion list's Shift+Enter (a new line instead).
    const std::set<std::string> own = {"ctrl+z", "ctrl+shift+z", "tab", "shift+tab", "up",
                                       "down", "left", "right", "enter", "shift+enter",
                                       "ctrl+c", "ctrl+x", "ctrl+v", "escape", "alt+enter",
                                       "ctrl+left", "ctrl+right", "ctrl+backspace", "ctrl+delete",
                                       "ctrl+up", "ctrl+down"};
    for (const std::string &k : listed) {
        Key key;
        parseKey(k, &key);
        // A lone letter is a word of the prose ("or a pinch").
        if (key.mods == 0 && key.name.size() == 1) continue;
        const bool known = table.count(k) || own.count(k);
        if (!known) std::printf("  README's Linux column has %s, which no Linux command has\n", k.c_str());
        CHECK(known);
    }
}
