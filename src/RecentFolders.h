// RecentFolders.h — Objective-C++. The folders windows were opened on, most
// recent first, for File > Open Recent and Ctrl+R (VS Code's Open Recent,
// workbench.action.openRecent). Kept in the app's defaults (MCAppDefaults,
// key MCRecentFolders), 20 at most, by the path as the window spelled it;
// folders that are gone are left out when listed. Nothing is noted under
// MINICODE_DEMO or MINICODE_BENCH, as the tab sessions are not.
#import <Cocoa/Cocoa.h>

extern NSNotificationName const MCRecentFoldersDidChangeNotification;

// A window's folder, now the most recent.
void MCNoteRecentFolder(NSString *path);
// The folders, most recent first, that still exist.
NSArray<NSString *> *MCRecentFolders(void);
void MCClearRecentFolders(void);

// Fill File > Open Recent (made by main.mm): one item per folder sending
// openRecentFolder: with its path as the representedObject, then Clear
// Recently Opened (clearRecentFolders:). Kept current as folders are noted,
// so the palette lists them too ("File > Open Recent > ~/Code/x").
void MCInstallRecentFoldersMenu(NSMenu *menu);
