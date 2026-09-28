// MediaView.cpp — see MediaView.h.
#include "MediaView.h"
#ifdef MINICODE_ENABLE_PDF
#include "PdfView.h"
#endif

#include <algorithm>
#include <cctype>
#include <cstring>

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

std::string baseName(const std::string& path) {
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "0:05", "3:41", "1:02:03", from microseconds.
std::string formatDuration(gint64 us) {
    const gint64 total = (us + 500000) / 1000000;
    const gint64 h = total / 3600, m = total / 60 % 60, sec = total % 60;
    char buf[32];
    if (h > 0) g_snprintf(buf, sizeof buf, "%d:%02d:%02d", (int)h, (int)m, (int)sec);
    else g_snprintf(buf, sizeof buf, "%d:%02d", (int)m, (int)sec);
    return buf;
}

// A page in a scroller that keeps its minimum size to itself. The window's
// vertical split may shrink the editor, so it measures the editor's side at
// small heights; the text view's scroller asks for nothing, but a video's
// controls need 64 pixels, and GTK then warned "Trying to measure GtkBox for
// height of 46" whenever a video was on screen. EXTERNAL shows no scrollbar
// (a video in a sliver of a pane is cut off); AUTOMATIC lets a long message
// scroll.
GtkWidget* unmeasured(GtkWidget* page, GtkPolicyType vertical) {
    GtkWidget* sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_EXTERNAL, vertical);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), page);
    gtk_widget_set_hexpand(sw, TRUE);
    gtk_widget_set_vexpand(sw, TRUE);
    return sw;
}

// Whether GStreamer failed for want of a decoder or demuxer. GTK 4.22 plays
// through GstPlay, which re-wraps every error in its own domain, so the
// original code is gone and only the text is left: the translated message,
// then GStreamer's debug line, which is never translated ("No suitable
// plugins found" from decodebin3, "No decoder available" from the older
// decodebin). A GTK that passes GStreamer's own error through is recognised
// by its domain and code (GstStreamError, GstCoreError), matched by name so
// that no GStreamer header, and no build dependency, is needed.
bool isMissingPlugin(const GError* e) {
    const char* d = g_quark_to_string(e->domain);
    if (d && !strcmp(d, "gst-stream-error-quark") && e->code == 6)   // CODEC_NOT_FOUND
        return true;
    if (d && !strcmp(d, "gst-core-error-quark") && e->code == 12)    // MISSING_PLUGIN
        return true;
    const char* m = e->message ? e->message : "";
    return strstr(m, "No suitable plugins found") || strstr(m, "No decoder available") ||
           strstr(m, "missing a plug-in");
}

// GTK's GStreamer backend reports "Error from element <pipeline path>:
// <message>", then the message again, then GStreamer's debug detail (source
// file and line). Only the plain message is for people.
std::string plainMessage(const char* msg) {
    std::string m = msg ? msg : "";
    if (m.rfind("Error from element ", 0) == 0) {
        auto nl = m.find('\n');
        if (nl != std::string::npos) {
            auto end = m.find('\n', nl + 1);
            std::string second = m.substr(nl + 1, end == std::string::npos ? end : end - nl - 1);
            if (!second.empty()) return second;
        }
        auto colon = m.find(": ");
        if (colon != std::string::npos) m = m.substr(colon + 2);
    }
    auto nl = m.find('\n');
    return nl == std::string::npos ? m : m.substr(0, nl);
}

}  // namespace

// ---------------------------------------------------------------- construction

