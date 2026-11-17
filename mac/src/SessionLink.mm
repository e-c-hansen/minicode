// SessionLink.mm — see SessionLink.h.
#import "SessionLink.h"

#import "AppSettings.h"
#import "EditorTab.h"
#import "Lsp.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <os/log.h>
#include <vector>

#include "Ot.h"
#include "SessionHost.h"
#include "SessionProtocol.h"

static os_log_t SessionLog(void) {
    static os_log_t log = os_log_create("com.minicode.editor", "session");
    return log;
}

// ---------------------------------------------------------------- the host

namespace {

// Both live as long as the process: the host's thread waits for messages
// until the app exits, and no destructor runs at exit to join it.
Shared::SessionHost *gHost = nullptr;
Shared::Client *gClient = nullptr;
Shared::ClientId gId = 0;
bool gLoopback = false;
std::atomic<bool> gDispatchQueued{false};

Shared::Message Wire(Shared::Message &&m) { return gLoopback ? Shared::roundTrip(m) : std::move(m); }

void DispatchOnMain() {
    gDispatchQueued = false;
    gClient->dispatch();
    for (const std::string &e : gClient->takeErrors())
        os_log_error(SessionLog(), "the session's host: %{public}s", e.c_str());
}

void Start() {
    const char *mode = std::getenv("MINICODE_HOST");
    gLoopback = mode && std::string(mode) == "loopback";
    const char *delay = std::getenv("MINICODE_HOST_DELAY_US");
    const unsigned delayUs = delay ? (unsigned)std::strtoul(delay, nullptr, 10) : 0;
    gClient = new Shared::Client(Model::Unit::Utf16, "mac");
    // The host's messages arrive on its thread; the main queue dispatches
    // them, once per batch, at the top of a run-loop turn (never inside a
    // text system callback; a wait that dispatches there defers the edits,
    // MCSessionDoc's hostEdited).
    gClient->setWake([] {
        if (!gDispatchQueued.exchange(true)) dispatch_async(dispatch_get_main_queue(), ^{ DispatchOnMain(); });
    });
    gHost = new Shared::SessionHost(
        [](Shared::ClientId, Shared::Message &&m) { gClient->deliver(Wire(std::move(m))); }, delayUs);
    gId = gHost->connect();
    gClient->connect([](Shared::Message &&m) { gHost->post(gId, Wire(std::move(m))); });
}

}  // namespace

BOOL MCSessionHostEnabled(void) {
    static const BOOL forced = [] {
        const char *e = std::getenv("MINICODE_SESSION_HOST");
        return (BOOL)(e && (std::string(e) == "on" || std::string(e) == "1"));
    }();
    return forced || [AppSettings shared].settings.sessionHost();
}

Shared::Client &MCSessionClient(void) {
    if (!gClient) Start();
    return *gClient;
}

BOOL MCSessionLoopback(void) {
    MCSessionClient();
    return gLoopback;
}

// ---------------------------------------------------------------- helpers

static std::u16string U16(NSString *s) {
    std::u16string out(s.length, u'\0');
    if (s.length) [s getCharacters:(unichar *)out.data() range:NSMakeRange(0, s.length)];
    return out;
}

static NSString *NSFromU16(const std::u16string &s) {
    return [NSString stringWithCharacters:(const unichar *)s.data() length:s.size()];
}

static std::string Utf8(NSString *s) {
    return Model::utf8FromUtf16(U16(s ?: @""));
}

static BOOL IsHigh(unichar c) { return c >= 0xD800 && c <= 0xDBFF; }
static BOOL IsLow(unichar c) { return c >= 0xDC00 && c <= 0xDFFF; }

static Shared::Selection SelectionOf(NSArray<NSValue *> *ranges) {
    Shared::Selection out;
    for (NSValue *v in ranges) {
        const NSRange r = v.rangeValue;
        out.push_back({r.location, NSMaxRange(r)});
    }
    return out;
}

