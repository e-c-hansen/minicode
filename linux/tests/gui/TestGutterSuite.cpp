// TestGutterSuite.cpp — test_gutter: the ▶ beside each test (TestGutter.h),
// the Mac's suite of the same name (mac/tests/gui/TestGutterSuite.mm). A
// pytest file shows one by each test, in the gutter's test lane, with a
// tooltip naming it; a real click on one saves the file, types that one
// test's command into the terminal (test.python's template, naming a
// stand-in pytest on the shell's PATH that fails the TestBroken class and
// writes down what it was asked to run) and leaves the keyboard in the
// editor; the mark turns into a pass or a failure when that command ends,
// by the integration's exit status. Run Test at Cursor runs the test the
// caret is in, or the whole file above the first test; an edit clears
// what the runs said and moves the marks with their lines; a busy
// terminal is never typed into (a new tab takes the run); a file that is
// no test file has no lane. Everything is read from the app and the
// marks, never off the terminal's screen.
#include "Harness.h"
#include "AppSeam.h"
#include "BufferLines.h"
#include "CodeView.h"
#include "Editor.h"
#include "EnvLock.h"
#include "Gutter.h"
#include "Terminal.h"
#include "TerminalDock.h"
#include "TestGutter.h"
#include "WindowLayout.h"

#include <vte/vte.h>

#include <sys/stat.h>

#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
Gutter* gut() { return ed()->codeView()->gutter(); }
TestGutter* tests() { return MCGuiApp().tests; }
BufferLines* lines() { return BufferLines::of(gtk_text_view_get_buffer(GTK_TEXT_VIEW(ed()->textView()))); }
std::vector<TestState> states() { return tests()->testStates(); }
size_t markLine(size_t i) { return lines() && i < lines()->tests().size() ? lines()->tests()[i].line : 999; }
std::string status() { return gtk_label_get_text(GTK_LABEL(MCGuiApp().statusLabel)); }

std::string gDir, gPy, gLog, gBin;
std::string gOldPath, gOldShell;
bool gHadShell = false;
Terminal* gTerm = nullptr;     // the suite's own terminal
Terminal* gSecond = nullptr;   // the tab a run made while gTerm was busy
int gTabs = 0;
long long gStarted = 0;
unsigned gLooks = 0;

bool idle(Terminal* t) {
    if (!t || !Terminal::byId(t->controlId())) return false;
    const auto& s = t->shellState();
    return t->shellPid() > 0 && s.known && s.atPrompt && !s.busy;
}

bool keyboardInEditor() {
    GtkWidget* f = gtk_window_get_focus(MCGuiApp().window);
    return f && (f == ed()->textView() || gtk_widget_is_ancestor(f, ed()->textView()));
}

// A real click on test `i`'s mark.
void clickTest(int i) {
    const graphene_rect_t r = gut()->rectOfTest(i);
    GCHECK(r.size.width > 0 && r.size.height > 0);
    MCGuiClick(gut()->widget(), r.origin.x + r.size.width / 2, r.origin.y + r.size.height / 2);
}

std::string tipOf(int i) {
    const graphene_rect_t r = gut()->rectOfTest(i);
    return gut()->testTipAt(r.origin.x + r.size.width / 2, r.origin.y + r.size.height / 2);
}

bool runAtCursorEnabled() {
    return g_action_group_get_action_enabled(G_ACTION_GROUP(MCGuiApp().window), "runtestatcursor");
}

// What the stand-in was asked to run, one line a run.
std::vector<std::string> ran() {
    std::vector<std::string> out;
    std::string text = MCGuiReadFile(gLog), line;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(line);
            line.clear();
        } else {
            line += c;
        }
    }
    return out;
}

}  // namespace

