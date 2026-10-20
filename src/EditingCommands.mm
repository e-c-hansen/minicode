// EditingCommands.mm — see EditingCommands.h.
#import "EditingCommands.h"
#import <objc/runtime.h>
#include "AutoIndent.h"
#include "KeepSorted.h"
#include "MarkdownTasks.h"
#include <string>

namespace {

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

// One replacement as its own undo step, with the selection after it.
void Apply(NSTextView *tv, size_t start, size_t length, const std::u16string &replacement,
           size_t selStart, size_t selEnd, NSString *action) {
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
        [tv scrollRangeToVisible:sel];
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
        AutoIndent::Edit e = AutoIndent::newline(text, a, b, name);
        if (!e.changed) return NO;
        Apply(tv, e.replaceStart, e.replaceLength, e.replacement, e.selStart, e.selEnd, @"Typing");
        return YES;
    }
    AutoIndent::Edit e;
    NSString *action = @"Typing";
    if (command == @selector(insertTab:)) {
        e = AutoIndent::tab(U16(tv.string), a, b, name);
        if (e.changed && e.replacement.find(u'\n') != std::u16string::npos) action = @"Indent";
    } else if (command == @selector(insertBacktab:)) {
        e = AutoIndent::backtab(U16(tv.string), a, b, name);
        action = @"Outdent";
        if (!e.changed) return YES;   // Shift+Tab never leaves the editor
    } else if (command == @selector(deleteBackward:)) {
        // Only ever in leading spaces: look before copying the whole text.
        if (sel.length != 0 || a == 0 || [tv.string characterAtIndex:a - 1] != ' ') return NO;
        e = AutoIndent::backspace(U16(tv.string), a, b, name);
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
    AutoIndent::Edit e = AutoIndent::typed(U16(tv.string), caret, FileName(path));
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
    if (e.changed)
        Apply(tv, e.replaceStart, e.replaceLength, e.replacement, e.selStart, e.selEnd,
              @"Sort keep-sorted Blocks");
    return e.error && !e.message.empty() ? [NSString stringWithUTF8String:e.message.c_str()] : nil;
}
