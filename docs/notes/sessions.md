# Tabs, sessions, backups and the disk

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## Tabs, sessions and the disk (Mac)

- **The tabs as data** (2026-11-16, SESSION-DESIGN.md stage 3a,
  `EditorTabs.h`, `session:*`): `Model::Window` holds a window's tabs by
  the rules below: one or two groups (positional, 0 the left; the right
  one only while split), each group's tabs in order and its tab on show,
  the focused group, the share, each document's file, preview state
  (`Preview::User`, `Agent`) and last use, once however many groups show
  it, and the closed tabs. Documents are the port's ids (`DocId`); the
  window reads nothing and shows nothing: a port changes it
  (`prepareOpen`, `add`, `activate`, `close`, `putInOther`, `focus`,
  `move`, ...) and shows what it says. `Model::Session` is the app's
  windows. Both editors keep their tabs in one (GTK: linux/HANDOFF.md
  item 73; the Mac since stage 3c: `_win`, one of a process-wide
  `Model::Session` made in EditorController.mm, with `_docs` the MCTabs
  by `docId`). A rule changed here is changed in the window, with its
  `session:*` check. Since stage 3d (2026-11-17) both ports run the
  same rules, the Mac's: a new tab goes in through `add`
  (`groupAddTab:preview:passOver:` on the Mac, then
  `letGoOfReplacedTab:` once the new one shows), each group has its own
  preview tab and a replaced one the other group also shows stays there,
  and the agents have two preview tabs (`Preview::Agent` for notes,
  `AgentDiff` for comparisons), a new one in the old one's place.
- **One text view, many documents.** `MCTab` holds a document's path, text,
  `savedText`, mode flags, `dirty`, its own `NSUndoManager` (from
  `undoManagerForTextView:`), its own `LatexView`, folds, viewer kind,
  `comparison` and where its view was. Its place among the tabs is the
  window's: `preview`, `agentPreview` and `lastActive` are read-only and
  ask its `owner` (`MCTabOwner`, EditorController+Groups.mm), which
  answers from `_win`; `setCurrentPath:` tells it the new path. The
  controller's
  document properties are forwarding accessors to `_activeTab`
  (`MC_TAB_PROPERTY`), nil/NO with no tab. Shared viewers are let go by
  `stashActiveTab` (a playing sound stops) and rebuilt by `showActiveTab`. A
  session's tab is read when first shown (`loaded`). Diffs and commits share
  one tab.
- `EditorTabs` rules: a new tab right of the active one, the most recently used
  after closing the active one, wrap on next and previous, labels that add
  folders when names clash. `TabStrip` only draws and reports clicks.
  **Widths and scrolling** (2026-11-18, VS Code's tabSizing "fit",
  `tabs:strip-widths`, `tabs:strip-scroll`, `tabs:fit-title`,
  `tabs:names`, `preview_tabs`): a tab keeps its natural width (80 to
  320 points, `EditorTabs::layoutStrip`) and is never squeezed; a longer
  row scrolls by trackpad or wheel, vertical too (`wheelScroll`), with a
  thin bar while the pointer is over it and fades at the cut-off ends
  (`clipped`), and a dragged tab held near an end scrolls the row
  (`dragAutoScroll`, a 60 Hz timer). The tab on show is revealed
  (`revealScroll`) only when it changes: a new active index, a row of
  another length, or its title or tooltip changed (a preview replaced),
  and on a new width; a dirty dot leaves the row where the user scrolled
  it. A title is cut in the middle only past the widest tab
  (`fitMiddle`, `fitName`; the strip cuts it, the paragraph style only
  clips). The editor's tabs carry the name and, for same-named files, a
  hint (`EditorTabs::tabNames`, `Model::Window::tabNames`: `labels`'
  extra folders) drawn dimmed a point smaller after it
  (`MCTabStripItem.hint`); the terminals' and the browser's strips are
  the same class and scroll the same way. `displayedTitleForTab:`,
  `contentWidth` and `scrollOffset` are for the checks. GTK's
  `TabStrip` (2026-11-19, `linux/HANDOFF.md` item 115, `tab_strip`)
  does the same by the same calls, in every strip (both editor groups,
  the terminal dock's, the browser's): a GtkBox with a custom layout
  places each tab at `layoutStrip`'s place less the scroll, the title cut
  by `fitName` (a label for the name, one for the hint), the wheel and a
  touchpad by `wheelScroll`, a held drag's frames by `dragAutoScroll`,
  the fades by `clipped`; the editor's owners take `tabNames`
  (Tabs.cpp, Groups.cpp). The strips' titles are `tabs.font-size` points (12 unset; `MCTabStripFont`) in every strip, editor, split group, browser and terminals, and a strip is `MCTabStripHeight()` tall (30, more for a bigger font), which the owners read on every layout and lay out again on a settings change. A tab's
  right click (`menuForTabAtIndex:`) has the close commands, copy paths,
  reveal, rename and trash; the File menu repeats them for the active tab so
  the palette finds them.
