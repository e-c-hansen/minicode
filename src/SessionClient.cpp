// SessionClient.cpp — see SessionClient.h.
#include "SessionClient.h"

#include <algorithm>
#include <chrono>

namespace Shared {

Client::Client(Model::Unit unit, std::string name) : unit_(unit), name_(std::move(name)) {}

Client::~Client() = default;

void Client::connect(Send send) {
    send_ = std::move(send);
    Message h;
    h.kind = Kind::Hello;
    h.unit = unit_;
    h.name = name_;
    if (readOnly_) h.flag = 1;
    else if (guest_) h.flag = 2;
    else if (keepsBackups_) h.flag = 4;
    if (hearsDirty_) h.flag |= 8;
    if (keepsBackups_ && !readOnly_ && !guest_ && savesThroughHost_) h.flag |= 16;
    if (hearsDisk_) h.flag |= 32;
    send_(std::move(h));
}

std::uint64_t Client::newId(int timeoutMs) {
    if (!idBase_ && send_ && !lost_) settle(timeoutMs);
    if (!idBase_ || idsUsed_ + 1 >= kIdRange) return 0;
    return idBase_ + ++idsUsed_;
}

Client::Doc* Client::doc(DocId id) {
    auto it = docs_.find(id);
    return it == docs_.end() ? nullptr : &it->second;
}

std::uint64_t Client::version(DocId id) const {
    auto it = docs_.find(id);
    return it == docs_.end() ? 0 : it->second.known;
}

// ---------------------------------------------------------------- sending

void Client::queue(Message&& m) { outbox_.push_back(std::move(m)); }

// In order; an edit (or a reset) goes only when nothing of this renderer's is
// in flight, so the host moves it past nothing of its own.
void Client::pump() {
    if (!send_) return;
    if (lost_) {   // nothing more reaches the host, and nothing waits for it
        outbox_.clear();
        inFlight_ = false;
        return;
    }
    while (!inFlight_ && !outbox_.empty()) {
        Message m = std::move(outbox_.front());
        outbox_.pop_front();
        if (m.doc) {
            m.base = version(m.doc);
            if (Doc* d = doc(m.doc)) d->told = m.base;
        }
        // A detached document is forgotten once its last message is out
        // (an open() since replaced the entry, which stays).
        if (m.kind == Kind::DocDetach) {
            auto it = docs_.find(m.doc);
            if (it != docs_.end() && it->second.detaching) docs_.erase(it);
        }
        const bool needsAck = (m.kind == Kind::DocEdit && !m.ops.empty()) || m.kind == Kind::DocReset;
        if (needsAck) {
            m.seq = ++seq_;
            inFlight_ = true;
            head_ = m;
            head_.text8.clear();
        }
        send_(std::move(m));
    }
}

void Client::open(DocId id, const std::string& path, std::string text8, Listener* listener) {
    Doc& d = docs_[id];
    d = Doc();
    d.listener = listener;
    Message m;
    m.kind = Kind::DocOpen;
    m.doc = id;
    m.name = path;
    m.text8 = std::move(text8);
    queue(std::move(m));
    pump();
}

void Client::view(DocId id, Listener* listener) { open(id, std::string(), std::string(), listener); }

void Client::setPath(DocId id, const std::string& path) {
    Doc* d = doc(id);
    if (!d || d->detaching) return;
    Message m;
    m.kind = Kind::DocPath;
    m.doc = id;
    m.name = path;
    queue(std::move(m));
    pump();
}

void Client::detach(DocId id) {
    Doc* d = doc(id);
    if (!d || d->detaching) return;
    // Its edits waiting in the outbox still go, before the DocDetach; the
    // entry stays (for their bases and acknowledgements) until that is sent.
    d->listener = nullptr;
    d->watchers.clear();
    d->detaching = true;
    Message m;
    m.kind = Kind::DocDetach;
    m.doc = id;
    queue(std::move(m));
    pump();
}

void Client::reset(DocId id, std::string text8) {
    Doc* d = doc(id);
    if (!d) return;
    // A step open around the reset goes on as a new step on the new text.
    if (d->depth > 0) {
        d->stepId = ++nextStep_;
        d->announced = false;
        d->before.clear();
    }
    d->lastAfter.clear();
    d->moved = d->broken = true;
    Message m;
    m.kind = Kind::DocReset;
    m.doc = id;
    m.text8 = std::move(text8);
    queue(std::move(m));
    pump();
}

void Client::close(DocId id) {
    if (!docs_.erase(id)) return;
    // What it had not sent yet no longer matters.
    outbox_.erase(std::remove_if(outbox_.begin(), outbox_.end(),
                                 [id](const Message& m) { return m.doc == id; }),
                  outbox_.end());
    Message m;
    m.kind = Kind::DocClose;
    m.doc = id;
    queue(std::move(m));
    pump();
}

// ---------------------------------------------------------------- steps

void Client::openStep(DocId id, const Model::UndoHistory::StepInfo& info, const Selection& before) {
    Doc* d = doc(id);
    if (!d || d->depth++ > 0) return;
    d->stepId = ++nextStep_;
    d->announced = false;
    d->before = before;
    d->info = info;
    // Typing joins the step before only where that one left the caret, with
    // nothing moving it or breaking the run since: the host checks the
    // first, the renderer knows the rest.
    d->fresh = d->moved || d->broken;
}

void Client::renameStep(DocId id, const Model::UndoHistory::StepInfo& info) {
    Doc* d = doc(id);
    if (!d || d->depth == 0) return;
    d->info = info;
    if (!outbox_.empty()) {
        Message& tail = outbox_.back();
        if (tail.kind == Kind::DocEdit && tail.doc == id && tail.step.id == d->stepId) {
            tail.step.name = d->info.name;
            tail.step.coalesce = d->info.coalesce;
        }
    }
}

bool Client::stepOpen(DocId id) const {
    auto it = docs_.find(id);
    return it != docs_.end() && it->second.depth > 0;
}

void Client::record(DocId id, Ot::Op op) {
    Doc* d = doc(id);
    if (!d) return;
    if (d->depth == 0) {
        openStep(id, {"Edit", false}, d->lastAfter);
        record(id, std::move(op));
        closeStep(id, d->lastAfter);
        return;
    }
    if (!outbox_.empty()) {
        Message& tail = outbox_.back();
        if (tail.kind == Kind::DocEdit && tail.doc == id && tail.step.id == d->stepId && !tail.step.close) {
            tail.ops.push_back(std::move(op));
            return;
        }
    }
    Message m;
    m.kind = Kind::DocEdit;
    m.doc = id;
    m.step.id = d->stepId;
    m.step.name = d->info.name;
    m.step.coalesce = d->info.coalesce;
    if (!d->announced) {
        m.step.open = true;
        m.step.fresh = d->fresh;
        m.step.before = d->before;
    }
    d->announced = true;
    m.ops.push_back(std::move(op));
    queue(std::move(m));
}

void Client::closeStep(DocId id, const Selection& after) {
    Doc* d = doc(id);
    if (!d || d->depth == 0 || --d->depth > 0) return;
    if (d->announced) {
        Message* tail = outbox_.empty() ? nullptr : &outbox_.back();
        if (tail && tail->kind == Kind::DocEdit && tail->doc == id && tail->step.id == d->stepId &&
            !tail->step.close) {
            tail->step.close = true;
            tail->step.after = after;
            tail->step.name = d->info.name;
            tail->step.coalesce = d->info.coalesce;
        } else {
            Message m;
            m.kind = Kind::DocEdit;
            m.doc = id;
            m.step.id = d->stepId;
            m.step.name = d->info.name;
            m.step.coalesce = d->info.coalesce;
            m.step.close = true;
            m.step.after = after;
            queue(std::move(m));
        }
        d->lastAfter = after;
        d->moved = d->broken = false;
    }
    d->announced = false;
    pump();
}

void Client::recordOutside(DocId id, Ot::Op op) {
    Doc* d = doc(id);
    if (!d || (op.del == 0 && op.ins.empty())) return;
    d->broken = true;
    if (!outbox_.empty()) {
        Message& tail = outbox_.back();
        if (tail.kind == Kind::DocEdit && tail.doc == id && tail.flag == 1) {
            tail.ops.push_back(std::move(op));
            return;
        }
    }
    Message m;
    m.kind = Kind::DocEdit;
    m.doc = id;
    m.flag = 1;
    m.ops.push_back(std::move(op));
    queue(std::move(m));
    pump();
}

void Client::forgetHistory(DocId id) {
    Doc* d = doc(id);
    if (!d) return;
    d->lastAfter.clear();
    d->moved = d->broken = true;
    Message m;
    m.kind = Kind::DocForget;
    m.doc = id;
    queue(std::move(m));
    pump();
}

void Client::breakCoalescing(DocId id) {
    if (Doc* d = doc(id)) d->broken = true;
}

void Client::noteSelection(DocId id, const Selection& now) {
    Doc* d = doc(id);
    if (d && d->depth == 0 && now != d->lastAfter) d->moved = true;
}

std::size_t Client::unconfirmed(DocId id) const {
    std::size_t n = inFlight_ && head_.doc == id ? 1 : 0;
    for (const Message& m : outbox_) n += m.doc == id ? 1 : 0;
    return n;
}

void Client::saved(DocId id) {
    if (!doc(id)) return;
    Message m;
    m.kind = Kind::DocSaved;
    m.doc = id;
    queue(std::move(m));
    fenceDirty(id);
    pump();
}

// Behind a DocSaved: a Ping whose Pong says the host has taken it, so a
// DocDirty the host sent before it (an edit's, acknowledged first) is not
// told after the renderer counted the document saved (SessionClient.h).
void Client::fenceDirty(DocId id) {
    if (!dirtyListener_) return;
    Message p;
    p.kind = Kind::Ping;
    p.req = ++nextReq_;
    fencePings_[p.req] = id;
    fenced_[id].out++;
    queue(std::move(p));
}

bool Client::savedAs(DocId id, std::u16string text) {
    if (!doc(id) || !hostSavesForApps()) return false;
    Message m;
    m.kind = Kind::DocSaved;
    m.doc = id;
    m.flag = 1;
    m.text16 = std::move(text);
    queue(std::move(m));
    fenceDirty(id);
    pump();
    return true;
}

std::uint64_t Client::requestSave(DocId id, std::uint64_t window, bool overwrite, const std::string& encoding) {
    if (!doc(id) || !(hostFlags_ & 1)) return 0;
    Message m;
    m.kind = Kind::DocSave;
    m.doc = id;
    m.window = window;
    m.flag = overwrite ? 1 : 0;
    if (!encoding.empty() && hostSavesForApps()) {
        m.json = Json::object();
        m.json.set("encoding", Json(encoding));
    }
    m.req = ++nextReq_;
    const std::uint64_t req = m.req;
    // Behind this renderer's edits, so what is written holds them.
    queue(std::move(m));
    pump();
    return req;
}

bool Client::saveAnswer(std::uint64_t req, SaveAnswer* out) {
    auto it = replies_.find(req);
    if (it == replies_.end() || it->second.kind != Kind::DocSaveDone) return false;
    if (out) {
        out->ok = it->second.outcome == Outcome::Done;
        out->version = it->second.v;
        out->json = std::move(it->second.json);
    }
    replies_.erase(it);
    return true;
}

std::uint64_t Client::requestReload(DocId id) {
    if (!doc(id) || !hostWatches()) return 0;
    Message m;
    m.kind = Kind::DocReload;
    m.doc = id;
    m.req = ++nextReq_;
    const std::uint64_t req = m.req;
    // Behind this renderer's edits, so the step it makes follows them.
    queue(std::move(m));
    pump();
    return req;
}

std::uint64_t Client::watchFolders(std::uint64_t window, const std::vector<std::string>& open, bool hidden) {
    if (!hostWatches()) return 0;
    Json req = Json::object();
    req.set("op", Json("watch"));
    Json paths = Json::array();
    for (const std::string& p : open) paths.push(Json(p));
    req.set("paths", std::move(paths));
    req.set("hidden", Json(hidden));
    return requestRead(window, std::move(req));
}

bool Client::reloadAnswer(std::uint64_t req, SaveAnswer* out) {
    auto it = replies_.find(req);
    if (it == replies_.end() || it->second.kind != Kind::DocReloadDone) return false;
    if (out) {
        out->ok = it->second.outcome == Outcome::Done;
        out->version = it->second.v;
        out->json = std::move(it->second.json);
    }
    replies_.erase(it);
    return true;
}

// ---------------------------------------------------------------- receiving

void Client::setWake(std::function<void()> wake) {
    std::lock_guard<std::mutex> lk(mu_);
    wake_ = std::move(wake);
}

void Client::deliver(Message&& m) {
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (inbox_.empty()) wake = wake_;
        inbox_.push_back(std::move(m));
    }
    cv_.notify_all();
    if (wake) wake();
}

