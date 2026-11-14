// EditorController+StatusBar.mm — EditorController: the status bar, its notes and "Saved ...", and the shortcut hints.
#import "EditorController+Private.h"
#import "EditorFont.h"
#include "PageMeasure.h"

@implementation EditorController (StatusBar)

// -------------------------------------------------------- status bar + hints
- (void)buildStatusBarInContainer:(NSView *)container height:(CGFloat)barH {
    self.statusBar = [[NSView alloc]
        initWithFrame:NSMakeRect(0, 0, container.bounds.size.width, barH)];
    self.statusBar.wantsLayer = YES;   // colors set in applySettings
    self.statusBar.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    self.statusBar.menu = MCAdjustSettingsMenu(@"statusbar", self.statusBar);

    NSTextField *label = [NSTextField labelWithString:@"⇧⌘H  Shortcuts"];
    label.font = [NSFont systemFontOfSize:11];
    label.backgroundColor = [NSColor clearColor];
    label.frame = NSMakeRect(0, 3, container.bounds.size.width - 34, 16);
    label.alignment = NSTextAlignmentRight;
    label.autoresizingMask = NSViewWidthSizable;
    self.statusLabel = label;
    [self.statusBar addSubview:label];

    // Far right: restart minicode, keeping its windows, tabs, unsaved
    // changes, browser pages and a conversation with Claude (restartMinicode:).
    NSButton *restart = [NSButton buttonWithTitle:@"↻" target:nil
                                           action:NSSelectorFromString(@"restartMinicode:")];
    restart.bordered = NO;
    restart.font = [NSFont systemFontOfSize:13];
    restart.toolTip = @"Restart minicode, keeping its windows, tabs, unsaved changes "
                      @"and browser pages";
    restart.frame = NSMakeRect(container.bounds.size.width - 28, 1, 22, 20);
    restart.autoresizingMask = NSViewMinXMargin;
    self.restartButton = restart;
    [self.statusBar addSubview:restart];

    // Left side: the first problem in the settings file, if any.
    NSTextField *errors = [NSTextField labelWithString:@""];
    errors.font = [NSFont systemFontOfSize:11];
    errors.frame = NSMakeRect(12, 3, container.bounds.size.width * 0.6, 16);
    errors.autoresizingMask = NSViewWidthSizable;
    errors.lineBreakMode = NSLineBreakByTruncatingTail;
    self.settingsLabel = errors;
    [self.statusBar addSubview:errors];

    // Left of them, with the tree's last-saved column on
    // (Option+Shift+Cmd+I): when the file on show was last saved.
    NSTextField *saved = [NSTextField labelWithString:@""];
    saved.font = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular];
    saved.lineBreakMode = NSLineBreakByTruncatingTail;
    saved.hidden = YES;
    self.savedLabel = saved;
    [self.statusBar addSubview:saved];

    // Middle: the language server for the open file and its problem count,
    // right-aligned just left of the shortcuts hint.
    MCLspStatusField *lsp = [[MCLspStatusField alloc] initWithFrame:NSZeroRect];
    lsp.font = [NSFont systemFontOfSize:11];
    lsp.alignment = NSTextAlignmentRight;
    lsp.lineBreakMode = NSLineBreakByTruncatingHead;
    lsp.frame = NSMakeRect(container.bounds.size.width - 150 - 340, 3, 340, 16);
    lsp.autoresizingMask = NSViewMinXMargin;
    __weak EditorController *weakSelf = self;
    // A missing server: how to install it. Otherwise every server.
    lsp.onClick = ^{
        EditorController *s = weakSelf;
        if (s.lsp.missingServer) [s showInstallHelpFrom:s.lspLabel];
        else [s showLanguageServers:nil];
    };
    self.lspLabel = lsp;
    [self.statusBar addSubview:lsp];
    // Left of it: the window's problems, every open file's, clickable to
    // show the Problems list.
    MCLspStatusField *problems = [[MCLspStatusField alloc] initWithFrame:NSZeroRect];
    problems.font = [NSFont systemFontOfSize:11];
    problems.alignment = NSTextAlignmentRight;
    problems.lineBreakMode = NSLineBreakByTruncatingTail;
    problems.autoresizingMask = NSViewMinXMargin;
    problems.toolTip = @"Problems in the open files: click to list them (⇧⌘M)";
    problems.hidden = YES;
    problems.onClick = ^{ [weakSelf toggleProblems:nil]; };
    self.problemsLabel = problems;
    [self.statusBar addSubview:problems];
    [container addSubview:self.statusBar];
    [self layoutStatusRight];
}

// The right of the status bar: the problem counts, then the language
// server, each as wide as its text, ending where the shortcuts hint begins.
- (void)layoutStatusRight {
    if (!self.statusBar) return;
    const CGFloat W = self.statusBar.bounds.size.width;
    const CGFloat right = W - 150;
    CGFloat lw = MIN(340, ceil(self.lspLabel.attributedStringValue.size.width) + 6);
    self.lspLabel.frame = NSMakeRect(right - lw, 3, lw, 16);
    CGFloat left = right - lw;
    if (!self.problemsLabel.hidden) {
        CGFloat pw = MIN(220, ceil(self.problemsLabel.attributedStringValue.size.width) + 6);
        self.problemsLabel.frame = NSMakeRect(right - lw - 16 - pw, 3, pw, 16);
        left = right - lw - 16 - pw;
    }
    [self layoutLargeFileNoteEndingAt:left - 16];
}

