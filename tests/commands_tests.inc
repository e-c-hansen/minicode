// commands_tests.inc — the command registry and keys.conf (Commands.h),
// included by run_tests.cpp.

static Commands::Key K(const char *text) {
    Commands::Key k;
    std::string why;
    if (!Commands::parseKey(text, &k, &why)) std::printf("  bad key in test: %s (%s)\n", text, why.c_str());
    return k;
}

static bool HasKey(const Commands::Keymap &m, const char *id, const char *key) {
    for (const Commands::Key &k : m.keysOf(id))
        if (k == K(key)) return true;
    return false;
}

static void testCommandKeys() {
    using namespace Commands;
    GROUP("commands:keys");
    Key k;
    std::string why;
    CHECK(parseKey("shift+cmd+p", &k) && k.mods == (Shift | Cmd) && k.name == "p");
    CHECK(parseKey(" Cmd + Shift + P ", &k) && k.mods == (Shift | Cmd) && k.name == "p");
    CHECK(parseKey("command+option+control+shift+F", &k) && k.mods == (Ctrl | Alt | Shift | Cmd));
    CHECK(parseKey("opt+]", &k) && k.mods == Alt && k.name == "]");
    CHECK(parseKey("ctrl+`", &k) && k.name == "`");
    CHECK(parseKey("ctrl+backtick", &k) && k.name == "`");
    CHECK(parseKey("cmd+=", &k) && k.name == "=");
    CHECK(parseKey("shift+F12", &k) && k.name == "f12" && k.mods == Shift);
    CHECK(parseKey("f20", &k) && k.name == "f20");
    CHECK(!parseKey("f21", &k, &why));
    CHECK(!parseKey("f0", &k, &why));
    CHECK(parseKey("cmd+return", &k) && k.name == "enter");
    CHECK(parseKey("esc", &k) && k.name == "escape");
    CHECK(parseKey("cmd+keypad0", &k) && k.name == "numpad0");
    CHECK(parseKey("cmd+num5", &k) && k.name == "numpad5");
    CHECK(parseKey("menu", &k) && k.name == "contextmenu");
    CHECK(!parseKey("", &k, &why) && why == "no key");
    CHECK(!parseKey("cmd+shift", &k, &why) && why.find("no key") != std::string::npos);
    CHECK(!parseKey("hyper+k", &k, &why) && why.find("modifier") != std::string::npos);
    CHECK(!parseKey("cmd+k cmd+s", &k, &why) && why.find("chords") != std::string::npos);
    CHECK(!parseKey("ctrl+~", &k, &why) && why.find("shift+`") != std::string::npos);
    CHECK(!parseKey("cmd+{", &k, &why) && why.find("shift+[") != std::string::npos);
    CHECK(!parseKey("cmd+?", &k, &why) && why.find("shift+/") != std::string::npos);
    CHECK(!parseKey("cmd++", &k, &why));
    CHECK(!parseKey("cmd+banana", &k, &why) && why.find("banana") != std::string::npos);

    CHECK(format(K("cmd+shift+alt+ctrl+k")) == "ctrl+alt+shift+cmd+k");
    CHECK(format(K("shift+f12")) == "shift+f12");
    CHECK(glyphs(K("shift+cmd+p")) == "⇧⌘P");
    CHECK(glyphs(K("ctrl+alt+shift+cmd+k")) == "⌃⌥⇧⌘K");
    CHECK(glyphs(K("ctrl+space")) == "⌃Space");
    CHECK(glyphs(K("shift+f12")) == "⇧F12");
    CHECK(glyphs(K("shift+cmd+enter")) == "⇧⌘⏎");
    CHECK(glyphs(K("ctrl+shift+tab")) == "⌃⇧⇥");
    CHECK(glyphs(K("cmd+numpad0")) == "⌘Keypad 0");
    CHECK(glyphs(K("alt+cmd+down")) == "⌥⌘↓");

    CHECK(isFunctionKey(K("f9")) && isFunctionKey(K("contextmenu")) && !isFunctionKey(K("f")));
    CHECK(typesText(K("k")) && typesText(K("shift+k")) && typesText(K("enter")));
    CHECK(typesText(K("shift+up")) && !typesText(K("alt+up")) && !typesText(K("f9")));
    CHECK(!typesText(K("ctrl+g")) && !typesText(K("shift+f12")));
    CHECK(overlaps(When::Any, When::Editor) && overlaps(When::Terminal, When::Terminal));
    CHECK(!overlaps(When::Editor, When::Terminal) && !overlaps(When::Tree, When::Editor));
}