bool Client::take(Message* out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (inbox_.empty()) return false;
    *out = std::move(inbox_.front());
    inbox_.pop_front();
    return true;
}

void Client::dispatch() {
    std::size_t queued;
    {
        std::lock_guard<std::mutex> lk(mu_);
        queued = inbox_.size();
    }
    Message m;
    while (queued-- && take(&m)) process(std::move(m));
    // Messages that came while those were taken found the queue not empty and
    // called no wake: call it now, so they are not left until the next.
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!inbox_.empty()) wake = wake_;
    }
    if (wake) wake();
}

void Client::process(Message&& m) {
    switch (m.kind) {
    case Kind::Welcome:
        id_ = m.client;
        hostFlags_ = m.flag;
        if (m.v != idBase_) {
            idBase_ = m.v;
            idsUsed_ = 0;
        }
        break;
    case Kind::DocOpened:
        if (Doc* d = doc(m.doc)) {
            d->known = m.v;
            if (!m.text16.empty() && d->listener)
                d->listener->hostEdited(m.doc, Ot::Ops{Ot::Op{0, 0, m.text16}}, 0, nullptr);
        }
        break;
    case Kind::DocEdited: onEdited(m); break;
    case Kind::UndoState:
        if (Doc* d = doc(m.doc)) d->undo = m.undo;
        break;
    case Kind::DocUndone:
        if (Doc* d = doc(m.doc)) d->broken = true;
        if (!abandoned_.erase(m.req)) replies_[m.req] = std::move(m);
        break;
    case Kind::DocDirty: {
        if ((m.flag & 2) && writtenListener_) writtenListener_(m.doc, m.v, m.json);
        // Behind a DocSaved the host has not taken yet: held until it has.
        auto f = fenced_.find(m.doc);
        if (f != fenced_.end()) {
            f->second.held = true;
            f->second.dirty = (m.flag & 1) != 0;
        } else if (dirtyListener_) {
            dirtyListener_(m.doc, (m.flag & 1) != 0);
        }
        break;
    }
    case Kind::FsChanged:
        if (foldersListener_) foldersListener_(m.window, m.json);
        break;
    case Kind::DocDisk:
        if (diskListener_) diskListener_(m.doc, m.flag);
        break;
    case Kind::Pong: {
        auto p = fencePings_.find(m.req);
        if (p == fencePings_.end()) {
            if (!abandoned_.erase(m.req)) replies_[m.req] = std::move(m);
            break;
        }
        const DocId id = p->second;
        fencePings_.erase(p);
        auto f = fenced_.find(id);
        if (f == fenced_.end() || --f->second.out > 0) break;
        const Fence was = f->second;
        fenced_.erase(f);
        // The latest the host said once it had taken every DocSaved; nothing
        // heard, nothing flipped since the listener was last told.
        if (was.held && dirtyListener_) dirtyListener_(id, was.dirty);
        break;
    }
    case Kind::DocText:
    case Kind::State:
    case Kind::FsData:
    case Kind::DocSaveDone:
    case Kind::DocReloadDone: {
        const std::uint64_t req = m.req;
        if (!abandoned_.erase(req)) replies_[req] = std::move(m);
        break;
    }
    case Kind::Error: errors_.push_back(m.name); break;
    case Kind::DocGone: {
        auto it = docs_.find(m.doc);
        if (it == docs_.end()) break;
        Listener* l = it->second.listener;
        docs_.erase(it);
        if (l) l->hostGone(m.doc);
        break;
    }
    case Kind::TermOpened:
    case Kind::TermOutput:
    case Kind::TermReplay:
    case Kind::TermShell:
    case Kind::TermMarks:
    case Kind::TermExited: onTerminal(m); break;
    default: break;
    }
}

