// EditorController+Highlighting.mm — EditorController: the source's syntax colors, kept up as it is edited.
#import "EditorController+Private.h"
#import "EditorController+HighlightingIvars.h"

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
    [self.textView.textStorage setAttributedString:attr];
    self.textView.typingAttributes = base;      // typed text stays monospaced
    self.textView.editable = YES;
    [self applyWordWrap];   // a preview always wraps; the source as the setting says
    // Regions by the file's name; an untitled buffer by its extension.
    self.textView.folds.fileName = self.currentPath ?:
        [@"untitled." stringByAppendingString:self.currentExt ?: @"txt"];
    [self applyHighlighting];
    [self.textView scrollToBeginningOfDocument:nil];
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
    for (int i = 0; i < 8; ++i) _styleColors[i] = ColorForStyle((TokenStyle)i);
    _highlightingSettingsFile = [self isSettingsFile];
    if (_sourceAttributes) {
        NSMutableDictionary *base = [_sourceAttributes mutableCopy];
        base[NSForegroundColorAttributeName] = _plainColor;
        _sourceAttributes = base;
    }

    std::vector<Token> tokens;
    if (SyntaxHighlighter::supports(cext)) {
        _hl.reset(new IncrementalHighlighter<char16_t>(cext));
        _hl->reset(NSStringSource(storage.string), &tokens);
    } else {
        _hl.reset();
    }
    [storage beginEditing];
    [self restyleRange:NSMakeRange(0, storage.length) tokens:tokens inStorage:storage];
    [storage endEditing];
    _pending.clear();   // all of it is fresh now
    _liveHighlight = YES;
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
    if (_sourceAttributes && !_highlightingSettingsFile && range.length <= 65536) {
        [self restyleChangedIn:range tokens:tokens inStorage:storage];
        return;
    }
    if (_sourceAttributes) {
        [storage setAttributes:_sourceAttributes range:range];
    } else {
        [storage addAttribute:NSForegroundColorAttributeName value:_plainColor range:range];
        [storage removeAttribute:NSBackgroundColorAttributeName range:range];
        [storage removeAttribute:NSLinkAttributeName range:range];
    }
    const NSUInteger limit = NSMaxRange(range);
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
        [storage addAttribute:NSForegroundColorAttributeName
                        value:_styleColors[(int)st & 7]
                        range:NSMakeRange(start, end - start)];
    }
    if (_highlightingSettingsFile) [self decorateColorsIn:storage range:range];
}

// The attributes a token of `style` gets: the source attributes with its
// color (index 8 is plain text), made once per font and color scheme.
- (NSDictionary *)attributesForStyle:(int)style {
    if (_styleAttributesBase != _sourceAttributes) {
        NSMutableArray *all = [NSMutableArray arrayWithCapacity:9];
        for (int i = 0; i < 8; i++) {
            NSMutableDictionary *d = [_sourceAttributes mutableCopy];
            d[NSForegroundColorAttributeName] = _styleColors[i];
            [all addObject:[d copy]];
        }
        [all addObject:_sourceAttributes];
        _styleAttributes = all;
        _styleAttributesBase = _sourceAttributes;
    }
    return _styleAttributes[(NSUInteger)style];
}

// restyleRange: for the few lines an edit touches: the same attributes, but
// only pieces whose attributes differ are set. Setting the whole range
// first merged its attribute runs and adding the colors split them again,
// and each merge or split moved the storage's run array along by a
// memmove of everything after it (440,000 runs in a 100,000-line file), a
// good part of what was left of a keystroke there.
- (void)restyleChangedIn:(NSRange)range tokens:(const std::vector<Token> &)tokens
               inStorage:(NSTextStorage *)storage {
    const NSUInteger limit = NSMaxRange(range);
    auto paint = ^(NSUInteger start, NSUInteger end, int style) {
        if (end <= start) return;
        NSDictionary *want = [self attributesForStyle:style];
        NSRange run;
        NSDictionary *have = [storage attributesAtIndex:start effectiveRange:&run];
        if (run.location <= start && NSMaxRange(run) >= end &&
            (have == want || [have isEqualToDictionary:want]))
            return;
        [storage setAttributes:want range:NSMakeRange(start, end - start)];
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
        paint(at, start, 8);
        paint(start, end, (int)st & 7);
        at = end;
    }
    paint(at, limit, 8);
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
    if (!_liveHighlight || !(actions & NSTextStorageEditedCharacters)) return;
    if (storage != self.textView.textStorage) return;
    NSUInteger newLen = edited.length;
    NSUInteger oldLen = (NSUInteger)((NSInteger)edited.length - delta);
    NSRange range;
    if (_hl) {
        std::vector<Token> unused;   // recomputed for the final text at flush time
        auto r = _hl->edit(NSStringSource(storage.string), edited.location, oldLen,
                           newLen, unused);
        range = NSMakeRange(r.start, r.end - r.start);
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
        done = NSMaxRange(range);
    }
}

@end
