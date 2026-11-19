// HintsSearchBox.cpp — the shortcut hints' search box (HintsSearchBox.h).
#include "HintsSearchBox.h"

#include "HintsSearch.h"

#include <algorithm>
#include <cstdint>

namespace {

// `s` in UTF-16, as HintsSearch reads it, and the byte each unit starts at
// (one more for the end), so a span found maps back to the text.
std::u16string utf16(const std::string& s, std::vector<size_t>* byteAt) {
    std::u16string out;
    if (byteAt) byteAt->clear();
    const char* p = s.c_str();
    const char* end = p + s.size();
    while (p < end) {
        const gunichar c = g_utf8_get_char_validated(p, end - p);
        const char* next = (c == (gunichar)-1 || c == (gunichar)-2) ? p + 1 : g_utf8_next_char(p);
        const size_t at = (size_t)(p - s.c_str());
        if (c == (gunichar)-1 || c == (gunichar)-2) {
            out += u'�';
            if (byteAt) byteAt->push_back(at);
        } else if (c >= 0x10000) {
            out += (char16_t)(0xD800 + ((c - 0x10000) >> 10));
            out += (char16_t)(0xDC00 + ((c - 0x10000) & 0x3FF));
            if (byteAt) byteAt->insert(byteAt->end(), 2, at);
        } else {
            out += (char16_t)c;
            if (byteAt) byteAt->push_back(at);
        }
        p = next;
    }
    if (byteAt) byteAt->push_back(s.size());
    return out;
}

void addDim(PangoAttrList* list, size_t from, size_t to) {
    if (to <= from) return;
    PangoAttribute* a = pango_attr_foreground_alpha_new(19661);   // 0.3, as the Mac's
    a->start_index = (guint)from;
    a->end_index = (guint)to;
    pango_attr_list_insert(list, a);
}

// The Mac's mark: #EA5C00 at half strength behind the text.
void addMark(PangoAttrList* list, size_t from, size_t to) {
    if (to <= from) return;
    PangoAttribute* bg = pango_attr_background_new(0xEAEA, 0x5C5C, 0x0000);
    bg->start_index = (guint)from;
    bg->end_index = (guint)to;
    pango_attr_list_insert(list, bg);
    PangoAttribute* alpha = pango_attr_background_alpha_new(32768);
    alpha->start_index = (guint)from;
    alpha->end_index = (guint)to;
    pango_attr_list_insert(list, alpha);
}

}  // namespace

HintsSearchBox::HintsSearchBox() {
    entry_ = gtk_search_entry_new();
    g_object_set(entry_, "placeholder-text", "Type to search the shortcuts", nullptr);
    gtk_widget_add_css_class(entry_, "minicode-hints-search");
    g_object_set_data_full(G_OBJECT(entry_), "mutible-hints-search-box", this, destroyed);
    g_signal_connect(entry_, "changed", G_CALLBACK(changed), this);
    GtkEventController* focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(focusLeft), this);
    gtk_widget_add_controller(entry_, focus);
}

HintsSearchBox::~HintsSearchBox() { setPrevious(nullptr); }

void HintsSearchBox::destroyed(gpointer self) { delete static_cast<HintsSearchBox*>(self); }

std::string HintsSearchBox::query() const {
    const char* t = gtk_editable_get_text(GTK_EDITABLE(entry_));
    return t ? t : "";
}

void HintsSearchBox::changed(GtkEditable*, gpointer self) {
    auto* box = static_cast<HintsSearchBox*>(self);
    const std::string q = box->query();
    if (q == box->last_) return;
    box->last_ = q;
    if (box->onChange) box->onChange();
}

// The keyboard left the box (a click elsewhere): where it came from no
// longer says where it goes back to.
void HintsSearchBox::focusLeft(GtkEventControllerFocus*, gpointer self) {
    static_cast<HintsSearchBox*>(self)->setPrevious(nullptr);
}

void HintsSearchBox::clear() {
    if (!query().empty()) gtk_editable_set_text(GTK_EDITABLE(entry_), "");
}

GtkWidget* HintsSearchBox::text() const {
    GtkEditable* d = gtk_editable_get_delegate(GTK_EDITABLE(entry_));
    return d ? GTK_WIDGET(d) : entry_;
}

void HintsSearchBox::setPrevious(GtkWidget* w) {
    if (previous_ == w) return;
    if (previous_) g_object_remove_weak_pointer(G_OBJECT(previous_), (gpointer*)&previous_);
    previous_ = w;
    if (previous_) g_object_add_weak_pointer(G_OBJECT(previous_), (gpointer*)&previous_);
}

bool HintsSearchBox::hasKeyboard() const {
    GtkRoot* root = gtk_widget_get_root(entry_);
    if (!root) return false;
    GtkWidget* focus = gtk_root_get_focus(root);
    return focus && (focus == entry_ || gtk_widget_is_ancestor(focus, entry_));
}

