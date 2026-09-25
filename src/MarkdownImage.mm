// MarkdownImage.mm — see MarkdownImage.h.
#import "MarkdownImage.h"

@interface MCMarkdownImageView : NSTextAttachmentViewProvider
@end

static CGRect MCFitPicture(NSImage *picture, CGFloat width) {
    const NSSize s = picture.size;
    if (s.width <= 0 || s.height <= 0) return CGRectZero;
    const CGFloat w = MIN(s.width, MAX(width, 16.0));
    return CGRectMake(0, 0, w, round(s.height * w / s.width));
}

@implementation MCMarkdownImage

- (instancetype)initWithPicture:(NSImage *)picture {
    if ((self = [super initWithData:nil ofType:nil])) _picture = picture;
    return self;
}

// TextKit 1, if something ever switches the editor over.
- (NSRect)attachmentBoundsForTextContainer:(NSTextContainer *)textContainer
                      proposedLineFragment:(NSRect)lineFrag
                             glyphPosition:(NSPoint)position
                            characterIndex:(NSUInteger)charIndex {
    return MCFitPicture(self.picture, lineFrag.size.width - position.x);
}

- (NSImage *)imageForBounds:(NSRect)imageBounds
              textContainer:(NSTextContainer *)textContainer
             characterIndex:(NSUInteger)charIndex {
    return self.picture;
}

// There is deliberately no override of the TextKit 2 bounds method
// (attachmentBoundsForAttributes:location:...): NSTextAttachment's own asks
// the view provider, and overriding it means no view is ever made. The
// provider below does the sizing instead.
- (NSTextAttachmentViewProvider *)viewProviderForParentView:(NSView *)parentView
                                                   location:(id<NSTextLocation>)location
                                              textContainer:(NSTextContainer *)textContainer {
    MCMarkdownImageView *provider =
        [[MCMarkdownImageView alloc] initWithTextAttachment:self
                                                 parentView:parentView
                                          textLayoutManager:textContainer.textLayoutManager
                                                   location:location];
    provider.tracksTextAttachmentViewBounds = NO;
    return provider;
}

@end

@implementation MCMarkdownImageView

- (void)loadView {
    NSImageView *view = [[NSImageView alloc] init];
    view.image = ((MCMarkdownImage *)self.textAttachment).picture;
    view.imageScaling = NSImageScaleProportionallyUpOrDown;
    view.animates = YES;
    view.editable = NO;
    self.view = view;
}

- (CGRect)attachmentBoundsForAttributes:(NSDictionary<NSAttributedStringKey, id> *)attributes
                               location:(id<NSTextLocation>)location
                          textContainer:(NSTextContainer *)textContainer
                   proposedLineFragment:(CGRect)proposedLineFragment
                               position:(CGPoint)position {
    return MCFitPicture(((MCMarkdownImage *)self.textAttachment).picture,
                        proposedLineFragment.size.width - position.x);
}

@end

NSImage *MCMarkdownLoadImage(NSString *src, NSString *folder) {
    if (src.length == 0) return nil;
    NSString *path;
    if ([src hasPrefix:@"file://"]) {
        path = [NSURL URLWithString:src].path;
    } else if ([src containsString:@"://"] || [src hasPrefix:@"data:"]) {
        return nil;   // no network fetches from a preview
    } else {
        // Drop a query or fragment, then undo percent-encoding (%20).
        NSRange cut = [src rangeOfCharacterFromSet:
            [NSCharacterSet characterSetWithCharactersInString:@"?#"]];
        if (cut.location != NSNotFound) src = [src substringToIndex:cut.location];
        path = src.stringByRemovingPercentEncoding ?: src;
        path = path.stringByExpandingTildeInPath;
        if (!path.absolutePath && folder)
            path = [folder stringByAppendingPathComponent:path];
    }
    if (!path) return nil;
    NSDictionary *info = [[NSFileManager defaultManager] attributesOfItemAtPath:path
                                                                          error:nil];
    // Regular files only, and nothing absurd: a preview is not an image viewer.
    if (![info.fileType isEqualToString:NSFileTypeRegular] ||
        info.fileSize > 64ull * 1024 * 1024)
        return nil;
    NSImage *picture = [[NSImage alloc] initWithContentsOfFile:path];
    if (!picture) return nil;
    // Pixels, not points, as the image viewer does: a 144 dpi screenshot would
    // otherwise show at half its size.
    NSImageRep *rep = picture.representations.firstObject;
    if (rep.pixelsWide > 0 && rep.pixelsHigh > 0)
        picture.size = NSMakeSize(rep.pixelsWide, rep.pixelsHigh);
    return picture;
}
