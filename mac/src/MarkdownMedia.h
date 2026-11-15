// MarkdownMedia.h — video and audio in the rendered Markdown preview.
//
// A <video>, an <audio>, or a picture whose address is a video or a sound
// (MdRun::media) is an attachment in the page: a video shows its first
// frame at its own size (scaled down to the line, never up) with a play
// button over it, audio a slim bar with its name. A click puts the
// system's player (AVPlayerView, with its controls) over it, as a subview
// of the page's text view placed by hand (the TextKit 1 editor never asks
// for a TextKit 2 view provider), and it plays there. It pauses when
// scrolled out of view, and stops, letting go of the player, when the tab
// changes or the window closes (MCMarkdownMediaStopAll) or when the page
// it is on is replaced (the attachment goes, and the player with it).
#import <Cocoa/Cocoa.h>

@class AVPlayerView;

// What the page knows of a media address before it plays: whether this
// Mac can play it, a video's size (its rotation applied) and first frame.
// Worked out in the background, once per address for the whole app.
@interface MCMediaInfo : NSObject
@property(nonatomic, readonly) BOOL playable;
@property(nonatomic, readonly) NSSize size;             // zero for audio
@property(nonatomic, readonly, strong) NSImage *poster;  // a video's first frame
@property(nonatomic, readonly, copy) NSString *problem;  // why it cannot play
@end

// The address `src` (as written in a Markdown file in `folder`) as a URL:
// a path relative to the folder, an absolute or ~ path, a file:// URL, or
// an https address when `web` is YES (markdown.web-images); nil for
// anything else (plain http, data:, another scheme) or a missing file.
NSURL *MCMarkdownMediaURL(NSString *src, NSString *folder, BOOL web);

// The info for `url` when it has been worked out, else nil, and the work
// started unless it is under way. MCMarkdownMediaNotification follows,
// with MCMarkdownMediaURLKey the URL's absoluteString. Main thread only.
MCMediaInfo *MCMarkdownMediaInfo(NSURL *url);
extern NSNotificationName const MCMarkdownMediaNotification;
extern NSString *const MCMarkdownMediaURLKey;

// A video or audio in the page.
@interface MCMarkdownMedia : NSTextAttachment
// `size` in points (0 for a side not given) and `widthPercent` of the line
// as for a picture (MCMarkdownImage); a video without either is as big as
// its frames, scaled down to the line.
- (instancetype)initWithURL:(NSURL *)url
                      video:(BOOL)video
                       name:(NSString *)name
                       size:(NSSize)size
               widthPercent:(CGFloat)widthPercent;
@property(nonatomic, readonly) NSURL *url;
@property(nonatomic, readonly) BOOL video;
// A <video poster>, drawn in place of the first frame when set.
@property(nonatomic, strong) NSImage *poster;
// The player over it, while there is one.
@property(nonatomic, readonly) AVPlayerView *playerView;
// What a click does: the player over `frame` (the attachment's place in
// `view`), playing.
- (void)playInView:(NSView *)view frame:(NSRect)frame;
// Pauses it and lets the player go.
- (void)stop;
@end

// Stops every page player in `window`, or in every window for nil.
void MCMarkdownMediaStopAll(NSWindow *window);
