// TestGutter.h — the ▶ beside each test in a test file, the GTK port's
// counterpart of the Mac's MCTestGutter (mac/src/TestGutter.mm), over the
// same core (src/Runnables.h: pytest, go test, cargo test, Jest and
// Vitest). One per window: it finds the tests in the file on show off the
// main thread, 0.3 s after typing stops and at once for another tab, and
// gives the gutter's test lane (Gutter.h; the marks are the buffer's
// BufferLines::tests, so they move with each edit) a mark for each. A
// click on one, or Run Test at Cursor (testing.runAtCursor, no key; the
// test the caret is in, or the whole file above the first test), saves
// the file and types the command into the terminal as Run File does: never
// into a busy terminal (a new tab instead), the keyboard left in the
// editor, a `cd` to the project's folder first unless the shell is there.
//
// The run is matched to the command that finishes it: the terminal it was
// typed into, that terminal's shell (a restarted shell is another), and
// the integration's count of commands started when it was typed, so only
// the command it typed counts, never just any command (and when the
// finished command's line is known it must hold the run's). Its 133;D exit
// status makes the mark a green check (0) or a red cross, until the file
// is edited. No marks in large-file mode, a preview or a diff, or for a
// language whose test.<language> is off.
//
// It knows nothing of the terminal itself (a build may have none): the
// window types the line (`run`) and tells it of finished commands and
// closed terminals.
#pragma once

#include <gio/gio.h>
#include <gtk/gtk.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "BufferLines.h"
#include "Runnables.h"

class Editor;

class TestGutter {
public:
    explicit TestGutter(Editor* editor);
    // Called while the window's widgets still exist. A look running on the
    // worker finishes there; its answer is dropped.
    ~TestGutter();
    TestGutter(const TestGutter&) = delete;
    TestGutter& operator=(const TestGutter&) = delete;

    // The window's action (Keys.cpp names it): runtestatcursor.
    void installActions(GActionMap* window);
    // Look again soon (the settings changed: test.<language>).
    void refreshSoon();

    // The ▶ on test `index` (in the file's order), and Run Test at Cursor.
    // False, with a bell and a status note, when nothing can run.
    bool runTestAtIndex(int index);
    bool runTestAtCaret();
    // Run Test at Cursor can act: the file on show is a test file with tests.
    bool canRunTestAtCaret() const;

    // ---- what the window tells it
    // Where a run's line went: the terminal (the control channel's id), its
    // shell (the host's terminal, another for a restarted shell), and the
    // count of commands that shell had started when the line was typed.
    struct Typed {
        int terminal = 0;
        unsigned long long shell = 0;
        long long started = 0;
    };
    // A command finished in a terminal (its onCommandFinished), with the
    // shell's count of finished commands, its exit status, and its line
    // when the integration gave it ("" unknown).
    struct Finished {
        int terminal = 0;
        unsigned long long shell = 0;
        long long done = 0;
        int status = -1;
        std::string line;
    };
    void commandFinished(const Finished& f);
    // The window's terminals changed (one closed): runs in a terminal gone
    // say nothing more, and their marks go back to ▶.
    void terminalsChanged();

    // ---- what the window gives it
    std::function<std::string()> root;                                 // the window's folder
    std::function<std::string(const std::string& path)> python;        // {python}, as Run File's
    std::function<std::string(const std::string& language)> templateFor;   // test.<language>
    // Saves the file and types `line` into a terminal as Run File does, run
    // in `dir`. False when nothing will be typed (the save failed and said
    // why); else `typed` is called once, perhaps at once, with where it
    // went and "" once typed, or why it was not.
    std::function<bool(const std::string& line, const std::string& dir,
                       std::function<void(const Typed& where, const std::string& why)> typed)>
        run;
    // Is the terminal with this id still open?
    std::function<bool(int terminal)> terminalOpen;
    std::function<void(const std::string& text)> onNote;   // the status bar

    // ---- for the checks
    std::vector<std::string> testLabels() const;     // "TestParser::test_parse"
    std::vector<TestState> testStates() const;       // the marks on show
    unsigned looks() const { return looks_; }        // looks finished
    const std::string& lastCommand() const { return lastCommand_; }   // the last typed, no cd

    struct Run;

private:
    std::string pathToLook() const;
    Runnables::Kind kindOf(const std::string& path) const;
    GtkTextBuffer* viewBuffer() const;
    std::string labelOf(const Runnables::Test& t) const;
    std::string bufferText(GtkTextBuffer* b) const;

    void attachBuffer();
    void detachBuffer();
    void edited();
    void scheduleAfter(int ms);
    void look();
    void pushMarks();
    bool start(const std::vector<Runnables::Test>& tests, bool wholeFile);
    void forget(int runId);   // a run that will say nothing: its marks back to ▶
    void updateAction();
    void post(std::function<void()> job);
    void note(const std::string& text);

    Editor* editor_ = nullptr;
    GtkWidget* view_ = nullptr;
    GThreadPool* pool_ = nullptr;
    std::shared_ptr<bool> alive_;
    GtkTextBuffer* buffer_ = nullptr;       // the view's, held while listened to
    unsigned generation_ = 0;               // every edit, switch and look
    guint timer_ = 0;
    gint64 lastEdit_ = 0;                   // monotonic µs
    bool pauseWanted_ = false;

    std::string shownPath_;                 // the file the tests are of
    GtkTextBuffer* shownBuffer_ = nullptr;  // its buffer, compared, never followed
    Runnables::Kind kind_ = Runnables::Kind::None;
    std::vector<Runnables::Test> tests_;
    // What the last runs said, by file and test, until the file changes.
    std::map<std::string, std::map<std::string, TestState>> results_;
    std::vector<std::unique_ptr<Run>> runs_;
    int nextRun_ = 0;

    GSimpleAction* action_ = nullptr;
    unsigned looks_ = 0;
    std::string lastCommand_;
};