static void testCommandTable() {
    using namespace Commands;
    GROUP("commands:table");
    std::set<std::string> ids;
    std::set<std::pair<std::string, int>> selectors;
    for (const Command &c : all()) {
        CHECK(ids.insert(c.id).second);
        if (c.selector) {
            bool fresh = selectors.insert({c.selector, c.tag}).second;
            if (!fresh) std::printf("  two commands send %s tag %d\n", c.selector, c.tag);
            CHECK(fresh);
            CHECK(findBySelector(c.selector, c.tag) == &c);
        }
        CHECK(find(c.id) == &c);
        CHECK(c.title && *c.title && c.menu && *c.menu);
        // Fixed commands name who handles them, and have no selector.
        if (c.flags & Fixed) CHECK(c.note && !c.selector);
        // A language scope is the editor's.
        if (c.language) CHECK(c.when == When::Editor && *c.language);
        // Every default key parses, and none (but a fixed one's) types text.
        std::string word, list = c.keys;
        size_t n = 0;
        for (size_t i = 0; i <= list.size(); i++) {
            if (i < list.size() && list[i] != ' ') { word += list[i]; continue; }
            if (word.empty()) continue;
            Key k;
            std::string why;
            bool ok = parseKey(word, &k, &why);
            if (!ok) std::printf("  %s: bad default key %s (%s)\n", c.id, word.c_str(), why.c_str());
            CHECK(ok);
            // Written as format() writes it, so keys.conf's list reads back the same.
            if (ok && format(k) != word)
                std::printf("  %s: write %s as %s\n", c.id, word.c_str(), format(k).c_str());
            CHECK(!ok || format(k) == word);
            // A language command may have one (Shift+Return in Python).
            if (!(c.flags & Fixed) && !c.language) CHECK(!typesText(k));
            n++;
            word.clear();
        }
        CHECK(defaultKeys(c).size() == n);
    }
    CHECK(all().size() > 130);
    CHECK(find("workbench.action.showCommands") &&
          std::string(find("workbench.action.showCommands")->selector) == "showCommandPalette:");
    CHECK(find("nope") == nullptr);
    CHECK(findBySelector("performTextFinderAction:", 1) == find("actions.find"));
    CHECK(findBySelector("selectTabNumber:", 9) == find("workbench.action.lastEditorInGroup"));
    CHECK(findBySelector("selectTabNumber:", 10) == nullptr);

    GROUP("commands:defaults");
    Keymap d = defaults();
    std::vector<std::string> clashes = d.conflicts();
    for (const std::string &s : clashes) std::printf("  default conflict: %s\n", s.c_str());
    CHECK(clashes.empty());
    CHECK(HasKey(d, "workbench.action.showCommands", "shift+cmd+p"));
    CHECK(d.keysOf("workbench.action.terminal.toggleTerminal").size() == 3);
    CHECK(d.keysOf("workbench.action.terminal.toggleTerminal")[0] == K("shift+cmd+t"));
    CHECK(d.keysOf("minicode.about").empty());
    CHECK(d.keysOf("no.such").empty());
    // Cmd+Home is the editor's in the editor and the terminal's there.
    CHECK(d.commandsFor(K("cmd+home")).size() == 2);
    CHECK(d.commandFor(K("cmd+home"), When::Editor) == find("cursorTop"));
    CHECK(d.commandFor(K("cmd+home"), When::Terminal) ==
          find("workbench.action.terminal.scrollToTop"));
    CHECK(d.commandFor(K("cmd+home"), When::Tree) == nullptr);
    CHECK(d.commandFor(K("cmd+k"), When::Terminal) == find("workbench.action.terminal.clear"));
    CHECK(d.commandFor(K("cmd+k"), When::Editor) == nullptr);
    CHECK(d.commandFor(K("shift+cmd+p"), When::Terminal) == find("workbench.action.showCommands"));
    // minicode's Shift+Cmd keys and VS Code's where it has the feature.
    CHECK(HasKey(d, "minicode.requestChange", "shift+cmd+k"));
    CHECK(HasKey(d, "editor.action.deleteLines", "ctrl+shift+k"));
    CHECK(HasKey(d, "editor.action.formatDocument", "alt+shift+f"));
    CHECK(HasKey(d, "editor.action.rename", "f2"));
    // The tree's own keys are rows too (they were hard-coded in FileTree.mm
    // and TerminalGridView.mm, out of keys.conf's sight).
    CHECK(d.commandFor(K("f2"), When::Tree) == find("renameFile"));
    CHECK(d.commandFor(K("f2"), When::Editor) == find("editor.action.rename"));
    CHECK(d.commandFor(K("cmd+backspace"), When::Tree) == find("moveFileToTrash"));
    CHECK(d.commandFor(K("cmd+backspace"), When::Editor) == find("deleteAllLeft"));
    CHECK(d.commandFor(K("cmd+backspace"), When::Terminal) ==
          find("minicode.terminal.deleteToLineStart"));
    CHECK(d.commandFor(K("alt+left"), When::Terminal) == find("minicode.terminal.wordLeft"));
    CHECK(d.commandFor(K("space"), When::Tree) == find("minicode.tree.quickLook"));
    {
        std::vector<Error> errs;
        Keymap m = resolve("f2 = -renameFile\nctrl+alt+cmd+r = renameFile\n", &errs);
        CHECK(errs.empty());
        CHECK(m.commandFor(K("f2"), When::Tree) == nullptr);
        CHECK(m.commandFor(K("f2"), When::Editor) == find("editor.action.rename"));
        CHECK(m.commandFor(K("ctrl+alt+cmd+r"), When::Tree) == find("renameFile"));
        // Moving Rename Symbol leaves the tree's F2 alone.
        m = resolve("f2 = -editor.action.rename\n", &errs);
        CHECK(errs.empty() && m.commandFor(K("f2"), When::Tree) == find("renameFile"));
        // The grid's line edits and Quick Look's Space are handled by their
        // views, so keys.conf cannot give their keys to another command there.
        m = resolve("alt+left = workbench.action.terminal.clear\n", &errs);
        CHECK(errs.size() == 1);
    }
}

