
// Lsp+Completion.mm — Objective-C++. LspSession (Completion): the
// completion list (asked for as you type, parsed and first filtered off
// the main thread, refiltered, accepted with its extra edits as one undo
// step, resolved after the accept), its documentation panel, the keys
// routed from the text view while a list or popup shows, and the
// signature hint.
#import "Lsp+Private.h"
#import "Lsp+CompletionIvars.h"
#import "SnippetSession.h"
#include "AutoIndent.h"
#include "Snippet.h"
#include <ctime>

static BOOL Contains(const std::vector<std::string> &list, const std::string &s) {
    return std::find(list.begin(), list.end(), s) != list.end();
}

// The panel beside the completion list: the item's detail (its type or
// signature), then its documentation. Nil when there is neither.
static NSAttributedString *MCCompletionDoc(const Lsp::CompletionItem &item) {
    NSMutableAttributedString *s = [NSMutableAttributedString new];
    if (!item.detail.empty())
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:NS(item.detail)
                attributes:@{NSFontAttributeName: DocFont(YES, NO, NO, 11.5),
                             NSForegroundColorAttributeName: LHex(0xD4D4D4)}]];
    NSString *doc = [NS(item.documentation) stringByTrimmingCharactersInSet:
                                               [NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (doc.length) {
        if (s.length)
            [s appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n\n"
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:5]}]];
        [s appendAttributedString:MCDocText(doc, item.docIsMarkdown)];
    }
    return s.length ? s : nil;
}

// The first paragraph of some documentation, as one plain line.
static NSString *FirstParagraph(const std::string &doc, bool markdown) {
    NSString *s = markdown ? MCDocText(NS(doc), YES).string : NS(doc);
    s = [s stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    NSRange gap = [s rangeOfString:@"\n\n"];
    if (gap.location != NSNotFound) s = [s substringToIndex:gap.location];
    s = [s stringByReplacingOccurrencesOfString:@"\n" withString:@" "];
    return s.length > 300 ? [[s substringToIndex:300] stringByAppendingString:@"…"] : s;
}

static NSAttributedString *MCSignatureText(const Lsp::SignatureHelp &help) {
    const Lsp::SignatureInfo &sig = help.signatures[(size_t)help.activeSignature];
    NSString *label = NS(sig.label);
    NSMutableAttributedString *s = [[NSMutableAttributedString alloc]
        initWithString:label
            attributes:@{NSFontAttributeName: DocFont(YES, NO, NO, 12),
                         NSForegroundColorAttributeName: LHex(0xD4D4D4)}];
    int ap = help.activeParameter;
    if (ap >= 0 && (size_t)ap < sig.params.size()) {
        std::pair<int, int> r = sig.params[(size_t)ap];
        if (r.first >= 0 && r.second > r.first && (NSUInteger)r.second <= label.length) {
            NSRange range = NSMakeRange((NSUInteger)r.first, (NSUInteger)(r.second - r.first));
            [s addAttributes:@{NSFontAttributeName: DocFont(YES, YES, NO, 12),
                               NSForegroundColorAttributeName: LHex(0x4EA1F7)}
                       range:range];
        }
    }
    if (help.signatures.size() > 1)
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:[NSString stringWithFormat:@"   %d of %zu",
                               help.activeSignature + 1, help.signatures.size()]
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11],
                             NSForegroundColorAttributeName: LHex(0x9CA3AF)}]];
    NSString *doc = nil;
    if (ap >= 0 && (size_t)ap < sig.paramDocs.size() && !sig.paramDocs[(size_t)ap].empty())
        doc = FirstParagraph(sig.paramDocs[(size_t)ap], true);
    else if (!sig.documentation.empty())
        doc = FirstParagraph(sig.documentation, sig.docIsMarkdown);
    if (doc.length)
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:[@"\n" stringByAppendingString:doc]
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11],
                             NSForegroundColorAttributeName: LHex(0x9CA3AF)}]];
    return s;
}


// The undo step of an accepted completion. Its edits go in with undo
// registration off and are put back by this step's own handler, last
// first, so an import the server sends after the word went in (the
// resolve answer arriving later) joins the same step while nothing else
// has changed: one Cmd+Z takes away the word and its import.
@interface MCAcceptUndo : NSObject
@property(nonatomic) NSRange selection;   // before the accept
@property(nonatomic) BOOL undone;
- (void)addStepAfter:(NSArray<NSValue *> *)after olds:(NSArray<NSString *> *)olds;
- (void)undoIn:(CodeTextView *)tv;
@end
@implementation MCAcceptUndo {
    NSMutableArray<NSArray<NSValue *> *> *_after;
    NSMutableArray<NSArray<NSString *> *> *_olds;
}
- (instancetype)init {
    if ((self = [super init])) {
        _after = [NSMutableArray array];
        _olds = [NSMutableArray array];
    }
    return self;
}
- (void)addStepAfter:(NSArray<NSValue *> *)after olds:(NSArray<NSString *> *)olds {
    [_after addObject:after];
    [_olds addObject:olds];
}
// Each step's old text back through replaceRanges:, which registers the
// redo (performed in the opposite order, so the word comes back first).
- (void)undoIn:(CodeTextView *)tv {
    _undone = YES;
    for (NSInteger i = (NSInteger)_after.count - 1; i >= 0; i--)
        [tv replaceRanges:_after[(NSUInteger)i] withStrings:_olds[(NSUInteger)i]
               actionName:@"Completion"];
    if (NSMaxRange(_selection) <= tv.string.length) {
        tv.selectedRange = _selection;
        [tv scrollRangeToVisible:_selection];
    }
}
@end

