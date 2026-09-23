// LatexClick.cpp — see LatexClick.h.
#ifdef MINICODE_ENABLE_PDF

#include "LatexClick.h"

#include <gio/gio.h>

#include <algorithm>
#include <vector>

PageText pageTextOf(PopplerPage* page) {
    char* text = poppler_page_get_text(page);
    PopplerRectangle* rects = nullptr;
    guint n = 0;
    std::vector<PageBox> boxes;
    // One rectangle per character of the text above, newlines included, in
    // points from the page's top-left: the frame SyncTeX uses too.
    if (poppler_page_get_text_layout(page, &rects, &n)) {
        boxes.reserve(n);
        for (guint i = 0; i < n; ++i) {
            PageBox b;
            b.x1 = std::min(rects[i].x1, rects[i].x2);
            b.x2 = std::max(rects[i].x1, rects[i].x2);
            b.y1 = std::min(rects[i].y1, rects[i].y2);
            b.y2 = std::max(rects[i].y1, rects[i].y2);
            boxes.push_back(b);
        }
        g_free(rects);
    }
    PageText out(text ? text : "", std::move(boxes));
    g_free(text);
    return out;
}

std::string readGzipFile(const std::string& path) {
    std::string out;
    GFile* file = g_file_new_for_path(path.c_str());
    GFileInputStream* raw = g_file_read(file, nullptr, nullptr);
    g_object_unref(file);
    if (!raw) return out;
    GZlibDecompressor* gz = g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP);
    GInputStream* in = g_converter_input_stream_new(G_INPUT_STREAM(raw), G_CONVERTER(gz));
    char buf[64 * 1024];
    for (;;) {
        GError* err = nullptr;
        const gssize got = g_input_stream_read(in, buf, sizeof(buf), nullptr, &err);
        if (got <= 0) {
            if (err) { out.clear(); g_error_free(err); }   // corrupt: none of it
            break;
        }
        out.append(buf, (size_t)got);
    }
    g_object_unref(in);
    g_object_unref(gz);
    g_object_unref(raw);
    return out;
}

#endif  // MINICODE_ENABLE_PDF