MediaView::MediaView() {
    stack_ = gtk_stack_new();
    gtk_widget_set_hexpand(stack_, TRUE);
    gtk_widget_set_vexpand(stack_, TRUE);
    // Sized by the page on screen only. A homogeneous stack counts the
    // minimum size of hidden pages too, so the audio page's controls made
    // GTK warn "Trying to measure GtkBox for height of 46" from launch on,
    // with nothing playing (see unmeasured, above, for the rest).
    gtk_stack_set_hhomogeneous(GTK_STACK(stack_), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(stack_), FALSE);

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

    // Video: GtkVideo draws the frames and its own controls, which show while
    // paused and when the pointer moves. Nothing plays until asked.
    video_ = gtk_video_new();
    gtk_video_set_autoplay(GTK_VIDEO(video_), FALSE);
    gtk_widget_set_hexpand(video_, TRUE);
    gtk_widget_set_vexpand(video_, TRUE);
    gtk_widget_add_css_class(video_, "minicode-media");
    gtk_stack_add_named(GTK_STACK(stack_), unmeasured(video_, GTK_POLICY_EXTERNAL), "video");

    // Audio has no picture, and GtkVideo would show an empty pane with its
    // controls hidden until the pointer moved over it, so audio gets the name
    // and the same controls, always shown.
    audio_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(audio_, "minicode-media");
    gtk_widget_set_hexpand(audio_, TRUE);
    gtk_widget_set_vexpand(audio_, TRUE);
    // The controls stretch across the pane, less a margin, rather than
    // asking for a width, which would stop the pane from being narrowed.
    GtkWidget* inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_valign(inner, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(inner, TRUE);
    gtk_widget_set_margin_start(inner, 48);
    gtk_widget_set_margin_end(inner, 48);
    audioName_ = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(audioName_), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(audioName_, "minicode-media-note");
    controls_ = gtk_media_controls_new(nullptr);
    gtk_widget_set_hexpand(controls_, TRUE);
    // Light on dark, as GtkVideo draws its own controls; the theme's
    // default icons were dark grey on the editor's dark background.
    gtk_widget_add_css_class(controls_, "osd");
    gtk_box_append(GTK_BOX(inner), audioName_);
    gtk_box_append(GTK_BOX(inner), controls_);
    gtk_box_append(GTK_BOX(audio_), inner);
    gtk_stack_add_named(GTK_STACK(stack_), unmeasured(audio_, GTK_POLICY_AUTOMATIC), "audio");

    // Why a file cannot play, in the player's place.
    note_ = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(note_), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(note_), 72);
    gtk_label_set_selectable(GTK_LABEL(note_), TRUE);
    gtk_label_set_xalign(GTK_LABEL(note_), 0.0f);
    gtk_widget_set_valign(note_, GTK_ALIGN_START);
    gtk_widget_set_halign(note_, GTK_ALIGN_START);
    gtk_widget_set_margin_start(note_, 24);
    gtk_widget_set_margin_top(note_, 24);
    gtk_widget_set_margin_end(note_, 24);
    gtk_widget_add_css_class(note_, "minicode-media-note");
    GtkWidget* noteBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(noteBox, "minicode-media");
    gtk_widget_set_hexpand(noteBox, TRUE);
    gtk_widget_set_vexpand(noteBox, TRUE);
    gtk_box_append(GTK_BOX(noteBox), note_);
    gtk_stack_add_named(GTK_STACK(stack_), unmeasured(noteBox, GTK_POLICY_AUTOMATIC), "note");
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

bool MediaView::isVideoPath(const std::string& path) {
    static const char* const kExts[] = {"mp4", "m4v", "mov", "webm", "mkv", "ogv", "avi"};
    const std::string e = lowerExt(path);
    for (const char* x : kExts)
        if (e == x) return true;
    return false;
}

bool MediaView::isAudioPath(const std::string& path) {
    static const char* const kExts[] = {"mp3", "wav", "m4a", "aac", "flac", "ogg", "opus"};
    const std::string e = lowerExt(path);
    for (const char* x : kExts)
        if (e == x) return true;
    return false;
}

bool MediaView::handles(const std::string& path) {
    return isImagePath(path) || isPdfPath(path) || isVideoPath(path) || isAudioPath(path);
}

bool MediaView::show(const std::string& path) {
    clear();
    bool ok = false;
    // path_ first: a stream can fail at once, and its note names the file.
    path_ = path;
    if (isImagePath(path)) ok = showImage(path);
    else if (isPdfPath(path)) ok = showPdf(path, false);
    else if (isVideoPath(path) || isAudioPath(path)) ok = showStream(path, -1, false);
    if (!ok) {
        path_.clear();
        return false;
    }
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
    releaseStream();
    failed_ = false;
    durationUs_ = 0;
    restorePos_ = -1;
    restorePlay_ = false;
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
    if ((kind_ == Kind::Video || kind_ == Kind::Audio) && !failed_) {
        std::string s;
        if (kind_ == Kind::Video && imgW_ > 0 && imgH_ > 0)
            s = std::to_string(imgW_) + " × " + std::to_string(imgH_);
        if (durationUs_ > 0) s += (s.empty() ? "" : ", ") + formatDuration(durationUs_);
        return s.empty() ? s : "  " + s;
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

// ---------------------------------------------------------------- video and audio

// A GtkMediaFile is GTK's media backend (GStreamer, built into libgtk-4 on
// current distributions). It opens the file on GStreamer's threads and
// reports back through properties: prepared once it knows what the file
// holds, error if it cannot play it. A video's first frame shows once it is
// prepared, without playing.
bool MediaView::showStream(const std::string& path, gint64 position, bool play) {
    releaseStream();
    failed_ = false;
    durationUs_ = 0;
    imgW_ = imgH_ = 0;
    restorePos_ = position;
    restorePlay_ = play;
    kind_ = isAudioPath(path) ? Kind::Audio : Kind::Video;
    gtk_label_set_text(GTK_LABEL(audioName_), baseName(path).c_str());

    GFile* f = g_file_new_for_path(path.c_str());
    stream_ = gtk_media_file_new_for_file(f);
    g_object_unref(f);
    if (!stream_) return false;

    // A GTK built without a media backend hands back a stand-in that fails
    // at once, saying only "check your installation".
    if (!strcmp(G_OBJECT_TYPE_NAME(stream_), "GtkNoMediaFile")) {
        failed_ = true;
        showNote("Cannot play “" + baseName(path) + "”.\n\n"
                 "This GTK has no media backend, so it cannot play video or audio. "
                 "The backend is GTK's GStreamer support. On Ubuntu 26.04 and later it "
                 "is part of libgtk-4-1; on older Ubuntu and Debian releases it is the "
                 "package\n\n"
                 "libgtk-4-media-gstreamer\n\n"
                 "and elsewhere it comes with a GTK 4 built with GStreamer. Restart "
                 "MiniCode after installing it.");
        return true;
    }

    g_signal_connect(stream_, "notify::prepared", G_CALLBACK(onStreamNotify), this);
    g_signal_connect(stream_, "notify::error", G_CALLBACK(onStreamNotify), this);
    g_signal_connect(stream_, "notify::duration", G_CALLBACK(onStreamNotify), this);
    g_signal_connect(stream_, "invalidate-size", G_CALLBACK(onStreamSize), this);

    if (kind_ == Kind::Audio) {
        gtk_media_controls_set_media_stream(GTK_MEDIA_CONTROLS(controls_), stream_);
        gtk_stack_set_visible_child_name(GTK_STACK(stack_), "audio");
    } else {
        gtk_video_set_media_stream(GTK_VIDEO(video_), stream_);
        gtk_stack_set_visible_child_name(GTK_STACK(stack_), "video");
    }
    // Either can have happened already, before the handlers were connected.
    if (gtk_media_stream_get_error(stream_)) streamFailed();
    else if (gtk_media_stream_is_prepared(stream_)) streamPrepared();
    return true;
}

// Stops the stream and lets it go, taking GStreamer's pipeline with it, so
// nothing is heard or decoded once another file, a diff or nothing is shown.
void MediaView::releaseStream() {
    if (!stream_) return;
    g_signal_handlers_disconnect_by_data(stream_, this);
    gtk_media_stream_pause(stream_);
    gtk_video_set_media_stream(GTK_VIDEO(video_), nullptr);
    gtk_media_controls_set_media_stream(GTK_MEDIA_CONTROLS(controls_), nullptr);
    if (GTK_IS_MEDIA_FILE(stream_)) gtk_media_file_clear(GTK_MEDIA_FILE(stream_));
    g_object_unref(stream_);
    stream_ = nullptr;
}

void MediaView::showNote(const std::string& text, const char* detail) {
    gtk_label_set_text(GTK_LABEL(note_), text.c_str());
    gtk_widget_set_tooltip_text(note_, detail);
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "note");
}

void MediaView::onStreamNotify(GObject*, GParamSpec* pspec, gpointer selfp) {
    MediaView* self = static_cast<MediaView*>(selfp);
    if (!self->stream_) return;
    const char* name = g_param_spec_get_name(pspec);
    if (!strcmp(name, "error")) {
        if (gtk_media_stream_get_error(self->stream_)) self->streamFailed();
    } else if (!strcmp(name, "prepared")) {
        self->streamPrepared();
    } else if (!strcmp(name, "duration")) {
        const gint64 d = gtk_media_stream_get_duration(self->stream_);
        if (d != self->durationUs_) {
            self->durationUs_ = d;
            self->notifyChanged();
        }
    }
}

// A video's size is known once its first frame is decoded, after prepared.
void MediaView::onStreamSize(GdkPaintable* stream, gpointer selfp) {
    MediaView* self = static_cast<MediaView*>(selfp);
    const int w = gdk_paintable_get_intrinsic_width(stream);
    const int h = gdk_paintable_get_intrinsic_height(stream);
    if (w == self->imgW_ && h == self->imgH_) return;
    self->imgW_ = w;
    self->imgH_ = h;
    self->notifyChanged();
}

void MediaView::streamPrepared() {
    if (!stream_ || failed_ || !gtk_media_stream_is_prepared(stream_)) return;
    durationUs_ = gtk_media_stream_get_duration(stream_);
    // The page follows what the file holds, not its name: an .ogg with
    // Theora in it is a video, and an .mp4 of sound alone is audio.
    const bool hasVideo = gtk_media_stream_has_video(stream_);
    if (hasVideo && kind_ == Kind::Audio) {
        gtk_media_controls_set_media_stream(GTK_MEDIA_CONTROLS(controls_), nullptr);
        gtk_video_set_media_stream(GTK_VIDEO(video_), stream_);
        gtk_stack_set_visible_child_name(GTK_STACK(stack_), "video");
        kind_ = Kind::Video;
    } else if (!hasVideo && kind_ == Kind::Video) {
        gtk_video_set_media_stream(GTK_VIDEO(video_), nullptr);
        gtk_media_controls_set_media_stream(GTK_MEDIA_CONTROLS(controls_), stream_);
        gtk_stack_set_visible_child_name(GTK_STACK(stack_), "audio");
        kind_ = Kind::Audio;
    }
    imgW_ = gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(stream_));
    imgH_ = gdk_paintable_get_intrinsic_height(GDK_PAINTABLE(stream_));
    g_log("minicode-media", G_LOG_LEVEL_DEBUG,
          "%s: prepared, %s, %d x %d, %lld us, back to %lld us%s", path_.c_str(),
          hasVideo ? "video" : "audio", imgW_, imgH_, (long long)durationUs_,
          (long long)std::max<gint64>(restorePos_, 0), restorePlay_ ? ", playing" : "");
    // After a reload: back to where it was, playing if it was.
    if (restorePos_ > 0 && gtk_media_stream_is_seekable(stream_))
        gtk_media_stream_seek(stream_, durationUs_ > 0 ? std::min(restorePos_, durationUs_)
                                                       : restorePos_);
    if (restorePlay_) gtk_media_stream_play(stream_);
    restorePos_ = -1;
    restorePlay_ = false;
    notifyChanged();
}

// The stream is kept (it has stopped by itself) until clear() lets it go:
// this runs inside its own notify, which is no place to drop it.
void MediaView::streamFailed() {
    if (failed_) return;
    failed_ = true;
    restorePos_ = -1;
    restorePlay_ = false;
    const GError* e = gtk_media_stream_get_error(stream_);
    if (e)
        g_log("minicode-media", G_LOG_LEVEL_DEBUG, "%s: %s %d: %s", path_.c_str(),
              g_quark_to_string(e->domain), e->code, e->message);
    std::string text = "Cannot play “" + baseName(path_) + "”.\n\n";
    if (e && isMissingPlugin(e))
        text += "GStreamer has no decoder for what is in this file. On Ubuntu and "
                "Debian these packages add the common ones:\n\n"
                "gstreamer1.0-libav, for H.264, AAC and most other formats\n"
                "gstreamer1.0-plugins-good, for WebM, Matroska, MP3 and FLAC\n\n"
                "Other distributions have the same GStreamer plugins under similar "
                "names. Restart MiniCode after installing them.\n\n";
    if (e && e->message) text += "(" + plainMessage(e->message) + ")";
    showNote(text, e ? e->message : nullptr);
    notifyChanged();
}

bool MediaView::canPlay() const {
    return stream_ && !failed_ && gtk_media_stream_is_prepared(stream_) &&
           !gtk_media_stream_get_error(stream_);
}

bool MediaView::playing() const {
    return stream_ && gtk_media_stream_get_playing(stream_);
}

void MediaView::togglePlay() {
    if (!canPlay()) return;
    if (gtk_media_stream_get_playing(stream_)) {
        gtk_media_stream_pause(stream_);
        return;
    }
    // At the end, playing starts over, as the controls' own button does.
    if (gtk_media_stream_get_ended(stream_)) gtk_media_stream_seek(stream_, 0);
    gtk_media_stream_play(stream_);
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
    else if (kind_ == Kind::Video || kind_ == Kind::Audio) {
        // Where it was and whether it was playing, carried to the new stream;
        // a reload whose stream was not prepared yet passes its own on.
        gint64 pos = 0;
        bool play = false;
        if (restorePos_ >= 0 || restorePlay_) {
            pos = std::max<gint64>(restorePos_, 0);
            play = restorePlay_;
        } else if (stream_ && !failed_) {
            pos = gtk_media_stream_get_timestamp(stream_);
            play = gtk_media_stream_get_playing(stream_);
        }
        ok = showStream(path_, pos, play);
    }
    if (ok && changedCb_) changedCb_(changedUser_);
}
