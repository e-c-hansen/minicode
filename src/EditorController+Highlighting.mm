// EditorController+Highlighting.mm — EditorController: the source's syntax colors, kept up as it is edited,
// a language server's semantic tokens laid over them, and bracket-pair colors.
#import "EditorController+Private.h"
#import "EditorController+HighlightingIvars.h"

// attributesForStyle:'s indexes: the lexer's styles by TokenStyle, then
// plain text, the semantic colors (SemanticColor order), and the bracket
// colors (BracketPairs::colorIndex: depth 1, 2, 3 in turn, unexpected).
static const int kPlainStyle = kTokenStyleCount;
static const int kSemanticStyle = kPlainStyle + 1;
static const int kBracketStyle = kSemanticStyle + kSemanticColorCount;
static const int kStyleCount = kBracketStyle + 4;

// attributesForStyle:'s index for a semantic token's style: a function
// takes the lexer's function color; -1 for none.
static int SemanticStyleIndex(Lsp::SemanticStyle s) {
    switch (s) {
    case Lsp::SemanticStyle::Function: return (int)TokenStyle::Function;
    case Lsp::SemanticStyle::Class: return kSemanticStyle + (int)SemanticColor::Class;
    case Lsp::SemanticStyle::Variable: return kSemanticStyle + (int)SemanticColor::Variable;
    case Lsp::SemanticStyle::Parameter: return kSemanticStyle + (int)SemanticColor::Parameter;
    case Lsp::SemanticStyle::Property: return kSemanticStyle + (int)SemanticColor::Property;
    case Lsp::SemanticStyle::Constant: return kSemanticStyle + (int)SemanticColor::Constant;
    case Lsp::SemanticStyle::None: break;
    }
    return -1;
}

// A color laid over the lexer's: a semantic token's or a bracket's,
// [start, end) in the storage, by attributesForStyle: index. Never over a
// string or a comment.
struct MCOverlay {
    NSUInteger start, end;
    int style;
};

@implementation EditorController (Highlighting)

// Show the raw text as an editable, monospaced, syntax-highlighted document.
- (void)displaySourceEditable {
    NSFont *mono = MCEditorFont();   // editor.font-size and the zoom
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineSpacing = 2.0;
    NSDictionary *base = @{
        NSFontAttributeName: mono,
        NSForegroundColorAttributeName: [[AppSettings shared] text:Surface::Editor],
        NSParagraphStyleAttributeName: ps,
    };
    NSString *content = self.sourceText ?: @"";
    NSMutableAttributedString *attr =
        [[NSMutableAttributedString alloc] initWithString:content attributes:base];
    _liveHighlight = NO;                        // applyHighlighting starts it again
    _sourceAttributes = base;
    // Becoming editable shows the line numbers, which narrows the text view,
    // and a narrower view lays out all of its text at once to learn its
    // height: 3 s for 25 MB, after which every keystroke walked every line
    // fragment (45 ms a key). For a long text the gutter comes first, over
    // an empty storage.
    if (!self.textView.editable && content.length > (1u << 20)) {
        [self.textView.textStorage setAttributedString:[NSAttributedString new]];
        self.textView.editable = YES;
    }
    if (_layoutHeldTextBlocks) [self forgetTextBlockLayout];
    [self.textView.textStorage setAttributedString:attr];
    self.textView.typingAttributes = base;      // typed text stays monospaced
    self.textView.editable = YES;
    [self applyWordWrap];   // a preview always wraps; the source as the setting says
    // Regions by the file's name; an untitled buffer by its extension. None
    // in large-file mode (EditorController+LargeFile.mm).
    self.textView.folds.fileName = [self largeFileFeaturesOff] ? nil : self.currentPath ?:
        [@"untitled." stringByAppendingString:self.currentExt ?: @"txt"];
    [self applyHighlighting];
    [self.textView scrollToBeginningOfDocument:nil];
}