static void testCommandLanguages() {
    using namespace Commands;
    GROUP("commands:language");
    CHECK(languageOfPath("/a/README.md") == "markdown");
    CHECK(languageOfPath("notes.MarkDown") == "markdown");
    CHECK(languageOfPath("/a/b/run.py") == "python" && languageOfPath("x.pyi") == "python");
    CHECK(languageOfPath("/a/.py").empty() && languageOfPath("/a/main.cpp").empty());
    CHECK(languageOfPath("").empty() && languageOfPath("Makefile").empty());
    CHECK(placeName(*find("python.execSelectionInTerminal")) == "the editor, in Python files");
    CHECK(placeName(*find("workbench.action.terminal.clear")) == "the terminal");

    Keymap d = defaults();
    const Command *task = find("minicode.markdown.toggleTask");
    CHECK(d.commandFor(K("alt+c"), When::Editor, "markdown") == task);
    CHECK(d.commandFor(K("alt+c"), When::Editor) == nullptr);   // types its character
    CHECK(d.commandFor(K("alt+c"), When::Editor, "python") == nullptr);
    CHECK(d.commandFor(K("alt+c"), When::Terminal, "markdown") == nullptr);
    CHECK(d.commandFor(K("alt+cmd+l"), When::Editor, "markdown") == task);
    CHECK(d.commandFor(K("shift+enter"), When::Editor, "python") ==
          find("python.execSelectionInTerminal"));
    CHECK(d.commandFor(K("shift+enter"), When::Editor, "markdown") == nullptr);
    // Cmd+B is the sidebar everywhere, Markdown included; Cmd+I the agent.
    CHECK(d.commandFor(K("cmd+b"), When::Editor, "markdown") ==
          find("workbench.action.toggleSidebarVisibility"));
    CHECK(d.commandFor(K("cmd+i"), When::Editor, "markdown") == find("inlineChat.start"));
    CHECK(d.keysOf("markdown.extension.editing.toggleBold").empty());
    CHECK(d.keysOf("markdown.extension.editing.toggleItalic").empty());
    // The new keys of the daily-work commands.
    CHECK(HasKey(d, "workbench.action.showCommands", "f1"));
    CHECK(HasKey(d, "editor.action.indentLines", "cmd+]"));
    CHECK(HasKey(d, "editor.action.outdentLines", "cmd+["));
    CHECK(HasKey(d, "editor.action.insertLineAfter", "cmd+enter"));
    CHECK(HasKey(d, "workbench.action.closeOtherEditors", "alt+cmd+t"));
    CHECK(HasKey(d, "workbench.action.nextEditor", "alt+cmd+right"));
    CHECK(HasKey(d, "workbench.action.previousEditor", "alt+cmd+left"));
    CHECK(HasKey(d, "workbench.action.files.saveAll", "alt+cmd+s"));
    CHECK(HasKey(d, "workbench.action.openRecent", "ctrl+r"));
    CHECK(d.keysOf("workbench.action.files.saveAs").empty());   // Shift+Cmd+S is Export PDF

    // keys.conf: a key given to a language command shadows the other
    // command there, and stays the other's elsewhere.
    std::vector<Error> errs;
    Keymap m = resolve("cmd+b = markdown.extension.editing.toggleBold\n", &errs);
    CHECK(errs.empty());
    CHECK(m.commandFor(K("cmd+b"), When::Editor, "markdown") ==
          find("markdown.extension.editing.toggleBold"));
    CHECK(m.commandFor(K("cmd+b"), When::Editor, "python") ==
          find("workbench.action.toggleSidebarVisibility"));
    CHECK(m.commandFor(K("cmd+b"), When::Tree) == find("workbench.action.toggleSidebarVisibility"));
    CHECK(m.conflicts().empty());
    // Given to a command for every file afterwards, the key leaves it.
    m = resolve("cmd+b = markdown.extension.editing.toggleBold\n"
                "cmd+b = workbench.action.toggleSidebarVisibility\n", &errs);
    CHECK(errs.empty());
    CHECK(m.commandFor(K("cmd+b"), When::Editor, "markdown") ==
          find("workbench.action.toggleSidebarVisibility"));
    // Two commands of one language on one key are refused.
    errs.clear();
    m = resolve("ctrl+alt+b = markdown.extension.editing.toggleBold\n"
                "ctrl+alt+b = markdown.extension.editing.toggleItalic\n", &errs);
    CHECK(errs.size() == 1 && errs[0].line == 2);
    if (!errs.empty())
        CHECK(errs[0].message.find("in the editor, in Markdown files") != std::string::npos);
    // A typing key only for a language command.
    errs.clear();
    resolve("shift+enter = editor.action.formatDocument\n", &errs);
    CHECK(errs.size() == 1);
    errs.clear();
    m = resolve("alt+c = -minicode.markdown.toggleTask\n", &errs);
    CHECK(errs.empty() && m.commandFor(K("alt+c"), When::Editor, "markdown") == nullptr);
    CHECK(defaultFileText().find("in the editor, in Python files") != std::string::npos);
}