- **The preview tab** (2026-11-08, VS Code's enablePreview,
  `workbench.enable-preview`): `Model::Preview::User` (`MCTab preview`),
  italic like `agentPreview`, one per editor group (VS Code's).
  Every open goes through `openFileAtPath:from:` (`openFileAtPath:` is
  `OpenFrom::Other`, kept) and the rules are the core's
  (`EditorTabs::opensInPreview`, `treeClick`, `staysPreview`,
  `placePreview`, `tabs:*`, and `Model::Window::prepareOpen` and `add`,
  `session:preview`): a tree click, a search result's single click
  (`SearchPanel previewHandler`) and a code navigation
  (`lsp.openFileForNavigation`; `prepareOpen` keeps the preview tab it
  starts from: enablePreviewFromCodeNavigation off) open in preview; Quick
  Open only with `workbench.enable-preview-from-quick-open`. A kept open of
  a preview tab's file (the user's or the agents') keeps it; so do an edit
  (`noteEdited`), a save (`writeText:`), a double-click on its title and
  Keep Open (tab menu, File menu). The replaced tab is dropped unasked and
  never in Reopen Closed Tab; a dirty one is kept instead. Agent previews
  stay their own (neither replaces the other; a note's and a diff's are
  apart too), and while the user types the agents' preview tab they are in
  is passed over (`add`'s `passOver`), never replaced. The
  workspace edit applier opens kept (`lsp.openFile`): it opens every file
  before editing any, so previews would replace each other. No
  `workbench.editor.limit` (off by default in VS Code too).
- **Dirty means "differs from disk"**: `noteEdited` compares with `savedText`
  (length first), so undoing back clears the dot. `savedText` is the text as
  read (CRLF survives); a UTF-8 BOM is kept apart in `MCTab bom` and written
  back. **UTF-16 with a BOM** (2026-11-08, D10) is text too: `MCReadDiskText`
  decodes through `FolderSearch::decodeFileText` (strict: a lone surrogate
  or an odd byte leaves the file to Quick Look) and `MCTab encoding` says
  how it is written; every write goes through `MCEncodeFileText`
  (`encodeFileText`), so an unedited file round-trips byte for byte, Save
  As keeps the encoding, and a backup records it (`file-encoding`) for a
  file gone by the restore. Anything comparing a tab with the disk reads
  the disk with `MCReadDiskText`, never `stringWithContentsOfFile:` as
  UTF-8 (Replace in Folder once took a clean UTF-16 tab for unsaved
  changes). UTF-16 without a BOM stays Quick Look. **Windows-1252**
  (2026-11-18, C3b): bytes that are not UTF-8 and hold no NUL anywhere
  (`FolderSearch::readsAsWindows1252`, the rule Find in Folder searches
  by) decode as Windows-1252, its five undefined bytes as Latin-1's C1
  controls, so all 255 non-NUL bytes round-trip; valid UTF-8 is never
  read so. `MCTextEncodingWindows1252` mirrors the core's value. A save
  whose text holds a character Windows-1252 lacks (`canEncode`) writes
  UTF-8 instead (`MCEncodingToWrite`), switches the tab to UTF-8 and
  says so in the status bar: chosen over a question on every save, and
  never a '?' written. The status bar's `encodingLabel` names any
  encoding but plain UTF-8 (`MCEncodingStatusLabel`, the core's
  `encodingLabel`), refreshed from `updateSavedStatus`. The git gutter
  and the review read a Windows-1252 file's blob the same way
  (`QuickDiff::readOriginal`, `encodingOfBlob`) and stage it back so.
  Replace in Folder still leaves these files alone (it writes UTF-8 and
  UTF-16 only). **UTF-8 with stray bytes** (2026-11-19, round 10's
  workspace item 2, `encoding:utf8-stray-bytes`, `make guitest
  SUITES=save`): bytes that are not valid UTF-8 and hold no NUL but are
  mostly UTF-8 (at least as many valid multi-byte characters as runs of
  stray bytes: `FolderSearch::readsAsUtf8WithStrayBytes`, the exact
  complement of `readsAsWindows1252` among such bytes) decode as
  `TextEncoding::Utf8Raw` (`MCTextEncodingUtf8Raw`, "utf8raw", label
  "UTF-8, bad bytes kept"): each valid character as itself and each stray
  byte b as U+10FE00 + b (`kStrayByteBase`, supplementary private use),
  which `encodeText` writes back as b. Unedited or edited, the file
  round-trips, and `canEncode` is always true, so `MCEncodingToWrite`
  never switches it. A file already holding a character in
  U+10FE80..U+10FEFF is never read so (it stays Windows-1252). Find in
  Folder's whole-file read (`readFile`) uses the same decode; the large
  streamed read still switches to Windows-1252 at the first piece that is
  not UTF-8 (round 10's item 3, open). The review and the gutter read a
  Utf8Raw file's blob the same way (`encodingOfBlob`, `readOriginal`). The GTK editor follows the same rules (2026-11-18,
  `linux/HANDOFF.md` item 102): `Document::readText` decodes through
  `decodeFileText` (UTF-16 and a UTF-8 mark too, which it had not
  handled: UTF-16 was "Cannot display", a UTF-8 mark sat in the buffer
  as U+FEFF), `Document::FileEncoding` is kept per document (read,
  reload, the background load, attachHeld, a backup's record), `save`
  writes `encodeFileText` or UTF-8 by `canEncode` with the same note,
  and main.cpp's status bar shows `Document::encodingLabel` left of the
  problem counts; the git gutter reads the blob in the file's encoding.
  GTK reads Utf8Raw through the same `decodeFileText` (2026-11-19, item
  121, `latin1_files`): the label is the core's, the tooltip says the
  bytes are saved back as they were, and Replace in Folder's skip note
  says "has bytes that are not UTF-8".
- **One writer**: Cmd+S, `saveTab:` and auto-save all go through
  `writeText:toTab:interactive:` over `FolderSearch::writeFileAtomically`
  (a symlink stays a link, a dangling one too, creating the file it names;
  hard links and files owned by someone else are
  written in place, xattrs, ACL and creation date kept, F_FULLFSYNC), which
  refuses a tab without text of its own (`tabHasTextToWrite:`: SQLite, Quick
  Look, a large CSV) and sends didSave. `writeToFile:atomically:` replaced a
  link with a plain file. What a tab knows of its file is a stat with
  symlinks followed (`MCDiskStamp`: date, inode, size; `tab:changedOnDisk:`
  compares all three), taken before the read (`MCReadDiskText`, over
  `FolderSearch::readFileStable`), so a file still being written never reads
  as clean; `attributesOfItemAtPath:` did not follow links.
- **Keys**: Ctrl+Tab / Ctrl+Shift+Tab, Shift+Cmd+] / [, Shift+Cmd+1..9 and
  Ctrl+1..9 (macOS takes Shift+Cmd+3, 4, 5). Cmd+W (`closeFocused:`) closes the
  tab, or the terminal, the browser's tab (the browser with its last) or
  Problems list with the keyboard; a repeat
  (`isARepeat`) is ignored, so a held Cmd+W closes one thing. Shift+Cmd+W
  closes the window. Cmd+N New File (with no window, opens one), Shift+Cmd+N
  (outside the tree) and Option+Cmd+N New Window. Reopen Closed Tab (Option+Shift+Cmd+T): the newest of the
  window's last 50 (`Model::Window`'s closed tabs, each with
  `closedPlaceOf:`'s place, selection, scroll, mode and side page),
  skipping files gone.
- **Unsaved tabs are never dropped**: a dirty tab asks Save / Don't Save /
  Cancel; window close and Open Folder ask once for all. Quit keeps them
  for the next launch without asking (hot exit, below); with
  `files.hot-exit = off` it asks once for all too. Trash closes
  only clean tabs of what went; `pathMovedFrom:to:` moves tabs.
- **The disk**: a buffer replaced from disk by choice (Reload) loses its undo
  steps (`forgetHistoryOf:`). Every save checks the stamp first (`mayWriteTab:`:
  changed outside asks Keep Mine / Reload / Cancel; gone asks before writing).
  Save All asks every question first (`diskDecisionForTab:`), then writes.
  `saveTab:` saves a background tab in place (a keep-sorted sort waits in
  `pendingSort`). Moved or deleted outside (FSEvents, `checkTabsOnDisk`, show
  and focus): a same-folder rename is followed by inode and date (`fileID`),
  else the tab is `gone` ("(deleted)") until the file returns, and is read in
  the same pass when it does. **A clean tab follows the disk**: from FSEvents
  (`followCleanTabInBackground:`) the file is read and diffed off the main
  thread, generation-counted, and a file over 8 MB is only noted and read on
  show or focus; on show or focus (`reloadCleanTab:`) on the main thread. The
  new text goes in as one undo step, "Reload from Disk", keeping caret, top
  line and the steps before it (`replaceSourceWithText:`, or for a tab not on
  show as text an undo action of its own, `registerReloadUndoForTab:`; an
  NSTextView registration made during an undo redid onto the wrong range).
  A read that fails moves no date.
- **Tabs that show no file come back made again** (2026-11-18,
  `TabSessions::PageTab`, `Session::pages` and `activePage`,
  `tab-sessions:pages`, `EditorController+PageTabs.mm`): the review page
  ("Uncommitted Changes"), a commit's page and a side-by-side comparison
  are kept beside the files (each with `at`, its place among the tabs) in
  both the folder's session and a window's record, and restoreSession makes
  them again after the files (`tabForRestoredPage:`, inserted at `at`; the
  one on show is shown instead of a file): the review and commit pages
  (`newChangeReviewPage`, `newCommitReviewPage` in +Git, the same hooks as
  a click makes) read git when first shown, and a comparison's two sides
  are read off the main thread (`remakeComparisonOfTab:`) from its recipe,
  `MCComparison leftPath/leftRev/rightPath/rightRev` (rev "" the disk, ":"
  the index, else a revision; a side git lacks is empty), set where a
  comparison can be made again: the review page's side by side (index
  against the disk, HEAD against the index, a commit against its parent)
  and `mutible diff` (a revision against the disk, a file against a file).
  Left out: a worktree's page and comparison, a conflict's two sides, an
  agent's proposed change (`decisionPrompt`), agent preview tabs, the old
  shared diff tab, and a revision the record refuses (`isComparedRev`: no
  option, no `..`, nothing but letters, digits and `._/~^-`; a commit's
  ref is hex only). Pages are a key of their own, so an older mutible
  reads the files as before and leaves the pages out. The GTK port keeps
  and brings back the review and commit pages the same way
  (`Persistence::Host::pageRecord` and `restorePage`, `Editor::addPageAt`,
  `linux/HANDOFF.md` item 134), a duplicate in the record left out before
  it is made, since a review page owns the panel's refresh; comparisons
  are still the Mac's only.
- **Sessions** (since stage 3c, 2026-11-17, `SessionStore.{h,mm}`): a
  folder's `TabSessions::Session` in sessions.json
  (`~/Library/Application Support/Mutible/sessions.json`, NSHomeDirectory,
  or `$MUTIBLE_SESSIONS_FILE`; the GTK port's format, by resolved folder,
  50 folders) reopens its tabs and its browser pane (each tab's
  `interactionState` as base64, `archived` for NSKeyedArchiver's form,
  `browserShown`, `browserFraction`), else a read-only empty editor. Every
  read and write runs on one serial queue: a save is handed over at once
  and written in that turn over the file as it is then (another
  mutible's folders stay); a read waits for the writes queued before it;
  `applicationWillTerminate` waits for all (`MCSessionsFlush`). The user
  defaults' `MCTabSessions` and `MCOpenWindows` are carried over once,
  by the first mutible with no file (`MCSessionsFromDefaults`, checked in
  window_restore; `MCSessionsMovedToFile` marks it, and the old keys stay
  for an older mutible). Off under `MUTIBLE_DEMO` and `MUTIBLE_BENCH`;
  the harness's scratch home holds the file, and `MUTIBLE_DEFAULTS_SUITE`
  the old keys. The checks read it through `storedSession`. A tab over 16 MB or
  not a plain file waits for a click. Windows on one folder share it: each save
  lists the saving window's files then the others'. Agent preview tabs are left
  out (they were saved until 2026-11-08); the user's preview tab is saved as
  `preview` and comes back as one unless a backup made it dirty.
- **Every window comes back** (2026-11-16, W1; VS Code's
  window.restoreWindows "all"; `TabSessions::Window`, `tab-sessions:windows`,
  `make guitest SUITES=window_restore`): a launch with no path argument
  (the Dock, Finder, `open -a`, a `brew upgrade` reopening, ↻ Restart's
  relaunch) reopens every window open at the last quit, front first. Before
  this, a Dock launch opened a window on `/` (its cwd) with nothing in it,
  and the ↻ restart brought folders and tabs back but no frames, carets,
  panels or tree. The record is the core's (`encodeWindows`, sessions.json's
  `"windows"` since stage 3c, the defaults' `MCOpenWindows` before): per window its frame (`fitFrame` onto today's screens), full
  screen, folder, its own tabs as a `TabSessions::Session` (each tab's
  selection, top character and offset, page or source, side preview, the
  groups), the sidebar's panel, width and Cmd+B state, editor hidden,
  Problems, the terminal dock (shown, height, each tab's panes' folders,
  names and `TerminalLayout` shares; fresh shells, nothing reattached),
  and the tree's expanded folders, selected row and scroll. Written by
  `applicationWillTerminate`, by `windowWillClose:` (`noteWindowClosing`,
  `TabSessions::afterClosing`: the last window is kept, since closing it
  quits) and 3 s after a tab switch (saveSession), never empty. **Several
  mutibles at once** (2026-11-17, `TabSessions::mergeWindows`,
  `toRestore`, `tab-sessions:windows-of-several`): each window carries its
  writer's pid, start time and save time, and a process writes its own
  windows over its own and the last run's only, keeping those of one still
  running or one that wrote since it started (it quit meanwhile); a launch
  skips windows a running mutible holds open. Before, each wrote its list
  over the whole record, so a build beside the Homebrew copy, `mutible
  -n`, or the GUI harness's parent beside its child apps wrote each
  other's windows away (main's CI 44044357638: the parent's 3 s save
  landed between window_restore's children; the suite now writes the
  parent's windows at every point between them). A restored
  window takes its tabs from the record, not the folder's session
  (two windows on one folder each get theirs); showWindow puts back the
  frame first (`placeRestoredWindow`), then the sidebar, terminals and
  Problems before the tabs, so text is laid out at its width
  (`restoreWindowLayout`), and the tree, a hidden editor and the keyboard
  last (`finishRestoringWindow`); a tab's place goes back when it is first
  read (`MCTab placeFromSession`, `applySessionPlaceOf:` from
  `loadActiveTab`). A path argument or a Finder open at launch opens only
  that, as before, but **a folder opened by name comes back as its
  window** (2026-11-18, VS Code's way with a folder; the walker's
  `open -n -a Mutible.app <folder>` lost its terminal, review page and
  commit tab): AppDelegate's `openWindowAtPath:` (every open of a folder
  no window here shows: the command line, Finder, `open -a`, `mutible
  dir`, a window per folder with backups) asks
  `windowRecordToReopenFolder:` (`MCSessionsWindowToReopen`, the core's
  `toReopen`, `tab-sessions:to-reopen`) for the record's first window on
  that folder, compared with links resolved, that another running mutible
  does not hold and this process did not write (a window this process
  closed went to the folder's session; the GUI harness's own windows never
  come back into its next ones), and restores it whole: frame, tabs with
  their places and pages, terminal dock with fresh shells, panels, tree.
  None: the folder's session alone, as before. A second window on a
  folder already open (New Window) takes no record. The Mac's selection offsets are UTF-16 and GTK's code
  points: each reads only what it wrote, which stage 3 must settle.
- **One record on both ports** (2026-11-16, stage 3b): GTK's sessions.json
  keeps the same `TabSessions::Window` list under the key `"windows"`,
  beside the folders' sessions (`TabSessions::Store`,
  `encodeStore`/`decodeStore`, `tab-sessions:store`); an older file reads
  as folders only and an older mutible skips the key. The GTK app
  reopens those windows on a launch with no path from the home folder or
  `/` (its size, own tabs, sidebar, terminal, Problems; linux/HANDOFF.md
  item 76). The Mac keeps the same file since 3c (see "Sessions").
  GTK writes a window's session 1 s after its tabs change
  (`Planner::sessionChanged`), so a window opening on a folder first has
  every other window on it write a pending save (`takeSession`,
  `Persistence::flushSession`, `saveplan:session-before-another-window-reads`;
  GTK split_editor's flake, #81); the Mac writes at once.
- AppDelegate's `current` maps a window an editor made (`ownsWindow:`, Find in
  Folder and TODOs) to that editor; items that edit the file on show are
  disabled while another window is key, so keys reach that window's field.
