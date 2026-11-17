// session_host_tests.inc — the `ot:*` and `host:*` groups of
// run_tests.cpp: edits crossing in flight (Ot.h), the protocol's messages
// (SessionProtocol.h), and the host and its renderers (SessionHost.h,
// SessionClient.h), SESSION-DESIGN.md stage 4. The fuzz tests are the
// oracle: renderers in both units edit, undo and redo at random while their
// messages are held back and delivered in random order (each link keeps
// its own order, as a pipe does), and once everything is delivered every
// renderer's mirror, and every copy of the host's text, must be the host's
// text. Included there, after undo_history_tests.inc.

#include <random>

#include "EditorTabs.h"
#include "Ot.h"
#include "SessionClient.h"
#include "SessionHost.h"
#include "SessionProtocol.h"

namespace {

using Shared::Message;
using Model::Unit;

const std::u16string kPieces[] = {u"a", u"b", u"é", u"\n", u"\U0001F600", u"xy", u" "};

std::u16string randomInsert(std::mt19937& rng, int most) {
    std::u16string s;
    const int n = (int)(rng() % (unsigned)(most + 1));
    for (int i = 0; i < n; i++) s += kPieces[rng() % (sizeof kPieces / sizeof kPieces[0])];
    return s;
}

// One operation that fits `text` (in `u`). In UTF-16 it never splits a
// surrogate pair, as no text view's edit does: a renderer counting code
// points could not follow one that did.
Ot::Op randomOp(std::mt19937& rng, const std::u16string& text, Unit u) {
    const std::size_t points = Model::countCodePoints(text);
    Ot::Op o;
    std::size_t at = rng() % (points + 1);
    const std::size_t room = points - at;
    std::size_t del = room ? rng() % (std::min<std::size_t>(room, 4) + 1) : 0;
    if (u == Unit::Utf16) {
        const std::size_t a = EditSpans::unitsAt(text, at), b = EditSpans::unitsAt(text, at + del);
        at = a;
        del = b - a;
    }
    o.pos = at;
    o.del = del;
    o.ins = randomInsert(rng, 2);
    if (o.del == 0 && o.ins.empty()) o.ins = u"q";
    return o;
}

// Up to `most` operations made one after another on `text`.
Ot::Ops randomOps(std::mt19937& rng, const std::u16string& text, Unit u, int most) {
    std::u16string t = text;
    Ot::Ops ops;
    const int n = 1 + (int)(rng() % (unsigned)most);
    for (int i = 0; i < n; i++) {
        Ot::Op o = randomOp(rng, t, u);
        Ot::apply(t, Ot::Ops{o}, u);
        ops.push_back(o);
    }
    return ops;
}

// A document's mirror as a renderer keeps it: a plain string in the
// renderer's unit, and one caret.
struct TestMirror : Shared::Client::Listener {
    Shared::Client* client = nullptr;
    Shared::DocId doc = 0;
    Unit unit = Unit::Utf16;
    std::u16string text;
    std::size_t caret = 0;
    int hostEdits = 0;
    void hostEdited(Shared::DocId, const Ot::Ops& ops, int, const Shared::Selection* sel) override {
        Ot::apply(text, ops, unit);
        caret = Ot::mapOffset(caret, ops, unit);
        if (sel && !sel->empty()) caret = (*sel)[0].caret;
        caret = std::min(caret, length());
        hostEdits++;
    }
    std::size_t length() const { return Ot::lengthOf(text, unit); }
    Shared::Selection sel() const { return {{caret, caret}}; }
    // Typing: one coalescing step of one insertion at the caret.
    void type(const std::u16string& s) {
        client->openStep(doc, {"Typing", true}, sel());
        Ot::Op o{caret, 0, s};
        Ot::apply(text, Ot::Ops{o}, unit);
        client->record(doc, o);
        caret += Ot::lengthOf(s, unit);
        client->closeStep(doc, sel());
    }
    // A step of up to three operations anywhere.
    void edit(std::mt19937& rng, const std::string& name) {
        client->openStep(doc, {name, name == "Typing"}, sel());
        const Ot::Ops ops = randomOps(rng, text, unit, 3);
        for (const Ot::Op& o : ops) {
            Ot::apply(text, Ot::Ops{o}, unit);
            client->record(doc, o);
            caret = std::min(o.pos + Ot::lengthOf(o.ins, unit), length());
        }
        client->closeStep(doc, sel());
    }
    void moveCaret(std::mt19937& rng) {
        caret = rng() % (Model::countCodePoints(text) + 1);
        if (unit == Unit::Utf16) caret = EditSpans::unitsAt(text, caret);
        client->noteSelection(doc, sel());
    }
};

// A host and renderers whose messages wait in queues, one each way per
// renderer, until the test delivers them; or, `immediate`, go at once (the
// renderer's waits then return), as in one process.
struct Net {
    struct Link {
        std::unique_ptr<Shared::Client> client;
        Shared::ClientId id = 0;
        std::deque<Message> up, down;
        std::map<Shared::DocId, std::unique_ptr<TestMirror>> mirrors;
    };
    bool loopback = false, immediate = false;
    Shared::Host host;
    std::vector<std::unique_ptr<Link>> links;
    size_t delivered = 0;

