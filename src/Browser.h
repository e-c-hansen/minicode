// Browser.h — an embedded WKWebView with a minimal chrome (URL bar + nav).
// WebKit is a macOS system framework, so this adds no third-party dependency.
#import <Cocoa/Cocoa.h>

// The editor window. The browser's page asks to be the first responder
// whenever it focuses one of its fields (an autofocus search box, a script),
// and this window turns that down unless the user asked: a click in the
// page, Tab from the toolbar, or a page loaded from the toolbar while the
// keyboard is still there; never while the browser is hidden.
@interface MCKeyboardWindow : NSWindow
@end

@interface BrowserView : NSView
- (instancetype)initWithHomeURL:(NSString *)url;
- (void)focusURLBar;
// Loads an address, or searches for words that are not one.
- (void)navigateToString:(NSString *)raw;
@end
