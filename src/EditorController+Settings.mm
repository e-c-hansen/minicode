// EditorController+Settings.mm — EditorController: the settings file: color swatches, opacity, Adjust Settings.
#import "EditorController+Private.h"

@implementation EditorController (Settings)

// minicode's own rewrites of settings.conf (View > Theme, the IDE link)
// ask every window first: a tab holding the file with unsaved changes
// refuses them, as Replace in Folder leaves such files alone, and the
// reason shows in the front window's status bar.
+ (void)load {
    AppSettings.unsavedTextCheck = ^BOOL(NSString *path) {
        for (NSWindow *w in NSApp.windows) {
            id d = w.delegate;
            if ([d isKindOfClass:[EditorController class]] && [(EditorController *)d tabForPath:path].dirty)
                return YES;
        }
        return NO;
    };
    AppSettings.writeProblemHandler = ^(NSString *message) {
        NSMutableArray<NSWindow *> *order = [NSMutableArray arrayWithArray:NSApp.orderedWindows];
        if (NSApp.mainWindow) [order insertObject:NSApp.mainWindow atIndex:0];
        if (NSApp.keyWindow) [order insertObject:NSApp.keyWindow atIndex:0];
        for (NSWindow *w in order) {
            if (![w.delegate isKindOfClass:[EditorController class]]) continue;
            [(EditorController *)w.delegate showStatusNote:message];
            return;
        }
    };
}

// Cmd+, : open the settings file in this window, creating it first.
- (void)openSettings:(id)sender {
    AppSettings *cfg = [AppSettings shared];
    if (![cfg ensureFileExists]) {
        [self warn:[NSString stringWithFormat:@"Could not create %@.", cfg.path]];
        return;
    }
    [self openFileAtPath:cfg.path];
    [self focusEditor:nil];
}

// keys.conf, as Settings… opens settings.conf; the first time it is written
// with every command and its keys, commented out (Commands::defaultFileText).
- (void)openKeyboardShortcuts:(id)sender {
    (void)sender;
    MCKeymap *keys = [MCKeymap shared];
    if (![keys ensureFileExists]) {
        [self warn:[NSString stringWithFormat:@"Could not create %@.", keys.path]];
        return;
    }
    [self openFileAtPath:keys.path];
    [self focusEditor:nil];
}

// ------------------------------------------------ settings file: color swatches
- (BOOL)isSettingsFile {
    if (!self.currentPath) return NO;
    NSString *mine = self.currentPath.stringByResolvingSymlinksInPath;
    return [mine isEqualToString:
        [AppSettings shared].path.stringByResolvingSymlinksInPath];
}

static NSColor *ContrastColor(const Rgba &c) {
    return MCColor([AppSettings shared].settings.contrastText(c));
}

// Show every color value in `range` as a swatch of itself, clickable to pick a
// new one.
- (void)decorateColorsIn:(NSTextStorage *)storage range:(NSRange)range {
    NSString *str = storage.string;
    [str enumerateSubstringsInRange:[str lineRangeForRange:range]
                            options:NSStringEnumerationByLines
                         usingBlock:^(NSString *line, NSRange lr, NSRange er, BOOL *stop) {
        (void)er; (void)stop;
        ColorSpan span;
        std::u16string wide = U16(line);
        if (!Settings::findColor(wide, span)) {
            // An opacity is a link too, opening a slider; a dotted underline
            // says it can be clicked without looking like a swatch.
            ValueSpan os;
            double value;
            if (Settings::findOpacity(wide, os, value))
                [storage addAttributes:@{
                    NSLinkAttributeName: kOpacityLink,
                    NSUnderlineStyleAttributeName:
                        @(NSUnderlineStyleSingle | NSUnderlinePatternDot),
                    NSUnderlineColorAttributeName: Hex(0x4EA1F7),
                } range:NSMakeRange(lr.location + os.start, os.length)];
            return;
        }
        [storage addAttributes:@{
            NSBackgroundColorAttributeName: MCColor(span.color),
            NSForegroundColorAttributeName: ContrastColor(span.color),
            NSLinkAttributeName: kColorLink,
        } range:NSMakeRange(lr.location + span.start, span.length)];
    }];
}

- (BOOL)textView:(NSTextView *)tv clickedOnLink:(id)link atIndex:(NSUInteger)i {
    if (self.isMarkdown && self.previewMode && [link isKindOfClass:NSString.class] &&
        [link hasPrefix:kTaskLinkPrefix]) {
        [self toggleTaskOnSourceLine:[[link substringFromIndex:kTaskLinkPrefix.length] intValue]];
        return YES;
    }
    if (self.isMarkdown && self.previewMode && [link isEqual:kMathDownloadLink]) {
        [self downloadTectonicForMath];
        return YES;
    }
    if ([self showsRenderedMarkdown] && [link isKindOfClass:NSString.class]) {
        [self openMarkdownLink:link];
        return YES;
    }
    if ([link isEqual:kOpacityLink]) {
        [self pickOpacityAtIndex:i];
        return YES;
    }
    if (![link isEqual:kColorLink]) return NO;
    [self pickColorAtIndex:i];
    return YES;
}

