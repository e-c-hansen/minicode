// Browser.h — an embedded WKWebView with a minimal chrome (URL bar + nav).
// WebKit is a macOS system framework, so this adds no third-party dependency.
#import <Cocoa/Cocoa.h>

@interface BrowserView : NSView
- (instancetype)initWithHomeURL:(NSString *)url;
- (void)focusURLBar;
@end