MC_GUI_SUITE(test_gutter) {
    MCGuiStep("a pytest file, a stand-in pytest on the shell's PATH, a terminal of its own", [] {
        gDir = MCGuiProjectFile("tested");
        gPy = gDir + "/test_calc.py";
        gLog = MCGuiScratch() + "/test-gutter-ran.txt";
        gBin = MCGuiScratch() + "/test-gutter-bin";
        MCGuiWriteFile(gPy, "def test_adds():\n"
                            "    assert 1 + 1 == 2\n"
                            "\n"
                            "\n"
                            "class TestBroken:\n"
                            "    def test_fails(self):\n"
                            "        assert False\n");
        // Writes down what it was asked to run; TestBroken fails.
        MCGuiWriteFile(gBin + "/pytest", "#!/bin/sh\n"
                                         "echo \"$*\" >> '" + gLog + "'\n"
                                         "case \"$*\" in *TestBroken*) echo 'fake: 1 failed'; exit 1;; esac\n"
                                         "echo 'fake: 1 passed'\n");
        chmod((gBin + "/pytest").c_str(), 0755);
        MCGuiWriteFile(gLog, "");
        const char* path = g_getenv("PATH");
        gOldPath = path ? path : "";
        EnvLock::envSet("PATH", (gBin + ":" + gOldPath).c_str(), TRUE);
        const char* shell = g_getenv("SHELL");
        gHadShell = shell != nullptr;
        gOldShell = shell ? shell : "";
        EnvLock::envSet("SHELL", "/bin/bash", TRUE);
        MCGuiSetSettings("lsp.enabled = false\ntest.python = pytest {tests}\n");
        if (!MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        // Its own tab, whose shell gets the PATH above, at a prompt, so what
        // earlier suites left in theirs plays no part.
        gTerm = MCGuiApp().termDock->newTerminal();
        gTerm->focus();
        MCGuiOpenFileKept(gPy);
    });
    MCGuiWait("its shell at the prompt", 30, [] { return idle(gTerm) && gTerm->shellState().prompts >= 1; });
    MCGuiStep("the keyboard in the editor", [] { gtk_widget_grab_focus(ed()->textView()); });
    MCGuiWait("a mark by each test, drawn in a gutter wide enough for the lane", 10, [] {
        if (!tests() || tests()->testLabels() != std::vector<std::string>{"test_adds", "TestBroken::test_fails"} ||
            !lines() || lines()->tests().size() != 2 || !keyboardInEditor())
            return false;
        int marked = 0;
        for (const Gutter::Row& row : gut()->drawnRows()) marked += row.test >= 0;
        return marked == 2 && gtk_widget_get_width(gut()->widget()) >= gut()->wantedWidth();
    });
    MCGuiStep("in the test lane, on the tests' lines, each with its tooltip; a click on the first", [] {
        GCHECK(gut()->testLane());
        GCHECK(markLine(0) == 0 && markLine(1) == 5);
        GCHECK((states() == std::vector<TestState>{TestState::Idle, TestState::Idle}));
        GCHECK_EQ_STR(tipOf(1), "Run TestBroken::test_fails");
        // Leftmost, inside the gutter's width, and drawn there.
        const graphene_rect_t r = gut()->rectOfTest(1);
        GCHECK(r.origin.x == 0 && r.size.width == Gutter::kTestLane);
        GCHECK(gut()->laneWidth() >= Gutter::kTestLane + Gutter::kDotLane + Gutter::kFoldLane);
        GCHECK(gut()->wantedWidth() <= gtk_widget_get_width(gut()->widget()));
        int marked = 0;
        for (const Gutter::Row& row : gut()->drawnRows())
            if (row.test == (int)TestState::Idle) marked++;
        GCHECK(marked == 2);
        MCGuiSnapshot(gut()->widget(), "test-gutter-idle");
        GCHECK(runAtCursorEnabled());
        GCHECK(MCGuiApp().terminal == gTerm);
        gStarted = gTerm->shellState().started;
        clickTest(0);
    });
    MCGuiWait("the click typed the test's command", 10, [] {
        return tests()->lastCommand().find("tested/test_calc.py::test_adds") != std::string::npos;
    });
    MCGuiStep("the keyboard still in the editor", [] {
        GCHECK(keyboardInEditor());
        MCGuiNote("typed: %s", tests()->lastCommand().c_str());
    });
    MCGuiWait("the test ran in the terminal and passed", 20, [] {
        return !ran().empty() && states().size() == 2 && states()[0] == TestState::Passed;
    });
    MCGuiStep("a check by the passed test; the other untouched; that one command ran", [] {
        GCHECK_EQ_STR(ran().back(), "tested/test_calc.py::test_adds");
        GCHECK(ran().size() == 1);
        GCHECK(gTerm->shellState().done == gStarted + 1 && gTerm->shellState().status == 0);
        GCHECK(states()[1] == TestState::Idle);
        GCHECK_EQ_STR(tipOf(0), "test_adds passed. Click to run it again.");
        GCHECK(keyboardInEditor());
    });
    MCGuiWait("back at the prompt", 10, [] { return idle(gTerm); });
    MCGuiStep("the failing test's mark", [] { clickTest(1); });
    MCGuiWait("it ran and failed", 20, [] {
        return ran().size() == 2 && states().size() == 2 && states()[1] == TestState::Failed;
    });
    MCGuiStep("a cross by the failed test; the pass still shows", [] {
        GCHECK_EQ_STR(ran().back(), "tested/test_calc.py::TestBroken::test_fails");
        GCHECK(gTerm->shellState().status == 1);
        GCHECK(states()[0] == TestState::Passed);
        GCHECK_EQ_STR(tipOf(1), "TestBroken::test_fails failed. Click to run it again.");
        gtk_widget_queue_draw(gut()->widget());
    });
    MCGuiWait("drawn so", 5, [] {
        int passed = 0, failed = 0;
        for (const Gutter::Row& row : gut()->drawnRows()) {
            passed += row.test == (int)TestState::Passed;
            failed += row.test == (int)TestState::Failed;
        }
        return passed == 1 && failed == 1;
    });
    MCGuiStep("a picture", [] { MCGuiSnapshot(gut()->widget(), "test-gutter-results"); });
    MCGuiWait("the prompt again", 10, [] { return idle(gTerm); });
    MCGuiStep("Run Test at Cursor, the caret in test_fails's body", [] {
        GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ed()->textView()));
        GtkTextIter it, end;
        gtk_text_buffer_get_start_iter(b, &it);
        GCHECK(gtk_text_iter_forward_search(&it, "assert False", GTK_TEXT_SEARCH_TEXT_ONLY, &it, &end, nullptr));
        gtk_text_buffer_place_cursor(b, &it);
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApp().window), "runtestatcursor", nullptr);
        const std::string& c = tests()->lastCommand();
        const std::string want = "tested/test_calc.py::TestBroken::test_fails";
        GCHECK(c.size() >= want.size() && c.compare(c.size() - want.size(), want.size(), want) == 0);
        GCHECK(states()[1] == TestState::Running);
    });
    MCGuiWait("it failed again", 20, [] {
        return ran().size() == 3 && states()[1] == TestState::Failed && idle(gTerm);
    });

    MCGuiStep("an edit: the marks move with their lines and the results go", [] {
        gLooks = tests()->looks();
        GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ed()->textView()));
        GtkTextIter start;
        gtk_text_buffer_get_start_iter(b, &start);
        gtk_text_buffer_begin_user_action(b);
        gtk_text_buffer_insert(b, &start, "import os\n", -1);
        gtk_text_buffer_end_user_action(b);
        GCHECK(markLine(0) == 1 && markLine(1) == 6);
    });
    MCGuiWait("the tests found again", 10, [] { return tests()->looks() > gLooks; });
    MCGuiStep("each test's ▶ again, a line down; Run Test at Cursor above the first: the file", [] {
        GCHECK((states() == std::vector<TestState>{TestState::Idle, TestState::Idle}));
        GCHECK(markLine(0) == 1 && markLine(1) == 6);
        GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ed()->textView()));
        GtkTextIter start;
        gtk_text_buffer_get_start_iter(b, &start);
        gtk_text_buffer_place_cursor(b, &start);
        GCHECK(ed()->dirty());
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApp().window), "runtestatcursor", nullptr);
        // Saved first, as Run File saves.
        GCHECK(!ed()->dirty());
        GCHECK(MCGuiReadFile(gPy).compare(0, 10, "import os\n") == 0);
        const std::string& c = tests()->lastCommand();
        const std::string want = " tested/test_calc.py";
        GCHECK(c.size() >= want.size() && c.compare(c.size() - want.size(), want.size(), want) == 0);
        GCHECK((states() == std::vector<TestState>{TestState::Running, TestState::Running}));
    });
    MCGuiWait("the whole file ran and passed", 20, [] {
        return ran().size() == 4 && (states() == std::vector<TestState>{TestState::Passed, TestState::Passed}) &&
               idle(gTerm);
    });
    MCGuiStep("one argument: the file", [] { GCHECK_EQ_STR(ran().back(), "tested/test_calc.py"); });

    // A busy terminal is never typed into: the run goes to a new tab.
    MCGuiStep("the terminal busy with a command of its own", [] {
        gStarted = gTerm->shellState().started;
        gTerm->runLine("sleep 30", [](const std::string& why) { GCHECK_EQ_STR(why, ""); });
    });
    MCGuiWait("it runs", 10, [] { return gTerm->shellState().busy && gTerm->busy(); });
    MCGuiStep("a click on the first test's mark", [] {
        gTabs = MCGuiApp().termDock->count();
        gtk_widget_grab_focus(ed()->textView());
        clickTest(0);
    });
    MCGuiWait("a new tab ran it, and it passed", 30, [] {
        return MCGuiApp().termDock->count() == gTabs + 1 && ran().size() == 5 && states().size() == 2 &&
               states()[0] == TestState::Passed;
    });
    MCGuiStep("nothing typed into the busy one; said in the status bar; keyboard kept", [] {
        gSecond = MCGuiApp().termDock->active();
        GCHECK(gSecond != gTerm);
        GCHECK(gTerm->shellState().started == gStarted + 1 && gTerm->shellState().busy);
        GCHECK_EQ_STR(ran().back(), "tested/test_calc.py::test_adds");
        GCHECK(status().find("The terminal was busy, so the test runs in a new one") != std::string::npos);
        GCHECK(keyboardInEditor());
        // The sleep interrupted.
        vte_terminal_feed_child(VTE_TERMINAL(gTerm->vteWidget()), "\x03", 1);
    });
    MCGuiWait("the busy one back at its prompt", 10, [] { return idle(gTerm); });
    MCGuiStep("its interrupted command changed no mark", [] {
        GCHECK((states() == std::vector<TestState>{TestState::Passed, TestState::Passed}));
    });

    MCGuiStep("a file that is not a test file has no lane", [] {
        MCGuiWriteFile(gDir + "/calc.py", "def test_like():\n    pass\n");
        MCGuiOpenFileKept(gDir + "/calc.py");
    });
    MCGuiWait("no lane", 10, [] { return ed()->currentPath() == gDir + "/calc.py" && !gut()->testLane(); });
    MCGuiStep("nothing to run there; back to the test file: its results kept", [] {
        GCHECK(!runAtCursorEnabled());
        MCGuiOpenFileKept(gPy);
    });
    MCGuiWait("the lane back with the passes", 10, [] {
        return gut()->testLane() && (states() == std::vector<TestState>{TestState::Passed, TestState::Passed});
    });
    MCGuiStep("the terminals, PATH and SHELL as they were", [] {
        if (gSecond && Terminal::byId(gSecond->controlId())) MCGuiApp().termDock->closePane(gSecond, false);
        if (gTerm && Terminal::byId(gTerm->controlId())) MCGuiApp().termDock->closePane(gTerm, false);
        gSecond = gTerm = nullptr;
        EnvLock::envSet("PATH", gOldPath.c_str(), TRUE);
        if (gHadShell) EnvLock::envSet("SHELL", gOldShell.c_str(), TRUE);
        else EnvLock::envUnset("SHELL");
        gtk_widget_grab_focus(ed()->textView());
    });
    // The settings as the suite found them (the harness's LEFT line).
    MCGuiStep("the settings as found", [] { MCGuiSetSettings("lsp.enabled = false\n"); });
}
