// Editor.h — the text editing surface: a GtkTextView + GtkTextBuffer with
// syntax highlighting driven by the shared SyntaxHighlighter core, plus a
// Markdown preview toggle backed by Markdown.{h,cpp}.
#pragma once

#include <gtk/gtk.h>
#include <string>

class Editor {
public:
    Editor();
    ~Editor();

    // The scrolled widget to drop into the layout.
    GtkWidget* widget() const { return scroller_; }

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

private:
    void rehighlight();          // full re-lex of the raw buffer
    void renderPreview();        // build the Markdown preview into the buffer
    void loadRawIntoBuffer();    // put source_ back as editable, highlighted text
    void ensureTags();           // create the per-style + markdown GtkTextTags once
    void markDirty(bool d);

    static void onBufferChanged(GtkTextBuffer* buf, gpointer self);

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
    guint       rehiTimer_  = 0;  // debounce id for re-highlight

    TitleCb titleCb_ = nullptr;
    void*   titleUser_ = nullptr;
};