// The hints panel: a scroll view over a text view, at the top right of the
// window, as tall as its text or the window's room (then it scrolls). The
// text view neither edits nor selects; the panel keeps out of the way.
- (void)buildHintsPanelInContainer:(NSView *)container {
    const CGFloat pw = 360, ph = 240, margin = 18;
    NSRect r = NSMakeRect(container.bounds.size.width - pw - margin,
                          container.bounds.size.height - ph - margin, pw, ph);
    self.hintsPanel = [[NSView alloc] initWithFrame:r];
    self.hintsPanel.wantsLayer = YES;
    self.hintsPanel.layer.backgroundColor =
        [NSColor colorWithSRGBRed:0.11 green:0.11 blue:0.12 alpha:0.97].CGColor;
    self.hintsPanel.layer.cornerRadius = 8;
    self.hintsPanel.layer.borderWidth = 1;
    self.hintsPanel.layer.borderColor = Hex(0x3C3C3C).CGColor;
    self.hintsPanel.layer.masksToBounds = YES;
    self.hintsPanel.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    self.hintsPanel.hidden = YES;

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSInsetRect(self.hintsPanel.bounds, 1, 1)];
    scroll.drawsBackground = NO;
    scroll.borderType = NSNoBorder;
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = NO;
    scroll.autohidesScrollers = YES;
    scroll.automaticallyAdjustsContentInsets = NO;
    scroll.scrollerKnobStyle = NSScrollerKnobStyleLight;
    NSTextView *tv = [[NSTextView alloc] initWithFrame:scroll.bounds];
    tv.editable = NO;
    tv.selectable = NO;
    tv.drawsBackground = NO;
    tv.richText = YES;
    tv.usesFontPanel = NO;
    tv.textContainerInset = NSMakeSize(kHintsPad - 1, kHintsPad - 1);
    tv.textContainer.lineFragmentPadding = 0;
    tv.textContainer.widthTracksTextView = NO;
    tv.horizontallyResizable = NO;
    tv.verticallyResizable = YES;
    tv.minSize = NSZeroSize;
    tv.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    scroll.documentView = tv;
    [self.hintsPanel addSubview:scroll];
    self.hintsScroll = scroll;
    self.hintsTextView = tv;
    [container addSubview:self.hintsPanel];
}

// Fill the panel for the room the window has: two columns when they fit
// side by side, else one, never a row broken in the middle; as tall as the
// text, or the room, with a scroll bar. Anchored to the top right.
- (void)updateHints {
    NSTextView *tv = self.hintsTextView;
    NSView *container = self.hintsPanel.superview;
    if (!tv || !container) return;
    const CGFloat margin = 18, inset = kHintsPad;
    const CGFloat W = NSWidth(container.bounds), H = NSHeight(container.bounds);
    const CGFloat top = H - [self titlebarHeight] - margin;
    const CGFloat roomH = MAX(80, top - kStatusBarHeight - margin);
    const CGFloat roomW = MAX(120, W - 2 * margin) - 2 * inset;
    // Scroll bars that take room (no trackpad, CI) take it from the text.
    const CGFloat bar = NSScroller.preferredScrollerStyle == NSScrollerStyleLegacy
        ? [NSScroller scrollerWidthForControlSize:NSControlSizeRegular
                                     scrollerStyle:NSScrollerStyleLegacy]
        : 0;
    NSArray *rows = [self hintsRows];
    PageMeasure::HintsLayout layout;
    NSAttributedString *text = [self hintsTextForRows:rows room:roomW layout:&layout];
    CGFloat textH = [self hintsHeightOf:text width:layout.textWidth];
    const BOOL scrolls = textH + 2 * inset > roomH;
    if (scrolls && bar > 0) {
        text = [self hintsTextForRows:rows room:roomW - bar layout:&layout];
        textH = [self hintsHeightOf:text width:layout.textWidth];
    }
    self.hintsColumns = layout.columns;
    const CGFloat pw = ceil(layout.textWidth + 2 * inset + (scrolls ? bar : 0));
    const CGFloat ph = ceil(MIN(textH + 2 * inset, roomH));
    self.hintsPanel.frame = NSMakeRect(W - pw - margin, top - ph, pw, ph);
    self.hintsScroll.frame = NSInsetRect(self.hintsPanel.bounds, 1, 1);
    tv.textContainer.size = NSMakeSize(layout.textWidth, CGFLOAT_MAX);
    [tv.textStorage setAttributedString:text];
    [tv sizeToFit];
    [self.hintsScroll tile];
}

