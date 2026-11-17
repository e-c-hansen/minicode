// EditorController+LatexInputs.mm — EditorController: the files a LaTeX
// document reads (\input, \include), as its preview edits them. A
// double-click on such a file's text in the PDF opens the preview's edit
// box on that file's source (Latex.mm); Save puts the edit into the file's
// tab here, opened in the background when it was not open, unsaved and one
// undo step there, as an edit of the document itself goes into its buffer.
// tectonic reads those files from disk, so the preview's bar names the
// ones with unsaved changes; Cmd+S with the preview on show saves the
// document and every one of them, and any save of a file a preview read
// typesets that preview again.
#import "EditorController+Private.h"
#import "EditorTab.h"
#import "Latex.h"

@implementation EditorController (LatexInputs)

- (void)wireLatexInputsOf:(LatexView *)view {
    __weak EditorController *weakSelf = self;
    view.inputText = ^NSString *(NSString *path) {
        return [weakSelf latexInputText:path];
    };
    view.onInputEdited = ^BOOL(NSString *path, NSRange range, NSString *text) {
        return [weakSelf latexEditInput:path range:range text:text];
    };
    view.inputUnsaved = ^BOOL(NSString *path) {
        MCTab *t = [weakSelf tabForPath:path];
        return t.dirty;
    };
}

// The file's text as the user has it: its tab's buffer (the editor's while
// it shows the tab as text), else nil, which the preview reads from disk.
- (NSString *)latexInputText:(NSString *)path {
    MCTab *t = [self tabForPath:path];
    if (!t || !t.loaded) return nil;
    if (t == _activeTab && self.textView.editable && !t.showingMessage)
        return [self.textView.string copy];
    return t.sourceText;
}

// The file's tab with its text read, for an edit: the open one, or a new
// one beside the document's, not shown. nil when the file does not read
// as text, or its tab holds a backup not yet compared with the disk.
- (MCTab *)latexInputTab:(NSString *)path {
    MCTab *t = [self tabForPath:path];
    if (t && t.loaded) return (t.showingMessage || !t.sourceText) ? nil : t;
    if (t.restoredBackup) return nil;   // shown once, it settles against the disk
    MCDiskText *disk = MCReadDiskText(path, 16ull << 20);
    if (!disk.text) return nil;
    const BOOL fresh = t == nil;
    if (fresh) {
        t = [MCTab new];
        t.currentPath = path;
    }
    // As loadActiveTab reads a text file, shown as source when it is shown:
    // a part of a document does not typeset by itself.
    NSString *ext = path.pathExtension.lowercaseString;
    t.loaded = YES;
    t.gone = NO;
    t.currentExt = ext;
    t.isLatex = [ext isEqualToString:@"tex"] || [ext isEqualToString:@"ltx"] ||
                [ext isEqualToString:@"latex"];
    t.previewMode = NO;
    t.sourceText = disk.text;
    t.savedText = disk.text;
    t.bom = disk.bom;
    t.encoding = disk.encoding;
    t.dirty = NO;
    [self recordStamp:disk.stamp ofTab:t];
    if (fresh) [self insertTab:t];
    return t;
}

- (BOOL)latexEditInput:(NSString *)path range:(NSRange)range text:(NSString *)text {
    MCTab *t = [self latexInputTab:path];
    if (!t) return NO;
    NSString *now = [self latexInputText:path] ?: t.sourceText ?: @"";
    if (NSMaxRange(range) > now.length) return NO;
    NSString *edited = [now stringByReplacingCharactersInRange:range withString:text ?: @""];
    [self putBackText:edited inTab:t actionName:@"Edit in Preview"];
    if (t.preview) [self keepPreviewTab:t];   // edited: kept, as typing keeps it
    [self reloadTabStrip];
    [self saveSession];
    return YES;
}

// Cmd+S with the LaTeX preview on show: the files the last typeset read
// that have unsaved changes are saved with the document, so the page shows
// them.
- (void)saveLatexInputsOfActiveTab {
    if (!self.isLatex || !self.previewMode || !self.latex) return;
    for (NSString *path in [self.latex inputFiles]) {
        MCTab *t = [self tabForPath:path];
        if (t && t != _activeTab && t.dirty) [self saveTab:t];
    }
}

// A file was written: the previews whose last typeset read it typeset again
// (they read it from disk), and their bars drop it from the unsaved ones.
- (void)latexFileSaved:(MCTab *)saved {
    NSString *path = saved.currentPath;
    if (!path.length) return;
    for (MCTab *t in [self tabs])
        if (t != saved && t.latex && [t.latex typesetReadFile:path]) [t.latex compileNow];
}

@end
