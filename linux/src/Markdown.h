// Markdown.h — renders the shared MarkdownParser's MdRun list into a styled
// GtkTextBuffer. The buffer is expected to already carry the markdown tags
// created by Editor::ensureTags() (h1..h6, md_code, md_quote, md_rule,
// md_link, md_table, md_bold, md_italic, md_strike, md_gap, md_done, plainmsg).
//
// Tables and pictures are widgets anchored in the text (a grid of wrapping
// labels, and MdPicture, which plays GIFs); fit() sizes them to the pane.
// So is a task's box ("- [ ] x"), a check button a click toggles through
// Hooks::taskToggle; a checked item's text gets the md_done tag.
// The Page it returns maps the rendered text back to the source, for links,
// for Ctrl+Shift+P keeping its place, and for editing from the preview.
//
// A picture from an https:// address (WebImages.h) is shown at once when it
// is kept in memory. Otherwise its alt text stands in, the Page lists it as
// pending, and the editor fetches it and calls placeWebPicture when it
// arrives, which swaps the picture in for the alt text in place.
#pragma once

#include <gtk/gtk.h>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "WebImages.h"

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

// A web picture not here yet: the alt text standing in for it.
struct PendingPicture {
    std::string url;          // the https:// address
    int start = 0, end = 0;   // the alt text's characters in the buffer
    int span = -1;            // its index in Page::spans
    std::string link;         // a linked picture's (a badge's) target
};

struct Hooks;

struct Page {
    std::vector<Span> spans;             // in buffer order
    std::map<std::string, int> anchors;  // heading anchor -> character offset
    std::vector<Embed> embeds;
    std::vector<PendingPicture> pending; // web pictures to fetch, in buffer order
    std::shared_ptr<Hooks> hooks;        // what the page's widgets call back
    // The span at character `offset`, or null.
    const Span* spanAt(int offset) const;
    // The first character from source line `line` or later (the end when none).
    int offsetForLine(int line, int endOffset) const;
};

struct Hooks {
    std::string folder;   // the Markdown file's folder, for relative pictures
    // Pictures from https:// addresses are shown (markdown.web-images).
    bool webImages = false;
    // A click on a link inside a table cell or on a linked picture (links
    // in the text are the editor's to handle, from the spans).
    std::function<void(const std::string& url)> link;
    // A double-click on a table cell: the table's first source line, the
    // cell's row (0 = header) and column, and the cell's widget.
    std::function<void(int line, int row, int column, GtkWidget* cell)> cellDoubleClick;
    // A click on a task's box ("- [ ] x"): the item's source line. Called
    // from an idle once the click is over, so the page may be rendered again.
    std::function<void(int line)> taskToggle;
};

// Replace the buffer's contents with the rendered Markdown.
Page render(GtkTextView* view, GtkTextBuffer* buffer, const std::string& source,
            const Hooks& hooks);

// Size the page's tables and pictures to a pane `width` pixels wide.
void fit(Page& page, int width);

// The web picture `url` has arrived (or failed, when `picture` is null):
// every pending entry for it is taken off the page, and with a picture its
// alt text is replaced by the picture, in the buffer as it stands, with the
// offsets of everything after it moved to match. Returns how many were
// placed; fit() sizes them. The buffer must still hold this page.
int placeWebPicture(GtkTextView* view, GtkTextBuffer* buffer, Page& page,
                    const std::string& url, const WebImages::PicturePtr& picture);

}  // namespace Markdown