// The text's height laid out `width` points wide.
- (CGFloat)hintsHeightOf:(NSAttributedString *)text width:(CGFloat)width {
    NSTextStorage *storage = [[NSTextStorage alloc] initWithAttributedString:text];
    NSLayoutManager *lm = [NSLayoutManager new];
    NSTextContainer *tc = [[NSTextContainer alloc] initWithSize:NSMakeSize(width, CGFLOAT_MAX)];
    tc.lineFragmentPadding = 0;
    [lm addTextContainer:tc];
    [storage addLayoutManager:lm];
    [lm ensureLayoutForTextContainer:tc];
    return ceil(NSHeight([lm usedRectForTextContainer:tc]));
}

// The hints are laid out on tab stops, never with spaces: the key glyphs
// (⌘ ⇧ ⌃ ⌫ ⏎) have different widths even in a monospaced font, so padding with
// spaces leaves every column ragged. A row holds one or two shortcuts:
//   key | label | key | label       (two shortcuts)
//   key | label | state             (a pane, with whether it is open)
// The stops come from the widest key and label of each column as drawn
// (PageMeasure::layoutHints), at the editor's zoom (MCZoomScale), so no
// row breaks in the middle; a window too narrow for two columns gets one,
// a shortcut a row.
static const CGFloat kHintsPad = 16;

- (void)appendHintsHeading:(NSString *)title to:(NSMutableArray *)s {
    [s addObject:@{@"heading": title.uppercaseString}];
}

// One row. `key2`/`label2` may be nil; `state` (muted) takes the second
// column instead, for panes.
- (void)appendHintsRow:(NSMutableArray *)s
                   key:(NSString *)key1 label:(NSString *)label1
                  key2:(NSString *)key2 label2:(NSString *)label2
                 state:(NSString *)state {
    NSMutableDictionary *row = [NSMutableDictionary dictionary];
    row[@"key1"] = key1 ?: @"";
    row[@"label1"] = label1 ?: @"";
    if (key2) {
        row[@"key2"] = key2;
        row[@"label2"] = label2 ?: @"";
    } else if (state) {
        row[@"state"] = state;
    }
    [s addObject:row];
}

// The rows as text for `room` points of width, and where its stops went.
- (NSAttributedString *)hintsTextForRows:(NSArray<NSDictionary *> *)rows room:(CGFloat)room
                                  layout:(PageMeasure::HintsLayout *)layoutOut {
    const CGFloat z = MCZoomScale();
    NSFont *keyFont = MCMonoFont(round(12 * z * 2) / 2, NSFontWeightMedium);
    NSFont *labelFont = [NSFont systemFontOfSize:round(12 * z * 2) / 2];
    NSFont *headingFont = [NSFont systemFontOfSize:round(10 * z * 2) / 2
                                            weight:NSFontWeightSemibold];
    NSDictionary *headingMeasure = @{NSFontAttributeName: headingFont, NSKernAttributeName: @(0.6 * z)};
    NSDictionary *keyMeasure = @{NSFontAttributeName: keyFont};
    NSDictionary *labelMeasure = @{NSFontAttributeName: labelFont};
    auto width = [](NSString *t, NSDictionary *a) -> double {
        return t.length ? ceil([t sizeWithAttributes:a].width) : 0;
    };
    std::vector<PageMeasure::HintRow> measured;
    for (NSDictionary *row in rows) {
        PageMeasure::HintRow m;
        if (row[@"heading"]) {
            m.heading = true;
            m.key1 = width(row[@"heading"], headingMeasure);
        } else if (row[@"gap"]) {
            m.heading = true;
        } else {
            m.key1 = width(row[@"key1"], keyMeasure);
            m.label1 = width(row[@"label1"], labelMeasure);
            if (row[@"key2"]) {
                m.hasKey2 = true;
                m.key2 = width(row[@"key2"], keyMeasure);
                m.label2 = width(row[@"label2"], labelMeasure);
            } else if (row[@"state"]) {
                m.hasState = true;
                m.state = width(row[@"state"], labelMeasure);
            }
        }
        measured.push_back(m);
    }
    const PageMeasure::HintsLayout layout =
        PageMeasure::layoutHints(measured, MAX(room, 40), 12 * z, 28 * z);
    if (layoutOut) *layoutOut = layout;

    auto tab = [](CGFloat at) {
        return [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft location:at options:@{}];
    };
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineSpacing = round(3 * z);
    ps.headIndent = layout.label1;   // a label that must wrap stays in its column
    ps.lineBreakMode = NSLineBreakByWordWrapping;
    if (layout.columns == 2)
        ps.tabStops = @[tab(layout.label1), tab(layout.key2), tab(layout.label2)];
    else if (!layout.wraps)
        ps.tabStops = @[tab(layout.label1), tab(layout.state)];
    else
        ps.tabStops = @[tab(layout.label1)];
    NSDictionary *keyAttr = @{NSFontAttributeName: keyFont,
                              NSForegroundColorAttributeName: Hex(0x4EA1F7),
                              NSParagraphStyleAttributeName: ps};
    NSDictionary *labelAttr = @{NSFontAttributeName: labelFont,
                                NSForegroundColorAttributeName: Hex(0xE0E0E0),
                                NSParagraphStyleAttributeName: ps};
    NSDictionary *stateAttr = @{NSFontAttributeName: labelFont,
                                NSForegroundColorAttributeName: Hex(0x9CA3AF),
                                NSParagraphStyleAttributeName: ps};
    NSMutableAttributedString *s = [NSMutableAttributedString new];
    void (^add)(NSString *, NSDictionary *) = ^(NSString *t, NSDictionary *a) {
        [s appendAttributedString:[[NSAttributedString alloc] initWithString:t attributes:a]];
    };
    BOOL first = YES;
    for (NSDictionary *row in rows) {
        if (row[@"heading"]) {
            NSMutableParagraphStyle *hp = [NSMutableParagraphStyle new];
            hp.paragraphSpacingBefore = first ? 0 : round(10 * z);
            hp.paragraphSpacing = round(4 * z);
            add([row[@"heading"] stringByAppendingString:@"\n"], @{
                NSFontAttributeName: headingFont,
                NSForegroundColorAttributeName: Hex(0x9CA3AF),
                NSKernAttributeName: @(0.6 * z),
                NSParagraphStyleAttributeName: hp,
            });
        } else if (row[@"gap"]) {
            add(@"\n", @{NSFontAttributeName: [NSFont systemFontOfSize:round(6 * z)]});
        } else if (layout.columns == 2 || !row[@"key2"]) {
            add(row[@"key1"], keyAttr);
            add([@"\t" stringByAppendingString:row[@"label1"]], labelAttr);
            if (row[@"key2"]) {
                add([@"\t" stringByAppendingString:row[@"key2"]], keyAttr);
                add([@"\t" stringByAppendingString:row[@"label2"]], labelAttr);
            } else if (row[@"state"]) {
                // At its stop; when even one column wraps, after its label.
                add([(layout.wraps ? @" " : @"\t") stringByAppendingString:row[@"state"]],
                    stateAttr);
            }
            add(@"\n", labelAttr);
        } else {
            // One column: the pair's shortcuts on rows of their own.
            add(row[@"key1"], keyAttr);
            add([NSString stringWithFormat:@"\t%@\n", row[@"label1"]], labelAttr);
            add(row[@"key2"], keyAttr);
            add([NSString stringWithFormat:@"\t%@\n", row[@"label2"]], labelAttr);
        }
        first = NO;
    }
    // No trailing newline, or the panel sizes itself one empty line too tall.
    if ([s.string hasSuffix:@"\n"]) [s deleteCharactersInRange:NSMakeRange(s.length - 1, 1)];
    return s;
}

