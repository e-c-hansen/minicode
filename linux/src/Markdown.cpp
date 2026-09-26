// Markdown.cpp — see Markdown.h. Mirrors the run-to-attributes mapping in the
// macOS EditorController markdown renderer (headings white+bold+larger, code in
// #CE9178 on #2A2A2A monospace, blockquotes gray+indented, rules as a box-draw
// line, links blue+underlined), and like it draws tables as real tables and
// shows local pictures, GIFs playing.
#include "Markdown.h"
#include "MarkdownParser.h"
#include "Palette.h"

#include <sys/stat.h>
#include <algorithm>
#include <memory>

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

// ------------------------------------------------------------- MdPicture
//
// A picture drawn at the size fit() gives it. GtkPicture always asks for its
// image's own size, which a text view then gives it, so a 960-pixel GIF ran
// off a narrower pane. A GIF's frames are stepped here while it is mapped,
// as MediaView does (gdk-pixbuf's animation API is deprecated with no
// replacement, hence the silenced warnings).

G_DECLARE_FINAL_TYPE(MdPicture, md_picture, MD, PICTURE, GtkWidget)

struct _MdPicture {
    GtkWidget parent_instance;
    GdkTexture* texture;
    int width, height;
    GdkPixbufAnimation* anim;
    GdkPixbufAnimationIter* iter;
    guint timer;
};

G_DEFINE_FINAL_TYPE(MdPicture, md_picture, GTK_TYPE_WIDGET)

// A GdkPixbuf frame as a texture (pixbuf pixels are straight RGB(A)).
GdkTexture* textureFromPixbuf(GdkPixbuf* pb) {
    const int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    const int stride = gdk_pixbuf_get_rowstride(pb);
    const bool alpha = gdk_pixbuf_get_has_alpha(pb);
    const gsize len = (gsize)stride * (h - 1) + (gsize)w * (alpha ? 4 : 3);
    GBytes* bytes = g_bytes_new(gdk_pixbuf_read_pixels(pb), len);
    GdkTexture* t = gdk_memory_texture_new(
        w, h, alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8, bytes, stride);
    g_bytes_unref(bytes);
    return t;
}

