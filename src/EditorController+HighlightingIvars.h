// EditorController+HighlightingIvars.h — the instance variables only
// EditorController+Highlighting.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"
#include "LspClient.h"   // Lsp::SemanticSpan
#include "BracketPairs.h"

@interface EditorController () {
    // Edited, not yet recolored: [start, end) as distances from the end of
    // the text (an edit before a range leaves them alone), by start
    // descending, so the edits of a multi-range edit (last first) append.
    std::vector<std::pair<NSUInteger, NSUInteger>> _pending;
    BOOL _flushScheduled;
    BOOL _highlightingSettingsFile;        // give color values swatches
    NSColor *_styleColors[kTokenStyleCount];   // by TokenStyle, from the settings
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

    // Bracket-pair colors (BracketPairs.h): on for this text (the setting,
    // and a highlighter), what each line does to the depth, kept edit by
    // edit, and the characters whose brackets show their current colors
    // (an edit that changes the depth below it takes the rest out; those
    // on screen are painted again at once, the others as they scroll in).
    BOOL _bracketsOn;
    BracketPairs::Depths _bracketDepths;
    NSMutableIndexSet *_bracketPainted;
    NSColor *_bracketColors[4];
    NSUInteger _bracketLinesPainted;   // lines repainted for their brackets alone (tests)

    // A page with text blocks went into the editor's storage (a Markdown
    // preview): displaySourceEditable resets the layout manager's block
    // table before source goes in (layoutHeldTextBlocks).
    BOOL _layoutHeldTextBlocks;
}
@end