// The edit `op`, made on `text`, widened to whole code points: an edit
// that starts or ends inside a surrogate pair takes the pair's other half
// with it (and puts it back), so a renderer that counts code points can
// follow it (SESSION-DESIGN.md, "Stage 4a as built").
static Ot::Op WholePoints(NSString *text, Ot::Op op) {
    const NSUInteger n = text.length;
    if (op.pos > 0 && op.pos < n && IsLow([text characterAtIndex:op.pos]) &&
        IsHigh([text characterAtIndex:op.pos - 1])) {
        op.ins.insert(op.ins.begin(), (char16_t)[text characterAtIndex:op.pos - 1]);
        op.pos--;
        op.del++;
    }
    const NSUInteger end = op.pos + op.del;
    if (end > 0 && end < n && IsHigh([text characterAtIndex:end - 1]) && IsLow([text characterAtIndex:end])) {
        op.ins.push_back((char16_t)[text characterAtIndex:end]);
        op.del++;
    }
    return op;
}

// ---------------------------------------------------------------- a document

@class MCSessionDoc;
@interface MCSessionDoc ()
- (void)hostEdited:(const Ot::Ops &)ops flag:(int)flag select:(const Shared::Selection *)select;
@end

namespace {
struct DocListener : Shared::Client::Listener {
    __weak MCSessionDoc *doc = nil;
    void hostEdited(Shared::DocId, const Ot::Ops &ops, int flag, const Shared::Selection *select) override {
        [doc hostEdited:ops flag:flag select:select];
    }
};
uint64_t gNextDoc = 0;   // ids on the host: the main thread's only
}  // namespace

@implementation MCSessionDoc {
    std::unique_ptr<DocListener> _listener;
    BOOL _open;
    BOOL _stepOpen, _named;
    // The step is replaceRanges:'s own (an explicit undo group of its own
    // under NSUndoManager), how deep its calls are, and whether it ended:
    // a later edit of the turn then starts a new step, as a new group did.
    BOOL _explicit, _sealed;
    NSInteger _namedDepth;
    BOOL _forgetting;     // read afresh this turn: a whole set resets
    BOOL _flushQueued;
    // The host's edits not in the mirror yet, in the mirror's text as it
    // is: applied once the storage is done editing.
    Ot::Ops _deferred;
    Shared::Selection _deferredSelect;
    BOOL _deferredHasSelect;
    __weak CodeTextView *_view;   // the view the step's edits were made in
}

- (instancetype)initWithTab:(MCTab *)tab text:(NSString *)text {
    if ((self = [super init])) {
        _tab = tab;
        _docId = ++gNextDoc;
        _listener = std::make_unique<DocListener>();
        _listener->doc = self;
        // UTF-8 cannot carry a lone surrogate (no text read from a file has
        // one: such a file goes to Quick Look); one typed in becomes U+FFFD
        // in the host's copy, which the GUI checks would catch.
        MCSessionClient().open(_docId, Utf8(tab.currentPath), Utf8(text), _listener.get());
        _open = YES;
    }
    return self;
}

- (void)dealloc { [self close]; }

- (void)close {
    if (!_open) return;
    _open = NO;
    _stepOpen = NO;
    if (gClient && gClient->isOpen(_docId)) gClient->close(_docId);
}

- (NSUInteger)deferredEdits { return _deferred.size(); }

// The view showing the tab's text, or nil when the tab is not in the
// storage (a background tab, a preview, a viewer).
- (CodeTextView *)mirrorView {
    MCTab *t = _tab;
    id owner = t.owner;
    if (!t || ![owner respondsToSelector:@selector(textViewShowingSourceOf:)]) return nil;
    return [owner textViewShowingSourceOf:t];
}

- (Shared::Selection)selectionNow:(CodeTextView *)view {
    if (view) return SelectionOf(view.selectedRanges);
    const NSRange r = _tab.selection;
    return {{r.location, NSMaxRange(r)}};
}

// ---- steps

- (void)openStepWithName:(NSString *)name view:(CodeTextView *)view {
    if (_stepOpen || !_open) return;
    _explicit = _sealed = NO;
    Model::UndoHistory::StepInfo info{"Edit", false};
    if (name) info = {name.UTF8String ?: "Edit", false};
    else if (view.sessionTyping > 0) info = {"Typing", true};
    _view = view;
    MCSessionClient().openStep(_docId, info, [self selectionNow:view]);
    _stepOpen = YES;
    _named = name != nil;
    // The step ends with the run-loop turn, as NSUndoManager's group by
    // event did: everything one key press, menu item or check step did is
    // one step.
    __weak MCSessionDoc *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf closeStep]; });
}