void mdPictureStop(MdPicture* p) {
    if (p->timer) g_source_remove(p->timer);
    p->timer = 0;
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
gboolean mdPictureTick(gpointer data) {
    MdPicture* p = MD_PICTURE(data);
    p->timer = 0;
    if (!p->iter) return G_SOURCE_REMOVE;
    gdk_pixbuf_animation_iter_advance(p->iter, nullptr);
    GdkTexture* tex = textureFromPixbuf(gdk_pixbuf_animation_iter_get_pixbuf(p->iter));
    g_set_object(&p->texture, tex);
    g_object_unref(tex);
    gtk_widget_queue_draw(GTK_WIDGET(p));
    const int delay = gdk_pixbuf_animation_iter_get_delay_time(p->iter);
    if (delay >= 0) p->timer = g_timeout_add(std::max(delay, 20), mdPictureTick, p);
    return G_SOURCE_REMOVE;
}

void mdPictureStart(MdPicture* p) {
    if (!p->iter || p->timer) return;
    const int delay = gdk_pixbuf_animation_iter_get_delay_time(p->iter);
    if (delay >= 0) p->timer = g_timeout_add(std::max(delay, 20), mdPictureTick, p);
}
G_GNUC_END_IGNORE_DEPRECATIONS

void md_picture_init(MdPicture* p) {
    p->texture = nullptr;
    p->width = p->height = 1;
    p->anim = nullptr;
    p->iter = nullptr;
    p->timer = 0;
}

void mdPictureDispose(GObject* o) {
    MdPicture* p = MD_PICTURE(o);
    mdPictureStop(p);
    g_clear_object(&p->iter);
    g_clear_object(&p->anim);
    g_clear_object(&p->texture);
    G_OBJECT_CLASS(md_picture_parent_class)->dispose(o);
}

void mdPictureMeasure(GtkWidget* w, GtkOrientation o, int, int* min, int* nat, int*, int*) {
    MdPicture* p = MD_PICTURE(w);
    *min = *nat = o == GTK_ORIENTATION_HORIZONTAL ? p->width : p->height;
}

void mdPictureSnapshot(GtkWidget* w, GtkSnapshot* s) {
    MdPicture* p = MD_PICTURE(w);
    if (!p->texture) return;
    graphene_rect_t r = GRAPHENE_RECT_INIT(0, 0, (float)gtk_widget_get_width(w),
                                           (float)gtk_widget_get_height(w));
    gtk_snapshot_append_scaled_texture(s, p->texture, GSK_SCALING_FILTER_TRILINEAR, &r);
}

void mdPictureMap(GtkWidget* w) {
    GTK_WIDGET_CLASS(md_picture_parent_class)->map(w);
    mdPictureStart(MD_PICTURE(w));
}

void mdPictureUnmap(GtkWidget* w) {
    mdPictureStop(MD_PICTURE(w));
    GTK_WIDGET_CLASS(md_picture_parent_class)->unmap(w);
}

void md_picture_class_init(MdPictureClass* k) {
    G_OBJECT_CLASS(k)->dispose = mdPictureDispose;
    GtkWidgetClass* wk = GTK_WIDGET_CLASS(k);
    wk->measure = mdPictureMeasure;
    wk->snapshot = mdPictureSnapshot;
    wk->map = mdPictureMap;
    wk->unmap = mdPictureUnmap;
}

// The picture `src` names, as written in a Markdown file: a path relative to
// `folder`, an absolute or ~ path, or a file:// URL. Web pictures and files
// that do not decode give null, and the alt text is shown instead.
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
GtkWidget* loadPicture(const std::string& srcIn, const std::string& folder,
                       int* width, int* height) {
    std::string src = srcIn, path;
    if (src.rfind("file://", 0) == 0) {
        char* f = g_filename_from_uri(src.c_str(), nullptr, nullptr);
        if (!f) return nullptr;
        path = f;
        g_free(f);
    } else {
        if (src.find("://") != std::string::npos || src.rfind("data:", 0) == 0)
            return nullptr;   // no network fetches from a preview
        const size_t cut = src.find_first_of("?#");
        if (cut != std::string::npos) src = src.substr(0, cut);
        char* un = g_uri_unescape_string(src.c_str(), nullptr);
        path = un ? un : src;
        g_free(un);
        if (path.rfind("~/", 0) == 0) path = std::string(g_get_home_dir()) + path.substr(1);
        if (!path.empty() && path[0] != '/') path = folder + "/" + path;
    }
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size > 64ll * 1024 * 1024)
        return nullptr;

    MdPicture* p = MD_PICTURE(g_object_new(md_picture_get_type(), nullptr));
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".gif") == 0) {
        GdkPixbufAnimation* anim = gdk_pixbuf_animation_new_from_file(path.c_str(), nullptr);
        if (anim && !gdk_pixbuf_animation_is_static_image(anim)) {
            p->anim = anim;
            p->iter = gdk_pixbuf_animation_get_iter(anim, nullptr);
            p->texture = textureFromPixbuf(gdk_pixbuf_animation_iter_get_pixbuf(p->iter));
        } else if (anim) {
            g_object_unref(anim);
        }
    }
    if (!p->texture) p->texture = gdk_texture_new_from_filename(path.c_str(), nullptr);
    if (!p->texture) {
        g_object_ref_sink(p);
        g_object_unref(p);
        return nullptr;
    }
    *width = p->width = gdk_texture_get_width(p->texture);
    *height = p->height = gdk_texture_get_height(p->texture);
    return GTK_WIDGET(p);
}
G_GNUC_END_IGNORE_DEPRECATIONS

void mdPictureSetSize(GtkWidget* w, int width, int height) {
    MdPicture* p = MD_PICTURE(w);
    if (p->width == width && p->height == height) return;
    p->width = std::max(1, width);
    p->height = std::max(1, height);
    gtk_widget_queue_resize(w);
}

// ------------------------------------------------------------- tables

struct CellInfo {
    std::shared_ptr<Markdown::Hooks> hooks;
    int line, row, column;
};

std::string escaped(const std::string& s) {
    char* e = g_markup_escape_text(s.c_str(), (gssize)s.size());
    std::string out = e ? e : "";
    g_free(e);
    return out;
}