// Open the system color panel on the color at character index i. Picks
// rewrite that line (uncommenting it if needed) and save, so the change
// shows up live.
- (void)pickColorAtIndex:(NSUInteger)i {
    NSString *str = self.textView.string;
    if (i > str.length) return;
    NSUInteger start, contentsEnd;
    [str getLineStart:&start end:NULL contentsEnd:&contentsEnd
             forRange:NSMakeRange(i, 0)];
    ColorSpan span;
    if (!Settings::findColor(U16([str substringWithRange:
            NSMakeRange(start, contentsEnd - start)]), span)) return;
    self.colorLineStart = start;
    self.colorEditPath = self.currentPath;
    // Picks from this panel session undo together, apart from earlier typing.
    [self.textView breakUndoCoalescing];
    // Caret on the color, so nothing is selected when the panel also sends
    // changeColor: to the text view.
    [self.textView setSelectedRange:NSMakeRange(start + span.start, 0)];

    NSColorPanel *panel = [NSColorPanel sharedColorPanel];
    panel.showsAlpha = YES;
    panel.continuous = YES;
    panel.target = nil;   // setting the starting color must not count as a pick
    panel.action = NULL;
    panel.color = MCColor(span.color);
    panel.target = self;
    panel.action = @selector(colorPicked:);
    [panel orderFront:nil];
}

- (void)colorPicked:(NSColorPanel *)panel {
    NSTextView *tv = self.textView;
    if (!self.colorEditPath || ![self.currentPath isEqualToString:self.colorEditPath] ||
        !tv.editable)
        return;
    NSColor *c = [panel.color colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    if (!c) return;
    auto byte = [](CGFloat v) { return (uint8_t)lround(MIN(MAX(v, 0.0), 1.0) * 255); };
    Rgba rgba;
    rgba.r = byte(c.redComponent);
    rgba.g = byte(c.greenComponent);
    rgba.b = byte(c.blueComponent);
    rgba.a = MIN(MAX(c.alphaComponent, 0.0), 1.0);

    NSString *str = tv.string;
    if (self.colorLineStart > str.length) return;
    NSUInteger start, contentsEnd;
    [str getLineStart:&start end:NULL contentsEnd:&contentsEnd
             forRange:NSMakeRange(self.colorLineStart, 0)];
    NSRange lineRange = NSMakeRange(start, contentsEnd - start);
    std::u16string line = U16([str substringWithRange:lineRange]);
    ColorSpan span;
    if (!Settings::findColor(line, span)) return;   // the line was edited away
    std::u16string updated = Settings::setColor(line, rgba);
    if (updated == line) return;
    NSString *replacement = FromU16(updated);
    if (![tv shouldChangeTextInRange:lineRange replacementString:replacement]) return;
    [tv.textStorage replaceCharactersInRange:lineRange withString:replacement];
    [tv didChangeText];
    [tv.undoManager setActionName:@"Change Color"];
    ColorSpan now;
    if (Settings::findColor(updated, now))
        [tv setSelectedRange:NSMakeRange(start + now.start, 0)];

    // Save shortly after the last change, so dragging around the color wheel
    // doesn't write the file dozens of times a second.
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(saveCurrentFile:)
                                               object:nil];
    [self performSelector:@selector(saveCurrentFile:) withObject:nil afterDelay:0.15];
}

// The rectangle of a character range in the text view's coordinates.
- (NSRect)editorRectForRange:(NSRange)range {
    NSTextView *tv = self.textView;
    NSLayoutManager *lm = tv.layoutManager;   // the editor is TextKit 1 (see Lsp.mm)
    [lm ensureLayoutForCharacterRange:NSMakeRange(0, NSMaxRange(range))];
    NSRange glyphs = [lm glyphRangeForCharacterRange:range actualCharacterRange:NULL];
    NSRect r = [lm boundingRectForGlyphRange:glyphs inTextContainer:tv.textContainer];
    return NSOffsetRect(r, tv.textContainerOrigin.x, tv.textContainerOrigin.y);
}