// ---------------------------------------------------------------- terminals

void Client::sendNow(Message&& m) {
    if (send_ && !lost_) send_(std::move(m));
}

void Client::openTerminal(TermId t, const TermSpec& spec, TermListener* l) {
    terms_[t] = l;
    Message m;
    m.kind = Kind::TermOpen;
    m.term = t;
    m.json = termSpecJson(spec);
    sendNow(std::move(m));
}

void Client::attachTerminal(TermId t, TermListener* l) {
    terms_[t] = l;
    Message m;
    m.kind = Kind::TermAttach;
    m.term = t;
    sendNow(std::move(m));
}

void Client::terminalInput(TermId t, std::string bytes, bool answer) {
    if (bytes.empty() || !terms_.count(t)) return;
    Message m;
    m.kind = Kind::TermInput;
    m.term = t;
    m.flag = answer ? 1 : 0;
    m.bytes = std::move(bytes);
    sendNow(std::move(m));
}

void Client::resizeTerminal(TermId t, int cols, int rows) {
    if (!terms_.count(t)) return;
    Message m;
    m.kind = Kind::TermResize;
    m.term = t;
    m.cols = cols;
    m.rows = rows;
    sendNow(std::move(m));
}

void Client::ackTerminal(TermId t, std::size_t bytes) {
    if (!bytes || !terms_.count(t)) return;
    Message m;
    m.kind = Kind::TermAck;
    m.term = t;
    m.v = bytes;
    sendNow(std::move(m));
}