// A page with text blocks laid out in the editor (a Markdown preview's
// quotes, code boxes, h1 and h2 rules, tables) leaves the layout manager a
// table of block runs that outlives the text. While it exists, TextKit
// works out the container's used rect after every edit by visiting every
// laid-out line fragment, where without it the first and last fragment
// do (_recalculateUsageForTextContainerAtIndex:, from the edit's
// _enableTextViewResizing): 1.5 ms a keystroke with 50,000 lines laid out
// against 0.3 ms, and tens of ms in a 25 MB file. Nothing public empties
// the table; taking the layout manager off the storage and putting it
// back does, keeping the same object, its delegate (the folds) and its
// settings. Done over an empty storage, before the source goes in.
- (void)layoutHeldTextBlocks {
    _layoutHeldTextBlocks = YES;
}

- (void)forgetTextBlockLayout {
    _layoutHeldTextBlocks = NO;
    NSTextStorage *storage = self.textView.textStorage;
    NSLayoutManager *layout = self.textView.layoutManager;   // held while off the storage
    if (!storage || !layout) return;
    if (storage.length) [storage setAttributedString:[NSAttributedString new]];
    [storage removeLayoutManager:layout];
    [storage addLayoutManager:layout];
}

// The editor stops being editable (another tab, a viewer, a preview, a
// file read afresh): the line numbers hide, which widens the text view,
// and a wider view lays out all of its text to learn its height (1.3 to
// 5 s for 25 MB, on the main thread). A long text goes first, as on the
// way in (displaySourceEditable); whatever shows next puts its own text
// in, and the tab keeps its own copy (stashActiveTab, sourceText). Every
// way on from here tells the language server what shows next
// (documentOpened:) in the same turn, which drops the emptying before it
// is sent as an edit (detachDocument), as for any text replaced on show.
- (void)makeEditorReadOnly {
    NSTextView *tv = self.textView;
    if (tv.editable && tv.textStorage.length > (1u << 20)) {
        _liveHighlight = NO;
        [tv.textStorage setAttributedString:[NSAttributedString new]];
    }
    tv.editable = NO;
}

// Recolor the whole text storage in place (preserves cursor/selection), and
// start highlighting edits as they happen. Runs when a file is shown as source
// and when the settings change colors; typing goes through
// textStorage:willProcessEditing:, which re-lexes only the lines it changes.
- (void)applyHighlighting {
    std::string cext = self.currentExt.UTF8String ? self.currentExt.UTF8String : "";
    NSTextStorage *storage = self.textView.textStorage;
    AppSettings *cfg = [AppSettings shared];
    _plainColor = [cfg text:Surface::Editor];
    for (int i = 0; i < kTokenStyleCount; ++i) _styleColors[i] = ColorForStyle((TokenStyle)i);
    for (int i = 0; i < kSemanticColorCount; ++i)
        _semanticColors[i] = MCColor(cfg.settings.semanticColor((SemanticColor)i));
    for (int i = 0; i < 4; ++i) _bracketColors[i] = MCColor(cfg.settings.bracketColor(i));
    _highlightingSettingsFile = [self isSettingsFile];
    // Semantic colors turned off, or a file in large-file mode: none.
    if (!cfg.settings.semanticHighlighting() || [self largeFileFeaturesOff]) {
        _semanticCurrent = NO;
        _semantic.clear();
    }
    if (_sourceAttributes) {
        NSMutableDictionary *base = [_sourceAttributes mutableCopy];
        base[NSForegroundColorAttributeName] = _plainColor;
        _sourceAttributes = base;
    }

    std::vector<Token> tokens;
    // Large-file mode: plain text, though edits are still followed (the
    // lines an edit touches are painted plain, which costs nothing).
    if (SyntaxHighlighter::supports(cext) && ![self largeFileFeaturesOff]) {
        _hl.reset(new IncrementalHighlighter<char16_t>(cext));
        _hl->reset(NSStringSource(storage.string), &tokens);
    } else {
        _hl.reset();
    }
    // Bracket-pair colors need the highlighter's tokens (none in large-file
    // mode, which has no highlighter); a settings file keeps its swatches.
    _bracketsOn = _hl && cfg.settings.bracketPairColorization() && !_highlightingSettingsFile;
    _bracketDepths.assign({});
    if (_bracketsOn)
        _bracketDepths.assign(BracketPairs::summarizeLines(NSStringSource(storage.string), *_hl,
                                                           0, _hl->lineCount(), tokens));
    _bracketPainted = [NSMutableIndexSet indexSet];
    [storage beginEditing];
    [self restyleRange:NSMakeRange(0, storage.length) tokens:tokens inStorage:storage];
    [storage endEditing];
    if (_bracketsOn) {
        [_bracketPainted addIndexesInRange:NSMakeRange(0, storage.length)];
        [self observeEditorScrolling];
    }
    _pending.clear();   // all of it is fresh now
    _liveHighlight = YES;
    // New colors (a theme, the zoom) over the same text: the server's
    // tokens still hold, painted again where they show.
    if (_semanticCurrent) {
        _semanticPainted = [NSMutableIndexSet indexSet];
        [self paintVisibleLines];
    }
}