- (void)closeStep {
    if (!_stepOpen) return;
    _stepOpen = NO;
    _explicit = _sealed = NO;
    CodeTextView *view = _view;
    if (view && view != [self mirrorView]) view = nil;
    MCSessionClient().closeStep(_docId, [self selectionNow:view]);
}

- (void)nameStep:(NSString *)name {
    if (!_open || !name.length) return;
    if (!_stepOpen) return;   // asked after a step of the turn: nothing to name
    if (_named) return;
    MCSessionClient().renameStep(_docId, {name.UTF8String ?: "Edit", false});
    _named = YES;
}

// replaceRanges:withStrings:actionName: begins (endNamed when it ends). It
// made an explicit undo group of its own under NSUndoManager, nested in
// whatever group was open: inside a group (the tab's undo manager's, or
// another replaceRanges:'s), or after this turn's typing, it is part of
// that step; otherwise it is a step of its own, which a later edit of the
// turn does not join.
- (void)beginNamed:(NSString *)name view:(CodeTextView *)view {
    if (!_open) return;
    const BOOL nested = _namedDepth > 0 || view.undoManager.groupingLevel > 0;
    _namedDepth++;
    if (_stepOpen && !_sealed && (nested || !_explicit)) {
        [self nameStep:name];
        return;
    }
    [self closeStep];
    [self openStepWithName:name view:view];
    _explicit = !nested;
}

- (void)endNamed {
    if (_namedDepth > 0 && --_namedDepth == 0 && _stepOpen && _explicit) _sealed = YES;
}

- (void)breakCoalescing {
    if (_open) MCSessionClient().breakCoalescing(_docId);
}

- (void)noteSelection:(NSArray<NSValue *> *)ranges {
    if (_open && !_stepOpen && !_applying) MCSessionClient().noteSelection(_docId, SelectionOf(ranges));
}

- (void)saved {
    if (_open) {
        [self closeStep];
        MCSessionClient().saved(_docId);
    }
}

// ---- recording

- (void)record:(Ot::Op)op {
    Shared::Client &c = MCSessionClient();
    if (_deferred.empty()) {
        c.record(_docId, std::move(op));
        return;
    }
    // The host's edits not in the mirror yet came first, as the client
    // sees it: the mirror's edit is moved past them, and they past it.
    Ot::Ops mine, theirs;
    Ot::transform(Ot::Ops{op}, _deferred, Model::Unit::Utf16, &mine, &theirs);
    _deferred = std::move(theirs);
    for (Ot::Op &o : mine) c.record(_docId, std::move(o));
}

- (void)storageWillReplace:(NSRange)range with:(NSString *)string view:(CodeTextView *)view {
    if (!_open || _applying) return;
    if (_sealed) [self closeStep];
    if (!_stepOpen) [self openStepWithName:nil view:view];
    NSString *text = view.textStorage.string;
    [self record:WholePoints(text, Ot::Op{range.location, range.length, U16(string ?: @"")})];
}

- (void)textWillChangeFrom:(NSString *)old to:(NSString *)now {
    if (!_open || _applying) return;
    old = old ?: @"";
    now = now ?: @"";
    if (old == now || [old isEqualToString:now]) return;
    if (_forgetting) {
        [self resetTo:now];
        return;
    }
    // The one range in which they differ.
    const std::u16string x = U16(old), y = U16(now);
    const size_t a = x.size(), b = y.size();
    const size_t pre = (size_t)(std::mismatch(x.begin(), x.begin() + (long)std::min(a, b), y.begin()).first -
                                x.begin());
    size_t suf = 0;
    while (suf < a - pre && suf < b - pre && x[a - 1 - suf] == y[b - 1 - suf]) suf++;
    NSString *ins = [now substringWithRange:NSMakeRange(pre, b - pre - suf)];
    if (_sealed) [self closeStep];
    if (!_stepOpen) [self openStepWithName:nil view:nil];
    [self record:WholePoints(old, Ot::Op{pre, a - pre - suf, U16(ins)})];
}

- (void)resetTo:(NSString *)text {
    _stepOpen = NO;
    _explicit = _sealed = NO;
    _deferred.clear();
    _deferredHasSelect = NO;
    MCSessionClient().reset(_docId, Utf8(text));
}

