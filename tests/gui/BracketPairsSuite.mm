// BracketPairsSuite.mm — `make guitest SUITES=bracket_pairs`: bracket-pair
// colors in the editor (editor.bracket-pair-colorization, BracketPairs.h).
// Three depths take Dark+'s three bracket colors, in the storage and in
// pixels; a closer with nothing open takes the unexpected color; brackets
// in strings and comments keep the lexer's. A typed "(" at the top recolors
// the brackets on screen at once by painting only those lines (counted),
// leaves the lines far below alone until they scroll into view, and costs
// little CPU; undo puts the colors back. Off by its key; none in large-file
// mode. Also the lexer's escape and self colors.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "ControlServer.h"

namespace {
NSTextView *Text(EditorController *c) { return (NSTextView *)[c valueForKey:@"textView"]; }
MCTab *Active(EditorController *c) { return [c valueForKey:@"activeTab"]; }

NSColor *RGB(NSColor *c) { return [c colorUsingColorSpace:NSColorSpace.sRGBColorSpace]; }

// The color at a character as #RRGGBB.
NSString *ColorAt(EditorController *c, NSUInteger i) {
    NSTextStorage *ts = Text(c).textStorage;
    if (i >= ts.length) return @"(past the end)";
    NSColor *rgb = RGB([ts attribute:NSForegroundColorAttributeName atIndex:i effectiveRange:NULL]);
    if (!rgb) return @"(none)";
    return [NSString stringWithFormat:@"#%02X%02X%02X", (int)lround(rgb.redComponent * 255),
                                      (int)lround(rgb.greenComponent * 255),
                                      (int)lround(rgb.blueComponent * 255)];
}

// Where the n-th (0-based) occurrence of `what` starts.
NSUInteger Nth(NSString *s, NSString *what, int n) {
    NSUInteger from = 0;
    NSRange r = NSMakeRange(NSNotFound, 0);
    for (int k = 0; k <= n; k++) {
        r = [s rangeOfString:what options:0 range:NSMakeRange(from, s.length - from)];
        if (r.location == NSNotFound) return NSNotFound;
        from = NSMaxRange(r);
    }
    return r.location;
}

NSRange Visible(EditorController *c) {
    NSTextView *tv = Text(c);
    NSLayoutManager *lm = tv.layoutManager;
    NSRange g = [lm glyphRangeForBoundingRect:tv.visibleRect inTextContainer:tv.textContainer];
    return [lm characterRangeForGlyphRange:g actualGlyphRange:NULL];
}

NSUInteger Painted(EditorController *c) {
    return [[c valueForKey:@"bracketLinesPainted"] unsignedIntegerValue];
}

// Which of `colors` (hex) the character at i is drawn in, from the view's
// pixels: the pixel in its glyph box farthest from the background, read as
// background + a * (color - background) for each candidate, the closest
// fit winning (antialiasing blends a thin glyph's edges into the
// background). -1 when nothing is drawn there.
int DrawnIn(EditorController *c, NSUInteger i, NSArray<NSNumber *> *colors) {
    NSTextView *tv = Text(c);
    NSLayoutManager *lm = tv.layoutManager;
    NSRange g = [lm glyphRangeForCharacterRange:NSMakeRange(i, 1) actualCharacterRange:NULL];
    NSRect box = [lm boundingRectForGlyphRange:g inTextContainer:tv.textContainer];
    box = NSOffsetRect(box, tv.textContainerOrigin.x, tv.textContainerOrigin.y);
    NSBitmapImageRep *rep = [tv bitmapImageRepForCachingDisplayInRect:tv.visibleRect];
    [tv cacheDisplayInRect:tv.visibleRect toBitmapImageRep:rep];
    const CGFloat scale = rep.pixelsWide / NSWidth(tv.visibleRect);
    const NSRect vis = tv.visibleRect;
    NSColor *bg = RGB([rep colorAtX:1 y:1]);
    double best = 0;
    NSColor *ink = nil;
    for (CGFloat y = NSMinY(box); y < NSMaxY(box); y += 0.5 / scale * 2)
        for (CGFloat x = NSMinX(box); x < NSMaxX(box); x += 0.5 / scale * 2) {
            NSColor *p = RGB([rep colorAtX:(NSInteger)((x - NSMinX(vis)) * scale)
                                         y:(NSInteger)((y - NSMinY(vis)) * scale)]);
            const double d = fabs(p.redComponent - bg.redComponent) +
                             fabs(p.greenComponent - bg.greenComponent) +
                             fabs(p.blueComponent - bg.blueComponent);
            if (d > best) { best = d; ink = p; }
        }
    if (!ink || best < 0.2) return -1;
    int pick = -1;
    double pickErr = 1e9;
    for (NSUInteger k = 0; k < colors.count; k++) {
        const uint32_t h = colors[k].unsignedIntValue;
        const double cr = ((h >> 16) & 0xFF) / 255.0, cg = ((h >> 8) & 0xFF) / 255.0,
                     cb = (h & 0xFF) / 255.0;
        const double dr = cr - bg.redComponent, dg = cg - bg.greenComponent,
                     db = cb - bg.blueComponent;
        const double pr = ink.redComponent - bg.redComponent,
                     pg = ink.greenComponent - bg.greenComponent,
                     pb = ink.blueComponent - bg.blueComponent;
        const double len = dr * dr + dg * dg + db * db;
        if (len <= 0) continue;
        const double a = (pr * dr + pg * dg + pb * db) / len;
        const double er = pr - a * dr, eg = pg - a * dg, eb = pb - a * db;
        const double err = er * er + eg * eg + eb * eb + (a < 0.3 ? 1 : 0);
        if (err < pickErr) { pickErr = err; pick = (int)k; }
    }
    return pick;
}

NSString *const kGold = @"#FFD700", *const kOrchid = @"#DA70D6", *const kBlue = @"#179FFF";
NSString *const kUnexpected = @"#FF1212", *const kString = @"#CE9178", *const kPlain = @"#D4D4D4";
NSString *const kComment = @"#6A9955", *const kEscape = @"#D7BA7D", *const kSelf = @"#569CD6";
}  // namespace

