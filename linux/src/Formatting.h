// Formatting.h — Format Document in a window (PARITY.md 2.4), the GTK
// port's EditorController+Format.mm. One per window; main.cpp makes it and
// calls it from the key, Ctrl+S, auto-save and tab switches.
//
//   - The key (Ctrl+Shift+I, VS Code's Linux key) runs the file's formatter
//     (FormatRunner.h, the plan in Formatter.h) on the buffer's text off the
//     main thread and puts its output in as one undo step through
//     MultiEdit::apply (D5): only the line runs that changed, so the caret,
//     the selection and the line at the top of the view stay with their
//     text, and the highlighter and the language server follow each run as
//     they follow typing. With lines selected only those are formatted,
//     where the formatter can (clang-format, yapf, pyink, black): VS Code's
//     Format Selection is a chord, which minicode has none of, and the Mac
//     formats a selection's lines with the same key.
//   - A result for text that changed meanwhile (typing, another tab) is
//     dropped. A failure (a non-zero exit, no output, output that is not
//     UTF-8, the 10 s limit) leaves the buffer alone and shows why, with the
//     formatter's first lines of stderr, in a note by the caret.
//   - A formatter in the folder's own .venv or node_modules, or one whose
//     configuration in the folder runs code, runs only once the folder
//     allows its own tools (FolderConsent.h): asked first, Allow, Don't
//     Allow or Cancel, and the answer kept for the folder.
//   - When nothing is planned for the file at all, its language server
//     formats it, if one is running that does (LspSession::formatDocument).
//   - format.on-save: Ctrl+S formats, then saves; Save All and auto-save
//     never format, and auto-save leaves a tab whose Ctrl+S waits. A Ctrl+S
//     is never lost: the tab leaving the screen first is saved as it is.
//   - editor.organize-imports-on-save waits for the port's code actions
//     (item 2.2).
#pragma once

#include <gtk/gtk.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "FormatRun.h"

class Document;
class Editor;
class LspSession;

class Formatting {
public:
    struct Host {
        std::function<std::string()> root;   // the window's folder
        // Ctrl+S's own save of `d`, unformatted, with its error shown.
        std::function<void(Document* d)> save;
    };

    Formatting(GtkWindow* window, Editor* editor, LspSession* lsp, Host host);
    ~Formatting();
    Formatting(const Formatting&) = delete;
    Formatting& operator=(const Formatting&) = delete;

    // The view shows a file's editable source.
    bool canFormat() const;
    // The key: the bell when there is nothing to format.
    void formatDocument();
    // Ctrl+S with format.on-save on: true when a format started, and the
    // save comes when it is done; false when the file has nothing to format
    // it, and the caller saves at once.
    bool formatBeforeSaving();
    // A tab whose Ctrl+S waits for its formatter (auto-save leaves it).
    bool saveWaits(const Document* d) const;
    // The tab on show is about to leave the screen (another tab, or it
    // closes): a Ctrl+S waiting for its formatter saves it now, as it is.
    void tabLeaving();

    // For checks: the last note shown ("" for none), whether a note shows,
    // and how many times the folder's question was put.
    const std::string& lastNote() const { return lastNote_; }
    bool noteShown() const;
    int notesShown() const { return notes_; }
    int questionsAsked() const { return questions_; }

private:
    // `then` runs once the result is in (applied or not) while the same tab
    // is on show; `quiet` leaves out the notes that only answer the key (no
    // formatter for this kind of file, already formatted).
    void run(bool quiet, std::function<void()> then);
    void ask(const FormatRun::Decision& d, bool quiet, std::function<void()> then);
    void finishSave();
    // The formatter's output in place of the source, as one undo step; false
    // when it is the same text.
    bool apply(const std::string& text);
    void showNote(const std::string& message, bool error);
    void closeNote();
    static void onViewUnrealize(GtkWidget* view, gpointer self);

    GtkWindow* window_;
    Editor* editor_;
    LspSession* lsp_;
    Host host_;
    unsigned generation_ = 0;
    uint64_t saveTab_ = 0;   // Document::id of the tab whose Ctrl+S waits
    std::string lastNote_;
    int notes_ = 0;
    int questions_ = 0;
    GtkWidget* note_ = nullptr;        // a popover on the text view
    GtkWidget* noteLabel_ = nullptr;
    guint noteTimer_ = 0;
    gulong noteBufferHandler_ = 0;     // the note closes on the next edit
    GtkTextBuffer* noteBuffer_ = nullptr;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};
