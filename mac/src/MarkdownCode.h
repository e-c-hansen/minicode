// MarkdownCode.h — syntax colors for fenced code blocks in the rendered
// Markdown preview (```python and the like), from SyntaxHighlighter, the
// editor's own highlighter. The Markdown renderer notes where each block's
// text landed and which language it named; this colors them afterwards.
#import <Cocoa/Cocoa.h>
#include <string>

@interface MCMarkdownCodeBlocks : NSObject
// A run of a fenced block with a language, appended at `range` of the page.
- (void)noteRun:(NSRange)range language:(const std::string &)lang;
// Colors every noted block in `page` with the syntax colors in use.
- (void)colorPage:(NSMutableAttributedString *)page;
@end