// Whether the bracket depths describe the storage as it is (they follow
// every edit the highlighter follows).
- (BOOL)bracketDepthsCurrent {
    return _bracketsOn && _hl && _hl->length() == self.textView.textStorage.length &&
           _bracketDepths.lineCount() == _hl->lineCount();
}

// The bracket colors of the whole lines of `range`, from their tokens, as
// overlays (ascending).
- (void)bracketOverlaysIn:(NSRange)range tokens:(const std::vector<Token> &)tokens
                     into:(std::vector<MCOverlay> &)out {
    if (![self bracketDepthsCurrent] || !range.length) return;
    const size_t first = _hl->lineOf(range.location);
    const size_t end = _hl->lineOf(NSMaxRange(range) - 1) + 1;
    std::vector<BracketPairs::Bracket> brackets;
    BracketPairs::colorLines(NSStringSource(self.textView.textStorage.string), *_hl, first, end,
                             tokens, _bracketDepths.depthAt(first), &brackets);
    out.reserve(out.size() + brackets.size());
    for (const BracketPairs::Bracket &b : brackets)
        out.push_back({b.at, b.at + 1, kBracketStyle + BracketPairs::colorIndex(b.depth)});
}

// Watch the editor scroll, to paint the lines coming into view: with the
// server's tokens, and with bracket colors after an edit changed depths.
- (void)observeEditorScrolling {
    if (_semanticObserving) return;
    NSClipView *clip = self.textView.enclosingScrollView.contentView;
    if (!clip) return;
    clip.postsBoundsChangedNotifications = YES;
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(semanticViewScrolled:)
                                                 name:NSViewBoundsDidChangeNotification
                                               object:clip];
    _semanticObserving = YES;
}

