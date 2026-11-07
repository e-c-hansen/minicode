// SyncTex.h — pure C++ reader for the .synctex file TeX writes beside a PDF.
//
// It answers two questions: given a point on a page of the PDF, which lines
// of which source file put something there (what lets a click in the preview
// find the source it came from), and the other way round, where on the pages
// did a source line go (forward search, from the caret to the page). The
// file itself is gzipped; callers hand the decompressed text to parse().
#pragma once
#include <cstddef>
#include <map>
#include <string>
#include <vector>

struct SyncTexHit {
    int tag = 0;            // which input file (see pathForTag)
    int line = 1;           // 1-based source line
    double distance = 0;    // 0 when the point is inside the box, in points
    double area = 0;        // box area in points^2; smaller is more specific
    // The box, in PDF points measured from the top-left of the page.
    double x = 0, y = 0, width = 0, height = 0;
};

// A box on a page, in PDF points from the page's top-left.
struct SyncTexBox {
    int page = 1;           // 1-based
    double x = 0, y = 0, width = 0, height = 0;
};

class SyncTexIndex {
public:
    static SyncTexIndex parse(const std::string &text);

    bool valid() const { return !records_.empty(); }
    std::size_t recordCount() const { return records_.size(); }
    std::string pathForTag(int tag) const;
    // The tag of the input whose path is `path`, or whose file name matches it.
    int tagForPath(const std::string &path) const;

    // Boxes covering (or nearest to) a point, best first, at most `maxHits`.
    // `page` is 1-based; x and y are in points from the page's top-left.
    std::vector<SyncTexHit> hitsAtPoint(int page, double x, double y,
                                        std::size_t maxHits = 8) const;

    // Like hitsAtPoint, but for a point on a line of text, and more exact.
    // A line's box carries the source line where its *paragraph* ended, often
    // several lines past the words in it. The glue and kerns TeX set between
    // the words carry the line each word was read from, so those beside the
    // point come first: the first one to its right (the space after the
    // word), then the nearest to its left. The box hits follow.
    std::vector<SyncTexHit> textHitsAtPoint(int page, double x, double y,
                                            std::size_t maxHits = 8) const;

    // Forward search: the lines of text on the page where `line` of input
    // `tag` (0 = any input) put something, top to bottom, all on the first
    // page it reached. A line that put nothing there (a blank line, a
    // comment, a \usepackage) gives the nearest line that did, a line
    // below first; `foundLine` says which (0 when none within 40). The
    // boxes are the lines of text (TeX's horizontal boxes) holding the glue
    // and kerns that line's words were set with, since a line's box itself
    // carries the line its paragraph ended on.
    std::vector<SyncTexBox> boxesForLine(int tag, int line, int *foundLine = nullptr) const;

private:
    struct Record {
        char type = 'h';
        int page = 1, tag = 0, line = 1;
        double x = 0, y = 0, w = 0, h = 0, d = 0;   // points; y is the baseline
    };
    std::vector<Record> records_;
    std::map<int, std::string> inputs_;
    double unit_ = 1.0, xoff_ = 0.0, yoff_ = 0.0;
};