// A command's first key as the hints show it (from keys.conf and the
// registry, Keymap.h); "—" when keys.conf took every key away.
- (NSString *)hk:(NSString *)commandId {
    NSString *k = MCKeymapGlyphs(commandId);
    return k.length ? k : @"—";
}

// Every key of a command, "⌥↓ ⌘↓"; the one at index, "" when there is none;
// and a pane's title with its other keys, "Terminal  (also ⌘J, ⌃`)".
- (NSString *)hkAll:(NSString *)commandId {
    NSArray<NSString *> *all = MCKeymapAllGlyphs(commandId);
    return all.count ? [all componentsJoinedByString:@" "] : @"—";
}
- (NSString *)hk:(NSString *)commandId at:(NSUInteger)index {
    NSArray<NSString *> *all = MCKeymapAllGlyphs(commandId);
    return index < all.count ? all[index] : @"";
}
- (NSString *)hkAlso:(NSString *)commandId title:(NSString *)title {
    NSArray<NSString *> *all = MCKeymapAllGlyphs(commandId);
    if (all.count < 2) return title;
    NSArray *rest = [all subarrayWithRange:NSMakeRange(1, all.count - 1)];
    return [NSString stringWithFormat:@"%@  (also %@)", title, [rest componentsJoinedByString:@", "]];
}

// The shortcut list as text for the widest panel (tests read it).
- (NSAttributedString *)hintsText {
    return [self hintsTextForRows:[self hintsRows] room:CGFLOAT_MAX layout:NULL];
}

