// HintsColumns.cpp — the shortcut hints in columns (HintsColumns.h).
#include "HintsColumns.h"

#include "PageMeasure.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>

namespace {

enum Font { kKey = 0, kLabel = 1, kHeading = 2 };

// The most columns, for the widest screens at the smallest zoom (the Mac's).
const int kMaxColumns = 16;

// The Mac's colors: keys blue (theme::kHintsKeyColor), what they do light,
// states and headings muted.
const guint32 kLabelColor = 0xE0E0E0, kMutedColor = 0x9D9D9D;

void insert(PangoAttrList* list, PangoAttribute* a, size_t from, size_t to) {
    a->start_index = (guint)from;
    a->end_index = (guint)to;
    pango_attr_list_insert(list, a);
}

PangoAttribute* color(guint32 rgb) {
    return pango_attr_foreground_new((guint16)(((rgb >> 16) & 0xFF) * 257),
                                     (guint16)(((rgb >> 8) & 0xFF) * 257),
                                     (guint16)((rgb & 0xFF) * 257));
}

}  // namespace

HintsColumns::HintsColumns() {
    // Dark Modern's, until main.cpp gives the theme's.
    colors_ = {Rgba::hex(0xCE9178), Rgba::hex(0xC586C0), Rgba::hex(0xDCDCAA)};
    area_ = gtk_drawing_area_new();
    gtk_widget_add_css_class(area_, "minicode-hints-text");
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_), draw, this, nullptr);
    g_object_set_data_full(G_OBJECT(area_), "mutible-hints-columns", this, destroyed);
}

HintsColumns::~HintsColumns() {
    clearColumns();
    if (scratch_) g_object_unref(scratch_);
    for (PangoFontDescription* f : fonts_)
        if (f) pango_font_description_free(f);
}

void HintsColumns::destroyed(gpointer self) { delete static_cast<HintsColumns*>(self); }

void HintsColumns::clearColumns() {
    for (Column& c : columns_) {
        if (c.layout) g_object_unref(c.layout);
        if (c.base) pango_attr_list_unref(c.base);
    }
    columns_.clear();
}

std::string HintsColumns::text(int column) const {
    return column >= 0 && column < count() ? columns_[(size_t)column].text : std::string();
}

double HintsColumns::x(int column) const {
    return column >= 0 && column < count() ? columns_[(size_t)column].x : 0;
}

PangoLayout* HintsColumns::layout(int column) const {
    return column >= 0 && column < count() ? columns_[(size_t)column].layout : nullptr;
}

