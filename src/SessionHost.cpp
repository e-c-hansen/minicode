// SessionHost.cpp — see SessionHost.h.
#include "SessionHost.h"

#include "SessionFiles.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace Shared {

// ---------------------------------------------------------------- Host

ClientId Host::connect() { return ++nextClient_; }

Host::~Host() = default;

void Host::disconnect(ClientId c) {
    renderers_.erase(c);
    if (workers_) workers_->cancelAll(c);   // its searches answer no one now
    if (watches_.erase(c)) watchGen_++;   // its trees' folders are no one's now
    // Its terminals stay for the next renderer; one nobody shows keeps
    // running, its output going only into the host's screen, unthrottled.
    // One whose size was this renderer's takes the next one's.
    for (auto& [id, t] : terms_)
        if (t.viewers.erase(c)) fit(id, t);
    for (auto& [id, d] : docs_) {
        if (d.stepOpen && d.stepBy == c) closeStep(d);
        d.seats.erase(c);
        trimLog(d);
    }
    dropEmptyPageDocs();
    backupsChanged();   // an app gone: what it had unsaved is the host's to keep
}

void Host::dropEmptyPageDocs() {
    for (auto it = docs_.begin(); it != docs_.end();) {
        const Doc& d = it->second;
        if (d.fromPage && d.seats.empty() && d.path.empty() && d.doc.text().size() == 0) {
            discardBackup(it->second);
            it = docs_.erase(it);
            watchGen_++;
        } else {
            ++it;
        }
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

bool Host::refusedFromViewer(ClientId c, const Message& m) const {
    auto it = renderers_.find(c);
    if (it == renderers_.end() || (!it->second.readOnly && !it->second.guest)) return false;
    const bool guest = it->second.guest;
    switch (m.kind) {
    case Kind::Hello:
    case Kind::DocOpen:      // a view seat (onOpen)
    case Kind::DocClose:     // its own seat
    case Kind::DocWatch:
    case Kind::DocQuery:
    case Kind::StateQuery:
    case Kind::Ping:
    case Kind::TermAttach:
    case Kind::TermAck:
    case Kind::FsRead:       // reads the folder, changes nothing
    case Kind::DocDetach:    // its own seat
    case Kind::TermDetach:   // its own place among the viewers
        return false;
    case Kind::DocEdit:
        // A guest's edits, in the undo history; from a reader, only the
        // report of how far it has read (SessionClient's).
        if (guest) return m.flag != 0;
        return !m.ops.empty() || m.step.id || m.flag;
    case Kind::DocUndo:
    case Kind::TermInput:
    // A guest's reload: the file's text in place of its document's, one
    // undoable step; never of a document an app's window has open (onReload).
    case Kind::DocReload:
    // A guest's save: held to the session's folder, and never of a
    // document an app's window has open (onSave).
    case Kind::DocSave:
    // A guest's view size: the pty takes it only while the guest is the
    // one that typed last (Host::sizer), so a narrow page shrinks nothing
    // until someone types there.
    case Kind::TermResize:
        return !guest;
    default: return true;
    }
}

void Host::handle(ClientId from, Message&& m) {
    if (refusedFromViewer(from, m)) {
        error(from, m.doc, std::string("a renderer that only reads cannot send ") +
                               kindName(m.kind));
        return;
    }
    switch (m.kind) {
    case Kind::Hello: onHello(from, m); break;
    case Kind::DocOpen: onOpen(from, m); break;
    case Kind::DocReset: onReset(from, m); break;
    case Kind::DocClose: onClose(from, m); break;
    case Kind::DocEdit: onEdit(from, m); break;
    case Kind::DocUndo: onUndo(from, m); break;
    case Kind::DocSaved: onSaved(from, m); break;
    case Kind::DocSave: onSave(from, m); break;
    case Kind::DocForget: onForget(from, m); break;
    case Kind::DocWatch: onWatch(from, m); break;
    case Kind::DocQuery: onQuery(from, m); break;
    case Kind::DocPath: onPath(from, m); break;
    case Kind::DocDetach: onDetach(from, m); break;
    case Kind::DocReload: onReload(from, m); break;
    case Kind::WindowTabs:
        windows_[m.window] = {from, std::move(m.json)};
        windowSeen_[m.window] = ++windowOrder_;
        break;
    case Kind::WindowClose:
        windows_.erase(m.window);
        windowSeen_.erase(m.window);
        break;
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
    case Kind::TermOpen: onTermOpen(from, m); break;
    case Kind::TermAttach: onTermAttach(from, m); break;
    case Kind::TermInput: {
        auto it = terms_.find(m.term);
        if (it == terms_.end() || !it->second.pty) break;
        Term& t = it->second;
        // Whoever types sets the size, before what it typed is read; an
        // emulator's answer to a query is not typing.
        auto v = t.viewers.find(from);
        if (v != t.viewers.end() && !(m.flag & 1)) {
            v->second.typed = ++order_;
            fit(it->first, t);
        }
        t.pty->write(m.bytes);
        break;
    }
    case Kind::TermResize: {
        // The renderer's view: kept as its size, the pty's only while it
        // is the sizer.
        auto it = terms_.find(m.term);
        if (it == terms_.end() || m.cols < 2 || m.rows < 1) break;
        auto v = it->second.viewers.find(from);
        if (v == it->second.viewers.end()) break;
        v->second.cols = m.cols;
        v->second.rows = m.rows;
        fit(it->first, it->second);
        break;
    }
    case Kind::TermAck: {
        auto it = terms_.find(m.term);
        if (it == terms_.end()) break;
        auto v = it->second.viewers.find(from);
        if (v != it->second.viewers.end())
            v->second.unacked = std::max(0LL, v->second.unacked - (long long)m.v);
        break;
    }
    case Kind::TermClose:
        if (terms_.count(m.term)) endTerminal(m.term, false, from);
        break;
    case Kind::TermDetach: {
        // As a renderer going (disconnect): the shell runs on, its size the
        // next viewer's by the rule.
        auto it = terms_.find(m.term);
        if (it != terms_.end() && it->second.viewers.erase(from)) fit(it->first, it->second);
        break;
    }
    case Kind::FsRead: onRead(from, m); break;
    default: error(from, m.doc, std::string("the host takes no ") + kindName(m.kind)); break;
    }
    switch (m.kind) {
    case Kind::Hello:
    case Kind::DocOpen:
    case Kind::DocReset:
    case Kind::DocClose:
    case Kind::DocEdit:
    case Kind::DocUndo:
    case Kind::DocSaved:
    case Kind::DocSave:
    case Kind::DocPath:
    case Kind::DocReload:
    case Kind::DocDetach: backupsChanged(); break;
    default: break;
    }
    switch (m.kind) {
    case Kind::DocReset:
    case Kind::DocEdit:
    case Kind::DocUndo:
    case Kind::DocSaved:
    case Kind::DocSave:
    case Kind::DocReload: tellDirty(m.doc); break;
    default: break;
    }
}

bool Host::slotFree(std::uint64_t slot) const {
    const std::uint64_t lo = slot * kIdRange, hi = lo + kIdRange;
    for (const auto& [id, r] : renderers_)
        if (r.idBase == lo) return false;
    auto holds = [lo, hi](const auto& map) {
        auto it = map.lower_bound(lo);
        return it != map.end() && it->first < hi;
    };
    if (holds(docs_) || holds(terms_) || holds(windows_)) return false;
    for (const Dying& d : dying_)
        if (d.term >= lo && d.term < hi) return false;
    return true;
}

std::uint64_t Host::chooseIdBase() {
    // A slot never given before, skipping any where something holds an id
    // all the same (a renderer that chose its own).
    while (lastSlot_ + 1 < kIdSlots)
        if (slotFree(++lastSlot_)) return lastSlot_ * kIdRange;
    // Every slot was given once (two million connections): the first free.
    for (std::uint64_t s = 1; s < kIdSlots; s++)
        if (slotFree(s)) return s * kIdRange;
    return 0;
}

std::string Host::readRoot(std::uint64_t window) const {
    if (!folder_.empty()) return folder_;
    auto w = windows_.find(window);
    return w == windows_.end() ? std::string() : w->second.second["root"].asString();
}

void Host::onRead(ClientId c, Message& m) {
    Message r;
    r.kind = Kind::FsData;
    r.req = m.req;
    r.window = m.window;
    const std::string op = m.json["op"].asString();
    if (op == "watch") {
        r.json = onWatchAsk(c, m.window, m.json);
        send(c, std::move(r));
        return;
    }
    if (op == "replace") return onReplace(c, m);
    if (isWorkerRead(op)) {
        // Quick Open's list and Find in Folder, on a worker: answered as it
        // goes (takeReads). A cancel stops the renderer's own read and has
        // no answer of its own.
        if (op == "cancel") {
            if (workers_) workers_->cancel(c, (std::uint64_t)std::max<int64_t>(0, m.json["req"].asInt()));
            return;
        }
        const std::string root = readRoot(m.window);
        std::string why = "no folder to read in";
        if (!root.empty()) {
            if (!workers_) workers_ = std::make_unique<ReadWorkers>();
            if (workers_->start(c, m.req, m.window, root, m.json, &why, readSeconds_)) return;
        }
        Json e = Json::object();
        e.set("op", Json(op));
        e.set("error", Json(why));
        e.set("done", Json(true));
        r.json = std::move(e);
        send(c, std::move(r));
        return;
    }
    const std::string root = readRoot(m.window);
    if (root.empty()) {
        Json e = Json::object();
        e.set("op", m.json["op"]);
        e.set("path", m.json["path"]);
        e.set("error", Json(std::string("no folder to read in")));
        r.json = std::move(e);
    } else {
        r.json = readWorkspace(root, m.json, &r.bytes);
    }
    send(c, std::move(r));
}

void Host::takeReads() {
    if (!workers_) return;
    for (ReadWorkers::Answer& a : workers_->take()) {
        if (!renderers_.count(a.to)) continue;   // gone meanwhile
        Message r;
        r.kind = Kind::FsData;
        r.req = a.req;
        r.window = a.window;
        r.json = std::move(a.json);
        send(a.to, std::move(r));
    }
}

bool Host::waitReads(int timeoutMs) {
    if (!workers_) return false;
    const bool came = workers_->wait(timeoutMs);
    takeReads();
    return came;
}

void Host::onHello(ClientId c, Message& m) {
    Renderer& r = renderers_[c];
    r.unit = m.unit;
    r.name = m.name;
    r.hello = true;
    r.readOnly = (m.flag & 1) != 0;
    r.guest = !r.readOnly && (m.flag & 2) != 0;
    r.keepsBackups = !r.readOnly && !r.guest && (m.flag & 4) != 0;
    r.hearsDirty = (m.flag & 8) != 0;
    r.savesThroughHost = r.keepsBackups && (m.flag & 16) != 0;
    r.hearsDisk = (m.flag & 32) != 0;
    // Its id base, kept for a second Hello: what it made with it stays its.
    if (!r.idBase) r.idBase = chooseIdBase();
    Message w;
    w.kind = Kind::Welcome;
    w.client = c;
    w.v = r.idBase;
    w.name = folder_;
    w.flag = 1 | 2;   // this host saves (DocSave), for apps too
    if (watching_) w.flag |= 4;   // and watches the disk (FsRead's "watch", DocReload)
    w.flag |= 8;   // and lists files and searches on workers (FsRead's "files", "search", "cancel")
    // And writes for a page: Save As (DocSave's "saveAs"), a new untitled
    // document (DocOpen flag 2), Replace in Folder (FsRead's "replace").
    w.flag |= 16;
    // And runs a terminal in this machine's own login shell and environment
    // (TermSpec's hostShell): a renderer on another machine (stage 8).
    w.flag |= 32;
    if (!askedFile_.empty()) {
        Json j = Json::object();
        j.set("file", Json(askedFile_));
        w.json = std::move(j);
    }
    send(c, std::move(w));
}

void Host::onOpen(ClientId c, Message& m) {
    auto it = docs_.find(m.doc);
    Message r;
    r.kind = Kind::DocOpened;
    r.doc = m.doc;
    auto who = renderers_.find(c);
    if (who != renderers_.end() && who->second.guest && (m.flag & 2)) {
        // A page's New File: an untitled document the host holds, with no
        // path and no text, the page's seat a view as any of its seats.
        // Let go again when nobody has it and it is still empty.
        if (it != docs_.end()) {
            error(c, m.doc, "doc.open of a new document whose id is taken");
            return;
        }
        Doc& d = docs_[m.doc];
        d.fromPage = true;
        d.seats[c].viewer = true;
        r.v = 0;
        send(c, std::move(r));
        tellUndoState(m.doc, d);
        return;
    }
    if (who != renderers_.end() && (who->second.readOnly || who->second.guest)) {
        // A view: of the document as it is, never a new one.
        if (it == docs_.end()) {
            Message g;
            g.kind = Kind::DocGone;
            g.doc = m.doc;
            send(c, std::move(g));
            return;
        }
        Doc& d = it->second;
        Doc::Seat& seat = d.seats[c];
        seat.base = d.v;
        seat.viewer = true;
        r.v = d.v;
        r.text16 = d.doc.text().str();
        send(c, std::move(r));
        return;
    }
    if (it == docs_.end()) {
        Doc& d = docs_[m.doc];
        d.path = m.name;
        d.doc = Model::Document(Model::utf16FromUtf8(m.text8));
        d.seats[c].base = 0;
        it = docs_.find(m.doc);
        watchGen_++;   // its folder is watched
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
    d.diskLooked = false;
    setDisk(m.doc, d, 0);   // read afresh: in step with its file
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
    auto seat = d.seats.find(c);
    const bool viewer = seat != d.seats.end() && seat->second.viewer;
    if (seat != d.seats.end()) d.seats.erase(seat);
    if (viewer) {
        // A view going changes nothing, even when nobody else has it open
        // (a renderer that detached leaves its documents for the next),
        // but a page's new document that nobody typed into.
        trimLog(d);
        dropEmptyPageDocs();
        return;
    }
    bool onlyViews = true;
    for (const auto& [id, s] : d.seats) onlyViews = onlyViews && s.viewer;
    if (!onlyViews) {
        trimLog(d);
        return;
    }
    // The last renderer that edits it closed it: the views hear it went.
    for (const auto& [id, s] : d.seats) {
        Message g;
        g.kind = Kind::DocGone;
        g.doc = m.doc;
        send(id, std::move(g));
    }
    discardBackup(d);   // closed: what it held is let go of
    docs_.erase(it);
    watchGen_++;
}

void Host::onPath(ClientId c, Message& m) {
    auto it = docs_.find(m.doc);
    if (it == docs_.end()) return;
    auto seat = it->second.seats.find(c);
    if (seat == it->second.seats.end() || seat->second.viewer) {
        error(c, m.doc, "doc.path of a document not open");
        return;
    }
    it->second.path = m.name;
    it->second.diskLooked = false;
    it->second.realDir.clear();
    watchGen_++;
}

void Host::onDetach(ClientId c, Message& m) {
    // The seat goes as it would with the renderer's connection; the
    // document stays, whoever else has it, for a renderer to open again.
    auto it = docs_.find(m.doc);
    if (it == docs_.end()) return;
    Doc& d = it->second;
    if (d.stepOpen && d.stepBy == c) closeStep(d);
    d.seats.erase(c);
    trimLog(d);
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

Host::Entry Host::applyOps(Doc& d, const Ot::Ops& ops, Model::Unit u, bool record, bool outside) {
    Entry e;
    e.ops16.reserve(ops.size());
    e.opsCp.reserve(ops.size());
    for (const Ot::Op& op : ops) {
        const Model::Text& t = d.doc.text();
        const std::size_t a16 = t.toUnits(op.pos, u), b16 = t.toUnits(op.pos + op.del, u);
        const std::size_t aCp = u == Model::Unit::CodePoint ? op.pos : t.codePointOf(a16);
        const std::size_t bCp = u == Model::Unit::CodePoint ? op.pos + op.del : t.codePointOf(b16);
        std::u16string removed;
        if ((record || outside) && b16 > a16) removed = t.slice(a16, b16 - a16);
        d.doc.replace(a16, b16, op.ins);
        if (record) d.history.record({a16, std::move(removed), op.ins});
        else if (outside) d.history.foreign({{a16, std::move(removed), op.ins}});
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
    if (m.flag == 1) {
        // Outside the history: in no step, and every step moved past it
        // (a step open when it came is closed first: its edits were made
        // before it).
        closeStep(d);
        if (!mine.empty()) {
            e = applyOps(d, mine, u, false, true);
            e.by = c;
            d.v++;
            d.log.push_back(e);
        }
        if (m.seq || !mine.empty()) broadcast(m.doc, d, e, c, m.seq, 0, 0, nullptr);
        trimLog(d);
        tellUndoState(m.doc, d);
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
    // An app wrote it: the host knows no stamp for the file now, so its own
    // next save reads the file and compares it with this text.
    auto it = docs_.find(m.doc);
    if (it == docs_.end()) return;
    // Flag 1: the file holds the text sent, not the one the host has (a
    // window's Keep Mine over a file changed on disk).
    if (m.flag & 1) it->second.doc.markSavedAs(std::move(m.text16));
    else it->second.doc.markSaved(FolderSearch::FileStamp());
    // The app read or wrote the file: in step with it, as the app has it.
    it->second.diskLooked = false;
    setDisk(m.doc, it->second, 0);
}

namespace {

// The real place of `path`, symlinks followed; "" when there is none.
std::string realOf(const std::string& path) {
    char buf[PATH_MAX];
    if (path.empty() || !::realpath(path.c_str(), buf)) return std::string();
    return buf;
}

std::string nameIn(const std::string& path) { return path.substr(path.rfind('/') + 1); }

}  // namespace

DocId Host::docAtPath(const std::string& abs, DocId except) const {
    const std::string real = realOf(abs);
    for (const auto& [id, d] : docs_) {
        if (id == except || d.path.empty()) continue;
        if (d.path == abs) return id;
        if (!real.empty() && realOf(d.path) == real) return id;
    }
    return 0;
}

void Host::onSaveAs(ClientId c, Message& m) {
    Message r;
    r.kind = Kind::DocSaveDone;
    r.doc = m.doc;
    r.req = m.req;
    r.outcome = Outcome::Refused;
    SaveDone done;
    std::string path;
    auto it = docs_.find(m.doc);
    if (it == docs_.end()) {
        done.reason = "notopen";
        done.why = "That document is no longer open.";
    } else {
        Doc& d = it->second;
        const std::string was = d.path.empty() ? std::string("This document") : nameIn(d.path);
        const std::string root = readRoot(m.window);
        const NewPath np = newFilePath(root, m.json["saveAs"].asString(), m.json["hidden"].asBool());
        path = np.abs;
        const std::string name = np.ok ? nameIn(np.abs) : std::string();
        if (!np.ok) {
            done.reason = np.reason;
            done.why = np.why;
        } else if (shownByApp(d)) {
            // The app's window names its own files (and asks its own
            // questions); the document stays where it is.
            done.reason = "app";
            done.why = was + " is open in a mutible window: use Save As there.";
        } else if (docAtPath(np.abs, m.doc)) {
            // Never two documents of one path.
            done.reason = "open";
            done.why = name + " is open in this session already: close it first, or save under another name.";
        } else if (np.exists && !(m.flag & 1)) {
            done.reason = "exists";
            done.why = name + " already exists: saving would replace it.";
        } else {
            // Written in the document's own encoding (a new file has none of
            // its own), whatever is there now: the person answered for it.
            SaveAsk a;
            a.path = np.abs;
            a.root = root;
            a.text = d.doc.text().str();
            a.encoding = d.doc.encoding();
            a.bom = d.doc.bom();
            a.keepEncoding = true;
            a.overwrite = true;
            done = saveDocument(a);
            if (done.ok) {
                // Its file is the new one for every view; the old file is
                // left as it was on disk.
                d.path = np.abs;
                d.realDir.clear();
                watchGen_++;
                d.doc.setEncoding(done.encoding, done.bom);
                d.doc.markSaved(done.written);
                d.doc.setGone(false);
                d.diskSeen = done.written;
                d.diskLooked = true;
                setDisk(m.doc, d, 0);
                r.outcome = Outcome::Done;
                r.v = d.v;
            }
        }
    }
    r.json = saveDoneJson(path, done);
    if (done.ok) {
        Json written = Json::object();
        written.set("path", Json(path));
        written.set("encodingName", r.json["encodingName"]);
        written.set("stamp", r.json["stamp"]);
        tellWritten(m.doc, it->second, c, written);
    }
    send(c, std::move(r));
}

void Host::onSave(ClientId c, Message& m) {
    auto who = renderers_.find(c);
    const bool guest = who != renderers_.end() && who->second.guest;
    // A page's Save As (a name for an untitled document, or a new file for
    // one with a file); one to the file it has already is a save.
    if (guest && m.json.isObject() && m.json["saveAs"].isString()) {
        auto d = docs_.find(m.doc);
        std::string abs, why;
        const std::string root = readRoot(m.window);
        bool same = d != docs_.end() && !d->second.path.empty() &&
                    confinedPath(root, m.json["saveAs"].asString(), &abs, &why);
        if (same && abs != d->second.path) {
            const std::string real = realOf(abs);
            same = !real.empty() && real == realOf(d->second.path);
        }
        if (!same) return onSaveAs(c, m);
    }
    Message r;
    r.kind = Kind::DocSaveDone;
    r.doc = m.doc;
    r.req = m.req;
    r.outcome = Outcome::Refused;
    SaveDone done;
    std::string path;
    auto it = docs_.find(m.doc);
    if (it == docs_.end()) {
        done.reason = "notopen";
        done.why = "That document is no longer open.";
    } else {
        Doc& d = it->second;
        path = d.path;
        const std::string name = path.substr(path.rfind('/') + 1);
        if (guest && shownByAppSavingItself(d)) {
            // Two writers would each keep a stamp the other's write moves:
            // the app's window saves what it shows, with its own questions.
            // An app that saves through the host leaves the host the one
            // writer, and follows its writes (tellWritten).
            done.reason = "app";
            done.why = (name.empty() ? std::string("This document") : name) +
                       " is open in a mutible window: save it there.";
        } else {
            SaveAsk a;
            a.path = d.path;
            if (guest) {
                a.root = readRoot(m.window);
                if (a.root.empty()) {
                    done.reason = "outside";
                    done.why = "The session has no folder to save in.";
                }
            }
            if (done.reason.empty()) {
                a.text = d.doc.text().str();
                a.saved = d.doc.savedText();
                a.stamp = d.doc.stamp();
                a.encoding = d.doc.encoding();
                a.bom = d.doc.bom();
                a.overwrite = (m.flag & 1) != 0;
                // An app's own encoding, as its tab writes the file.
                FolderSearch::TextEncoding e;
                bool b = false;
                if (!guest && m.json.isObject() &&
                    FolderSearch::parseEncodingName(m.json["encoding"].asString(), &e, &b)) {
                    a.encoding = e;
                    a.bom = b;
                    a.keepEncoding = true;
                }
                done = saveDocument(a);
            }
            if (done.ok) {
                d.doc.setEncoding(done.encoding, done.bom);
                d.doc.markSaved(done.written);
                r.outcome = Outcome::Done;
                r.v = d.v;
                d.diskSeen = done.written;
                d.diskLooked = true;
                setDisk(m.doc, d, 0);
            }
        }
    }
    r.json = saveDoneJson(path, done);
    if (done.ok) {
        Json written = Json::object();
        written.set("path", Json(path));
        written.set("encodingName", r.json["encodingName"]);
        written.set("stamp", r.json["stamp"]);
        tellWritten(m.doc, it->second, c, written);
    }
    send(c, std::move(r));
}

void Host::tellWritten(DocId id, Doc& d, ClientId by, const Json& written) {
    bool anyone = false;
    for (const auto& [c, r] : renderers_) anyone = anyone || r.hearsDirty;
    if (!anyone) return;
    const bool now = d.doc.dirty();
    const bool flipped = now != d.toldDirty;
    d.toldDirty = now;
    for (const auto& [c, r] : renderers_) {
        if (!r.hearsDirty || (c == by && !flipped)) continue;
        Message m;
        m.kind = Kind::DocDirty;
        m.doc = id;
        m.flag = now ? 1 : 0;
        m.v = d.v;
        if (c != by) {
            m.flag |= 2;
            m.json = written;
        }
        send(c, std::move(m));
    }
}

void Host::tellDirty(DocId id) {
    auto it = docs_.find(id);
    if (it == docs_.end()) return;
    bool anyone = false;
    for (const auto& [c, r] : renderers_) anyone = anyone || r.hearsDirty;
    if (!anyone) return;
    Doc& d = it->second;
    const bool now = d.doc.dirty();
    if (now == d.toldDirty) return;
    d.toldDirty = now;
    for (const auto& [c, r] : renderers_) {
        if (!r.hearsDirty) continue;
        Message m;
        m.kind = Kind::DocDirty;
        m.doc = id;
        m.flag = now ? 1 : 0;
        m.v = d.v;
        send(c, std::move(m));
    }
}

void Host::onForget(ClientId, Message& m) {
    auto it = docs_.find(m.doc);
    if (it == docs_.end()) return;
    Doc& d = it->second;
    closeStep(d);
    d.history.clear();
    tellUndoState(m.doc, d);
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
        // Whether the renderer that reported it is still here (one that
        // went left it for the next to take), and when it last reported.
        j.set("connected", Json(renderers_.count(w.first) > 0));
        auto seen = windowSeen_.find(id);
        j.set("seen", Json((int64_t)(seen == windowSeen_.end() ? 0 : seen->second)));
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
        if (d.disk) j.set("disk", Json(d.disk == 2 ? "gone" : "changed"));
        docs.push(std::move(j));
    }
    s.set("documents", std::move(docs));
    Json terms = Json::array();
    for (const auto& [id, t] : terms_) {
        Json j = Json::object();
        j.set("term", Json((int64_t)id));
        j.set("cols", Json((int64_t)t.screen.cols()));
        j.set("rows", Json((int64_t)t.screen.rows()));
        j.set("pid", Json((int64_t)(t.pty ? t.pty->pid() : 0)));
        j.set("shell", termShellJson(t.shell));
        j.set("renderers", Json((int64_t)t.viewers.size()));
        terms.push(std::move(j));
    }
    s.set("terminals", std::move(terms));
    if (!folder_.empty()) s.set("folder", Json(folder_));
    return s;
}

std::string Host::busyReason() const {
    for (const auto& [id, d] : docs_) {
        if (d.doc.dirty())
            return (d.path.empty() ? "a document" : d.path) + " has unsaved changes";
        // A document with no file holding text is unsaved whatever its
        // history says (one opened with text was never written anywhere).
        if (d.path.empty() && d.doc.text().size() > 0) return "a document with no file holds text";
    }
    for (const auto& [id, t] : terms_) {
        if (!t.pty) continue;
        if (t.shell.busy)
            return "terminal " + std::to_string(id) + " is running " +
                   (t.shell.command.empty() ? std::string("a command") : t.shell.command);
        if (t.shell.foreground > 0 && t.shell.foreground != t.pty->pid())
            return "terminal " + std::to_string(id) + " is running a program";
    }
    return "";
}

// ---------------------------------------------------------------- backups

void Host::keepBackups(BackupKeeping k) {
    keep_ = std::move(k);
    keeping_ = !keep_.store.dir.empty() && keep_.store.fs.writeAtomic;
    backupsChanged();
}

double Host::monoNow() const {
    if (keep_.monoNow) return keep_.monoNow();
    using namespace std::chrono;
    return (double)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count() / 1e6;
}

bool Host::unsaved(const Doc& d) {
    return d.doc.dirty() || (d.path.empty() && d.doc.text().size() > 0);
}

bool Host::shownByApp(const Doc& d) const {
    for (const auto& [c, seat] : d.seats) {
        if (seat.viewer) continue;
        auto r = renderers_.find(c);
        if (r != renderers_.end() && r->second.keepsBackups) return true;
    }
    return false;
}

bool Host::shownByAppSavingItself(const Doc& d) const {
    for (const auto& [c, seat] : d.seats) {
        if (seat.viewer) continue;
        auto r = renderers_.find(c);
        if (r != renderers_.end() && r->second.keepsBackups && !r->second.savesThroughHost) return true;
    }
    return false;
}

std::string Host::keptPath(DocId id, Doc& d) {
    if (!d.path.empty()) return d.path;
    if (d.kept.untitled.empty()) {
        // A name in the folder that no file and no other buffer has.
        const std::string folder = keep_.root.empty() ? folder_ : keep_.root;
        d.kept.untitled = Backups::untitledPath(folder, [&](const std::string& p) {
            struct stat st;
            if (lstat(p.c_str(), &st) == 0) return true;
            for (const auto& [other, o] : docs_)
                if (other != id && (o.path == p || o.kept.untitled == p)) return true;
            return false;
        });
    }
    return d.kept.untitled;
}

std::vector<SavePlan::Tab> Host::keptTabs() {
    std::vector<SavePlan::Tab> out;
    for (auto& [id, d] : docs_) {
        SavePlan::Tab t;
        t.id = id;
        // Whether it is unsaved is asked only of what an app does not show
        // (a text the same length as the saved one is compared whole).
        t.keepsText = !shownByApp(d);
        t.dirty = t.keepsText && unsaved(d);
        t.generation = d.v;
        t.path = t.dirty && t.keepsText ? keptPath(id, d) : d.path;
        t.backupWritten = d.kept.written;
        t.backupGeneration = d.kept.v;
        t.backupPath = d.kept.path;
        out.push_back(std::move(t));
    }
    return out;
}

void Host::backupsChanged() {
    if (!keeping_) return;
    for (auto& [id, d] : docs_) {
        const bool shown = shownByApp(d);
        const bool justLeft = d.kept.wasShown && !shown;
        d.kept.wasShown = shown;
        if (shown || !unsaved(d)) {   // shown first: no compare for what an app has
            // An app's window has it (the window's backup is the one), or
            // there is nothing to keep: the host's goes now.
            if (d.kept.written) discardBackup(d);
            continue;
        }
        // The last app's window let go of it unsaved: on disk at once.
        if (justLeft) writeBackup(id, d);
    }
    // The rest (edits from renderers that keep no backups, a document an
    // app never showed) when typing pauses, as the apps time theirs.
    keepPlan_.backupsChanged(keptTabs(), monoNow());
}

namespace {

// What the file is as the backup's base (Backups::compare at a restore):
// its stamp when it holds the text the host counts saved, so a change on
// disk after now is seen as one; a stamp that cannot match when it holds
// something else already (it changed since the buffer was based on it); and
// never there when there is none and the buffer never had a file's text.
void diskBase(const std::string& path, const std::u16string& saved, Backups::Entry* e) {
    e->diskExisted = false;
    e->diskMtime = 0;
    e->diskInode = 0;
    if (path.empty()) return;
    std::string bytes;
    FolderSearch::FileStamp st;
    const FolderSearch::ReadStatus r = FolderSearch::readFileStable(path, 512ull << 20, &bytes, &st);
    if (r == FolderSearch::ReadStatus::Missing) {
        e->diskExisted = !saved.empty();   // gone, when the buffer had text from it
        return;
    }
    e->diskExisted = true;
    e->diskInode = st.inode;
    const double mtime = (double)st.mtimeSec + st.mtimeNsec / 1e9;
    e->diskMtime = mtime - 1;   // changed, unless it reads as the saved text
    if (r != FolderSearch::ReadStatus::Ok) return;
    FolderSearch::TextEncoding enc;
    bool bom = false;
    if (!FolderSearch::decodeFileText(&bytes, &enc, &bom)) return;
    if (Model::utf16FromUtf8(bytes) != saved) return;
    e->diskMtime = mtime;
    e->fileEncoding = FolderSearch::encodingName(enc, bom);
}

}  // namespace

void Host::writeBackup(DocId id, Doc& d) {
    if (!keeping_) return;
    if (d.kept.id.empty()) d.kept.id = keep_.newId ? keep_.newId() : Backups::newId();
    Backups::Entry e;
    e.id = d.kept.id;
    e.path = keptPath(id, d);
    e.root = keep_.root.empty() ? folder_ : keep_.root;
    diskBase(d.path, d.doc.savedText(), &e);
    if (keep_.wallNow) {
        e.saved = keep_.wallNow();
    } else {
        using namespace std::chrono;
        e.saved = (double)duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() / 1e6;
    }
    e.ownerPid = keep_.ownerPid;
    e.ownerStart = keep_.ownerStart;
    if (!keep_.store.save(e, Model::utf8FromUtf16(d.doc.text().str()))) {
        std::fprintf(stderr, "mutible: could not write the backup of %s in %s\n", e.path.c_str(),
                     keep_.store.dir.c_str());
        return;
    }
    d.kept.written = true;
    d.kept.v = d.v;
    d.kept.path = e.path;
}

void Host::discardBackup(Doc& d) {
    if (!d.kept.written) return;
    keep_.store.discard(d.kept.id);
    d.kept.written = false;
    d.kept.v = 0;
    d.kept.path.clear();
}

void Host::writeBackupsNow() {
    if (!keeping_) return;
    for (const SavePlan::Tab& t : keptTabs()) {
        if (!SavePlan::needsBackup(t)) continue;
        auto it = docs_.find(t.id);
        if (it != docs_.end()) writeBackup(t.id, it->second);
    }
}

std::string Host::backupId(DocId d) const {
    auto it = docs_.find(d);
    return it != docs_.end() && it->second.kept.written ? it->second.kept.id : std::string();
}

std::string Host::backupPath(DocId d) const {
    auto it = docs_.find(d);
    return it != docs_.end() && it->second.kept.written ? it->second.kept.path : std::string();
}

// ---------------------------------------------------------------- the disk

namespace {

// `path`'s folder ("/" for a file at the top).
std::string folderOf(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) return std::string();
    return slash == 0 ? std::string("/") : path.substr(0, slash);
}

bool isHigh(char16_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool isLow(char16_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

// The most folders one renderer may name as open (FsRead's "watch").
constexpr std::size_t kMaxWatched = 4096;
// The most listings one FsChanged carries; the rest wait for the tree's
// next look (its refresh, coming back to the page).
constexpr std::size_t kMaxChangedListings = 256;

}  // namespace

void Host::setWatching(bool on) {
    if (watching_ == on) return;
    watching_ = on;
    watchGen_++;
}

int Host::diskState(DocId d) const {
    auto it = docs_.find(d);
    return it == docs_.end() ? 0 : it->second.disk;
}

Json Host::onWatchAsk(ClientId c, std::uint64_t window, const Json& request) {
    Json a = Json::object();
    a.set("op", Json("watch"));
    if (!watching_) {
        a.set("error", Json(std::string("this host does not watch the disk")));
        return a;
    }
    const std::string root = readRoot(window);
    if (root.empty()) {
        a.set("error", Json(std::string("no folder to read in")));
        return a;
    }
    WatchAsk w;
    w.window = window;
    w.root = root;
    w.hidden = request["hidden"].asBool();
    std::set<std::string> seen;
    for (const Json& p : request["paths"].elements()) {
        if (!p.isString() || !DiskWatch::cleanRelative(p.asString())) continue;
        if (seen.insert(p.asString()).second) w.open.push_back(p.asString());
        if (w.open.size() >= kMaxWatched) break;
    }
    auto it = watches_.find(c);
    const bool same = it != watches_.end() && it->second.root == w.root && it->second.open == w.open &&
                      it->second.window == w.window && it->second.hidden == w.hidden;
    if (!same) {
        watches_[c] = w;
        watchGen_++;
    }
    a.set("root", Json(root));
    a.set("watching", Json((std::int64_t)w.open.size()));
    return a;
}

std::vector<DiskWatch::Root> Host::watchRoots() const {
    std::vector<DiskWatch::Root> out;
    if (!watching_) return out;
    std::map<std::string, std::set<std::string>> roots;
    if (!folder_.empty()) roots[folder_];
    for (const auto& [c, w] : watches_) roots[w.root].insert(w.open.begin(), w.open.end());
    // A document's folder is watched whatever the ignore files say, so a
    // file opened from build output follows the disk too.
    for (const auto& [id, d] : docs_) {
        if (d.path.empty() || d.path[0] != '/') continue;
        const std::string dir = folderOf(d.path);
        for (auto& [root, open] : roots) {
            std::string rel;
            if (DiskWatch::relativeTo(root, dir, &rel) && !rel.empty() && DiskWatch::cleanRelative(rel))
                open.insert(rel);
        }
    }
    for (const auto& [root, open] : roots)
        out.push_back(DiskWatch::Root{root, std::vector<std::string>(open.begin(), open.end())});
    return out;
}

void Host::diskChanged(const std::vector<DiskWatch::Change>& changes) {
    if (changes.empty() || !watching_) return;
    // What counts under each root (the filter's rule: not .git's insides,
    // not what the ignore files ignore unless open), and whether Source
    // Control may have changed (a change in a folder not ignored).
    struct Counted {
        std::vector<DiskWatch::Change> changes;
        bool git = false;
    };
    std::map<std::string, Counted> byRoot;
    for (const DiskWatch::Root& r : watchRoots()) {
        DiskWatch::Filter f(r.path, r.open);
        Counted& k = byRoot[r.path];
        for (const DiskWatch::Change& c : changes) {
            std::string rel;
            if (!DiskWatch::relativeTo(r.path, c.path, &rel)) continue;
            const DiskWatch::Seen s = f.seen(rel);
            if (s == DiskWatch::Seen::No) continue;
            k.changes.push_back(c);
            if (s == DiskWatch::Seen::Yes) k.git = true;
        }
    }
    auto hits = [](const std::vector<DiskWatch::Change>& cs, const std::string& path) {
        for (const DiskWatch::Change& c : cs)
            if (c.path == path || (c.below && DiskWatch::under(path, c.path))) return true;
        return false;
    };

    // The trees: each open folder that changed, listed again for the
    // renderers that have it open and hear the disk.
    std::map<std::string, Json> listed;   // root, hidden, path -> the listing
    for (const auto& [c, w] : watches_) {
        auto who = renderers_.find(c);
        if (who == renderers_.end() || !who->second.hearsDisk) continue;
        auto k = byRoot.find(w.root);
        if (k == byRoot.end() || k->second.changes.empty()) continue;
        Json listings = Json::array();
        for (const std::string& rel : w.open) {
            if (listings.size() >= kMaxChangedListings) break;
            if (!hits(k->second.changes, rel.empty() ? w.root : w.root + "/" + rel)) continue;
            const std::string key = w.root + '\n' + (w.hidden ? "1" : "0") + '\n' + rel;
            auto have = listed.find(key);
            if (have == listed.end()) {
                Json req = Json::object();
                req.set("op", Json("list"));
                req.set("path", Json(rel));
                req.set("hidden", Json(w.hidden));
                have = listed.emplace(key, readWorkspace(w.root, req)).first;
            }
            listings.push(have->second);
        }
        if (listings.size() == 0 && !k->second.git) continue;
        Message m;
        m.kind = Kind::FsChanged;
        m.window = w.window;
        Json j = Json::object();
        j.set("root", Json(w.root));
        j.set("listings", std::move(listings));
        j.set("git", Json(k->second.git));
        m.json = std::move(j);
        send(c, std::move(m));
    }

    // The documents whose folder changed.
    bool any = false;
    for (auto& [id, d] : docs_) {
        if (d.path.empty() || d.path[0] != '/') continue;
        const std::string dir = folderOf(d.path);
        bool hit = false;
        for (const auto& [root, k] : byRoot) {
            if (DiskWatch::under(dir, root)) {
                hit = hits(k.changes, dir);
            } else {
                // Spelled otherwise than the root (/tmp under /private/tmp):
                // its folder's real place.
                if (d.realDir.empty()) {
                    char buf[PATH_MAX];
                    const std::string real = realpath(dir.c_str(), buf) ? std::string(buf) : std::string();
                    d.realDir = real.empty() ? dir : real;
                }
                hit = DiskWatch::under(d.realDir, root) && hits(k.changes, d.realDir);
            }
            if (hit) break;
        }
        if (!hit) continue;
        lookAtDisk(id, d);
        any = true;
    }
    if (any) backupsChanged();
}

void Host::setDisk(DocId id, Doc& d, int disk) {
    d.disk = disk;
    if (d.toldDisk == disk) return;
    d.toldDisk = disk;
    for (const auto& [c, r] : renderers_) {
        if (!r.hearsDisk) continue;
        Message m;
        m.kind = Kind::DocDisk;
        m.doc = id;
        m.flag = disk;
        m.v = d.v;
        send(c, std::move(m));
    }
}

void Host::lookAtDisk(DocId id, Doc& d) {
    // An app's window follows its own tab (it reads the file, and tells the
    // host what it holds), so the host leaves it alone.
    if (shownByApp(d)) return;
    const FolderSearch::FileStamp now = FolderSearch::stampOf(d.path);
    // The host's own write or read, or nothing new since the last look.
    if (d.disk == 0 && d.doc.stamp().exists && now == d.doc.stamp()) {
        d.diskSeen = now;
        d.diskLooked = true;
        return;
    }
    if (d.diskLooked && now == d.diskSeen) return;
    DiskText t;
    if (now.exists) t = readDiskText(d.path);
    else t.stamp = now;
    d.diskSeen = t.stamp;
    d.diskLooked = true;
    const std::u16string& saved = d.doc.savedText();
    DiskLook l;
    l.hadFile = d.doc.stamp().exists || !saved.empty() || d.disk != 0;
    l.exists = t.exists;
    l.text = t.text;
    l.sameAsSaved = t.text && t.body == saved;
    l.edited = d.doc.text().size() != saved.size() || !(d.doc.text() == saved);
    l.wasGone = d.doc.gone();
    switch (diskAction(l)) {
    case DiskAction::Nothing: break;
    case DiskAction::Same:
        d.doc.setEncoding(t.encoding, t.bom);
        d.doc.confirmSaved(t.stamp);
        setDisk(id, d, 0);
        tellDirty(id);   // a file back where it went: no longer unsaved for that
        break;
    case DiskAction::Follow:
        takeDiskText(id, d, t);
        setDisk(id, d, 0);
        break;
    case DiskAction::Changed: setDisk(id, d, 1); break;
    case DiskAction::Gone:
        d.doc.setGone(true);
        setDisk(id, d, 2);
        tellDirty(id);
        break;
    }
}

void Host::takeDiskText(DocId id, Doc& d, const DiskText& t) {
    hostStep(id, d, t.body, "Reload from Disk");
    d.doc.setEncoding(t.encoding, t.bom);
    d.doc.markSaved(t.stamp);
    tellDirty(id);
}

void Host::hostStep(DocId id, Doc& d, const std::u16string& next, const char* name) {
    closeStep(d);
    // The one range in which the two differ, never splitting a surrogate
    // pair, so the step keeps everything around it as it was.
    const std::u16string now = d.doc.text().str();
    std::size_t p = 0;
    const std::size_t most = std::min(now.size(), next.size());
    while (p < most && now[p] == next[p]) p++;
    if (p > 0 && isHigh(now[p - 1])) p--;
    std::size_t s = 0;
    while (s < now.size() - p && s < next.size() - p && now[now.size() - 1 - s] == next[next.size() - 1 - s]) s++;
    if (s > 0 && isLow(now[now.size() - s])) s--;
    const Ot::Op op{p, now.size() - p - s, next.substr(p, next.size() - p - s)};
    if (op.del || !op.ins.empty()) {
        d.stepInfo = {name, false};
        d.history.breakCoalescing();
        const Selection before{{p, p}};
        d.history.open(d.stepInfo, before);
        d.stepOpen = true;
        d.stepBy = 0;
        d.stepId = 0;
        d.stepAfter = before;
        Entry e = applyOps(d, Ot::primitives(Ot::Ops{op}), Model::Unit::Utf16, true);
        e.by = 0;
        d.v++;
        d.log.push_back(e);
        closeStep(d);
        broadcast(id, d, e, 0, 0, 0, 0, nullptr);
        trimLog(d);
        tellUndoState(id, d);
    }
}

void Host::onReplace(ClientId c, Message& m) {
    Message r;
    r.kind = Kind::FsData;
    r.req = m.req;
    r.window = m.window;
    auto answer = [&](Json a) {
        a.set("op", Json("replace"));
        a.set("done", Json(true));
        r.json = std::move(a);
        send(c, std::move(r));
    };
    auto fail = [&](const std::string& why) {
        Json e = Json::object();
        e.set("error", Json(why));
        answer(std::move(e));
    };
    // It writes files: never from a renderer that only reads.
    auto who = renderers_.find(c);
    if (who == renderers_.end() || who->second.readOnly) return fail("A page that only reads cannot replace.");
    const std::string root = readRoot(m.window);
    if (root.empty()) return fail("no folder to read in");
    const Json& q = m.json;
    const std::string query = q["query"].asString(), replacement = q["replacement"].asString();
    const bool hidden = q["hidden"].asBool();
    const FolderSearchOptions options = searchOptions(q);
    if (!FolderSearch::isSearchable(query, options)) return fail("Nothing to replace: search for something first.");
    if (readWorkers().running() >= kMaxReadJobs) return fail("too many searches at once on this host; try again in a moment");
    // The documents the host holds are edited here, each one step; the
    // other files go to a worker.
    Json rest = Json::array(), skipped = Json::array();
    std::int64_t edited = 0, editedMatches = 0;
    std::size_t taken = 0;
    for (const Json& f : q["files"].elements()) {
        if (!f.isObject() || !f["path"].isString() || taken >= kMaxReplaceRows) continue;
        std::vector<ReplaceRow> rows;
        if (!replaceRowsFrom(f["rows"], kMaxReplaceRows - taken, &rows) || rows.empty()) continue;
        taken += rows.size();
        const std::string rel = f["path"].asString();
        std::string abs, why;
        const DocId id = (!hidden && replaceHidesPath(rel)) || !confinedPath(root, rel, &abs, &why)
                             ? 0 : docAtPath(abs, 0);
        if (!id) {   // the worker names it, or rewrites it
            Json g = Json::object();
            g.set("path", Json(rel));
            g.set("rows", f["rows"]);
            rest.push(std::move(g));
            continue;
        }
        Doc& d = docs_[id];
        const std::string name = nameIn(rel);
        auto skip = [&](const char* reason, const std::string& says) {
            Json s = Json::object();
            s.set("path", Json(rel));
            s.set("reason", Json(reason));
            s.set("why", Json(name + says));
            skipped.push(std::move(s));
        };
        // Unsaved changes are the person's, as the windows' Replace in
        // Folder leaves them.
        if (d.doc.dirty()) {
            skip("unsaved", " has unsaved changes: it was left alone.");
            continue;
        }
        std::string out;
        std::size_t n = 0;
        const ReplaceCheck rc =
            replaceRows(Model::utf8FromUtf16(d.doc.text().str()), rows, query, replacement, options, &out, &n);
        if (rc == ReplaceCheck::Nothing) continue;
        if (rc == ReplaceCheck::Changed) {
            skip("changed", " changed since the search: it was left alone.");
            continue;
        }
        hostStep(id, d, Model::utf16FromUtf8(out), "Replace in Folder");
        tellDirty(id);
        edited++;
        editedMatches += (std::int64_t)n;
    }
    Json job = q;
    job.set("files", std::move(rest));
    Json ed = Json::object();
    ed.set("edited", Json(edited));
    ed.set("editedMatches", Json(editedMatches));
    ed.set("skipped", skipped);
    job.set("edited", std::move(ed));
    std::string why;
    if (readWorkers().start(c, m.req, m.window, root, job, &why, readSeconds_)) return;
    // No worker after all: what was done here is said, and why the rest was not.
    Json a = Json::object();
    a.set("error", Json(why));
    a.set("replaced", Json(editedMatches));
    a.set("files", Json((std::int64_t)0));
    a.set("edited", Json(edited));
    a.set("editedMatches", Json(editedMatches));
    a.set("skipped", std::move(skipped));
    answer(std::move(a));
}

void Host::onReload(ClientId c, Message& m) {
    Message r;
    r.kind = Kind::DocReloadDone;
    r.doc = m.doc;
    r.req = m.req;
    r.outcome = Outcome::Refused;
    std::string path, reason, why;
    auto it = docs_.find(m.doc);
    if (it == docs_.end()) {
        reason = "notopen";
        why = "That document is no longer open.";
    } else {
        Doc& d = it->second;
        path = d.path;
        const std::string name = path.empty() ? std::string("This document") : path.substr(path.rfind('/') + 1);
        if (path.empty() || path[0] != '/') {
            reason = "nofile";
            why = name + " has no file to reload from.";
        } else if (shownByApp(d)) {
            // The app's window has its own Reload, and its own questions.
            reason = "app";
            why = name + " is open in a mutible window: reload it there.";
        } else {
            const DiskText t = readDiskText(path);
            if (!t.exists) {
                reason = "gone";
                why = name + " is not on disk: there is nothing to reload.";
            } else if (!t.text) {
                reason = "notext";
                why = name + " on disk is not text that can be shown here.";
            } else {
                takeDiskText(m.doc, d, t);
                d.diskSeen = t.stamp;
                d.diskLooked = true;
                setDisk(m.doc, d, 0);
                r.outcome = Outcome::Done;
                r.v = d.v;
            }
        }
    }
    Json j = Json::object();
    j.set("path", Json(path));
    j.set("ok", Json(r.outcome == Outcome::Done));
    if (!reason.empty()) {
        j.set("reason", Json(reason));
        j.set("why", Json(why));
    }
    r.json = std::move(j);
    send(c, std::move(r));
}

// ---------------------------------------------------------------- terminals

void applyTermEvent(TermShell& s, const TermEvent& e) {
    switch (e.kind) {
    case TermEvent::PromptStart:
        s.known = true;
        if (s.busy) s.done = s.started;   // a command whose end never came
        s.busy = false;
        s.prompts++;
        s.atPrompt = true;
        s.inputReady = false;
        s.command.clear();
        break;
    case TermEvent::InputStart:
        s.known = true;
        s.inputReady = true;
        break;
    case TermEvent::CommandStart:
        s.known = true;
        s.started++;
        s.busy = true;
        s.atPrompt = false;
        s.inputReady = false;
        s.command = e.text;
        break;
    case TermEvent::CommandEnd:
        s.known = true;
        if (s.busy) {
            s.done = s.started;
            s.status = e.status;
            s.busy = false;
            s.command.clear();
        }
        break;
    case TermEvent::Directory: s.cwd = e.text; break;
    case TermEvent::Title: s.title = e.text; break;
    default: break;
    }
}

bool Host::openPty(ClientId c, TermId id, const TermSpec& spec, std::string* error) {
    if (terms_.count(id)) {
        if (error) *error = "a terminal with that id is open";
        return false;
    }
    PtySpec p;
    p.program = spec.program;
    p.argv = spec.argv;
    p.env = spec.env;
    p.cwd = spec.cwd;
    p.cols = std::max(2, spec.cols);
    p.rows = std::max(1, spec.rows);
    // A renderer on another machine asks for this machine's own shell.
    if (spec.hostShell) p = ownShellSpec(ownShell_, ownEnv_, spec.env, spec.cwd, p.cols, p.rows);
    std::unique_ptr<Pty> pty = Pty::spawn(p, error);
    if (!pty) return false;
    Term& t = terms_[id];
    if (c) {
        Term::Viewer& v = t.viewers[c];
        v.cols = p.cols;
        v.rows = p.rows;
        v.joined = ++order_;
        t.opener = c;
    }
    t.pty = std::move(pty);
    t.screen = TerminalScreen(p.cols, p.rows);
    t.shell.cols = p.cols;
    t.shell.rows = p.rows;
    t.screen.setScrollbackLimit(std::max(0, spec.scrollback));
    t.nonce = spec.nonce;
    t.screen.setMarkNonce(spec.nonce);
    t.stream.setMarkNonce(spec.nonce);
    t.stream.setEventsOnly(true);
    t.screen.takeDirty();
    return true;
}

void Host::onTermOpen(ClientId c, Message& m) {
    std::string why;
    if (!openPty(c, m.term, termSpecFrom(m.json), &why)) {
        Message r;
        r.kind = Kind::TermExited;
        r.term = m.term;
        r.flag = -1;
        r.name = why.empty() ? "the shell could not be started" : why;
        send(c, std::move(r));
        return;
    }
    Term& t = terms_[m.term];
    Message r;
    r.kind = Kind::TermOpened;
    r.term = m.term;
    r.v = (std::uint64_t)t.pty->pid();
    send(c, std::move(r));
    look(t);
    sendShell(m.term, t, true);
}

void Host::onTermAttach(ClientId c, Message& m) {
    auto it = terms_.find(m.term);
    if (it == terms_.end()) {
        Message r;
        r.kind = Kind::TermExited;
        r.term = m.term;
        r.flag = -1;
        r.name = "no such terminal";
        send(c, std::move(r));
        return;
    }
    Term& t = it->second;
    // A renderer attaching again starts afresh, keeping its size and when
    // it typed and joined.
    Term::Viewer& v = t.viewers[c];
    v.unacked = 0;
    if (!v.joined) v.joined = ++order_;
    Message r;
    r.kind = Kind::TermReplay;
    r.term = m.term;
    r.bytes = t.screen.replay(t.nonce);
    r.v = (std::uint64_t)(t.screen.historyBase() + t.screen.cursorRow());
    r.flag = t.screen.altScreen() ? 1 : 0;
    send(c, std::move(r));
    sendShell(m.term, t, true);
    sendMarks(m.term, t);
}

void Host::sendShell(TermId id, Term& t, bool always) {
    if (!always && t.told && t.shell == t.sent) return;
    t.sent = t.shell;
    t.told = true;
    if (t.viewers.empty()) return;
    Message r;
    r.kind = Kind::TermShell;
    r.term = id;
    r.json = termShellJson(t.shell);
    sendToViewers(t, r);
}

const Host::Term::Viewer* Host::sizer(const Term& t) const {
    const Term::Viewer* best = nullptr;
    for (const auto& [c, v] : t.viewers)
        if (v.cols && v.typed && (!best || v.typed > best->typed)) best = &v;
    if (best) return best;
    auto o = t.viewers.find(t.opener);
    if (t.opener && o != t.viewers.end() && o->second.cols) return &o->second;
    for (const auto& [c, v] : t.viewers)
        if (v.cols && (!best || v.joined < best->joined)) best = &v;
    return best;
}

void Host::fit(TermId id, Term& t) {
    const Term::Viewer* v = sizer(t);
    if (!v || (v->cols == t.screen.cols() && v->rows == t.screen.rows())) return;
    const int cols = v->cols, rows = v->rows;
    t.screen.resize(cols, rows);
    if (t.pty) t.pty->resize(cols, rows);
    t.sizedAt = std::chrono::steady_clock::now();
    t.shell.cols = cols;
    t.shell.rows = rows;
    if (t.screen.takeDirty().marks) sendMarks(id, t);
    sendShell(id, t);
}

void Host::sendToViewers(const Term& t, const Message& m) {
    for (const auto& [c, v] : t.viewers) {
        (void)v;
        Message copy = m;
        send(c, std::move(copy));
    }
}

void Host::sendMarks(TermId id, Term& t) {
    if (t.viewers.empty()) return;
    const long long base = t.screen.historyBase();
    std::vector<TermMark> marks;
    for (const TermCommand& c : t.screen.commands()) {
        TermMark k;
        k.prompt = base + c.promptLine;
        k.output = c.ran ? base + c.outputLine : -1;
        k.end = c.ended ? base + c.endLine : -1;
        k.status = c.status;
        marks.push_back(k);
    }
    Message r;
    r.kind = Kind::TermMarks;
    r.term = id;
    r.json = termMarksJson(marks);
    sendToViewers(t, r);
}

void Host::look(Term& t) {
    if (!t.pty) return;
    const auto now = std::chrono::steady_clock::now();
    // A resize bash's readline undid (it reads the size and writes it back
    // whenever it takes the terminal: a resize between the two is lost, the
    // pty left at the old size while the screen and every renderer are at
    // the new). Put back, which sends the shell SIGWINCH again; only for a
    // while after the host resized, so a program's own `stty cols` stands.
    // Looks come with output (readline's prompt follows its write) and
    // every 200 ms.
    if (now - t.sizedAt < std::chrono::seconds(5)) t.pty->keepSize();
    t.shell.foreground = t.pty->foreground();
    t.shell.raw = t.pty->raw();
    t.shell.echoOff = t.pty->echoOff();
    t.shell.signalsOff = t.pty->signalsOff();
    t.nextLook = now + std::chrono::milliseconds(200);
}

void Host::output(TermId id, Term& t, std::string&& bytes) {
    if (bytes.empty()) return;
    // Raw with signals off is a program a widget runs (fzf), counting
    // clusters; zsh's line editor keeps ISIG (the Mac's rule, Terminal.mm).
    t.screen.setRawTty(t.shell.signalsOff);
    t.screen.feed(bytes);
    t.screen.takeReplies();   // the renderer's emulator answers
    for (const TermEvent& e : t.stream.feed(bytes)) applyTermEvent(t.shell, e);
    const bool marks = t.screen.takeDirty().marks;
    look(t);
    // And again shortly: a shell prints a command's start mark before the
    // command takes the foreground.
    t.nextLook = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    if (!t.viewers.empty()) {
        Message r;
        r.kind = Kind::TermOutput;
        r.term = id;
        r.v = (std::uint64_t)(t.screen.historyBase() + t.screen.cursorRow());
        r.flag = t.screen.altScreen() ? 1 : 0;
        for (auto& [c, v] : t.viewers) v.unacked += (long long)bytes.size();
        r.bytes = std::move(bytes);
        sendToViewers(t, r);
    }
    // After the output, so a renderer has fed the bytes the marks are in.
    if (marks) sendMarks(id, t);
    sendShell(id, t);
}

void Host::pollSet(std::vector<PollFd>* out) const {
    for (const auto& [id, t] : terms_) {
        if (!t.pty || t.pty->fd() < 0) continue;
        PollFd p;
        p.fd = t.pty->fd();
        p.term = id;
        p.read = true;
        for (const auto& [c, v] : t.viewers) p.read = p.read && v.unacked < kTermCredit;
        p.write = t.pty->wantsWrite();
        if (p.read || p.write) out->push_back(p);
    }
}

void Host::readable(TermId id) {
    auto it = terms_.find(id);
    if (it == terms_.end() || !it->second.pty) return;
    Term& t = it->second;
    std::string bytes;
    const bool more = t.pty->read(bytes);
    output(id, t, std::move(bytes));
    if (!more) endTerminal(id, true);
}

void Host::writable(TermId id) {
    auto it = terms_.find(id);
    if (it != terms_.end() && it->second.pty) it->second.pty->flush();
}

void Host::endTerminal(TermId id, bool drain, ClientId closer) {
    auto it = terms_.find(id);
    if (it == terms_.end()) return;
    Term& t = it->second;
    if (drain && t.pty) {
        // What the shell wrote before it went comes first.
        for (int i = 0; i < 64; i++) {
            std::string bytes;
            const bool more = t.pty->read(bytes);
            if (bytes.empty()) break;
            output(id, t, std::move(bytes));
            if (!more) break;
        }
    }
    if (t.pty) {
        t.pty->hangUp();
        Dying d;
        d.term = id;
        for (const auto& [c, v] : t.viewers)
            if (c != closer) d.tell.push_back(c);
        d.pty = std::move(t.pty);
        dying_.push_back(std::move(d));
    }
    terms_.erase(it);
}

int Host::tick() {
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < dying_.size();) {
        int status = -1;
        if (!dying_[i].pty->reap(&status)) {
            i++;
            continue;
        }
        for (ClientId c : dying_[i].tell) {
            if (!renderers_.count(c)) continue;
            Message r;
            r.kind = Kind::TermExited;
            r.term = dying_[i].term;
            r.flag = status;
            send(c, std::move(r));
        }
        dying_.erase(dying_.begin() + (long)i);
    }
    std::vector<TermId> exited;
    for (auto& [id, t] : terms_) {
        if (!t.pty) continue;
        // A shell gone while a program it started holds the pty open: the
        // terminal ends with the shell, as VTE's child-exited does.
        if (t.pty->exited(nullptr)) {
            exited.push_back(id);
            continue;
        }
        if (now >= t.nextLook) {
            look(t);
            sendShell(id, t);
        }
    }
    for (TermId id : exited) endTerminal(id, true);
    // Backups planned for now (keepBackups), and when the next is.
    long long backupWait = -1;
    if (keeping_) {
        const double mono = monoNow();
        if (keepPlan_.nextDue() <= mono) {
            const SavePlan::Plan p = keepPlan_.tick(keptTabs(), mono);
            for (std::uint64_t id : p.writeBackups) {
                auto it = docs_.find(id);
                if (it != docs_.end()) writeBackup(id, it->second);
            }
        }
        const double due = keepPlan_.nextDue();
        if (due != SavePlan::kNever)
            backupWait = std::max(1LL, (long long)std::ceil((due - monoNow()) * 1000.0));
    }
    auto sooner = [backupWait](long long ms) {
        return (int)(backupWait < 0 ? ms : ms < 0 ? backupWait : std::min(ms, backupWait));
    };
    if (!dying_.empty()) return sooner(50);
    if (terms_.empty()) return sooner(-1);
    long long wait = 200;
    for (const auto& [id, t] : terms_) {
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.nextLook - now).count();
        wait = std::max(1LL, std::min(wait, ms));
    }
    return sooner(wait);
}

const TerminalScreen* Host::screen(TermId t) const {
    auto it = terms_.find(t);
    return it == terms_.end() ? nullptr : &it->second.screen;
}

TermShell Host::shell(TermId t) const {
    auto it = terms_.find(t);
    return it == terms_.end() ? TermShell() : it->second.shell;
}

int Host::ptyFd(TermId t) const {
    auto it = terms_.find(t);
    return it == terms_.end() || !it->second.pty ? -1 : it->second.pty->fd();
}

int Host::shellPid(TermId t) const {
    auto it = terms_.find(t);
    return it == terms_.end() || !it->second.pty ? 0 : it->second.pty->pid();
}

// ---------------------------------------------------------------- SessionHost

SessionHost::SessionHost(Host::Sink sink, unsigned delayUs)
    : host_(std::move(sink)), delayUs_(delayUs) {
    if (pipe(wake_) == 0) {
        for (int fd : wake_) {
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        }
    }
    thread_ = std::thread([this] { loop(); });
}

SessionHost::~SessionHost() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    wake();
    if (thread_.joinable()) thread_.join();
    for (int fd : wake_)
        if (fd >= 0) close(fd);
}

// Any thread: the loop takes the queue at its next turn, out of poll at once.
void SessionHost::wake() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (wakePending_) return;
        wakePending_ = true;
    }
    const char b = 1;
    while (write(wake_[1], &b, 1) < 0 && errno == EINTR) {}
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
    wake();
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return done; });
    return id;
}