// Esc: the box's text first, then the panel.
void HintsSearchBox::escape() {
    if (!query().empty()) clear();
    else if (onClose) onClose();
}

bool HintsSearchBox::takeKey(GtkEventControllerKey* controller, guint keyval,
                             GdkModifierType state) {
    // The commands' Ctrl keys stay theirs (Ctrl+Shift+H closes the panel
    // through its accelerator), and so does anything with Super.
    if (state & (GDK_CONTROL_MASK | GDK_SUPER_MASK | GDK_HYPER_MASK)) return false;
    GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
    if (event && gdk_key_event_is_modifier(event)) return false;
    if ((keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F35) || keyval == GDK_KEY_Menu) return false;
    if (keyval == GDK_KEY_Escape) {
        escape();
        return true;
    }
    // Return and Tab neither end the search nor move the keyboard on.
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter || keyval == GDK_KEY_ISO_Enter ||
        keyval == GDK_KEY_Tab || keyval == GDK_KEY_KP_Tab || keyval == GDK_KEY_ISO_Left_Tab)
        return true;
    GtkWidget* t = text();
    if (!hasKeyboard()) {
        GtkRoot* root = gtk_widget_get_root(entry_);
        if (!root || !gtk_widget_get_mapped(entry_)) return false;
        GtkWidget* from = gtk_root_get_focus(root);
        const bool took = GTK_IS_TEXT(t) ? gtk_text_grab_focus_without_selecting(GTK_TEXT(t))
                                         : gtk_widget_grab_focus(t);
        if (!took) return false;
        setPrevious(from);
        // The key goes after what the box holds.
        gtk_editable_set_position(GTK_EDITABLE(entry_), -1);
    }
    if (state & GDK_ALT_MASK) {
        // An input method types nothing for Alt+letter: the box types the
        // letter (Shift's case kept), as the Mac's box types Option's.
        const gunichar c = gdk_keyval_to_unicode(keyval);
        if (c && !g_unichar_iscntrl(c)) {
            char utf8[8];
            const int n = g_unichar_to_utf8(c, utf8);
            GtkEditable* e = GTK_EDITABLE(entry_);
            gtk_editable_delete_selection(e);
            int pos = gtk_editable_get_position(e);
            gtk_editable_insert_text(e, utf8, n, &pos);
            gtk_editable_set_position(e, pos);
            return true;
        }
    }
    gtk_event_controller_key_forward(controller, t);
    return true;
}

bool HintsSearchBox::giveBackKeyboard() {
    if (!hasKeyboard()) return true;
    GtkWidget* p = previous_;
    setPrevious(nullptr);
    if (p && gtk_widget_get_root(p) == gtk_widget_get_root(entry_) && gtk_widget_get_mapped(p) &&
        gtk_widget_grab_focus(p))
        return true;
    return false;
}

PangoAttrList* HintsSearchBox::attributes(const std::vector<HintRow>& rows,
                                          const std::string& query, int* firstHit) {
    if (firstHit) *firstHit = -1;
    const std::vector<HintsSearch::Word> words =
        HintsSearch::parse(utf16(query, nullptr), HintsSearch::Spelling::Words);
    if (words.empty()) return nullptr;
    PangoAttrList* list = pango_attr_list_new();
    std::vector<bool> found(rows.size(), false);
    size_t first = SIZE_MAX;
    std::vector<size_t> keyAt, textAt;
    for (size_t i = 0; i < rows.size(); ++i) {
        const HintRow& r = rows[i];
        if (r.kind == HintRow::Heading) continue;
        if (r.kind == HintRow::Text && r.text.empty()) continue;   // a blank line
        std::vector<HintsSearch::Field> fields;
        if (r.kind == HintRow::Row) fields.push_back({utf16(r.key, &keyAt), true});
        fields.push_back({utf16(r.text, &textAt), false});
        const HintsSearch::Match m = HintsSearch::match(words, fields);
        if (!m.matches) {
            addDim(list, r.start, r.end);
            continue;
        }
        found[i] = true;
        for (const HintsSearch::Span& sp : m.spans) {
            const bool inKey = r.kind == HintRow::Row && sp.field == 0;
            const std::vector<size_t>& at = inKey ? keyAt : textAt;
            if (sp.start + sp.length >= at.size()) continue;
            const size_t base = inKey ? r.start : r.textAt;
            const size_t from = base + at[sp.start], to = base + at[sp.start + sp.length];
            addMark(list, from, to);
            first = std::min(first, from);
        }
    }
    // A heading dims when nothing under it was found.
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].kind != HintRow::Heading) continue;
        bool lit = false;
        for (size_t j = i + 1; j < rows.size() && rows[j].kind != HintRow::Heading; ++j)
            lit = lit || found[j];
        if (!lit) addDim(list, rows[i].start, rows[i].end);
    }
    if (firstHit && first != SIZE_MAX) *firstHit = (int)first;
    return list;
}