- (void)forgetHistoryWithText:(NSString *)text {
    if (!_open) return;
    [self resetTo:text ?: @""];
    if (_forgetting) return;
    _forgetting = YES;
    __weak MCSessionDoc *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        MCSessionDoc *d = weakSelf;
        if (d) d->_forgetting = NO;
    });
}

// ---- the host's edits

static BOOL StorageBusy(CodeTextView *view) {
    // Inside an edit of the storage (between edited: and the end of
    // processEditing, its notifications included), nothing else may edit
    // it.
    return view.textStorage.editedMask != 0;
}

- (void)hostEdited:(const Ot::Ops &)ops flag:(int)flag select:(const Shared::Selection *)select {
    if (!_open || (ops.empty() && !select)) return;
    CodeTextView *view = [self mirrorView];
    if (view && (StorageBusy(view) || !_deferred.empty() || _applying)) {
        _deferred.insert(_deferred.end(), ops.begin(), ops.end());
        if (select) {
            _deferredSelect = *select;
            _deferredHasSelect = YES;
        }
        [self scheduleFlush];
        return;
    }
    [self apply:ops flag:flag select:select view:view];
}

- (void)scheduleFlush {
    if (_flushQueued) return;
    _flushQueued = YES;
    __weak MCSessionDoc *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        MCSessionDoc *d = weakSelf;
        if (!d) return;
        d->_flushQueued = NO;
        [d flushDeferred];
    });
}

- (void)flushDeferred {
    if (_deferred.empty() && !_deferredHasSelect) return;
    CodeTextView *view = [self mirrorView];
    if (view && StorageBusy(view)) {
        [self scheduleFlush];
        return;
    }
    Ot::Ops ops = std::move(_deferred);
    _deferred.clear();
    Shared::Selection sel = _deferredSelect;
    const BOOL hasSelect = _deferredHasSelect;
    _deferredHasSelect = NO;
    os_log(SessionLog(), "%lu of the host's edits put in a turn late: the storage was editing",
           (unsigned long)ops.size());
    [self apply:ops flag:0 select:hasSelect ? &sel : nullptr view:view];
}

- (void)apply:(const Ot::Ops &)ops
         flag:(int)flag
       select:(const Shared::Selection *)select
         view:(CodeTextView *)view {
    MCTab *tab = _tab;
    if (!tab) return;
    _applying = YES;
    if (view) {
        const BOOL editable = view.editable;
        view.editable = YES;   // the mirror follows the host even while read-only
        view.sessionUndoing = flag != 0;
        // An undo's plan comes as replacements apart in the text: one
        // multi-range edit, as replaceRanges: makes them. Any other shape
        // goes in an operation at a time.
        std::vector<Ot::Replacement> reps;
        BOOL done = NO;
        if (!ops.empty() && Ot::asReplacements(ops, Model::Unit::Utf16, &reps)) {
            NSMutableArray<NSValue *> *ranges = [NSMutableArray arrayWithCapacity:reps.size()];
            NSMutableArray<NSString *> *strings = [NSMutableArray arrayWithCapacity:reps.size()];
            for (const Ot::Replacement &r : reps) {
                [ranges addObject:[NSValue valueWithRange:NSMakeRange(r.start, r.length)]];
                [strings addObject:NSFromU16(r.text)];
            }
            done = reps.empty() || [view replaceRanges:ranges withStrings:strings actionName:nil];
        } else if (ops.empty()) {
            done = YES;
        }
        if (!done)
            for (const Ot::Op &o : ops)
                if (![view replaceRanges:@[ [NSValue valueWithRange:NSMakeRange(o.pos, o.del)] ]
                             withStrings:@[ NSFromU16(o.ins) ]
                              actionName:nil])
                    os_log_error(SessionLog(), "the host's edit at %lu did not go into the editor",
                                 (unsigned long)o.pos);
        if (select && !select->empty()) {
            const NSUInteger n = view.textStorage.length;
            const Model::UndoHistory::Range &r = (*select)[0];
            const NSUInteger a = MIN(MIN(r.anchor, r.caret), n), b = MIN(MAX(r.anchor, r.caret), n);
            [view setSelectedRange:NSMakeRange(a, b - a)];
            [view scrollRangeToVisible:NSMakeRange(a, b - a)];
        }
        view.sessionUndoing = NO;
        view.editable = editable;
    } else {
        NSMutableString *text = [(tab.currentText ?: @"") mutableCopy];
        for (const Ot::Op &o : ops) {
            if (o.pos + o.del > text.length) break;
            [text replaceCharactersInRange:NSMakeRange(o.pos, o.del) withString:NSFromU16(o.ins)];
        }
        tab.sourceText = text;
        if (select && !select->empty()) {
            const Model::UndoHistory::Range &r = (*select)[0];
            const NSUInteger a = MIN(MIN(r.anchor, r.caret), text.length),
                             b = MIN(MAX(r.anchor, r.caret), text.length);
            tab.selection = NSMakeRange(a, b - a);
        }
        id owner = tab.owner;
        if ([owner respondsToSelector:@selector(tabTextChangedByHost:)]) [owner tabTextChangedByHost:tab];
    }
    _applying = NO;
}