void SessionHost::post(ClientId from, Message&& m) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(Item{from, std::move(m), nullptr});
    }
    wake();
}

void SessionHost::disconnect(ClientId c) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(Item{0, Message(), [this, c] { host_.disconnect(c); }});
    }
    wake();
}

void SessionHost::setFolder(std::string folder) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(Item{0, Message(), [this, f = std::move(folder)] { host_.setFolder(f); }});
    }
    wake();
}

void SessionHost::watchDisk(bool on) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(Item{0, Message(), [this, on] {
                                  watchDisk_ = on;
                                  host_.setWatching(on);
                              }});
    }
    wake();
}

bool SessionHost::diskWatchSettled(int timeoutMs, std::size_t* folders) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeoutMs));
    for (;;) {
        auto st = std::make_shared<std::pair<std::atomic<int>, std::atomic<std::size_t>>>();
        st->first = -1;
        st->second = 0;
        // On the host's thread, after the roots asked last were handed over.
        {
            std::lock_guard<std::mutex> lk(mu_);
            queue_.push_back(Item{0, Message(), [this, st] {
                                      lookAtWatch();
                                      if (watcher_) {
                                          st->second = watcher_->folders();
                                          st->first = watcher_->settled() ? 1 : 0;
                                      } else {
                                          st->first = 2;
                                      }
                                  }});
        }
        wake();
        while (st->first < 0 && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (folders) *folders = st->second;
        if (st->first == 1) return true;
        if (st->first == 2 || std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void SessionHost::lookAtWatch() {
    if (!watchDisk_) {
        watcher_.reset();
        coalesce_.take();
        return;
    }
    if (host_.watchGeneration() == watchSeen_) return;
    watchSeen_ = host_.watchGeneration();
    const std::vector<DiskWatch::Root> roots = host_.watchRoots();
    std::vector<std::string> paths;
    for (const DiskWatch::Root& r : roots) paths.push_back(r.path);
    coalesce_.setRoots(paths);
    if (roots.empty()) {
        watcher_.reset();
        return;
    }
    if (!watcher_) watcher_ = DiskWatch::Watcher::make();
    if (watcher_) watcher_->watch(roots);
}

void SessionHost::setAskedFile(std::string file) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(Item{0, Message(), [this, f = std::move(file)] { host_.setAskedFile(f); }});
    }
    wake();
}

void SessionHost::setOwnShell(std::vector<std::string> env, std::string shell) {
    auto shared = std::make_shared<std::pair<std::vector<std::string>, std::string>>(std::move(env),
                                                                                       std::move(shell));
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(Item{0, Message(), [this, shared] {
                                  host_.setOwnShell(std::move(shared->first), std::move(shared->second));
                              }});
    }
    wake();
}

