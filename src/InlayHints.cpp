// InlayHints.cpp — see InlayHints.h.
#include "InlayHints.h"
#include <algorithm>

namespace Lsp {

namespace {

// A position's number: a whole number from 0 up, else -1.
long wholeNumber(const Json& j) {
    if (!j.isNumber()) return -1;
    const double d = j.asNumber();
    if (d < 0 || d > 1e9 || d != (double)(long)d) return -1;
    return (long)d;
}

std::string textOf(const Json& j) {
    if (j.isString()) return j.asString();
    if (j.isObject() && j["value"].isString()) return j["value"].asString();   // MarkupContent
    return "";
}

bool before(const Position& a, const Position& b) {
    return a.line != b.line ? a.line < b.line : a.character < b.character;
}

}  // namespace

std::vector<InlayHint> parseInlayHints(const Json& result) {
    std::vector<InlayHint> out;
    if (!result.isArray()) return out;
    for (const Json& h : result.elements()) {
        if (!h.isObject()) continue;
        const long line = wholeNumber(h["position"]["line"]);
        const long ch = wholeNumber(h["position"]["character"]);
        if (line < 0 || ch < 0) continue;
        InlayHint hint;
        hint.position = Position{(int)line, (int)ch};
        const Json& label = h["label"];
        if (label.isString()) {
            hint.label = label.asString();
        } else if (label.isArray()) {
            // InlayHintLabelPart[]: the parts read as one label (a part's
            // own location and command, for Cmd+click, are not used).
            for (const Json& part : label.elements()) {
                hint.label += part["value"].asString();
                if (hint.tooltip.empty()) hint.tooltip = textOf(part["tooltip"]);
            }
        }
        if (hint.label.empty()) continue;
        const long kind = wholeNumber(h["kind"]);
        if (kind == 1) hint.kind = InlayHintKind::Type;
        else if (kind == 2) hint.kind = InlayHintKind::Parameter;
        hint.paddingLeft = h["paddingLeft"].asBool();
        hint.paddingRight = h["paddingRight"].asBool();
        const std::string tip = textOf(h["tooltip"]);
        if (!tip.empty()) hint.tooltip = tip;
        if (h["textEdits"].isArray())
            for (const Json& e : h["textEdits"].elements()) {
                if (!e.isObject() || !e["range"].isObject() || !e["newText"].isString()) continue;
                const Json& r = e["range"];
                if (wholeNumber(r["start"]["line"]) < 0 || wholeNumber(r["start"]["character"]) < 0 ||
                    wholeNumber(r["end"]["line"]) < 0 || wholeNumber(r["end"]["character"]) < 0)
                    continue;
                hint.textEdits.push_back(TextEdit{parseRange(r), e["newText"].asString()});
            }
        out.push_back(std::move(hint));
    }
    std::stable_sort(out.begin(), out.end(), [](const InlayHint& a, const InlayHint& b) {
        return before(a.position, b.position);
    });
    return out;
}

void shortenInlayLabels(std::vector<InlayHint>& hints, size_t maximumLength) {
    if (maximumLength == 0) return;
    std::vector<InlayHint> kept;
    kept.reserve(hints.size());
    int line = -1;
    size_t total = 0;
    for (InlayHint& h : hints) {
        if (h.position.line != line) {
            line = h.position.line;
            total = 0;
        }
        if (total > maximumLength) continue;   // the line is full: VS Code drops the rest
        std::u16string u = toUtf16(h.label);
        total += u.size();
        if (total > maximumLength) {
            // VS Code's label.slice(0, -over) + '…', moved back off a low
            // surrogate so no character is cut in two.
            const size_t over = total - maximumLength;
            size_t keep = over >= u.size() ? 0 : u.size() - over;
            if (keep > 0 && keep < u.size() && u[keep] >= 0xDC00 && u[keep] <= 0xDFFF) keep--;
            u.resize(keep);
            u += u'…';
            h.label = toUtf8(u);
        }
        kept.push_back(std::move(h));
    }
    hints.swap(kept);
}

bool inlayWordChar(char16_t c) {
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') ||
           c == u'_' || c >= 0x80;
}

std::vector<PlacedInlayHint> placeInlayHints(const std::vector<InlayHint>& hints,
                                             const std::u16string& window, size_t base,
                                             int firstLine) {
    std::vector<PlacedInlayHint> out;
    if (window.empty()) return out;
    const PositionIndex index(window);
    const long lines = (long)index.lineCount();
    for (size_t i = 0; i < hints.size(); i++) {
        const Position& p = hints[i].position;
        const long rel = (long)p.line - firstLine;
        if (rel < 0 || rel >= lines) continue;
        // The window ends after a line break, which makes an empty last
        // "line" there: that line belongs to the next window.
        if (rel == lines - 1 && rel > 0 && (window.back() == u'\n' || window.back() == u'\r')) continue;
        const size_t at = index.offset(Position{(int)rel, p.character});
        // At a line's start (the window starts one) it goes with what
        // follows it.
        const char16_t prev = at == 0 ? u'\n' : window[at - 1];
        PlacedInlayHint placed;
        placed.offset = base + at;
        placed.followsBefore = prev != u'\n' && prev != u'\r' && inlayWordChar(prev);
        placed.hint = i;
        out.push_back(placed);
    }
    return out;
}

bool inlayFollowEdit(size_t& offset, bool followsBefore, size_t start, size_t oldLen,
                     size_t newLen) {
    const size_t end = start + oldLen;
    if (start > offset) return true;              // after it
    if (start == offset) {                        // right at it
        if (oldLen == 0 && followsBefore) offset += newLen;
        return true;
    }
    if (end < offset) {                           // before it
        offset = offset - oldLen + newLen;
        return true;
    }
    if (end == offset) {                          // ending right at it
        offset = start + newLen;
        return true;
    }
    return false;                                 // over it
}

std::vector<OffsetEdit> inlayHintAcceptEdits(const InlayHint& hint, const std::u16string& text,
                                             bool fresh, size_t offsetNow) {
    std::vector<OffsetEdit> out;
    if (hint.textEdits.empty()) return out;
    if (!fresh) {
        // Typed since: only what goes in at the hint itself, which has moved
        // with its text.
        if (offsetNow > text.size()) return out;
        for (const TextEdit& e : hint.textEdits) {
            if (positionBefore(e.range.start, hint.position) ||
                positionBefore(hint.position, e.range.start) ||
                positionBefore(e.range.start, e.range.end) || positionBefore(e.range.end, e.range.start))
                return {};
        }
        for (auto it = hint.textEdits.rbegin(); it != hint.textEdits.rend(); ++it)
            out.push_back(OffsetEdit{offsetNow, 0, toUtf16(it->newText)});
        return out;
    }
    // In order (an import at the top comes after the label's place in
    // basedpyright's list), insertions at one place in the server's order.
    std::vector<TextEdit> edits = hint.textEdits;
    std::stable_sort(edits.begin(), edits.end(), [](const TextEdit& a, const TextEdit& b) {
        return positionBefore(a.range.start, b.range.start);
    });
    const PositionIndex index(text);
    const long lines = (long)index.lineCount();
    for (size_t i = 0; i < edits.size(); i++) {
        if (edits[i].range.end.line >= lines || positionBefore(edits[i].range.end, edits[i].range.start))
            return {};
        if (i > 0 && positionBefore(edits[i].range.start, edits[i - 1].range.end)) return {};
    }
    return offsetEdits(text, edits);
}

void LineBreakCounter::feed(const char16_t* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const char16_t c = s[i];
        if (c == u'\r') lines_++;
        else if (c == u'\n' && !cr_) lines_++;
        cr_ = c == u'\r';
    }
}

}  // namespace Lsp
