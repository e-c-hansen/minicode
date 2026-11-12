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
#include "PreviewScroll.h"

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
    bool setContext(const std::string& folder, bool webImages, bool mermaid = false,
                    bool dark = false, bool math = false,
                    const std::string& mathColor = "");   // true when it changed
    void invalidate() { stale_ = true; }

    // The source is shown (or changed): render after `delayMs`.
    void schedule(int delayMs = 300);
    // Empty and forget the page; pending work is dropped.
    void clear();

    // ---- scroll sync. `source` is the editor's text view (one for every tab);
    // its scrolling leads the page, the page's scrolling leads the source,
    // and a caret moved in the source brings its place in the page into
    // view. Only the user's moves lead: our own (a render, a layout, following)
    // are held, and a change that leaves a side's value as last seen is no move.
    void setSource(GtkTextView* source);
    // Typing in the source: the source leads again after the render.
    void edited() { pageLeads_ = false; }
    // The caret is on source line `line` (0-based).
    void sourceCaret(int line);
    // The source's top line and the page's, with fractions (checks read these).
    double sourceTopLine();
    double paneTopLine();
    // The y in the page's buffer of source line `line` (a fraction of a line
    // included), from the two marks around it.
    double paneYForLine(double line);
    bool pageLeads() const { return pageLeads_; }
    bool syncing() const { return settleTimer_ != 0; }   // a target still being held

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

    // scroll sync
    struct Target {
        bool toPane = true;      // the page is moved (else the source)
        double line = 0;         // the source line to show
        double into = 0;         // as a fraction of the pane's height down
    };
    bool syncActive();
    void rebuildMarks();
    PreviewScroll::Page pageAround(int markIndex);
    double topOfAt(long at);
    double sourceYForLine(double line);
    void sourceMoved();
    void paneMoved();
    void apply(const Target& t);
    void follow(const Target& t);
    void startSettle();
    void stopSettle();
    void afterRender();
    GtkTextView* source_ = nullptr;
    GtkAdjustment *srcAdj_ = nullptr, *paneAdj_ = nullptr;
    double lastSrcV_ = -1, lastPaneV_ = -1, lastSrcUp_ = -1, lastPaneUp_ = -1;
    bool holding_ = false;
    bool pageLeads_ = false;
    std::vector<PreviewScroll::Mark> marks_;
    bool marksDirty_ = true;
    Target target_;
    guint settleTimer_ = 0;
    int settleStable_ = 0, settleTicks_ = 0;
    double settleUpper_ = -1;

    GtkWidget* scroller_ = nullptr;
    GtkWidget* view_ = nullptr;
    GtkTextBuffer* buffer_ = nullptr;
    std::vector<Info> infos_;
    std::shared_ptr<Markdown::Hooks> hooks_;
    std::string folder_;
    bool webImages_ = false;
    bool mermaid_ = false, dark_ = false;   // diagrams drawn, in which theme
    int mermaidListener_ = 0, mathListener_ = 0;
    bool math_ = false;            // formulas typeset, in this color
    std::string mathColor_;
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