    Net(bool loop, bool now)
        : loopback(loop), immediate(now), host([this](Shared::ClientId to, Message&& m) { toClient(to, std::move(m)); }) {}

    Message wire(Message&& m) const { return loopback ? Shared::roundTrip(m) : std::move(m); }

    void toClient(Shared::ClientId to, Message&& m) {
        for (auto& l : links)
            if (l->id == to) {
                if (immediate) l->client->deliver(wire(std::move(m)));
                else l->down.push_back(wire(std::move(m)));
            }
    }

    Link& add(Unit u, const std::string& name) {
        links.push_back(std::make_unique<Link>());
        Link* l = links.back().get();
        l->id = host.connect();
        l->client = std::make_unique<Shared::Client>(u, name);
        l->client->connect([this, l](Message&& m) {
            if (immediate) host.handle(l->id, wire(std::move(m)));
            else l->up.push_back(wire(std::move(m)));
        });
        return *l;
    }

    TestMirror& open(Link& l, Shared::DocId doc, const std::u16string& text) {
        auto m = std::make_unique<TestMirror>();
        m->client = l.client.get();
        m->doc = doc;
        m->unit = l.client->unit();
        m->text = text;
        TestMirror& ref = *m;
        l.mirrors[doc] = std::move(m);
        l.client->open(doc, "/tmp/doc" + std::to_string(doc), Model::utf8FromUtf16(text), &ref);
        return ref;
    }

    // One message along a random non-empty queue; false when none waits.
    bool step(std::mt19937& rng) {
        std::vector<std::pair<Link*, bool>> ready;
        for (auto& l : links) {
            if (!l->up.empty()) ready.push_back({l.get(), true});
            if (!l->down.empty()) ready.push_back({l.get(), false});
        }
        if (ready.empty()) return false;
        auto [l, up] = ready[rng() % ready.size()];
        if (up) {
            Message m = std::move(l->up.front());
            l->up.pop_front();
            host.handle(l->id, std::move(m));
        } else {
            Message m = std::move(l->down.front());
            l->down.pop_front();
            l->client->deliver(std::move(m));
            l->client->dispatch();
        }
        delivered++;
        return true;
    }

    void quiesce(std::mt19937& rng) {
        while (step(rng)) {}
        for (auto& l : links) l->client->dispatch();
    }
};

std::string u8(const std::u16string& s) { return Model::utf8FromUtf16(s); }

}  // namespace

