// MultiEdit.cpp — see MultiEdit.h.
#include "MultiEdit.h"

#include "EditSpans.h"

namespace MultiEdit {

namespace {
int gDepth = 0;
}  // namespace

bool applying() { return gDepth > 0; }

bool apply(GtkTextBuffer* buffer, const std::vector<Change>& changes, bool keepCaret) {
    if (!buffer || changes.empty()) return false;
    const std::size_t size = (std::size_t)gtk_text_buffer_get_char_count(buffer);
    std::vector<EditSpans::Span> spans;
    spans.reserve(changes.size());
    for (const Change& c : changes) {
        if (!g_utf8_validate(c.text.data(), (gssize)c.text.size(), nullptr)) return false;
        spans.push_back({c.start, c.length,
                         (std::size_t)g_utf8_strlen(c.text.data(), (gssize)c.text.size())});
    }
    if (!EditSpans::valid(spans, size)) return false;

    GtkTextIter ins, bound;
    gtk_text_buffer_get_iter_at_mark(buffer, &ins, gtk_text_buffer_get_insert(buffer));
    gtk_text_buffer_get_iter_at_mark(buffer, &bound, gtk_text_buffer_get_selection_bound(buffer));
    const std::size_t insAt = (std::size_t)gtk_text_iter_get_offset(&ins);
    const std::size_t boundAt = (std::size_t)gtk_text_iter_get_offset(&bound);

    gDepth++;
    gtk_text_buffer_begin_user_action(buffer);
    // No selection while the changes go in: with one, every delete offers
    // it to the primary clipboard again (update_selection_clipboards),
    // which under X is a round trip to the server, about 1.4 ms each: a
    // Replace All of 40,000 matches took 58 s of deletes (the find_bar
    // suite's bound caught it). It comes back, carried through, below.
    if (insAt != boundAt) gtk_text_buffer_place_cursor(buffer, &ins);
    for (std::size_t k = changes.size(); k-- > 0;) {
        const Change& c = changes[k];
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buffer, &a, (int)c.start);
        if (c.length) {
            gtk_text_buffer_get_iter_at_offset(buffer, &b, (int)(c.start + c.length));
            gtk_text_buffer_delete(buffer, &a, &b);   // a is revalidated at the place
        }
        if (!c.text.empty()) gtk_text_buffer_insert(buffer, &a, c.text.data(), (int)c.text.size());
    }
    GtkTextIter at, other;
    gtk_text_buffer_get_iter_at_offset(buffer, &at, (int)EditSpans::mapThrough(insAt, spans));
    if (keepCaret || insAt == boundAt) {
        gtk_text_buffer_place_cursor(buffer, &at);
    } else {
        gtk_text_buffer_get_iter_at_offset(buffer, &other, (int)EditSpans::mapThrough(boundAt, spans));
        gtk_text_buffer_select_range(buffer, &at, &other);
    }
    gtk_text_buffer_end_user_action(buffer);
    gDepth--;
    return true;
}

}  // namespace MultiEdit