// Paint `range` (whole lines) from scratch: the plain source attributes, then
// the tokens, then any color swatches. Adjacent tokens of one style are
// painted together (a comment spanning lines comes as one piece per line).
//
// The reset is one setAttributes: call, not addAttribute: of the plain color.
// Over a range holding many attribute runs, addAttribute: is dramatically
// slower (typing "/*" at the top of a 50,000-line file took 3.4 s that way,
// against milliseconds here).
- (void)restyleRange:(NSRange)range tokens:(const std::vector<Token> &)tokens
           inStorage:(NSTextStorage *)storage {
    if (range.length == 0) return;
    std::vector<MCOverlay> brackets;
    if (_bracketsOn) [self bracketOverlaysIn:range tokens:tokens into:brackets];
    // Without tokens (plain text, a line too long to lex) only what differs
    // is set, however long the range: setting a 5 MB line's attributes
    // again on each keystroke made TextKit lay it out again.
    if (_sourceAttributes && !_highlightingSettingsFile &&
        (range.length <= 65536 || tokens.empty())) {
        [self restyleChangedIn:range tokens:tokens overlays:brackets inStorage:storage];
        return;
    }
    if (_sourceAttributes) {
        [storage setAttributes:_sourceAttributes range:range];
    } else {
        [storage addAttribute:NSForegroundColorAttributeName value:_plainColor range:range];
        [storage removeAttribute:NSBackgroundColorAttributeName range:range];
        [storage removeAttribute:NSLinkAttributeName range:range];
    }
    // Tokens and bracket colors in order of place, so each attribute run
    // splits off the end of what is done: a second pass over the brackets
    // split runs in the middle, each moving all the runs after it (40 s to
    // open a 100,000-line file).
    const NSUInteger limit = NSMaxRange(range);
    size_t b = 0;
    auto bracketsBefore = [&](NSUInteger at) {
        for (; b < brackets.size() && brackets[b].start < at; ++b)
            if (brackets[b].start >= range.location && brackets[b].end <= limit)
                [storage addAttribute:NSForegroundColorAttributeName
                                value:_bracketColors[brackets[b].style - kBracketStyle]
                                range:NSMakeRange(brackets[b].start, 1)];
    };
    for (size_t i = 0; i < tokens.size();) {
        size_t start = tokens[i].start, end = start + tokens[i].length;
        TokenStyle st = tokens[i].style;
        size_t j = i + 1;
        while (j < tokens.size() && tokens[j].style == st && tokens[j].start == end) {
            end = tokens[j].start + tokens[j].length;
            ++j;
        }
        i = j;
        if (start < range.location || end > limit || end <= start) continue;
        bracketsBefore(start);
        [storage addAttribute:NSForegroundColorAttributeName
                        value:_styleColors[(int)st]
                        range:NSMakeRange(start, end - start)];
        bracketsBefore(end);   // inside it (a preprocessor line's)
    }
    bracketsBefore(limit);
    if (_highlightingSettingsFile) [self decorateColorsIn:storage range:range];
}

// The attributes a token of `style` gets: the source attributes with its
// color (kPlainStyle is plain text, then the semantic and bracket colors),
// made once per font and color scheme.
- (NSDictionary *)attributesForStyle:(int)style {
    if (_styleAttributesBase != _sourceAttributes) {
        NSMutableArray *all = [NSMutableArray arrayWithCapacity:kStyleCount];
        auto add = ^(NSColor *color) {
            NSMutableDictionary *d = [_sourceAttributes mutableCopy];
            d[NSForegroundColorAttributeName] = color ?: _plainColor;
            [all addObject:[d copy]];
        };
        for (int i = 0; i < kTokenStyleCount; i++) add(_styleColors[i]);
        [all addObject:_sourceAttributes];
        for (int i = 0; i < kSemanticColorCount; i++) add(_semanticColors[i]);
        for (int i = 0; i < 4; i++) add(_bracketColors[i]);
        _styleAttributes = all;
        _styleAttributesBase = _sourceAttributes;
    }
    return _styleAttributes[(NSUInteger)style];
}

// Give [start, end) the attributes of `style`, unless it has them already.
- (void)setStyle:(int)style from:(NSUInteger)start to:(NSUInteger)end
       inStorage:(NSTextStorage *)storage {
    if (end <= start) return;
    NSDictionary *want = [self attributesForStyle:style];
    NSRange run;
    // As stored: an inlay hint's gap (SourceStorage.h) is no color to set.
    NSDictionary *have = [storage isKindOfClass:[MCSourceStorage class]]
        ? [(MCSourceStorage *)storage storedAttributesAtIndex:start longestEffectiveRange:&run
                                                       inRange:NSMakeRange(start, end - start)]
        : [storage attributesAtIndex:start longestEffectiveRange:&run
                             inRange:NSMakeRange(start, end - start)];
    if (run.location <= start && NSMaxRange(run) >= end &&
        (have == want || [have isEqualToDictionary:want]))
        return;
    [storage setAttributes:want range:NSMakeRange(start, end - start)];
}