MC_GUI_SUITE(bracket_pairs) {
    __block EditorController *c = nil;
    __block NSString *saved = nil;
    __block NSUInteger paintedBefore = 0, farBracket = 0, lastCloser = 0;
    __block double typeMs = 0;
    // A window of its own: the large file opened at the end stays out of the
    // shared window later suites type in.
    NSString *dir = [MCGuiScratch() stringByAppendingPathComponent:@"bracket_pairs"];
    NSString *file = [dir stringByAppendingPathComponent:@"depths.py"];
    NSString *big = [dir stringByAppendingPathComponent:@"big.py"];
    const int kLines = 3000;
    NSMutableString *body = [NSMutableString stringWithString:
        @"result = outer(inner[key{0}])\n"
        @"s = \"(x)\\n\"  # (comment)\n"
        @"def area(self):\n"];
    for (int k = 3; k < kLines; k++) [body appendFormat:@"value_%d = call(item)\n", k];
    [body appendString:@"oops = 1)\n"];   // the last line: a closer with nothing open
    NSString *original = [body copy];

    MCGuiStep(@"a Python file with nested brackets", ^{
        MCGuiWriteFile(file, original);
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:dir];
        for (EditorController *w in MCGuiControllers())
            if ([w.rootPath isEqualToString:dir]) c = w;
        GCHECK(c != nil);
        MCGuiSetKeyWindow(c.window);
        saved = [NSString stringWithContentsOfFile:NSProcessInfo.processInfo.environment[
                                                       @"MINICODE_SETTINGS"]
                                          encoding:NSUTF8StringEncoding
                                             error:nil] ?: @"";
        // Large enough to read the colors off the pixels; no auto-closing,
        // so a typed "(" is one character.
        MCGuiSetSettings([saved stringByAppendingString:
                                    @"\neditor.font-size = 24\neditor.auto-close = off\n"]);
        GCHECK([c showFile:file]);
        [Text(c) scrollRangeToVisible:NSMakeRange(0, 0)];
        GCHECK([[c valueForKey:@"bracketsOn"] boolValue]);
    });

    MCGuiStep(@"three depths in three colors, an unexpected closer in its own", ^{
        NSString *s = Text(c).string;
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"(", 0)), kGold);      // outer(
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"[", 0)), kOrchid);    // inner[
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"{", 0)), kBlue);      // key{
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"}", 0)), kBlue);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"]", 0)), kOrchid);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @")", 0)), kGold);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"1)", 0) + 1), kUnexpected);
        // In a string and a comment they are the string's and the comment's.
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"\"(x)", 0) + 1), kString);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"(comment)", 0)), kComment);
        // The lexer's newer styles: the escape and self.
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"\\n", 0)), kEscape);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"self", 0)), kSelf);
        // Far down, off screen, each line's call is at depth 0.
        farBracket = Nth(s, @"value_2900 = call(", 0) + 17;
        GCHECK_EQ_STR(ColorAt(c, farBracket), kGold);
        GCHECK(!NSLocationInRange(farBracket, Visible(c)));
    });

    MCGuiStep(@"and they are drawn so", ^{
        NSString *s = Text(c).string;
        NSArray *three = @[@0xFFD700, @0xDA70D6, @0x179FFF, @0xD4D4D4];
        const int a = DrawnIn(c, Nth(s, @"(", 0), three);
        const int b = DrawnIn(c, Nth(s, @"[", 0), three);
        const int d = DrawnIn(c, Nth(s, @"{", 0), three);
        MCGuiNote(@"drawn in colors %d, %d, %d", a, b, d);
        GCHECK(a == 0 && b == 1 && d == 2);
    });

    MCGuiStep(@"a ( typed at the top recolors the brackets on screen, and only those lines", ^{
        NSTextView *tv = Text(c);
        [c.window makeFirstResponder:tv];
        tv.selectedRange = NSMakeRange(0, 0);
        paintedBefore = Painted(c);
        const double t0 = MCGuiThreadCpuMs();
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@"(" replacementRange:NSMakeRange(0, 0)];
        });
        typeMs = MCGuiThreadCpuMs() - t0;
        NSString *s = tv.string;
        GCHECK_EQ_STR(ColorAt(c, 0), kGold);                    // the new one
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"(", 1)), kOrchid);    // outer( one deeper
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"[", 0)), kBlue);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"{", 0)), kGold);      // the cycle comes round
        // A line on screen below, one depth deeper now.
        const NSRange vis = Visible(c);
        NSUInteger lastOnScreen = NSNotFound;
        for (int k = 3; k < kLines; k++) {
            NSUInteger at = Nth(s, [NSString stringWithFormat:@"value_%d = call(", k], 0);
            if (!NSLocationInRange(at, vis)) break;
            lastOnScreen = at + [NSString stringWithFormat:@"value_%d = call", k].length;
        }
        GCHECK(lastOnScreen != NSNotFound);
        if (lastOnScreen != NSNotFound) GCHECK_EQ_STR(ColorAt(c, lastOnScreen), kOrchid);
        // Only the lines on screen were painted again, not the 3,000.
        const NSUInteger lines = Painted(c) - paintedBefore;
        const NSUInteger visibleLines =
            [[tv.string substringWithRange:vis] componentsSeparatedByString:@"\n"].count;
        MCGuiNote(@"the ( repainted %lu lines for their brackets (%lu on screen), in %.2f ms "
                  @"of CPU", (unsigned long)lines, (unsigned long)visibleLines, typeMs);
        GCHECK(lines > 0 && lines <= visibleLines + 2);
        // Far below, still the old color until it is seen; the last line's
        // closer, which now closes the new one, still unexpected.
        farBracket = Nth(s, @"value_2900 = call(", 0) + 17;
        lastCloser = Nth(s, @"oops = 1)", 0) + 8;
        GCHECK_EQ_STR(ColorAt(c, farBracket), kGold);
        GCHECK_EQ_STR(ColorAt(c, lastCloser), kUnexpected);
        GCHECK_TIME(typeMs, 60, @"typing ( at the top of 3,000 lines");
        [tv scrollRangeToVisible:NSMakeRange(farBracket, 1)];
    });
    MCGuiWait(@"scrolled into view, its new depth's color", 5, ^BOOL {
        return [ColorAt(c, farBracket) isEqualToString:kOrchid];
    });
    MCGuiStep(@"to the end", ^{
        [Text(c) scrollRangeToVisible:NSMakeRange(lastCloser, 1)];
    });
    MCGuiWait(@"where the last closer closes the new (", 5, ^BOOL {
        return [ColorAt(c, lastCloser) isEqualToString:kGold];
    });

    MCGuiStep(@"undo puts the colors back", ^{
        NSTextView *tv = Text(c);
        [tv scrollRangeToVisible:NSMakeRange(0, 0)];
        [tv.undoManager undo];
        GCHECK([tv.string isEqualToString:original]);
        NSString *s = tv.string;
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"(", 0)), kGold);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"[", 0)), kOrchid);
        lastCloser = Nth(s, @"oops = 1)", 0) + 8;
        [tv scrollRangeToVisible:NSMakeRange(lastCloser, 1)];
    });
    MCGuiWait(@"and at the end too, once seen", 5, ^BOOL {
        return [ColorAt(c, lastCloser) isEqualToString:kUnexpected];
    });
    MCGuiStep(@"and in between", ^{
        farBracket = Nth(Text(c).string, @"value_2900 = call(", 0) + 17;
        [Text(c) scrollRangeToVisible:NSMakeRange(farBracket, 1)];
    });
    MCGuiWait(@"far below, its old color", 5, ^BOOL {
        return [ColorAt(c, farBracket) isEqualToString:kGold];
    });

    MCGuiStep(@"off by its key: the brackets are plain text", ^{
        MCGuiSetSettings([saved stringByAppendingString:
                                    @"\neditor.font-size = 24\neditor.auto-close = off\n"
                                    @"editor.bracket-pair-colorization = off\n"]);
        NSString *s = Text(c).string;
        GCHECK(![[c valueForKey:@"bracketsOn"] boolValue]);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"(", 0)), kPlain);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"[", 0)), kPlain);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"1)", 0) + 1), kPlain);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"\\n", 0)), kEscape);   // the lexer's own stay
        // The same keystroke without them, for comparison.
        NSTextView *tv = Text(c);
        [tv scrollRangeToVisible:NSMakeRange(0, 0)];
        tv.selectedRange = NSMakeRange(0, 0);
        const double t0 = MCGuiThreadCpuMs();
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@"(" replacementRange:NSMakeRange(0, 0)];
        });
        MCGuiNote(@"the same ( without bracket colors: %.2f ms of CPU (%.2f with them)",
                  MCGuiThreadCpuMs() - t0, typeMs);
        [tv.undoManager undo];
        GCHECK([tv.string isEqualToString:original]);
        MCGuiSetSettings([saved stringByAppendingString:
                                    @"\neditor.font-size = 24\neditor.auto-close = off\n"]);
        GCHECK([[c valueForKey:@"bracketsOn"] boolValue]);
        GCHECK_EQ_STR(ColorAt(c, Nth(Text(c).string, @"[", 0)), kOrchid);
    });

    MCGuiStep(@"a 30,000-line file opens at about the cost it has without them", ^{
        // Past 64K characters the whole text is colored in one pass, whose
        // attribute runs must split in order (the brackets once went in a
        // second pass: 40 s for 100,000 lines).
        NSMutableString *t = [NSMutableString string];
        for (int k = 0; k < 30000; k++) [t appendString:@"    x = g(h[1], {'k': (2)})\n"];
        NSString *wide = [dir stringByAppendingPathComponent:@"wide.py"];
        MCGuiWriteFile(wide, t);
        double ms[2];
        for (int on = 1; on >= 0; on--) {
            MCGuiSetSettings([saved stringByAppendingString:
                                        on ? @"\neditor.auto-close = off\n"
                                           : @"\neditor.auto-close = off\n"
                                             @"editor.bracket-pair-colorization = off\n"]);
            const double t0 = MCGuiThreadCpuMs();
            GCHECK([c showFile:wide]);
            ms[on] = MCGuiThreadCpuMs() - t0;
            GCHECK([[c valueForKey:@"bracketsOn"] boolValue] == (BOOL)on);
            GCHECK([c closeTab:Active(c) prompt:NO]);
        }
        MCGuiNote(@"opening 30,000 lines: %.0f ms of CPU with bracket colors, %.0f without",
                  ms[1], ms[0]);
        GCHECK_TIME(ms[1], 2.5 * ms[0] + 150, @"opening with bracket colors");
        MCGuiSetSettings([saved stringByAppendingString:
                                    @"\neditor.font-size = 24\neditor.auto-close = off\n"]);
    });

    MCGuiStep(@"none in large-file mode", ^{
        // 300,001 lines: past VS Code's line limit, so large-file mode.
        NSMutableString *t = [NSMutableString stringWithCapacity:1700000];
        for (int k = 0; k <= 300000; k++) [t appendString:@"f(x)\n"];
        MCGuiWriteFile(big, t);
        GCHECK([c showFile:big]);
        GCHECK([c largeFileFeaturesOff]);
        GCHECK(![[c valueForKey:@"bracketsOn"] boolValue]);
        GCHECK_EQ_STR(ColorAt(c, 1), kPlain);
    });

    MCGuiStep(@"put back: the settings, the window, the files", ^{
        MCGuiSetSettings(saved);
        while (Active(c)) GCHECK([c closeTab:Active(c) prompt:NO]);
        [c.window close];
        c = nil;
        MCGuiSetKeyWindow(nil);
        [[NSFileManager defaultManager] removeItemAtPath:dir error:nil];
    });
}
