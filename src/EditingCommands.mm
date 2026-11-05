// EditingCommands.mm — see EditingCommands.h.
#import "EditingCommands.h"
#import <objc/runtime.h>
#include "AutoIndent.h"
#include "KeepSorted.h"
#include "MarkdownTasks.h"
#include <string>

// AutoIndent's scan cache for one text storage, told about every change to
// its characters (all of them reach NSTextStorageDidProcessEditing: typing,
// undo, a formatter, a tab switch replacing the whole text).
@interface MCIndentCache : NSObject {
@public
    AutoIndent::ScanCache _cache;
}
@end

@implementation MCIndentCache
- (instancetype)initWithStorage:(NSTextStorage *)storage {
    if ((self = [super init]))
        [[NSNotificationCenter defaultCenter] addObserver:self
                                                 selector:@selector(storageEdited:)
                                                     name:NSTextStorageDidProcessEditingNotification
                                                   object:storage];
    return self;
}
- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}
- (void)storageEdited:(NSNotification *)note {
    NSTextStorage *storage = note.object;
    if (storage.editedMask & NSTextStorageEditedCharacters) _cache.edited(storage.editedRange.location);
}
@end

namespace {

const void *const kIndentCacheKey = &kIndentCacheKey;

AutoIndent::ScanCache *CacheFor(NSTextView *tv) {
    NSTextStorage *storage = tv.textStorage;
    if (!storage) return nullptr;
    MCIndentCache *c = objc_getAssociatedObject(storage, kIndentCacheKey);
    if (!c) {
        c = [[MCIndentCache alloc] initWithStorage:storage];
        objc_setAssociatedObject(storage, kIndentCacheKey, c, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return &c->_cache;
}

std::u16string U16(NSString *s) {
    std::u16string u(s.length, u'\0');
    [s getCharacters:(unichar *)u.data() range:NSMakeRange(0, s.length)];
    return u;
}

NSString *FromU16(const std::u16string &u) {
    return [NSString stringWithCharacters:(const unichar *)u.data() length:u.size()];
}

std::string FileName(NSString *path) {
    const char *c = path.UTF8String;
    return c ? c : "";
}

// Set while Apply edits, so its own replacement never counts as typing.
bool gApplying = false;

// One replacement as its own undo step, with the selection after it, which
// is scrolled into view unless `scroll` is off.
void Apply(NSTextView *tv, size_t start, size_t length, const std::u16string &replacement,
           size_t selStart, size_t selEnd, NSString *action, bool scroll = true) {
    NSRange range = NSMakeRange(start, length);
    NSString *text = FromU16(replacement);
    gApplying = true;
    [tv breakUndoCoalescing];
    if ([tv shouldChangeTextInRange:range replacementString:text]) {
        [tv.textStorage replaceCharactersInRange:range withString:text];
        [tv didChangeText];
        [tv breakUndoCoalescing];
        if (action) [tv.undoManager setActionName:action];
        NSRange sel = NSMakeRange(selStart, selEnd - selStart);
        [tv setSelectedRange:sel];
        if (scroll) [tv scrollRangeToVisible:sel];
    }
    gApplying = false;
}

const void *const kElectricKey = &kElectricKey;

NSString *Message(const KeepSorted::Edit &e) {
    if (!e.message.empty()) return [NSString stringWithUTF8String:e.message.c_str()];
    return e.changed ? nil : @"Already sorted";
}

NSString *ApplyKeepSorted(NSTextView *tv, const KeepSorted::Edit &e, NSString *action) {
    if (e.changed)
        Apply(tv, e.replaceStart, e.replaceLength, e.replacement, e.selStart, e.selEnd, action);
    else if (e.error)
        NSBeep();
    return Message(e);
}

}  // namespace

BOOL MCIndentCommand(NSTextView *tv, SEL command, NSString *path, BOOL markdown) {
    if (!tv.editable || tv.hasMarkedText) return NO;
    const std::string name = FileName(path);
    const NSRange sel = tv.selectedRange;
    const size_t a = sel.location, b = NSMaxRange(sel);
    if (command == @selector(insertNewline:)) {
        const std::u16string text = U16(tv.string);
        // In a Markdown list, Return starts the next item, or ends the list
        // on an empty one.
        if (markdown) {
            MarkdownTasks::Edit e = MarkdownTasks::newline(text, a, b);
            if (e.changed) {
                Apply(tv, e.replaceStart, e.replaceLength, e.replacement, e.selStart, e.selEnd,
                      @"Typing");
                return YES;
            }
        }
        AutoIndent::ScanCache *cache = CacheFor(tv);
        AutoIndent::Edit e =
            AutoIndent::newline(text, a, b, name, AutoIndent::detect(text, name, cache), cache);
        if (!e.changed) return NO;
        Apply(tv, e.replaceStart, e.replaceLength, e.replacement, e.selStart, e.selEnd, @"Typing");
        return YES;
    }
    AutoIndent::Edit e;
    NSString *action = @"Typing";
    // The file's indentation unit, kept until an edit within the lines
    // detect reads.
    auto unit = [&](const std::u16string &text) {
        return AutoIndent::detect(text, name, CacheFor(tv));
    };
    if (command == @selector(insertTab:)) {
        const std::u16string text = U16(tv.string);
        e = AutoIndent::tab(text, a, b, name, unit(text));
        if (e.changed && e.replacement.find(u'\n') != std::u16string::npos) action = @"Indent";
    } else if (command == @selector(insertBacktab:)) {
        const std::u16string text = U16(tv.string);
        e = AutoIndent::backtab(text, a, b, name, unit(text));
        action = @"Outdent";
        if (!e.changed) return YES;   // Shift+Tab never leaves the editor
    } else if (command == @selector(deleteBackward:)) {
        // Only ever in leading spaces: look before copying the whole text.
        if (sel.length != 0 || a == 0 || [tv.string characterAtIndex:a - 1] != ' ') return NO;
        const std::u16string text = U16(tv.string);
        e = AutoIndent::backspace(text, a, b, name, unit(text));
    } else {
        return NO;
    }
    if (!e.changed) return NO;
    Apply(tv, e.replaceStart, e.replaceLength, e.replacement, e.selStart, e.selEnd, action);
    return YES;
}

void MCIndentWillChange(NSTextView *tv, NSRange range, NSString *replacement) {
    if (gApplying || !tv.editable || replacement.length != 1 || tv.undoManager.isUndoing ||
        tv.undoManager.isRedoing)
        return;
    unichar c = [replacement characterAtIndex:0];
    if (c != ':' && c != ')' && c != ']' && c != '}') return;
    objc_setAssociatedObject(tv, kElectricKey, @(range.location + 1),
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

void MCIndentDidChange(NSTextView *tv, NSString *path) {
    NSNumber *at = objc_getAssociatedObject(tv, kElectricKey);
    if (!at) return;
    objc_setAssociatedObject(tv, kElectricKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    const NSUInteger caret = at.unsignedIntegerValue;
    const NSRange sel = tv.selectedRange;
    if (gApplying || sel.length != 0 || sel.location != caret || caret > tv.string.length ||
        tv.hasMarkedText)
        return;
    // Only a closer first on its line, or a colon on a line starting with
    // one of Python's dedenting words, can move it: look at the line before
    // copying the whole text.
    NSString *s = tv.string;
    const unichar ch = [s characterAtIndex:caret - 1];
    const NSUInteger ls = [s lineRangeForRange:NSMakeRange(caret - 1, 0)].location;
    NSString *lead = [[s substringWithRange:NSMakeRange(ls, caret - 1 - ls)]
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if (ch == ':') {
        BOOL dedents = NO;
        for (NSString *w in @[ @"else", @"elif", @"except", @"finally", @"case" ])
            dedents = dedents || [lead hasPrefix:w];
        if (!dedents) return;
    } else if (lead.length) {
        return;
    }
    const std::string name = FileName(path);
    const std::u16string text = U16(s);
    AutoIndent::ScanCache *cache = CacheFor(tv);
    AutoIndent::Edit e =
        AutoIndent::typed(text, caret, name, AutoIndent::detect(text, name, cache), cache);
    if (!e.changed) return;
    Apply(tv, e.replaceStart, e.replaceLength, e.replacement, e.selStart, e.selEnd, @"Indent");
}

NSString *MCSortLines(NSTextView *tv) {
    if (!tv.editable) {
        NSBeep();
        return nil;
    }
    const NSRange sel = tv.selectedRange;
    return ApplyKeepSorted(
        tv, KeepSorted::sortCommand(U16(tv.string), sel.location, NSMaxRange(sel)), @"Sort Lines");
}

NSString *MCFixKeepSorted(NSTextView *tv) {
    if (!tv.editable) {
        NSBeep();
        return nil;
    }
    const NSRange sel = tv.selectedRange;
    return ApplyKeepSorted(tv,
                           KeepSorted::fixCommand(U16(tv.string), sel.location, NSMaxRange(sel)),
                           @"Sort keep-sorted Blocks");
}

NSString *MCKeepSortedBeforeSave(NSTextView *tv) {
    if (!tv.editable) return nil;
    const std::u16string text = U16(tv.string);
    if (!KeepSorted::hasBlocks(text)) return nil;
    const NSRange sel = tv.selectedRange;
    KeepSorted::Edit e = KeepSorted::fixCommand(text, sel.location, NSMaxRange(sel));
    if (e.changed) {
        // Saving never moves the view: the lines on screen stay where they
        // are, wherever the caret went.
        NSClipView *clip = tv.enclosingScrollView.contentView;
        const NSPoint origin = clip.bounds.origin;
        Apply(tv, e.replaceStart, e.replaceLength, e.replacement, e.selStart, e.selEnd,
              @"Sort keep-sorted Blocks", false);
        if (clip) {
            [clip scrollToPoint:[clip constrainBoundsRect:(NSRect){origin, clip.bounds.size}].origin];
            [tv.enclosingScrollView reflectScrolledClipView:clip];
        }
    }
    return e.error && !e.message.empty() ? [NSString stringWithUTF8String:e.message.c_str()] : nil;
}

NSString *MCKeepSortedText(NSString *text, NSString **problem) {
    const std::u16string u = U16(text ?: @"");
    if (!KeepSorted::hasBlocks(u)) return nil;
    KeepSorted::Edit e = KeepSorted::fixCommand(u, 0, 0);
    if (problem && e.error && !e.message.empty())
        *problem = [NSString stringWithUTF8String:e.message.c_str()];
    if (!e.changed) return nil;
    std::u16string out = u;
    out.replace(e.replaceStart, e.replaceLength, e.replacement);
    return FromU16(out);
}
