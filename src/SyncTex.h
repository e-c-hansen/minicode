// SyncTex.h — pure C++ reader for the .synctex file TeX writes beside a PDF.
//
// It answers one question: given a point on a page of the PDF, which lines of
// which source file put something there? That is what lets a click in the
// preview find the source it came from. The file itself is gzipped; callers
// hand the decompressed text to parse().
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

private:
    struct Record {
        int page = 1, tag = 0, line = 1;
        double x = 0, y = 0, w = 0, h = 0, d = 0;   // points; y is the baseline
    };
    std::vector<Record> records_;
    std::map<int, std::string> inputs_;
    double unit_ = 1.0, xoff_ = 0.0, yoff_ = 0.0;
};
