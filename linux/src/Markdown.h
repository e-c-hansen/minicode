// Markdown.h — renders the shared MarkdownParser's MdRun list into a styled
// GtkTextBuffer. The buffer is expected to already carry the markdown tags
// created by Editor::ensureTags() (h1..h6, md_code, md_quote, md_rule,
// md_link, md_table, md_bold, md_italic, plainmsg).
//
// Tables and pictures are widgets anchored in the text (a grid of wrapping
// labels, and MdPicture, which plays GIFs); fit() sizes them to the pane.
// The Page it returns maps the rendered text back to the source, for links,
// for Ctrl+Shift+P keeping its place, and for editing from the preview.
#pragma once

#include <gtk/gtk.h>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Markdown {

// A stretch of rendered text and the 0-based source line it came from. A
// table is one span over its anchor character.
struct Span {
    int start = 0, end = 0;   // character offsets in the buffer
    int line = -1;
    std::string url;          // a link's target, as written
};

struct Embed {
    GtkWidget* widget = nullptr;
    bool table = false;
    int columns = 0;                    // a table's
    std::vector<GtkWidget*> labels;     // a table's cell labels
    int width = 0, height = 0;          // a picture's own size
};

struct Page {
    std::vector<Span> spans;             // in buffer order
    std::map<std::string, int> anchors;  // heading anchor -> character offset
    std::vector<Embed> embeds;
    // The span at character `offset`, or null.
    const Span* spanAt(int offset) const;
    // The first character from source line `line` or later (the end when none).
    int offsetForLine(int line, int endOffset) const;
};

struct Hooks {
    std::string folder;   // the Markdown file's folder, for relative pictures
    // A click on a link inside a table cell (links in the text are the
    // editor's to handle, from the spans).
    std::function<void(const std::string& url)> link;
    // A double-click on a table cell: the table's first source line, the
    // cell's row (0 = header) and column, and the cell's widget.
    std::function<void(int line, int row, int column, GtkWidget* cell)> cellDoubleClick;
};

// Replace the buffer's contents with the rendered Markdown.
Page render(GtkTextView* view, GtkTextBuffer* buffer, const std::string& source,
            const Hooks& hooks);

// Size the page's tables and pictures to a pane `width` pixels wide.
void fit(Page& page, int width);

}  // namespace Markdown
