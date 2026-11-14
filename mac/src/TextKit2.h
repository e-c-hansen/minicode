// TextKit2.h — whether a text view lays out with TextKit 2.
//
// The editor runs TextKit 1 (an NSTextView subclass that overrides drawRect:
// gets a layout manager), but the code asks first, because asking a TextKit 2
// view for its layoutManager switches it over. NSTextView has no
// textLayoutManager before macOS 12, so there every view is TextKit 1.
#pragma once
#import <Cocoa/Cocoa.h>

static inline BOOL MCUsesTextKit2(NSTextView *tv) {
    if (@available(macOS 12.0, *)) return tv.textLayoutManager != nil;
    return NO;
}
