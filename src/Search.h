// Search.h — a project-wide text search panel. Objective-C++.
#import <Cocoa/Cocoa.h>

@interface SearchPanel : NSObject
- (instancetype)initWithRoot:(NSString *)root
                 openHandler:(void (^)(NSString *path, NSInteger line))handler;
- (void)show;
- (void)setScope:(NSString *)dir;   // point search at a specific folder
@end
