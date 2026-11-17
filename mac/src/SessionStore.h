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
// This process's windows, front first, as the ones a launch brings back.
// Written over this process's own and the last run's; the windows another
// minicode running now (or quit since this one started) wrote stay
// (TabSessions::mergeWindows), so two at once never write each other's
// windows away.
void MCSessionsPutWindows(const std::vector<TabSessions::Window> &windows);
// The windows a launch with nothing to open brings back: the stored ones
// but those another running minicode holds open (TabSessions::toRestore).
std::vector<TabSessions::Window> MCSessionsWindowsToRestore(void);
// This process as the windows record names it, and whether a process it
// names runs now (pid and start time, as the backups' owners).
TabSessions::Process MCSessionsThisProcess(void);
bool MCSessionsIsRunning(long pid, double start, void *context);
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