// A completion accepted before its server's completionItem/resolve
// answered, waiting (kResolveAfterAccept at most) for the additional
// edits that answer may bring.
@interface MCPendingAccept : NSObject
@property(nonatomic) int requestId;
@property(nonatomic, strong) MCLspServer *server;
@property(nonatomic, copy) NSString *uri;
@property(nonatomic, copy) NSString *label;
@property(nonatomic) NSUInteger edits;    // the session's edit count once the word was in
// Everything before it is as it was when the item was accepted: the
// word's start, or the start of an earlier edit made since.
@property(nonatomic) NSUInteger limit;
@property(nonatomic, strong) MCAcceptUndo *undo;
@end
@implementation MCPendingAccept
@end

// A completion list parsed off the main thread, with its first filtering
// for the word as typed when it was asked for.
struct MCParsedCompletion {
    std::vector<Lsp::CompletionItem> items, shown;
    bool incomplete = false;
    std::string prefix;
};

// The most rows the list shows.
static const size_t kMaxShownCompletions = 200;

// Parsing a big list, and freeing one, off the main thread.
static dispatch_queue_t CompletionQueue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("minicode.lsp.completion", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}
static void FreeOffMain(std::vector<Lsp::CompletionItem> &items) {
    if (items.empty()) return;
    auto gone = std::make_shared<std::vector<Lsp::CompletionItem>>(std::move(items));
    items.clear();
    dispatch_async(CompletionQueue(), ^{ (void)gone; });
}

@implementation LspSession (Completion)

// ---- completion
- (NSUInteger)cursor {
    NSRange sel = _tv.selectedRange;
    return NSMaxRange(sel);
}

- (void)triggerCompletion {
    if (![self serverFor:Lsp::Feature::Completion]) {
        NSBeep();
        [self flash:self.statusText.length ? self.statusText : @"No language server for this file"];
        return;
    }
    [self requestCompletion:nil origin:Lsp::CompletionOrigin::Explicit];
}

- (void)requestCompletion:(NSString *)trigger origin:(Lsp::CompletionOrigin)origin {
    MCLspServer *server = [self serverFor:Lsp::Feature::Completion];
    if (!server || !_uri) return;
    [self flushChanges];
    NSString *s = _tv.string;
    NSUInteger cursor = [self cursor];
    NSUInteger anchor = cursor;
    while (anchor > 0 && IsIdentChar([s characterAtIndex:anchor - 1])) anchor--;
    if (!_completing || anchor != _anchor) {
        // A new word: forget the old list. The same word asked again (an
        // incomplete list) keeps it showing until the answer, and keeps how
        // it first came up unless it is now asked for.
        _items.clear();
        _haveItems = NO;
        _origin = origin;
        _navigated = NO;
    } else if (origin == Lsp::CompletionOrigin::Explicit) {
        _origin = origin;
    }
    _anchor = anchor;
    _completing = YES;
    int gen = ++_completionGen;
    __weak LspSession *weakSelf = self;
    NSString *uri = _uri;
    Lsp::Position pos = Lsp::positionForOffset(U16Of(s), cursor);
    _completionServer = server;
    const std::string prefix = Std([s substringWithRange:NSMakeRange(anchor, cursor - anchor)]);
    // The answer is parsed and first filtered off the main thread (5,000
    // items took about 10 ms there), and freed there too.
    server.client.completion(Std(uri), pos,
        [weakSelf, gen, uri, prefix](Json &result, const Json &error) {
            (void)error;
            auto owned = std::make_shared<Json>(std::move(result));
            dispatch_async(CompletionQueue(), ^{
                auto parsed = std::make_shared<MCParsedCompletion>();
                parsed->items = Lsp::parseCompletion(*owned, &parsed->incomplete);
                parsed->prefix = prefix;
                parsed->shown = Lsp::filterCompletions(parsed->items, prefix);
                if (parsed->shown.size() > kMaxShownCompletions)
                    parsed->shown.resize(kMaxShownCompletions);
                dispatch_async(dispatch_get_main_queue(), ^{
                    LspSession *me = weakSelf;
                    if (me) [me completionParsed:parsed gen:gen uri:uri];
                    FreeOffMain(parsed->items);
                    FreeOffMain(parsed->shown);
                });
            });
        }, trigger ? Std(trigger) : "");
}

- (void)completionParsed:(const std::shared_ptr<MCParsedCompletion> &)parsed gen:(int)gen
                     uri:(NSString *)uri {
    if (gen != _completionGen || ![uri isEqualToString:_uri] || !_completing) return;
    FreeOffMain(_items);
    _items = std::move(parsed->items);
    parsed->items.clear();
    _incomplete = parsed->incomplete;
    _haveItems = YES;
    if (_items.empty()) { [self closeCompletion]; return; }
    [self refilterCompletionWith:parsed.get()];
}

- (NSString *)typedPrefix {
    NSUInteger cursor = [self cursor];
    if (cursor < _anchor || cursor > _tv.string.length) return @"";
    return [_tv.string substringWithRange:NSMakeRange(_anchor, cursor - _anchor)];
}

- (void)refilterCompletion { [self refilterCompletionWith:nullptr]; }

