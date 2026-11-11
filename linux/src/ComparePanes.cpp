// ComparePanes.cpp — see ComparePanes.h.
#include "ComparePanes.h"

#include <algorithm>
#include <memory>

#include "QuickDiff.h"
#include "SyntaxHighlighter.h"

namespace {

constexpr size_t kMaxColoredText = 2 * 1024 * 1024;   // UTF-16 units a side, as the Mac

std::vector<SideBySide::Span> syntaxSpans(const std::u16string& text, const std::string& ext) {
    std::vector<SideBySide::Span> out;
    if (ext.empty() || text.empty() || text.size() > kMaxColoredText ||
        !SyntaxHighlighter::supports(ext))
        return out;
    IncrementalHighlighter<char16_t> hl(ext);
    std::vector<Token> tokens;
    hl.reset(StringSource<char16_t>(text), &tokens);
    out.reserve(tokens.size());
    for (const Token& t : tokens)
        if (t.style != TokenStyle::Plain && t.length)
            out.push_back({t.start, t.length, (int)t.style});
    return out;
}

// UTF-16 offset -> UTF-8 byte offset, for offsets asked in ascending order.
class ByteCursor {
public:
    explicit ByteCursor(const std::u16string& t) : t_(t) {}
    size_t at(size_t unit) {
        if (unit < unit_) { unit_ = 0; byte_ = 0; }
        unit = std::min(unit, t_.size());
        while (unit_ < unit) {
            const char16_t c = t_[unit_];
            if (c < 0x80) byte_ += 1;
            else if (c < 0x800) byte_ += 2;
            else if (c >= 0xD800 && c < 0xDC00 && unit_ + 1 < t_.size() &&
                     t_[unit_ + 1] >= 0xDC00 && t_[unit_ + 1] < 0xE000) {
                byte_ += 4;
                unit_++;   // the pair is one 4-byte character
            } else byte_ += 3;
            unit_++;
        }
        return byte_;
    }
private:
    const std::u16string& t_;
    size_t unit_ = 0, byte_ = 0;
};

std::string extensionOf(const std::string& name) {
    const size_t slash = name.find_last_of('/');
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string e = name.substr(dot + 1);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

}  // namespace

std::string ComparePane::rowText(size_t r) const {
    if (r >= line.size()) return "";
    return text.substr(starts[r], starts[r + 1] - 1 - starts[r]);
}

CompareData* prepareComparison(const CompareSpec& spec) {
    std::unique_ptr<CompareData> d(new CompareData());
    d->spec = spec;
    const std::u16string a = QuickDiff::fromUtf8(spec.leftText),
                         b = QuickDiff::fromUtf8(spec.rightText);
    d->rows = SideBySide::align(a, b);
    d->hunks = SideBySide::hunks(d->rows);
    for (const SideBySide::Row& r : d->rows)
        if (r.kind != SideBySide::Kind::Same) d->changedRows++;
    const std::string ext = extensionOf(spec.fileName.empty() ? spec.path : spec.fileName);

    std::u16string paneU16[2];
    std::vector<size_t> startsU16[2];
    for (int side = 0; side < 2; side++) {
        const bool left = side == 0;
        const std::u16string& text = left ? a : b;
        ComparePane& p = d->panes[side];
        paneU16[side] = SideBySide::paneText(text, d->rows, left, &startsU16[side]);
        const auto spans = SideBySide::spansOnPane(text, d->rows, left, startsU16[side],
                                                   syntaxSpans(text, ext));
        p.text = QuickDiff::toUtf8(paneU16[side]);
        ByteCursor cur(paneU16[side]);
        p.starts.reserve(d->rows.size() + 1);
        p.line.reserve(d->rows.size());
        for (size_t r = 0; r < d->rows.size(); r++) {
            p.starts.push_back(cur.at(startsU16[side][r]));
            p.line.push_back(left ? d->rows[r].left : d->rows[r].right);
        }
        p.starts.push_back(p.text.size() + 1);
        ByteCursor sc(paneU16[side]);
        p.syntax.reserve(spans.size());
        for (const SideBySide::Span& s : spans) {
            const size_t from = sc.at(s.start), to = sc.at(s.start + s.length);
            if (to > from) p.syntax.push_back({from, to - from, s.style});
        }
        size_t col = 0;
        for (char16_t ch : paneU16[side]) {
            if (ch == u'\n') col = 0;
            else col += ch == u'\t' ? 8 : 1;
            p.columns = std::max(p.columns, col);
        }
    }
    // The words that changed in changed pairs of lines. A line longer than a
    // screen is left to its tint, and past a few megabytes of changed lines
    // the rest are too, as on the Mac.
    size_t budget = 4 * 1024 * 1024;
    ByteCursor wc[2] = {ByteCursor(paneU16[0]), ByteCursor(paneU16[1])};
    for (size_t r = 0; r < d->rows.size() && budget > 0; r++) {
        if (d->rows[r].kind != SideBySide::Kind::Changed) continue;
        std::u16string line[2];
        for (int side = 0; side < 2; side++) {
            const size_t from = startsU16[side][r];
            const size_t to = r + 1 < startsU16[side].size() ? startsU16[side][r + 1] - 1
                                                              : paneU16[side].size();
            line[side] = paneU16[side].substr(from, to - from);
        }
        if (line[0].size() > 2000 || line[1].size() > 2000) continue;
        budget -= std::min(budget, line[0].size() + line[1].size());
        const SideBySide::WordChanges w = SideBySide::wordChanges(line[0], line[1]);
        for (int side = 0; side < 2; side++)
            for (const SideBySide::Span& s : side == 0 ? w.left : w.right) {
                const size_t from = wc[side].at(startsU16[side][r] + s.start);
                const size_t to = wc[side].at(startsU16[side][r] + s.start + s.length);
                if (to > from) d->panes[side].words.push_back({from, to - from, 0});
            }
    }
    return d.release();
}
