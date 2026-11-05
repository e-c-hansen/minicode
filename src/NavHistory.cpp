// NavHistory.cpp — see NavHistory.h.
#include "NavHistory.h"

namespace Nav {

bool near(const Place& a, const Place& b) {
    if (a.path != b.path) return false;
    if (!a.hasText || !b.hasText) return true;
    int d = a.line - b.line;
    return (d < 0 ? -d : d) <= kFarLines;
}

void History::push(const Place& p) {
    if (list_.empty()) {
        list_.push_back(p);
        cur_ = 0;
        return;
    }
    list_.resize(cur_ + 1);   // what lay ahead goes
    if (near(list_[cur_], p)) {
        list_[cur_] = p;
        return;
    }
    list_.push_back(p);
    cur_++;
    trim();
}

void History::trim() {
    while (list_.size() > cap_ && cap_ > 0) {
        list_.erase(list_.begin());
        if (cur_ > 0) cur_--;
    }
}

void History::jump(const Place& from, const Place& to) {
    push(from);
    push(to);
}

bool History::canGoBack(const Place& here) const {
    if (list_.empty()) return false;
    return cur_ > 0 || !near(list_[cur_], here);
}

bool History::canGoForward() const { return !list_.empty() && cur_ + 1 < list_.size(); }

bool History::back(const Place& here, Place* out) {
    if (list_.empty()) return false;
    if (near(list_[cur_], here)) {
        list_[cur_] = here;
    } else {
        // Wandered off since the last jump: remember where, for Forward.
        list_.resize(cur_ + 1);
        list_.push_back(here);
        cur_++;
        trim();
    }
    if (cur_ == 0) return false;
    cur_--;
    if (out) *out = list_[cur_];
    return true;
}

bool History::forward(const Place& here, Place* out) {
    if (!canGoForward()) return false;
    if (near(list_[cur_], here)) list_[cur_] = here;
    cur_++;
    if (out) *out = list_[cur_];
    return true;
}

void History::moved(const std::string& from, const std::string& to) {
    const std::string prefix = from + "/";
    for (Place& p : list_) {
        if (p.path == from) p.path = to;
        else if (p.path.compare(0, prefix.size(), prefix) == 0)
            p.path = to + "/" + p.path.substr(prefix.size());
    }
}

void History::removeIf(const std::function<bool(const std::string& path)>& gone) {
    if (list_.empty()) return;
    std::vector<Place> kept;
    size_t cur = 0;
    bool curSet = false;
    for (size_t i = 0; i < list_.size(); i++) {
        if (gone(list_[i].path)) {
            if (i == cur_ && !curSet) {   // the current one goes: the one before it
                cur = kept.empty() ? 0 : kept.size() - 1;
                curSet = true;
            }
            continue;
        }
        if (!kept.empty() && near(kept.back(), list_[i])) {
            kept.back() = list_[i];
        } else {
            kept.push_back(list_[i]);
        }
        if (i == cur_) {
            cur = kept.size() - 1;
            curSet = true;
        }
    }
    list_ = kept;
    cur_ = list_.empty() ? 0 : (cur < list_.size() ? cur : list_.size() - 1);
}

}  // namespace Nav
