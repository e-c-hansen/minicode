// Markdown.cpp — see Markdown.h. Mirrors the run-to-attributes mapping in the
// macOS EditorController markdown renderer (headings white+bold+larger, code in
// #CE9178 on #2A2A2A monospace, blockquotes gray+indented, rules as a box-draw
// line, tables monospace, links blue+underlined).
#include "Markdown.h"
#include "MarkdownParser.h"

namespace {
// Apply a named tag to the [startOffset, endOffset) character range.
void applyTag(GtkTextBuffer* buf, const char* name, int startOffset, int endOffset) {
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(buf, &a, startOffset);
    gtk_text_buffer_get_iter_at_offset(buf, &b, endOffset);
    gtk_text_buffer_apply_tag_by_name(buf, name, &a, &b);
}
} // namespace

void Markdown::render(GtkTextBuffer* buffer, const std::string& source) {
    gtk_text_buffer_set_text(buffer, "", 0);

    std::vector<MdRun> runs = MarkdownParser::parse(source);

    for (const MdRun& r : runs) {
        std::string text = r.text;
        // A horizontal rule renders as a full line of box-drawing characters,
        // matching the macOS build.
        // 32 box-drawing characters, the same width the macOS build draws.
        if (r.rule) text = "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
                           "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
                           "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
                           "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
                           "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
                           "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
                           "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
                           "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\n";
        if (text.empty()) continue;

        // Insert at end, remembering the char offset span we just added.
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer, &end);
        int startOff = gtk_text_buffer_get_char_count(buffer);
        gtk_text_buffer_insert(buffer, &end, text.c_str(), (int)text.size());
        int endOff = gtk_text_buffer_get_char_count(buffer);

        // Layer tags. Order roughly follows the macOS precedence.
        if (r.heading > 0 && r.heading <= 6) {
            char name[8];
            g_snprintf(name, sizeof(name), "h%d", r.heading);
            applyTag(buffer, name, startOff, endOff);
        }
        if (r.codeBlock || r.code) applyTag(buffer, "md_code",  startOff, endOff);
        if (r.table)               applyTag(buffer, "md_table", startOff, endOff);
        if (r.quote)               applyTag(buffer, "md_quote", startOff, endOff);
        if (r.rule)                applyTag(buffer, "md_rule",  startOff, endOff);
        if (r.link)                applyTag(buffer, "md_link",  startOff, endOff);
        if (r.bold)                applyTag(buffer, "md_bold",  startOff, endOff);
        if (r.italic)              applyTag(buffer, "md_italic", startOff, endOff);
    }
}