// Context-aware shortcut list: headings and rows (appendHintsRow:).
- (NSArray<NSDictionary *> *)hintsRows {
    NSMutableArray *s = [NSMutableArray array];
    NSString *(^openOr)(BOOL) = ^NSString *(BOOL open) {
        return open ? @"open" : @"hidden";
    };

    [self appendHintsHeading:@"Editing" to:s];
    [self appendHintsRow:s key:[self hk:@"explorer.newFile"] label:@"New file"
                    key2:[self hk:@"workbench.action.files.openFolder"] label2:@"Open folder" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.files.save"] label:@"Save"
                    key2:[self hk:@"actions.find"] label2:@"Find in file" state:nil];
    [self appendHintsRow:s key:[self hk:@"undo"] label:@"Undo"
                    key2:[self hk:@"redo"] label2:@"Redo" state:nil];
    [self appendHintsRow:s key:[NSString stringWithFormat:@"%@ %@", [self hk:@"editor.action.clipboardCopyAction"], [self hk:@"editor.action.clipboardCutAction"]] label:@"Copy, cut (the line if none)"
                    key2:[self hk:@"editor.action.selectAll"] label2:@"Select all" state:nil];
    [self appendHintsRow:s key:[self hk:@"expandLineSelection"] label:@"Select line"
                    key2:[self hk:@"editor.action.deleteLines"] label2:@"Delete line" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.commentLine"] label:@"Toggle comment"
                    key2:[self hk:@"workbench.action.findInFiles"] label2:@"Find in folder" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.startFindReplaceAction"] label:@"Replace in file"
                    key2:[self hk:@"workbench.action.replaceInFiles"] label2:@"Replace in folder" state:nil];
    [self appendHintsRow:s key:[NSString stringWithFormat:@"%@ %@", [self hk:@"editor.action.nextMatchFindAction"], [self hk:@"editor.action.previousMatchFindAction"]] label:@"Next, previous match"
                    key2:[self hk:@"actions.findWithSelection"] label2:@"Use selection for find" state:nil];
    [self appendHintsRow:s key:[NSString stringWithFormat:@"%@ %@", [self hk:@"toggleFindCaseSensitive"], [self hk:@"toggleFindWholeWord"]] label:@"Match case, whole word"
                    key2:[self hk:@"toggleFindRegex"] label2:@"Regular expression" state:nil];
    [self appendHintsRow:s key:@"⇧⌘1" label:@"Replace (in the find bar)"
                    key2:[self hk:@"editor.action.replaceAll"] label2:@"Replace all" state:nil];
    [self appendHintsRow:s key:@"⌥⌘L" label:@"Find in selection (in the bar)"
                    key2:[self hk:@"togglePreserveCase"] label2:@"Preserve case" state:nil];
    [self appendHintsRow:s key:@"⇧⌘drag" label:@"Column, or two clicks"
                    key2:@"⌥click" label2:@"Add a cursor" state:nil];
    [self appendHintsRow:s key:[self hkAll:@"editor.action.moveLinesDownAction"] label:@"Move line down"
                    key2:[self hkAll:@"editor.action.moveLinesUpAction"] label2:@"Move line up" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.copyLinesDownAction"] label:@"Copy line down"
                    key2:[self hk:@"editor.action.copyLinesUpAction"] label2:@"Copy line up" state:nil];
    [self appendHintsRow:s key:[self hk:@"cursorTop"] label:@"Start of the file"
                    key2:[self hk:@"cursorBottom"] label2:@"End of the file (⇧ selects)" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.insertCursorBelow"] label:@"Add cursor below"
                    key2:[self hk:@"editor.action.insertCursorAbove"] label2:@"Add cursor above" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.addSelectionToNextFindMatch"] label:@"Add next occurrence"
                    key2:[self hk:@"editor.action.selectHighlights"] label2:@"Select all occurrences" state:nil];
    [self appendHintsRow:s key:[self hk:@"minicode.markdown.toggleTask"] label:@"Toggle task (Markdown)"
                    key2:[self hk:@"minicode.showTodos"] label2:@"TODOs in folder" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.openSettings"] label:@"Settings"
                    key2:[self hk:@"editor.action.triggerSuggest"] label2:@"Complete" state:nil];
    [self appendHintsRow:s key:[self hk:@"inlineChat.start"] label:@"Inline agent"
                    key2:[self hk:@"editor.action.showHover"] label2:@"Hover info" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.revealDefinition"] label:@"Go to definition"
                    key2:nil label2:nil state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.goToReferences"] label:@"Find references"
                    key2:[self hk:@"editor.action.rename"] label2:@"Rename symbol" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.marker.next"] label:@"Next problem"
                    key2:[self hk:@"editor.action.marker.prev"] label2:@"Previous problem" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.editor.nextChange"] label:@"Next change (git)"
                    key2:[self hk:@"workbench.action.editor.previousChange"] label2:@"Previous change" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.quickFix"] label:@"Quick fixes"
                    key2:[self hk:@"editor.action.organizeImports"] label2:@"Organize imports" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.formatDocument"] label:@"Format document"
                    key2:@"⇥" label2:@"Take completion" state:nil];
    [self appendHintsRow:s key:[self hk:@"minicode.sortLines"] label:@"Sort lines"
                    key2:[self hk:@"minicode.sortKeepSortedBlocks"] label2:@"Sort keep-sorted blocks" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.indentLines"] label:@"Indent lines (or ⇥)"
                    key2:[self hk:@"editor.action.outdentLines"] label2:@"Outdent lines (or ⇧⇥)" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.insertLineAfter"] label:@"Insert line below"
                    key2:[self hk:@"python.execSelectionInTerminal"] label2:@"Line to Python REPL" state:nil];
    [self appendHintsRow:s key:[NSString stringWithFormat:@"%@ %@", [self hk:@"editor.action.fontZoomIn"], [self hk:@"editor.action.fontZoomOut"]] label:@"Zoom the editor in, out"
                    key2:[self hk:@"editor.action.fontZoomReset"] label2:@"Reset zoom" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.toggleWordWrap"] label:@"Word wrap"
                    key2:nil label2:nil state:MCEditorWordWrap() ? @"on" : @"off"];
    [self appendHintsRow:s key:[self hk:@"editor.fold"] label:@"Fold"
                    key2:[self hk:@"editor.unfold"] label2:@"Unfold" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.foldAll"] label:@"Fold all"
                    key2:[self hk:@"editor.unfoldAll"] label2:@"Unfold all" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.toggleFold"] label:@"Toggle fold"
                    key2:@"⌃⌘1…7" label2:@"Fold level 1 to 7" state:nil];

    [self appendHintsHeading:@"Files" to:s];
    [self appendHintsRow:s key:[self hk:@"workbench.action.newWindow"] label:@"New window"
                    key2:[self hk:@"explorer.newFolder"] label2:@"New folder" state:nil];
    [self appendHintsRow:s key:@"⌘⌫" label:@"Trash (in the tree)"
                    key2:[self hk:@"workbench.files.action.refreshFilesExplorer"] label2:@"Refresh tree" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.rename"] label:@"Rename (in the tree)"
                    key2:@"Space" label2:@"Quick Look (in the tree)" state:nil];
    [self appendHintsRow:s key:[self hk:@"minicode.toggleHiddenFiles"] label:@"Show hidden files"
                    key2:[self hk:@"minicode.tree.toggleSizes"] label2:@"Sizes" state:nil];
    [self appendHintsRow:s key:[self hk:@"minicode.tree.toggleSaved"] label:@"When last saved"
                    key2:[self hk:@"minicode.tree.toggleCreated"] label2:@"When created" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.files.saveAll"] label:@"Save all"
                    key2:[self hk:@"workbench.action.openRecent"] label2:@"Open recent folder" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.closeOtherEditors"] label:@"Close other tabs"
                    key2:nil label2:nil state:nil];

    [self appendHintsHeading:@"Navigation" to:s];
    [self appendHintsRow:s key:[self hk:@"workbench.action.showCommands"] label:@"Command palette"
                    key2:[self hk:@"workbench.action.quickOpen"] label2:@"Go to file" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.gotoLine"] label:@"Go to line"
                    key2:[self hk:@"editor.action.jumpToBracket"] label2:@"Go to bracket" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.gotoSymbol"] label:@"Go to symbol"
                    key2:[self hk:@"workbench.action.showAllSymbols"] label2:@"Symbol in workspace" state:nil];
    [self appendHintsRow:s key:[NSString stringWithFormat:@"%@ %@", [self hk:@"workbench.action.navigateBack"], [self hk:@"workbench.action.navigateForward"]] label:@"Back, forward"
                    key2:nil label2:nil state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.focusSideBar"] label:@"Focus tree"
                    key2:[self hk:@"workbench.action.focusFirstEditorGroup"] label2:@"Focus editor (left group)" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.focusSecondEditorGroup"] label:@"Focus right group"
                    key2:[self hk:@"minicode.focusBrowser"] label2:@"Focus browser" state:nil];
    [self appendHintsRow:s key:[self hk:@"minicode.runFile"] label:@"Run the file in the terminal"
                    key2:[self hk:@"minicode.requestChange"] label2:@"Request a change to minicode" state:nil];
    [self appendHintsRow:s key:@"↑ ↓" label:@"Browse tree"
                    key2:@"⏎" label2:@"Open file" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.toggleSidebarVisibility"] label:@"Toggle sidebar"
                    key2:[self hk:@"editor.action.showContextMenu"] label2:@"Context menu" state:nil];

    [self appendHintsHeading:@"Tabs" to:s];
    [self appendHintsRow:s key:[self hk:@"workbench.action.nextEditor"] label:@"Next tab"
                    key2:[self hk:@"workbench.action.previousEditor"] label2:@"Previous tab" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.nextEditor" at:1] label:@"Next tab"
                    key2:[self hk:@"workbench.action.previousEditor" at:1] label2:@"Previous tab" state:nil];
    [self appendHintsRow:s key:@"⌃1…9" label:@"Tab 1 to 9 (also ⇧⌘1…9; 9 is the last)"
                    key2:nil label2:nil state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.closeActiveEditor"] label:@"Close tab, or the focused panel"
                    key2:nil label2:nil state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.reopenClosedEditor"] label:@"Reopen closed tab"
                    key2:nil label2:nil state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.splitEditor"] label:@"Split editor"
                    key2:[self hk:@"workbench.action.moveEditorToNextGroup"] label2:@"Tab to right group" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.closeWindow"] label:@"Close window"
                    key2:[self hk:@"workbench.action.quit"] label2:@"Quit" state:nil];

    [self appendHintsHeading:@"Panes" to:s];
    [self appendHintsRow:s key:[self hk:@"minicode.toggleEditor"] label:@"Editor"
                    key2:nil label2:nil state:openOr(!self.editorHidden)];
    [self appendHintsRow:s key:@"⇧⌘T" label:[self hkAlso:@"workbench.action.terminal.toggleTerminal" title:@"Terminal"]
                    key2:nil label2:nil state:openOr(self.terminalVisible)];
    if (self.terminalVisible) {
        [self appendHintsRow:s key:[self hk:@"workbench.action.terminal.new"] label:@"New terminal"
                        key2:[self hk:@"workbench.action.terminal.split"] label2:@"Split terminal" state:nil];
        [self appendHintsRow:s key:[self hk:@"workbench.action.terminal.focusNext"] label:@"Next terminal"
                        key2:[self hk:@"workbench.action.terminal.focusPrevious"] label2:@"Previous" state:nil];
        [self appendHintsRow:s key:[self hk:@"workbench.action.terminal.kill"] label:@"Close terminal"
                        key2:nil label2:nil state:nil];
        [self appendHintsRow:s key:[self hk:@"workbench.action.terminal.resizePaneLeft"]
                       label:@"Resize pane left"
                        key2:[self hk:@"workbench.action.terminal.resizePaneUp"]
                      label2:@"Up (right, down too)" state:nil];
        [self appendHintsRow:s key:@"⌘ click" label:@"Open a file:line or URL in the terminal"
                        key2:nil label2:nil state:nil];
        [self appendHintsRow:s key:[self hk:@"actions.find"] label:@"Find in the terminal"
                        key2:[self hk:@"workbench.action.terminal.clear"] label2:@"Clear the terminal" state:nil];
        [self appendHintsRow:s
                         key:[NSString stringWithFormat:@"%@ %@",
                              [self hk:@"workbench.action.terminal.fontZoomIn"],
                              [self hk:@"workbench.action.terminal.fontZoomOut"]]
                       label:@"Zoom the terminals (in one)"
                        key2:[self hk:@"workbench.action.terminal.fontZoomReset"]
                      label2:@"Reset their zoom" state:nil];
        if (!self.terminal.lineMode) {
            [self appendHintsRow:s key:@"⇧⇞" label:@"Scroll back"
                            key2:@"⌘↖" label2:@"Top of the scrollback" state:nil];
            [self appendHintsRow:s key:@"⌘← ⌘→" label:@"Start, end of the line"
                            key2:@"⌘⌫" label2:@"Delete to its start" state:nil];
            [self appendHintsRow:s
                             key:[NSString stringWithFormat:@"%@ %@",
                                  [self hk:@"workbench.action.terminal.scrollToPreviousCommand"],
                                  [self hk:@"workbench.action.terminal.scrollToNextCommand"]]
                           label:@"Previous, next command"
                            key2:[self hk:@"workbench.action.terminal.selectToPreviousCommand"]
                          label2:@"Select it" state:nil];
        }
    }
    [self appendHintsRow:s key:[self hk:@"minicode.toggleBrowser"] label:@"Browser"
                    key2:nil label2:nil state:openOr(self.browserVisible)];
    if (self.browserVisible) {
        // The browser's tabs, while it has the keyboard: Cmd+T (a page key)
        // and the editor's tab keys, which the pane answers itself.
        [self appendHintsRow:s key:[self hk:@"minicode.browser.newTab"] label:@"New browser tab"
                        key2:[self hk:@"workbench.action.nextEditor"]
                      label2:@"Next browser tab" state:nil];
        [self appendHintsRow:s key:[self hk:@"actions.find"] label:@"Find in the page"
                        key2:[self hk:@"minicode.browser.saveBookmark"]
                      label2:@"Save as bookmark" state:nil];
        // A page's own keys, kept off the menus (Keymap.mm).
        [self appendHintsRow:s key:[NSString stringWithFormat:@"%@ %@", [self hk:@"minicode.page.zoomIn"],
                                    [self hk:@"minicode.page.zoomOut"]]
                       label:@"Zoom the page or a PDF"
                        key2:[self hk:@"minicode.page.actualSize"] label2:@"Actual size" state:nil];
    }
    [self appendHintsRow:s key:[self hk:@"minicode.toggleMaximizedPane"] label:@"Maximize pane"
                    key2:nil label2:nil
                   state:self.maximizedPane != MCPaneNone ? @"maximized" : nil];
    [self appendHintsRow:s key:[self hk:@"workbench.view.scm"] label:@"Source control"
                    key2:nil label2:nil state:openOr(self.gitPanelVisible)];
    [self appendHintsRow:s key:[self hk:@"minicode.toggleAgents"] label:@"Agents"
                    key2:nil label2:nil state:openOr(self.agentsVisible)];
    [self appendHintsRow:s key:[self hk:@"workbench.actions.view.problems"] label:@"Problems"
                    key2:nil label2:nil state:openOr(self.problemsVisible)];
    if (self.problemsVisible) {
        [self appendHintsRow:s key:@"⏎" label:@"Go to the problem"
                        key2:@"⎋" label2:@"Back to the editor" state:nil];
    }
    if (self.agentsVisible) {
        [self appendHintsRow:s key:@"⏎" label:@"Evidence, or open the file"
                        key2:@"Space" label2:@"Expand or collapse" state:nil];
    }
    if (self.gitPanelVisible) {
        [self appendHintsRow:s key:@"⏎" label:@"Show the diff or commit"
                        key2:@"Space" label2:@"Stage or unstage" state:nil];
        [self appendHintsRow:s key:@"⇥" label:@"Changes, graph, message"
                        key2:@"⌘⏎" label2:@"Commit (in the message)" state:nil];
    }
    if (self.isMarkdown) {
        [self appendHintsRow:s key:[self hk:@"markdown.showPreview"] label:@"Markdown preview"
                        key2:nil label2:nil
                       state:self.previewMode ? @"rendered" : @"source"];
        [self appendHintsRow:s key:[self hk:@"markdown.showPreviewToSide"]
                       label:@"Preview to the side" key2:nil label2:nil
                       state:[self sidePreviewShowing] ? @"open" : nil];
        [self appendHintsRow:s key:[self hk:@"minicode.markdown.exportHtml"]
                       label:@"Export as HTML (File menu)" key2:nil label2:nil state:nil];
    } else if (self.isLatex) {
        [self appendHintsRow:s key:[self hk:@"markdown.showPreview"] label:@"LaTeX preview"
                        key2:nil label2:nil
                       state:self.previewMode ? @"typeset" : @"source"];
        [self appendHintsRow:s key:[self hk:@"minicode.latex.exportPDF"] label:@"Export PDF"
                        key2:[self hk:@"latex-workshop.synctex"] label2:@"Cursor's line in PDF"
                       state:nil];
    }
    if (self.isMedia) {
        [self appendHintsRow:s key:[self hk:@"minicode.togglePlayback"] label:@"Play or pause"
                        key2:[self hk:@"workbench.action.focusFirstEditorGroup"] label2:@"Focus the player" state:nil];
    }
    if (self.isDiff && _activeTab.comparison) {
        [self appendHintsRow:s key:[self hk:@"editor.action.accessibleDiffViewer.next"] label:@"Next difference"
                        key2:[self hk:@"editor.action.accessibleDiffViewer.prev"] label2:@"Previous difference" state:nil];
    }
    if (_notes.tourActive) {
        [self appendHintsRow:s key:[self hk:@"minicode.tour.next"] label:@"Next stop of the tour"
                        key2:[self hk:@"minicode.tour.previous"] label2:@"Previous stop" state:nil];
    }

    // A short gap, not a whole empty heading, before the way out.
    [s addObject:@{@"gap": @YES}];
    [self appendHintsRow:s key:[self hk:@"minicode.toggleShortcutHints"] label:@"Hide these hints"
                    key2:nil label2:nil state:nil];
    return s;
}