// restyleRange: for the few lines an edit touches: the same attributes, but
// only pieces whose attributes differ are set. Setting the whole range
// first merged its attribute runs and adding the colors split them again,
// and each merge or split moved the storage's run array along by a
// memmove of everything after it (440,000 runs in a 100,000-line file), a
// good part of what was left of a keystroke there.
- (void)restyleChangedIn:(NSRange)range tokens:(const std::vector<Token> &)tokens
               inStorage:(NSTextStorage *)storage {
    [self restyleChangedIn:range
                    tokens:tokens
                  overlays:std::vector<MCOverlay>()
                 inStorage:storage];
}

// The same, with overlays (ascending: semantic spans, brackets) laid over
// the tokens: an overlay takes its own color, except over a string, an
// escape or a comment, which keep the lexer's.
- (void)restyleChangedIn:(NSRange)range tokens:(const std::vector<Token> &)tokens
                overlays:(const std::vector<MCOverlay> &)overlays
               inStorage:(NSTextStorage *)storage {
    const NSUInteger limit = NSMaxRange(range);
    auto set = ^(NSUInteger start, NSUInteger end, int style) {
        [self setStyle:style from:start to:end inStorage:storage];
    };
    const MCOverlay *spans = overlays.data();
    const size_t spanCount = overlays.size();
    __block size_t next = 0;   // the first overlay that may reach past where painting is
    auto paint = ^(NSUInteger start, NSUInteger end, int style) {
        if (end <= start) return;
        if (style == (int)TokenStyle::String || style == (int)TokenStyle::Escape ||
            style == (int)TokenStyle::Comment) {
            set(start, end, style);
            return;
        }
        NSUInteger at = start;
        while (next < spanCount && spans[next].end <= at) next++;
        for (size_t k = next; k < spanCount && spans[k].start < end; k++) {
            const NSUInteger a = MAX(at, spans[k].start);
            const NSUInteger b = MIN(end, spans[k].end);
            if (b <= a) continue;
            set(at, a, style);
            set(a, b, spans[k].style);
            at = b;
        }
        set(at, end, style);
    };
    NSUInteger at = range.location;
    for (size_t i = 0; i < tokens.size();) {
        size_t start = tokens[i].start, end = start + tokens[i].length;
        TokenStyle st = tokens[i].style;
        size_t j = i + 1;
        while (j < tokens.size() && tokens[j].style == st && tokens[j].start == end) {
            end = tokens[j].start + tokens[j].length;
            ++j;
        }
        i = j;
        if (start < at || end > limit || end <= start) continue;
        paint(at, start, kPlainStyle);
        paint(start, end, (int)st);
        at = end;
    }
    paint(at, limit, kPlainStyle);
}

