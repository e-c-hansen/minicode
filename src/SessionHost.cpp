// SessionHost.cpp — see SessionHost.h.
#include "SessionHost.h"

#include <algorithm>
#include <chrono>

namespace Shared {

// ---------------------------------------------------------------- Host

ClientId Host::connect() { return ++nextClient_; }

void Host::disconnect(ClientId c) {
    renderers_.erase(c);
    for (auto& [id, d] : docs_) {
        if (d.stepOpen && d.stepBy == c) closeStep(d);
        d.seats.erase(c);
        trimLog(d);
    }
}

void Host::send(ClientId to, Message&& m) {
    if (sink_) sink_(to, std::move(m));
}

void Host::error(ClientId to, DocId d, const std::string& what) {
    Message m;
    m.kind = Kind::Error;
    m.doc = d;
    m.name = what;
    send(to, std::move(m));
}

Model::Unit Host::unitOf(ClientId c) const {
    auto it = renderers_.find(c);
    return it == renderers_.end() ? Model::Unit::Utf16 : it->second.unit;
}

void Host::handle(ClientId from, Message&& m) {
    switch (m.kind) {
    case Kind::Hello: onHello(from, m); break;
    case Kind::DocOpen: onOpen(from, m); break;
    case Kind::DocReset: onReset(from, m); break;
    case Kind::DocClose: onClose(from, m); break;
    case Kind::DocEdit: onEdit(from, m); break;
    case Kind::DocUndo: onUndo(from, m); break;
    case Kind::DocSaved: onSaved(from, m); break;
    case Kind::DocWatch: onWatch(from, m); break;
    case Kind::DocQuery: onQuery(from, m); break;
    case Kind::WindowTabs: windows_[m.window] = {from, std::move(m.json)}; break;
    case Kind::WindowClose: windows_.erase(m.window); break;
    case Kind::StateQuery: {
        Message r;
        r.kind = Kind::State;
        r.req = m.req;
        r.json = state();
        send(from, std::move(r));
        break;
    }
    case Kind::Ping: {
        Message r;
        r.kind = Kind::Pong;
        r.req = m.req;
        send(from, std::move(r));
        break;
    }
    default: error(from, m.doc, std::string("the host takes no ") + kindName(m.kind)); break;
    }
}

void Host::onHello(ClientId c, Message& m) {
    Renderer& r = renderers_[c];
    r.unit = m.unit;
    r.name = m.name;
    r.hello = true;
    Message w;
    w.kind = Kind::Welcome;
    w.client = c;
    send(c, std::move(w));
}

void Host::onOpen(ClientId c, Message& m) {
    auto it = docs_.find(m.doc);
    Message r;
    r.kind = Kind::DocOpened;
    r.doc = m.doc;
    if (it == docs_.end()) {
        Doc& d = docs_[m.doc];
        d.path = m.name;
        d.doc = Model::Document(Model::utf16FromUtf8(m.text8));
        d.seats[c].base = 0;
        it = docs_.find(m.doc);
    } else {
        // Open already (another renderer's): this one starts from the text
        // as it is.
        Doc& d = it->second;
        d.seats[c].base = d.v;
        r.v = d.v;
        r.text16 = d.doc.text().str();
    }
    send(c, std::move(r));
    tellUndoState(m.doc, it->second);
}

void Host::onReset(ClientId c, Message& m) {
    auto it = docs_.find(m.doc);
    if (it == docs_.end() || !it->second.seats.count(c)) {
        error(c, m.doc, "doc.reset of a document not open");
        return;
    }
    Doc& d = it->second;
    d.seats[c].base = std::max(d.seats[c].base, m.base);
    closeStep(d);
    std::u16string now = Model::utf16FromUtf8(m.text8);
    Entry e;
    e.by = c;
    const std::size_t old16 = d.doc.text().size(), oldCp = d.doc.text().codePoints();
    if (old16) {
        e.ops16.push_back(Ot::Op{0, old16, std::u16string()});
        e.opsCp.push_back(Ot::Op{0, oldCp, std::u16string()});
    }
    if (!now.empty()) {
        e.ops16.push_back(Ot::Op{0, 0, now});
        e.opsCp.push_back(Ot::Op{0, 0, now});
    }
    d.doc = Model::Document(now);
    d.history.clear();
    if (!e.ops16.empty()) {
        d.v++;
        d.log.push_back(e);
    }
    broadcast(m.doc, d, e, c, m.seq, 0, 0, nullptr);
    trimLog(d);
    tellUndoState(m.doc, d);
}

void Host::onClose(ClientId c, Message& m) {
    auto it = docs_.find(m.doc);
    if (it == docs_.end()) return;
    Doc& d = it->second;
    if (d.stepOpen && d.stepBy == c) closeStep(d);
    d.seats.erase(c);
    if (d.seats.empty()) docs_.erase(it);
    else trimLog(d);
}

bool Host::concurrent(const Doc& d, std::uint64_t base, Model::Unit u, Ot::Ops* out) const {
    out->clear();
    if (base > d.v || base < d.logStart) return false;
    for (std::uint64_t ver = base + 1; ver <= d.v; ver++) {
        const Entry& e = d.log[(std::size_t)(ver - d.logStart - 1)];
        const Ot::Ops& ops = u == Model::Unit::Utf16 ? e.ops16 : e.opsCp;
        out->insert(out->end(), ops.begin(), ops.end());
    }
    return true;
}

void Host::trimLog(Doc& d) {
    std::uint64_t keepFrom = d.v;
    for (const auto& [c, s] : d.seats) keepFrom = std::min(keepFrom, s.base);
    while (d.logStart < keepFrom && !d.log.empty()) {
        d.log.pop_front();
        d.logStart++;
    }
}

Selection Host::toUnits(const Doc& d, const Selection& s, Model::Unit u) const {
    if (u == Model::Unit::Utf16) return s;
    const Model::Text& t = d.doc.text();
    Selection out;
    for (const auto& r : s) out.push_back({t.unitOf(r.anchor), t.unitOf(r.caret)});
    return out;
}

Selection Host::fromUnits(const Doc& d, const Selection& s, Model::Unit u) const {
    if (u == Model::Unit::Utf16) return s;
    const Model::Text& t = d.doc.text();
    Selection out;
    for (const auto& r : s) out.push_back({t.codePointOf(r.anchor), t.codePointOf(r.caret)});
    return out;
}

Selection Host::mapSelection(const Selection& s, const Ot::Ops& ops, Model::Unit u) const {
    if (ops.empty()) return s;
    Selection out;
    for (const auto& r : s)
        out.push_back({Ot::mapOffset(r.anchor, ops, u), Ot::mapOffset(r.caret, ops, u)});
    return out;
}

Host::Entry Host::applyOps(Doc& d, const Ot::Ops& ops, Model::Unit u, bool record) {
    Entry e;
    e.ops16.reserve(ops.size());
    e.opsCp.reserve(ops.size());
    for (const Ot::Op& op : ops) {
        const Model::Text& t = d.doc.text();
        const std::size_t a16 = t.toUnits(op.pos, u), b16 = t.toUnits(op.pos + op.del, u);
        const std::size_t aCp = u == Model::Unit::CodePoint ? op.pos : t.codePointOf(a16);
        const std::size_t bCp = u == Model::Unit::CodePoint ? op.pos + op.del : t.codePointOf(b16);
        std::u16string removed;
        if (record && b16 > a16) removed = t.slice(a16, b16 - a16);
        d.doc.replace(a16, b16, op.ins);
        if (record) d.history.record({a16, std::move(removed), op.ins});
        e.ops16.push_back(Ot::Op{a16, b16 - a16, op.ins});
        e.opsCp.push_back(Ot::Op{aCp, bCp - aCp, op.ins});
        const std::size_t caret = a16 + op.ins.size();
        if (record) d.stepAfter = Selection{{caret, caret}};
    }
    return e;
}

void Host::closeStep(Doc& d) {
    if (!d.stepOpen) return;
    d.stepOpen = false;
    d.history.close(d.stepAfter);
}

void Host::broadcast(DocId id, Doc& d, const Entry& e, ClientId by, std::uint64_t seq, int flag,
                     ClientId selectFor, const Selection* select16) {
    for (auto& [k, seat] : d.seats) {
        Message out;
        out.kind = Kind::DocEdited;
        out.doc = id;
        out.v = d.v;
        out.client = by;
        out.flag = flag;
        const Model::Unit u = unitOf(k);
        if (k == by && seq) out.seq = seq;   // its own: it has the ops
        else out.ops = u == Model::Unit::Utf16 ? e.ops16 : e.opsCp;
        if (seat.watch) out.ops16 = e.ops16;
        if (select16 && k == selectFor) {
            out.hasSelect = true;
            out.select = fromUnits(d, *select16, u);
        }
        send(k, std::move(out));
    }
}

void Host::onEdit(ClientId c, Message& m) {
    auto it = docs_.find(m.doc);
    if (it == docs_.end() || !it->second.seats.count(c)) {
        error(c, m.doc, "doc.edit of a document not open");
        return;
    }
    Doc& d = it->second;
    const Model::Unit u = unitOf(c);
    Ot::Ops crossed;   // what the host applied since the renderer's base
    Entry e;
    e.by = c;
    bool ok = concurrent(d, m.base, u, &crossed);
    Ot::Ops mine, crossedPastMine;
    if (ok) {
        d.seats[c].base = std::max(d.seats[c].base, m.base);
        if (crossed.empty()) mine = Ot::primitives(m.ops);
        else Ot::transform(m.ops, crossed, u, &mine, &crossedPastMine);
        ok = Ot::valid(mine, d.doc.text().length(u), u);
    }
    if (!ok) {
        // A renderer out of step: it is told, and its edit is acknowledged
        // unapplied so it does not wait for ever; the mirror checks catch
        // what follows.
        error(c, m.doc, "doc.edit on version " + std::to_string(m.base) + " that does not fit version " +
                            std::to_string(d.v));
        if (m.seq) {
            Message ack;
            ack.kind = Kind::DocEdited;
            ack.doc = m.doc;
            ack.v = d.v;
            ack.client = c;
            ack.seq = m.seq;
            send(c, std::move(ack));
        }
        return;
    }
    const bool sameStep = d.stepOpen && d.stepBy == c && d.stepId == m.step.id;
    if (!sameStep && (!mine.empty() || m.step.open)) {
        closeStep(d);
        const Selection before =
            toUnits(d, mapSelection(m.step.before, crossed, u), u);
        d.stepInfo = {m.step.name, m.step.coalesce};
        if (m.step.fresh) d.history.breakCoalescing();
        d.history.open(d.stepInfo, before);
        d.stepOpen = true;
        d.stepBy = c;
        d.stepId = m.step.id;
        d.stepAfter = before;
    }
    const bool recording = d.stepOpen && d.stepBy == c && d.stepId == m.step.id;
    if (!mine.empty()) {
        e = applyOps(d, mine, u, recording);
        e.by = c;
        d.v++;
        d.log.push_back(e);
    }
    if (recording && m.step.close) {
        d.stepAfter = toUnits(d, mapSelection(m.step.after, crossedPastMine, u), u);
        d.history.rename({m.step.name, m.step.coalesce});
        closeStep(d);
    }
    if (m.seq || !mine.empty()) broadcast(m.doc, d, e, c, m.seq, 0, 0, nullptr);
    trimLog(d);
    tellUndoState(m.doc, d);
}

void Host::onUndo(ClientId c, Message& m) {
    Message done;
    done.kind = Kind::DocUndone;
    done.doc = m.doc;
    done.req = m.req;
    auto it = docs_.find(m.doc);
    if (it == docs_.end()) {
        done.outcome = Outcome::Nothing;
        send(c, std::move(done));
        return;
    }
    Doc& d = it->second;
    closeStep(d);
    Model::UndoHistory::Plan plan;
    const bool redo = m.flag != 0;
    const Model::UndoHistory::Outcome o =
        redo ? d.history.redo(d.doc.text(), &plan) : d.history.undo(d.doc.text(), &plan);
    done.outcome = o == Model::UndoHistory::Outcome::Done      ? Outcome::Done
                   : o == Model::UndoHistory::Outcome::Nothing ? Outcome::Nothing
                                                               : Outcome::Refused;
    if (o == Model::UndoHistory::Outcome::Done) {
        // The plan is ascending and apart in the text now: last first, each
        // is still where it was.
        Ot::Ops ops;
        for (std::size_t i = plan.changes.size(); i-- > 0;) {
            const Model::UndoHistory::Change& ch = plan.changes[i];
            ops.push_back(Ot::Op{ch.start, ch.removed.size(), ch.inserted});
        }
        Entry e = applyOps(d, Ot::primitives(ops), Model::Unit::Utf16, false);
        e.by = 0;
        if (!e.ops16.empty()) {
            d.v++;
            d.log.push_back(e);
        }
        broadcast(m.doc, d, e, 0, 0, redo ? 2 : 1, c, &plan.select);
    }
    send(c, std::move(done));
    trimLog(d);
    tellUndoState(m.doc, d);
}

void Host::onSaved(ClientId, Message& m) {
    auto it = docs_.find(m.doc);
    if (it != docs_.end()) it->second.doc.markSaved(FolderSearch::FileStamp());
}

void Host::onWatch(ClientId c, Message& m) {
    Message r;
    r.kind = Kind::DocText;
    r.doc = m.doc;
    r.req = m.req;
    auto it = docs_.find(m.doc);
    if (it != docs_.end() && it->second.seats.count(c)) {
        Doc& d = it->second;
        d.seats[c].watch = m.flag != 0;
        r.v = d.v;
        if (m.flag) r.text16 = d.doc.text().str();
    }
    send(c, std::move(r));
}

void Host::onQuery(ClientId c, Message& m) {
    Message r;
    r.kind = Kind::DocText;
    r.doc = m.doc;
    r.req = m.req;
    auto it = docs_.find(m.doc);
    if (it != docs_.end()) {
        const Doc& d = it->second;
        r.v = d.v;
        r.text16 = d.doc.text().str();
        r.flag = d.doc.dirty() ? 1 : 0;
        r.undo = undoInfo(m.doc);
    } else {
        r.name = "no such document";
    }
    send(c, std::move(r));
}

void Host::tellUndoState(DocId id, Doc& d) {
    const UndoInfo now = undoInfo(id);
    for (auto& [k, seat] : d.seats) {
        if (seat.heard == now) continue;
        seat.heard = now;
        Message m;
        m.kind = Kind::UndoState;
        m.doc = id;
        m.undo = now;
        send(k, std::move(m));
    }
}

std::u16string Host::text(DocId d) const {
    auto it = docs_.find(d);
    return it == docs_.end() ? std::u16string() : it->second.doc.text().str();
}

std::uint64_t Host::version(DocId d) const {
    auto it = docs_.find(d);
    return it == docs_.end() ? 0 : it->second.v;
}

bool Host::dirty(DocId d) const {
    auto it = docs_.find(d);
    return it != docs_.end() && it->second.doc.dirty();
}

UndoInfo Host::undoInfo(DocId id) const {
    UndoInfo u;
    auto it = docs_.find(id);
    if (it == docs_.end()) return u;
    const Model::UndoHistory& h = it->second.history;
    u.canUndo = h.canUndo();
    u.canRedo = h.canRedo();
    u.undoName = h.undoName();
    u.redoName = h.redoName();
    return u;
}

std::size_t Host::logSize(DocId d) const {
    auto it = docs_.find(d);
    return it == docs_.end() ? 0 : it->second.log.size();
}

Json Host::state() const {
    Json s = Json::object();
    Json wins = Json::array();
    for (const auto& [id, w] : windows_) {
        Json j = Json::object();
        j.set("window", Json((int64_t)id));
        j.set("renderer", Json((int64_t)w.first));
        j.set("tabs", w.second);
        wins.push(std::move(j));
    }
    s.set("windows", std::move(wins));
    Json docs = Json::array();
    for (const auto& [id, d] : docs_) {
        Json j = Json::object();
        j.set("doc", Json((int64_t)id));
        j.set("path", Json(d.path));
        j.set("version", Json((int64_t)d.v));
        j.set("length", Json((int64_t)d.doc.text().size()));
        j.set("dirty", Json(d.doc.dirty()));
        docs.push(std::move(j));
    }
    s.set("documents", std::move(docs));
    return s;
}

// ---------------------------------------------------------------- SessionHost

SessionHost::SessionHost(Host::Sink sink, unsigned delayUs)
    : host_(std::move(sink)), delayUs_(delayUs) {
    thread_ = std::thread([this] { loop(); });
}

SessionHost::~SessionHost() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

ClientId SessionHost::connect() {
    // Ids come from the Host, on its thread.
    ClientId id = 0;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(Item{0, Message(), [&] {
                                  id = host_.connect();
                                  std::lock_guard<std::mutex> l(m);
                                  done = true;
                                  cv.notify_all();
                              }});
    }
    cv_.notify_all();
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return done; });
    return id;
}

void SessionHost::post(ClientId from, Message&& m) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(Item{from, std::move(m), nullptr});
    }
    cv_.notify_all();
}

void SessionHost::disconnect(ClientId c) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(Item{0, Message(), [this, c] { host_.disconnect(c); }});
    }
    cv_.notify_all();
}

void SessionHost::inspect(const std::function<void(const Host&)>& f) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(Item{0, Message(), [&] {
                                  f(host_);
                                  std::lock_guard<std::mutex> l(m);
                                  done = true;
                                  cv.notify_all();
                              }});
    }
    cv_.notify_all();
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return done; });
}

void SessionHost::loop() {
    for (;;) {
        Item item;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (queue_.empty()) return;   // stopping, and everything done
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        if (delayUs_) std::this_thread::sleep_for(std::chrono::microseconds(delayUs_));
        if (item.run) item.run();
        else host_.handle(item.from, std::move(item.m));
    }
}

}  // namespace Shared
