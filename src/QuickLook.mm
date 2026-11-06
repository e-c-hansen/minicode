// QuickLook.mm — see QuickLook.h.
#import "QuickLook.h"
#import "AppSettings.h"

@implementation MCQuickLookSlot {
    QLPreviewView *_preview;
    NSString *_path;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.layer.backgroundColor = [[AppSettings shared] background:Surface::Editor].CGColor;
    }
    return self;
}

- (QLPreviewView *)preview { return _preview; }
- (NSString *)path { return _path; }

- (void)showPath:(NSString *)path {
    if (!_preview) {
        // Made on first use: a window that never previews never loads Quick
        // Look's machinery. It lives as long as the slot, across files.
        _preview = [[QLPreviewView alloc] initWithFrame:self.bounds
                                                  style:QLPreviewViewStyleNormal];
        _preview.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        _preview.shouldCloseWithWindow = NO;
        [self addSubview:_preview];
    }
    _path = [path copy];
    NSURL *url = [NSURL fileURLWithPath:path];
    // The same file again (a reload after a change on disk) is shown afresh.
    if ([_preview.previewItem.previewItemURL isEqual:url]) [_preview refreshPreviewItem];
    else _preview.previewItem = url;
}

- (void)clear {
    _path = nil;
    _preview.previewItem = nil;
}

- (void)dealloc {
    [_preview close];
}

- (NSMenu *)menuForEvent:(NSEvent *)event {
    return nil;   // Quick Look's own (Open with...) comes from the preview
}
@end

// ---------------------------------------------------------------------------
@implementation MCQuickLookPanel {
    BOOL _shown;
    NSArray<NSURL *> *_items;
    QLPreviewPanel *_controlled;   // while we are its controller
}

+ (instancetype)shared {
    static MCQuickLookPanel *one;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ one = [MCQuickLookPanel new]; });
    return one;
}

- (BOOL)shown { return _shown; }
- (NSArray<NSURL *> *)items { return _items ?: @[]; }

- (void)toggleWithURLs:(NSArray<NSURL *> *)urls {
    if (_shown) { [self close]; return; }
    if (urls.count == 0) { NSBeep(); return; }
    _items = [urls copy];
    _shown = YES;
    QLPreviewPanel *panel = [QLPreviewPanel sharedPreviewPanel];
    [[NSNotificationCenter defaultCenter] removeObserver:self
                                                    name:NSWindowWillCloseNotification
                                                  object:panel];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(panelWillClose:)
                                                 name:NSWindowWillCloseNotification
                                               object:panel];
    // Ordered front, not made key: the tree keeps the keyboard, so Space and
    // Esc reach it and the arrows go on moving through the files. Ordering
    // it in asks the key window's responder chain for a controller.
    [panel orderFront:nil];
    if (_controlled == panel) {
        [panel reloadData];
        panel.currentPreviewItemIndex = 0;
    }
}

- (void)followURLs:(NSArray<NSURL *> *)urls {
    if (!_shown || urls.count == 0 || [urls isEqualToArray:_items]) return;
    _items = [urls copy];
    if (_controlled) {
        [_controlled reloadData];
        _controlled.currentPreviewItemIndex = 0;
    }
}

- (void)close {
    if (!_shown) return;
    _shown = NO;
    if ([QLPreviewPanel sharedPreviewPanelExists])
        [[QLPreviewPanel sharedPreviewPanel] orderOut:nil];
}

- (void)panelWillClose:(NSNotification *)note {
    _shown = NO;   // closed by its own button, or by Esc while it was key
}

- (BOOL)acceptsControl { return _shown; }

- (void)beginControl:(QLPreviewPanel *)panel {
    _controlled = panel;
    panel.dataSource = self;
    panel.delegate = self;
    [panel reloadData];
}

- (void)endControl:(QLPreviewPanel *)panel {
    if (_controlled == panel) _controlled = nil;
    if (panel.dataSource == self) panel.dataSource = nil;
    if (panel.delegate == self) panel.delegate = nil;
}

// ---- QLPreviewPanelDataSource
- (NSInteger)numberOfPreviewItemsInPreviewPanel:(QLPreviewPanel *)panel {
    return (NSInteger)self.items.count;
}

- (id<QLPreviewItem>)previewPanel:(QLPreviewPanel *)panel previewItemAtIndex:(NSInteger)index {
    NSArray<NSURL *> *items = self.items;
    return index >= 0 && index < (NSInteger)items.count ? items[(NSUInteger)index] : nil;
}

// ---- QLPreviewPanelDelegate: Space and Esc while the panel itself is key
// (after a click in it) close it too.
- (BOOL)previewPanel:(QLPreviewPanel *)panel handleEvent:(NSEvent *)event {
    if (event.type != NSEventTypeKeyDown) return NO;
    NSEventModifierFlags mods = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask &
        (NSEventModifierFlagCommand | NSEventModifierFlagOption | NSEventModifierFlagControl);
    if (mods == 0 && (event.keyCode == 49 || event.keyCode == 53)) {
        [self close];
        return YES;
    }
    return NO;
}
@end