// `parsed`, when given, holds the list's first filtering, for the word as
// it was when the list was asked for.
- (void)refilterCompletionWith:(MCParsedCompletion *)parsed {
    if (!_completing) return;
    NSString *s = _tv.string;
    NSUInteger cursor = [self cursor];
    if (_tv.selectedRange.length > 0 || cursor < _anchor || cursor > s.length ||
        _tv.window.firstResponder != _tv) {
        [self closeCompletion];
        return;
    }
    for (NSUInteger i = _anchor; i < cursor; i++)
        if (!IsIdentChar([s characterAtIndex:i])) { [self closeCompletion]; return; }
    if (!_haveItems) return;   // still waiting for the server
    NSString *prefix = [self typedPrefix];
    // The item highlighted before, by label and text (an incomplete list
    // asked for again numbers its items afresh).
    NSInteger prevRow = _popup.visible ? _popup.selectedRow : -1;
    std::string prevLabel, prevText;
    if (prevRow >= 0 && (size_t)prevRow < _shown.size()) {
        prevLabel = _shown[(size_t)prevRow].label;
        prevText = _shown[(size_t)prevRow].textToInsert();
    }
    FreeOffMain(_shown);
    if (parsed && parsed->prefix == Std(prefix)) {
        _shown = std::move(parsed->shown);
        parsed->shown.clear();
    } else {
        _shown = Lsp::filterCompletions(_items, Std(prefix));
    }
    // Nothing to offer, or only exactly what is already typed.
    if (_shown.empty() ||
        (_shown.size() == 1 && _shown[0].textToInsert() == Std(prefix))) {
        [self closeCompletion];
        return;
    }
    MCCompletionPopup *popup = [self completionPopup];
    NSMutableArray *labels = [NSMutableArray array], *details = [NSMutableArray array];
    size_t limit = std::min<size_t>(_shown.size(), kMaxShownCompletions);
    if (_shown.size() > limit) {
        std::vector<Lsp::CompletionItem> rest(std::make_move_iterator(_shown.begin() + (long)limit),
                                              std::make_move_iterator(_shown.end()));
        _shown.resize(limit);
        FreeOffMain(rest);
    }
    // The best match (filterCompletions puts it first), unless the arrows
    // chose an item that is still listed. A stale highlight from a wider
    // list used to stay ("os.pa" kept POSIX_SPAWN_CLOSE from "os.p").
    NSInteger select = 0;
    if (_navigated) {
        NSInteger kept = -1;
        for (size_t i = 0; i < limit && kept < 0; i++)
            if (_shown[i].label == prevLabel && _shown[i].textToInsert() == prevText)
                kept = (NSInteger)i;
        if (kept >= 0) select = kept;
        else _navigated = NO;   // the chosen item went: back to choosing for the user
    }
    for (size_t i = 0; i < limit; i++) {
        [labels addObject:NS(_shown[i].label)];
        [details addObject:NS(_shown[i].detail)];
    }
    NSRect caret = [_tv firstRectForCharacterRange:NSMakeRange(_anchor, 0) actualRange:NULL];
    [popup showLabels:labels details:details select:select atScreenRect:caret
               parent:_tv.window];
}

// The list's windows, made on first use.
- (MCCompletionPopup *)completionPopup {
    if (!_popup) {
        _popup = [MCCompletionPopup new];
        __weak LspSession *weakSelf = self;
        _popup.onAccept = ^(NSInteger row) { [weakSelf acceptCompletion:row]; };
        _popup.onSelectionChanged = ^{ [weakSelf completionSelectionMoved]; };
    }
    return _popup;
}

// Made ahead, a piece per turn of the run loop while nothing else is
// going on, once a server that completes is running: building the list's
// windows and laying out its first rows and documentation took 10 to 15
// ms on the first showing.
- (void)prepareCompletionPopup {
    if (_completing || ![self serverFor:Lsp::Feature::Completion]) return;
    [[self completionPopup] prepareStep];
    if (!_popup.prepared)
        [self performSelector:@selector(prepareCompletionPopup) withObject:nil afterDelay:0.05];
}

// The selection in the list moved (or the list was rebuilt): show the
// item's documentation, asking the server for it when the list came
// without.
- (void)completionSelectionMoved {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(resolveSelected)
                                               object:nil];
    // On the next turn of the run loop: the list shows first, and its
    // documentation's layout is not added to the same pause.
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(showSelectedCompletionDoc)
                                               object:nil];
    [self performSelector:@selector(showSelectedCompletionDoc) withObject:nil afterDelay:0];
}

- (void)showSelectedCompletionDoc {
    if (!_completing || !_popup.visible) return;
    NSInteger row = _popup.selectedRow;
    if (row < 0 || (size_t)row >= _shown.size()) { [_popup showDoc:nil]; return; }
    const Lsp::CompletionItem &item = _shown[(size_t)row];
    BOOL willResolve = !item.resolved && _completionServer &&
                       _completionServer.client.canResolveCompletion();
    NSAttributedString *doc = MCCompletionDoc(item);
    // While the documentation is on its way, what shows stays rather than
    // flickering away and back as the arrows move.
    if (doc || !willResolve) [_popup showDoc:doc];
    // Asked a moment after the selection settles, so holding an arrow key
    // down does not ask for every item it passes.
    if (willResolve)
        [self performSelector:@selector(resolveSelected) withObject:nil afterDelay:0.08];
}

- (void)resolveSelected {
    MCLspServer *server = _completionServer;
    if (!server || !_completing) return;
    NSInteger row = _popup.selectedRow;
    if (row < 0 || (size_t)row >= _shown.size() || _shown[(size_t)row].resolved) return;
    if (_resolveId && _resolveServer) _resolveServer.client.cancel(_resolveId);
    int gen = ++_resolveGen;
    int index = _shown[(size_t)row].index;
    __weak LspSession *weakSelf = self;
    _resolveServer = server;
    _resolveId = server.client.resolveCompletion(_shown[(size_t)row],
        [weakSelf, gen, index](const Json &result, const Json &error) {
            [weakSelf completionResolved:error.isNull() ? result : Json() gen:gen index:index];
        });
}