void Client::closeTerminal(TermId t) {
    if (!terms_.erase(t)) return;
    Message m;
    m.kind = Kind::TermClose;
    m.term = t;
    sendNow(std::move(m));
}

void Client::detachTerminal(TermId t) {
    if (!terms_.erase(t)) return;
    Message m;
    m.kind = Kind::TermDetach;
    m.term = t;
    sendNow(std::move(m));
}

void Client::onTerminal(Message& m) {
    auto it = terms_.find(m.term);
    if (it == terms_.end() || !it->second) return;
    TermListener* l = it->second;
    switch (m.kind) {
    case Kind::TermOpened: l->termOpened(m.term, (int)m.v); break;
    case Kind::TermOutput:
    case Kind::TermReplay:
        l->termOutput(m.term, m.bytes, (long long)m.v, m.flag & 1, m.kind == Kind::TermReplay);
        break;
    case Kind::TermShell: l->termShell(m.term, termShellFrom(m.json)); break;
    case Kind::TermMarks: l->termMarks(m.term, termMarksFrom(m.json)); break;
    case Kind::TermExited:
        terms_.erase(it);   // nothing more comes
        l->termExited(m.term, m.flag, m.name);
        break;
    default: break;
    }
}

Selection Client::mapSelection(const Selection& s, const Ot::Ops& ops) const {
    if (ops.empty()) return s;
    Selection out;
    for (const auto& r : s)
        out.push_back({Ot::mapOffset(r.anchor, ops, unit_), Ot::mapOffset(r.caret, ops, unit_)});
    return out;
}