static void testKeymapFile() {
    using namespace Commands;
    GROUP("commands:keymap");
    std::vector<Error> errs;
    // A key added: the command keeps its default and shows the new one first.
    Keymap m = resolve("ctrl+alt+cmd+k = editor.action.deleteLines\n", &errs);
    CHECK(errs.empty());
    CHECK(m.keysOf("editor.action.deleteLines").size() == 2);
    CHECK(m.keysOf("editor.action.deleteLines")[0] == K("ctrl+alt+cmd+k"));
    CHECK(HasKey(m, "editor.action.deleteLines", "ctrl+shift+k"));

    // The task's example: Request a Change gives up Shift+Cmd+K.
    errs.clear();
    m = resolve("# mine\n"
                "cmd+shift+k = editor.action.deleteLines   # VS Code's\n"
                "ctrl+shift+k = -\n",
                &errs);
    CHECK(errs.empty());
    CHECK(m.keysOf("editor.action.deleteLines").size() == 1);
    CHECK(m.keysOf("editor.action.deleteLines")[0] == K("shift+cmd+k"));
    CHECK(m.keysOf("minicode.requestChange").empty());
    CHECK(m.commandsFor(K("ctrl+shift+k")).empty());
    CHECK(m.conflicts().empty());

    // A minus with a command takes the key from that command only.
    errs.clear();
    m = resolve("cmd+j = -workbench.action.terminal.toggleTerminal\n", &errs);
    CHECK(errs.empty());
    CHECK(m.keysOf("workbench.action.terminal.toggleTerminal").size() == 2);
    CHECK(!HasKey(m, "workbench.action.terminal.toggleTerminal", "cmd+j"));
    errs.clear();
    m = resolve("cmd+j = -workbench.action.quickOpen\n", &errs);
    CHECK(errs.size() == 1 && errs[0].line == 1 &&
          errs[0].message.find("not a key of") != std::string::npos);

    // A key of the terminal's is kept beside an editor-only command.
    errs.clear();
    m = resolve("cmd+k = editor.action.deleteLines\n", &errs);
    CHECK(errs.empty());
    CHECK(m.commandsFor(K("cmd+k")).size() == 2);
    CHECK(m.commandFor(K("cmd+k"), When::Editor) == find("editor.action.deleteLines"));
    CHECK(m.commandFor(K("cmd+k"), When::Terminal) == find("workbench.action.terminal.clear"));
    CHECK(m.conflicts().empty());
    // ... but one that applies anywhere takes it.
    m = resolve("cmd+k = minicode.toggleBrowser\n", &errs);
    CHECK(m.keysOf("workbench.action.terminal.clear").empty());
    CHECK(m.commandFor(K("cmd+k"), When::Terminal) == find("minicode.toggleBrowser"));

    // Two lines, one key, two commands in one place: the later is refused.
    errs.clear();
    m = resolve("ctrl+alt+cmd+j = minicode.toggleBrowser\n"
                "\n"
                "ctrl+alt+cmd+j = minicode.toggleAgents\n",
                &errs);
    CHECK(errs.size() == 1 && errs[0].line == 3);
    CHECK(!errs.empty() && errs[0].message.find("from line 1") != std::string::npos);
    CHECK(HasKey(m, "minicode.toggleBrowser", "ctrl+alt+cmd+j"));
    CHECK(!HasKey(m, "minicode.toggleAgents", "ctrl+alt+cmd+j"));
    // ... the same line twice is no conflict, and neither is a terminal one.
    errs.clear();
    m = resolve("ctrl+alt+cmd+j = minicode.toggleBrowser\n"
                "ctrl+alt+cmd+j = minicode.toggleBrowser\n"
                "ctrl+alt+cmd+u = editor.action.commentLine\n"
                "ctrl+alt+cmd+u = workbench.action.terminal.clear\n",
                &errs);
    CHECK(errs.empty());
    CHECK(m.keysOf("minicode.toggleBrowser")[0] == K("ctrl+alt+cmd+j"));
    CHECK(m.keysOf("minicode.toggleBrowser").size() == 2);

    // Mistakes, each on its own line.
    errs.clear();
    m = resolve("cmd+k\n"                                     // 1: no "="
                "cmd+k =\n"                                   // 2: no command
                "cmd+banana = minicode.toggleBrowser\n"       // 3: no such key
                "cmd+k = minicode.nothing\n"                  // 4: no such command
                "k = minicode.toggleBrowser\n"                // 5: types a letter
                "shift+up = minicode.toggleBrowser\n"         // 6: moves the caret
                "alt+down = minicode.toggleBrowser\n"         // 7: the editor's own
                "cmd+up = -\n"                                // 8: likewise
                "f5 = editor.action.moveLinesDownAction\n"    // 9: cannot be remapped
                "f5 = minicode.toggleBrowser\n",              // fine
                &errs);
    std::vector<int> lines;
    for (const Error &e : errs) lines.push_back(e.line);
    CHECK((lines == std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9}));
    CHECK(errs.size() == 9 && errs[3].message.find("minicode.nothing") != std::string::npos);
    CHECK(errs.size() == 9 && errs[6].message.find("Move Line Down") != std::string::npos);
    CHECK(HasKey(m, "minicode.toggleBrowser", "f5"));
    CHECK(!HasKey(m, "minicode.toggleBrowser", "alt+down"));
    CHECK(HasKey(m, "editor.action.moveLinesUpAction", "cmd+up"));
    CHECK(HasKey(m, "editor.action.moveLinesDownAction", "alt+down"));
    // A key the editor keeps for itself can still go to a terminal command.
    errs.clear();
    m = resolve("cmd+up = workbench.action.terminal.clear\n", &errs);
    CHECK(errs.empty());
    CHECK(m.commandFor(K("cmd+up"), When::Terminal) == find("workbench.action.terminal.clear"));

    // "=" as the key, CRLF and a key with spaces round it.
    errs.clear();
    m = resolve("ctrl+cmd+= = editor.action.fontZoomIn\r\n  ctrl + cmd + - = editor.action.fontZoomOut\r\n",
                &errs);
    CHECK(errs.empty());
    CHECK(HasKey(m, "editor.action.fontZoomIn", "ctrl+cmd+="));
    CHECK(HasKey(m, "editor.action.fontZoomOut", "ctrl+cmd+-"));

    // Unbinding then binding again, in order.
    errs.clear();
    m = resolve("f9 = -\nf9 = editor.action.commentLine\n", &errs);
    CHECK(errs.empty());
    CHECK(m.keysOf("minicode.sortLines").empty());
    CHECK(m.commandFor(K("f9"), When::Editor) == find("editor.action.commentLine"));

    GROUP("commands:file-text");
    // Keyboard Shortcuts' file: as it is, nothing changes; uncommented, every
    // line reads back as the defaults with no error.
    const std::string text = defaultFileText();
    errs.clear();
    Keymap same = resolve(text, &errs);
    CHECK(errs.empty());
    CHECK(same.keys == defaults().keys);
    std::string uncommented, line;
    std::istringstream in(text);
    size_t bindings = 0;
    bool listing = false;   // past the explanation at the top
    while (std::getline(in, line)) {
        if (line.rfind("# ---- ", 0) == 0) listing = true;
        if (listing && line.rfind("# ", 0) == 0 && line.find(" = ") != std::string::npos &&
            line.find("(no key)") == std::string::npos) {
            uncommented += line.substr(2) + "\n";
            bindings++;
        }
    }
    errs.clear();
    Keymap back = resolve(uncommented, &errs);
    for (const Error &e : errs) std::printf("  file-text line %d: %s\n", e.line, e.message.c_str());
    CHECK(errs.empty());
    CHECK(bindings > 120);
    // Each command keeps exactly its default keys (order aside).
    bool same2 = true;
    for (const Command &c : all()) {
        std::vector<Key> a = back.keysOf(c.id), b = defaults().keysOf(c.id);
        auto byName = [](const Key &x, const Key &y) { return format(x) < format(y); };
        std::sort(a.begin(), a.end(), byName);
        std::sort(b.begin(), b.end(), byName);
        if (!(a == b)) { same2 = false; std::printf("  file-text changes %s\n", c.id); }
    }
    CHECK(same2);
    CHECK(text.find("workbench.action.showCommands") != std::string::npos);
    CHECK(text.find("# (no key) = minicode.about") != std::string::npos);
    CHECK(text.find("Move Line Down") != std::string::npos);
    for (const Command &c : all()) CHECK(text.find(c.id) != std::string::npos || (c.flags & Fixed));
}

