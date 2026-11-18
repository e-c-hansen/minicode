// UsageView.h — Objective-C++. The plan usage forecast (A13) in the Agents
// panel, under its heading: the 5-hour and weekly windows' usage now, when
// each reaches 100% at the current rate (with a range), the burn rate at
// API prices, and the usage this Mac's transcripts do not explain; then
// the action that turns the status line log on or off (StatusLineOffer.h).
//
// MCUsageModel is one per app (the plan is the user's, not a folder's): on
// a serial queue of its own it reads the transcripts incrementally
// (ClaudeUsage::Scanner, MCAgentModel's projects folder) and the status line
// log (MCUsageDir), works out the forecast and its lines
// (ClaudeUsage::describe), and posts MCUsageModelDidChangeNotification on
// the main thread. The Agents panel asks it to refresh when it reloads (its
// model's changes and its 30 s timer).
#import <Cocoa/Cocoa.h>

extern NSString *const MCUsageModelDidChangeNotification;

@interface MCUsageModel : NSObject
+ (instancetype)shared;
// The forecast as lines of prose, the fit's line last when there is one.
@property(nonatomic, readonly, copy) NSArray<NSString *> *lines;
@property(nonatomic, readonly) BOOL loaded;
// Reads what was added since the last look; at most one look at a time,
// and none within 5 s of the last unless forced.
- (void)refresh;
// Forced; `done` on the main thread once the lines are new (for the checks).
- (void)refreshThen:(void (^)(void))done;
@end

@interface MCUsageView : NSView
@property(nonatomic, readonly) NSButton *actionButton;   // log, or stop logging
@property(nonatomic, readonly, copy) NSString *forecastText;
// The fit's line, shown as the text's tooltip ("" before there is one).
@property(nonatomic, readonly, copy) NSString *fitText;
// The height changed (lines came or went): the panel lays out again.
@property(nonatomic, copy) void (^onHeightChange)(void);
- (CGFloat)heightForWidth:(CGFloat)width;
// The model refreshed, and the action's title for the settings as they are.
- (void)refresh;
- (void)applyTextColor:(NSColor *)color;
@end