void Client::onEdited(Message& m) {
    // Its own edit, acknowledged: the next may go.
    if (inFlight_ && m.client == id_ && m.seq && m.seq == head_.seq) {
        inFlight_ = false;
        head_ = Message();
        if (Doc* d = doc(m.doc)) {
            d->known = std::max(d->known, m.v);
            if (d->copy && !m.ops16.empty()) {
                Ot::apply(*d->copy, m.ops16, Model::Unit::Utf16);
                for (auto& [owner, w] : d->watchers) w(m.doc, m.ops16);
            }
        }
        pump();
        return;
    }
    Doc* d = doc(m.doc);
    if (!d) return;
    d->known = m.v;
    if (d->known >= d->told + kReportEvery && !inFlight_ && outbox_.empty()) {
        // Nothing of its own to send: a report of how far it has read.
        Message r;
        r.kind = Kind::DocEdit;
        r.doc = m.doc;
        queue(std::move(r));
        pump();
    }
    if (d->copy && !m.ops16.empty()) {
        Ot::apply(*d->copy, m.ops16, Model::Unit::Utf16);
        for (auto& [owner, w] : d->watchers) w(m.doc, m.ops16);
    }
    // Moved past what this renderer has not had confirmed, which moves past
    // it in turn: the edit in flight, then those waiting, in order.
    Ot::Ops f = m.ops;
    Ot::Ops pending;
    bool resetPending = false;
    if (inFlight_ && head_.doc == m.doc) {
        if (head_.kind == Kind::DocReset) {
            resetPending = true;
        } else {
            Ot::Ops h, f2;
            Ot::transform(head_.ops, f, unit_, &h, &f2);
            head_.ops = std::move(h);
            f = std::move(f2);
            pending = head_.ops;
        }
    }
    for (Message& q : outbox_) {
        if (resetPending) break;
        if (q.doc != m.doc) continue;
        if (q.kind == Kind::DocReset) {
            resetPending = true;
            break;
        }
        if (q.kind != Kind::DocEdit) continue;
        if (q.step.open) q.step.before = mapSelection(q.step.before, f);
        Ot::Ops q2, f2;
        Ot::transform(q.ops, f, unit_, &q2, &f2);
        if (q.step.close) q.step.after = mapSelection(q.step.after, f2);
        q.ops = std::move(q2);
        f = std::move(f2);
        pending.insert(pending.end(), q.ops.begin(), q.ops.end());
    }
    // A reset of this renderer's still to come replaces it all.
    if (resetPending) f.clear();
    if (d->depth > 0 && !d->announced) d->before = mapSelection(d->before, f);
    d->lastAfter = mapSelection(d->lastAfter, f);
    // An undo's selection is in the host's text: through what is pending to
    // the mirror's, and meaningless past a reset of this renderer's.
    const bool hasSelect = m.hasSelect && !resetPending;
    Selection select;
    if (hasSelect) select = mapSelection(m.select, pending);
    if (d->listener && (!f.empty() || hasSelect || m.flag))
        d->listener->hostEdited(m.doc, f, m.flag, hasSelect ? &select : nullptr);
}