// The fonts for `points`: keys in the panel's monospace, labels in the
// desktop's font, headings in it at 10/12 of the size, semibold, spaced
// and in capitals, as the Mac's. A line is as tall as the taller of the
// first two, and a little more; its baseline leaves the room above and
// below the two fonts' tallest ascent and descent even.
void HintsColumns::setFonts(double points, double scale) {
    char* name = nullptr;
    if (GtkSettings* gs = gtk_settings_get_default()) g_object_get(gs, "gtk-font-name", &name, nullptr);
    PangoFontDescription* desktop = pango_font_description_from_string(name ? name : "Sans 10");
    g_free(name);
    const char* family = pango_font_description_get_family(desktop);
    const std::string desk = family && *family ? family : "Sans";
    pango_font_description_free(desktop);
    if (fonts_[kLabel] && points == points_ && scale == scale_ &&
        desk == pango_font_description_get_family(fonts_[kLabel]))
        return;
    points_ = points;
    scale_ = scale;
    for (auto& w : widths_) w.clear();
    for (PangoFontDescription*& f : fonts_) {
        if (f) pango_font_description_free(f);
        f = nullptr;
    }
    auto make = [](const std::string& fam, double pt, PangoWeight weight) {
        PangoFontDescription* f = pango_font_description_new();
        pango_font_description_set_family(f, fam.c_str());
        pango_font_description_set_size(f, (int)std::lround(pt * PANGO_SCALE));
        pango_font_description_set_weight(f, weight);
        return f;
    };
    fonts_[kKey] = make("Monospace", points, PANGO_WEIGHT_NORMAL);
    fonts_[kLabel] = make(desk, points, PANGO_WEIGHT_NORMAL);
    fonts_[kHeading] = make(desk, std::round(points * 10 / 12 * 2) / 2, PANGO_WEIGHT_SEMIBOLD);
    letterSpacing_ = 0.6 * scale;
    if (!scratch_) scratch_ = gtk_widget_create_pango_layout(area_, nullptr);
    double tallest = 0, ascent = 0, descent = 0;
    for (int f : {kKey, kLabel}) {
        pango_layout_set_attributes(scratch_, nullptr);
        pango_layout_set_font_description(scratch_, fonts_[f]);
        pango_layout_set_text(scratch_, "Ag", -1);
        PangoRectangle logical;
        pango_layout_get_extents(scratch_, nullptr, &logical);
        tallest = std::max(tallest, (double)logical.height / PANGO_SCALE);
        const double base = (double)pango_layout_get_baseline(scratch_) / PANGO_SCALE;
        ascent = std::max(ascent, base);
        descent = std::max(descent, (double)logical.height / PANGO_SCALE - base);
    }
    lineH_ = std::ceil(tallest + std::round(3 * scale));
    baseline_ = std::round((lineH_ - ascent - descent) / 2 + ascent);
}

// `text`'s width in `font` as drawn, whole pixels rounded up (a key's
// spaces half a cell, as keyAttributes draws them).
double HintsColumns::measure(int font, const std::string& text) {
    if (text.empty()) return 0;
    auto known = widths_[font].find(text);
    if (known != widths_[font].end()) return known->second;
    pango_layout_set_font_description(scratch_, fonts_[font]);
    PangoAttrList* attrs = nullptr;
    if (font == kHeading) {
        attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs,
                               pango_attr_letter_spacing_new((int)(letterSpacing_ * PANGO_SCALE)));
        pango_attr_list_insert(attrs, pango_attr_text_transform_new(PANGO_TEXT_TRANSFORM_UPPERCASE));
    } else if (font == kKey) {
        attrs = pango_attr_list_new();
        keyAttributes(attrs, text, 0, false);
    }
    pango_layout_set_attributes(scratch_, attrs);
    if (attrs) pango_attr_list_unref(attrs);
    pango_layout_set_text(scratch_, text.c_str(), (int)text.size());
    PangoRectangle logical;
    pango_layout_get_extents(scratch_, nullptr, &logical);
    const double w = std::ceil((double)logical.width / PANGO_SCALE);
    widths_[font][text] = w;
    return w;
}

// The thin space between a key's parts (Commands::kLinuxKeyJoin) or a
// space in one ("Page Down"): bytes at `i`, or 0.
static size_t spaceAt(const std::string& s, size_t i) {
    if (s[i] == ' ') return 1;
    return s.compare(i, 3, "\xE2\x80\x89") == 0 ? 3 : 0;
}

void HintsColumns::keyAttributes(PangoAttrList* list, const std::string& key, size_t at,
                                 bool colors) const {
    for (size_t i = 0; i < key.size();) {
        if (const size_t n = spaceAt(key, i)) {
            // Half a cell of the keys' monospace (4 pixels at 10 points):
            // a whole one spreads a key out, the desktop font's thin space
            // (2 pixels) ran "Ctrl Alt S" together.
            insert(list, pango_attr_scale_new(0.5), at + i, at + i + n);
            i += n;
            continue;
        }
        size_t j = i;
        while (j < key.size() && !spaceAt(key, j)) ++j;
        const std::string word = key.substr(i, j - i);
        const Rgba* c = word == "Ctrl"    ? &colors_.ctrl
                        : word == "Alt"   ? &colors_.alt
                        : word == "Super" ? &colors_.super
                                          : nullptr;
        if (c && colors) insert(list, color((guint32)(c->r << 16 | c->g << 8 | c->b)), at + i, at + j);
        i = j;
    }
}