- (void)completionResolved:(const Json &)result gen:(int)gen index:(int)index {
    if (gen != _resolveGen) return;
    _resolveId = 0;
    _resolveServer = nil;
    if (!_completing) return;
    if (index >= 0 && (size_t)index < _items.size() && _items[(size_t)index].index == index)
        Lsp::applyResolved(_items[(size_t)index], result);
    for (Lsp::CompletionItem &it : _shown)
        if (it.index == index) Lsp::applyResolved(it, result);
    NSInteger row = _popup.selectedRow;
    if (row >= 0 && (size_t)row < _shown.size() && _shown[(size_t)row].index == index)
        [_popup showDoc:MCCompletionDoc(_shown[(size_t)row])];
}

- (void)closeCompletion {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(resolveSelected)
                                               object:nil];
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(showSelectedCompletionDoc)
                                               object:nil];
    if (_resolveId && _resolveServer) _resolveServer.client.cancel(_resolveId);
    _resolveId = 0;
    _resolveServer = nil;
    _completing = NO;
    _haveItems = NO;
    _incomplete = NO;
    _navigated = NO;
    _choiceMode = NO;
    FreeOffMain(_items);
    FreeOffMain(_shown);
    [_popup close];
}

// How long an accepted item's additional edits (an auto-import) may take
// to come from completionItem/resolve, after the word went in.
static const NSTimeInterval kResolveAfterAccept = 1.0;

- (void)acceptCompletion:(NSInteger)row {
    if (_choiceMode) { [self acceptChoice:row]; return; }
    if (row < 0 || (size_t)row >= _shown.size()) { [self closeCompletion]; return; }
    Lsp::CompletionItem item = _shown[(size_t)row];
    MCLspServer *server = _completionServer;
    NSUInteger cursor = [self cursor];
    NSUInteger start = _anchor;
    [self closeCompletion];
    // Answers for the list still on their way must not bring it back.
    _completionGen++;
    // An item whose additional edits come only from resolve
    // (typescript-language-server's auto-imports) goes in now, and the
    // edits follow when the answer comes: nothing waits for the server.
    const BOOL later = server && Lsp::completionWantsResolve(item, server.client.canResolveCompletion());
    std::u16string text = U16Of(_tv.string);
    if (item.hasEdit) {
        // The edit's range was computed on the text as it was when asked;
        // everything before the word is unchanged since, so its start holds.
        size_t editStart = Lsp::offsetForPosition(text, item.editRange.start);
        if (editStart <= cursor) start = editStart;
    }
    // A snippet goes in expanded (variables, mirrors, the line's
    // indentation), and its tab stops follow (SnippetSession).
    Snippet::Expansion expansion;
    std::u16string insert;
    if (item.isSnippet) {
        expansion = Snippet::expand(item.textToInsert(),
                                    [self snippetContextAt:start text:text asIs:item.insertAsIs]);
        insert = expansion.text;
    } else {
        insert = Lsp::toUtf16(item.textToInsert());
    }
    // The word, and the edits that go with it (an import at the top, an
    // #include), as one replacement: one undo step, one change for the
    // highlighter and the server.
    Lsp::CompletionEdits ce = Lsp::completionEdits(text, start, cursor, insert,
                                                   item.additionalEdits);
    NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
    NSMutableArray<NSString *> *strings = [NSMutableArray array];
    for (const Lsp::OffsetEdit &e : ce.edits) {
        [ranges addObject:[NSValue valueWithRange:NSMakeRange(e.start, e.length)]];
        [strings addObject:[NSString stringWithCharacters:(const unichar *)e.text.data()
                                                   length:e.text.size()]];
    }
    MCAcceptUndo *undo = [MCAcceptUndo new];
    undo.selection = _tv.selectedRange;
    if (![self applyAcceptEdits:ranges strings:strings into:undo]) return;
    NSUndoManager *um = _tv.undoManager;
    if (um && um.isUndoRegistrationEnabled && _tv.allowsUndo) {
        [_tv breakUndoCoalescing];
        [um beginUndoGrouping];
        [um registerUndoWithTarget:_tv handler:^(CodeTextView *tv) { [undo undoIn:tv]; }];
        [um setActionName:@"Completion"];
        [um endUndoGrouping];
    }
    const NSUInteger base = ce.caret - insert.size();   // where the inserted text starts
    if (item.isSnippet && expansion.hasStops()) {
        [_tv.snippets startWithExpansion:expansion base:base];
    } else if (item.isSnippet) {
        // Only a final stop ($0, maybe inside the text): the caret goes there.
        [_tv.snippets end];
        const Snippet::Range f = expansion.stops.back().ranges.front();
        if (base + f.end <= _tv.string.length)
            [_tv setSelectedRange:NSMakeRange(base + f.start, f.end - f.start)];
    } else if (ce.caret <= _tv.string.length) {
        [_tv setSelectedRange:NSMakeRange(ce.caret, 0)];
    }
    [_tv scrollRangeToVisible:_tv.selectedRange];
    if (later) [self resolveAfterAccepting:item server:server undo:undo limit:start];
}

