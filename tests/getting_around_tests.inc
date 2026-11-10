// getting_around_tests.inc — linux/PARITY.md item 1.11, "Getting around":
// the editor's and the terminals' zoom and the word wrap toggle
// (ViewZoom.h, `zoom:*`), what a window records for Go Back and Go Forward
// (NavRecorder.h over NavHistory.h, `navrecorder:*`), Maximize Pane in the
// right side's layout (RightAreaLayout.h, `rightarea:maximize`), whether
// the settings file sets the font sizes (`settings:font-size-set`), and the
// Linux keys of all of these (`commands:linux-getting-around`). Included by
// run_tests.cpp after commands_linux_tests.inc, whose K it uses.
#include "NavRecorder.h"
#include "ViewZoom.h"

namespace {

// A window for the recorder: files that exist, the one on show, and where
// its caret is. Lines are ten units long, so a place's line is its
// location / 10 (linesOf says the same).
struct NavWin {
    std::set<std::string> files;
    std::string path;
    size_t location = 0, textLength = 10000;
    bool hasText = true;
    int shows = 0, bells = 0;
    Nav::Recorder* rec = nullptr;
    // Where the caret goes: its line, at the line's start.
    void at(const std::string& p, int line) {
        path = p;
        location = (size_t)line * 10;
    }
    void caret(int line) {
        location = (size_t)line * 10;
        if (rec) rec->caretMoved();
    }
};

Nav::Recorder::Host navHost(NavWin* w) {
    Nav::Recorder::Host h;
    h.current = [w](Nav::Place* p, size_t* len) {
        if (w->path.empty()) return false;
        p->path = w->path;
        p->hasText = w->hasText;
        p->location = w->hasText ? w->location : 0;
        p->length = 0;
        p->line = w->hasText ? (int)(w->location / 10) : 0;
        *len = w->hasText ? w->textLength : 0;
        return true;
    };
    h.show = [w](const Nav::Place& p) {
        if (!w->files.count(p.path)) return false;
        w->shows++;
        w->path = p.path;
        w->hasText = p.hasText;
        if (p.hasText) w->location = p.location;
        // Showing moves the caret, as a real view tells its recorder.
        if (w->rec) w->rec->caretMoved();
        return true;
    };
    h.exists = [w](const std::string& p) { return w->files.count(p) > 0; };
    h.linesOf = [](const std::vector<size_t>& offsets) {
        std::vector<int> out;
        for (size_t o : offsets) out.push_back((int)(o / 10));
        return out;
    };
    h.bell = [w] { w->bells++; };
    return h;
}

std::string navEntries(Nav::Recorder& r) {
    std::string s;
    const std::vector<std::string> e = r.entries();
    for (size_t i = 0; i < e.size(); i++) {
        if (!s.empty()) s += " ";
        s += (i == r.index() ? ">" : "") + e[i];
    }
    return s;
}

}  // namespace

