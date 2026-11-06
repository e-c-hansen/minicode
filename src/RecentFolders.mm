// RecentFolders.mm — see RecentFolders.h.
#import "RecentFolders.h"
#import "TreeDetails.h"   // MCAppDefaults

NSNotificationName const MCRecentFoldersDidChangeNotification =
    @"MCRecentFoldersDidChangeNotification";

static NSString *const kRecentFoldersKey = @"MCRecentFolders";
static const NSUInteger kMaxRecentFolders = 20;

static BOOL MCRecentFoldersOff(void) {
    NSDictionary *env = NSProcessInfo.processInfo.environment;
    return [env[@"MINICODE_DEMO"] length] || [env[@"MINICODE_BENCH"] length];
}

static NSArray<NSString *> *MCStoredFolders(void) {
    NSArray *list = [MCAppDefaults() arrayForKey:kRecentFoldersKey];
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (id p in list)
        if ([p isKindOfClass:[NSString class]] && [p length]) [out addObject:p];
    return out;
}

void MCNoteRecentFolder(NSString *path) {
    if (!path.length || MCRecentFoldersOff()) return;
    NSString *real = path.stringByResolvingSymlinksInPath;
    NSMutableArray<NSString *> *list = [MCStoredFolders() mutableCopy];
    if (list.count && [list[0].stringByResolvingSymlinksInPath isEqualToString:real]) return;
    for (NSInteger i = (NSInteger)list.count - 1; i >= 0; i--)
        if ([list[(NSUInteger)i].stringByResolvingSymlinksInPath isEqualToString:real])
            [list removeObjectAtIndex:(NSUInteger)i];
    [list insertObject:path atIndex:0];
    while (list.count > kMaxRecentFolders) [list removeLastObject];
    [MCAppDefaults() setObject:list forKey:kRecentFoldersKey];
    [[NSNotificationCenter defaultCenter] postNotificationName:MCRecentFoldersDidChangeNotification
                                                        object:nil];
}

NSArray<NSString *> *MCRecentFolders(void) {
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    NSFileManager *fm = [NSFileManager defaultManager];
    for (NSString *p in MCStoredFolders()) {
        BOOL dir = NO;
        if ([fm fileExistsAtPath:p isDirectory:&dir] && dir) [out addObject:p];
    }
    return out;
}

void MCClearRecentFolders(void) {
    [MCAppDefaults() removeObjectForKey:kRecentFoldersKey];
    [[NSNotificationCenter defaultCenter] postNotificationName:MCRecentFoldersDidChangeNotification
                                                        object:nil];
}

static void MCFillRecentFoldersMenu(NSMenu *menu) {
    [menu removeAllItems];
    NSArray<NSString *> *folders = MCRecentFolders();
    for (NSString *p in folders) {
        NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:p.stringByAbbreviatingWithTildeInPath
                                                    action:@selector(openRecentFolder:)
                                             keyEquivalent:@""];
        it.representedObject = p;
        [menu addItem:it];
    }
    if (folders.count) [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"Clear Recently Opened" action:@selector(clearRecentFolders:)
             keyEquivalent:@""];
}

void MCInstallRecentFoldersMenu(NSMenu *menu) {
    MCFillRecentFoldersMenu(menu);
    __weak NSMenu *weakMenu = menu;
    [[NSNotificationCenter defaultCenter]
        addObserverForName:MCRecentFoldersDidChangeNotification object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *note) {
                    (void)note;
                    NSMenu *m = weakMenu;
                    if (m) MCFillRecentFoldersMenu(m);
                }];
}