// A click on an opacity value in the settings file: a slider in a popover.
// Like a color pick, each change rewrites the line (uncommenting it) and the
// file is saved shortly after, so the panel changes as the slider moves.
- (void)pickOpacityAtIndex:(NSUInteger)i {
    NSString *str = self.textView.string;
    if (i > str.length) return;
    NSUInteger start, contentsEnd;
    [str getLineStart:&start end:NULL contentsEnd:&contentsEnd
             forRange:NSMakeRange(i, 0)];
    ValueSpan span;
    double value;
    if (!Settings::findOpacity(U16([str substringWithRange:
            NSMakeRange(start, contentsEnd - start)]), span, value)) return;
    self.opacityLineStart = start;
    self.colorEditPath = self.currentPath;
    [self.textView breakUndoCoalescing];
    NSRange valueRange = NSMakeRange(start + span.start, span.length);
    [self.textView setSelectedRange:NSMakeRange(valueRange.location, 0)];
    __weak EditorController *weakSelf = self;
    [MCOpacityPopover showForValue:value
                    relativeToRect:[self editorRectForRange:valueRange]
                            ofView:self.textView
                          onChange:^(double v) { [weakSelf opacityPicked:v]; }];
}

- (void)opacityPicked:(double)value {
    NSTextView *tv = self.textView;
    if (!self.colorEditPath || ![self.currentPath isEqualToString:self.colorEditPath] ||
        !tv.editable)
        return;
    NSString *str = tv.string;
    if (self.opacityLineStart > str.length) return;
    NSUInteger start, contentsEnd;
    [str getLineStart:&start end:NULL contentsEnd:&contentsEnd
             forRange:NSMakeRange(self.opacityLineStart, 0)];
    NSRange lineRange = NSMakeRange(start, contentsEnd - start);
    std::u16string line = U16([str substringWithRange:lineRange]);
    std::u16string updated = Settings::setOpacity(line, value);
    if (updated == line) return;
    NSString *replacement = FromU16(updated);
    if (![tv shouldChangeTextInRange:lineRange replacementString:replacement]) return;
    [tv.textStorage replaceCharactersInRange:lineRange withString:replacement];
    [tv didChangeText];
    [tv.undoManager setActionName:@"Change Opacity"];
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(saveCurrentFile:)
                                               object:nil];
    [self performSelector:@selector(saveCurrentFile:) withObject:nil afterDelay:0.15];
}

// ------------------------------------------------- Adjust Settings (right click)
// The editor's context menu: AppKit's items, then Adjust Settings for the
// editor, or for the Markdown preview while it shows.
- (NSMenu *)textView:(NSTextView *)tv menu:(NSMenu *)menu forEvent:(NSEvent *)event
             atIndex:(NSUInteger)index {
    (void)event; (void)index;
    if (tv == self.textView)
        MCAddAdjustSettingsItem(menu, (self.isMarkdown && self.previewMode) ? @"markdown"
                                                                            : @"editor", tv);
    return menu;
}

// View > Adjust Settings > (surface): the id is the item's representedObject.
- (void)adjustAppearance:(id)sender {
    id surface = [sender respondsToSelector:@selector(representedObject)]
        ? [sender representedObject] : nil;
    if ([surface isKindOfClass:[NSString class]]) [self showSettingsForSurface:surface];
}

// Open the settings file with a surface's keys in view: the first key's line
// in the middle of the pane with its value selected, and every key's line
// tinted for a few seconds.
- (void)showSettingsForSurface:(NSString *)surface {
    const AppearanceSurface *s = Settings::appearanceSurface(surface.UTF8String ?: "");
    if (!s) return;
    [self openSettings:nil];
    if (![self isSettingsFile] || (self.isMarkdown && self.previewMode)) return;

    NSTextView *tv = self.textView;
    // A file written before some of these keys existed has no line for
    // them: their commented-out defaults are added at its end first, so
    // the swatches and sliders are there to use.
    const std::string missing = Settings::missingSurfaceLines(Utf8String(tv.string),
                                                              surface.UTF8String ?: "");
    if (!missing.empty() && tv.editable) [self appendSettingsLines:missing title:s->title];
    NSString *text = tv.string;
    std::string utf8 = text.UTF8String ?: "";
    // Start of each line, so a line number from the core maps to a range.
    NSMutableArray<NSNumber *> *starts = [NSMutableArray arrayWithObject:@0];
    for (NSUInteger i = 0; i < text.length; i++)
        if ([text characterAtIndex:i] == '\n') [starts addObject:@(i + 1)];
    NSMutableArray<NSValue *> *lines = [NSMutableArray array];
    for (const char *key : s->keys) {
        int n = Settings::findKeyLine(utf8, key);
        if (n < 0 || n >= (int)starts.count) continue;
        NSRange r = [text lineRangeForRange:NSMakeRange(starts[n].unsignedIntegerValue, 0)];
        while (r.length && [text characterAtIndex:NSMaxRange(r) - 1] == '\n') r.length--;
        NSValue *v = [NSValue valueWithRange:r];
        if (![lines containsObject:v]) [lines addObject:v];
    }
    if (!lines.count) {
        [self flashStatus:[NSString stringWithFormat:@"No %s settings in this file.", s->title]];
        return;
    }

    NSRange first = lines.firstObject.rangeValue;
    ValueSpan value;
    NSRange selection = NSMakeRange(first.location, 0);
    if (Settings::findValue(U16([text substringWithRange:first]), value))
        selection = NSMakeRange(first.location + value.start, value.length);
    [tv setSelectedRange:selection];
    [self centerEditorOnRange:first];
    // Once more after the pane has had its layout pass.
    dispatch_async(dispatch_get_main_queue(), ^{
        if ([self isSettingsFile] && NSMaxRange(first) <= tv.string.length)
            [self centerEditorOnRange:first];
    });
    [self tintKeyLines:lines];
}

