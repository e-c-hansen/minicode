// EditorController+LargeFile.mm — EditorController: large-file mode, as VS
// Code's (LargeFile.h has the numbers). A file over 20 MB or 300,000 lines,
// with editor.large-file-optimizations on, opens without syntax colors,
// folding, bracket boxes or a language server; the status bar says so, and
// a click there turns them on for that tab. Word wrap stays as set (see
// LargeFile.h for why it differs from VS Code there). Opening a file
// over 50 MB by hand asks first. The other files only ask here
// (largeFileFeaturesOff, languageServerPath:) where they would otherwise
// turn a feature on.
#import "EditorController+Private.h"
#import "EditorController+LargeFileIvars.h"
#include "LargeFile.h"
#include <vector>

namespace {

// LargeFile::isLargeText over an NSString, a piece at a time (a 20 MB text
// is never copied whole to count its lines).
BOOL TextIsLarge(NSString *text) {
    const NSUInteger n = text.length;
    if (n > LargeFile::kSizeThreshold) return YES;
    if (n < LargeFile::kLineCountThreshold) return NO;
    const NSUInteger piece = 1 << 16;
    std::vector<unichar> buf(piece + 1);
    size_t lines = 1;
    for (NSUInteger at = 0; at < n; at += piece) {
        const NSUInteger len = MIN(piece, n - at);
        const NSUInteger read = MIN(piece + 1, n - at);   // and the unit after, for "\r\n"
        [text getCharacters:buf.data() range:NSMakeRange(at, read)];
        for (NSUInteger i = 0; i < len; i++) {
            const unichar c = buf[i];
            const unichar next = i + 1 < read ? buf[i + 1] : 0;
            if (c == '\n' || c == 0x2029 || (c == '\r' && next != '\n')) lines++;
        }
        if (lines > LargeFile::kLineCountThreshold) return YES;
    }
    return NO;
}

}  // namespace

@implementation EditorController (LargeFile)

// The tab on show was just read: whether it opens in large-file mode. The
// setting is read here, as VS Code reads its own when a file opens.
- (void)noteLargeFileText:(NSString *)text {
    if (!_activeTab) return;
    _activeTab.largeFile = [AppSettings shared].settings.largeFileOptimizations() &&
                           TextIsLarge(text ?: @"");
}

- (BOOL)largeFileFeaturesOff {
    return _activeTab.largeFile && !_activeTab.largeFileFeatures;
}

// Whether a language server may have the tab's text: not in large-file
// mode, and never past VS Code's 50 MB sync limit (didChange sends the
// whole text).
- (BOOL)languageServerMayRead:(MCTab *)tab {
    if (!tab) return NO;
    const NSUInteger length = tab == _activeTab && self.textView.editable
                                  ? self.textView.textStorage.length
                                  : tab.sourceText.length;
    return LargeFile::modeFor(length, tab.largeFile, true, tab.largeFileFeatures).languageServer;
}

- (NSString *)languageServerPath:(NSString *)path {
    return [self languageServerMayRead:_activeTab] ? path : nil;
}

// Opening a file over 50 MB by hand: Open Anyway or Cancel, as VS Code asks
// past its own limit. Only a file that would be read as text asks; images,
// PDFs, media, databases, large CSV tables and Quick Look's documents
// open as before.
- (BOOL)confirmOpeningLargeFile:(NSString *)path {
    struct stat st;
    if (!path || stat(path.fileSystemRepresentation, &st) != 0 || !S_ISREG(st.st_mode) ||
        (uint64_t)st.st_size <= LargeFile::kAskBeforeOpeningBytes)
        return YES;
    if ([EditorController isImagePath:path] || [EditorController isMediaPath:path] ||
        [path.pathExtension.lowercaseString isEqualToString:@"pdf"] ||
        MCViewerKindWithoutText(MCViewerKindForFile(path)))
        return YES;
    NSAlert *a = [NSAlert new];
    a.messageText = [NSString stringWithFormat:@"“%@” is %@. Open it anyway?",
                     path.lastPathComponent,
                     [NSByteCountFormatter stringFromByteCount:st.st_size
                                                    countStyle:NSByteCountFormatterCountStyleFile]];
    a.informativeText = @"A file this large takes a while to read and show. It opens "
                        @"without syntax colors, folding or a language server, which "
                        @"the status bar can turn on for it.";
    [a addButtonWithTitle:@"Open Anyway"];
    [a addButtonWithTitle:@"Cancel"];
    return [a runModal] == NSAlertFirstButtonReturn;
}

