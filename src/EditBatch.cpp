// EditBatch.cpp — see EditBatch.h.
#include "EditBatch.h"

namespace EditBatch {

bool valid(const std::vector<Change>& changes, size_t length) {
    size_t end = 0;
    for (const Change& c : changes) {
        if (c.pos < end || c.pos > length || c.oldLen > length - c.pos) return false;
        end = c.pos + c.oldLen;
    }
    return true;
}

std::string applied(const std::string& base, const std::vector<Change>& changes) {
    size_t length = base.size();
    for (const Change& c : changes) length = length - c.oldLen + c.text.size();
    std::string out;
    out.reserve(length);
    size_t b = 0;   // where the old text resumes
    for (const Change& c : changes) {
        out.append(base, b, c.pos - b);
        out += c.text;
        b = c.pos + c.oldLen;
    }
    out.append(base, b, std::string::npos);
    return out;
}

std::string apply(IncrementalHighlighter<char>& hl, const std::string& base,
                  const std::vector<Change>& changes, const Step& step) {
    std::string text = applied(base, changes);
    std::vector<IncrementalHighlighter<char>::Edit> edits;
    edits.reserve(changes.size());
    for (const Change& c : changes) edits.push_back({c.pos, c.oldLen, c.text.size()});
    std::vector<Token> out;
    const StringSource<char> src(text);
    const std::vector<IncrementalHighlighter<char>::EditedRange> ranges =
        hl.editMany(src, edits, out);
    if (step) {
        size_t from = 0;
        std::vector<Token> relexed;
        for (const auto& r : ranges) {
            const size_t lines = r.range.endLine - r.range.firstLine;
            relexed.assign(out.begin() + (long)from, out.begin() + (long)r.tokensEnd);
            from = r.tokensEnd;
            step(src, r.range, hl.lineCount() - lines + r.oldLines, relexed);
        }
    }
    return text;
}

}  // namespace EditBatch
