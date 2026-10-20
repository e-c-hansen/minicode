// Search.h — a project-wide text search panel. Objective-C++.
#import <Cocoa/Cocoa.h>

@interface SearchPanel : NSObject
- (instancetype)initWithRoot:(NSString *)root
                 openHandler:(void (^)(NSString *path, NSInteger line))handler;
// The TODO list: every TODO comment and open Markdown task under the folder
// (FolderSearch::findTodos), filtered by what is typed.
- (instancetype)initTodosWithRoot:(NSString *)root
                      openHandler:(void (^)(NSString *path, NSInteger line))handler;
- (void)show;
// Its window, which belongs to the editor window that made it: keys pressed
// in it are that editor window's menu targets.
@property(nonatomic, strong, readonly) NSWindow *window;
- (void)setScope:(NSString *)dir;   // point search at a specific folder
@end