// The storage's characters changed (typing, paste, undo, a Cmd+/ toggle, a
// color pick). Only the bookkeeping happens here: the highlighter's line
// table follows every edit, and the lines to recolor are remembered. The
// colors are applied in flushHighlighting, once the edit is complete.
// Changing attributes from inside this callback widens the storage's edited
// range, and NSTextView then moves the insertion point to the end of that
// range: typing "ab" in the middle of a line came out as "a" there and "b" on
// the next line.
- (void)textStorage:(NSTextStorage *)storage
    willProcessEditing:(NSTextStorageEditActions)actions
                 range:(NSRange)edited
        changeInLength:(NSInteger)delta {
    // The server's tokens were for the text before this edit. The lines it
    // touches are painted with the lexer's colors alone (flushHighlighting);
    // the rest keep theirs, which move with their text, until the next
    // answer.
    if ((actions & NSTextStorageEditedCharacters) && storage == self.textView.textStorage &&
        _semanticCurrent) {
        _semanticCurrent = NO;
        _semantic.clear();
    }
    if (!_liveHighlight || !(actions & NSTextStorageEditedCharacters)) return;
    if (storage != self.textView.textStorage) return;
    NSUInteger newLen = edited.length;
    NSUInteger oldLen = (NSUInteger)((NSInteger)edited.length - delta);
    NSRange range;
    if (_hl) {
        // The tokens are recomputed for the final text at flush time; the
        // bracket depths take what each line does to them now, so they
        // follow the text edit by edit.
        std::vector<Token> relexed;
        const size_t linesBefore = _hl->lineCount();
        NSStringSource src(storage.string);
        auto r = _hl->edit(src, edited.location, oldLen, newLen, relexed);
        range = NSMakeRange(r.start, r.end - r.start);
        if (_bracketsOn) {
            const size_t replaced = (r.endLine - r.firstLine) + linesBefore - _hl->lineCount();
            const bool moved = _bracketDepths.replace(
                r.firstLine, replaced,
                BracketPairs::summarizeLines(src, *_hl, r.firstLine, r.endLine, relexed));
            // What was painted with its bracket colors follows its text; the
            // edited characters are not, and when the edit changed the depth
            // the lines after it start at, nothing after it is.
            [_bracketPainted removeIndexesInRange:NSMakeRange(edited.location, oldLen)];
            [_bracketPainted shiftIndexesStartingAtIndex:edited.location + oldLen by:delta];
            if (moved && storage.length > edited.location)
                [_bracketPainted removeIndexesInRange:NSMakeRange(edited.location,
                                                                  storage.length - edited.location)];
        }
    } else {
        range = [storage.string lineRangeForRange:edited];
    }

    // Carry the ranges still pending across this edit, then add this edit's
    // own. Those after the edit keep their distances from the end; those
    // before it (at the back) move by the change in length, and those it
    // touches join this edit's range. A multi-range edit comes last range
    // first (CodeTextView replaceRanges:), so each edit finds nothing at the
    // back but what it touches: a few ranges looked at per edit, not all.
    const NSUInteger total = storage.length, oldTotal = total - (NSUInteger)delta;
    const NSUInteger editEnd = edited.location + oldLen;
    NSUInteger a = range.location, b = NSMaxRange(range);
    std::vector<std::pair<NSUInteger, NSUInteger>> before;
    while (!_pending.empty()) {
        const NSUInteger s0 = oldTotal - _pending.back().first;
        const NSUInteger e0 = oldTotal - _pending.back().second;
        if (s0 > editEnd) break;
        _pending.pop_back();
        if (e0 < edited.location) {
            before.push_back({s0, e0});
        } else {
            a = MIN(a, MIN(s0, edited.location));
            b = MAX(b, e0 >= editEnd ? e0 + newLen - oldLen : edited.location + newLen);
        }
    }
    _pending.push_back({total - a, total - b});
    for (auto it = before.rbegin(); it != before.rend(); ++it)
        _pending.push_back({total - it->first, total - it->second});
    // textDidChange: flushes right after the edit; this catches a change
    // made without didChangeText.
    if (!_flushScheduled) {
        _flushScheduled = YES;
        [self performSelector:@selector(flushHighlightingLater) withObject:nil afterDelay:0];
    }
}

- (void)flushHighlightingLater {
    _flushScheduled = NO;
    [self flushHighlighting];
}