void HintsColumns::show(const std::vector<HintRow>& rows, double room, double points,
                        double scale, const std::string& query) {
    using Kind = PageMeasure::HintItem::Kind;
    setFonts(points, scale);
    given_ = rows;
    room_ = std::max(room, 40.0);

    // The rows as the core's items: a row with no key goes with the one
    // before it, a line of text is kept with what follows it as a heading
    // is, and the blank line before the last row is a gap.
    items_.clear();
    measured_.clear();
    for (size_t i = 0; i < given_.size(); ++i) {
        const HintRow& r = given_[i];
        if (r.kind == HintRow::Row && r.key.empty() && !measured_.empty() &&
            measured_.back().kind == Kind::Shortcut) {
            items_.back().push_back(i);
            measured_.back().height += lineH_;
            measured_.back().label = std::max(measured_.back().label, measure(kLabel, r.text));
            continue;
        }
        PageMeasure::HintItem m;
        if (r.kind == HintRow::Heading) {
            m.kind = Kind::Heading;
            m.key = measure(kHeading, r.text);
            m.height = lineH_;
            m.spaceBefore = lineH_;
        } else if (r.kind == HintRow::Text && r.text.empty()) {
            m.kind = Kind::Gap;
            m.spaceBefore = lineH_;
        } else if (r.kind == HintRow::Text) {
            m.kind = Kind::Heading;
            m.key = measure(kLabel, r.text);
            m.height = lineH_;
        } else {
            m.key = measure(kKey, r.key);
            m.label = measure(kLabel, r.text);
            m.hasState = !r.state.empty();
            m.state = measure(kLabel, r.state);
            m.height = lineH_;
        }
        items_.push_back({i});
        measured_.push_back(m);
    }
    // The list as it is without a query, whose columns and size a query's
    // keeps to.
    const PageMeasure::HintsLayout plain =
        PageMeasure::layoutHints(measured_, room_, 12 * scale_, 28 * scale_, kMaxColumns);
    plainCount_ = plain.count();
    plainWidth_ = (int)std::ceil(plain.textWidth);
    plainHeight_ = plain.wraps ? 0 : (int)std::ceil(plain.textHeight);
    query_ = query;
    order_ = orderFor(query_);
    layOut();
    applyMarks();
}

HintsSearch::Bubbled HintsColumns::orderFor(const std::string& query) const {
    HintsSearch::Bubbled b;
    const std::vector<bool> found = HintsSearchBox::found(given_, query);
    if (found.empty()) {
        for (size_t i = 0; i < items_.size(); ++i) b.order.push_back(i);
        b.rest = b.order.size();
        return b;
    }
    auto isLine = [this](size_t item) {
        const HintRow& r = given_[items_[item].front()];
        return r.kind == HintRow::Text && !r.text.empty();
    };
    std::vector<HintsSearch::Item> list;
    list.reserve(items_.size());
    for (size_t i = 0; i < items_.size(); ++i) {
        const HintRow& r = given_[items_[i].front()];
        HintsSearch::Item it;
        if (r.kind == HintRow::Heading) {
            it.kind = HintsSearch::Item::Heading;
        } else if (r.kind == HintRow::Text && r.text.empty()) {
            it.kind = HintsSearch::Item::Gap;
        } else if (r.kind == HintRow::Text) {
            // A note's lines go together.
            it.kind = HintsSearch::Item::Line;
            it.withNext = i + 1 < items_.size() && isLine(i + 1);
        }
        for (size_t ri : items_[i]) it.matches = it.matches || found[ri];
        list.push_back(it);
    }
    return HintsSearch::bubbleMatches(list);
}

