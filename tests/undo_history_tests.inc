// undo_history_tests.inc — the `undo:*` groups of run_tests.cpp: the core's
// undo history (UndoHistory.h, SESSION-DESIGN.md stage 2), held to the Mac's
// undo rules: named steps, typing joined until something breaks it, a
// multi-range edit as one step, Reload from Disk as a step that keeps the
// ones before it, and an undo that checks its ranges still hold what it put
// there. A fuzz test drives it with random edits, in both units, against a
// plain string and the texts it passed through. Included there, after
// text_model_tests.inc.

#include "UndoHistory.h"

namespace {

using UH = Model::UndoHistory;

UH::Selection caretAt(size_t at) { return UH::Selection{UH::Range{at, at}}; }

// A plain string and the history recording its edits, as a port keeps them.
struct UndoDoc {
    std::u16string text;
    Model::Unit unit;
    UH h;
    UH::Selection sel;
    explicit UndoDoc(std::u16string t, Model::Unit u = Model::Unit::Utf16)
        : text(std::move(t)), unit(u), h(u), sel(caretAt(0)) {}

    size_t units(size_t offset) const {
        return unit == Model::Unit::Utf16 ? offset : EditSpans::unitsAt(text, offset);
    }
    size_t length() const {
        return unit == Model::Unit::Utf16 ? text.size() : Model::countCodePoints(text);
    }
    size_t lenOf(const std::u16string &s) const {
        return unit == Model::Unit::Utf16 ? s.size() : Model::countCodePoints(s);
    }
    std::u16string read(size_t start, size_t len) const {
        const size_t a = units(start), b = units(start + len);
        return text.substr(a, b - a);
    }
    UH::Reader reader() const {
        return [this](size_t s, size_t l) { return read(s, l); };
    }
    // One change in the text as it is, recorded and made.
    UH::Change change(size_t start, size_t len, const std::u16string &with) {
        UH::Change c{start, read(start, len), with};
        const size_t a = units(start), b = units(start + len);
        text.replace(a, b - a, with);
        h.record(c);
        return c;
    }
    // Typing at the caret: a coalescing step of one change.
    void type(const std::u16string &s, const std::string &name = "Typing") {
        const size_t at = sel[0].caret;
        h.open({name, true}, sel);
        change(at, 0, s);
        sel = caretAt(at + lenOf(s));
        h.close(sel);
    }
    void backspace() {
        const size_t at = sel[0].caret;
        if (at == 0) return;
        h.open({"Typing", true}, sel);
        change(at - 1, 1, u"");
        sel = caretAt(at - 1);
        h.close(sel);
    }
    void moveTo(size_t at) {
        sel = caretAt(at);
        h.noteSelection(sel);
    }
    // A batch (ascending, apart, in the text now) applied last first.
    void applyBatch(const std::vector<UH::Change> &batch) {
        for (size_t k = batch.size(); k-- > 0;) {
            const size_t a = units(batch[k].start), b = units(batch[k].start + lenOf(batch[k].removed));
            text.replace(a, b - a, batch[k].inserted);
        }
    }
    UH::Outcome undo(std::string *name = nullptr) { return step(true, name); }
    UH::Outcome redo(std::string *name = nullptr) { return step(false, name); }
    UH::Outcome step(bool undoing, std::string *name) {
        UH::Plan plan;
        const UH::Outcome o = undoing ? h.undo(reader(), &plan) : h.redo(reader(), &plan);
        if (o != UH::Outcome::Done) return o;
        applyBatch(plan.changes);
        sel = plan.select;
        if (name) *name = plan.name;
        return o;
    }
};

}  // namespace