// Recolor the lines edits have touched since the last flush.
- (void)flushHighlighting {
    if (_pending.empty()) return;
    NSTextStorage *storage = self.textView.textStorage;
    const NSUInteger total = storage.length;
    // Ascending, with ranges that touch joined.
    std::vector<std::pair<NSUInteger, NSUInteger>> spans;
    for (auto it = _pending.rbegin(); it != _pending.rend(); ++it) {
        const NSUInteger s = total - MIN(it->first, total), e = total - MIN(it->second, total);
        if (!spans.empty() && s <= spans.back().second)
            spans.back().second = MAX(spans.back().second, MAX(e, s));
        else
            spans.push_back({s, MAX(e, s)});
    }
    _pending.clear();
    if (!_liveHighlight) return;
    // Each span its own attribute edit: one around them all would invalidate
    // the layout from the first to the last.
    NSUInteger done = 0;   // where the last span's lines ended
    for (const auto &span : spans) {
        NSUInteger start = MAX(span.first, done), end = MAX(span.second, start);
        if (start >= total && done > 0) break;
        std::vector<Token> tokens;
        NSRange range;
        if (_hl) {
            size_t first = _hl->lineOf(start);
            size_t last = _hl->lineOf(end > start ? end - 1 : start);
            _hl->lineTokens(NSStringSource(storage.string), first, last + 1, tokens);
            size_t to = last + 1 < _hl->lineCount() ? _hl->lineStart(last + 1) : _hl->length();
            range = NSMakeRange(_hl->lineStart(first), to - _hl->lineStart(first));
        } else {
            range = [storage.string lineRangeForRange:NSMakeRange(start, end - start)];
        }
        if (NSMaxRange(range) <= done && done > 0) continue;   // these lines are done
        [storage beginEditing];
        [self restyleRange:range tokens:tokens inStorage:storage];
        [storage endEditing];
        if (_bracketsOn) [_bracketPainted addIndexesInRange:range];
        done = NSMaxRange(range);
    }
    // An edit that changed the depth below it: the brackets on screen take
    // their new colors now, the rest as they scroll into view.
    if (_bracketsOn) [self paintVisibleLines];
}

// ---- semantic tokens
// The session's answer for the text the storage holds now
// (LspSession.onSemanticTokens): kept, and painted over the lines on
// screen; other lines are painted as they scroll into view, until an edit
// ends them.
- (void)semanticTokensArrived:(NSData *)data {
    NSTextView *tv = self.textView;
    if (!_liveHighlight || !tv.editable || [self largeFileFeaturesOff] ||
        ![AppSettings shared].settings.semanticHighlighting())
        return;
    [self flushHighlighting];   // nothing waits to be lexed under them
    const size_t n = data.length / sizeof(Lsp::SemanticSpan);
    const auto *p = (const Lsp::SemanticSpan *)data.bytes;
    const NSUInteger total = tv.textStorage.length;
    _semantic.clear();
    _semantic.reserve(n);
    for (size_t i = 0; i < n; i++)
        if ((NSUInteger)p[i].start + p[i].length <= total) _semantic.push_back(p[i]);
    _semanticCurrent = YES;
    _semanticPainted = [NSMutableIndexSet indexSet];
    [self observeEditorScrolling];
    [self paintVisibleLines];
}

- (void)semanticViewScrolled:(NSNotification *)note {
    (void)note;
    if ((!_semanticCurrent && !_bracketsOn) || _semanticPaintScheduled) return;
    _semanticPaintScheduled = YES;   // once a run-loop turn, however many scroll events
    [self performSelector:@selector(paintVisibleLines) withObject:nil afterDelay:0];
}