void HintsColumns::layOut() {
    using Kind = PageMeasure::HintItem::Kind;
    ++layouts_;
    bool reordered = false;
    for (size_t p = 0; p < order_.order.size(); ++p) reordered = reordered || order_.order[p] != p;

    // The items in their order, and their rows, a heading that is there
    // twice a row each time.
    std::vector<PageMeasure::HintItem> measured;
    std::vector<std::vector<size_t>> units;   // each place's rows in `placed`
    std::vector<HintRow> placed;
    std::vector<size_t> rowAt;
    for (size_t p = 0; p < order_.order.size(); ++p) {
        const size_t item = order_.order[p];
        measured.push_back(measured_[item]);
        units.emplace_back();
        for (size_t ri : items_[item]) {
            HintRow r = given_[ri];
            r.column = -1;
            r.start = r.textAt = r.stateAt = r.end = 0;
            r.rest = p >= order_.rest;
            units.back().push_back(placed.size());
            placed.push_back(std::move(r));
            rowAt.push_back(p);
        }
    }
    const PageMeasure::HintsLayout layout = PageMeasure::layoutHints(
        measured, room_, 12 * scale_, 28 * scale_, reordered ? plainCount_ : kMaxColumns);
    wraps_ = layout.wraps;

    // Each column's text, a line a row, and its fonts and colors.
    clearColumns();
    int tallest = 0;
    for (int c = 0; c < layout.count(); ++c) {
        const PageMeasure::HintsColumn& col = layout.columns[(size_t)c];
        Column out;
        out.x = col.x;
        std::string& s = out.text;
        PangoAttrList* attrs = pango_attr_list_new();
        auto style = [&](size_t from, int font, guint32 rgb) {
            if (from >= s.size()) return;
            insert(attrs, pango_attr_font_desc_new(fonts_[font]), from, s.size());
            insert(attrs, color(rgb), from, s.size());
            if (font == kHeading) {
                insert(attrs, pango_attr_letter_spacing_new((int)(letterSpacing_ * PANGO_SCALE)),
                       from, s.size());
                insert(attrs, pango_attr_text_transform_new(PANGO_TEXT_TRANSFORM_UPPERCASE), from,
                       s.size());
            }
        };
        bool begun = false;
        auto line = [&] {
            if (begun) s += "\n";
            begun = true;
        };
        for (size_t i = col.first; i < col.end; ++i) {
            // A blank line above a heading or the last row's gap, except at
            // the column's top.
            if (i != col.first && measured[i].spaceBefore > 0) line();
            for (size_t pi : units[i]) {
                HintRow& r = placed[pi];
                r.column = c;
                if (measured[i].kind == Kind::Gap) continue;   // its blank line
                line();
                if (r.kind == HintRow::Row) {
                    r.start = s.size();
                    s += r.key;
                    style(r.start, kKey, theme::kHintsKeyColor);
                    keyAttributes(attrs, r.key, r.start, true);
                    s += "\t";
                    r.textAt = s.size();
                    s += r.text;
                    style(r.textAt, kLabel, kLabelColor);
                    if (!r.state.empty()) {
                        s += wraps_ ? " " : "\t";
                        r.stateAt = s.size();
                        s += r.state;
                        style(r.stateAt, kLabel, kMutedColor);
                    }
                } else {
                    r.start = r.textAt = s.size();
                    s += r.text;
                    style(r.start, r.kind == HintRow::Heading ? kHeading : kLabel, kMutedColor);
                }
                r.end = s.size();
            }
        }
        out.layout = gtk_widget_create_pango_layout(area_, nullptr);
        pango_layout_set_font_description(out.layout, fonts_[kLabel]);
        pango_layout_set_text(out.layout, s.c_str(), (int)s.size());
        PangoTabArray* tabs = pango_tab_array_new(wraps_ ? 1 : 2, TRUE);
        pango_tab_array_set_tab(tabs, 0, PANGO_TAB_LEFT, (int)col.label);
        if (!wraps_) pango_tab_array_set_tab(tabs, 1, PANGO_TAB_LEFT, (int)col.state);
        pango_layout_set_tabs(out.layout, tabs);
        pango_tab_array_free(tabs);
        if (wraps_) {
            // Labels wrap under themselves: every line but a row's first
            // starts at the label's stop.
            pango_layout_set_width(out.layout, (int)(layout.textWidth * PANGO_SCALE));
            pango_layout_set_wrap(out.layout, PANGO_WRAP_WORD_CHAR);
            pango_layout_set_indent(out.layout, -(int)(col.label * PANGO_SCALE));
        }
        pango_layout_set_attributes(out.layout, attrs);
        out.base = attrs;
        tallest = std::max(tallest, (int)(pango_layout_get_line_count(out.layout) * lineH_));
        columns_.push_back(out);
    }
    rows_ = std::move(placed);
    rowAt_ = std::move(rowAt);
    // Reordered, the panel keeps at least its size without a query, so
    // that it neither shrinks nor jumps as the query changes.
    width_ = std::max((int)std::ceil(layout.textWidth), reordered ? plainWidth_ : 0);
    height_ = std::max(tallest, reordered ? plainHeight_ : 0);
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(area_), std::max(1, width_));
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area_), std::max(1, height_));
}

