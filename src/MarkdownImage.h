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
// relative to `folder` (the Markdown file's own), an absolute or ~ path, or a
// file:// URL. Web images and anything that does not decode give nil, and the
// caller shows the alt text instead.
NSImage *MCMarkdownLoadImage(NSString *src, NSString *folder);
