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

// Longest logical line (one Pango paragraph) we will hand GtkTextView.
//
// MarkdownParser deliberately joins every consecutive non-blank line of a
// paragraph into one logical line, because that is what lets the paragraph
// reflow to the window width instead of keeping the source's hard wrapping.
// GtkTextView lays out each logical line as a single PangoLayout, and past a
// few hundred thousand characters that layout fails outright: the paragraph is
// measured as zero-height and simply does not render, so a big document opens
// to a blank preview. Wrapping does not save it -- the limit is on the length
// of the paragraph, not its on-screen width.
//
// Breaking the logical line every so often costs nothing on real documents (no
// hand-written paragraph comes close) and keeps generated or minified Markdown
// readable rather than invisible.
const int kMaxLogicalLine = 32000;

// Rewrite `text` so that, appended to a logical line already `lineLen`
// characters long, no logical line exceeds kMaxLogicalLine. Breaks are made at
// a space where one is available on the current line, otherwise at the nearest
// UTF-8 character boundary. `lineLen` is left holding the length of the
// trailing logical line. Real documents never trip this, so the common path is
// a plain copy.
std::string boundLogicalLines(const std::string& text, int& lineLen) {
    const long chars = g_utf8_strlen(text.c_str(), (gssize)text.size());
    if (lineLen + chars <= kMaxLogicalLine &&
        text.find('\n') == std::string::npos) {
        lineLen += (int)chars;
        return text;
    }

    std::string out;
    out.reserve(text.size() + 16);
    size_t lastSpace = std::string::npos;   // byte index in `out`, or npos
    const char* p = text.c_str();
    const char* end = p + text.size();
    while (p < end) {
        const char* next = g_utf8_find_next_char(p, end);
        if (!next || next <= p) next = p + 1;   // defensive: never stall

        if (*p == '\n') {
            out.append(p, next - p);
            lineLen = 0;
            lastSpace = std::string::npos;
        } else {
            if (lineLen >= kMaxLogicalLine) {
                if (lastSpace != std::string::npos) {
                    out[lastSpace] = '\n';      // break at the last space
                    lineLen = (int)g_utf8_strlen(out.c_str() + lastSpace + 1, -1);
                } else {
                    out.push_back('\n');        // no space to break at
                    lineLen = 0;
                }
                lastSpace = std::string::npos;
            }
            if (*p == ' ') lastSpace = out.size();
            out.append(p, next - p);
            lineLen++;
        }
        p = next;
    }
    return out;
}
} // namespace

void Markdown::render(GtkTextBuffer* buffer, const std::string& source) {
    gtk_text_buffer_set_text(buffer, "", 0);

    std::vector<MdRun> runs = MarkdownParser::parse(source);

    // Characters emitted since the last newline, i.e. the length of the logical
    // line being built up.
    int lineLen = 0;

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

        text = boundLogicalLines(text, lineLen);

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