// Paint the lines on screen not painted yet: with the server's tokens
// while they are current (each line's lexer tokens with the spans and the
// bracket colors over them, only the pieces whose attributes differ, in one
// attribute edit), and with bracket colors after an edit changed the depth
// they are at (only the brackets, so the server's colors, which move with
// their text, stay until its next answer). Lines too long to lex keep their
// plain color, as the lexer leaves them.
- (void)paintVisibleLines {
    _semanticPaintScheduled = NO;
    NSTextView *tv = self.textView;
    const BOOL sem = _semanticCurrent;
    const BOOL br = [self bracketDepthsCurrent];
    if ((!sem && !br) || !_liveHighlight || !tv.editable) return;
    NSTextStorage *storage = tv.textStorage;
    NSString *string = storage.string;
    const NSUInteger total = storage.length;
    if (!total) return;
    // Typing that left the depths alone: every bracket is current (the
    // usual case, decided without asking the layout what shows).
    if (!sem && [_bracketPainted containsIndexesInRange:NSMakeRange(0, total)]) return;
    NSLayoutManager *lm = tv.layoutManager;
    const NSRange glyphs = [lm glyphRangeForBoundingRect:tv.visibleRect
                                         inTextContainer:tv.textContainer];
    NSRange visible = [lm characterRangeForGlyphRange:glyphs actualGlyphRange:NULL];
    if (visible.location >= total) return;
    visible.length = MIN(visible.length, total - visible.location);
    if (visible.length && (!sem || [_semanticPainted containsIndexesInRange:visible]) &&
        (!br || [_bracketPainted containsIndexesInRange:visible]))
        return;
    BOOL editing = NO;
    NSUInteger at = visible.location;
    const NSUInteger end = NSMaxRange(visible);
    int32_t depth = br ? _bracketDepths.depthAt(_hl->lineOf(at)) : 0;
    NSStringSource src(string);
    do {
        NSRange line;
        size_t ln = 0;
        if (_hl) {
            ln = _hl->lineOf(at);
            const size_t to = ln + 1 < _hl->lineCount() ? _hl->lineStart(ln + 1) : _hl->length();
            line = NSMakeRange(_hl->lineStart(ln), to - _hl->lineStart(ln));
        } else {
            line = [string lineRangeForRange:NSMakeRange(at, 0)];
        }
        if (!line.length) break;
        const BOOL needSem = sem && ![_semanticPainted containsIndexesInRange:line];
        const BOOL needBr = br && ![_bracketPainted containsIndexesInRange:line];
        const int32_t depthHere = depth;
        if (br) depth = _bracketDepths.line(ln).after(depth);
        if (needSem || needBr) {
            std::vector<Token> tokens;
            if (_hl) _hl->lineTokens(src, ln, ln + 1, tokens);
            std::vector<BracketPairs::Bracket> brackets;
            if (br) BracketPairs::colorLines(src, *_hl, ln, ln + 1, tokens, depthHere, &brackets);
            if (!editing) {
                [storage beginEditing];
                editing = YES;
            }
            if (needSem) {
                [_semanticPainted addIndexesInRange:line];
                if (line.length <= kMaxTokenizationLineLength + 2) {
                    // The line's spans and brackets, in order.
                    const Lsp::SemanticSpan key{(uint32_t)line.location, 0,
                                                Lsp::SemanticStyle::None};
                    auto it = std::lower_bound(
                        _semantic.begin(), _semantic.end(), key,
                        [](const Lsp::SemanticSpan &a, const Lsp::SemanticSpan &b) {
                            return (uint64_t)a.start + a.length <= b.start;
                        });
                    std::vector<MCOverlay> overlays;
                    size_t b = 0;
                    for (; it != _semantic.end() && it->start < NSMaxRange(line); ++it) {
                        const int style = SemanticStyleIndex(it->style);
                        if (style < 0) continue;
                        for (; b < brackets.size() && brackets[b].at < it->start; ++b)
                            overlays.push_back({brackets[b].at, brackets[b].at + 1,
                                                kBracketStyle +
                                                    BracketPairs::colorIndex(brackets[b].depth)});
                        overlays.push_back({it->start, (NSUInteger)it->start + it->length, style});
                    }
                    for (; b < brackets.size(); ++b)
                        overlays.push_back({brackets[b].at, brackets[b].at + 1,
                                            kBracketStyle +
                                                BracketPairs::colorIndex(brackets[b].depth)});
                    [self restyleChangedIn:line tokens:tokens overlays:overlays inStorage:storage];
                }
            } else {
                for (const BracketPairs::Bracket &b : brackets)
                    [self setStyle:kBracketStyle + BracketPairs::colorIndex(b.depth)
                              from:b.at
                                to:b.at + 1
                         inStorage:storage];
                _bracketLinesPainted++;
            }
            if (br) [_bracketPainted addIndexesInRange:line];
        }
        at = NSMaxRange(line);
    } while (at < end);
    if (editing) [storage endEditing];
}

@end