// ------------------------------------------------- the README's table
// The keys a README cell names, spelled as the README spells them: "Shift
// Command P", "Control 1 to 9", "Option Command [", "Command equals (or
// Command plus)".
static std::vector<Commands::Key> ReadmeKeys(const std::string &cell) {
    using namespace Commands;
    std::vector<std::string> words;
    std::string w;
    for (char ch : cell + " ") {
        if (ch == ' ') {
            while (w.size() > 1 && (w.back() == ',' || w.back() == ';' || w.back() == ')'))
                w.pop_back();
            while (w.size() > 1 && w.front() == '(') w.erase(w.begin());
            if (!w.empty()) words.push_back(w);
            w.clear();
        } else {
            w += ch;
        }
    }
    static const std::map<std::string, std::string> named = {
        {"Space", "space"}, {"Return", "enter"}, {"Tab", "tab"}, {"Escape", "escape"},
        {"Delete", "backspace"}, {"Up", "up"}, {"Down", "down"}, {"Left", "left"},
        {"Right", "right"}, {"Home", "home"}, {"End", "end"}, {"backslash", "\\"},
        {"backtick", "`"}, {"comma", ","}, {"period", "."}, {"minus", "-"}, {"equals", "="},
        {"Menu", "contextmenu"},
    };
    std::vector<Key> out;
    unsigned mods = 0;
    for (size_t i = 0; i < words.size(); i++) {
        const std::string &t = words[i];
        if (t == "Shift") { mods |= Shift; continue; }
        if (t == "Command") { mods |= Cmd; continue; }
        if (t == "Option") { mods |= Alt; continue; }
        if (t == "Control") { mods |= Ctrl; continue; }
        if (t == "with" || t == "fn") continue;
        std::string name;
        unsigned extra = 0;
        if (named.count(t)) name = named.at(t);
        else if (t == "plus") { name = "="; extra = Shift; }
        else if (t == "keypad" && i + 1 < words.size()) name = "numpad" + words[++i];
        else if (t == "Forward" && i + 1 < words.size() && words[i + 1] == "Delete") { name = "delete"; i++; }
        else if (t == "Page" && i + 1 < words.size()) name = "page" + std::string(words[++i] == "Up" ? "up" : "down");
        else if (t.size() >= 2 && t[0] == 'F' && std::isdigit((unsigned char)t[1])) name = "f" + t.substr(1);
        else if (t.size() == 1 && !std::isspace((unsigned char)t[0])) name = std::string(1, (char)std::tolower((unsigned char)t[0]));
        if (name.empty()) { mods = 0; continue; }
        Key k;
        k.mods = mods | extra;
        k.name = name;
        out.push_back(k);
        // "Control 1 to 9"
        if (name.size() == 1 && std::isdigit((unsigned char)name[0]) && i + 2 < words.size() &&
            words[i + 1] == "to" && words[i + 2].size() == 1 && std::isdigit((unsigned char)words[i + 2][0])) {
            for (char d = (char)(name[0] + 1); d <= words[i + 2][0]; d++) {
                Key r = k;
                r.name = std::string(1, d);
                out.push_back(r);
            }
            i += 2;
        }
        mods = 0;
    }
    return out;
}

