// MediaView.h — images, PDFs, video and audio shown in the editor's slot,
// the GTK side of the macOS showImageAtPath: / showPDFAtPath: and its player.
//
// Images (the macOS list: png, jpg, jpeg, gif, tif, tiff, bmp, heic, heif,
// webp, ico, icns) are a GtkPicture that fits the pane but never enlarges a
// small image; animated GIFs play. PDFs are a PdfView, when the build has
// poppler-glib. SVG is not in the list, on purpose, as on the Mac: it is
// source people edit, so it opens as text.
//
// Video (mp4, m4v, mov, webm, mkv, ogv, avi) is a GtkVideo, audio (mp3, wav,
// m4a, aac, flac, ogg, opus) a GtkMediaControls under the file's name, both
// over a GtkMediaFile, which is GTK's GStreamer backend. Nothing plays until
// asked; a video shows its first frame. When the backend or a decoder is
// missing, the slot says so and names the package to install. The stream is
// stopped and let go in clear(), so no sound outlives the file.
//
// The shown file is watched with a GFileMonitor and redrawn when it changes
// on disk; a PDF keeps its scroll position across that reload, and video or
// audio its position and whether it was playing. Nothing here ever writes to
// the file.
#pragma once

#include <gtk/gtk.h>
#include <string>

class PdfView;

class MediaView {
public:
    enum class Kind { None, Image, Pdf, Video, Audio };

    MediaView();
    ~MediaView();
    MediaView(const MediaView&) = delete;
    MediaView& operator=(const MediaView&) = delete;

    // A stack holding the picture and (when built) the PDF view.
    GtkWidget* widget() const { return stack_; }

    // Whether a path is routed here by its extension, before anything tries
    // to read it as text.
    static bool isImagePath(const std::string& path);
    static bool isPdfPath(const std::string& path);   // false without poppler
    static bool isVideoPath(const std::string& path);
    static bool isAudioPath(const std::string& path);
    // Any of the four: opened here rather than as text.
    static bool handles(const std::string& path);

    // Decode and show the file, and start watching it. False if it cannot be
    // decoded; the caller then falls through to the text path, which shows
    // the usual "Cannot display" message. Video and audio are decoded later,
    // on GStreamer's threads, so for them this is true unless no stream could
    // be made at all; a file that then fails to play gets a message here.
    bool show(const std::string& path);
    // Stop showing and watching anything.
    void clear();
    // The file shown was renamed or moved: keep showing it, and watch it
    // under its new name so it still reloads when it changes.
    void setPath(const std::string& path);

    Kind kind() const { return kind_; }
    const std::string& path() const { return path_; }
    int imageWidth() const { return imgW_; }    // pixels
    int imageHeight() const { return imgH_; }
    int pageCount() const;
    // What the window title adds after the name, in the macOS wording:
    // "  640 × 480", "  1 page", "  12 pages", and for video and audio the
    // size and length once GStreamer knows them ("  640 × 360, 0:05",
    // "  3:41"). Empty when nothing is shown.
    std::string titleSuffix() const;

    // Video and audio: whether there is a stream that can play (prepared, no
    // error), and play or pause it. At the end, playing starts over.
    bool canPlay() const;
    void togglePlay();
    bool playing() const;

    // Called after the file was reloaded because it changed on disk (a PDF's
    // page count may be different now), and when a stream learns its size
    // and length, or fails.
    using ChangedCb = void (*)(void* user);
    void setChangedCallback(ChangedCb cb, void* user) { changedCb_ = cb; changedUser_ = user; }

#ifdef MINICODE_ENABLE_PDF
    PdfView* pdfView() const { return pdf_; }
#endif

private:
    bool showImage(const std::string& path);
    GdkTexture* animatedGif(const std::string& path);
    bool showPdf(const std::string& path, bool reload);
    void stopAnimation();
    bool showStream(const std::string& path, gint64 position, bool play);
    void releaseStream();
    // detail, if any, is GStreamer's whole message, kept as the tooltip.
    void showNote(const std::string& text, const char* detail = nullptr);
    void streamPrepared();
    void streamFailed();
    void notifyChanged() { if (changedCb_) changedCb_(changedUser_); }
    void watch(const std::string& path);
    void reloadFromDisk();

    static void onFileChanged(GFileMonitor* m, GFile* file, GFile* other,
                              GFileMonitorEvent ev, gpointer self);
    static gboolean onAnimationTick(gpointer self);
    static void onStreamNotify(GObject* stream, GParamSpec* pspec, gpointer self);
    static void onStreamSize(GdkPaintable* stream, gpointer self);

    GtkWidget* stack_   = nullptr;
    GtkWidget* picture_ = nullptr;
#ifdef MINICODE_ENABLE_PDF
    PdfView*   pdf_     = nullptr;
#endif
    GtkWidget* video_    = nullptr;   // GtkVideo
    GtkWidget* audio_    = nullptr;   // box: name, then the controls
    GtkWidget* audioName_ = nullptr;
    GtkWidget* controls_ = nullptr;   // GtkMediaControls, for audio
    GtkWidget* note_     = nullptr;   // a message in place of the player

    Kind        kind_ = Kind::None;
    std::string path_;
    int         imgW_ = 0, imgH_ = 0;

    GdkPixbufAnimation*     anim_     = nullptr;
    GdkPixbufAnimationIter* animIter_ = nullptr;
    guint                   animTimer_ = 0;

    // Video and audio. restorePos_ and restorePlay_ carry the place and the
    // play state across a reload until the new stream is prepared.
    GtkMediaStream* stream_      = nullptr;
    bool            failed_      = false;   // a note stands in for the player
    gint64          durationUs_  = 0;
    gint64          restorePos_  = -1;
    bool            restorePlay_ = false;

    GFileMonitor* monitor_     = nullptr;
    guint         reloadTimer_ = 0;

    ChangedCb changedCb_   = nullptr;
    void*     changedUser_ = nullptr;
};