double HintsColumns::mark(const std::string& query) {
    if (query == query_) return applyMarks();
    query_ = query;
    HintsSearch::Bubbled b = orderFor(query_);
    if (b.order != order_.order) {
        // What the query finds changed: laid out again.
        order_ = std::move(b);
        layOut();
    } else if (b.rest != order_.rest) {
        // The same order, nothing found or everything (or the query
        // emptied with nothing found before): only the rows dimmed change.
        order_.rest = b.rest;
        for (size_t i = 0; i < rows_.size(); ++i) rows_[i].rest = rowAt_[i] >= order_.rest;
    }
    return applyMarks();
}

double HintsColumns::applyMarks() {
    std::vector<PangoAttrList*> lists;
    for (const Column& c : columns_) lists.push_back(pango_attr_list_copy(c.base));
    std::vector<std::pair<int, size_t>> hits;
    HintsSearchBox::attributes(rows_, query_, lists, &hits);
    for (size_t c = 0; c < columns_.size(); ++c) {
        pango_layout_set_attributes(columns_[c].layout, lists[c]);
        pango_attr_list_unref(lists[c]);
    }
    double top = -1;
    for (const auto& [c, at] : hits) {
        const double y = lineTop(c, (int)at);
        if (top < 0 || y < top) top = y;
    }
    gtk_widget_queue_draw(area_);
    return top;
}
double HintsColumns::lineTop(int column, int index) const {
    PangoLayout* l = layout(column);
    if (!l) return 0;
    int line = 0, x = 0;
    pango_layout_index_to_line_x(l, index, FALSE, &line, &x);
    return line * lineH_;
}

// A line at a time, line n's top n line heights down, at its own left
// edge (a wrapped label's lines at the label's stop).
void HintsColumns::draw(GtkDrawingArea*, cairo_t* cr, int, int, gpointer self) {
    auto* me = static_cast<HintsColumns*>(self);
    cairo_set_source_rgb(cr, ((kLabelColor >> 16) & 0xFF) / 255.0,
                         ((kLabelColor >> 8) & 0xFF) / 255.0, (kLabelColor & 0xFF) / 255.0);
    for (const Column& c : me->columns_) {
        PangoLayoutIter* it = pango_layout_get_iter(c.layout);
        int n = 0;
        do {
            PangoRectangle logical;
            pango_layout_iter_get_line_extents(it, nullptr, &logical);
            cairo_move_to(cr, c.x + (double)logical.x / PANGO_SCALE, n * me->lineH_ + me->baseline_);
            pango_cairo_show_layout_line(cr, pango_layout_iter_get_line_readonly(it));
            ++n;
        } while (pango_layout_iter_next_line(it));
        pango_layout_iter_free(it);
    }
}
