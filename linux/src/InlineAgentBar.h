// InlineAgentBar.h — the inline agent's bar over the editor (Ctrl+I; the
// Mac's AgentPanel.mm, PARITY.md 3.3 part one: edits in the editor). A box
// at the caret's line with a request field. The request goes, with the
// buffer and the selection (InlineAgent::editPrompt), to agent.command run
// through the user's login shell in print mode; its stream-json output is
// read on a worker thread into InlineAgent::Stream, and the bar shows a
// preview of it at most every 80 ms.
//
// The edit is pending, not made: the SEARCH/REPLACE blocks are applied to a
// copy, the bar shows the diff (InlineAgent::pendingDiff) and the lines it
// replaces are tinted in the buffer. Return keeps it: the blocks are made
// again on the buffer as it is then, the selection moved through the user's
// own edits (moveOffset), and when that is not the change shown
// (sameChange) the new diff is shown and Keep waits for another Return;
// else it goes in as one undo step. Esc drops it, nothing to restore. A
// request typed while an edit waits builds on it. A bar closed from
// elsewhere (another tab, the window) with an edit waiting says in the
// status bar that it was not made.
//
// Part two, by what has the keyboard (the Mac's rules; main.cpp decides):
// a text file or the Markdown preview (the edit is made on its source) is an
// edit; an idle terminal a Command (Claude's answer typed at the prompt,
// never run); anything else, or Ctrl+Enter in the bar, a Task: the prompt
// (InlineAgent::taskPrompt) goes to a 0600 file in $TMPDIR/MiniCode-agent
// (the files of a process that quit, or a day old, removed as the next is
// written) and `cd <root> && <agent.command> "$(cat <file>)"` is typed into
// the window's terminal for the user to read and run. The Linux port has one
// terminal, not tabs. Focus leaving the editor closes the bar (focusMoved).
#pragma once

#include <gtk/gtk.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "InlineAgent.h"

class Editor;
class Document;

class InlineAgentBar {
public:
    explicit InlineAgentBar(Editor* editor);
    ~InlineAgentBar();
    InlineAgentBar(const InlineAgentBar&) = delete;
    InlineAgentBar& operator=(const InlineAgentBar&) = delete;

    // What the window gives it: agent.command, the folder to run in, and a
    // line for the status bar.
    std::function<std::string()> command;
    std::function<std::string()> folder;
    std::function<void(const std::string&)> note;

    // Ctrl+I: opens the bar at the caret (or takes the keyboard again).
    // False when the editor shows no editable text.
    enum class Kind { Edit, Command, Task };
    // `where` says for a Task where it was asked from ("the file tree").
    bool open(Kind kind = Kind::Edit, const std::string& where = std::string());
    Kind kind() const { return kind_; }
    // Types a line into the window's terminal, never running it, showing and
    // focusing the terminal; false when it cannot (none, a program has it).
    std::function<bool(const std::string&)> typeLine;
    // The keyboard moved to `focus`: outside the editor closes the bar.
    void focusMoved(GtkWidget* focus);
    // The prompt file of the last Task, for the checks.
    const std::string& lastTaskFile() const { return lastTaskFile_; }
    const std::string& lastTypedLine() const { return lastTypedLine_; }
    // Closes it, stopping a run; an edit waiting is said not made.
    void close();
    // The tab on show is about to change, the window closes: close().
    void closeFromElsewhere() { if (mode_ != Mode::Closed) close(); }

    enum class Mode { Closed, Input, Working, Pending, Message };
    Mode mode() const { return mode_; }
    // For the checks.
    GtkWidget* widget() const { return box_; }
    GtkWidget* entry() const { return entry_; }
    const std::string& message() const { return message_; }   // what the bar says
    bool messageIsError() const { return messageError_; }
    const std::string& diffText() const { return diffText_; }   // the pending diff, plain
    int tintedLines() const;     // lines of the buffer tinted as replaced
    const std::string& lastPrompt() const { return lastPrompt_; }
    const std::string& lastCommand() const { return lastCommand_; }
    int previews() const { return previews_; }   // times the working preview changed
    const std::vector<std::string>& previewLog() const { return previewLog_; }   // what they said

private:
    struct Run;
    void build();
    void reposition();
    void setMode(Mode m);
    void say(const std::string& text, bool error);
    void submit(bool asTask = false);
    void runTask(const std::string& request);
    bool readSource(std::u16string& text, size_t& a, size_t& b) const;
    void retint();
    void applyTintLines(const std::vector<InlineAgent::DiffLine>& diff);
    void start(const std::string& prompt);
    void ended(const std::string& reply, const std::string& error, bool notFound);
    void pollPreview();
    void showPending(const std::u16string& now);
    void keep();
    void drop();
    void clearTint();
    void tint(const std::u16string& now, const std::vector<InlineAgent::DiffLine>& diff);
    bool applyToBuffer(const std::string& after);
    bool makeRounds(const std::string& from, size_t selStart, size_t selEnd, std::string* text,
                    size_t* selA, size_t* selB, std::string* error) const;
    void cancelRun();
    void focusText();
    static gboolean onKeyPressed(GtkEventControllerKey*, guint, guint, GdkModifierType, gpointer);

    Editor* ed_;
    Mode mode_ = Mode::Closed;
    Kind kind_ = Kind::Edit;
    std::string where_, lastTaskFile_, lastTypedLine_;
    gulong tintChanged_ = 0;
    guint retintTimer_ = 0;
    Document* doc_ = nullptr;
    GtkWidget *box_ = nullptr, *entry_ = nullptr, *label_ = nullptr, *hint_ = nullptr;
    GtkAdjustment* adjustment_ = nullptr;
    gulong scrollHandler_ = 0;
    GtkTextBuffer* tintBuffer_ = nullptr;
    GtkTextTag* tintTag_ = nullptr;

    std::string message_, diffText_, lastPrompt_, lastCommand_;
    bool messageError_ = false;
    int previews_ = 0;
    std::vector<std::string> previewLog_;

    // The edit waiting: the blocks of each request, the buffer they were
    // first made on (UTF-16) with the selection then, and what they make.
    std::vector<std::vector<InlineAgent::Edit>> rounds_;
    std::vector<std::string> requests_;
    std::u16string base_, pending_;
    size_t baseA_ = 0, baseB_ = 0;   // the selection in base_ (UTF-16 units)
    std::string pendingText_;        // UTF-8 of pending_
    size_t pendA_ = 0, pendB_ = 0;   // the selection in pendingText_ (bytes)
    // The text and selection the run in flight was asked about (bytes).
    std::string askedText_;
    size_t askedA_ = 0, askedB_ = 0;
    std::u16string askedBase_;
    size_t askedBaseA_ = 0, askedBaseB_ = 0;
    std::string asking_;

    std::shared_ptr<Run> run_;
    guint pollTimer_ = 0;
    unsigned generation_ = 0;
};
