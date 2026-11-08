// EditorController+HighlightingIvars.h — the instance variables only
// EditorController+Highlighting.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"
#include "LspClient.h"   // Lsp::SemanticSpan

@interface EditorController () {
    // Edited, not yet recolored: [start, end) as distances from the end of
    // the text (an edit before a range leaves them alone), by start
    // descending, so the edits of a multi-range edit (last first) append.
    std::vector<std::pair<NSUInteger, NSUInteger>> _pending;
    BOOL _flushScheduled;
    BOOL _highlightingSettingsFile;        // give color values swatches
    NSColor *_styleColors[8];              // by TokenStyle, from the settings
    NSColor *_plainColor;
    NSArray<NSDictionary *> *_styleAttributes;   // attributesForStyle:, for _styleAttributesBase
    NSDictionary *_styleAttributesBase;

    // Semantic tokens (LspSession (Semantic)): the server's spans, current
    // while the storage's characters are what they were computed on (an
    // edit ends that until the next answer), and the characters painted
    // with them so far (the lines on screen, as they come into view).
    std::vector<Lsp::SemanticSpan> _semantic;
    BOOL _semanticCurrent;
    NSMutableIndexSet *_semanticPainted;
    BOOL _semanticPaintScheduled, _semanticObserving;
    NSColor *_semanticColors[kSemanticColorCount];   // by SemanticColor
}
@end
