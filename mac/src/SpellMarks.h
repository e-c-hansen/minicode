// SpellMarks.h — Objective-C++. Spell checking in the editor
// (editor.spell-check): the misspelled words of the lines on screen, from
// macOS's own checker, drawn as a dotted underline, with the checker's
// corrections, Learn Spelling, and Ignore Spelling in the right-click menu.
//
// The core (SpellCheck.h) decides which words are prose for the file's kind
// (Markdown and LaTeX text, comments, docstrings); only those go to
// NSSpellChecker, copied into a string of spaces as long as the lines, so
// its answer's ranges are the text's. NSTextView's own continuous checking
// stays off in the editor: it would mark code and lay out the whole text.
//
// Checks run for the lines on screen only, half a second after typing
// stops and as lines scroll into view, the words picked on a queue of their
// own and checked by the system off the main thread
// (requestCheckingOfString:). Typing only moves the marks after the edit
// and drops those it touches, so a keystroke costs what it did.
#pragma once
#import <Cocoa/Cocoa.h>
#include <vector>
#include "SyntaxHighlighter.h"

@class CodeTextView;

// The whole lines holding `range`, and the highlighter's tokens for them
// (offsets into the text) appended to *tokens when there are any.
typedef NSRange (^MCSpellLinesProvider)(NSRange range, std::vector<Token> *tokens);

@interface MCSpellMarks : NSObject
- (instancetype)initWithTextView:(CodeTextView *)textView;
// What the editor shows: a key for the document (its path; words ignored
// are ignored in that document only), its Spell::Kind (Kind::None checks
// nothing), the underline's color, and how to read its lines' tokens.
- (void)showDocument:(NSString *)key
                kind:(NSInteger)kind
               color:(NSColor *)color
               lines:(MCSpellLinesProvider)lines;
// The text view's characters changed (CodeTextView's storage observer).
- (void)textEdited:(NSTextStorage *)storage;
// The misspelled words, ascending.
@property(nonatomic, readonly) NSArray<NSValue *> *marks;
// Answers taken from the checker so far (for checks).
@property(nonatomic, readonly) NSUInteger checksDone;
// Draw the marks inside `dirty` (from the text view's drawRect:).
- (void)drawInRect:(NSRect)dirty;
// The right-click menu: the corrections, Learn Spelling, and Ignore
// Spelling for the misspelled word at a character, above the rest.
- (void)addItemsToMenu:(NSMenu *)menu atIndex:(NSUInteger)index;
// Those items (none when no word there is marked).
- (NSArray<NSMenuItem *> *)itemsAtIndex:(NSUInteger)index;
// Check the lines on screen now, without the pause after typing.
- (void)checkNow;
@end
