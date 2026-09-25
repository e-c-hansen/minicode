// MediaView.cpp — see MediaView.h.
#include "MediaView.h"
#ifdef MINICODE_ENABLE_PDF
#include "PdfView.h"
#endif

#include <algorithm>
#include <cctype>

namespace {

std::string lowerExt(const std::string& path) {
    auto slash = path.find_last_of('/');
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string e = path.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return e;
}

// Waits this long after the last change event before reloading, so a file
// being written in pieces is read once, when it is done.
constexpr guint kReloadDelayMs = 150;

// A GdkPixbuf frame as a texture, without the deprecated
// gdk_texture_new_for_pixbuf. Pixbuf pixels are straight (not premultiplied)
// RGB or RGBA, which GDK_MEMORY_R8G8B8(A8) describes exactly.
GdkTexture* textureFromPixbuf(GdkPixbuf* pb) {
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

}  // namespace

// ---------------------------------------------------------------- construction

MediaView::MediaView() {
    stack_ = gtk_stack_new();
    gtk_widget_set_hexpand(stack_, TRUE);
    gtk_widget_set_vexpand(stack_, TRUE);

    // Fit the pane, but never blow a small image up past its own size: the
    // macOS NSImageScaleProportionallyDown. can_shrink lets a big image go
    // smaller than its pixel size instead of forcing the window wider.
    picture_ = gtk_picture_new();
    gtk_picture_set_can_shrink(GTK_PICTURE(picture_), TRUE);
    gtk_picture_set_content_fit(GTK_PICTURE(picture_), GTK_CONTENT_FIT_SCALE_DOWN);
    gtk_widget_set_hexpand(picture_, TRUE);
    gtk_widget_set_vexpand(picture_, TRUE);
    gtk_widget_add_css_class(picture_, "minicode-media");
    gtk_stack_add_named(GTK_STACK(stack_), picture_, "image");

#ifdef MINICODE_ENABLE_PDF
    pdf_ = new PdfView();
    gtk_stack_add_named(GTK_STACK(stack_), pdf_->widget(), "pdf");
#endif
}

MediaView::~MediaView() {
    clear();
#ifdef MINICODE_ENABLE_PDF
    delete pdf_;
#endif
}

// ---------------------------------------------------------------- routing

bool MediaView::isImagePath(const std::string& path) {
    static const char* const kExts[] = {"png", "jpg", "jpeg", "gif", "tif", "tiff",
                                        "bmp", "heic", "heif", "webp", "ico", "icns"};
    const std::string e = lowerExt(path);
    for (const char* x : kExts)
        if (e == x) return true;
    return false;
}

bool MediaView::isPdfPath(const std::string& path) {
#ifdef MINICODE_ENABLE_PDF
    return lowerExt(path) == "pdf";
#else
    (void)path;
    return false;
#endif
}

bool MediaView::show(const std::string& path) {
    clear();
    bool ok = false;
    if (isImagePath(path)) ok = showImage(path);
    else if (isPdfPath(path)) ok = showPdf(path, false);
    if (!ok) return false;
    path_ = path;
    watch(path);
    return true;
}

void MediaView::setPath(const std::string& path) {
    if (kind_ == Kind::None || path == path_) return;
    if (reloadTimer_) g_source_remove(reloadTimer_);
    reloadTimer_ = 0;
    if (monitor_) {
        g_signal_handlers_disconnect_by_data(monitor_, this);
        g_file_monitor_cancel(monitor_);
        g_object_unref(monitor_);
        monitor_ = nullptr;
    }
    path_ = path;
    watch(path);
}

void MediaView::clear() {
    if (reloadTimer_) g_source_remove(reloadTimer_);
    reloadTimer_ = 0;
    if (monitor_) {
        g_signal_handlers_disconnect_by_data(monitor_, this);
        g_file_monitor_cancel(monitor_);
        g_object_unref(monitor_);
        monitor_ = nullptr;
    }
    stopAnimation();
    gtk_picture_set_paintable(GTK_PICTURE(picture_), nullptr);
#ifdef MINICODE_ENABLE_PDF
    pdf_->clear();
#endif
    kind_ = Kind::None;
    path_.clear();
    imgW_ = imgH_ = 0;
}

int MediaView::pageCount() const {
#ifdef MINICODE_ENABLE_PDF
    if (kind_ == Kind::Pdf) return pdf_->pageCount();
#endif
    return 0;
}

std::string MediaView::titleSuffix() const {
    if (kind_ == Kind::Image)
        return "  " + std::to_string(imgW_) + " × " + std::to_string(imgH_);
    if (kind_ == Kind::Pdf) {
        const int n = pageCount();
        return n == 1 ? std::string("  1 page") : "  " + std::to_string(n) + " pages";
    }
    return "";
}

// ---------------------------------------------------------------- images

// Still images go through GTK's own texture loader: PNG, JPEG and TIFF
// natively, everything else through the system's image loaders (webp and
// heif work when their loaders are installed, as on a stock Ubuntu desktop).
// An animated GIF is the one case that needs frames; see animatedGif.
bool MediaView::showImage(const std::string& path) {
    stopAnimation();
    GdkTexture* tex = lowerExt(path) == "gif" ? animatedGif(path) : nullptr;
    if (!tex) {
        GError* err = nullptr;
        tex = gdk_texture_new_from_filename(path.c_str(), &err);
        g_clear_error(&err);
    }
    if (!tex) return false;
    // A texture's size is its pixel size, whatever dpi the file claims, so it
    // is drawn at one image pixel per logical pixel at most.
    imgW_ = gdk_texture_get_width(tex);
    imgH_ = gdk_texture_get_height(tex);
    gtk_picture_set_paintable(GTK_PICTURE(picture_), GDK_PAINTABLE(tex));
    g_object_unref(tex);
    kind_ = Kind::Image;
    gtk_stack_set_visible_child(GTK_STACK(stack_), picture_);
    return true;
}

// gdk-pixbuf 2.44 deprecated its animation API with no replacement among the
// libraries a distribution ships alongside GTK, and GtkPicture draws only a
// GIF's first frame. The macOS build plays GIFs, so this keeps using the API,
// with the warning silenced for these two functions and nowhere else.
// Returns the first frame and starts the timer, or null for a still GIF,
// which the texture loader then takes.
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
GdkTexture* MediaView::animatedGif(const std::string& path) {
    GdkPixbufAnimation* anim = gdk_pixbuf_animation_new_from_file(path.c_str(), nullptr);
    if (!anim) return nullptr;
    if (gdk_pixbuf_animation_is_static_image(anim)) {
        g_object_unref(anim);
        return nullptr;
    }
    anim_ = anim;
    animIter_ = gdk_pixbuf_animation_get_iter(anim_, nullptr);
    GdkTexture* tex = textureFromPixbuf(gdk_pixbuf_animation_iter_get_pixbuf(animIter_));
    const int delay = gdk_pixbuf_animation_iter_get_delay_time(animIter_);
    if (delay >= 0) animTimer_ = g_timeout_add(std::max(delay, 20), onAnimationTick, this);
    return tex;
}

gboolean MediaView::onAnimationTick(gpointer selfp) {
    MediaView* self = static_cast<MediaView*>(selfp);
    self->animTimer_ = 0;
    if (!self->animIter_) return G_SOURCE_REMOVE;
    gdk_pixbuf_animation_iter_advance(self->animIter_, nullptr);
    GdkTexture* tex = textureFromPixbuf(gdk_pixbuf_animation_iter_get_pixbuf(self->animIter_));
    gtk_picture_set_paintable(GTK_PICTURE(self->picture_), GDK_PAINTABLE(tex));
    g_object_unref(tex);
    const int delay = gdk_pixbuf_animation_iter_get_delay_time(self->animIter_);
    if (delay >= 0)
        self->animTimer_ = g_timeout_add(std::max(delay, 20), onAnimationTick, self);
    return G_SOURCE_REMOVE;
}
G_GNUC_END_IGNORE_DEPRECATIONS

void MediaView::stopAnimation() {
    if (animTimer_) g_source_remove(animTimer_);
    animTimer_ = 0;
    if (animIter_) g_object_unref(animIter_);
    animIter_ = nullptr;
    if (anim_) g_object_unref(anim_);
    anim_ = nullptr;
}

// ---------------------------------------------------------------- PDFs

bool MediaView::showPdf(const std::string& path, bool reload) {
#ifdef MINICODE_ENABLE_PDF
    if (!pdf_->load(path, reload)) return false;
    kind_ = Kind::Pdf;
    gtk_stack_set_visible_child(GTK_STACK(stack_), pdf_->widget());
    return true;
#else
    (void)path;
    (void)reload;
    return false;
#endif
}

// ---------------------------------------------------------------- watching

void MediaView::watch(const std::string& path) {
    GFile* f = g_file_new_for_path(path.c_str());
    // WATCH_MOVES so a save that writes a temporary file and renames it over
    // this one (what most tools do) arrives as a rename, not as a deletion.
    monitor_ = g_file_monitor_file(f, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    g_object_unref(f);
    if (monitor_) g_signal_connect(monitor_, "changed", G_CALLBACK(onFileChanged), this);
}

void MediaView::onFileChanged(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent ev,
                              gpointer selfp) {
    MediaView* self = static_cast<MediaView*>(selfp);
    switch (ev) {
        case G_FILE_MONITOR_EVENT_CHANGED:
        case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
        case G_FILE_MONITOR_EVENT_CREATED:
        case G_FILE_MONITOR_EVENT_RENAMED:
        case G_FILE_MONITOR_EVENT_MOVED_IN:
            break;
        default:
            return;   // attribute changes, and deletion (the old picture stays)
    }
    if (self->reloadTimer_) g_source_remove(self->reloadTimer_);
    self->reloadTimer_ = g_timeout_add(kReloadDelayMs, [](gpointer p) -> gboolean {
        MediaView* s = static_cast<MediaView*>(p);
        s->reloadTimer_ = 0;
        s->reloadFromDisk();
        return G_SOURCE_REMOVE;
    }, self);
}

// Redraw from the file as it is now. If it cannot be decoded right now (a
// PDF half written), what was on screen stays until the next change.
void MediaView::reloadFromDisk() {
    if (path_.empty() || !g_file_test(path_.c_str(), G_FILE_TEST_EXISTS)) return;
    bool ok = false;
    if (kind_ == Kind::Image) ok = showImage(path_);
    else if (kind_ == Kind::Pdf) ok = showPdf(path_, true);
    if (ok && changedCb_) changedCb_(changedUser_);
}
