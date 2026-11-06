// EditorController+HighlightingIvars.h — the instance variables only
// EditorController+Highlighting.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"

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
}
@end
