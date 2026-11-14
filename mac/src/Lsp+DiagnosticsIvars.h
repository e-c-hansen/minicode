// Lsp+DiagnosticsIvars.h — the instance variables only Lsp+Diagnostics.mm
// uses, which imports it, as does Lsp.mm, which lays them out. See the rule
// at the top of Lsp+Private.h.
#pragma once
#import "Lsp+Private.h"

@class MCProblemFile;

@interface LspSession () {
    NSMutableDictionary<NSString *, MCProblemFile *> *_problemFiles;   // by canonical URI
    // F8: the mark the last step went to, in the marks it was one of.
    NSArray<MCDiagnosticMark *> *_stepMarks;
    long _stepIndex;
}
@end
