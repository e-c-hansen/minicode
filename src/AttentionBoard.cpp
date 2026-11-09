// AttentionBoard.cpp — see AttentionBoard.h.
#include "AttentionBoard.h"
#include "AgentIndex.h"

namespace Attention {

Mark strongest(Mark a, Mark b) {
    if (a == Mark::Waiting || b == Mark::Waiting) return Mark::Waiting;
    if (a == Mark::Done || b == Mark::Done) return Mark::Done;
    return Mark::None;
}

bool Board::apply(int terminal, ClaudeHooks::Action action, const std::string &message,
                  const Session &session, double now) {
    if (terminal <= 0) return false;
    Entry &e = entries_[terminal];
    if (!session.empty()) {
        e.session = session;
        e.sessionTime = now;
    }
    const Mark oldMark = e.mark;
    const std::string oldMessage = e.message;
    switch (action) {
    case ClaudeHooks::Action::Waiting:
        e.mark = Mark::Waiting;
        e.message = message;
        break;
    case ClaudeHooks::Action::Done:
        e.mark = Mark::Done;
        e.message.clear();
        break;
    case ClaudeHooks::Action::Clear:
        e.mark = Mark::None;
        e.message.clear();
        break;
    case ClaudeHooks::Action::None: break;
    }
    if (e.mark != oldMark) e.markTime = now;
    return e.mark != oldMark || e.message != oldMessage;
}

bool Board::typed(int terminal) {
    auto it = entries_.find(terminal);
    if (it == entries_.end() || it->second.mark == Mark::None) return false;
    it->second.mark = Mark::None;
    it->second.message.clear();
    return true;
}

bool Board::forget(int terminal) {
    auto it = entries_.find(terminal);
    if (it == entries_.end()) return false;
    const bool marked = it->second.mark != Mark::None;
    entries_.erase(it);
    return marked;
}

Mark Board::mark(int terminal) const {
    auto it = entries_.find(terminal);
    return it == entries_.end() ? Mark::None : it->second.mark;
}

std::string Board::message(int terminal) const {
    auto it = entries_.find(terminal);
    return it == entries_.end() ? std::string() : it->second.message;
}

const Session *Board::session(int terminal) const {
    auto it = entries_.find(terminal);
    return it == entries_.end() || it->second.session.empty() ? nullptr : &it->second.session;
}

double Board::sessionTime(int terminal) const {
    auto it = entries_.find(terminal);
    return it == entries_.end() || it->second.session.empty() ? 0 : it->second.sessionTime;
}

int Board::terminalFor(const std::string &sessionId, const std::string &transcript) const {
    int best = 0;
    double bestTime = -1;
    // By id first; the transcript only when no terminal names the id.
    for (int pass = 0; pass < 2 && !best; pass++) {
        for (const auto &kv : entries_) {
            const Session &s = kv.second.session;
            bool match = pass == 0 ? !sessionId.empty() && s.sessionId == sessionId
                                   : !transcript.empty() && s.transcriptPath == transcript;
            if (match && kv.second.sessionTime > bestTime) {
                best = kv.first;
                bestTime = kv.second.sessionTime;
            }
        }
    }
    return best;
}

int Board::waitingCount() const {
    int n = 0;
    for (const auto &kv : entries_) n += kv.second.mark == Mark::Waiting;
    return n;
}

int Board::nextWaiting(const std::vector<int> &order, int current) const {
    const size_t n = order.size();
    if (n == 0) return 0;
    size_t start = 0;
    for (size_t i = 0; i < n; i++)
        if (order[i] == current) start = i + 1;
    for (size_t k = 0; k < n; k++) {
        int t = order[(start + k) % n];
        if (mark(t) == Mark::Waiting) return t;
    }
    return 0;
}

std::vector<bool> preferSessionMatches(const std::vector<std::string> &matchPaths,
                                       const std::vector<std::string> &sessionRoots) {
    std::vector<bool> keep(matchPaths.size(), false);
    bool any = false;
    for (size_t i = 0; i < matchPaths.size(); i++) {
        const std::string p = AgentIndex::normalizePath(matchPaths[i]);
        for (const std::string &root : sessionRoots) {
            if (root.empty()) continue;
            if (AgentIndex::isInside(p, AgentIndex::normalizePath(root))) {
                keep[i] = true;
                any = true;
            }
        }
    }
    if (!any) keep.assign(matchPaths.size(), true);
    return keep;
}

}  // namespace Attention