// Add the default lines of keys the settings file lacks at its end, under a
// line saying where they came from, as one undo step. A file with no unsaved
// changes is saved at once (the lines are all commented out, so nothing
// changes but the file), one with changes is left for the user to save.
- (void)appendSettingsLines:(const std::string &)lines title:(const char *)title {
    NSTextView *tv = self.textView;
    NSString *text = tv.string;
    const BOOL wasClean = !self.dirty;
    NSMutableString *add = [NSMutableString string];
    if (text.length && ![text hasSuffix:@"\n"]) [add appendString:@"\n"];
    if (text.length) [add appendString:@"\n"];
    [add appendFormat:@"# Added by Adjust Settings: the %s keys this file did not have yet.\n",
                      title];
    [add appendString:[NSString stringWithUTF8String:lines.c_str()] ?: @""];
    const NSRange end = NSMakeRange(text.length, 0);
    [tv breakUndoCoalescing];
    if (![tv shouldChangeTextInRange:end replacementString:add]) return;
    [tv.textStorage replaceCharactersInRange:end withString:add];
    [tv didChangeText];
    [tv breakUndoCoalescing];
    [tv.undoManager setActionName:@"Adjust Settings"];
    if (wasClean) [self saveCurrentFile:nil];
}

- (void)centerEditorOnRange:(NSRange)range {
    NSRect r = [self editorRectForRange:range];
    NSClipView *clip = self.editorScroll.contentView;
    CGFloat visible = clip.bounds.size.height;
    CGFloat maxY = MAX(0, NSHeight(self.textView.frame) - visible);
    CGFloat y = MIN(MAX(0, NSMidY(r) - visible / 2), maxY);
    [clip scrollToPoint:NSMakePoint(clip.bounds.origin.x, y)];
    [self.editorScroll reflectScrolledClipView:clip];
}

// A quiet accent tint behind the lines, held for a moment and then faded
// out. Temporary attributes, so the file and its undo are untouched; a color
// swatch keeps its own color.
- (void)tintKeyLines:(NSArray<NSValue *> *)lines {
    NSLayoutManager *lm = self.textView.layoutManager;
    NSUInteger generation = ++self.keyFlashGeneration;
    void (^paint)(CGFloat) = ^(CGFloat alpha) {
        if (generation != self.keyFlashGeneration) return;
        NSString *text = self.textView.string;
        NSUInteger length = text.length;
        [lm removeTemporaryAttribute:NSBackgroundColorAttributeName
                   forCharacterRange:NSMakeRange(0, length)];
        if (alpha <= 0 || ![self isSettingsFile]) return;
        NSColor *tint = [Hex(0x4EA1F7) colorWithAlphaComponent:alpha];
        for (NSValue *v in lines) {
            NSRange r = v.rangeValue;
            if (NSMaxRange(r) > length) continue;
            NSRange skip = NSMakeRange(NSMaxRange(r), 0);
            ColorSpan swatch;
            if (Settings::findColor(U16([text substringWithRange:r]), swatch))
                skip = NSMakeRange(r.location + swatch.start, swatch.length);
            NSRange before = NSMakeRange(r.location, skip.location - r.location);
            NSRange after = NSMakeRange(NSMaxRange(skip), NSMaxRange(r) - NSMaxRange(skip));
            for (NSValue *part in @[[NSValue valueWithRange:before],
                                    [NSValue valueWithRange:after]])
                if (part.rangeValue.length)
                    [lm addTemporaryAttribute:NSBackgroundColorAttributeName value:tint
                            forCharacterRange:part.rangeValue];
        }
    };
    paint(0.24);
    const double steps[][2] = {{2.4, 0.16}, {2.7, 0.09}, {3.0, 0.04}, {3.3, 0}};
    for (const auto &step : steps) {
        CGFloat alpha = step[1];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(step[0] * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{ paint(alpha); });
    }
}

@end