// What a snippet's variables and layout are, for one going in at `start`.
- (Snippet::Context)snippetContextAt:(NSUInteger)start text:(const std::u16string &)text
                                asIs:(BOOL)asIs {
    Snippet::Context ctx;
    if (_path) ctx.filePath = Std(_path);
    if (_root) ctx.workspaceFolder = Std(_root);
    NSString *s = _tv.string;
    start = MIN(start, s.length);
    const NSRange line = [s lineRangeForRange:NSMakeRange(start, 0)];
    NSUInteger contentsEnd = 0;
    [s getLineStart:NULL end:NULL contentsEnd:&contentsEnd forRange:line];
    ctx.currentLine = Std([s substringWithRange:NSMakeRange(line.location,
                                                            contentsEnd - line.location)]);
    NSUInteger w0 = start, w1 = start;
    while (w0 > line.location && IsIdentChar([s characterAtIndex:w0 - 1])) w0--;
    while (w1 < contentsEnd && IsIdentChar([s characterAtIndex:w1])) w1++;
    ctx.currentWord = Std([s substringWithRange:NSMakeRange(w0, w1 - w0)]);
    long lineIndex = 0;
    for (size_t i = 0; i < line.location && i < text.size(); i++)
        if (text[i] == u'\n' || (text[i] == u'\r' && (i + 1 >= text.size() || text[i + 1] != u'\n')))
            lineIndex++;
    ctx.lineIndex = lineIndex;
    NSString *clip = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    if (clip) ctx.clipboard = Std(clip);
    Snippet::commentsFor(ctx.filePath, ctx.lineComment, ctx.blockCommentStart,
                         ctx.blockCommentEnd);
    const time_t now = time(nullptr);
    localtime_r(&now, &ctx.time);
    ctx.timezoneOffsetSeconds = ctx.time.tm_gmtoff;
    ctx.unixSeconds = (int64_t)now;
    ctx.random = [] { return arc4random(); };
    // The line's leading blanks (up to where the snippet goes), the
    // file's indentation unit, and its line break.
    if (!asIs) {
        for (size_t i = line.location; i < start && i < text.size(); i++) {
            if (text[i] != u' ' && text[i] != u'\t') break;
            ctx.indent += text[i];
        }
        ctx.unit = AutoIndent::detect(text, ctx.filePath).text();
    }
    const size_t nl = text.find(u'\n');
    if (nl != std::u16string::npos) ctx.eol = nl > 0 && text[nl - 1] == u'\r' ? u"\r\n" : u"\n";
    else if (text.find(u'\r') != std::u16string::npos) ctx.eol = u"\r";
    return ctx;
}

// A choice stop's option, chosen: it replaces the stop's text at every
// caret (its mirrors too), and the session goes on from there.
- (void)acceptChoice:(NSInteger)row {
    NSString *choice = row >= 0 && (size_t)row < _shown.size() ? NS(_shown[(size_t)row].label) : nil;
    [self closeCompletion];
    if (!choice) return;
    if (_tv.multiCaretActive) {
        [_tv.multiCarets insertText:choice];
        return;
    }
    const NSRange sel = _tv.selectedRange;
    if (NSMaxRange(sel) > _tv.string.length) return;
    if ([_tv replaceRanges:@[[NSValue valueWithRange:sel]] withStrings:@[choice]
                actionName:@"Typing"])
        [_tv setSelectedRange:NSMakeRange(sel.location + choice.length, 0)];
}

- (void)showSnippetChoices:(NSArray<NSString *> *)choices range:(NSRange)range {
    [self closeCompletion];
    if (choices.count == 0) return;
    std::vector<Lsp::CompletionItem> items;
    NSMutableArray *details = [NSMutableArray array];
    for (NSString *c in choices) {
        Lsp::CompletionItem it;
        it.label = Std(c);
        it.insertText = it.label;
        it.index = (int)items.size();
        it.resolved = true;
        items.push_back(it);
        [details addObject:@""];
    }
    _shown = std::move(items);
    _choiceMode = YES;
    _choiceRange = range;
    _origin = Lsp::CompletionOrigin::Explicit;
    _navigated = NO;
    NSRect caret = [_tv firstRectForCharacterRange:NSMakeRange(range.location, 0) actualRange:NULL];
    [[self completionPopup] showLabels:choices details:details select:0 atScreenRect:caret
                                parent:_tv.window];
}

// While a list shows, Tab, Shift+Tab and Esc are its own (the choices
// list takes an option with Tab; Esc closes it, or the signature hint),
// before the snippet's.
- (BOOL)snippetKeyTakenFirst:(SEL)sel {
    if (!_popup.visible && !_signature.visible) return NO;
    return [self handleCommand:sel];
}

// Replacements (ascending and apart) made through replaceRanges: with
// undo registration off, their old text kept in `undo` to put back. NO
// when the view refused them.
- (BOOL)applyAcceptEdits:(NSArray<NSValue *> *)ranges strings:(NSArray<NSString *> *)strings
                    into:(MCAcceptUndo *)undo {
    NSString *text = _tv.string;
    NSMutableArray<NSValue *> *after = [NSMutableArray array];
    NSMutableArray<NSString *> *olds = [NSMutableArray array];
    NSInteger shift = 0;
    for (NSUInteger i = 0; i < ranges.count; i++) {
        const NSRange r = ranges[i].rangeValue;
        if (NSMaxRange(r) > text.length) return NO;
        [olds addObject:[text substringWithRange:r]];
        [after addObject:[NSValue valueWithRange:NSMakeRange((NSUInteger)((NSInteger)r.location + shift),
                                                             strings[i].length)]];
        shift += (NSInteger)strings[i].length - (NSInteger)r.length;
    }
    NSUndoManager *um = _tv.undoManager;
    const BOOL registering = um && um.isUndoRegistrationEnabled;
    if (registering) [um disableUndoRegistration];
    const BOOL ok = [_tv replaceRanges:ranges withStrings:strings actionName:nil];
    if (registering) [um enableUndoRegistration];
    if (ok) [undo addStepAfter:after olds:olds];
    return ok;
}