void SessionHost::keepBackups(Host::BackupKeeping k) {
    auto shared = std::make_shared<Host::BackupKeeping>(std::move(k));
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(Item{0, Message(), [this, shared] { host_.keepBackups(std::move(*shared)); }});
    }
    wake();
}

bool SessionHost::inspect(const std::function<void(const Host&)>& f, int timeoutMs) {
    // Shared with the queued item, which may outlive this call when it
    // times out: it then finds `abandoned` and leaves `f` (whose captures
    // are this caller's) alone.
    struct Wait {
        std::mutex m;
        std::condition_variable cv;
        bool started = false, done = false, abandoned = false;
    };
    auto w = std::make_shared<Wait>();
    const std::function<void(const Host&)>* fp = &f;
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(Item{0, Message(), [this, w, fp] {
                                  {
                                      std::lock_guard<std::mutex> l(w->m);
                                      if (w->abandoned) return;
                                      w->started = true;
                                  }
                                  (*fp)(host_);
                                  std::lock_guard<std::mutex> l(w->m);
                                  w->done = true;
                                  w->cv.notify_all();
                              }});
    }
    wake();
    std::unique_lock<std::mutex> lk(w->m);
    if (w->cv.wait_for(lk, std::chrono::milliseconds(std::max(0, timeoutMs)), [&] { return w->done; }))
        return true;
    if (!w->started) {
        w->abandoned = true;
        return false;
    }
    w->cv.wait(lk, [&] { return w->done; });   // `f` is running: it ends
    return true;
}

