// Editor.h — the text editing surface: a GtkTextView + GtkTextBuffer with
// syntax highlighting driven by the shared IncrementalHighlighter core, plus a
// Markdown preview toggle backed by Markdown.{h,cpp}.
#pragma once

#include <gtk/gtk.h>
#include <memory>
#include <string>
#include <vector>

#include "Settings.h"
#include "SyntaxHighlighter.h"

class Editor {
public:
    Editor();
    ~Editor();

    // The scrolled widget to drop into the layout.
    GtkWidget* widget() const { return scroller_; }

    // The GtkTextView / GtkTextBuffer themselves, for the shell's find bar.
    GtkWidget*     textView() const { return view_; }
    GtkTextBuffer* buffer()   const { return buffer_; }

    // Load a file from disk into the buffer, detect its extension and
    // re-highlight. Returns false if the file could not be read.
    bool openFile(const std::string& path);

    // Write the current buffer (or the raw source, in preview mode) to disk.
    bool save();

    // Toggle between the raw editable buffer and the rendered Markdown preview.
    // No-op unless the current file is Markdown.
    void togglePreview();
    bool isMarkdown() const { return isMarkdown_; }
    bool inPreview() const { return preview_; }

    const std::string& currentPath() const { return path_; }
    bool dirty() const { return dirty_; }

    // Optional: called whenever the dirty/title state changes so the shell can
    // refresh the window title. Set by main.cpp.
    using TitleCb = void(*)(void* user);
    void setTitleCallback(TitleCb cb, void* user) { titleCb_ = cb; titleUser_ = user; }

    // Show a plain gray message (welcome screen / errors), not editable.
    void showMessage(const std::string& msg);

    // Colors from the settings file: syntax and Markdown tags, and the swatches
    // shown while the settings file itself is open. The panel background and
    // text come from the stylesheet (ThemeCss).
    void applySettings(const Settings& s);
    // The settings file's path. While that file is open its color values are
    // clickable swatches that open a color picker.
    void setSettingsPath(const std::string& path) { settingsPath_ = path; }

    // Ctrl+/ : comment or uncomment the lines the selection touches. Returns
    // false when there is nothing to do it to (preview, message, a file type
    // with no line comments).
    bool toggleComment();

    // The open file now lives at `path` (a rename or save-as). Keeps the
    // buffer and its undo history, and re-highlights the whole buffer only if
    // the extension, and so the grammar, changed.
    void setPath(const std::string& path);

private:
    // Highlighting. startHighlighting() lexes the whole buffer and retags it
    // (file load, grammar change, settings change). After that every edit is
    // fed to the IncrementalHighlighter from the buffer's insert-text and
    // delete-range signals, and only the lines it reports are retagged: at
    // once if they are few or on screen, otherwise in idle time slices.
    void startHighlighting();
    void stopHighlighting();     // buffer no longer holds source (preview, message)
    void flushHighlighting();    // retag the lines pending since the last flush
    void retagSpan(size_t firstLine, size_t endLine);   // lex from stored states, retag
    void retagLines(size_t firstLine, size_t endLine, const std::vector<Token>& tokens);
    void visibleLines(size_t& first, size_t& end) const;
    void cancelDeferred();
    void resyncAfterBulk();      // one prefix/suffix diff for a many-edit user action
    void removeHighlightTags(const GtkTextIter* a, const GtkTextIter* b);
    size_t byteOffsetOf(const GtkTextIter* it) const;
    bool linesMatch() const;     // GTK's lines are the highlighter's '\n' lines
    bool isHighlightTag(GtkTextTag* tag) const;
    void noteEdit(size_t pos, size_t oldLen, size_t newLen);

    static void onInsertText(GtkTextBuffer* buf, GtkTextIter* loc, char* text,
                             int len, gpointer self);
    static void onDeleteRange(GtkTextBuffer* buf, GtkTextIter* a, GtkTextIter* b,
                              gpointer self);
    static void onInsertTextAfter(GtkTextBuffer* buf, GtkTextIter* loc, char* text,
                                  int len, gpointer self);
    static void onDeleteRangeAfter(GtkTextBuffer* buf, GtkTextIter* a, GtkTextIter* b,
                                   gpointer self);
    static void onApplyTag(GtkTextBuffer* buf, GtkTextTag* tag, GtkTextIter* a,
                           GtkTextIter* b, gpointer self);
    static void onBeginUserAction(GtkTextBuffer* buf, gpointer self);
    static void onEndUserAction(GtkTextBuffer* buf, gpointer self);
    static gboolean onIdleRetag(gpointer self);

    void renderPreview();        // build the Markdown preview into the buffer
    void loadRawIntoBuffer();    // put source_ back as editable, highlighted text
    void ensureTags();           // create the per-style + markdown GtkTextTags once
    void setProseFont(bool prose);  // proportional for preview/messages, mono for code
    void markDirty(bool d);

    static void onBufferChanged(GtkTextBuffer* buf, gpointer self);

    bool isSettingsFile() const;
    void decorateColors(int firstLine, int endLine);   // swatch tags over color values
    bool swatchAt(double x, double y, int* line) const;   // widget coords
    void pickColor(int line);
    void setLineColor(int line, const Rgba& c);
    static void onPressed(GtkGestureClick* g, int n, double x, double y, gpointer self);
    static void onMotion(GtkEventControllerMotion* m, double x, double y, gpointer self);

    GtkWidget*     scroller_ = nullptr;
    GtkWidget*     view_     = nullptr;   // GtkTextView
    GtkTextBuffer* buffer_   = nullptr;

    std::string path_;
    std::string ext_;         // lowercase, no dot
    std::string source_;      // authoritative UTF-8 source text
    bool        isMarkdown_ = false;
    bool        preview_    = false;
    bool        dirty_      = false;
    bool        tagsReady_  = false;
    bool        sourceMode_ = false;   // the buffer holds editable source text

    // Highlighting state, live only while the buffer holds source of a
    // language the lexer knows. mirror_ is a UTF-8 copy of the buffer kept in
    // step edit by edit, the TextSource the highlighter reads.
    std::unique_ptr<IncrementalHighlighter<char>> hl_;
    std::string  mirror_;
    size_t       pendStart_ = 0, pendEnd_ = 0;   // bytes still to retag
    bool         pending_ = false;
    size_t       defStart_ = 0, defEnd_ = 0;     // bytes left to the idle retag
    bool         deferred_ = false;
    guint        idleId_ = 0;
    bool         inAction_ = false;    // between begin- and end-user-action
    int          actionEdits_ = 0;     // edits seen in the current user action
    bool         bulk_ = false;        // stopped tracking edits one by one
    bool         applyingTags_ = false;
    std::vector<GtkTextTag*> hlTags_;   // syntax tags plus swatches: all a retag owns

    std::string settingsPath_;
    Settings    settings_;    // for tag colors and swatch text contrast
    bool        overSwatch_ = false;

    TitleCb titleCb_ = nullptr;
    void*   titleUser_ = nullptr;
};