- (void)resolveAfterAccepting:(const Lsp::CompletionItem &)item server:(MCLspServer *)server
                         undo:(MCAcceptUndo *)undo limit:(NSUInteger)limit {
    MCPendingAccept *p = [MCPendingAccept new];
    p.server = server;
    p.uri = _uri;
    p.label = NS(item.label);
    p.edits = _edits;
    p.limit = limit;
    p.undo = undo;
    __weak LspSession *weakSelf = self;
    __weak MCPendingAccept *weakPending = p;
    Lsp::CompletionItem copy = item;
    const int rid = server.client.resolveCompletion(item,
        [weakSelf, weakPending, copy](const Json &result, const Json &error) {
            Lsp::CompletionItem resolved = copy;
            Lsp::applyResolved(resolved, error.isNull() ? result : Json());
            [weakSelf pendingAccept:weakPending resolved:resolved];
        });
    if (!rid) return;
    p.requestId = rid;
    [_pendingAccepts addObject:p];
    [self performSelector:@selector(pendingAcceptTimedOut:) withObject:p
               afterDelay:kResolveAfterAccept];
}

// Every edit since an accept moves its limit back to the edit's start:
// the text before that is still as it was.
- (void)noteEditForPendingAccepts:(NSUInteger)location {
    for (MCPendingAccept *p in _pendingAccepts)
        if (location < p.limit) p.limit = location;
}

- (void)finishPendingAccept:(MCPendingAccept *)p {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(pendingAcceptTimedOut:)
                                               object:p];
    [_pendingAccepts removeObject:p];
}

- (void)dropPendingAccepts {
    for (MCPendingAccept *p in [_pendingAccepts copy]) {
        if (p.requestId && !p.server.stopping) p.server.client.cancel(p.requestId);
        [self finishPendingAccept:p];
    }
}

- (void)pendingAcceptTimedOut:(MCPendingAccept *)p {
    if (![_pendingAccepts containsObject:p]) return;
    if (p.requestId && !p.server.stopping) p.server.client.cancel(p.requestId);
    [self finishPendingAccept:p];
    // The server may have had an import to add with it (its answer would
    // have said): the status bar says so rather than leave it unseen.
    [self note:[NSString stringWithFormat:
        @"%@ went in without what the language server may have added with it (an import): "
         "it did not answer within a second", p.label]];
}

- (void)pendingAccept:(MCPendingAccept *)p resolved:(const Lsp::CompletionItem &)item {
    if (!p || ![_pendingAccepts containsObject:p]) return;
    [self finishPendingAccept:p];
    // Taken back before the answer came: its import is not wanted either.
    if (item.additionalEdits.empty() || p.undo.undone) return;
    if (![_servers containsObject:p.server] || ![_uri isEqualToString:p.uri]) return;
    std::vector<Lsp::OffsetEdit> edits;
    if (!Lsp::lateAdditionalEdits(U16Of(_tv.string), item.additionalEdits, p.limit, edits)) {
        [self note:[NSString stringWithFormat:
            @"The language server's addition for %@ (an import) was left out: the text it "
             "would change was edited before it answered", p.label]];
        return;
    }
    if (edits.empty()) return;
    NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
    NSMutableArray<NSString *> *strings = [NSMutableArray array];
    NSInteger shift = 0;
    for (const Lsp::OffsetEdit &e : edits) {
        [ranges addObject:[NSValue valueWithRange:NSMakeRange(e.start, e.length)]];
        [strings addObject:[NSString stringWithCharacters:(const unichar *)e.text.data()
                                                   length:e.text.size()]];
        shift += (NSInteger)e.text.size() - (NSInteger)e.length;
    }
    // The caret, a list being typed and the view's top line stay on their
    // text (all of it after the edits), as VS Code keeps the caret's line
    // where it was on screen.
    const NSUInteger limit = p.limit;
    auto moved = [&](NSUInteger at) {
        return at >= limit ? (NSUInteger)((NSInteger)at + shift) : at;
    };
    const NSRange sel = _tv.selectedRange;
    NSLayoutManager *lm = _tv.layoutManager;
    const NSRect visible = _tv.enclosingScrollView.contentView.bounds;
    const NSPoint origin = _tv.textContainerOrigin;
    NSUInteger top = NSNotFound;
    CGFloat topOffset = 0;
    if (lm && _tv.textContainer && _tv.string.length) {
        const NSPoint at = NSMakePoint(NSMinX(visible) - origin.x, NSMinY(visible) - origin.y);
        const NSUInteger glyph = [lm glyphIndexForPoint:at inTextContainer:_tv.textContainer];
        top = [lm characterIndexForGlyphAtIndex:glyph];
        const NSRect line = [lm lineFragmentRectForGlyphAtIndex:glyph effectiveRange:NULL];
        topOffset = NSMinY(visible) - (NSMinY(line) + origin.y);
    }
    _applyingEdit = YES;
    BOOL ok;
    if (_edits == p.edits && !p.undo.undone) {
        // Nothing has changed since the word went in: the same undo step.
        ok = [self applyAcceptEdits:ranges strings:strings into:p.undo];
    } else {
        // Typing came between: a step of its own, so each undoes in turn.
        ok = [_tv replaceRanges:ranges withStrings:strings actionName:@"Completion"];
    }
    _applyingEdit = NO;
    if (!ok) return;
    if (_completing && _anchor >= limit) _anchor = moved(_anchor);
    const NSUInteger length = _tv.string.length;
    const NSRange now = NSMakeRange(moved(sel.location), sel.length);
    if (NSMaxRange(now) <= length) _tv.selectedRange = now;
    if (lm && top != NSNotFound && top >= limit && moved(top) < length) {
        const NSUInteger glyph = [lm glyphIndexForCharacterAtIndex:moved(top)];
        const NSRect line = [lm lineFragmentRectForGlyphAtIndex:glyph effectiveRange:NULL];
        [_tv scrollPoint:NSMakePoint(NSMinX(visible), NSMinY(line) + origin.y + topOffset)];
    }
    if (_completing) [self refilterCompletion];
}