static void testGettingAround() {
    GROUP("zoom:steps");
    {
        using namespace ViewZoom;
        CHECK(size(13, 0) == 13 && size(13, 2) == 15 && size(13, 100) == 72 && size(13, -100) == 6);
        double z = 99;
        CHECK(step(13, 0, 1, &z) && z == 1);
        CHECK(step(13, 1, 1, &z) && z == 2);
        CHECK(step(13, 2, -3, &z) && z == -1);
        // A reset is true even at 0; a step at a limit changes nothing.
        z = 99;
        CHECK(step(13, 5, 0, &z) && z == 0);
        z = 99;
        CHECK(step(13, 0, 0, &z) && z == 0);
        z = 99;
        CHECK(!step(13, 59, 1, &z) && z == 99);
        CHECK(!step(13, -7, -1, &z) && z == 99);
        CHECK(step(13, -6, -1, &z) && z == -7);   // 6 points in all
        // A larger base (editor.font-size changed) brings the zoom within
        // the limits on the next step.
        CHECK(step(70, 5, 1, &z) && z == 2);
        CHECK(step(70, 0, 5, &z) && z == 2);
        // The preview's factor: (13 + zoom) / 13, never under a half.
        CHECK(previewScale(0) == 1 && previewScale(13) == 2 && previewScale(-10) == 0.5);
        CHECK(zoomed(15, 0) == 15 && zoomed(15, 2) == 17.5 && zoomed(26, 13) == 52);
        // Safari's steps for a web page.
        CHECK(pageStep(1, 1) == 1.15 && pageStep(1, -1) == 0.85 && pageStep(1.15, 1) == 1.25);
        CHECK(pageStep(3, 1) == 3 && pageStep(0.5, -1) == 0.5 && pageStep(4, 1) == 4);
        CHECK(pageStep(0.3, -1) == 0.3 && pageStep(0.3, 1) == 0.5);
        CHECK(pageStep(1.1, 1) == 1.15 && pageStep(1.1, -1) == 1.0 && pageStep(2.2, 0) == 1);
    }

    GROUP("zoom:state");
    {
        using namespace ViewZoom;
        State s;
        s.editor = 3;
        s.terminal = -2;
        CHECK(decode(encode(s)) == s);
        CHECK(encode(State()) == "{\"editor\":0,\"terminal\":0}");
        CHECK(decode("") == State() && decode("nonsense") == State() && decode("[1,2]") == State());
        State half = decode("{\"terminal\":4}");
        CHECK(half.editor == 0 && half.terminal == 4);
        CHECK(decode("{\"editor\":\"x\",\"terminal\":true}") == State());
        CHECK(decode("{\"editor\":1000}").editor == 66 && decode("{\"editor\":-1000}").editor == -66);
        CHECK(decode("{\"editor\":2.4}").editor == 2);
    }

    GROUP("zoom:wrap");
    {
        ViewZoom::Wrap w;
        CHECK(w.on(true) && !w.on(false) && !w.overridden());
        w.toggle(true);   // off for the session
        CHECK(!w.on(true) && !w.on(true) && w.overridden());
        w.toggle(true);   // and on again
        CHECK(w.on(true));
        // The setting changed since the toggle: it wins, and the toggle goes.
        w.toggle(true);
        CHECK(!w.on(true));
        CHECK(!w.on(false) && !w.overridden());
        CHECK(w.on(true));
        // Turned on over a setting of off; the setting turned on since keeps it on.
        ViewZoom::Wrap v;
        v.toggle(false);
        CHECK(v.on(false) && v.overridden());
        CHECK(v.on(true) && !v.overridden());
        CHECK(!v.on(false));
    }

    GROUP("navrecorder:far-moves");
    {
        NavWin w;
        w.files = {"a", "b", "c"};
        w.at("a", 5);
        Nav::Recorder r(navHost(&w));
        w.rec = &r;
        r.noteCaret();
        w.caret(9);   // near: nothing
        CHECK(r.entries().empty());
        w.caret(60);   // more than ten lines, the text as it was
        CHECK(navEntries(r) == "a:9 >a:60");
        CHECK(r.goBack() && w.location == 90 && w.shows == 1);
        CHECK(navEntries(r) == ">a:9 a:60");
        CHECK(r.goForward() && w.location == 600);
        CHECK(!r.canGoForward() && !r.goForward() && w.bells == 1);
        // Typing that moves the caret far is no jump: the text changed.
        r.clear();
        r.noteCaret();
        w.textLength += 300;
        w.caret(90);
        CHECK(r.entries().empty());
        // Ten lines exactly is near.
        w.caret(100);
        CHECK(r.entries().empty());
        w.caret(111);
        CHECK(navEntries(r) == "a:100 >a:111");
    }

    GROUP("navrecorder:jumps");
    {
        NavWin w;
        w.files = {"a", "b", "c", "d"};
        w.at("a", 0);
        Nav::Recorder r(navHost(&w));
        w.rec = &r;
        r.noteCaret();
        // A jump to another file, with a tab switch and caret moves inside
        // it: one jump, from where it began to where it ended.
        r.jump([&] {
            r.tabWillSwitch();
            w.at("b", 0);
            r.tabDidSwitch();
            w.caret(40);
        });
        CHECK(navEntries(r) == "a:0 >b:40");
        // A tab switch on its own is a jump.
        r.tabWillSwitch();
        w.at("c", 7);
        r.tabDidSwitch();
        CHECK(navEntries(r) == "a:0 b:40 >c:7");
        // Nested jumps are recorded once.
        r.jump([&] { r.jump([&] { w.caret(300); }); });
        CHECK(navEntries(r) == "a:0 b:40 c:7 >c:300");
        // A jump that went nowhere records nothing.
        r.jump([] {});
        CHECK(r.entries().size() == 4);
        // Back twice, then a new jump drops what lay ahead.
        CHECK(r.goBack() && w.path == "c" && w.location == 70);
        CHECK(r.goBack() && w.path == "b" && w.location == 400);
        CHECK(navEntries(r) == "a:0 >b:40 c:7 c:300");
        r.jump([&] { w.at("d", 0); });
        CHECK(navEntries(r) == "a:0 b:40 >d:0" && !r.canGoForward());
        // Going back and forth records nothing of its own, though showing
        // a place moves the caret (the host calls caretMoved then).
        const size_t n = r.entries().size();
        CHECK(r.goBack() && r.goForward());
        CHECK(r.entries().size() == n);
        // A jump from a place given: the palette previewed a symbol, so the
        // caret is already there when the jump runs.
        Nav::Place from;
        CHECK(r.current(&from));
        w.caret(250);   // a preview, while quiet
        r.clear();
        r.beginQuiet();
        w.caret(260);
        w.caret(500);
        CHECK(r.entries().empty());
        r.endQuiet();
        w.caret(500);   // no move since the quiet ended: nothing
        CHECK(r.entries().empty());
        r.jumpFrom(from, [] {});
        CHECK(navEntries(r) == "d:0 >d:500");
    }

    GROUP("navrecorder:files");
    {
        NavWin w;
        w.files = {"/p/a.txt", "/p/sub/b.txt", "/p/c.txt"};
        w.at("/p/a.txt", 0);
        Nav::Recorder r(navHost(&w));
        w.rec = &r;
        r.noteCaret();
        r.tabWillSwitch();
        w.at("/p/sub/b.txt", 3);
        r.tabDidSwitch();
        r.tabWillSwitch();
        w.at("/p/c.txt", 50);
        r.tabDidSwitch();
        CHECK(navEntries(r) == "/p/a.txt:0 /p/sub/b.txt:3 >/p/c.txt:50");
        // A folder renamed: its files' places follow it.
        r.moved("/p/sub", "/p/lib");
        w.files.erase("/p/sub/b.txt");
        w.files.insert("/p/lib/b.txt");
        CHECK(navEntries(r) == "/p/a.txt:0 /p/lib/b.txt:3 >/p/c.txt:50");
        CHECK(r.goBack() && w.path == "/p/lib/b.txt");
        // The file on show renamed: a far move afterwards is still a move in it.
        r.moved("/p/lib/b.txt", "/p/lib/bb.txt");
        w.files.insert("/p/lib/bb.txt");
        w.path = "/p/lib/bb.txt";
        w.caret(80);   // a jump of its own, which drops what lay ahead
        CHECK(navEntries(r) == "/p/a.txt:0 /p/lib/bb.txt:3 >/p/lib/bb.txt:80");
        // Deleted from the disk: passed over and dropped when going there.
        w.files.erase("/p/a.txt");
        CHECK(r.goBack() && w.location == 30);
        CHECK(!r.goBack() && w.bells == 1);
        CHECK(navEntries(r) == ">/p/lib/bb.txt:3 /p/lib/bb.txt:80");
        // Trashed in the app: dropped at once, a folder with what is in it.
        r.removed("/p/lib");
        CHECK(r.entries().empty());
        // A file that cannot be shown is passed over to the one before it.
        NavWin v;
        v.files = {"x", "y", "z"};
        v.at("x", 0);
        Nav::Recorder q(navHost(&v));
        v.rec = &q;
        q.noteCaret();
        q.jump([&] { v.at("y", 0); });
        q.jump([&] { v.at("z", 0); });
        v.files.erase("y");
        v.files.insert("y-not-shown");
        CHECK(q.goBack() && v.path == "x" && v.shows == 1);
        // Nothing on show (every tab closed): Back goes to the last place.
        v.path.clear();
        CHECK(q.goBack() && v.path == "x");
        v.path.clear();
        CHECK(!q.goForward());
    }

    GROUP("navrecorder:edits");
    {
        NavWin w;
        w.files = {"a"};
        w.at("a", 100);
        Nav::Recorder r(navHost(&w));
        w.rec = &r;
        r.noteCaret();
        w.caret(10);
        CHECK(navEntries(r) == "a:100 >a:10");
        // Twenty lines typed above the first place (and below the caret's):
        // it moves with its text, and its line is counted again when asked.
        r.edited("a", 500, 0, 200);
        w.textLength += 200;
        CHECK(navEntries(r) == "a:120 >a:10");
        CHECK(r.goBack() && w.location == 1200);
        // An edit over a place takes it to where the new text is.
        r.edited("a", 1150, 100, 20);
        CHECK(r.history().entries()[0].location == 1170);
        // A picture's place has no line; near anything in its file.
        NavWin p;
        p.files = {"pic.png", "t.txt"};
        p.at("t.txt", 0);
        Nav::Recorder q(navHost(&p));
        p.rec = &q;
        q.noteCaret();
        q.tabWillSwitch();
        p.at("pic.png", 0);
        p.hasText = false;
        q.tabDidSwitch();
        CHECK(navEntries(q) == "t.txt:0 >pic.png:0");
        CHECK(q.goBack() && p.path == "t.txt" && p.hasText);
    }

    GROUP("rightarea:maximize");
    {
        using namespace RightArea;
        Limits gtk;
        gtk.minTop = 70;
        gtk.bar = 4;
        State s;
        s.browserVisible = true;
        s.terminalVisible = true;
        const Frames before = layout(s, 804, 604, gtk);
        // From the browser: it alone, all of the right side, no bars.
        CHECK(toggleMaximized(s, Pane::Browser));
        Pane m = Pane::Editor;
        CHECK(maximizedNow(s, &m) && m == Pane::Browser);
        Frames f = layout(s, 804, 604, gtk);
        CHECK(!f.editor && f.browser && !f.terminal && !f.browserBar && !f.terminalBar);
        CHECK(raSame(f.browserRect, 0, 0, 804, 604));
        CHECK(onScreen(s, Pane::Browser) && !onScreen(s, Pane::Editor) &&
              !onScreen(s, Pane::Terminal));
        // The others are still what the user asked for.
        CHECK(shows(s, Pane::Editor) && shows(s, Pane::Terminal));
        // Again: everything back where it was.
        CHECK(toggleMaximized(s, Pane::Browser) && !maximizedNow(s));
        f = layout(s, 804, 604, gtk);
        CHECK(raSame(f.editorRect, before.editorRect.x, before.editorRect.y, before.editorRect.w,
                     before.editorRect.h) &&
              raSame(f.terminalRect, before.terminalRect.x, before.terminalRect.y,
                     before.terminalRect.w, before.terminalRect.h) &&
              f.browserBar && f.terminalBar);
        // The terminal: the whole height, no bar to drag down.
        CHECK(toggleMaximized(s, Pane::Terminal));
        f = layout(s, 804, 604, gtk);
        CHECK(f.terminal && !f.editor && !f.browser && !f.terminalBar &&
              raSame(f.terminalRect, 0, 0, 804, 604));
        // Bars cannot be dragged meanwhile.
        const State held = s;
        CHECK(dragTerminalBar(s, 2, 604, gtk) == Hid::Nothing && !s.editorHidden &&
              s.terminalHeight == held.terminalHeight);
        CHECK(dragBrowserBar(s, 2, 804, gtk) == Hid::Nothing && !s.editorHidden);
        // Showing or focusing another pane ends it; the pane itself does not.
        CHECK(!leaveMaximizedFor(s, Pane::Terminal) && maximizedNow(s));
        CHECK(leaveMaximizedFor(s, Pane::Editor) && !maximizedNow(s) && !s.maximized);
        CHECK(!leaveMaximizedFor(s, Pane::Editor));
        // From the editor, or the tree (no pane has the keyboard): the editor.
        CHECK(toggleMaximized(s, Pane::Editor) && maximizedNow(s, &m) && m == Pane::Editor);
        f = layout(s, 804, 604, gtk);
        CHECK(f.editor && raSame(f.editorRect, 0, 0, 804, 604));
        toggleMaximized(s, std::nullopt);
        CHECK(toggleMaximized(s, std::nullopt) && maximizedNow(s, &m) && m == Pane::Editor);
        // Hiding the maximized pane ends it.
        toggle(s, Pane::Editor);
        CHECK(!s.maximized && !maximizedNow(s));
        f = layout(s, 804, 604, gtk);
        CHECK(f.browser && f.terminal && !f.editor);
        // The editor hidden: the browser, then the terminal.
        CHECK(toggleMaximized(s, std::nullopt) && maximizedNow(s, &m) && m == Pane::Browser);
        toggleMaximized(s, std::nullopt);
        s.browserVisible = false;
        CHECK(toggleMaximized(s, std::nullopt) && maximizedNow(s, &m) && m == Pane::Terminal);
        // A maximized pane hidden some other way is no longer maximized.
        s.terminalVisible = false;
        CHECK(!maximizedNow(s) && !onScreen(s, Pane::Terminal));
        // Nothing shows: nothing to maximize.
        CHECK(!toggleMaximized(s, std::nullopt) && !s.maximized);
        // A build without a terminal: the keyboard there counts for nothing.
        State t;
        t.terminalVisible = true;
        Parts noTerm;
        noTerm.terminal = false;
        CHECK(toggleMaximized(t, Pane::Terminal, noTerm) && maximizedNow(t, &m, noTerm) &&
              m == Pane::Editor);
        // Its smallest size is the maximized pane's own.
        State u;
        u.browserVisible = true;
        u.terminalVisible = true;
        Minimums mins;
        mins.editorWidth = 300;
        mins.editorHeight = 200;
        mins.browserWidth = 250;
        mins.terminalHeight = 90;
        double mw = 0, mh = 0;
        toggleMaximized(u, Pane::Editor);
        minimumSize(u, gtk, mins, &mw, &mh);
        CHECK(mw == 300 && mh == 200);
    }

    GROUP("settings:font-size-set");
    {
        std::vector<SettingsError> errs;
        Settings s = Settings::parse("", &errs);
        CHECK(!s.editorFontSizeSet() && !s.terminalFontSizeSet() && s.editorFontSize() == 13);
        s = Settings::parse("editor.font-size = 15\n", &errs);
        CHECK(s.editorFontSizeSet() && s.editorFontSize() == 15 && !s.terminalFontSizeSet());
        s = Settings::parse("terminal.font-size = 14\n", &errs);
        CHECK(s.terminalFontSizeSet() && s.terminalFontSize() == 14 && !s.editorFontSizeSet());
        // Commented out, as the default file has it: not set.
        s = Settings::parse("# editor.font-size = 13\n# terminal.font-size = 12\n", &errs);
        CHECK(!s.editorFontSizeSet() && !s.terminalFontSizeSet());
        errs.clear();
        s = Settings::parse("editor.font-size = huge\n", &errs);
        CHECK(!s.editorFontSizeSet() && errs.size() == 1);
        // Set to the default's own value counts as set.
        s = Settings::parse("editor.font-size = 13\n", &errs);
        CHECK(s.editorFontSizeSet());
    }

    GROUP("commands:linux-getting-around");
    {
        using namespace Commands;
        const Keymap d = defaults(Platform::Linux);
        auto is = [](const Command* c, const char* id) { return c && std::string(c->id) == id; };
        // Ctrl+= and Ctrl+-: the editor's zoom, the page's in a page, the
        // terminals' in the terminal, no clash between them.
        CHECK(is(d.commandFor(K("ctrl+="), When::Editor), "editor.action.fontZoomIn"));
        CHECK(is(d.commandFor(K("ctrl+="), When::Tree), "editor.action.fontZoomIn"));
        CHECK(is(d.commandFor(K("ctrl+="), When::Page), "minicode.page.zoomIn"));
        CHECK(is(d.commandFor(K("ctrl+="), When::Terminal), "workbench.action.terminal.fontZoomIn"));
        CHECK(is(d.commandFor(K("ctrl+shift+="), When::Editor), "editor.action.fontZoomIn"));
        CHECK(is(d.commandFor(K("ctrl+-"), When::Editor), "editor.action.fontZoomOut"));
        CHECK(is(d.commandFor(K("ctrl+-"), When::Terminal), "workbench.action.terminal.fontZoomOut"));
        CHECK(is(d.commandFor(K("ctrl+-"), When::Page), "minicode.page.zoomOut"));
        // Ctrl+Alt+0: fit width in a page, the terminals' reset in the
        // terminal, and Fold All in the editor (PARITY.md D1: the family of
        // VS Code's Ctrl+K chords), so the editor's reset is VS Code's own
        // Ctrl+keypad 0 alone. Ctrl+0 stays Focus File Tree.
        CHECK(is(d.commandFor(K("ctrl+alt+0"), When::Page), "minicode.page.fitWidth"));
        CHECK(is(d.commandFor(K("ctrl+alt+0"), When::Editor), "editor.foldAll"));
        CHECK(is(d.commandFor(K("ctrl+alt+0"), When::Terminal),
                 "workbench.action.terminal.fontZoomReset"));
        CHECK(is(d.commandFor(K("ctrl+numpad0"), When::Editor), "editor.action.fontZoomReset"));
        CHECK(d.keysOf("editor.action.fontZoomReset").size() == 1);
        CHECK(is(d.commandFor(K("ctrl+0"), When::Editor), "workbench.action.focusSideBar"));
        // The terminals' zoom keeps its plain Ctrl keys in the terminal; the
        // editor's would be the program's there, where it does not apply.
        const Command& tzoom = *find("workbench.action.terminal.fontZoomIn");
        CHECK(!linuxTerminalTakes(tzoom, K("ctrl+=")) && !linuxTerminalTakes(tzoom, K("ctrl+shift+=")));
        CHECK(!linuxTerminalTakes(*find("workbench.action.terminal.fontZoomOut"), K("ctrl+-")));
        CHECK(!linuxTerminalTakes(*find("workbench.action.terminal.fontZoomReset"),
                                  K("ctrl+numpad0")));
        CHECK(linuxTerminalTakes(*find("editor.action.fontZoomIn"), K("ctrl+=")));
        CHECK(!appliesIn(find("editor.action.fontZoomIn")->when, When::Terminal));
        CHECK(!appliesIn(tzoom.when, When::Editor) && !appliesIn(tzoom.when, When::Tree));
        // VTE's copy and paste stay its own even for a terminal command.
        CHECK(linuxTerminalTakes(tzoom, K("ctrl+shift+v")));
        // Word wrap: VS Code's Alt+Z, the editor's; Meta to the program.
        CHECK(d.keysOf("editor.action.toggleWordWrap").size() == 1 &&
              is(d.commandFor(K("alt+z"), When::Editor), "editor.action.toggleWordWrap"));
        CHECK(linuxTerminalTakes(*find("editor.action.toggleWordWrap"), K("alt+z")));
        // Maximize Pane: Ctrl+Alt+M anywhere, the terminal included.
        CHECK(is(d.commandFor(K("ctrl+alt+m"), When::Terminal), "minicode.toggleMaximizedPane"));
        CHECK(!linuxTerminalTakes(*find("minicode.toggleMaximizedPane"), K("ctrl+alt+m")));
        // Go Back and Forward: VS Code's Linux keys; in the terminal
        // Ctrl+Shift+- is readline's undo (Ctrl+_), Ctrl+Alt+- still goes back.
        CHECK(d.keysOf("workbench.action.navigateBack").size() == 1 &&
              HasKey(d, "workbench.action.navigateBack", "ctrl+alt+-"));
        CHECK(d.keysOf("workbench.action.navigateForward").size() == 1 &&
              HasKey(d, "workbench.action.navigateForward", "ctrl+shift+-"));
        CHECK(!linuxTerminalTakes(*find("workbench.action.navigateBack"), K("ctrl+alt+-")));
        CHECK(linuxTerminalTakes(*find("workbench.action.navigateForward"), K("ctrl+shift+-")));
        using V = std::vector<std::string>;
        CHECK(linuxAccelerators(K("ctrl+shift+-")) == V{"<Ctrl>underscore"});
        CHECK((linuxAccelerators(K("ctrl+alt+-")) == V{"<Ctrl><Alt>minus", "<Ctrl><Alt>KP_Subtract"}));
        CHECK(linuxAccelerators(K("alt+z")) == V{"<Alt>z"});
        CHECK(linuxAccelerators(K("ctrl+alt+m")) == V{"<Ctrl><Alt>m"});
        // None of the new keys is GNOME's or bound twice where places meet.
        CHECK(d.conflicts().empty());
    }
}
