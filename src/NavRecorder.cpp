// NavRecorder.cpp — see NavRecorder.h. The Mac's MCNavigator, step for step.
#include "NavRecorder.h"

#include <cstdlib>

namespace Nav {

Recorder::Recorder(Host host, size_t cap) : host_(std::move(host)), history_(cap) {}

bool Recorder::current(Place* out) {
    if (!host_.current) return false;
    size_t length = 0;
    Place p;
    if (!host_.current(&p, &length) || p.path.empty()) return false;
    if (out) *out = p;
    return true;
}

void Recorder::noteCaret() {
    Place p;
    size_t length = 0;
    haveLast_ = host_.current && host_.current(&p, &length) && !p.path.empty() && p.hasText;
    if (haveLast_) {
        last_ = p;
        lastLength_ = length;
    }
}

void Recorder::edited(const std::string& path, size_t pos, size_t oldLen, size_t newLen) {
    history_.edited(path, pos, oldLen, newLen);
}

// Lines an edit may have moved are counted again, in one pass over the
// text, before the history compares or shows them.
void Recorder::fixLines() {
    Place here;
    if (!current(&here) || !here.hasText || !host_.linesOf) return;
    if (!history_.hasStaleLines(here.path)) return;
    history_.fixLines(here.path, host_.linesOf);
}

void Recorder::record(const Place& from, const Place& to) {
    fixLines();
    if (from.path == to.path && from.hasText == to.hasText && from.location == to.location &&
        from.length == to.length)
        return;   // nothing moved (no definition found, a refused open)
    history_.jump(from, to);
}

// A caret that went far without the text changing is a jump of its own.
void Recorder::caretMoved() {
    if (jumpDepth_ || quiet_ || switching_ || navigating_) return;
    Place p;
    size_t length = 0;
    const bool have = host_.current && host_.current(&p, &length) && !p.path.empty() && p.hasText;
    if (have && haveLast_ && p.path == last_.path && length == lastLength_ &&
        std::abs(p.line - last_.line) > kFarLines)
        record(last_, p);
    haveLast_ = have;
    if (have) {
        last_ = p;
        lastLength_ = length;
    }
}

void Recorder::jump(const std::function<void()>& body) {
    const bool outer = jumpDepth_ == 0 && !navigating_;
    if (outer) haveJumpFrom_ = current(&jumpFrom_);
    jumpDepth_++;
    if (body) body();
    jumpDepth_--;
    if (outer) {
        Place to;
        if (haveJumpFrom_ && current(&to)) record(jumpFrom_, to);
        haveJumpFrom_ = false;
        noteCaret();
    }
}

void Recorder::jumpFrom(const Place& from, const std::function<void()>& body) {
    const bool outer = jumpDepth_ == 0 && !navigating_;
    if (outer) {
        jumpFrom_ = from;
        haveJumpFrom_ = !from.path.empty();
    }
    jumpDepth_++;
    if (body) body();
    jumpDepth_--;
    if (outer) {
        Place to;
        if (haveJumpFrom_ && current(&to)) record(jumpFrom_, to);
        haveJumpFrom_ = false;
        noteCaret();
    }
}

void Recorder::beginQuiet() { quiet_++; }

void Recorder::endQuiet() {
    if (quiet_ > 0) quiet_--;
    if (!quiet_) noteCaret();
}

void Recorder::beQuiet(const std::function<void()>& body) {
    beginQuiet();
    if (body) body();
    endQuiet();
}

void Recorder::tabWillSwitch() {
    if (switching_ == 0) fixLines();   // while the text is still the one they count in
    if (switching_++ == 0 && !jumpDepth_ && !navigating_ && !quiet_)
        haveSwitchFrom_ = current(&switchFrom_);
}

void Recorder::tabDidSwitch() {
    if (switching_ == 0) return;
    if (--switching_ > 0) return;
    Place to;
    if (haveSwitchFrom_ && !jumpDepth_ && !navigating_ && !quiet_ && current(&to))
        record(switchFrom_, to);
    haveSwitchFrom_ = false;
    if (!jumpDepth_) noteCaret();
}

// Files deleted since they were recorded are no places to go.
void Recorder::dropMissing() {
    if (!host_.exists) return;
    history_.removeIf([this](const std::string& path) { return !host_.exists(path); });
}

bool Recorder::canGoBack() {
    fixLines();
    Place here;
    if (!current(&here)) return !history_.entries().empty();
    return history_.canGoBack(here);
}

bool Recorder::go(bool back) {
    dropMissing();
    fixLines();
    for (int tries = 0; tries < 50; tries++) {
        Place here, target;
        if (!current(&here)) {
            // Nothing on show (every tab closed): back to the last place.
            if (!back || history_.entries().empty()) break;
            target = history_.entries()[history_.index()];
        } else if (!(back ? history_.back(here, &target) : history_.forward(here, &target))) {
            break;
        }
        navigating_ = true;
        const bool shown = host_.show && host_.show(target);
        navigating_ = false;
        noteCaret();
        if (shown) return true;
        // It could not be opened: forget it and go on past it.
        const std::string gone = target.path;
        history_.removeIf([&gone](const std::string& p) { return p == gone; });
    }
    if (host_.bell) host_.bell();
    return false;
}

void Recorder::moved(const std::string& from, const std::string& to) {
    if (from.empty() || to.empty()) return;
    history_.moved(from, to);
    if (haveLast_) {
        const std::string prefix = from + "/";
        if (last_.path == from) last_.path = to;
        else if (last_.path.compare(0, prefix.size(), prefix) == 0)
            last_.path = to + "/" + last_.path.substr(prefix.size());
    }
}

void Recorder::removed(const std::string& path) {
    if (path.empty()) return;
    const std::string prefix = path + "/";
    history_.removeIf([&](const std::string& x) {
        return x == path || x.compare(0, prefix.size(), prefix) == 0;
    });
}

void Recorder::clear() {
    history_.clear();
    haveLast_ = false;
}

std::vector<std::string> Recorder::entries() {
    fixLines();
    std::vector<std::string> out;
    for (const Place& p : history_.entries()) out.push_back(p.path + ":" + std::to_string(p.line));
    return out;
}

}  // namespace Nav
