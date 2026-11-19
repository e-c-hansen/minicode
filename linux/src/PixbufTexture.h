// PixbufTexture.h — a GdkPixbuf's pixels as a GdkTexture, without
// gdk_texture_new_for_pixbuf (deprecated in GTK 4.20): the image viewer's
// animated GIF frames (MediaView.cpp) and the review page's pictures
// (ChangeReviewPage.cpp), which are decoded at the size they are drawn.
// Pixbuf pixels are straight (not premultiplied) RGB or RGBA, which
// GDK_MEMORY_R8G8B8(A8) describes exactly. A memory texture is immutable,
// so it may be made off the main thread.
#pragma once
#include <gtk/gtk.h>

inline GdkTexture* textureFromPixbuf(GdkPixbuf* pb) {
    const int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    const int stride = gdk_pixbuf_get_rowstride(pb);
    const bool alpha = gdk_pixbuf_get_has_alpha(pb);
    const int bpp = alpha ? 4 : 3;
    // The last row of a pixbuf may be shorter than the stride.
    const gsize len = (gsize)stride * (h - 1) + (gsize)w * bpp;
    GBytes* bytes = g_bytes_new(gdk_pixbuf_read_pixels(pb), len);
    GdkTexture* t = gdk_memory_texture_new(
        w, h, alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8, bytes, stride);
    g_bytes_unref(bytes);
    return t;
}
