// RecentFolders.h — Objective-C++. The folders windows were opened on, most
// recent first, for File > Open Recent and Ctrl+R (VS Code's Open Recent,
// workbench.action.openRecent). Kept in the app's defaults (MCAppDefaults,
// key MCRecentFolders), 20 at most, by the path as the window spelled it;
// folders that are gone are left out when listed. Nothing is noted under
// MINICODE_DEMO or MINICODE_BENCH, as the tab sessions are not.
//
// The same folders go to NSDocumentController's recent documents, which is
// the list the Dock shows when its icon is right-clicked, minicode running
// or not (Info.plist claims public.folder, so it takes folders; nothing else
// of NSDocumentController is used). A folder chosen there arrives as an odoc
// event at application:openURLs:. The Dock's own menu adds New Window and
// Open Folder… (MCDockMenu).
#import <Cocoa/Cocoa.h>

extern NSNotificationName const MCRecentFoldersDidChangeNotification;

// A window's folder, now the most recent.
void MCNoteRecentFolder(NSString *path);
// The folders, most recent first, that still exist.
NSArray<NSString *> *MCRecentFolders(void);
// Clears the Dock's list too.
void MCClearRecentFolders(void);
// At launch: when the Dock's list is empty (the first launch of a minicode
// that keeps one), give it the stored folders, oldest first.
void MCSeedDockRecentFolders(void);
// The Dock icon's own items: New Window (newWindow:) and Open Folder…
// (openFolderInNewWindow:), sent to target.
NSMenu *MCDockMenu(id target);
// Set by the GUI checks: the folder the Dock's Open Folder… takes, in place
// of the folder picker (defined in main.mm).
extern NSURL * (^MCDockFolderChooser)(void);

// Fill File > Open Recent (made by main.mm): one item per folder sending
// openRecentFolder: with its path as the representedObject, then Clear
// Recently Opened (clearRecentFolders:). Kept current as folders are noted,
// so the palette lists them too ("File > Open Recent > ~/Code/x").
void MCInstallRecentFoldersMenu(NSMenu *menu);