// ---- following the text and the caret
// A server came up: the list's window is made ahead of the first use when
// it completes, so the first list shows at once.
- (void)serverMayComplete:(MCLspServer *)server {
    if (!_popup.prepared && server.client.capabilities().has("completionProvider"))
        [self performSelector:@selector(prepareCompletionPopup) withObject:nil afterDelay:0.5];
}

// The document left the view: answers still on their way were for it.
- (void)forgetCompletionRequests {
    _completionGen++;
    _completionServer = nil;
    [self dropPendingAccepts];
}

// An edit typed into the text on show (storageEdited: in Lsp+Sync.mm, once
// the change is on its way to the servers; never a rename or fix going in).
- (void)popupsFollowEdit:(NSTextStorage *)ts {
    NSRange er = ts.editedRange;
    NSString *s = ts.string;
    BOOL typedOne = ts.changeInLength == 1 && er.length == 1;
    unichar c = typedOne ? [s characterAtIndex:er.location] : 0;
    unichar prev = typedOne && er.location > 0 ? [s characterAtIndex:er.location - 1] : 0;
    MCLspServer *signatureServer = [self serverFor:Lsp::Feature::SignatureHelp];
    MCLspServer *completionServer = [self serverFor:Lsp::Feature::Completion];

    // The signature hint: opened by the server's trigger characters ("(" and
    // ","), and brought up to date after any other edit while it shows.
    if (signatureServer && signatureServer.client.hasSignatureHelp()) {
        Lsp::Client &client = signatureServer.client;
        std::string ch = typedOne ? Std([NSString stringWithCharacters:&c length:1]) : "";
        if (typedOne && (Contains(client.signatureTriggers(), ch) ||
                         (_signature.visible && Contains(client.signatureRetriggers(), ch)))) {
            NSString *trigger = NS(ch);
            dispatch_async(dispatch_get_main_queue(), ^{ [self requestSignature:trigger]; });
        } else if (_signature.visible) {
            [self scheduleSignatureUpdate];
        }
    }

    if (!completionServer) return;
    if (typedOne) {
        // The end of ".", "->" or "::"?
        NSString *trigger = nil;
        if (c == '.') trigger = @".";
        else if (c == '>' && prev == '-') trigger = @">";
        else if (c == ':' && prev == ':') trigger = @":";
        if (trigger) {
            std::vector<std::string> triggers = completionServer.client.completionTriggers();
            if (triggers.empty() || Contains(triggers, Std(trigger))) {
                // After the text view has finished the edit and moved the caret.
                dispatch_async(dispatch_get_main_queue(), ^{
                    [self requestCompletion:trigger origin:Lsp::CompletionOrigin::Trigger];
                });
                return;
            }
        }
        // A word being typed: the list opens by itself on the first letter
        // after ".", "->" or "::" (so "json.lo" typed quickly, or after Esc,
        // still lists json's members), and on the second letter of any
        // other word.
        if (!_completing && IsIdentChar(c)) {
            NSUInteger start = er.location;
            while (start > 0 && IsIdentChar([s characterAtIndex:start - 1])) start--;
            NSUInteger length = er.location + 1 - start;
            unichar b1 = start > 0 ? [s characterAtIndex:start - 1] : 0;
            unichar b2 = start > 1 ? [s characterAtIndex:start - 2] : 0;
            BOOL afterAccess = b1 == '.' || (b1 == '>' && b2 == '-') || (b1 == ':' && b2 == ':');
            unichar first = [s characterAtIndex:start];
            BOOL number = first >= '0' && first <= '9';
            if (!number && (afterAccess || length == 2)) {
                // A member after "." counts as after the trigger character
                // (Return takes it); any other word's list leaves Return a
                // new line until the arrows are used.
                Lsp::CompletionOrigin origin = afterAccess ? Lsp::CompletionOrigin::Trigger
                                                           : Lsp::CompletionOrigin::Typing;
                dispatch_async(dispatch_get_main_queue(), ^{
                    [self requestCompletion:nil origin:origin];
                });
                return;
            }
        }
    }
    if (_completing) {
        // A list the server called incomplete is asked for again as the word
        // grows; the old one stays up, filtered, until the new one is in.
        if (_incomplete && _haveItems && typedOne && IsIdentChar(c))
            dispatch_async(dispatch_get_main_queue(), ^{
                [self requestCompletion:nil origin:Lsp::CompletionOrigin::Typing];
            });
        dispatch_async(dispatch_get_main_queue(), ^{ [self refilterCompletion]; });
    }
}

