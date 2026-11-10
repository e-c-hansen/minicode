// NavRecorder.h — pure C++. What a window records for Go Back and Go
// Forward, over NavHistory.h's rules: the Mac's MCNavigator
// (src/Navigation.mm) as a class any port drives. The GTK port's
// linux/src/NavHistoryUi.cpp is its host; the Mac keeps its own, which this
// follows rule for rule.
//
// A jump is recorded when the window moves somewhere far: a tab switch
// (around tabWillSwitch and tabDidSwitch, so every way of switching
// counts), anything run inside jump() (go to definition, a symbol, Go to
// Line, a terminal link, a Find in Folder result), and a caret that moved
// more than Nav::kFarLines lines at once with the text unchanged
// (caretMoved: Ctrl+End, a click far down, the find bar's next match).
// Going back and forth itself, what happens inside a jump, and anything
// while quiet (a symbol previewed while the palette moves through its
// list) record nothing more.
//
// The host says where the window is and shows a place again. Offsets are
// the port's own units (the Mac's are UTF-16, the GTK port's characters),
// as long as the host is consistent; lines are 0-based.
#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "NavHistory.h"

namespace Nav {

class Recorder {
public:
    struct Host {
        // Where the window is: false with no file on show. A file shown
        // without text to put a caret in has hasText false. `textLength`
        // is its text's length (0 without text), for telling a caret's
        // move from an edit.
        std::function<bool(Place* place, size_t* textLength)> current;
        // Show a place again: its file, opened if need be, and with text
        // its selection, in view, the keyboard in the editor. False when
        // the file could not be shown.
        std::function<bool(const Place& place)> show;
        // Whether a file is still there, for passing over deleted ones.
        std::function<bool(const std::string& path)> exists;
        // The 0-based lines of these offsets (ascending) of the text on
        // show, for places an edit may have moved.
        std::function<std::vector<int>(const std::vector<size_t>& offsets)> linesOf;
        // Nowhere to go (the Mac beeps).
        std::function<void()> bell;
    };

    explicit Recorder(Host host, size_t cap = 50);

    // Where the window is now, if anywhere.
    bool current(Place* out);

    // Run a jump: where the window was before it and where it is after are
    // recorded (once, however deeply jumps nest). `from`, when given,
    // stands for where it was (the palette has moved the caret while
    // previewing a symbol).
    void jump(const std::function<void()>& body);
    void jumpFrom(const Place& from, const std::function<void()>& body);
    // Nothing is recorded while quiet; the caret is noted afresh after.
    void beQuiet(const std::function<void()>& body);
    void beginQuiet();
    void endQuiet();
    bool quiet() const { return quiet_ > 0; }
    // Around a tab switch.
    void tabWillSwitch();
    void tabDidSwitch();
    // The caret or the selection moved (or the text did): a jump when it
    // went more than kFarLines lines with the text's length unchanged.
    void caretMoved();
    // Remember where the caret is now, for the next caretMoved to compare
    // with (after a tab came on show, a file reloaded, and the like).
    void noteCaret();
    // The text of `path` changed: [pos, pos + oldLen) became newLen units.
    void edited(const std::string& path, size_t pos, size_t oldLen, size_t newLen);

    bool canGoBack();
    bool canGoForward() const { return history_.canGoForward(); }
    // Back and Forward: false, with the bell, when there is nowhere to go.
    // Files deleted meanwhile, or that cannot be shown, are passed over and
    // dropped.
    bool goBack() { return go(true); }
    bool goForward() { return go(false); }

    // A file or folder moved; one was deleted (or a folder with what was
    // in it); another folder opened.
    void moved(const std::string& from, const std::string& to);
    void removed(const std::string& path);
    void clear();

    // Tests: the history's places, "path:line" each (lines counted again
    // first), and which is current.
    std::vector<std::string> entries();
    size_t index() const { return history_.index(); }
    const History& history() const { return history_; }

private:
    void record(const Place& from, const Place& to);
    void fixLines();
    void dropMissing();
    bool go(bool back);

    Host host_;
    History history_;
    int jumpDepth_ = 0, quiet_ = 0, switching_ = 0;
    bool navigating_ = false;
    bool haveJumpFrom_ = false, haveSwitchFrom_ = false;
    Place jumpFrom_, switchFrom_;
    // The caret as last seen, for telling a far move from typing.
    bool haveLast_ = false;
    Place last_;
    size_t lastLength_ = 0;
};

}  // namespace Nav
