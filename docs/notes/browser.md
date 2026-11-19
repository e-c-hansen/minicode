# The browser pane

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## The browser pane: sessions, tabs, bookmarks, the address bar, find

(Moved from CLAUDE.md's "The inline agent, Request a Change and restarting (Mac)"; the UI map's
"The page never takes the keyboard by itself" is in `docs/notes/ui.md`.)

- **The browser comes back**: the session entry's `browser` key (its `page`
  is `BrowserView savedState`: every tab's address, title and
  `WKWebView.interactionState`, about 2.6 KB with history and scroll, macOS
  12+, 2 MB a tab and 4 MB in all by `BrowserTabs::keptRecords`, and
  `active`; the tab on show again as the old single-page `url`/`state`, which
  `restoreState:` still takes; visibility; fraction), restored in
  `restoreBrowserFromSession` for a new window, never on Open Folder, with
  only the tab on show loading; `applicationWillTerminate` saves every
  window's session. Cookies persist in the default data store.
- **Browser tabs** (2026-11-08, round 7 O4, `BrowserTabs.{h,cpp}`,
  `browsertabs:*`, `make guitest SUITES=browser_tabs`): an `MCTabStrip` above
  the toolbar; placing, stepping, closing and dragging are `EditorTabs`'.
  Only the tab on show and the one used before it keep a web view
  (`BrowserTabs::livePages`, `kLivePages` 2); the rest are suspended to
  address, title and `interactionState` and load on show, so their WebKit
  processes end (eight 40 MB pages: about 450 MB of WebKit, 110 MB with
  two live). Keys reach the pane through the responder chain (it is in the
  chain of whatever in it has the keyboard): `closeFocused:` (the agent page
  when it shows, else the tab; the last tab calls `onEmpty`, which hides the
  pane), `selectNextTab:`/`selectPreviousTab:`/`selectTabNumber:` (with one
  tab passed on to the window's controller). Cmd+T is
  `mutible.browser.newTab`, a page key whose selector only the pane
  answers, so in a PDF, the editor or after a scroll in a page the key goes
  on to Go to Symbol in Workspace (now `NotPage`); View > New Browser Tab
  is `openBrowserTab:` from anywhere. A Command- or middle-clicked link
  opens a suspended tab behind (`decidePolicyForNavigationAction:`); a
  target=_blank or window.open page comes up beside its opener in the web
  view WebKit's configuration makes (`WKUIDelegate`). Harness traps: a
  Command key in a focused web view goes to the page and comes back to the
  menus a moment later (wait for it); WebKit gives a script's click no
  modifiers, so the Command-click check hands the delegate an action
  subclass.
- **Bookmarks and the browser window** (2026-11-12, `WebBookmark`,
  `Bookmarks.{h,mm}`, `EditorController+Bookmarks.mm`, `bookmarks:*`, `make
  guitest SUITES=bookmarks`): a bookmark is a `.url` file (the Internet
  Shortcut, plain text, the owner's choice over `.webloc`, which is read
  too, binary ones through Foundation). Only http and https addresses
  ever open from one (`WebBookmark::isWebAddress`); a file with another
  opens as text. `openFileAtPath:from:` hands a bookmark to the window's
  own browser pane before anything else (a preview open keeps the
  keyboard, a kept one gives it to the page), a tab already at the
  address is shown again (`BrowserView openAddresses:`), and the tree's
  Open as Text skips that once (`gOpenBookmarkAsText`). The browser window
  is the window rooted at `browser.bookmarks` (~/Bookmarks): Finder's
  bookmarks and `mutible https://...` go there (the launcher writes a
  `web` request file, `MCTakeOpenRequest` reads it), made with the folder
  when missing, the page over the editor's side while no file is open.
  Save Page as Bookmark is Cmd+D, a page key whose selector only the pane
  answers (`saveBookmarkFromPage:`; so Cmd+D after a scroll in a page is
  still the editor's Add Next Occurrence), File > Save Page as Bookmark
  from anywhere; in the browser window it saves to the tree's selected
  folder. A page is saved once: Save finds a bookmark of the page first
  (`MCBookmarkFileFor`, a walk of the folder with each file's address
  cached by size and date; `WebBookmark::sameAddress`) and only says where
  it is. The toolbar's star (`bookmarkButton`, filled while
  `bookmarkPath`, looked up off the main thread on every toolbar update)
  saves, or moves the bookmark to the Trash (`toggleBrowserBookmark:`);
  the book beside it and the menu bar's Bookmarks menu are the folder as a
  menu (`MCAttachBookmarksMenu`, filled in `menuNeedsUpdate:`, which the
  palette now calls before reading a menu), a bookmark loading in the tab
  on show (`BrowserView showAddress:inNewTab:`, Command for a new tab).
  Finder keeps sending .url files to the default browser unless the
  user picks mutible in Get Info (Info.plist only offers it, Alternate).
- **Bookmarks per workspace** (2026-11-18, the owner's decisions;
  `WebBookmark::places`, `lookupOrder`, `entries`, `mergedEntries`,
  `moveDestination`, `bookmarks:workspace-*`; the Mac's
  `MCSaveBookmarksFolder`, `MCSharedBookmarksFolder`,
  `MCWorkspaceBookmarkFileFor`, `MCMoveBookmarkToWorkspace`,
  `MCAttachWorkspaceBookmarksMenu`): a window on a folder saves in
  `<folder>/.mutible/bookmarks/` (made on the first save, no .gitignore:
  committing them is the point; the status bar says
  ".mutible/bookmarks"). The workspace is the window's folder
  (`rootPath`) whatever the tree's focus. `browser.bookmarks`
  (~/Bookmarks) is the shared folder: the menu bar's Bookmarks menu (root
  asked of the key window's controller each time it opens) and the
  toolbar's book (root from `folderForAddresses`) list the workspace's
  rows, then a "Shared" submenu when the shared folder has any; each
  shared folder's submenu ends with "Move to This Workspace", a submenu of
  its bookmarks (`moveBookmarkToWorkspace:`, the BrowserView passing it on
  to the controller), which moves the file to the workspace's top level
  under a free name. "Already saved" and the star look in the workspace,
  then Shared ("Already bookmarked in Shared as ..."). The footer's Show
  Bookmarks Folder opens `.mutible/bookmarks` in Finder
  (`showWorkspaceBookmarksFolder:` on AppDelegate, made first) and Show
  Shared Bookmarks Folder is the old `showBookmarksFolder:` (the browser
  window). The browser window (its root equal to the shared folder,
  however spelled: `MCPlaces` resolves both), no window, and Finder's and
  `mutible https://` opens are unchanged: the shared folder alone, saving
  to the tree's folder, no Shared part. A root inside the shared folder is
  an ordinary workspace. The tree hides `.mutible` with the other dot
  names until Show Hidden Files; a save or move refreshes the tree but
  selects the file only when the tree shows it, never widening the focus
  (`revealPath:andOpen:NO` widens the tree's focus and, for a hidden
  path, selects the folder where the way ends).
  The session web page shows no bookmark list, so it has nothing to
  follow. **Linux** (2026-11-18, `linux/HANDOFF.md` item 111, the
  `bookmark_workspace` suite) does the same with the same core calls:
  `Bookmarks::places` (the root taken as the shared folder when
  `sameFolder` says so), `Bookmarks::menu(Places)` (the workspace's rows,
  "No bookmarks" when none, then a "Shared" submenu whose folder and
  each folder in it end with a "Move to This Workspace" submenu,
  `win.movebookmark`), `Bookmarks::fileFor(Places, url)` and
  `Bookmarks::moveToWorkspace` (GIO's move without overwrite, the next
  free name on a clash). GtkPopoverMenu names a submenu's page by its
  label and opens the first of two alike, so Shared > Reading > Move to
  This Workspace showed Shared's own list: a label met again in one menu
  gets zero-width spaces after it. The menu bar's Bookmarks menu is one GMenu for
  every window, so it is filled for GTK's active window and again when
  another comes to the front; the palette's rows are "Bookmarks > Name"
  and "Bookmarks > Shared > Name". It never had the Mac's Show Bookmarks
  Folder footer, and has none now. The tree already hid every dot name
  until Show Hidden Files (Ctrl+Shift+.), and a save selects nothing in
  it.
- **The address bar** (2026-11-15, `PageCheck::typedAddress`,
  `page:typed-address`, the `browser_tabs` suite): one line
  (`usesSingleLineMode`, not wrapping, truncated in the middle while not
  edited; a field made with `init` wraps, and a long file:// address went
  onto a second line, cut off). `loadString:` asks the core: a file name or
  path (`index.html`, `docs/a.html`, `./x`, `~/x.html`, `/abs`, a
  `#fragment` kept) found from the window's folder (`folderForAddresses`,
  the controller's rootPath) loads with `loadFileURL:allowingReadAccessToURL:`
  scoped to that folder (the file's own folder when outside it), as does a
  typed file:// address; one plainly a file (a path form, a folder before
  a slash, an ending such as .html that is no domain's) and missing is
  said over the bar's right end (`addressNotice`, 5 s or until edited)
  and nothing loads; host:port goes to http; else a name with a dot to
  https and the rest to a search, as before.
- **Find in the page** (2026-11-09, `make guitest SUITES=browser_find`):
  the terminal's `MCTerminalFindBar` at the page's top right, Edit > Find's
  `performTextFinderAction:` answered by the pane in the responder chain
  (`performKeyEquivalent:` with the keyboard in the address bar, whose field
  editor answers the menu's Find items and refuses them). WebKit finds
  (`findString:withConfiguration:`) and selects; the count is a script in
  the client world over `innerText`, so it is the visible text's, and no
  "2 of 5" (WebKit does not say which match it is on).

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- The browser pane: sessions, tabs, bookmarks, the address bar, find: Harness traps: a Command key in a focused web view goes to the page and comes back to the menus a moment later (wait for it);
- Bookmarks per workspace: `revealPath:andOpen:NO` widens the tree's focus and, for a hidden `.mutible` path, selects the folder where the way ends; a saved bookmark is revealed only when the tree shows it, with `widen:NO`.
- Bookmarks per workspace (Linux): GtkPopoverMenu names a submenu's page by its label and opens the first of two alike, so Shared > Reading > Move to This Workspace showed Shared's own list: a label met again in one menu gets zero-width spaces after it.
