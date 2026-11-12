// MarkdownSide.h — a Markdown file's page right of its source (the Mac's
// MarkdownSidePreview.{h,mm}, PARITY.md 4.2): a read-only text view with a
// buffer of its own, kept up to date in pieces. 0.3 s after typing stops the
// text is parsed, split (MarkdownPage::split) and matched against the page
// on show (MarkdownPage::match) on a worker thread; on the main thread only
// the unmatched pieces, and pieces still waiting for a web picture, are
// built again and spliced in. The pieces that are kept are not touched, their
// widgets (tables, pictures) stay, and only their source lines move.
//
// A page of another context (the Markdown file's folder, web pictures on or
// off) is built whole: invalidate(). Scroll sync is not here yet (4.2b).
#pragma once

#include <gtk/gtk.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Markdown.h"
#include "MarkdownPage.h"

class MarkdownSide {
public:
    explicit MarkdownSide(GtkTextTagTable* tags);
    ~MarkdownSide();
    MarkdownSide(const MarkdownSide&) = delete;
    MarkdownSide& operator=(const MarkdownSide&) = delete;

    GtkWidget* widget() const { return scroller_; }
    GtkWidget* textView() const { return view_; }

    // The source's text, asked when a render starts.
    std::function<std::string()> getText;
    // A link clicked in the page, as written.
    std::function<void(const std::string& url)> onLink;

    // What the pieces are built from. A change makes the next render whole.
    bool setContext(const std::string& folder, bool webImages);   // true when it changed
    void invalidate() { stale_ = true; }

    // The source is shown (or changed): render after `delayMs`.
    void schedule(int delayMs = 300);
    // Empty and forget the page; pending work is dropped.
    void clear();

    // No timer, no job: the page shows the text last asked for.
    bool idle() const { return !timer_ && !job_ && !again_; }
    std::string pageText() const;

    // What the last render did (checks read these).
    struct Stats {
        int renders = 0;           // finished renders
        int pieces = 0;            // in the page now
        int built = 0, kept = 0;   // in the last render
        long builtTotal = 0;       // pieces built since the page was cleared
        double mainCpuMs = 0;      // the main thread's CPU for the last render
        double workerCpuMs = 0;    // the worker's
    };
    const Stats& stats() const { return stats_; }
    int embedCount() const;
    int pendingPictures() const;
    std::vector<GtkWidget*> embedWidgets() const;   // every table and picture, in order
    // Rendered text of the line(s) holding `needle`'s widgets etc. are the
    // view's: tests read the buffer.
    GtkTextBuffer* buffer() const { return buffer_; }

private:
    struct Info {
        MarkdownPage::Piece piece;
        Markdown::Page page;   // offsets from the piece's start
    };
    struct Job;

    void start();
    void apply(Job* job);
    void dropWidgets(size_t from, size_t to);
    void fit();
    void fetchPictures(const Info& info);
    void placeArrived();
    void clicked(double x, double y);

    GtkWidget* scroller_ = nullptr;
    GtkWidget* view_ = nullptr;
    GtkTextBuffer* buffer_ = nullptr;
    std::vector<Info> infos_;
    std::shared_ptr<Markdown::Hooks> hooks_;
    std::string folder_;
    bool webImages_ = false;
    bool stale_ = false;      // the context moved: build everything
    guint timer_ = 0, arrivedTimer_ = 0;
    Job* job_ = nullptr;      // running on the worker
    bool again_ = false;      // asked for while it ran
    uint64_t generation_ = 0;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    std::vector<std::pair<std::string, WebImages::PicturePtr>> arrived_;
    int fitWidth_ = 0;
    Stats stats_;
};