static void testUndoHistory() {
    using O = UH::Outcome;

    GROUP("undo:named-steps");
    {
        UndoDoc d(u"int x;\n");
        CHECK(!d.h.canUndo() && !d.h.canRedo());
        CHECK(d.h.undoName().empty());
        CHECK(d.undo() == O::Nothing && d.redo() == O::Nothing);
        // A formatter's result, then the Mac's sort of keep-sorted blocks.
        CHECK(d.h.add({{3, u" ", u"  "}}, {"Format Document", false}, caretAt(0), caretAt(0)));
        d.applyBatch({{3, u" ", u"  "}});
        CHECK(d.text == u"int  x;\n");
        d.h.open({"Sort keep-sorted Blocks", false}, caretAt(0));
        d.change(0, 3, u"long");
        d.h.close(caretAt(0));
        CHECK(d.h.undoSteps() == 2);
        CHECK(d.h.undoName() == "Sort keep-sorted Blocks");
        std::string name;
        CHECK(d.undo(&name) == O::Done && name == "Sort keep-sorted Blocks");
        CHECK(d.h.undoName() == "Format Document" && d.h.redoName() == "Sort keep-sorted Blocks");
        CHECK(d.undo(&name) == O::Done && name == "Format Document");
        CHECK(d.text == u"int x;\n" && !d.h.canUndo());
        CHECK(d.redo(&name) == O::Done && name == "Format Document" && d.text == u"int  x;\n");
        // rename(): the name the open step ends with.
        d.h.open({"Typing", true}, caretAt(0));
        d.h.rename({"Edit in Preview", false});
        d.change(0, 0, u"//");
        d.h.close(caretAt(2));
        CHECK(d.h.undoName() == "Edit in Preview" && !d.h.canRedo());
        // A step that changed nothing is no step; nested pairs are one.
        d.h.open({"Nothing", false}, caretAt(0));
        d.h.close(caretAt(0));
        CHECK(d.h.undoSteps() == 2);
        d.h.open({"Outer", false}, caretAt(0));
        d.h.open({"Inner", false}, caretAt(0));
        d.change(0, 0, u"a");
        d.h.close(caretAt(1));
        CHECK(d.h.isOpen());
        d.change(1, 0, u"b");
        d.h.close(caretAt(2));
        CHECK(!d.h.isOpen() && d.h.undoSteps() == 3 && d.h.undoName() == "Outer");
        CHECK(d.undo() == O::Done && d.text == u"//int  x;\n");
    }

    GROUP("undo:typing-coalesces");
    {
        // A run typed at the caret, Backspace in it, is one step; the
        // selection before is where the run began.
        UndoDoc d(u"x\n");
        d.moveTo(1);
        d.type(u"h");
        d.type(u"e");
        d.type(u"y");
        d.backspace();
        d.type(u"llo");
        CHECK(d.text == u"xhello\n");
        CHECK(d.h.undoSteps() == 1 && d.h.undoName() == "Typing");
        CHECK(d.undo() == O::Done && d.text == u"x\n");
        CHECK(d.sel == caretAt(1));
        CHECK(d.redo() == O::Done && d.text == u"xhello\n" && d.sel == caretAt(6));
        // The caret moved and back (a click, an arrow): a new step.
        d.moveTo(0);
        d.moveTo(6);
        d.type(u"!");
        CHECK(d.h.undoSteps() == 2);
        // breakUndoCoalescing: a new step.
        d.h.breakCoalescing();
        d.type(u"?");
        CHECK(d.h.undoSteps() == 3);
        // Another name, or a step that does not coalesce, never joins; nor
        // does typing join one.
        d.type(u"#", "Toggle Comment");
        CHECK(d.h.undoSteps() == 4);
        d.h.open({"Typing", false}, d.sel);
        d.change(d.sel[0].caret, 0, u"(");
        d.sel = caretAt(d.sel[0].caret + 1);
        d.h.close(d.sel);
        CHECK(d.h.undoSteps() == 5);
        d.type(u")");
        CHECK(d.h.undoSteps() == 6);
        // An undo breaks it: what is typed after is a step of its own.
        CHECK(d.undo() == O::Done && d.text == u"xhello!?#(\n");
        d.type(u"]");
        CHECK(d.h.undoSteps() == 6 && !d.h.canRedo());
        // Typing elsewhere than the caret the step left (a programmatic
        // edit marked as typing) does not join.
        d.h.open({"Typing", true}, d.sel);
        d.change(0, 0, u"^");
        d.h.close(caretAt(1));
        CHECK(d.h.undoSteps() == 7);
        // Typed and taken back to nothing: no step left.
        UndoDoc e(u"abc");
        e.moveTo(3);
        e.type(u"d");
        e.type(u"e");
        e.backspace();
        e.backspace();
        CHECK(e.text == u"abc" && !e.h.canUndo());
        // Backspace after Backspace is one step too.
        e.moveTo(3);
        e.backspace();
        e.backspace();
        CHECK(e.text == u"a" && e.h.undoSteps() == 1);
        CHECK(e.undo() == O::Done && e.text == u"abc" && e.sel == caretAt(3));
        // Typing over a selection starts a step (the selection was made).
        UndoDoc f(u"one two");
        f.sel = UH::Selection{UH::Range{4, 7}};
        f.h.noteSelection(f.sel);
        f.h.open({"Typing", true}, f.sel);
        f.change(4, 3, u"2");
        f.sel = caretAt(5);
        f.h.close(f.sel);
        f.type(u"!");
        CHECK(f.text == u"one 2!" && f.h.undoSteps() == 1);
        CHECK(f.undo() == O::Done && f.text == u"one two" && f.sel == (UH::Selection{UH::Range{4, 7}}));
    }

    GROUP("undo:multi-range");
    {
        // One edit at many places is one step, whichever way its changes
        // were recorded: last first (a GTK MultiEdit, the Mac's
        // replaceRanges:), first to last with the shifts, in no order, or
        // given whole.
        const std::u16string base = u"a.b.c.d.e.f.g.h";
        for (int shape = 0; shape < 4; shape++) {
            UndoDoc d(base);
            d.sel = UH::Selection{UH::Range{0, 0}, UH::Range{4, 4}, UH::Range{8, 8}};
            const std::vector<UH::Change> batch = {
                {1, u".", u"->"}, {5, u".", u"->"}, {9, u".", u"->"}, {13, u".", u"->"}};
            if (shape == 3) {
                CHECK(d.h.add(batch, {"Replace All", false}, d.sel, d.sel));
                d.applyBatch(batch);
            } else {
                d.h.open({"Replace All", false}, d.sel);
                if (shape == 0) {
                    for (size_t k = batch.size(); k-- > 0;) {
                        d.change(batch[k].start, 1, u"");   // a delete, then an insert, as GTK
                        d.change(batch[k].start, 0, u"->");
                    }
                } else if (shape == 1) {
                    long shift = 0;
                    for (const UH::Change &c : batch) {
                        d.change((size_t)((long)c.start + shift), 1, u"->");
                        shift += 1;
                    }
                } else {
                    d.change(9 + 0, 1, u"->");    // the third
                    d.change(1, 1, u"->");        // the first
                    d.change(15, 1, u"->");       // the fourth, shifted by two
                    d.change(6, 1, u"->");        // the second, shifted by one
                }
                d.h.close(d.sel);
            }
            CHECK(d.text == u"a->b.c->d.e->f.g->h");
            CHECK(d.h.undoSteps() == 1 && d.h.undoName() == "Replace All");
            UH::Plan plan;
            CHECK(d.h.undo(d.reader(), &plan) == O::Done);
            CHECK(plan.changes.size() == 4);
            // Ascending and apart in the text as it is.
            std::vector<EditSpans::Span> spans;
            for (const UH::Change &c : plan.changes) spans.push_back({c.start, c.removed.size(), c.inserted.size()});
            CHECK(EditSpans::valid(spans, d.text.size()));
            CHECK(plan.changes.size() == 4 && plan.changes[1].start == 6 && plan.changes[1].removed == u"->");
            d.applyBatch(plan.changes);
            CHECK(d.text == base);
            CHECK(plan.select.size() == 3);
            CHECK(d.redo() == O::Done && d.text == u"a->b.c->d.e->f.g->h");
        }
        // A Model::Text takes the plan as Text::apply takes edits.
        Model::Text t(u"x = 1\ny = 2\n");
        UH h;
        const std::vector<UH::Change> batch = {{0, u"x", u"alpha"}, {6, u"y", u"beta"}};
        CHECK(h.add(batch, {"Rename Symbol", false}, caretAt(0), caretAt(0)));
        std::vector<Model::Text::Edit> forward;
        for (const UH::Change &c : batch) forward.push_back({c.start, c.removed.size(), c.inserted});
        CHECK(t.apply(forward));
        UH::Plan plan;
        CHECK(h.undo(t, &plan) == O::Done && t.apply(plan.edits()) && t == u"x = 1\ny = 2\n");
        CHECK(h.redo(t, &plan) == O::Done && t.apply(plan.edits()) && t == u"alpha = 1\nbeta = 2\n");
        // A batch out of order is refused, recording nothing.
        CHECK(!h.add({{6, u"y", u"z"}, {0, u"x", u"w"}}, {"Bad", false}, caretAt(0), caretAt(0)));
        CHECK(h.undoSteps() == 1);
    }

    GROUP("undo:reload-step");
    {
        // Reload from Disk is one step, and the steps before it stay: undone
        // in turn, it gives back the text before the reload, then the
        // typing before that.
        UndoDoc d(u"first line\nsecond line\n");
        d.moveTo(5);
        d.type(u" typed");
        CHECK(d.text == u"first typed line\nsecond line\n");
        // The disk's text: only what differs is replaced, as the ports do.
        d.h.open({"Reload from Disk", false}, d.sel);
        d.change(17, 6, u"2nd");
        d.h.close(d.sel);
        CHECK(d.text == u"first typed line\n2nd line\n");
        CHECK(d.h.undoSteps() == 2 && d.h.undoName() == "Reload from Disk");
        std::string name;
        CHECK(d.undo(&name) == O::Done && name == "Reload from Disk");
        CHECK(d.text == u"first typed line\nsecond line\n");
        CHECK(d.undo() == O::Done && d.text == u"first line\nsecond line\n");
        CHECK(d.redo() == O::Done && d.redo() == O::Done && d.text == u"first typed line\n2nd line\n");
        // A reload whose text is the same changes nothing and is no step.
        d.h.open({"Reload from Disk", false}, d.sel);
        d.h.close(d.sel);
        CHECK(d.h.undoSteps() == 2);
    }

    GROUP("undo:checks-ranges");
    {
        // A change the history never heard of: the undo is refused, the
        // text is left as it is, and the history is cleared.
        UndoDoc d(u"alpha beta");
        d.moveTo(10);
        d.type(u" gamma");
        d.text.replace(12, 3, u"MMM");   // under what the step put in
        const std::u16string was = d.text;
        CHECK(d.undo() == O::Refused);
        CHECK(d.text == was && !d.h.canUndo() && !d.h.canRedo());
        // Redo checks what it would take away.
        UndoDoc e(u"one two three");
        e.h.add({{4, u"two", u"2"}}, {"Edit", false}, caretAt(0), caretAt(0));
        e.applyBatch({{4, u"two", u"2"}});
        CHECK(e.undo() == O::Done && e.text == u"one two three");
        e.text.replace(4, 1, u"T");
        CHECK(e.redo() == O::Refused && e.text == u"one Two three" && !e.h.canRedo());
        // A change reported as foreign moves the steps: one away from them
        // keeps them, and their selections move with the text.
        UndoDoc f(u"0123456789");
        f.moveTo(5);
        f.type(u"abc");
        f.h.breakCoalescing();
        f.moveTo(1);
        f.type(u"X");
        CHECK(f.text == u"0X1234abc56789");
        const std::vector<UH::Change> outside = {{0, u"", u">>"}, {14, u"", u"<<"}};
        f.applyBatch(outside);
        f.h.foreign(outside);
        CHECK(f.text == u">>0X1234abc56789<<");
        CHECK(f.undo() == O::Done && f.text == u">>01234abc56789<<" && f.sel == caretAt(3));
        CHECK(f.undo() == O::Done && f.text == u">>0123456789<<" && f.sel == caretAt(7));
        CHECK(f.redo() == O::Done && f.text == u">>01234abc56789<<");
        // A redo step moves too.
        f.applyBatch({{2, u"0", u"zero"}});
        f.h.foreign({{2, u"0", u"zero"}});
        CHECK(f.redo() == O::Done && f.text == u">>zeroX1234abc56789<<");
        // One that touches a step drops it and every older one; newer stay.
        UndoDoc g(u"abcdef");
        g.h.add({{0, u"a", u"A"}}, {"One", false}, caretAt(0), caretAt(0));
        g.applyBatch({{0, u"a", u"A"}});
        g.h.add({{5, u"f", u"F"}}, {"Two", false}, caretAt(0), caretAt(0));
        g.applyBatch({{5, u"f", u"F"}});
        g.h.add({{3, u"d", u"D"}}, {"Three", false}, caretAt(0), caretAt(0));
        g.applyBatch({{3, u"d", u"D"}});
        CHECK(g.text == u"AbcDeF");
        g.applyBatch({{4, u"eF", u"ef"}});
        g.h.foreign({{4, u"eF", u"ef"}});   // over Two's F
        CHECK(g.h.undoSteps() == 1 && g.h.undoName() == "Three");
        CHECK(g.undo() == O::Done && g.text == u"Abcdef" && !g.h.canUndo());
    }

    GROUP("undo:code-points");
    {
        // Counted in code points, as GTK counts: an emoji is one.
        UndoDoc d(u"a\U0001F600b", Model::Unit::CodePoint);
        d.moveTo(2);
        d.type(u"\U0001F680");
        d.type(u"c");
        CHECK(d.text == u"a\U0001F600\U0001F680cb");
        d.backspace();
        d.backspace();
        d.backspace();
        CHECK(d.text == u"ab" && d.h.undoSteps() == 1);
        CHECK(d.undo() == O::Done && d.text == u"a\U0001F600b" && d.sel == caretAt(2));
        d.h.add({{0, u"a\U0001F600", u"\U0001F431"}}, {"Edit", false}, caretAt(0), caretAt(1));
        d.applyBatch({{0, u"a\U0001F600", u"\U0001F431"}});
        UH::Plan plan;
        CHECK(d.h.undo(d.reader(), &plan) == O::Done);
        CHECK(plan.changes.size() == 1 && plan.changes[0].start == 0);
        CHECK(plan.edits().size() == 1 && plan.edits()[0].length == 1);
        Model::Text t(u"\U0001F431b");
        CHECK(t.apply(plan.edits(), Model::Unit::CodePoint) && t == u"a\U0001F600b");
    }

    GROUP("undo:limit");
    {
        UndoDoc d(u"");
        d.h.setLimit(3);
        for (int i = 0; i < 5; i++) {
            d.h.breakCoalescing();
            d.type(u"x");
        }
        CHECK(d.h.undoSteps() == 3);
        while (d.undo() == O::Done) {}
        CHECK(d.text == u"xx");
    }

    GROUP("undo:fuzz");
    {
        // Random edits in both units: typing and Backspace at the caret,
        // the caret moving, batches recorded every way a port makes them,
        // undos and redos. After each undo or redo the text is the one it
        // was when that step began or ended, and the plan is ascending and
        // apart. Then a round trip through changes reported as foreign:
        // undo all and redo all comes back to the same text, never refused.
        const char *scaleEnv = std::getenv("MC_FUZZ_SCALE");
        const int scale = scaleEnv && std::atoi(scaleEnv) > 0 ? std::atoi(scaleEnv) : 1;
        const std::u16string pieces[] = {u"a", u"b", u" ", u"\n", u"\U0001F600", u"é", u"xyz", u""};
        bool ok = true;
        int undos = 0, joined = 0, refused = 0;
        for (int unitPass = 0; unitPass < 2; unitPass++) {
            const Model::Unit unit = unitPass ? Model::Unit::CodePoint : Model::Unit::Utf16;
            std::mt19937 rng(20261116u + (unsigned)unitPass);
            auto rnd = [&](size_t n) { return n ? (size_t)(rng() % n) : (size_t)0; };
            auto randomText = [&](size_t n) {
                std::u16string s;
                for (size_t i = 0; i < n; i++) s += pieces[rnd(7)];
                return s;
            };
            for (int round = 0; round < 250 * scale && ok; round++) {
                UndoDoc d(randomText(rnd(12)), unit);
                std::vector<std::u16string> before, after;   // texts at undo and redo tops
                for (int stepNo = 0; stepNo < 50 && ok; stepNo++) {
                    const size_t n = d.length();
                    if (d.sel.empty() || d.sel[0].caret > n) d.sel = caretAt(rnd(n + 1));
                    const size_t c0 = d.h.undoSteps(), r0 = d.h.redoSteps();
                    const std::u16string textBefore = d.text;
                    const int what = (int)rnd(20);
                    bool recorded = true;
                    if (what < 7) {
                        d.type(pieces[rnd(7)]);
                    } else if (what < 9) {
                        d.backspace();
                    } else if (what < 10) {
                        d.moveTo(rnd(n + 1));
                        recorded = false;
                    } else if (what < 11) {
                        d.h.breakCoalescing();
                        recorded = false;
                    } else if (what < 15) {
                        // A batch of a few replacements, recorded one of four ways.
                        std::vector<UH::Change> batch;
                        size_t at = 0;
                        const size_t count = 1 + rnd(4);
                        for (size_t i = 0; i < count && at <= n; i++) {
                            const size_t s = at + rnd(std::min<size_t>(n - at, 6) + 1);
                            const size_t l = rnd(std::min<size_t>(n - s, 4) + 1);
                            batch.push_back({s, d.read(s, l), randomText(rnd(3))});
                            at = s + l + rnd(2);
                        }
                        const int how = (int)rnd(4);
                        const UH::Selection selBefore = d.sel;
                        std::vector<EditSpans::Span> spans;
                        for (const UH::Change &c : batch) spans.push_back({c.start, d.lenOf(c.removed), d.lenOf(c.inserted)});
                        UH::Selection selAfter = caretAt(EditSpans::mapThrough(d.sel[0].caret, spans));
                        if (how == 0) {
                            CHECK(d.h.add(batch, {"Batch", false}, selBefore, selAfter));
                            d.applyBatch(batch);
                        } else {
                            d.h.open({"Batch", false}, selBefore);
                            if (how == 1) {
                                for (size_t k = batch.size(); k-- > 0;) d.change(batch[k].start, d.lenOf(batch[k].removed), batch[k].inserted);
                            } else if (how == 2) {
                                long shift = 0;
                                for (const UH::Change &c : batch) {
                                    d.change((size_t)((long)c.start + shift), d.lenOf(c.removed), c.inserted);
                                    shift += (long)d.lenOf(c.inserted) - (long)d.lenOf(c.removed);
                                }
                            } else {
                                // Any edits at all, one after another.
                                const size_t edits = 1 + rnd(5);
                                for (size_t i = 0; i < edits; i++) {
                                    const size_t m = d.length();
                                    const size_t s = rnd(m + 1);
                                    d.change(s, rnd(std::min<size_t>(m - s, 5) + 1), randomText(rnd(3)));
                                }
                                selAfter = caretAt(std::min(d.sel[0].caret, d.length()));
                            }
                            d.h.close(selAfter);
                        }
                        d.sel = selAfter;
                    } else if (what < 18) {
                        UH::Plan plan;
                        const UH::Outcome o = d.h.undo(d.reader(), &plan);
                        recorded = false;
                        if (before.empty()) {
                            ok = o == UH::Outcome::Nothing;
                        } else {
                            ok = o == UH::Outcome::Done;
                            std::vector<EditSpans::Span> spans;
                            for (const UH::Change &c : plan.changes) spans.push_back({c.start, d.lenOf(c.removed), d.lenOf(c.inserted)});
                            ok = ok && EditSpans::valid(spans, d.length());
                            if (ok) {
                                d.applyBatch(plan.changes);
                                ok = d.text == before.back();
                                after.push_back(textBefore);
                                before.pop_back();
                                d.sel = plan.select;
                                undos++;
                            }
                        }
                    } else {
                        UH::Plan plan;
                        const UH::Outcome o = d.h.redo(d.reader(), &plan);
                        recorded = false;
                        if (after.empty()) {
                            ok = o == UH::Outcome::Nothing;
                        } else {
                            ok = o == UH::Outcome::Done;
                            if (ok) {
                                d.applyBatch(plan.changes);
                                ok = d.text == after.back();
                                before.push_back(textBefore);
                                after.pop_back();
                                d.sel = plan.select;
                            }
                        }
                    }
                    if (!ok) break;
                    if (recorded) {
                        const size_t c1 = d.h.undoSteps();
                        if (c1 == c0 + 1) {
                            before.push_back(textBefore);
                            after.clear();
                        } else if (c1 == c0) {
                            if (d.text != textBefore) {
                                joined++;
                                after.clear();
                            } else {
                                ok = d.h.redoSteps() == r0;   // nothing changed, nothing dropped
                            }
                        } else if (c1 + 1 == c0) {
                            ok = !before.empty() && d.text == before.back();
                            before.pop_back();
                            after.clear();
                        } else {
                            ok = false;
                        }
                        ok = ok && d.h.redoSteps() == after.size();
                    }
                    ok = ok && d.h.undoSteps() == before.size() && d.h.redoSteps() == after.size();
                }
                if (!ok) break;
                // Foreign changes between undos and redos, then the round trip.
                for (int k = 0; k < 3 && ok; k++) {
                    std::vector<UH::Change> outside;
                    const size_t n = d.length();
                    size_t at = 0;
                    for (size_t i = 0; i < 1 + rnd(2) && at <= n; i++) {
                        const size_t s = at + rnd(std::min<size_t>(n - at, 8) + 1);
                        const size_t l = rnd(std::min<size_t>(n - s, 2) + 1);
                        outside.push_back({s, d.read(s, l), randomText(rnd(2))});
                        at = s + l + 1;
                    }
                    d.applyBatch(outside);
                    d.h.foreign(outside);
                    if (rnd(2)) {
                        const UH::Outcome o = d.undo();
                        ok = o != UH::Outcome::Refused;
                    }
                }
                UH::Outcome o;
                while ((o = d.redo()) == UH::Outcome::Done) {}
                if (o == UH::Outcome::Refused) refused++;
                const std::u16string now = d.text;
                while ((o = d.undo()) == UH::Outcome::Done) {}
                if (o == UH::Outcome::Refused) refused++;
                while ((o = d.redo()) == UH::Outcome::Done) {}
                if (o == UH::Outcome::Refused) refused++;
                ok = ok && d.text == now;
            }
        }
        CHECK(ok);
        CHECK(refused == 0);
        CHECK(undos > 1000 && joined > 500);
    }

    GROUP("undo:speed");
    {
        // Replace All of 20,000 matches in a 100,000-line text, recorded as
        // GTK's MultiEdit makes it (a delete and an insert per match, last
        // first), checked and planned for its undo: well within a key's time.
        std::u16string big;
        for (int i = 0; i < 100000; i++) big += i % 5 == 0 ? u"value = old_name(x)\n" : u"pass\n";
        Model::Text text(big);
        UH h(Model::Unit::Utf16);
        const std::u16string from = u"old_name", to = u"new_name_longer";
        std::vector<size_t> at;
        for (size_t p = big.find(from); p != std::u16string::npos; p = big.find(from, p + 1)) at.push_back(p);
        CHECK(at.size() == 20000);
        const double t0 = cpuMs();
        h.open({"Replace All", false}, caretAt(0));
        for (size_t k = at.size(); k-- > 0;) {
            h.record({at[k], from, u""});
            h.record({at[k], u"", to});
        }
        h.close(caretAt(0));
        double ms = cpuMs() - t0;
        std::vector<Model::Text::Edit> forward;
        for (size_t p : at) forward.push_back({p, from.size(), to});
        CHECK(text.apply(forward));
        const double t1 = cpuMs();
        UH::Plan plan;
        CHECK(h.undo(text, &plan) == UH::Outcome::Done && plan.changes.size() == 20000);
        ms += cpuMs() - t1;
        CHECK(text.apply(plan.edits()) && text == big);
        std::printf("  undo: Replace All of 20,000 recorded, checked and planned in %.1f ms of CPU\n", ms);
        CHECK_TIME(ms, 800);
    }
}