// One cell's runs as Pango markup: bold, italic, code and links.
std::string cellMarkup(const std::vector<const MdRun*>& runs) {
    std::string m;
    for (const MdRun* r : runs) {
        std::string t = escaped(r->image && r->text.empty() ? r->src : r->text);
        if (r->code)
            t = std::string("<span font_family=\"monospace\" foreground=\"") + pal::MdCode +
                "\" background=\"" + pal::MdCodeBg + "\">" + t + "</span>";
        if (r->italic) t = "<i>" + t + "</i>";
        if (r->bold) t = "<b>" + t + "</b>";
        if (r->link && !r->url.empty())
            t = "<a href=\"" + escaped(r->url) + "\">" + t + "</a>";
        m += t;
    }
    return m;
}

GtkWidget* buildTable(const std::vector<MdRun>& runs, size_t first, size_t end,
                      const std::shared_ptr<Markdown::Hooks>& hooks, Markdown::Embed& embed) {
    const int cols = runs[first].tableCols;
    int rows = 0;
    for (size_t k = first; k < end; k++) rows = std::max(rows, runs[k].tableRow + 1);
    GtkWidget* grid = gtk_grid_new();
    gtk_widget_add_css_class(grid, "minicode-md-table");
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    embed.table = true;
    embed.columns = cols;
    const int line = runs[first].line;

    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            std::vector<const MdRun*> cell;
            int align = 0;
            for (size_t k = first; k < end; k++)
                if (runs[k].tableRow == row && runs[k].tableCol == col) {
                    cell.push_back(&runs[k]);
                    align = runs[k].tableAlign;
                }
            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            gtk_widget_add_css_class(box, "minicode-md-cell");
            if (row == 0) gtk_widget_add_css_class(box, "minicode-md-head");
            GtkWidget* label = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(label), cellMarkup(cell).c_str());
            gtk_label_set_wrap(GTK_LABEL(label), TRUE);
            gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
            // Its natural width is what fit() asks for, not the whole text on
            // one line; the height then follows from the wrapping.
            gtk_label_set_max_width_chars(GTK_LABEL(label), 1);
            gtk_label_set_xalign(GTK_LABEL(label), align == 2 ? 1.0f : align == 1 ? 0.5f : 0.0f);
            gtk_label_set_yalign(GTK_LABEL(label), 0.0f);
            gtk_widget_set_vexpand(label, TRUE);
            g_signal_connect(label, "activate-link", G_CALLBACK(+[](GtkLabel*, const char* uri,
                                                                  gpointer data) -> gboolean {
                auto* h = static_cast<Markdown::Hooks*>(data);
                if (h->link && uri) h->link(uri);
                return TRUE;
            }), hooks.get());
            gtk_box_append(GTK_BOX(box), label);

            auto* info = new CellInfo{hooks, line, row, col};
            g_object_set_data_full(G_OBJECT(box), "md-cell", info,
                                   [](gpointer d) { delete static_cast<CellInfo*>(d); });
            GtkGesture* click = gtk_gesture_click_new();
            g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick* g, int n, double,
                                                            double, gpointer data) {
                if (n != 2) return;
                auto* ci = static_cast<CellInfo*>(data);
                if (!ci->hooks->cellDoubleClick) return;
                gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
                GtkWidget* w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
                ci->hooks->cellDoubleClick(ci->line, ci->row, ci->column, w);
            }), info);
            gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));

            gtk_grid_attach(GTK_GRID(grid), box, col, row, 1, 1);
            embed.labels.push_back(label);
        }
    }
    // The hooks live as long as the table.
    auto* keep = new std::shared_ptr<Markdown::Hooks>(hooks);
    g_object_set_data_full(G_OBJECT(grid), "md-hooks", keep, [](gpointer d) {
        delete static_cast<std::shared_ptr<Markdown::Hooks>*>(d);
    });
    return grid;
}

} // namespace

const Markdown::Span* Markdown::Page::spanAt(int offset) const {
    auto it = std::upper_bound(spans.begin(), spans.end(), offset,
                               [](int o, const Span& s) { return o < s.start; });
    if (it == spans.begin()) return nullptr;
    --it;
    return offset < it->end ? &*it : nullptr;
}

int Markdown::Page::offsetForLine(int line, int endOffset) const {
    if (line <= 0) return 0;
    for (const Span& s : spans)
        if (s.line >= line) return s.start;
    return endOffset;
}

