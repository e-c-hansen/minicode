// Lsp+ServersIvars.h — the instance variables only Lsp+Servers.mm uses,
// which imports it, as does Lsp.mm, which lays them out. See the rule at
// the top of Lsp+Private.h.
#pragma once
#import "Lsp+Private.h"

@interface LspSession () {
    // Whether a folder configures Ruff, asked again after a file opens.
    NSMutableDictionary<NSString *, NSNumber *> *_ruffConfigured;
    BOOL _progressPending;    // a redraw for $/progress is queued
    // What the settings said last, so a change restarts only what it touches.
    NSString *_settingsSignature, *_pythonSetting;
}
@end
