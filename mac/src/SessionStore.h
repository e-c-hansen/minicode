// SessionStore.h — the Mac's sessions.json (TabSessions::Store, the format
// the GTK port writes too): every folder's tabs, which a window opening on
// the folder takes back, and the windows a launch that opens nothing brings
// back. Stage 3c of SESSION-DESIGN.md moved both here from the user
// defaults (MCTabSessions and MCOpenWindows), whose records are carried over
// once, the first time a minicode without the file asks for it.
//
// The file is ~/Library/Application Support/MiniCode/sessions.json
// (NSHomeDirectory, so CFFIXED_USER_HOME moves it), or $MINICODE_SESSIONS_FILE.
// Every read and write runs on one serial queue: a change is handed over at
// once and written in the same turn of the queue (several changes queued
// together are one write), over the file as it is then, so another minicode's
// folders stay; a read waits for the writes queued before it, so it sees
// them, as the user defaults did.
#pragma once
#import <Foundation/Foundation.h>

#ifdef __cplusplus
#include <string>
#include <vector>

#include "TabSessions.h"

NSString *MCSessionsFilePath(void);
// The store as this process knows it: the file, with every change handed
// over before this call.
TabSessions::Store MCSessionsRead(void);
// A folder's session, written now (TabSessions::put: removed when it holds
// no tab and no browser tab; the 50 folders used last are kept).
void MCSessionsPutFolder(const std::string &key, const TabSessions::Session &s);
// The windows a launch brings back, front first.
void MCSessionsPutWindows(const std::vector<TabSessions::Window> &windows);
// Waits until every change handed over is in the file (quitting).
void MCSessionsFlush(void);

// What the user defaults of minicode 1.5.30 and before held, as the store:
// MCTabSessions (each folder's files, file on show, preview tab, pages to
// the side, split groups and browser pane) and MCOpenWindows (already the
// core's text). Exposed for the checks.
TabSessions::Store MCSessionsFromDefaults(NSDictionary *tabSessions, NSString *openWindows);

// A browser pane's state (BrowserView savedState: tabs with url, title,
// state as NSData and archived, and active) as a session's browser tabs,
// and back.
void MCSessionPutBrowserPage(NSDictionary *page, TabSessions::Session *s);
NSDictionary *MCSessionBrowserPage(const TabSessions::Session &s);
#endif
