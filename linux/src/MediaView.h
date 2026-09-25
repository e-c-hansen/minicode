// MediaView.h — images and PDFs shown in the editor's slot, the GTK side of
// the macOS showImageAtPath: / showPDFAtPath:.
//
// Images (the macOS list: png, jpg, jpeg, gif, tif, tiff, bmp, heic, heif,
// webp, ico, icns) are a GtkPicture that fits the pane but never enlarges a
// small image; animated GIFs play. PDFs are a PdfView, when the build has
// poppler-glib. SVG is not in the list, on purpose, as on the Mac: it is
// source people edit, so it opens as text.
//
// The shown file is watched with a GFileMonitor and redrawn when it changes
// on disk; a PDF keeps its scroll position across that reload. Nothing here
// ever writes to the file.
#pragma once

#include <gtk/gtk.h>
#include <string>

class PdfView;

class MediaView {
public:
    enum class Kind { None, Image, Pdf };

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

    // Decode and show the file, and start watching it. False if it cannot be
    // decoded; the caller then falls through to the text path, which shows
    // the usual "Cannot display" message.
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
    // "  640 × 480", "  1 page", "  12 pages". Empty when nothing is shown.
    std::string titleSuffix() const;

    // Called after the file was reloaded because it changed on disk (a PDF's
    // page count may be different now).
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
    void watch(const std::string& path);
    void reloadFromDisk();

    static void onFileChanged(GFileMonitor* m, GFile* file, GFile* other,
                              GFileMonitorEvent ev, gpointer self);
    static gboolean onAnimationTick(gpointer self);

    GtkWidget* stack_   = nullptr;
    GtkWidget* picture_ = nullptr;
#ifdef MINICODE_ENABLE_PDF
    PdfView*   pdf_     = nullptr;
#endif

    Kind        kind_ = Kind::None;
    std::string path_;
    int         imgW_ = 0, imgH_ = 0;

    GdkPixbufAnimation*     anim_     = nullptr;
    GdkPixbufAnimationIter* animIter_ = nullptr;
    guint                   animTimer_ = 0;

    GFileMonitor* monitor_     = nullptr;
    guint         reloadTimer_ = 0;

    ChangedCb changedCb_   = nullptr;
    void*     changedUser_ = nullptr;
};