namespace {
double monoSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

// Messages first, then whatever the ptys have, then a wait in poll(2) for
// either. A terminal's output is read 64 KB a turn, so messages (a key the
// user typed) never wait behind a program printing without pause.
void SessionHost::loop() {
    std::vector<Host::PollFd> fds;
    std::vector<pollfd> polls;
    for (;;) {
        char buf[64];
        while (read(wake_[0], buf, sizeof buf) > 0) {}
        std::deque<Item> items;
        bool stopping;
        {
            std::lock_guard<std::mutex> lk(mu_);
            wakePending_ = false;
            items.swap(queue_);
            stopping = stop_;
        }
        for (Item& item : items) {
            if (delayUs_) std::this_thread::sleep_for(std::chrono::microseconds(delayUs_));
            if (item.run) item.run();
            else host_.handle(item.from, std::move(item.m));
        }
        if (stopping && items.empty()) {
            // Everything posted is done; what typing left unwritten goes to
            // disk before the host ends (a signal ended it unsaved).
            host_.writeBackupsNow();
            watcher_.reset();
            return;
        }
        // What the workers answered (Quick Open's list, a search's rows).
        host_.takeReads();
        // The disk: what to watch, if that changed, and a batch that is due.
        lookAtWatch();
        if (coalesce_.due(monoSeconds())) host_.diskChanged(coalesce_.take());
        int timeout = host_.tick();
        const double toBatch = coalesce_.wait(monoSeconds());
        if (toBatch >= 0) {
            const int ms = (int)std::ceil(toBatch * 1000);
            timeout = timeout < 0 ? ms : std::min(timeout, ms);
        }
        fds.clear();
        host_.pollSet(&fds);
        polls.assign(1, pollfd{wake_[0], POLLIN, 0});
        const int watchFd = watcher_ ? watcher_->fd() : -1;
        if (watchFd >= 0) polls.push_back(pollfd{watchFd, POLLIN, 0});
        // The workers' answers wake the loop; takeReads at its top sends them.
        const int readsFd = host_.readsFd();
        if (readsFd >= 0) polls.push_back(pollfd{readsFd, POLLIN, 0});
        const std::size_t firstPty = polls.size();
        for (const Host::PollFd& f : fds)
            polls.push_back(pollfd{f.fd, (short)((f.read ? POLLIN : 0) | (f.write ? POLLOUT : 0)), 0});
        int n;
        do n = poll(polls.data(), (nfds_t)polls.size(), stopping ? 0 : timeout);
        while (n < 0 && errno == EINTR);
        if (n <= 0) continue;
        if (watchFd >= 0 && polls[1].revents) {
            std::vector<DiskWatch::Change> changes;
            watcher_->take(&changes);
            const double now = monoSeconds();
            for (const DiskWatch::Change& c : changes) coalesce_.add(c, now);
        }
        for (size_t i = 0; i < fds.size(); i++) {
            const short r = polls[i + firstPty].revents;
            if (!r) continue;
            if (r & POLLOUT) host_.writable(fds[i].term);
            if (r & (POLLIN | POLLHUP | POLLERR)) host_.readable(fds[i].term);
        }
    }
}

}  // namespace Shared
