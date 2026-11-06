// MarkdownCode.mm — see MarkdownCode.h.
#import "MarkdownCode.h"
#import "AppSettings.h"
#include "MarkdownParser.h"
#include "SyntaxHighlighter.h"
#include <vector>

namespace {
struct Block {
    NSRange range;
    std::string ext;
};
}  // namespace

@implementation MCMarkdownCodeBlocks {
    std::vector<Block> _blocks;
}

- (void)noteRun:(NSRange)range language:(const std::string &)lang {
    if (lang.empty() || range.length == 0) return;
    const std::string ext = MarkdownParser::extensionForLanguage(lang);
    if (ext.empty() || !SyntaxHighlighter::supports(ext)) return;
    // A block is its runs one after another: the next line joins it.
    if (!_blocks.empty() && _blocks.back().ext == ext &&
        NSMaxRange(_blocks.back().range) == range.location) {
        _blocks.back().range.length += range.length;
        return;
    }
    _blocks.push_back({range, ext});
}

- (void)colorPage:(NSMutableAttributedString *)page {
    if (_blocks.empty()) return;
    AppSettings *cfg = [AppSettings shared];
    [page beginEditing];
    for (const Block &b : _blocks) {
        if (NSMaxRange(b.range) > page.length) continue;
        NSString *code = [page.string substringWithRange:b.range];
        const char *utf8 = code.UTF8String;
        if (!utf8) continue;
        const std::string text = utf8;
        // Token offsets are bytes; walk them to UTF-16 in step.
        size_t byte = 0, unit = 0;
        auto toUnits = [&](size_t target) {
            while (byte < target && byte < text.size()) {
                const unsigned char c = (unsigned char)text[byte];
                if ((c & 0xC0) != 0x80) unit += c >= 0xF0 ? 2 : 1;
                byte++;
            }
            return unit;
        };
        for (const Token &t : SyntaxHighlighter::highlight(text, b.ext)) {
            if (t.style == TokenStyle::Plain) continue;
            const size_t from = toUnits(t.start);
            const size_t to = toUnits(t.start + t.length);
            if (to <= from) continue;
            [page addAttribute:NSForegroundColorAttributeName value:[cfg syntax:t.style]
                         range:NSMakeRange(b.range.location + from, to - from)];
        }
    }
    [page endEditing];
}
@end