- (void)toggleHints:(id)sender {
    self.hintsVisible = !self.hintsVisible;
    if (self.hintsVisible) [self updateHints];
    self.hintsPanel.hidden = !self.hintsVisible;
    self.statusLabel.stringValue =
        self.hintsVisible ? @"⇧⌘H  Hide shortcuts" : @"⇧⌘H  Shortcuts";
}

// A passing note on the left of the status bar, where settings problems
// show; they come back when it goes.
- (void)showStatusNote:(NSString *)note {
    if (!note.length) return;
    self.settingsLabel.stringValue = note;
    self.settingsLabel.toolTip = note;
    NSString *shown = [note copy];
    __weak EditorController *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(6 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        EditorController *s = weakSelf;
        if ([s.settingsLabel.stringValue isEqualToString:shown]) [s showSettingsErrors];
    });
}

// The status bar's "Saved 14:32" for the tab on show, with details on.
- (void)updateSavedStatus {
    NSString *text = @"", *tip = nil;
    MCTab *t = _activeTab;
    if (MCTreeColumnShown(MCTreeColumnSaved) && t.currentPath && !t.isDiff) {
        NSDate *disk = t.fileModDate;
        if (t.gone || !disk) text = @"Not saved";
        else if (t.dirty) text = @"Unsaved changes";
        else {
            text = MCSavedPhrase(disk, [NSDate date]);
            // Pictures, PDFs and media are never saved from here.
            if ((t.isImage || t.isPDF || t.isMedia || MCViewerKindWithoutText(t.viewerKind)) &&
                [text hasPrefix:@"Saved"])
                text = [@"Modified" stringByAppendingString:[text substringFromIndex:5]];
        }
        if (disk) tip = [@"On disk since " stringByAppendingString:MCFullDate(disk)];
    }
    self.savedLabel.stringValue = text;
    self.savedLabel.toolTip = tip;
    self.savedLabel.hidden = text.length == 0;
    [self layoutStatusLabels];
}

// The left of the status bar: "Saved ..." then any settings problem.
- (void)layoutStatusLabels {
    if (!self.statusBar) return;
    const CGFloat W = self.statusBar.bounds.size.width;
    CGFloat x = 12;
    if (!self.savedLabel.hidden) {
        const CGFloat w = MIN(ceil(self.savedLabel.fittingSize.width) + 2, 220);
        self.savedLabel.frame = NSMakeRect(x, 3, w, 16);
        x += w + 14;
    }
    self.settingsLabel.frame = NSMakeRect(x, 3, MAX(0, W * 0.6 - (x - 12)), 16);
}

// A message in the status bar for a few seconds, in place of the hints key.
- (void)flashStatus:(NSString *)message {
    if (!message.length) return;
    self.statusLabel.stringValue = message;
    self.statusLabel.toolTip = message;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(endFlash)
                                               object:nil];
    [self performSelector:@selector(endFlash) withObject:nil afterDelay:6];
}
- (void)endFlash {
    self.statusLabel.stringValue =
        self.hintsVisible ? @"⇧⌘H  Hide shortcuts" : @"⇧⌘H  Shortcuts";
    self.statusLabel.toolTip = nil;
}

@end
