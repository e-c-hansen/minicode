// MarkdownImage.h — pictures in the rendered Markdown preview.
#import <Cocoa/Cocoa.h>

// An image in rendered Markdown. It is sized to the width of the line it
// sits on, but never enlarged past the image's own size, and animated GIFs
// play. A cell draws it (see MCAnimatedImageCell in the .mm).
@interface MCMarkdownImage : NSTextAttachment
- (instancetype)initWithPicture:(NSImage *)picture;
@property (nonatomic, readonly) NSImage *picture;
@end

// The picture that `src`, as written in a Markdown file, names: a path
// relative to `folder` (the Markdown file's own), an absolute or ~ path, a
// file:// URL, or a base64 data: URI. Web addresses and anything that does
// not decode give nil (see MCMarkdownWebImage for the web).
NSImage *MCMarkdownLoadImage(NSString *src, NSString *folder);

// Pictures from the web. Only https addresses are fetched, never plain http,
// in the background: no cookies, nothing written to disk, a 20 MB cap and a
// timeout. What arrives is kept in memory for the whole app (about 64 MB),
// keyed by the address as written, and a failure is remembered for half a
// minute so a dead address is not asked for again on every render.
BOOL MCMarkdownIsWebImage(NSString *src);

// The picture at an https address when it has already arrived, as a new
// NSImage each time (an animated cell steps its frames on its own copy).
// Otherwise nil, and a fetch is started unless one is under way or the
// address failed recently. *pending says whether one is under way, in which
// case MCMarkdownWebImageNotification follows. Main thread only.
NSImage *MCMarkdownWebImage(NSString *src, BOOL *pending);

// Whether MCMarkdownWebImage would answer with a picture now, without
// starting anything.
BOOL MCMarkdownWebImageIsReady(NSString *src);

// Posted on the main thread when a fetch ends, whether or not it brought a
// picture. userInfo: MCMarkdownWebImageURLKey (the address as written),
// MCMarkdownWebImageOKKey (NSNumber, YES when there is a picture) and, when
// there is not, MCMarkdownWebImageErrorKey (why, for logs and tests).
extern NSNotificationName const MCMarkdownWebImageNotification;
extern NSString *const MCMarkdownWebImageURLKey;
extern NSString *const MCMarkdownWebImageOKKey;
extern NSString *const MCMarkdownWebImageErrorKey;
