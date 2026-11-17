// HintsSearchBox.mm — the shortcut hints' search box (HintsSearchBox.h).
#import "HintsSearchBox.h"
#import <Carbon/Carbon.h>

NSAttributedStringKey const MCHintsMatchAttribute = @"MCHintsMatch";
NSAttributedStringKey const MCHintsDimmedAttribute = @"MCHintsDimmed";

@implementation MCHintsSearchBox {
    NSSearchField *_field;
    NSString *_last;
    __weak NSResponder *_previous;   // where the keyboard was before the box took it
}

- (instancetype)init {
    if (!(self = [super init])) return nil;
    _field = [[NSSearchField alloc] initWithFrame:NSMakeRect(0, 0, 200, 22)];
    _field.placeholderString = @"Type to search the shortcuts";
    _field.delegate = self;
    _field.target = self;
    _field.action = @selector(searched:);   // the clear button, and each change
    _field.sendsSearchStringImmediately = YES;
    _field.sendsWholeSearchString = NO;
    _field.focusRingType = NSFocusRingTypeNone;
    _field.cell.scrollable = YES;
    _field.cell.wraps = NO;
    _last = @"";
    return self;
}

- (NSSearchField *)field { return _field; }

- (NSString *)query {
    NSText *editor = _field.currentEditor;
    return (editor ? editor.string : _field.stringValue) ?: @"";
}

- (void)setFontSize:(CGFloat)size {
    NSFont *f = [NSFont systemFontOfSize:size];
    if (![_field.font isEqual:f]) _field.font = f;
}

- (CGFloat)height {
    const NSSize s = [_field.cell cellSizeForBounds:NSMakeRect(0, 0, 1000, 1000)];
    return ceil(MAX(22, s.height));
}

- (void)changed {
    NSString *q = [self query];
    if ([q isEqualToString:_last]) return;
    _last = [q copy];
    if (self.onChange) self.onChange(q);
}

- (void)searched:(id)sender { [self changed]; }
- (void)controlTextDidChange:(NSNotification *)note { [self changed]; }
// The keyboard left the box (a click elsewhere): where it came from no
// longer says where it goes back to.
- (void)controlTextDidEndEditing:(NSNotification *)note { _previous = nil; }

- (void)clear {
    NSText *editor = _field.currentEditor;
    if (editor) editor.string = @"";
    _field.stringValue = @"";
    [self changed];
}

// Esc: the box's text first, then the panel.
- (void)escape {
    if ([self query].length) [self clear];
    else if (self.onClose) self.onClose();
}

- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView
    doCommandBySelector:(SEL)sel {
    if (sel == @selector(cancelOperation:)) {
        [self escape];
        return YES;
    }
    // Return and Tab neither end the search nor move the keyboard on.
    return sel == @selector(insertNewline:) || sel == @selector(insertTab:) ||
           sel == @selector(insertBacktab:) || sel == @selector(insertLineBreak:);
}

- (BOOL)hasKeyboard {
    NSText *editor = _field.currentEditor;
    return editor && _field.window.firstResponder == editor;
}

- (BOOL)takeKeyDown:(NSEvent *)event {
    NSWindow *w = _field.window;
    if (!w || event.window != w || NSApp.modalWindow || w.attachedSheet) return NO;
    const NSEventModifierFlags flags =
        event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    // Menu keys and the commands' Control keys stay theirs (Shift+Cmd+H
    // closes the panel through its menu item).
    if (flags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) return NO;
    if (event.keyCode == 0x6E) return NO;   // the Menu key
    NSString *chars = event.charactersIgnoringModifiers;
    if (chars.length == 1) {
        const unichar c = [chars characterAtIndex:0];
        if ((c >= NSF1FunctionKey && c <= NSF35FunctionKey) || c == NSHelpFunctionKey) return NO;
    }
    NSText *editor = _field.currentEditor;
    const BOOL composing = [self hasKeyboard] && [editor isKindOfClass:[NSTextView class]] &&
                           ((NSTextView *)editor).hasMarkedText;
    if (event.keyCode == kVK_Escape && !composing) {
        [self escape];
        return YES;
    }
    if (![self hasKeyboard]) {
        NSResponder *fr = w.firstResponder;
        // Another field's editor: remember the field, which keeps its text.
        if ([fr isKindOfClass:[NSTextView class]] && ((NSTextView *)fr).isFieldEditor &&
            [((NSTextView *)fr).delegate isKindOfClass:[NSResponder class]])
            fr = (NSResponder *)((NSTextView *)fr).delegate;
        if (![w makeFirstResponder:_field]) return NO;
        _previous = fr == w ? nil : fr;
        editor = _field.currentEditor;
        // Taking the keyboard selects the text; the key goes after it.
        editor.selectedRange = NSMakeRange(editor.string.length, 0);
    }
    editor = _field.currentEditor;
    if (!editor) return NO;
    [editor keyDown:event];
    return YES;
}

- (BOOL)giveBackKeyboard {
    if (![self hasKeyboard]) return YES;
    NSWindow *w = _field.window;
    NSResponder *p = _previous;
    _previous = nil;
    if ([p isKindOfClass:[NSView class]] && ((NSView *)p).window == w && [w makeFirstResponder:p])
        return YES;
    [w makeFirstResponder:nil];
    return NO;
}

@end