// ---- undo

- (BOOL)canUndo:(BOOL)redo name:(NSString **)name {
    if (!_open) return NO;
    [self closeStep];
    const Shared::UndoInfo u = MCSessionClient().undoInfo(_docId);
    const BOOL can = redo ? u.canRedo : u.canUndo;
    if (name) *name = can ? @((redo ? u.redoName : u.undoName).c_str()) : nil;
    return can;
}

- (BOOL)undo:(BOOL)redo inView:(CodeTextView *)view {
    (void)view;
    if (!_open || _applying || _undoing) return NO;
    [self closeStep];
    [self flushDeferred];
    _undoing = redo ? 2 : 1;
    const Shared::Outcome o = MCSessionClient().undo(_docId, redo);
    _undoing = 0;
    if (o == Shared::Outcome::Refused) {
        os_log_error(SessionLog(),
                     "%{public}s in %{public}@ refused: the text it would change is not what it left there, "
                     "so the history was forgotten",
                     redo ? "redo" : "undo", _tab.currentPath ?: @"a new file");
        NSBeep();
    }
    return o == Shared::Outcome::Done;
}

// ---- the host's text

- (const Model::Text *)hostTextFor:(const void *)owner {
    if (!_open) return nullptr;
    // The step stays open: what it has recorded goes ahead of the settle's
    // ping (the outbox is sent in order), and closing it here, inside a
    // storage notification as completion asks, would split typing.
    Shared::Client &c = MCSessionClient();
    const Model::Text *copy = c.watch(_docId, owner, [](Shared::DocId, const Ot::Ops &) {}, 2000);
    if (!copy || !c.settle(2000)) return nullptr;
    return c.hostCopy(_docId);
}

- (void)releaseHostTextFor:(const void *)owner {
    if (_open && gClient) gClient->unwatch(_docId, owner);
}

@end

// ---------------------------------------------------------------- checks

NSString *MCSessionCheckTabs(NSArray<MCTab *> *tabs) {
    if (!gClient) return @"";
    Shared::Client &c = *gClient;
    NSMutableString *out = [NSMutableString string];
    if (!c.settle()) return @"the session's host did not answer";
    for (MCTab *t in tabs) {
        MCSessionDoc *d = t.session;
        if (!d) continue;
        if (d.deferredEdits) {
            [out appendFormat:@"%@ has %lu of the host's edits not put in; ", t.currentPath.lastPathComponent,
                              (unsigned long)d.deferredEdits];
            continue;
        }
        const Shared::Client::HostDoc h = c.hostDocument(d.docId);
        NSString *mine = t.currentText ?: @"";
        if (!h.open) {
            [out appendFormat:@"the host has no document for %@; ", t.currentPath.lastPathComponent ?: @"a tab"];
            continue;
        }
        if (h.text != U16(mine)) {
            // Where they part, for the log.
            const std::u16string m = U16(mine);
            size_t i = 0;
            while (i < m.size() && i < h.text.size() && m[i] == h.text[i]) i++;
            [out appendFormat:@"the host's text of %@ differs from the tab's (%lu units against %lu, "
                              @"first at %lu); ",
                              t.currentPath.lastPathComponent ?: @"a tab", (unsigned long)h.text.size(),
                              (unsigned long)m.size(), (unsigned long)i];
        }
    }
    for (const std::string &e : c.takeErrors()) [out appendFormat:@"the host said: %s; ", e.c_str()];
    return out;
}