// The selection moved (selectionChanged: in Lsp+Sync.mm).
- (void)popupsFollowSelection {
    // A choice stop's options go once its text is no longer what is selected.
    if (_choiceMode && _popup.visible && !_tv.multiCaretActive &&
        !NSEqualRanges(_tv.selectedRange, _choiceRange))
        dispatch_async(dispatch_get_main_queue(), ^{
            if (self->_choiceMode && !self->_tv.multiCaretActive &&
                !NSEqualRanges(self->_tv.selectedRange, self->_choiceRange))
                [self closeCompletion];
        });
    if (_completing)
        dispatch_async(dispatch_get_main_queue(), ^{ [self refilterCompletion]; });
    if (_signature.visible) {
        if (_tv.window.firstResponder != _tv) [self closeSignature];
        else [self scheduleSignatureUpdate];
    }
}

- (BOOL)handleCommand:(SEL)sel {
    // The references or quick fix list first (Lsp+Actions.mm).
    if ([self pickerTakesCommand:sel]) return YES;
    if (sel == @selector(complete:)) {
        // Without a server's, the text view's own word completion.
        if (![self serverFor:Lsp::Feature::Completion]) return NO;
        [self requestCompletion:nil origin:Lsp::CompletionOrigin::Explicit];
        return YES;
    }
    if (_popup.visible) {
        if (sel == @selector(moveUp:)) { _navigated = YES; [_popup moveSelection:-1]; return YES; }
        if (sel == @selector(moveDown:)) { _navigated = YES; [_popup moveSelection:1]; return YES; }
        BOOL ret = sel == @selector(insertNewline:);
        BOOL tab = sel == @selector(insertTab:);
        BOOL backTab = sel == @selector(insertBacktab:);
        if (ret || tab || backTab) {
            // Tab takes the item; Return does when the list was asked for,
            // came after ".", or was moved through, and is otherwise a new
            // line (Lsp::completionKeyAccepts). A key that does not take it
            // closes the list and goes on to the editor (AutoIndent).
            NSInteger row = _popup.selectedRow;
            if (row < 0 || (size_t)row >= _shown.size()) {
                // Nothing selected: the key is the editor's (Tab indents).
                [self closeCompletion];
                return NO;
            }
            // The word typed out exactly as some item (case and all) is a
            // new line, unless the arrows chose another item: "os.path"
            // used to become os.PathLike.
            std::string typed = _choiceMode ? std::string() : Std([self typedPrefix]);
            BOOL exact = !_choiceMode &&
                         (_shown[(size_t)row].textToInsert() == typed ||
                          (!_navigated && Lsp::exactCompletion(_shown, typed) >= 0));
            Lsp::CompletionKey key = ret ? Lsp::CompletionKey::Return
                                   : tab ? Lsp::CompletionKey::Tab
                                         : Lsp::CompletionKey::ShiftTab;
            if (!Lsp::completionKeyAccepts(_origin, _navigated, key, exact)) {
                [self closeCompletion];
                return NO;
            }
            [self acceptCompletion:row];
            return YES;
        }
        if (sel == @selector(cancelOperation:)) { [self closeCompletion]; return YES; }
        return NO;
    }
    if (_signature.visible && sel == @selector(cancelOperation:)) {
        [self closeSignature];
        return YES;
    }
    return [self hoverTakesCommand:sel];
}

- (BOOL)completionVisible { return _popup.visible; }
- (NSArray<NSString *> *)completionLabels { return _popup.visible ? _popup.labels : @[]; }
- (NSString *)completionSelectedLabel {
    NSInteger row = _popup.visible ? _popup.selectedRow : -1;
    return row >= 0 && (NSUInteger)row < _popup.labels.count ? _popup.labels[(NSUInteger)row] : nil;
}
- (NSString *)completionDocText { return _popup.visible ? _popup.docText : nil; }

// ---- signature help
- (void)scheduleSignatureUpdate {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(updateSignature)
                                               object:nil];
    [self performSelector:@selector(updateSignature) withObject:nil afterDelay:0.15];
}

- (void)updateSignature {
    if (_signature.visible) [self requestSignature:nil];
}

- (void)requestSignature:(NSString *)trigger {
    MCLspServer *server = [self serverFor:Lsp::Feature::SignatureHelp];
    if (!server || !_uri || !server.client.hasSignatureHelp()) return;
    [self flushChanges];
    int gen = ++_signatureGen;
    __weak LspSession *weakSelf = self;
    NSString *uri = _uri;
    NSUInteger cursor = [self cursor];
    Lsp::Position pos = Lsp::positionForOffset(U16Of(_tv.string), cursor);
    server.client.signatureHelp(Std(uri), pos,
        [weakSelf, gen, uri, cursor](const Json &result, const Json &error) {
            (void)error;
            [weakSelf signatureArrived:result gen:gen uri:uri cursor:cursor];
        }, trigger ? Std(trigger) : "", _signature.visible);
}

- (void)signatureArrived:(const Json &)result gen:(int)gen uri:(NSString *)uri
                  cursor:(NSUInteger)cursor {
    if (gen != _signatureGen || ![uri isEqualToString:_uri]) return;
    Lsp::SignatureHelp help = Lsp::parseSignatureHelp(result);
    if (help.signatures.empty() || _tv.window.firstResponder != _tv ||
        cursor > _tv.string.length) {
        [self closeSignature];
        return;
    }
    if (!_signature) _signature = [MCSignaturePopup new];
    NSRect caret = [_tv firstRectForCharacterRange:NSMakeRange(cursor, 0) actualRange:NULL];
    [_signature show:MCSignatureText(help) atScreenRect:caret parent:_tv.window];
}

- (void)closeSignature {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(updateSignature)
                                               object:nil];
    _signatureGen++;
    [_signature close];
}

- (NSString *)signatureText { return _signature.visible ? _signature.text : nil; }

@end
