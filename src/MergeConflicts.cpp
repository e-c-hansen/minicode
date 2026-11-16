// MergeConflicts.cpp — see MergeConflicts.h.
#include "MergeConflicts.h"

#include <algorithm>

namespace MergeConflicts {

namespace {

// UTF-16 to UTF-8, for the labels (a lone surrogate becomes U+FFFD).
std::string toUtf8(const std::u16string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        unsigned c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 &&
            s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}
std::string toUtf8(const std::string& s) { return s; }

enum class Marker { None, Start, Base, Separator, End };

// What line [from, to) (without its break) is: a marker is seven of its
// characters, then nothing, or a space or tab and a label; "=======" takes
// only blanks after it.
template <class S>
Marker markerOf(const S& t, size_t from, size_t to, S* label) {
    if (to - from < 7) return Marker::None;
    const auto c = t[from];
    Marker m;
    switch (c) {
    case '<': m = Marker::Start; break;
    case '|': m = Marker::Base; break;
    case '=': m = Marker::Separator; break;
    case '>': m = Marker::End; break;
    default: return Marker::None;
    }
    for (size_t i = from + 1; i < from + 7; i++)
        if (t[i] != c) return Marker::None;
    size_t i = from + 7;
    if (i == to) {
        label->clear();
        return m;
    }
    if (t[i] != ' ' && t[i] != '\t') return Marker::None;
    if (m == Marker::Separator) {
        for (; i < to; i++)
            if (t[i] != ' ' && t[i] != '\t') return Marker::None;
        label->clear();
        return m;
    }
    while (i < to && (t[i] == ' ' || t[i] == '\t')) i++;
    size_t e = to;
    while (e > i && (t[e - 1] == ' ' || t[e - 1] == '\t')) e--;
    *label = t.substr(i, e - i);
    return m;
}

template <class S>
std::vector<Block> parseText(const S& t) {
    std::vector<Block> out;
    enum class State { Outside, Current, Base, Incoming } state = State::Outside;
    Block b;
    S label;
    const size_t n = t.size();
    for (size_t at = 0; at < n;) {
        size_t nl = at;
        while (nl < n && t[nl] != '\n') nl++;
        const size_t next = nl < n ? nl + 1 : n;   // the next line's start
        size_t content = nl;                        // without "\r" before "\n"
        if (content > at && t[content - 1] == '\r') content--;
        const Marker m = markerOf(t, at, content, &label);
        if (m == Marker::Start) {
            // A start inside an unfinished block: that one was not a block.
            b = Block();
            b.start = at;
            b.currentStart = next;
            b.currentLabel = toUtf8(label);
            state = State::Current;
        } else if (state == State::Current && m == Marker::Base) {
            b.currentEnd = at;
            b.baseMarker = at;
            b.baseStart = next;
            b.baseLabel = toUtf8(label);
            state = State::Base;
        } else if ((state == State::Current || state == State::Base) && m == Marker::Separator) {
            if (state == State::Current) b.currentEnd = at;
            else b.baseEnd = at;
            b.separator = at;
            b.incomingStart = next;
            state = State::Incoming;
        } else if ((state == State::Current || state == State::Base) && m == Marker::End) {
            state = State::Outside;   // no separator: left alone
        } else if (state == State::Incoming && m == Marker::End) {
            b.incomingEnd = at;
            b.endMarker = at;
            b.end = next;
            b.incomingLabel = toUtf8(label);
            out.push_back(b);
            state = State::Outside;
        }
        at = next;
    }
    return out;
}

template <class S>
BasicSplice<S> resolveText(const S& t, const Block& b, Keep keep) {
    BasicSplice<S> s;
    s.start = b.start;
    s.length = b.end - b.start;
    if (keep != Keep::Incoming) s.text += t.substr(b.currentStart, b.currentEnd - b.currentStart);
    if (keep != Keep::Current) s.text += t.substr(b.incomingStart, b.incomingEnd - b.incomingStart);
    // The block ends the text with no break after ">>>>>>>": neither does
    // what is kept.
    const bool endsWithBreak = b.end > b.start && t[b.end - 1] == '\n';
    if (!endsWithBreak && !s.text.empty() && s.text.back() == '\n') {
        s.text.pop_back();
        if (!s.text.empty() && s.text.back() == '\r') s.text.pop_back();
    }
    return s;
}

std::string plural(size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

}  // namespace

std::vector<Block> parse(const std::u16string& text) { return parseText(text); }
std::vector<Block> parse(const std::string& text) { return parseText(text); }

bool mayHold(const std::u16string& text) {
    return text.find(u"<<<<<<<") != std::u16string::npos;
}

Splice resolve(const std::u16string& text, const Block& b, Keep keep) {
    return resolveText(text, b, keep);
}
Splice8 resolve(const std::string& text, const Block& b, Keep keep) {
    return resolveText(text, b, keep);
}

std::u16string resolved(const std::u16string& text, const Block& b, Keep keep) {
    const Splice s = resolve(text, b, keep);
    std::u16string out = text;
    out.replace(s.start, s.length, s.text);
    return out;
}

std::u16string resolvedAll(const std::u16string& text, const std::vector<Block>& blocks,
                           Keep keep) {
    std::u16string out = text;
    for (size_t i = blocks.size(); i-- > 0;) {
        const Splice s = resolve(text, blocks[i], keep);
        out.replace(s.start, s.length, s.text);
    }
    return out;
}

void followEdit(std::vector<Block>& blocks, size_t at, size_t removed, size_t inserted) {
    std::vector<Block> kept;
    kept.reserve(blocks.size());
    const size_t editEnd = at + removed;
    const long delta = (long)inserted - (long)removed;
    for (Block b : blocks) {
        if (b.end <= at) {   // wholly before (an edit at its end is the next line's)
            kept.push_back(b);
            continue;
        }
        // Wholly after. An edit ending at its start could join the
        // "<<<<<<<" line to the one before, or put text before it: dropped.
        if (b.start > editEnd) {
            auto move = [delta](size_t& v) {
                if (v != npos) v = (size_t)((long)v + delta);
            };
            move(b.start);
            move(b.end);
            move(b.currentStart);
            move(b.currentEnd);
            move(b.baseMarker);
            move(b.baseStart);
            move(b.baseEnd);
            move(b.separator);
            move(b.incomingStart);
            move(b.incomingEnd);
            move(b.endMarker);
            kept.push_back(b);
        }
        // Otherwise the edit touches the block: gone until the next parse.
    }
    blocks.swap(kept);
}

long blockAt(const std::vector<Block>& blocks, size_t offset) {
    for (size_t i = 0; i < blocks.size(); i++)
        if (offset >= blocks[i].start && offset < blocks[i].end) return (long)i;
    return -1;
}

long step(const std::vector<Block>& blocks, size_t offset, bool forward) {
    if (blocks.empty()) return -1;
    if (forward) {
        for (size_t i = 0; i < blocks.size(); i++)
            if (blocks[i].start > offset) return (long)i;
        return 0;
    }
    const long holding = blockAt(blocks, offset);
    const size_t from = holding >= 0 ? blocks[(size_t)holding].start : offset;
    for (size_t i = blocks.size(); i-- > 0;)
        if (blocks[i].start < from) return (long)i;
    return (long)blocks.size() - 1;
}

std::string buttonTitle(Keep keep) {
    switch (keep) {
    case Keep::Current: return "Keep Current";
    case Keep::Incoming: return "Keep Incoming";
    case Keep::Both: return "Keep Both";
    }
    return "";
}

std::string actionName(Keep keep) { return buttonTitle(keep) + (keep == Keep::Both ? " Changes" : " Change"); }

std::string tooltip(Keep keep, const Block& b) {
    const std::string cur = b.currentLabel.empty() ? "the current side" : b.currentLabel;
    const std::string inc = b.incomingLabel.empty() ? "the incoming side" : b.incomingLabel;
    switch (keep) {
    case Keep::Current: return "Keep the lines from " + cur + " and drop those from " + inc;
    case Keep::Incoming: return "Keep the lines from " + inc + " and drop those from " + cur;
    case Keep::Both: return "Keep the lines from " + cur + ", then those from " + inc;
    }
    return "";
}

std::string leftText(size_t left) {
    if (left == 0) return "No conflicts left";
    return plural(left, "conflict left", "conflicts left");
}

std::string positionText(size_t index, size_t count) {
    return "Conflict " + std::to_string(index + 1) + " of " + std::to_string(count);
}

std::string resolvedNote(const std::string& name, size_t left) {
    if (left == 0)
        return "No conflicts left in " + name +
               ". Stage it in Source Control (or Mark Resolved on the review page) to mark it "
               "resolved.";
    return leftText(left) + " in " + name + ".";
}

}  // namespace MergeConflicts