bool Client::waitFor(const std::function<bool()>& done, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        Message m;
        while (take(&m)) {
            process(std::move(m));
            if (done()) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
        }
        if (done()) return true;
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_until(lk, deadline, [this] { return !inbox_.empty() || lost_; })) return false;
        if (inbox_.empty()) return false;   // the host is gone: nothing more is coming
    }
}

void Client::hostLost() {
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (lost_.exchange(true)) return;
        wake = wake_;
    }
    cv_.notify_all();
    if (wake) wake();
}

bool Client::ask(Message&& m, Message* reply, int timeoutMs) {
    const std::uint64_t req = ++nextReq_;
    m.req = req;
    queue(std::move(m));
    pump();
    const bool ok = waitFor([&] { return replies_.count(req) > 0; }, timeoutMs);
    if (ok && reply) *reply = std::move(replies_[req]);
    replies_.erase(req);
    // Its answer, should it come after all, is dropped (process()).
    if (!ok) abandoned_.insert(req);
    return ok;
}

bool Client::settle(int timeoutMs) {
    Message p;
    p.kind = Kind::Ping;
    return ask(std::move(p), nullptr, timeoutMs);
}

// ---------------------------------------------------------------- undo

UndoInfo Client::undoInfo(DocId id) {
    if (lost_) return UndoInfo();   // nothing can be undone with the host gone
    settle();
    Doc* d = doc(id);
    return d ? d->undo : UndoInfo();
}

Outcome Client::undo(DocId id, bool redo) {
    if (!doc(id)) return Outcome::Nothing;
    Message u;
    u.kind = Kind::DocUndo;
    u.doc = id;
    u.flag = redo ? 1 : 0;
    Message reply;
    if (!ask(std::move(u), &reply)) return Outcome::Nothing;
    return reply.outcome;
}

std::uint64_t Client::requestUndo(DocId id, bool redo) {
    if (!doc(id)) return 0;
    Message u;
    u.kind = Kind::DocUndo;
    u.doc = id;
    u.flag = redo ? 1 : 0;
    u.req = ++nextReq_;
    const std::uint64_t req = u.req;
    queue(std::move(u));
    pump();
    return req;
}

bool Client::outcome(std::uint64_t req, Outcome* out) {
    auto it = replies_.find(req);
    if (it == replies_.end()) return false;
    if (out) *out = it->second.outcome;
    replies_.erase(it);
    return true;
}

// ---------------------------------------------------------------- the host's text