- **Two editor groups** (2026-11-08, `EditorController+Groups.mm`,
  `EditorMirror.{h,mm}`, `EditorTabs::restoreGroups`, `make guitest
  SUITES=split_editor`): VS Code's Split Editor, two groups at most. The
  one editor (CodeTextView and everything on it) is always in the focused
  group; the other group's tab shows in an `MCEditorMirror`: a plain
  read-only NSTextView with a layout manager of its own over the editor's
  MCSourceStorage when it is the same tab shown as editable text (TextKit 1:
  several layout managers, one storage), else a copy of that tab's
  attributed text, else a picture (`cacheDisplayInRect:`) of what the
  editor showed. Focusing a group (a press in the mirror or its strip,
  Cmd+1, Cmd+2) copies what the editor shows into the mirror, focuses the
  window's other group (`_win->focus`), swaps the strips (`self.tabStrip`
  is always the focused group's) and activates the other tab at the
  mirror's place; a press goes on to the same character. A two-click press
  on a page shown as a picture opens the page's block editor at once
  (`editPageAtWindowPoint:`, `docs/notes/markdown.md`), and the picture
  accepts the first mouse, as the text does. The groups are
  `_win`'s; `tabs` is every document of either group
  (`Model::Window::documents`: backups, sessions, auto-save, disk checks,
  the servers' open files untouched); the strip, tab keys and closing use
  `groupTabs`, and new tabs go in through `groupInsertTab:` (or
  `groupAppendTab:`, a session). A close that empties a group drops it in
  the window at once (`Model::Window::close`), and the views follow:
  `editorGroupsJoined`, or `editorGroupClosed` when the focused group
  went (the other takes the editor at the mirror's place). One MCTab per
  file even in both groups (one text, undo, backup). **Traps**: never move
  the mirror's layout manager with `replaceTextStorage:`, which carries
  every layout manager of the storage, the editor's too
  (`removeLayoutManager:` then `addLayoutManager:`); the mirror must let go
  of the editor's storage before its whole text is replaced
  (`MCSourceStorage willReplaceAll`, and `editorWillShowTab:` for an empty
  text), or it lays out the next tab's text or a Markdown page; the
  editor's layout manager stays the storage's first (`usesCodeWords`).
  Left out: squiggles, inlay labels, folds and carets in the mirror; a find
  bar per group; dragging tabs between strips; a third group or a split
  down. Cmd+2 was Focus Terminal (a Command-only key, so it yields).
- GTK's tabs are a `Model::Window` (above), with the same rules and the
  split editor; Android has no tabs.

## Backups, hot exit and auto-save (Mac)

- Every dirty tab's text is on disk while edited, as VS Code's backups.
  **Hot exit** (the owner's decision, 2026-11-18, which reversed "quitting
  still asks (the user wants the question)"; VS Code's `files.hotExit`
  `onExit`; `files.hot-exit` = `on-exit`, the default, or `off`): Quit
  never asks to save (see "Quit" below). `Backups.{h,cpp}` (format,
  `Store` over an `Fs`, `plan`, `foldersToReopen`, `expired`, `compare`),
  `BackupStore.mm` (folder, write queue, which process holds a backup),
  +Persistence (what and when). Store: `~/Library/Application
  Support/Mutible/Backups` (NSHomeDirectory, so `CFFIXED_USER_HOME` moves it)
  or `$MUTIBLE_BACKUP_DIR`; never in the project. One `<uuid>.mcbackup` per
  tab whose header is its own index entry, so processes never race. Written to
  a temporary (0600), flushed with `fcntl(F_FULLFSYNC)` (plain `fsync` leaves
  data in the drive's cache; fsync where a volume refuses, and on Linux),
  renamed, and the folder synced too.
- `scheduleBackups` (from `noteEdited` and `updateTitle`): a clean tab's backup
  goes at once; a changed dirty buffer (`textGeneration` compared with
  `backedUpGeneration`) is written 1 s after typing stops, at most 5 s after the
  first unwritten change, the immutable NSString converted on the queue. Only
  text tabs with a path.
- **The planner** (2026-11-16, stage 3b): these timing rules, auto-save's
  below included, are the core's `SavePlan::Planner` (`changed`,
  `focusChanged`, `tick(now)`, `nextDue()`; `saveplan:*` holds it to each
  rule here, with a random walk). Both ports run on it with one timer:
  the Mac since 3c, `_plan` and `_planTimer` (an NSTimer, re-armed for
  `nextDue()` by `armSavePlan`, which never loses a planned save: it is
  only when), fed by `scheduleBackups`, `scheduleAutoSave` and
  `autoSaveOnFocusChange` (`planTabOf:`: MCTab's `textGeneration`,
  `backedUpGeneration`, `backedUpPath` and `hasBackup` one for one,
  `tabTakesAutoSave`'s other conditions as `mayAutoSave`), stopped by
  `stopSavePlan` as the window closes. A rule changed here changes there.
- Restore: `planForRoot:` takes the folder's backups no live process holds (pid
  and start time); `takeBackup:intoTab:` makes the tab dirty with the backup's
  base date and inode, and on first show (`showRestoredText:`) the backup is
  the buffer, the disk text `savedText`, undo empty, with a note ("Restored
  unsaved changes to x", and if the disk changed since). A backup whose
  file never existed and still does not (`diskExisted` false: an untitled
  buffer the session's host held) comes back `untitled` (not "(deleted)",
  never auto-saved, Cmd+S makes the file).
  `reopenFoldersWithBackups` opens a window per folder with unheld backups.
  Orphans over 30 days and temporaries over a day go (`cleanUpOnce`).
- **Backups by window** (2026-11-19, round 10's workspace item 1,
  `backups:plan-windows`, `backups:draft-path`, `make guitest
  SUITES=backups`): every backup names its window (`Entry::window`, the
  header's `window`), the window's `backupKey` (`_backupKey`, a UUID
  kept in its record, `TabSessions::Window::backupKey`, and adopted by
  `restoreWindowRecord:`). `windowsToRestore` reserves the keys of every
  window the launch brings back (`reserveWindowKeys:`), and `plan` (with
  `window` and `reserved`) leaves a reserved key's backups to its window:
  two windows on one folder with one file unsaved in both each get their
  own. Of several backups of one file a window may take, its own newest
  goes in the tab (else the newest) and every other is a conflict, never
  deleted: `restoreConflictingBackups:` rewrites it under
  `Backups::draftPath` ("a (unsaved copy).txt", `moveID:toPath:window:`,
  never a file on disk until saved) and opens it as an untitled tab
  (`MCTab draftOf`, its own note), or removes it when its text is the
  file's tab's. **GTK** (2026-11-19, `linux/HANDOFF.md` item 124, `backups`
  suite's two-window part): `Persistence::backupKey` (made by
  `BackupStore::newId`, kept in `windowRecord`, adopted from the record in
  `newWindow` by `setBackupKey`), `writeBackup` sets `Entry::window`,
  `restoreOpenWindows` reserves every record's key
  (`BackupStore::reserveWindowKeys`) before the first window opens, and
  `planForRoot` passes the window and the reserved keys to `plan`.
  `Persistence::restoreConflicts` is `restoreConflictingBackups:`:
  `BackupStore::move` rewrites the backup under `draftPath` (on the
  store's thread, then a flush), the tab is `addTab` plus `takeBackup` of
  the moved entry (`diskExisted` false: unsaved, not on disk, never
  auto-saved: `autoSaveNow` skips a buffer never on disk), its own note,
  and the window's note once the tab on show is up. A backup of a file
  the window already holds from the session's host (`attachHeldDocuments`)
  is no longer removed unread: it goes only when its text is the held
  document's, else it is a copy too.
- **Quit** (Cmd+Q, the Dock, logout, restart; AppDelegate's
  `confirmQuitRestarting:`): the running-commands question stays (it is
  not about saving), then each window with unsaved tabs, under hot exit or
  a restart from Request a Change, calls `keepUnsavedTabsForNextLaunch`:
  every dirty buffer written synchronously (`writeNowID:`, F_FULLFSYNC, one
  write a tab, so `applicationShouldTerminate:` stays quick), the session
  saved, `_unsavedKeptForNextLaunch` set, nothing asked. NO (backups off,
  as under `MUTIBLE_DEMO`; a dirty tab `tabTakesBackup:` refuses; a write
  that fails, such as text UTF-8 cannot carry) and that window asks as
  before, so quitting never loses what was typed. `backupsAppWillTerminate:`
  removes the backups only of a window that asked (what is still unsaved
  there was let go with Don't Save); a Cancel anywhere clears every
  window's flag (`quitWasCancelled`), so a later Don't Save still removes
  them. Closing a tab or a window asks as before. The windows record and
  the sessions are written by `applicationWillTerminate` as always, so the
  next Dock launch brings every window back with its unsaved tabs (the
  backups no live process holds, `planForRoot:`), and a folder opened by
  name its window. With the session's host on (`docs/notes/session-host.md`):
  no question, so nothing is dropped there (`sessionDropOnClose:nil`); a
  headless host keeps the documents as Keep Unsaved would and the next
  window takes them over the backups (restoreSession removes a backup of
  a path the host gave back); an in-process host ends with the app and the
  backups carry the text, an untitled buffer included. A crash, SIGKILL or
  power loss leaves the backups too. Undo history does not survive a quit
  (the text, caret and scroll do). Trap: `keepUnsavedTabsForRestart`
  passed over a dirty tab it could not back up, which a quit without a
  question would have lost; `keepUnsavedTabsForNextLaunch` returns NO for
  it, and the window asks. Checked by window_restore (`_restore_writer`
  quits with no alert or sheet, `files.hot-exit = off` asks and Cancel
  keeps all; `_restore_reader` finds the unsaved and untitled tabs and the
  pages back).
- **Auto-save**: `editor.auto-save` = off (default), `after-delay`
  (`editor.auto-save-delay`, 1000 ms) or `focus-change` (the editor loses the
  keyboard, the window resigns key, another tab shows). `scheduleAutoSave` sits
  beside `scheduleBackups`. A plain write: no formatter, no keep-sorted, never
  a dialog from a timer (`writeTabForAutoSave:` checks the disk by hand; a file
  changed outside is skipped and named, so Cmd+S asks). Skipped: gone and
  unloaded tabs, a tab whose Cmd+S waits for its formatter, a tab with an
  inline-agent edit awaiting Keep or Discard (`MCAgentSession editPending`),
  and the tab on show while an input method composes (`hasMarkedText`). The
  close and quit questions run `autoSaveNow` first, so auto-save's writes are
  not asked about (`files.hot-exit = off`; hot exit asks nothing). A
  restored backup opens in its viewer (a CSV's table).
  Trap: the focus-change save and the delay's debounce shared a selector and
  `updateTitle` cancelled the save after a tab switch; nothing may cancel
  `autoSaveNow`.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- Tabs, sessions and the disk (Mac): **Traps**: never move the mirror's layout manager with `replaceTextStorage:`, which carries every layout manager of the storage, the editor's too (`removeLayoutManager:` then `addLayoutManager:`); the mirror must let go of the editor's storage before its whole text is replaced (`MCSourceStorage willReplaceAll`, and `editorWillShowTab:` ...
- Backups, hot exit and auto-save (Mac): Trap: the focus-change save and the delay's debounce shared a selector and `updateTitle` cancelled the save after a tab switch; nothing may cancel `autoSaveNow`.
- Backups by window (round 10's item 1): `plan` kept the newest backup of a file and listed the rest to delete, and the first window back on a folder planned for the whole folder, so after hot exit one of two windows' unsaved text in one file was deleted unseen, and the first window took the other's other files. A backup is deleted only once its text is back or saved; a planner never lists any to delete, and a window takes only its own and those of no window still to come back. GTK had the same until linux/HANDOFF.md item 124, and there a document held by the session's host also removed a backup of its file unread; it now goes only when its text is the held document's.
- Encodings (round 10's item 2): "not valid UTF-8" is not "not UTF-8". One stray byte made `readsAsWindows1252` read a whole UTF-8 file as Windows-1252, and the first character Windows-1252 lacks saved it as UTF-8, every character encoded twice. A new decoding rule must keep both directions byte for byte (`encoding:*` round-trips random bytes through `decodeFileText` and `encodeFileText`).
- Backups, hot exit and auto-save (Mac): Trap: `keepUnsavedTabsForRestart` passed over a dirty tab it could not back up, which a quit without a question would have lost; `keepUnsavedTabsForNextLaunch` returns NO for it, and the window asks.
