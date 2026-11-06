// MarkdownMermaid.h — Mermaid diagrams in the Markdown preview (macOS).
//
// A ```mermaid block is drawn by mermaid-cli's `mmdc` when it is installed
// (npm install -g @mermaid-js/mermaid-cli), as math is by tectonic: minicode
// bundles no Mermaid code (it has no third-party code at all) and draws no
// diagram itself. Each diagram is drawn in the background, one mmdc run at a
// time, as a transparent PNG at twice its size in the preview's theme, and
// kept in memory and under ~/Library/Caches/MiniCode/mermaid (by a key of
// its source and theme, MermaidDiagram.h), so a page opened again shows its
// diagrams at once. Until a diagram arrives, and for good when mmdc is not
// there, markdown.mermaid is off or the diagram does not parse, the block
// shows as its source in the code style (a failed one with mmdc's
// complaint as its tooltip), as every other port shows it.
#import <Cocoa/Cocoa.h>
#include "MarkdownParser.h"
#include <vector>

// mmdc: $MINICODE_MMDC, else found on PATH and the usual folders
// (MCFindProgram); nil when there is none.
NSString *MCMermaidPath(void);
// Where drawn diagrams are kept: $MINICODE_MERMAID_CACHE, else
// ~/Library/Caches/MiniCode/mermaid (at most 50 MB, oldest first out).
NSString *MCMermaidCacheDirectory(void);

// Marks the code a diagram stands in for while it is drawn (its value the
// diagram's key), so the preview keeps its place when the picture arrives.
extern NSAttributedStringKey const MCMarkdownMermaidAttribute;
// Whether the diagram with this key is drawn and ready to show.
BOOL MCMermaidIsReady(NSString *key);

// Posted on the main thread when an mmdc run ends; userInfo
// MCMermaidKeysKey is an NSSet of the keys it settled (drawn or failed).
extern NSNotificationName const MCMermaidDrawnNotification;
extern NSString *const MCMermaidKeysKey;

// One window's diagrams: what a render of its preview shows and waits for.
@interface MCMarkdownMermaid : NSObject
// A render of the preview begins: whether diagrams are drawn (the setting
// and mmdc), and in which theme.
- (void)beginPageDark:(BOOL)dark;
// At run `i`, the first of a ```mermaid block: the diagram's picture as a
// paragraph of its own (carrying the block's source line in `lineKey`, so
// a double-click edits the block), with *end set to the run after the
// block; else nil, and the block's runs show as code. *complaint is then
// mmdc's when the diagram failed, and *key the diagram's while it is drawn
// (the runs carry it as MCMarkdownMermaidAttribute).
- (NSAttributedString *)pictureForRuns:(const std::vector<MdRun> &)runs
                                    at:(size_t)i
                                   end:(size_t *)end
                               lineKey:(NSAttributedStringKey)lineKey
                             complaint:(NSString **)complaint
                                   key:(NSString **)key;
// The render is done: diagrams it is missing go to mmdc.
- (void)endPage;
// Called (on the main thread, once for diagrams arriving together) when a
// diagram the last page waited for is drawn.
@property(nonatomic, copy) void (^onArrived)(void);
// Waits for nothing more (another file, the window closed).
- (void)forget;
@end