const Model::Text* Client::watch(DocId id, const void* owner, Watcher w, int timeoutMs) {
    Doc* d = doc(id);
    if (!d) return nullptr;
    bool found = false;
    for (auto& [o, f] : d->watchers)
        if (o == owner) {
            f = w;
            found = true;
        }
    if (!found) d->watchers.emplace_back(owner, std::move(w));
    if (!d->copy) {
        Message m;
        m.kind = Kind::DocWatch;
        m.doc = id;
        m.flag = 1;
        Message reply;
        if (!ask(std::move(m), &reply, timeoutMs)) {
            // The host did not answer in time: no copy, so no watcher
            // either. Left registered, it would be called once a later
            // watch made the copy, after its owner (an LspSession, a
            // closed window's) may have gone. The host is told to stop
            // too, after the watch it has still to take.
            d = doc(id);
            if (d) {
                d->watchers.erase(std::remove_if(d->watchers.begin(), d->watchers.end(),
                                                 [owner](const std::pair<const void*, Watcher>& p) {
                                                     return p.first == owner;
                                                 }),
                                  d->watchers.end());
                if (d->watchers.empty()) {
                    Message off;
                    off.kind = Kind::DocWatch;
                    off.doc = id;
                    off.flag = 0;
                    queue(std::move(off));
                    pump();
                }
            }
            return nullptr;
        }
        d = doc(id);   // a listener may have closed it meanwhile
        if (!d) return nullptr;
        d->copy = std::make_unique<Model::Text>(reply.text16);
    }
    return d->copy.get();
}

void Client::unwatch(DocId id, const void* owner) {
    Doc* d = doc(id);
    if (!d) return;
    d->watchers.erase(std::remove_if(d->watchers.begin(), d->watchers.end(),
                                     [owner](const std::pair<const void*, Watcher>& p) {
                                         return p.first == owner;
                                     }),
                      d->watchers.end());
    if (!d->watchers.empty() || !d->copy) return;
    d->copy.reset();
    Message m;
    m.kind = Kind::DocWatch;
    m.doc = id;
    m.flag = 0;
    queue(std::move(m));
    pump();
}

bool Client::watching(DocId id, const void* owner) const {
    auto it = docs_.find(id);
    if (it == docs_.end()) return false;
    for (const auto& [o, f] : it->second.watchers)
        if (o == owner) return true;
    return false;
}

const Model::Text* Client::hostCopy(DocId id) const {
    auto it = docs_.find(id);
    return it == docs_.end() ? nullptr : it->second.copy.get();
}

// ---------------------------------------------------------------- checks

Client::HostDoc Client::hostDocument(DocId id) {
    HostDoc out;
    Message q;
    q.kind = Kind::DocQuery;
    q.doc = id;
    Message reply;
    if (!ask(std::move(q), &reply)) return out;
    out.open = reply.name.empty();
    out.text = std::move(reply.text16);
    out.version = reply.v;
    out.dirty = reply.flag != 0;
    out.undo = reply.undo;
    return out;
}

std::vector<std::string> Client::takeErrors() {
    std::vector<std::string> out;
    out.swap(errors_);
    return out;
}

// ---------------------------------------------------------------- the workspace

std::uint64_t Client::requestRead(std::uint64_t window, Json request) {
    Message m;
    m.kind = Kind::FsRead;
    m.window = window;
    m.json = std::move(request);
    m.req = ++nextReq_;
    const std::uint64_t req = m.req;
    sendNow(std::move(m));
    return req;
}

bool Client::readAnswer(std::uint64_t req, Json* out, std::string* bytes) {
    auto it = replies_.find(req);
    if (it == replies_.end() || it->second.kind != Kind::FsData) return false;
    if (out) *out = std::move(it->second.json);
    if (bytes) *bytes = std::move(it->second.bytes);
    replies_.erase(it);
    return true;
}

void Client::forgetRead(std::uint64_t req) {
    if (!replies_.erase(req)) abandoned_.insert(req);
}

Json Client::read(std::uint64_t window, Json request, int timeoutMs) {
    const std::uint64_t req = requestRead(window, std::move(request));
    Json out;
    if (!waitFor([&] { return replies_.count(req) > 0; }, timeoutMs)) {
        abandoned_.insert(req);
        return Json();
    }
    readAnswer(req, &out);
    return out;
}

// ---------------------------------------------------------------- windows

void Client::windowTabs(std::uint64_t window, Json tabs) {
    Message m;
    m.kind = Kind::WindowTabs;
    m.window = window;
    m.json = std::move(tabs);
    queue(std::move(m));
    pump();
}

void Client::windowClosed(std::uint64_t window) {
    Message m;
    m.kind = Kind::WindowClose;
    m.window = window;
    queue(std::move(m));
    pump();
}

Json Client::sessionState() {
    Message q;
    q.kind = Kind::StateQuery;
    Message reply;
    if (!ask(std::move(q), &reply)) return Json();
    return reply.json;
}

// ---------------------------------------------------------------- reattaching

