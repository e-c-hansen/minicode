// NavHistoryUi.h — Go Back and Go Forward in the GTK port (linux/PARITY.md
// item 1.11; VS Code's Linux Ctrl+Alt+- and Ctrl+Shift+-): one per window,
// the host of the core's Nav::Recorder (../src/NavRecorder.h, over
// NavHistory.h's rules), as the Mac's MCNavigator (src/Navigation.mm) is.
//
// What it records, as the Mac does, of what the port has: a tab switch
// (every way of switching: the strip, the keys, Quick Open, the tree,
// closing the tab on show), anything main.cpp runs inside jump() (a Find in
// Folder or TODO result, the language server's definition, a terminal or
// Markdown link), the palette's Go to Line and symbols (jumpFrom where the
// editor was when the palette opened; nothing while it previews), and the
// caret moving more than ten lines with the text unchanged (Ctrl+End, a
// click far down, the find bar's next match). Places are character offsets
// of their file's text, moved by edits to it (the source buffer on show,
// watched here), and follow renames and Move to Trash (moved, removed);
// files deleted otherwise are passed over when Back or Forward reaches
// them.
//
// The window says how to open a file again (Host::openFile, a kept tab)
// and calls tabWillSwitch and tabDidSwitch around every switch
// (Editor::onActiveWillChange and onActiveChanged).
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

#include "NavRecorder.h"

class Editor;

class NavHistoryUi {
public:
    struct Host {
        // The file on show in its tab, opened in a kept tab if it has none;
        // false when it could not be shown.
        std::function<bool(const std::string& path)> openFile;
        std::function<void()> bell;
    };
    NavHistoryUi(Editor* editor, Host host);
    ~NavHistoryUi();
    NavHistoryUi(const NavHistoryUi&) = delete;
    NavHistoryUi& operator=(const NavHistoryUi&) = delete;

    // Where the window is (a file's place, its caret), if anywhere.
    bool current(Nav::Place* out) { return rec_.current(out); }
    // A jump: where the window was and where it is after `body`.
    void jump(const std::function<void()>& body) {
        flushCaret();
        rec_.jump(body);
    }
    void jumpFrom(const Nav::Place& from, const std::function<void()>& body) {
        flushCaret();
        rec_.jumpFrom(from, body);
    }
    // The palette opened (nothing it previews is recorded) and closed; a
    // row's move to a line or symbol is a jump from where it opened.
    void paletteOpened();
    void paletteClosed();
    void paletteJump(const std::function<void()>& body);
    // Around a tab switch.
    void tabWillSwitch() {
        flushCaret();
        rec_.tabWillSwitch();
    }
    void tabDidSwitch();

    bool goBack() {
        flushCaret();
        return rec_.goBack();
    }
    bool goForward() {
        flushCaret();
        return rec_.goForward();
    }
    bool canGoBack() { return rec_.canGoBack(); }
    bool canGoForward() const { return rec_.canGoForward(); }

    // A file or folder moved (Rename, a drag in the tree); one went to the
    // Trash; another folder opened.
    void moved(const std::string& from, const std::string& to) { rec_.moved(from, to); }
    void removed(const std::string& path) { rec_.removed(path); }
    void clear();

    // For the checks: "path:line" a place, and which is current.
    std::vector<std::string> entries() {
        flushCaret();
        return rec_.entries();
    }
    size_t index() const { return rec_.index(); }

private:
    bool place(Nav::Place* out, size_t* length);
    bool show(const Nav::Place& p);
    void watch(GtkTextBuffer* b);   // the source buffer on show
    void unwatch();
    // The caret moved this turn: looked at now rather than in the idle.
    void flushCaret();
    static void onMarkSet(GtkTextBuffer* b, GtkTextIter*, GtkTextMark* mark, gpointer self);
    static void onInserted(GtkTextBuffer* b, GtkTextIter* end, char* text, int len, gpointer self);
    static void onDeleting(GtkTextBuffer* b, GtkTextIter* start, GtkTextIter* end, gpointer self);

    Editor* editor_;
    Host host_;
    Nav::Recorder rec_;
    GtkTextBuffer* watched_ = nullptr;   // a weak pointer
    std::string watchedPath_;
    bool replacingAll_ = false;   // the whole text deleted, the new text to come
    guint caretIdle_ = 0;         // the caret's look this turn (onMarkSet)
    bool paletteFromSet_ = false;
    Nav::Place paletteFrom_;
};