// "Large file: features off" in the status bar while such a tab is on show.
- (void)updateLargeFileNote {
    const BOOL show = [self largeFileFeaturesOff];
    if (!show && !_largeFileNote) return;
    if (!_largeFileNote && self.statusBar) {
        MCLspStatusField *note = [[MCLspStatusField alloc] initWithFrame:NSZeroRect];
        note.font = [NSFont systemFontOfSize:11];
        note.alignment = NSTextAlignmentRight;
        note.lineBreakMode = NSLineBreakByTruncatingTail;
        note.autoresizingMask = NSViewMinXMargin;
        note.textColor = self.lspLabel.textColor;
        __weak EditorController *weakSelf = self;
        note.onClick = ^{ [weakSelf turnOnLargeFileFeatures:nil]; };
        [note setStatus:@"Large file: features off" dot:nil];
        note.hidden = YES;
        _largeFileNote = note;
        [self.statusBar addSubview:note];
    }
    const BOOL changed = _largeFileNote.hidden == show;
    _largeFileNote.hidden = !show;
    if (show) {
        _largeFileNote.textColor = self.lspLabel.textColor;
        _largeFileNote.toolTip = [NSString stringWithFormat:
            @"“%@” is over 20 MB or 300,000 lines, so syntax colors, folding, bracket "
            @"matching and the language server are off for it, as in VS Code "
            @"(editor.large-file-optimizations). Click to turn them on for this tab.",
            self.currentPath.lastPathComponent ?: @""];
    }
    if (changed) [self layoutStatusRight];
}

// Placed left of the status bar's right-hand items, which end at `right`;
// returns where it starts.
- (CGFloat)layoutLargeFileNoteEndingAt:(CGFloat)right {
    if (!_largeFileNote || _largeFileNote.hidden) return right;
    const CGFloat w = ceil(_largeFileNote.attributedStringValue.size.width) + 6;
    _largeFileNote.frame = NSMakeRect(right - w, 3, w, 16);
    return right - w;
}

// The note's click: this tab gets everything a small file has. Lexing and
// laying out all of it takes as long as it takes; the user asked for it.
- (void)turnOnLargeFileFeatures:(id)sender {
    (void)sender;
    MCTab *t = _activeTab;
    if (!t.largeFile || t.largeFileFeatures) return;
    t.largeFileFeatures = YES;
    if (self.textView.editable && !self.showingMessage) {
        // Contiguous layout again, so folds and boxes have exact rects: the
        // lines above the top one are laid out now, and the place kept.
        NSTextView *tv = self.textView;
        const NSUInteger n = tv.string.length;
        const NSRect visible = tv.visibleRect;
        const NSUInteger top = n ? MIN([tv characterIndexForInsertionAtPoint:
                                           NSMakePoint(NSMinX(visible) + 20, NSMinY(visible) + 1)],
                                       n - 1)
                                 : 0;
        const NSRect before = n ? [self textRectForCharacter:top] : NSZeroRect;
        tv.layoutManager.allowsNonContiguousLayout = NO;
        if (n && !NSIsEmptyRect(before))
            [self scrollEditorToCharacter:top offset:NSMinY(before) - NSMinY(visible)];
        self.textView.folds.fileName = self.currentPath;
        [self applyHighlighting];
        [self updateBracketMatch];
    }
    [self.lsp documentOpened:self.previewMode ? nil : [self languageServerPath:self.currentPath]];
    [self updateLargeFileNote];
}

@end
