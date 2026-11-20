// TypesetPages.mm — Objective-C++. See TypesetPages.h.
#import "TypesetPages.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <cmath>
#include <cstring>

namespace {

// A PDF's bytes as CoreGraphics's document (copied, so it outlives the
// caller's string), NULL when it does not read one with pages.
CGPDFDocumentRef MakeDocument(const std::string &pdf, CFDataRef *bytes) {
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)pdf.data(), (CFIndex)pdf.size());
    if (!data) return NULL;
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGPDFDocumentRef doc = provider ? CGPDFDocumentCreateWithProvider(provider) : NULL;
    if (provider) CGDataProviderRelease(provider);
    if (doc && (CGPDFDocumentIsEncrypted(doc) && !CGPDFDocumentIsUnlocked(doc))) {
        CGPDFDocumentRelease(doc);
        doc = NULL;
    }
    if (doc && CGPDFDocumentGetNumberOfPages(doc) == 0) {
        CGPDFDocumentRelease(doc);
        doc = NULL;
    }
    if (!doc) {
        CFRelease(data);
        return NULL;
    }
    *bytes = data;
    return doc;
}

// Each drawing thread's last document, kept while the same bytes come
// again (every page of a typeset is drawn from one PDF).
struct Kept {
    CFDataRef bytes = NULL;
    CGPDFDocumentRef doc = NULL;
    ~Kept() { clear(); }
    void clear() {
        if (doc) CGPDFDocumentRelease(doc);
        if (bytes) CFRelease(bytes);
        doc = NULL;
        bytes = NULL;
    }
    CGPDFDocumentRef documentFor(const std::string &pdf) {
        if (doc && bytes && (size_t)CFDataGetLength(bytes) == pdf.size() &&
            std::memcmp(CFDataGetBytePtr(bytes), pdf.data(), pdf.size()) == 0)
            return doc;
        clear();
        doc = MakeDocument(pdf, &bytes);
        return doc;
    }
};
thread_local Kept gKept;

// A page's size as it shows: its crop box, turned by its rotation.
CGSize ShownSize(CGPDFPageRef page) {
    const CGRect box = CGPDFPageGetBoxRect(page, kCGPDFCropBox);
    const int turn = ((CGPDFPageGetRotationAngle(page) % 360) + 360) % 360;
    return turn == 90 || turn == 270 ? CGSizeMake(box.size.height, box.size.width) : box.size;
}

}  // namespace

bool MCTypesetPageSizes(const std::string &pdf, std::vector<Shared::Web::PageSize> *sizes) {
    @autoreleasepool {
        CFDataRef bytes = NULL;
        CGPDFDocumentRef doc = MakeDocument(pdf, &bytes);
        if (!doc) return false;
        const size_t n = CGPDFDocumentGetNumberOfPages(doc);
        sizes->clear();
        bool ok = n <= Shared::Web::kMaxTypesetPages;
        for (size_t i = 1; ok && i <= n; i++) {
            CGPDFPageRef page = CGPDFDocumentGetPage(doc, i);
            const CGSize s = page ? ShownSize(page) : CGSizeZero;
            if (!(s.width > 0 && s.height > 0)) ok = false;
            else sizes->push_back(Shared::Web::PageSize{s.width, s.height});
        }
        CGPDFDocumentRelease(doc);
        CFRelease(bytes);
        return ok && !sizes->empty();
    }
}

bool MCTypesetPageRender(const std::string &pdf, int page, int width, std::string *png) {
    @autoreleasepool {
        CGPDFDocumentRef doc = gKept.documentFor(pdf);
        if (!doc || page < 0 || (size_t)page >= CGPDFDocumentGetNumberOfPages(doc) || width <= 0) return false;
        CGPDFPageRef p = CGPDFDocumentGetPage(doc, (size_t)page + 1);
        if (!p) return false;
        const CGSize shown = ShownSize(p);
        if (!(shown.width > 0 && shown.height > 0)) return false;
        const CGFloat scale = width / shown.width;
        const size_t height = (size_t)std::max<double>(1, std::ceil(shown.height * scale));
        if ((double)width * (double)height > Shared::Web::kMaxPagePixels * 1.01) return false;
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(NULL, (size_t)width, height, 8, 0, space,
                                                 (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
        CGColorSpaceRelease(space);
        if (!ctx) return false;
        CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
        CGContextFillRect(ctx, CGRectMake(0, 0, width, height));
        CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
        CGContextSetShouldAntialias(ctx, true);
        CGContextSetShouldSmoothFonts(ctx, false);
        // The page's own transform at its size (rotation and the box's
        // origin), then scaled up: the drawing transform never scales up.
        CGContextScaleCTM(ctx, scale, scale);
        CGContextConcatCTM(ctx, CGPDFPageGetDrawingTransform(p, kCGPDFCropBox,
                                                             CGRectMake(0, 0, shown.width, shown.height), 0, true));
        CGContextClipToRect(ctx, CGPDFPageGetBoxRect(p, kCGPDFCropBox));
        CGContextDrawPDFPage(ctx, p);
        CGImageRef image = CGBitmapContextCreateImage(ctx);
        CGContextRelease(ctx);
        if (!image) return false;
        CFMutableDataRef out = CFDataCreateMutable(kCFAllocatorDefault, 0);
        CGImageDestinationRef dest = out ? CGImageDestinationCreateWithData(out, CFSTR("public.png"), 1, NULL) : NULL;
        bool ok = false;
        if (dest) {
            CGImageDestinationAddImage(dest, image, NULL);
            ok = CGImageDestinationFinalize(dest);
            CFRelease(dest);
        }
        CGImageRelease(image);
        if (ok && out) png->assign((const char *)CFDataGetBytePtr(out), (size_t)CFDataGetLength(out));
        if (out) CFRelease(out);
        return ok && !png->empty();
    }
}