static void testSessionHost() {
    GROUP("ot:primitives");
    {
        const Ot::Ops p = Ot::primitives({Ot::Op{2, 3, u"xy"}, Ot::Op{0, 0, u""}, Ot::Op{1, 0, u"z"}});
        CHECK(p.size() == 3);
        CHECK(p[0] == (Ot::Op{2, 3, u""}));
        CHECK(p[1] == (Ot::Op{2, 0, u"xy"}));
        CHECK(p[2] == (Ot::Op{1, 0, u"z"}));
        std::size_t after = 0;
        CHECK(Ot::valid({Ot::Op{2, 3, u"xy"}}, 5, Unit::Utf16, &after) && after == 4);
        CHECK(!Ot::valid({Ot::Op{3, 3, u""}}, 5, Unit::Utf16));
        // A code point is one unit in CodePoint, two in UTF-16.
        std::u16string t = u"a\U0001F600b";
        Ot::apply(t, {Ot::Op{1, 1, u"-"}}, Unit::CodePoint);
        CHECK(t == u"a-b");
        Model::Text text(u"a\U0001F600b");
        CHECK(Ot::apply(text, {Ot::Op{2, 1, u"c"}}, Unit::CodePoint));
        CHECK(text == std::u16string(u"a\U0001F600c"));
        CHECK(!Ot::apply(text, {Ot::Op{9, 0, u"c"}}, Unit::CodePoint));
    }

    GROUP("ot:rules");
    {
        // Two insertions at one place: the second argument's (the host's)
        // goes first.
        Ot::Ops a2, b2;
        Ot::transform({Ot::Op{1, 0, u"A"}}, {Ot::Op{1, 0, u"B"}}, Unit::Utf16, &a2, &b2);
        std::u16string x = u"xy", y = u"xy";
        Ot::apply(x, {Ot::Op{1, 0, u"A"}}, Unit::Utf16);
        Ot::apply(x, b2, Unit::Utf16);
        Ot::apply(y, {Ot::Op{1, 0, u"B"}}, Unit::Utf16);
        Ot::apply(y, a2, Unit::Utf16);
        CHECK(x == u"xBAy" && y == u"xBAy");
        // Typing inside a range someone deletes survives, at its start.
        Ot::transform({Ot::Op{3, 0, u"T"}}, {Ot::Op{1, 5, u""}}, Unit::Utf16, &a2, &b2);
        std::u16string p = u"0123456789", q = p;
        Ot::apply(p, {Ot::Op{3, 0, u"T"}}, Unit::Utf16);
        Ot::apply(p, b2, Unit::Utf16);
        Ot::apply(q, {Ot::Op{1, 5, u""}}, Unit::Utf16);
        Ot::apply(q, a2, Unit::Utf16);
        CHECK(p == u"0T6789" && q == p);
        // Two deletions overlapping delete their union once.
        Ot::transform({Ot::Op{2, 4, u""}}, {Ot::Op{4, 4, u""}}, Unit::Utf16, &a2, &b2);
        p = u"0123456789", q = p;
        Ot::apply(p, {Ot::Op{2, 4, u""}}, Unit::Utf16);
        Ot::apply(p, b2, Unit::Utf16);
        Ot::apply(q, {Ot::Op{4, 4, u""}}, Unit::Utf16);
        Ot::apply(q, a2, Unit::Utf16);
        CHECK(p == u"0189" && q == p);
        // Offsets follow.
        CHECK(Ot::mapOffset(5, {Ot::Op{1, 2, u"abc"}}, Unit::Utf16) == 6);
        CHECK(Ot::mapOffset(2, {Ot::Op{1, 2, u""}}, Unit::Utf16) == 1);
        CHECK(Ot::mapOffset(1, {Ot::Op{1, 0, u"ab"}}, Unit::Utf16) == 1);
        CHECK(Ot::mapOffset(1, {Ot::Op{1, 0, u"ab"}}, Unit::Utf16, true) == 3);
    }

    GROUP("ot:replacements");
    {
        const std::vector<EditSpans::Span> spans = {{1, 2, 1}, {5, 0, 2}, {5, 1, 0}};
        const std::vector<std::u16string> texts = {u"X", u"YZ", u""};
        const Ot::Ops ops = Ot::fromSpans(spans, texts);
        std::u16string a = u"0123456789";
        Ot::apply(a, ops, Unit::Utf16);
        CHECK(a == u"0X34YZ6789");
        std::vector<Ot::Replacement> back;
        CHECK(Ot::asReplacements(Ot::primitives(ops), Unit::Utf16, &back));
        // The deletion and the insertion at 5 are one replacement.
        CHECK(back.size() == 2 && back[0].start == 1 && back[0].length == 2 && back[0].text == u"X");
        CHECK(back.size() == 2 && back[1].start == 5 && back[1].length == 1 && back[1].text == u"YZ");
        std::u16string b = u"0123456789";
        for (std::size_t i = back.size(); i-- > 0;) b.replace(back[i].start, back[i].length, back[i].text);
        CHECK(b == a);
        // Operations that build on each other have no such shape.
        CHECK(!Ot::asReplacements({Ot::Op{1, 0, u"ab"}, Ot::Op{2, 1, u""}}, Unit::Utf16, nullptr));
    }

    GROUP("ot:fuzz");
    {
        // Two edits on one text, moved past each other, leave one text, in
        // both units, whatever they hold (surrogate pairs included).
        const char* scaleEnv = std::getenv("MC_FUZZ_SCALE");
        const int scale = scaleEnv ? std::max(1, std::atoi(scaleEnv)) : 1;
        std::mt19937 rng(20261117u);
        int bad = 0;
        for (int round = 0; round < 6000 * scale; round++) {
            const Unit u = round % 2 ? Unit::CodePoint : Unit::Utf16;
            const std::u16string base = randomInsert(rng, 8);
            const Ot::Ops a = randomOps(rng, base, u, 5), b = randomOps(rng, base, u, 5);
            Ot::Ops a2, b2;
            Ot::transform(a, b, u, &a2, &b2);
            std::u16string x = base, y = base;
            Ot::apply(x, a, u);
            std::size_t lx = 0, ly = 0;
            const bool fits = Ot::valid(b2, Ot::lengthOf(x, u), u, &lx);
            Ot::apply(x, b2, u);
            Ot::apply(y, b, u);
            const bool fits2 = Ot::valid(a2, Ot::lengthOf(y, u), u, &ly);
            Ot::apply(y, a2, u);
            if (!fits || !fits2 || x != y) {
                if (bad++ < 3)
                    std::printf("  ot:fuzz round %d: \"%s\" then a: \"%s\", then b: \"%s\"\n", round,
                                u8(base).c_str(), u8(x).c_str(), u8(y).c_str());
            }
        }
        CHECK(bad == 0);
    }

    GROUP("host:protocol");
    {
        Message m;
        m.kind = Shared::Kind::DocEdited;
        m.doc = 7;
        m.v = 12;
        m.client = 3;
        m.seq = 4;
        m.flag = 1;
        m.ops = {Ot::Op{1, 2, u"h\U0001F600"}, Ot::Op{0, 0, std::u16string(1, (char16_t)0xD800)}};
        m.ops16 = {Ot::Op{5, 0, u"\n\"\\"}};
        m.hasSelect = true;
        m.select = {{1, 3}, {4, 4}};
        m.undo = {true, false, "Typing", ""};
        Message back;
        std::string err;
        CHECK(Shared::decode(Shared::encode(m), &back, &err));
        CHECK(back.kind == m.kind && back.doc == 7 && back.v == 12 && back.client == 3 && back.seq == 4);
        CHECK(back.flag == 1 && back.ops == m.ops && back.ops16 == m.ops16);
        CHECK(back.hasSelect && back.select == m.select && back.undo == m.undo);
        // A lone surrogate survives the trip (UTF-8 cannot carry it).
        CHECK(back.ops[1].ins.size() == 1 && back.ops[1].ins[0] == 0xD800);
        Message e;
        e.kind = Shared::Kind::DocEdit;
        e.doc = 2;
        e.base = 9;
        e.step.id = 5;
        e.step.name = "Replace All";
        e.step.open = true;
        e.step.before = {{0, 0}};
        e.step.close = true;
        e.step.after = {};
        e.ops = {Ot::Op{0, 1, u""}};
        CHECK(Shared::decode(Shared::encode(e), &back));
        CHECK(back.step.id == 5 && back.step.name == "Replace All" && back.step.open && back.step.close);
        CHECK(back.step.before == e.step.before && back.step.after.empty() && back.base == 9);
        Message h;
        h.kind = Shared::Kind::Hello;
        h.unit = Unit::CodePoint;
        h.name = "gtk";
        CHECK(Shared::decode(Shared::encode(h), &back) && back.unit == Unit::CodePoint && back.name == "gtk");
        Message o;
        o.kind = Shared::Kind::DocOpen;
        o.text8 = "caf\xc3\xa9\n";
        o.json = Json::object({{"a", Json(1)}});
        CHECK(Shared::decode(Shared::encode(o), &back) && back.text8 == o.text8 && back.json == o.json);
        CHECK(!Shared::decode("{\"k\":\"nonsense\"}", &back, &err) && !err.empty());
        CHECK(!Shared::decode("{\"k\":\"doc.edit\",\"ops\":[[1]]}", &back));
        CHECK(!Shared::decode("not json", &back));
    }

    GROUP("host:edit-loop");
    {
        Net net(false, false);
        std::mt19937 rng(1);
        Net::Link& a = net.add(Unit::Utf16, "a");
        Net::Link& b = net.add(Unit::CodePoint, "b");
        TestMirror& ma = net.open(a, 1, u"hello \U0001F600 world");
        net.quiesce(rng);
        TestMirror& mb = net.open(b, 1, u"");
        net.quiesce(rng);
        // Attaching to a document open already gives its text.
        CHECK(mb.text == ma.text);
        // Both type at once, crossing in flight.
        ma.caret = 0;
        mb.caret = mb.length();
        ma.type(u"A");
        mb.type(u"B");
        ma.type(u"C");
        CHECK(ma.text != mb.text);
        net.quiesce(rng);
        CHECK(ma.text == u"AChello \U0001F600 worldB");
        CHECK(mb.text == ma.text);
        CHECK(net.host.text(1) == ma.text);
        CHECK(a.client->unconfirmed() == 0 && b.client->unconfirmed() == 0);
        CHECK(a.client->version(1) == net.host.version(1) && b.client->version(1) == net.host.version(1));
        // Only the one in flight: the next waits for its acknowledgement.
        ma.type(u"x");
        ma.type(u"y");
        CHECK(a.up.size() == 1 && a.client->unconfirmed() == 2);
        net.quiesce(rng);
        CHECK(net.host.text(1) == ma.text && mb.text == ma.text);
        // The log keeps no more than a renderer may still need: B, which
        // only reads, says now and then how far it has read.
        for (int i = 0; i < 100; i++) ma.type(u"k");
        net.quiesce(rng);
        CHECK(mb.text == ma.text);
        CHECK(net.host.logSize(1) <= Shared::Client::kReportEvery + 1);
    }

    GROUP("host:undo-across-renderers");
    {
        Net net(false, false);
        std::mt19937 rng(2);
        Net::Link& a = net.add(Unit::Utf16, "a");
        Net::Link& b = net.add(Unit::CodePoint, "b");
        TestMirror& ma = net.open(a, 1, u"one two");
        net.quiesce(rng);
        TestMirror& mb = net.open(b, 1, u"");
        net.quiesce(rng);
        // Typing joins one step; a caret moved by hand starts another.
        ma.caret = 3;
        ma.type(u"a");
        ma.type(u"b");
        ma.type(u"c");
        net.quiesce(rng);
        CHECK(ma.text == u"oneabc two");
        CHECK(net.host.undoInfo(1).undoName == "Typing");
        mb.caret = mb.length();
        mb.type(u"!");
        net.quiesce(rng);
        CHECK(ma.text == u"oneabc two!" && mb.text == ma.text);
        // The document's undo takes its last step back, whoever asks: A's
        // undo removes B's "!".
        std::uint64_t req = a.client->requestUndo(1, false);
        net.quiesce(rng);
        Shared::Outcome out = Shared::Outcome::Refused;
        CHECK(a.client->outcome(req, &out) && out == Shared::Outcome::Done);
        CHECK(ma.text == u"oneabc two" && mb.text == ma.text && net.host.text(1) == ma.text);
        // And the step before, "abc" whole, with the caret where it was
        // before it, for the one who asked.
        req = b.client->requestUndo(1, false);
        net.quiesce(rng);
        CHECK(b.client->outcome(req, &out) && out == Shared::Outcome::Done);
        CHECK(mb.text == u"one two" && ma.text == mb.text);
        CHECK(mb.caret == 3);
        // Redo from the other one.
        req = a.client->requestUndo(1, true);
        net.quiesce(rng);
        CHECK(a.client->outcome(req, &out) && out == Shared::Outcome::Done);
        CHECK(ma.text == u"oneabc two" && mb.text == ma.text);
        // An undo crossing another renderer's typing in flight: the host
        // takes A's undo first, and B's "Z" is moved past it.
        mb.caret = 0;
        mb.type(u"Z");
        req = a.client->requestUndo(1, false);
        CHECK(a.up.size() == 1 && b.up.size() == 1);
        net.host.handle(a.id, std::move(a.up.front()));
        a.up.pop_front();
        net.host.handle(b.id, std::move(b.up.front()));
        b.up.pop_front();
        net.quiesce(rng);
        CHECK(ma.text == mb.text && net.host.text(1) == ma.text);
        CHECK(ma.text == u"Zone two");
        // Nothing to redo once a new edit came.
        ma.caret = 0;
        ma.type(u"q");
        net.quiesce(rng);
        req = b.client->requestUndo(1, true);
        net.quiesce(rng);
        CHECK(b.client->outcome(req, &out) && out == Shared::Outcome::Nothing);
        // A caret moved between keys: two steps.
        ma.caret = ma.length();
        ma.type(u"1");
        ma.caret = 0;
        ma.client->noteSelection(1, ma.sel());
        ma.type(u"2");
        net.quiesce(rng);
        CHECK(ma.text == u"2qZone two1");
        a.client->requestUndo(1, false);
        net.quiesce(rng);
        CHECK(ma.text == u"qZone two1" && mb.text == ma.text);
    }

    GROUP("host:watch");
    {
        Net net(false, false);
        std::mt19937 rng(3);
        Net::Link& a = net.add(Unit::CodePoint, "a");
        Net::Link& b = net.add(Unit::Utf16, "b");
        TestMirror& ma = net.open(a, 4, u"x\U0001F600y");
        net.quiesce(rng);
        TestMirror& mb = net.open(b, 4, u"");
        net.quiesce(rng);
        // The watch waits for its answer: messages go at once meanwhile.
        std::vector<Ot::Ops> seen;
        net.immediate = true;
        net.quiesce(rng);
        const Model::Text* copy =
            a.client->watch(4, &seen, [&](Shared::DocId, const Ot::Ops& ops16) { seen.push_back(ops16); });
        net.immediate = false;
        CHECK(copy && copy->str() == u"x\U0001F600y");
        mb.caret = 1;
        mb.type(u"é");
        ma.caret = ma.length();
        ma.type(u"z");
        net.quiesce(rng);
        CHECK(copy->str() == net.host.text(4) && ma.text == net.host.text(4));
        CHECK(seen.size() == 2);
        a.client->unwatch(4, &seen);
        net.quiesce(rng);
        CHECK(a.client->hostCopy(4) == nullptr);
    }

    GROUP("host:watch-timeout");
    {
        // A watch the host does not answer in time keeps no watcher: once
        // another owner's watch makes the copy, the stalled one's owner
        // (which may have gone, as an LspSession of a closed window does)
        // is never called. Its late answer is dropped.
        Net net(false, false);
        std::mt19937 rng(5);
        Net::Link& a = net.add(Unit::Utf16, "mac");
        Net::Link& b = net.add(Unit::Utf16, "other");
        net.open(a, 6, u"abc");
        TestMirror& mb = net.open(b, 6, u"");
        net.quiesce(rng);
        int stalledCalls = 0, liveCalls = 0;
        const int stalledOwner = 0, liveOwner = 0;
        const Model::Text* none = a.client->watch(
            6, &stalledOwner, [&](Shared::DocId, const Ot::Ops&) { stalledCalls++; }, 20);
        CHECK(none == nullptr);
        CHECK(!a.client->watching(6, &stalledOwner));
        CHECK(a.client->hostCopy(6) == nullptr);
        // The host takes its messages late: the watch, then the unwatch.
        net.quiesce(rng);
        net.immediate = true;
        const Model::Text* copy =
            a.client->watch(6, &liveOwner, [&](Shared::DocId, const Ot::Ops&) { liveCalls++; });
        net.immediate = false;
        CHECK(copy && copy->str() == u"abc");
        CHECK(a.client->watching(6, &liveOwner) && !a.client->watching(6, &stalledOwner));
        mb.caret = 3;
        mb.type(u"d");
        net.quiesce(rng);
        CHECK(liveCalls == 1 && stalledCalls == 0);
        CHECK(copy->str() == u"abcd" && net.host.text(6) == u"abcd");
        CHECK(a.client->takeErrors().empty());
    }

    GROUP("host:fuzz");
    {
        // Renderers in both units type, edit in named steps, move their
        // carets, undo and redo, reset whole texts, while messages wait and
        // go in random order; once all are delivered, every mirror and every
        // watched copy is the host's text, and nothing is left unconfirmed.
        const char* scaleEnv = std::getenv("MC_FUZZ_SCALE");
        const int scale = scaleEnv ? std::max(1, std::atoi(scaleEnv)) : 1;
        int bad = 0;
        for (int round = 0; round < 60 * scale; round++) {
            std::mt19937 rng(9000u + (unsigned)round);
            Net net(round % 3 == 0, false);
            const int renderers = 2 + (int)(rng() % 2);
            for (int i = 0; i < renderers; i++)
                net.add((i + round) % 2 ? Unit::CodePoint : Unit::Utf16, "r" + std::to_string(i));
            const std::u16string start = randomInsert(rng, 10);
            for (int i = 0; i < renderers; i++) {
                net.open(*net.links[(size_t)i], 1, i == 0 ? start : u"");
                net.quiesce(rng);
            }
            std::vector<std::vector<Ot::Ops>> seen((size_t)renderers);
            // One renderer watches the host's text (the GTK port's language
            // server sync).
            net.immediate = true;
            const Model::Text* copy = net.links[0]->client->watch(1, &seen, [](Shared::DocId, const Ot::Ops&) {});
            net.immediate = false;
            for (int action = 0; action < 250; action++) {
                Net::Link& l = *net.links[rng() % (size_t)renderers];
                TestMirror& m = *l.mirrors[1];
                const unsigned what = rng() % 100;
                if (what < 30) m.type(randomInsert(rng, 1).empty() ? u"k" : randomInsert(rng, 1));
                else if (what < 45) m.edit(rng, what % 2 ? "Edit" : "Typing");
                else if (what < 52) m.moveCaret(rng);
                else if (what < 57) l.client->requestUndo(1, rng() % 3 == 0);
                else if (what < 58) {
                    const std::u16string whole = randomInsert(rng, 6);
                    m.text = whole;
                    m.caret = 0;
                    l.client->reset(1, u8(whole));
                } else {
                    for (int k = (int)(rng() % 4); k >= 0; k--) net.step(rng);
                }
            }
            net.quiesce(rng);
            const std::u16string want = net.host.text(1);
            bool ok = copy && copy->str() == want;
            std::string why = ok ? "" : "the watched copy differs; ";
            for (auto& l : net.links) {
                const std::vector<std::string> errors = l->client->takeErrors();
                if (l->mirrors[1]->text != want) why += l->client->unit() == Unit::Utf16 ? "a UTF-16 mirror differs; " : "a code point mirror differs; ";
                if (l->client->unconfirmed()) why += "edits unconfirmed; ";
                if (l->client->version(1) != net.host.version(1)) why += "a version behind; ";
                if (!errors.empty()) why += "the host said: " + errors[0] + "; ";
            }
            ok = why.empty();
            if (!ok && bad++ < 3)
                std::printf("  host:fuzz round %d (loopback %d, %d renderers): %s\n", round,
                            (int)net.loopback, renderers, why.c_str());
        }
        CHECK(bad == 0);
    }

    GROUP("host:tabs");
    {
        Net net(true, true);
        Net::Link& a = net.add(Unit::CodePoint, "gtk");
        Model::Window w;
        w.setRoot("/proj");
        w.append(11, "/proj/a.c");
        w.add(12, "/proj/b.c", Model::Preview::User);
        w.activate(12);
        a.client->windowTabs(5, Shared::windowJson(w));
        const Json s = a.client->sessionState();
        CHECK(s["windows"].size() == 1);
        const Json& tabs = s["windows"][0]["tabs"];
        CHECK(tabs == Shared::windowJson(w));
        CHECK(tabs["root"].asString() == "/proj" && tabs["groups"][0]["tabs"].size() == 2);
        CHECK(tabs["groups"][0]["active"].asInt() == 12);
        CHECK(tabs["groups"][0]["tabs"][1]["preview"].asInt() == (int)Model::Preview::User);
        a.client->windowClosed(5);
        CHECK(a.client->sessionState()["windows"].size() == 0);
    }

    GROUP("host:waits");
    {
        // In one process the renderer's waits are a round trip: an undo
        // comes back through the listener before undo() returns, and the
        // undo state is the host's.
        Net net(true, true);
        Net::Link& a = net.add(Unit::CodePoint, "gtk");
        TestMirror& m = net.open(a, 3, u"abc");
        m.caret = 3;
        m.type(u"d");
        m.type(u"e");
        Shared::UndoInfo u = a.client->undoInfo(3);
        CHECK(u.canUndo && !u.canRedo && u.undoName == "Typing");
        CHECK(a.client->undo(3, false) == Shared::Outcome::Done);
        CHECK(m.text == u"abc" && m.caret == 3);
        u = a.client->undoInfo(3);
        CHECK(!u.canUndo && u.canRedo && u.redoName == "Typing");
        CHECK(a.client->undo(3, false) == Shared::Outcome::Nothing);
        CHECK(a.client->undo(3, true) == Shared::Outcome::Done && m.text == u"abcde");
        const Shared::Client::HostDoc h = a.client->hostDocument(3);
        CHECK(h.open && h.text == u"abcde" && h.dirty);
        a.client->saved(3);
        CHECK(!a.client->hostDocument(3).dirty);
        // A reset forgets the history.
        m.text = u"new";
        a.client->reset(3, "new");
        CHECK(!a.client->undoInfo(3).canUndo && a.client->hostDocument(3).text == u"new");
        a.client->close(3);
        CHECK(!a.client->hostDocument(3).open);
    }

    GROUP("host:undo-multi-range");
    {
        // "zz" typed at two carets is one step of two pieces, recorded as
        // the Mac's replaceRanges: makes it (last first): one undo takes
        // both back, the plan reaches the mirror as both replacements, and
        // the selection it gives back is both carets.
        Net net(true, true);
        Net::Link& a = net.add(Unit::Utf16, "mac");
        const std::u16string start = u"name = 1\nprint(name)\n";
        TestMirror& m = net.open(a, 4, start);
        int edits = 0;
        Ot::Ops seen;
        Shared::Selection back;
        struct Spy : Shared::Client::Listener {
            TestMirror* m = nullptr;
            int* edits = nullptr;
            Ot::Ops* seen = nullptr;
            Shared::Selection* back = nullptr;
            void hostEdited(Shared::DocId d, const Ot::Ops& ops, int flag, const Shared::Selection* sel) override {
                (*edits)++;
                *seen = ops;
                if (sel) *back = *sel;
                m->hostEdited(d, ops, flag, sel);
            }
        } spy;
        spy.m = &m, spy.edits = &edits, spy.seen = &seen, spy.back = &back;
        a.client->close(4);
        a.client->open(4, "/tmp/two.py", u8(start), &spy);
        const std::size_t last = start.find(u"print(name)") + 6;
        const Shared::Selection carets{{0, 0}, {last, last}};
        a.client->openStep(4, {"Typing", true}, carets);
        a.client->record(4, Ot::Op{last, 0, u"zz"});
        m.text.insert(last, u"zz");
        a.client->record(4, Ot::Op{0, 0, u"zz"});
        m.text.insert(0, u"zz");
        a.client->closeStep(4, {{2, 2}, {last + 4, last + 4}});
        CHECK(a.client->hostDocument(4).text == m.text);
        CHECK(m.text == u"zzname = 1\nprint(zzname)\n");
        CHECK(a.client->undo(4, false) == Shared::Outcome::Done);
        CHECK(m.text == start && a.client->hostDocument(4).text == start);
        CHECK(edits == 1);
        std::vector<Ot::Replacement> reps;
        CHECK(Ot::asReplacements(seen, Unit::Utf16, &reps) && reps.size() == 2);
        CHECK(back == carets);
        // Redo puts both back, as one step too.
        CHECK(a.client->undo(4, true) == Shared::Outcome::Done);
        CHECK(m.text == u"zzname = 1\nprint(zzname)\n" && edits == 2);
        CHECK(a.client->hostDocument(4).text == m.text);
        CHECK(a.client->undo(4, false) == Shared::Outcome::Done && m.text == start);
        CHECK(!a.client->undoInfo(4).canUndo);
    }

    GROUP("host:outside-history");
    {
        // An edit made outside the undo history (the Mac's storage edited
        // outside its text view's change) is the host's too, in no step:
        // the step before is undone where that edit moved it, and a step
        // whose text it touched is gone, so an undo then changes nothing.
        // NSUndoManager's removeAllActions forgets the host's history.
        Net net(true, true);
        Net::Link& a = net.add(Unit::Utf16, "mac");
        const std::u16string start = u"name = 1\nprint(name)\n";
        TestMirror& m = net.open(a, 5, start);
        const std::size_t last = start.find(u"print(name)") + 6;
        auto typeAtTwo = [&] {
            a.client->openStep(5, {"Edit", false}, {{0, 0}});
            a.client->record(5, Ot::Op{last, 0, u"zz"});
            m.text.insert(last, u"zz");
            a.client->record(5, Ot::Op{0, 0, u"zz"});
            m.text.insert(0, u"zz");
            a.client->closeStep(5, {{2, 2}});
        };
        auto outside = [&](Ot::Op o) {
            Ot::apply(m.text, Ot::Ops{o}, Unit::Utf16);
            a.client->recordOutside(5, o);
        };
        typeAtTwo();
        outside(Ot::Op{0, 0, u"# top\n"});
        CHECK(a.client->hostDocument(5).text == m.text);
        CHECK(a.client->undoInfo(5).undoName == "Edit");
        CHECK(a.client->undo(5, false) == Shared::Outcome::Done);
        CHECK(m.text == u"# top\n" + start && a.client->hostDocument(5).text == m.text);
        // Between the pieces.
        outside(Ot::Op{0, 6, u""});
        typeAtTwo();
        outside(Ot::Op{12, 0, u"X"});
        CHECK(a.client->undo(5, false) == Shared::Outcome::Done);
        CHECK(m.text == u"name = 1\npXrint(name)\n" && a.client->hostDocument(5).text == m.text);
        // Redo puts both pieces back where they are now.
        CHECK(a.client->undo(5, true) == Shared::Outcome::Done);
        CHECK(m.text == u"zzname = 1\npXrint(zzname)\n" && a.client->hostDocument(5).text == m.text);
        // A whole replacement takes the step with it: nothing to undo.
        outside(Ot::Op{0, m.text.size(), u"something else"});
        CHECK(!a.client->undoInfo(5).canUndo);
        CHECK(a.client->undo(5, false) == Shared::Outcome::Nothing);
        CHECK(m.text == u"something else" && a.client->hostDocument(5).text == m.text);
        // Outside edits are no step of their own, and forgetting keeps the text.
        outside(Ot::Op{0, m.text.size(), start});
        typeAtTwo();
        CHECK(a.client->undoInfo(5).canUndo);
        a.client->forgetHistory(5);
        CHECK(!a.client->undoInfo(5).canUndo && !a.client->undoInfo(5).canRedo);
        CHECK(a.client->hostDocument(5).text == m.text);
        CHECK(a.client->takeErrors().empty());
        // Through the wire: the flag survives encoding.
        Message e;
        e.kind = Shared::Kind::DocEdit;
        e.doc = 5;
        e.flag = 1;
        e.ops = {Ot::Op{1, 2, u"ab"}};
        const Message back = Shared::roundTrip(e);
        CHECK(back.flag == 1 && back.ops == e.ops);
        Message f;
        f.kind = Shared::Kind::DocForget;
        f.doc = 5;
        CHECK(Shared::roundTrip(f).kind == Shared::Kind::DocForget);
    }

    GROUP("host:threaded");
    {
        // The host on its own thread, two renderers on this one, as the GTK
        // port runs it (and under TSan in CI).
        std::unique_ptr<Shared::Client> a, b;
        Shared::SessionHost host([&](Shared::ClientId to, Message&& m) {
            Shared::Client* c = to == 1 ? a.get() : b.get();
            c->deliver(std::move(m));
        });
        a = std::make_unique<Shared::Client>(Unit::CodePoint, "a");
        b = std::make_unique<Shared::Client>(Unit::Utf16, "b");
        const Shared::ClientId ia = host.connect(), ib = host.connect();
        CHECK(ia == 1 && ib == 2);
        a->connect([&](Message&& m) { host.post(ia, std::move(m)); });
        b->connect([&](Message&& m) { host.post(ib, std::move(m)); });
        TestMirror ma, mb;
        ma.client = a.get(), ma.doc = 8, ma.unit = Unit::CodePoint, ma.text = u"start";
        mb.client = b.get(), mb.doc = 8, mb.unit = Unit::Utf16;
        a->open(8, "/x", "start", &ma);
        CHECK(a->settle());
        b->open(8, "/x", "", &mb);
        CHECK(b->settle());
        CHECK(mb.text == u"start");
        std::mt19937 rng(7);
        for (int i = 0; i < 400; i++) {
            TestMirror& m = i % 2 ? ma : mb;
            if (i % 37 == 0) m.client->requestUndo(8, false);
            else m.edit(rng, "Typing");
            if (i % 5 == 0) {
                a->dispatch();
                b->dispatch();
            }
        }
        CHECK(a->settle() && b->settle());
        CHECK(a->settle() && b->settle());
        std::u16string want;
        host.inspect([&](const Shared::Host& h) { want = h.text(8); });
        a->dispatch();
        b->dispatch();
        CHECK(ma.text == want && mb.text == want);
        CHECK(a->unconfirmed() == 0 && b->unconfirmed() == 0);
    }

    GROUP("host:speed");
    {
        // A keystroke's whole round trip in one process (renderer, host,
        // acknowledgement) costs microseconds, at 100,000 lines too. The
        // renderer's own mirror is left out: it is the port's.
        Net net(false, true);
        Net::Link& a = net.add(Unit::CodePoint, "gtk");
        std::string big;
        for (int i = 0; i < 100000; i++) big += "int x = 0; // line\n";
        a.client->open(9, "/big.c", big, nullptr);
        std::size_t caret = big.size() / 2;
        const double t0 = cpuMs();
        for (int i = 0; i < 5000; i++) {
            a.client->openStep(9, {"Typing", true}, {{caret, caret}});
            a.client->record(9, Ot::Op{caret, 0, u"k"});
            caret++;
            a.client->closeStep(9, {{caret, caret}});
        }
        a.client->dispatch();
        const double ms = cpuMs() - t0;
        std::printf("  host:speed: 5,000 keys at 100,000 lines in %.1f ms CPU (%.1f us a key)\n", ms,
                    ms * 1000 / 5000);
        CHECK(net.host.text(9).size() == big.size() + 5000);
        CHECK(a.client->unconfirmed() == 0);
        CHECK(net.host.undoInfo(9).undoName == "Typing");
        CHECK(net.host.logSize(9) <= 1);
        CHECK_TIME(ms, 1500);
    }
}