Markdown::Page Markdown::render(GtkTextView* view, GtkTextBuffer* buffer,
                                const std::string& source, const Hooks& hooksIn) {
    gtk_text_buffer_set_text(buffer, "", 0);
    Page page;
    auto hooks = std::make_shared<Hooks>(hooksIn);

    std::vector<MdRun> runs = MarkdownParser::parse(source);

    // Characters emitted since the last newline, i.e. the length of the logical
    // line being built up.
    int lineLen = 0;

    auto addSpan = [&](int a, int b, const MdRun& r) {
        Span sp;
        sp.start = a;
        sp.end = b;
        sp.line = r.line;
        if (r.link) sp.url = r.url;
        page.spans.push_back(sp);
    };
    auto anchorWidget = [&](GtkWidget* w, const MdRun& r) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer, &end);
        const int at = gtk_text_buffer_get_char_count(buffer);
        GtkTextChildAnchor* anchor = gtk_text_buffer_create_child_anchor(buffer, &end);
        gtk_text_view_add_child_at_anchor(view, w, anchor);
        addSpan(at, at + 1, r);
        lineLen++;
    };

    std::string heading;   // the text of the heading being emitted
    int headingLine = -1, headingAt = 0;
    auto endHeading = [&] {
        if (headingLine < 0) return;
        std::string key = MarkdownParser::anchor(heading), unique = key;
        for (int n = 1; page.anchors.count(unique); n++) unique = key + "-" + std::to_string(n);
        if (!unique.empty()) page.anchors[unique] = headingAt;
        headingLine = -1;
        heading.clear();
    };

    for (size_t i = 0; i < runs.size(); i++) {
        const MdRun& r = runs[i];
        if (r.heading > 0 && r.line != headingLine) {
            endHeading();
            headingLine = r.line;
            headingAt = gtk_text_buffer_get_char_count(buffer);
        } else if (r.heading == 0) {
            endHeading();
        }
        if (r.heading > 0) heading += r.text;

        if (r.tableId > 0) {
            size_t end = i;
            while (end < runs.size() && runs[end].tableId == r.tableId) end++;
            Embed embed;
            GtkWidget* grid = buildTable(runs, i, end, hooks, embed);
            embed.widget = grid;
            anchorWidget(grid, r);
            page.embeds.push_back(embed);
            i = end - 1;
            continue;
        }
        if (r.image) {
            int w = 0, h = 0;
            if (GtkWidget* pic = loadPicture(r.src, hooks->folder, &w, &h)) {
                Embed embed;
                embed.widget = pic;
                embed.width = w;
                embed.height = h;
                anchorWidget(pic, r);
                page.embeds.push_back(embed);
                continue;
            }
        }

        std::string text = r.text;
        // A picture that cannot be shown: its alt text (or its path).
        if (r.image && text.empty()) text = r.src;
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
        addSpan(startOff, endOff, r);

        // Layer tags. Order roughly follows the macOS precedence.
        if (r.heading > 0 && r.heading <= 6) {
            char name[8];
            g_snprintf(name, sizeof(name), "h%d", r.heading);
            applyTag(buffer, name, startOff, endOff);
        }
        if (r.codeBlock || r.code) applyTag(buffer, "md_code",  startOff, endOff);
        if (r.quote)               applyTag(buffer, "md_quote", startOff, endOff);
        if (r.rule)                applyTag(buffer, "md_rule",  startOff, endOff);
        if (r.link)                applyTag(buffer, "md_link",  startOff, endOff);
        if (r.bold)                applyTag(buffer, "md_bold",  startOff, endOff);
        if (r.italic)              applyTag(buffer, "md_italic", startOff, endOff);
        if (r.image && !r.link)    applyTag(buffer, "plainmsg", startOff, endOff);
    }
    endHeading();
    return page;
}

void Markdown::fit(Page& page, int width) {
    width = std::max(width, 120);
    for (Embed& e : page.embeds) {
        if (e.table) {
            // Equal columns across the pane; each label wraps inside its own.
            // The cell's padding (8 each side) and border (1) come off.
            const int column = std::max(40, width / std::max(1, e.columns));
            for (GtkWidget* label : e.labels)
                gtk_widget_set_size_request(label, std::max(20, column - 17), -1);
            continue;
        }
        if (e.width <= 0 || e.height <= 0) continue;
        const int w = std::min(e.width, width);
        mdPictureSetSize(e.widget, w, (int)((double)e.height * w / e.width + 0.5));
    }
}
