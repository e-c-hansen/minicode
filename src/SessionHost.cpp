// SessionHost.cpp — see SessionHost.h.
#include "SessionHost.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace Shared {

// ---------------------------------------------------------------- Host

ClientId Host::connect() { return ++nextClient_; }

void Host::disconnect(ClientId c) {
    renderers_.erase(c);
    // Its terminals stay (stage 6 keeps them for the next renderer); their
    // output goes nowhere meanwhile, unthrottled.
    for (auto& [id, t] : terms_)
        if (t.owner == c) {
            t.owner = 0;
            t.unacked = 0;
        }
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
    case Kind::TermOpen: onTermOpen(from, m); break;
    case Kind::TermAttach: onTermAttach(from, m); break;
    case Kind::TermInput: {
        auto it = terms_.find(m.term);
        if (it != terms_.end() && it->second.pty) it->second.pty->write(m.bytes);
        break;
    }
    case Kind::TermResize: {
        auto it = terms_.find(m.term);
        if (it == terms_.end() || m.cols < 2 || m.rows < 1) break;
        Term& t = it->second;
        if (m.cols == t.screen.cols() && m.rows == t.screen.rows()) break;
        t.screen.resize(m.cols, m.rows);
        if (t.pty) t.pty->resize(m.cols, m.rows);
        if (t.screen.takeDirty().marks) sendMarks(it->first, t);
        break;
    }
    case Kind::TermAck: {
        auto it = terms_.find(m.term);
        if (it != terms_.end()) it->second.unacked = std::max(0LL, it->second.unacked - (long long)m.v);
        break;
    }
    case Kind::TermClose:
        if (terms_.count(m.term)) endTerminal(m.term, 0);
        break;
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
    std::unique_ptr<Pty> pty = Pty::spawn(p, error);
    if (!pty) return false;
    Term& t = terms_[id];
    t.owner = c;
    t.pty = std::move(pty);
    t.screen = TerminalScreen(p.cols, p.rows);
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
    t.owner = c;
    t.unacked = 0;
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
    if (!t.owner) return;
    Message r;
    r.kind = Kind::TermShell;
    r.term = id;
    r.json = termShellJson(t.shell);
    send(t.owner, std::move(r));
}

void Host::sendMarks(TermId id, Term& t) {
    if (!t.owner) return;
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
    send(t.owner, std::move(r));
}

void Host::look(Term& t) {
    if (!t.pty) return;
    t.shell.foreground = t.pty->foreground();
    t.shell.raw = t.pty->raw();
    t.shell.echoOff = t.pty->echoOff();
    t.nextLook = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
}

void Host::output(TermId id, Term& t, std::string&& bytes) {
    if (bytes.empty()) return;
    t.screen.setRawTty(t.shell.raw);
    t.screen.feed(bytes);
    t.screen.takeReplies();   // the renderer's emulator answers
    for (const TermEvent& e : t.stream.feed(bytes)) applyTermEvent(t.shell, e);
    const bool marks = t.screen.takeDirty().marks;
    look(t);
    if (t.owner) {
        Message r;
        r.kind = Kind::TermOutput;
        r.term = id;
        r.v = (std::uint64_t)(t.screen.historyBase() + t.screen.cursorRow());
        r.flag = t.screen.altScreen() ? 1 : 0;
        t.unacked += (long long)bytes.size();
        r.bytes = std::move(bytes);
        send(t.owner, std::move(r));
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
        p.read = !t.owner || t.unacked < kTermCredit;
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
    if (!more) endTerminal(id, t.owner);
}

void Host::writable(TermId id) {
    auto it = terms_.find(id);
    if (it != terms_.end() && it->second.pty) it->second.pty->flush();
}

void Host::endTerminal(TermId id, ClientId tell) {
    auto it = terms_.find(id);
    if (it == terms_.end()) return;
    Term& t = it->second;
    if (tell && t.pty) {
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
        d.tell = tell;
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
        if (dying_[i].tell && renderers_.count(dying_[i].tell)) {
            Message r;
            r.kind = Kind::TermExited;
            r.term = dying_[i].term;
            r.flag = status;
            send(dying_[i].tell, std::move(r));
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
    for (TermId id : exited) endTerminal(id, terms_[id].owner);
    if (!dying_.empty()) return 50;
    return terms_.empty() ? -1 : 200;
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
    wake();
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return done; });
}

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
        if (stopping && items.empty()) return;   // everything posted is done
        const int timeout = host_.tick();
        fds.clear();
        host_.pollSet(&fds);
        polls.assign(1, pollfd{wake_[0], POLLIN, 0});
        for (const Host::PollFd& f : fds)
            polls.push_back(pollfd{f.fd, (short)((f.read ? POLLIN : 0) | (f.write ? POLLOUT : 0)), 0});
        int n;
        do n = poll(polls.data(), (nfds_t)polls.size(), stopping ? 0 : timeout);
        while (n < 0 && errno == EINTR);
        if (n <= 0) continue;
        for (size_t i = 0; i < fds.size(); i++) {
            const short r = polls[i + 1].revents;
            if (!r) continue;
            if (r & POLLOUT) host_.writable(fds[i].term);
            if (r & (POLLIN | POLLHUP | POLLERR)) host_.readable(fds[i].term);
        }
    }
}

}  // namespace Shared
