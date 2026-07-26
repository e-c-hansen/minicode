// Markdown.h — renders the shared MarkdownParser's flat MdRun list into a
// styled GtkTextBuffer. The buffer is expected to already carry the markdown
// tags created by Editor::ensureTags() (h1..h6, md_code, md_quote, md_rule,
// md_link, md_table, md_bold, md_italic).
#pragma once

#include <gtk/gtk.h>
#include <string>

namespace Markdown {
    // Replace the buffer's contents with the rendered Markdown.
    void render(GtkTextBuffer* buffer, const std::string& source);
}
