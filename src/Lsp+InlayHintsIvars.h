// Lsp+InlayHintsIvars.h — the instance variables only Lsp+InlayHints.mm
// uses, which imports it, as does Lsp.mm, which lays them out. See the rule
// at the top of Lsp+Private.h.
#pragma once
#import "Lsp+Private.h"

@interface LspSession () {
    // The textDocument/inlayHint request out (0: none) and its server.
    int _inlayRequest;
    __weak MCLspServer *_inlayServer;
    // The characters the hints shown (or asked for) are about: the lines on
    // show and 30 either side, as VS Code asks. Scrolling past them asks
    // again.
    NSRange _inlayWindow;
    NSUInteger _inlayRequests;   // sent, for the checks
    BOOL _inlaySetting;          // editor.inlay-hints as last seen
    __weak NSClipView *_inlayClip;   // whose scrolling is followed
}
@end
