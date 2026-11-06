// EditorController+MarkdownIvars.h — the instance variables only the Markdown
// preview's two files use (EditorController+Markdown.mm and
// +MarkdownRender.mm), which import it, as does EditorController.mm, which
// lays them out. See the rule above the shared block in
// EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"

@interface EditorController () {
    // Rendered Markdown's headings, by GitHub's anchor spelling, for #links.
    NSDictionary<NSString *, NSNumber *> *_markdownAnchors;
    // Web pictures and formulas: the marks numbering their runs and the
    // re-render each queues (_awaitedWebImages, _awaitedMath are shared).
    NSUInteger _webImageMarks;             // numbers kMarkdownWebImage runs
    BOOL _webImageRenderQueued;
    NSMutableArray<NSArray *> *_mathWanted;
    NSUInteger _mathMarks;
    BOOL _mathRenderQueued;
    BOOL _mathOn;                          // this render typesets math
    NSString *_mathRunProblem;             // why a whole run failed, this render
    NSString *_tectonicProblem;            // why the last download failed
}
@end
