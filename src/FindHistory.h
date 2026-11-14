// FindHistory.h — pure C++, header only. What was searched for, for Up and
// Down in a find field, as VS Code's find widget keeps it: newest last, each
// text once (searched again, it moves to the end), at most `limit`.
//
// Up from the field's own text shows the newest entry, or the one before
// it when the field already holds the newest; Up again goes back further
// and stays at the oldest. Down goes forward, and past the newest gives
// back what the field held before Up. Typing ends the walk (reset).
#pragma once

#include <algorithm>
#include <string>
#include <vector>

class FindHistory {
public:
    explicit FindHistory(size_t limit = 50) : limit_(limit) {}

    void add(const std::string& text) {
        reset();
        if (text.empty()) return;
        entries_.erase(std::remove(entries_.begin(), entries_.end(), text), entries_.end());
        entries_.push_back(text);
        if (entries_.size() > limit_) entries_.erase(entries_.begin());
    }

    // Up: false when there is nothing older (the field stays as it is).
    bool previous(const std::string& current, std::string& out) {
        if (pos_ < 0) {
            size_t start = entries_.size();
            if (start > 0 && entries_.back() == current) start--;
            if (start == 0) return false;
            typed_ = current;
            pos_ = (long)start - 1;
        } else if (pos_ > 0) {
            pos_--;
        } else {
            return false;
        }
        out = entries_[(size_t)pos_];
        return true;
    }

    // Down: false when no walk is under way.
    bool next(std::string& out) {
        if (pos_ < 0) return false;
        if ((size_t)pos_ + 1 < entries_.size()) {
            pos_++;
            out = entries_[(size_t)pos_];
        } else {
            pos_ = -1;
            out = typed_;
        }
        return true;
    }

    void reset() { pos_ = -1; }
    const std::vector<std::string>& entries() const { return entries_; }

private:
    size_t limit_;
    std::vector<std::string> entries_;
    long pos_ = -1;        // the entry on show, or -1 when not walking
    std::string typed_;    // the field's text when the walk began
};