namespace {

void namedIn(const Json& tabs, std::vector<std::uint64_t>* docs, std::vector<std::uint64_t>* terms) {
    for (const Json& g : tabs["groups"].elements())
        for (const Json& t : g["tabs"].elements())
            if (std::uint64_t d = (std::uint64_t)t["doc"].asInt()) docs->push_back(d);
    for (const Json& t : tabs["terms"].elements())
        if (std::uint64_t id = (std::uint64_t)t.asInt()) terms->push_back(id);
}

}  // namespace

Reattach reattachFrom(const Json& state, ClientId me, const std::set<std::uint64_t>& live,
                      const std::function<bool(DocId)>& openHere,
                      const std::function<bool(TermId)>& showsHere, bool all) {
    Reattach out;
    if (!state.isObject()) return out;
    // Held documents a window can show, and terminals, by id.
    std::map<DocId, Reattach::Doc> docs;
    for (const Json& d : state["documents"].elements()) {
        Reattach::Doc h;
        h.id = (DocId)d["doc"].asInt();
        h.path = d["path"].asString();
        h.dirty = d["dirty"].asBool();
        const bool holds = !h.path.empty() || h.dirty || d["length"].asInt() > 0;
        if (h.id && holds && !(openHere && openHere(h.id))) docs[h.id] = h;
    }
    std::map<TermId, Reattach::Term> terms;
    for (const Json& t : state["terminals"].elements()) {
        Reattach::Term h;
        h.id = (TermId)t["term"].asInt();
        h.pid = (int)t["pid"].asInt();
        h.shell = termShellFrom(t["shell"]);
        if (h.id && h.pid > 0 && !(showsHere && showsHere(h.id))) terms[h.id] = h;
    }
    // Every record's names; the free records; the latest of them.
    std::set<DocId> named;
    std::set<TermId> namedTerms;
    const Json* best = nullptr;
    std::vector<const Json*> freeRecords;
    for (const Json& win : state["windows"].elements()) {
        const std::uint64_t id = (std::uint64_t)win["window"].asInt();
        const bool connected = win["connected"].isBool() ? win["connected"].asBool() : true;
        const bool mine = (ClientId)win["renderer"].asInt() == me;
        const bool isFree = !live.count(id) && (!connected || mine);
        std::vector<std::uint64_t> d, t;
        namedIn(win["tabs"], &d, &t);
        named.insert(d.begin(), d.end());
        namedTerms.insert(t.begin(), t.end());
        if (!isFree) {
            // A window still open (here or in another renderer) keeps them.
            for (std::uint64_t x : d) docs.erase(x);
            for (std::uint64_t x : t) terms.erase(x);
            continue;
        }
        freeRecords.push_back(&win);
        // One that names nothing (a window's report before it had tabs)
        // brings nothing back, and never hides one that does.
        if (d.empty() && t.empty()) continue;
        if (!best || win["seen"].asInt() > (*best)["seen"].asInt()) best = &win;
    }
    // One document of a path in a window (not for `all`).
    std::set<std::string> paths;
    auto takeDoc = [&](const Reattach::Doc& d) {
        if (!all && !d.path.empty() && !paths.insert(d.path).second) return false;
        out.docs.push_back(d);
        return true;
    };
    auto take = [&](const Json& win) {
        std::vector<std::uint64_t> d, t;
        namedIn(win["tabs"], &d, &t);
        for (std::uint64_t x : d) {
            auto it = docs.find(x);
            if (it == docs.end()) continue;
            if (takeDoc(it->second)) docs.erase(it);
        }
        for (std::uint64_t x : t) {
            auto it = terms.find(x);
            if (it == terms.end()) continue;
            out.terms.push_back(it->second);
            terms.erase(it);
        }
    };
    if (all) {
        for (const Json* w : freeRecords) take(*w);
    } else if (best) {
        out.window = (std::uint64_t)(*best)["window"].asInt();
        out.tabs = (*best)["tabs"];
        take(*best);
    }
    // What no record names at all (a record lost, or opened by a renderer
    // that kept none), oldest first.
    for (const auto& [id, d] : docs)
        if (!named.count(id)) takeDoc(d);
    for (const auto& [id, t] : terms)
        if (!namedTerms.count(id)) out.terms.push_back(t);
    if (all)
        for (const Json* w : freeRecords) out.records.push_back((std::uint64_t)(*w)["window"].asInt());
    return out;
}

}  // namespace Shared