static void testCommandsReadme() {
    using namespace Commands;
    GROUP("commands:readme");
    std::vector<Key> spelled = ReadmeKeys("Shift Command 1 to 9, or Control Command [ (Option Command ] too); "
                                          "Command equals (or Command plus), Command with keypad 0, the Menu key");
    std::vector<std::string> got;
    for (const Key &k : spelled) got.push_back(format(k));
    CHECK(got.size() == 15);
    CHECK(got.size() > 9 && got[0] == "shift+cmd+1" && got[8] == "shift+cmd+9" && got[9] == "ctrl+cmd+[");
    CHECK(got.size() > 14 && got[10] == "alt+cmd+]" && got[11] == "cmd+=" && got[12] == "shift+cmd+=");
    CHECK(got.size() > 14 && got[13] == "cmd+numpad0" && got[14] == "contextmenu");

    std::ifstream f("README.md");
    if (!f) {
        std::printf("  (README.md not found here; README checks skipped)\n");
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string readme = ss.str();
    size_t start = readme.find("| Action | macOS |");
    CHECK(start != std::string::npos);
    if (start == std::string::npos) return;
    size_t end = readme.find("\n\n", start);
    // Every key in the macOS column of the shortcut table.
    std::set<std::string> listed;
    std::istringstream rows(readme.substr(start, end - start));
    std::string row;
    while (std::getline(rows, row)) {
        size_t a = row.find('|', 1);
        size_t b = a == std::string::npos ? a : row.find('|', a + 1);
        if (b == std::string::npos) continue;
        for (const Key &k : ReadmeKeys(row.substr(a + 1, b - a - 1))) listed.insert(format(k));
    }
    // Every command's default keys are in it.
    for (const Command &c : all())
        for (const Key &k : defaultKeys(c)) {
            bool found = listed.count(format(k)) > 0;
            if (!found) std::printf("  README's table lacks %s (%s)\n", format(k).c_str(), c.id);
            CHECK(found);
        }
}
