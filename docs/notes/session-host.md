# The session's host on the Mac (editor.session-host)

SESSION-DESIGN.md (in `.grades/`) is the design; its "Stage 4a as built"
and "Stage 4b as built" say what was built. GTK's side is
`linux/HANDOFF.md` item 87. This file is the Mac's.

## What it is

With `editor.session-host = on` (off by default for one release), or
`MUTIBLE_SESSION_HOST=on` in the environment (the GUI checks' second
pass), every tab whose text is set after the setting is on gets a document
on the session's host: the core's `Shared::SessionHost` on a thread of its
own in the app's process, one `Shared::Client` in UTF-16 units for the
whole process (`mac/src/SessionLink.{h,mm}`). The host owns that
document's text, version and undo history; the tab's text is its mirror. A tab opened
before the setting changed keeps the way it was opened until it closes.

- `MCTab session` (an `MCSessionDoc`) is made by `MCTab setSourceText:`
  the first time the tab's text is set with the setting on, and closed
  with the tab (`dealloc`).
- **The storage's edits.** `MCSourceStorage willReplaceCharacters` is
  called before each character replacement, while the store still holds
  the old text. `EditorController+Session.mm` installs it once
  (`installSessionHooks`): while `_liveHighlight` is on (the storage holds
  the active tab's source), the edit goes to the active tab's document as
  an operation (`storageWillReplace:with:view:`), widened to whole code
  points so a surrogate pair is never split, and the tab is made to follow
  the storage at once (`followStorage:`), so its `currentText` is never a
  copy from before the edit.
- **Edits outside the history.** A storage edit made outside a change of
  the text view's (no `shouldChangeText...` before it: the storage edited
  directly, which NSUndoManager kept no step for) goes as the host's edit
  outside the history (`Client::recordOutside`, `doc.edit` with flag 1):
  the turn's step closes first, the host applies it in no step and moves
  its history past it (`UndoHistory::foreign`), so the step before is
  undone where it moved to, and a step whose text it touched goes with
  everything older. `CodeTextView sessionChanging` says whether a change
  of the view's is going on (set by `shouldChangeTextInRange(s):`, cleared
  in `didChangeText`). The tab's undo manager's `removeAllActions`
  forgets the host's history too (`doc.forget`), the text kept.
- **Putting a tab away** (`stashActiveTab`): while the storage held its
  source it was the mirror, so its text goes into the tab without telling
  the host (`MCTab takeTextFromStorage:`). The tab's own copy can be older:
  a storage replaced whole outside the text view leaves the tab the text
  from before (`willReplaceAllCharacters`), and sending that difference
  again applied it twice on the host (CI, 2026-11-17: the host 4 units
  short of the tab in multiedit).
- **Texts set whole** (a CSV table's edit, a preview's, a background tab
  following the disk, a pending sort): `MCTab setSourceText:` sends the
  one range in which the old and new texts differ.
- **Read afresh** (`forgetHistoryOf:`, a reload by choice, a first read):
  the host's copy is reset to the tab's text, its history gone, and any
  text set whole in the same run-loop turn resets it again rather than
  being an edit.
- **Steps.** The edits of one run-loop turn are one step, as
  NSUndoManager's grouping by event made them: it opens at the first edit
  and closes on the main queue's next turn. Its name is the first given:
  `replaceRanges:withStrings:actionName:`'s name (`beginNamed:view:`, before
  the storage edits), or a later `setActionName:` on the tab's undo manager
  (`MCTabUndoManager`); with none, "Typing" when the edit came from
  `insertText:` or a delete or insert command (`CodeTextView
  sessionTyping`), which joins the step before while the caret stays put,
  else "Edit". `breakUndoCoalescing` and a selection set by anything but an
  edit (`noteSelection:`) start the next typing step anew.
- **The language server sync** (`Lsp+Sync.mm`, `hostText`): the document
  on show is watched through the client (`Client::watch`, a copy of the
  host's text kept from the host's own edits), settled before each flush,
  and read for `didChange` and `didOpen`; the storage's edits still feed
  the `ChangeTracker`. A host text of another length than the storage's
  (an edit of the host's waiting to go in) is sent whole. The watch is let
  go when the document leaves the view and at shutdown.
- **Saving** is the host's write since stage 7's second write (below,
  "The Mac's tabs save through the host"): on a host that saves for apps
  the window asks its questions and `MCSessionDoc saveOnHostEncoding:bom:`
  has the host write the file; the window follows the host's writes for a
  page and its count of the tab as unsaved. On a host that does not (one
  lost, or from before), the window writes the file and tells the host
  (`MCSessionDoc saved`), as before. The disk checks, backups and
  auto-save's timing stay the Mac's.

## Undo is the host's

- `MCTabUndoManager` (EditorTab.mm), every tab's undo manager, is the
  host's front while the tab has a document: `undo`, `redo`,
  `undoNestedGroup`, `canUndo`, `canRedo`, the action names and menu
  titles ask the host, `isUndoing`/`isRedoing` say when its answer is going
  in, and `setActionName:` names the host's step. So everything that
  undoes through the tab's manager (the menu, the CSV table, the workspace
  edit applier's put-back, the GUI checks) undoes on the host. A group
  begun with event grouping off (the GUI checks' group around a simulated
  key) is a host step of its own, as it is an undo action of its own.
- NSTextView, `replaceRanges:` and completion's `MCAcceptUndo` still
  register on the tab's NSUndoManager stacks, which are never undone while
  the host owns the history (memory only, as before). **Trap (CI,
  2026-11-17):** turning NSTextView's `allowsUndo` off and answering its
  `undoManager` anyway made it register twice in every `replaceRanges:`
  (which turns `allowsUndo` off around `shouldChangeTextInRanges:`): it
  decides by the manager `undoManager` returns, not by the flag. Leave
  both alone. The host's own edit goes in with the tab's undo registration
  off.
- `undo:`/`redo:` on `CodeTextView` (answered only while the view shows a
  document on the host: `respondsToSelector:`) go to the host directly,
  with the menu item titled by the host's step. Edits are host steps named
  by `replaceRanges:`'s action name ("Completion" for an accepted item).
- An undo closes the turn's step first, then the host takes its last step
  back and the edit comes back before `undo:inView:` returns (in one
  process a round trip through the host's queue), with the selection the
  step started with: every selected piece of it (10,000 after typing over
  Select All Occurrences), else its first caret, as `replaceRanges:`'s own
  undo selected them. A step of several pieces (typing at two carets) is
  undone as one multi-range edit (`host:undo-multi-range`). A refused undo (the text it would change is not what
  it left) beeps and is logged; the host forgets the history.
- **A completion's late import** (`pendingAccept:resolved:`): the host
  cannot add to a closed step, so when nothing was typed since the accept,
  the word's "Completion" step is taken back on the host and the import
  and the word go in as one new step (the import is all before the word);
  an undo or redo on the host since the accept (`MCSessionDoc undoCount`)
  means the word was taken back, and the import is left out, as
  `MCAcceptUndo undone` said before.
- Known differences from NSUndoManager: a paste or cut is an "Edit" step
  (NSTextView named them); `registerReloadUndoForTab:`'s whole-text step
  is the host's ordinary step of the difference.
- **Follow-up: composing text.** Text an input method is composing
  (marked text) is in the storage, so it reaches the host as it is
  typed, and the commit replaces it there. The design sends only
  committed text; doing so means keeping the marked range out of every
  operation's offsets (the host's edits moved past it, the mirror's
  shifted by it) until the commit, which is a change of its own. It
  matters once a second screen shows the document.

## The host's edits

They come back through `MCSessionDoc hostEdited:` (the client's listener):
an undo's or redo's and, with a second screen, anyone else's. Into the storage through `replaceRanges:withStrings:
actionName:` as one multi-range edit when the operations are replacements
apart in the text (`Ot::asReplacements`), else one operation at a time,
with `applying` set so nothing is recorded; into a tab not in the storage
as its `sourceText`, then `tabTextChangedByHost:` (dirty, title, backups).

**Re-entrancy.** The client's waits (`settle`, `undo`, `undoInfo`, a first
`watch`) dispatch every message that arrived, wherever they are called:
the language server flushes before each request, and completion asks from
inside `NSTextStorageDidProcessEditing`. An edit of the host's that
arrives while the storage is editing (`editedMask` set, inside
`processEditing` and its notifications) is never put in there: it waits
(`deferredEdits`) for the next run-loop turn, and an edit of the mirror
made meanwhile is moved past it before it is recorded (`Ot::transform`),
and it past that edit. The normal dispatch is a main-queue block at the
top of a turn, never inside a text system callback.

## Checks

- `make test`: `host:*`, `ot:*` (the core), `host:undo-multi-range` (a
  two-caret step undone and redone whole), `host:outside-history` (edits
  outside the history move the steps, or drop the ones they touch;
  `doc.forget`), `host:watch-timeout` (a watch
  the host does not answer in time keeps no watcher, and its late answer
  is dropped), `settings:*` for the key.
- CI's macOS job runs the editing suites a second time with the setting on
  (`MUTIBLE_SESSION_HOST=on`, `MUTIBLE_HOST=loopback`,
  `MUTIBLE_HOST_DELAY_US=200`), a step of its own after the halves ("GUI
  checks with the session's host"): keystroke, multiedit, editor_keys,
  auto_close, save, backups, lsp_sync, snippets, format, git_conflicts and
  test_gutter.
- `make guitest`: at every suite's end the harness settles with the host
  and fails the suite (`FAIL [suite] the session's host and the window it
  renders differ`) unless every tab with a document has the host's text,
  nothing of the host's waits to go in, and the host reported nothing
  wrong (`CheckSessionMirrors`, `MCSessionCheckTabs`). With the setting off
  no tab has a document and the check does nothing.
- `MUTIBLE_HOST=loopback` puts every message through the protocol's
  encoding and back; `MUTIBLE_HOST_DELAY_US` delays each one on the host's
  thread. Errors the host reports go to the log (`os_log`, subsystem
  ai.ible.mutible, category session).

## Traps

- **bash's readline writes the pty's size back** (`term:size` on CI,
  2026-11-21): each time it takes the terminal (every prompt, and after a
  SIGWINCH) it reads the size and sets it again (`set_winsize` in
  `rltty.c`), so a resize between the read and the write is undone. The
  pty was left at the old size while the host's screen, every renderer
  and `TermShell` said the new one, and `fit()` saw nothing to do at the
  next resize (it compares with the screen). A typed command followed at
  once by a size change (a typist going, a quick second resize) lost it
  under load; zsh does not do this, macOS's `/bin/sh` is bash 3.2. For 5 s
  after it resizes, `Host::look` (on output, and every 200 ms) checks the
  pty against the size it set and puts it back (`Pty::keepSize`, a new
  SIGWINCH); after that a program's own `stty cols` stands. A check of the
  pty's size waits for it to agree with the host's screen (`SizeRig::sized`).
- **Cookies are not kept apart by port** (the browser view, 2026-11-18):
  a cookie for 127.0.0.1 goes to every port of it, so a view's credential
  is named for its port and scoped to its own path (`/v/ID/`); scoped to
  `/` it would reach any dev server the page frames in its browser pane.
  And `SameSite=Strict` is not sent on a navigation from another site, so
  the browser's id (only a name) is Lax, or a link clicked in a chat page
  would make the browser look new and forget that it was remembered.
- **`history.replaceState` runs first in the page's script**, before the
  socket opens; the page's own requests use `viewBase` (`{{BASE}}`), never a
  path relative to the address, which changes under them.
- **A page's socket is taken whatever its credential's state** and told
  `waiting`, `denied` or `expired`: refused at the upgrade, a page cannot
  tell why and reconnects every 2 s for ever. Only a socket with no
  credential cookie at all is refused (403).
- **A tree listing runs on the host's thread**, between typing messages;
  hence `kMaxListed`. Stage 7's bigger reads go on the host's workers
  (`ReadWorkers`, "Quick Open and Find in Folder in the page", below:
  "files" and "search"); the listings, a file's read and the host's save
  (synchronous, an F_FULLFSYNC each) are still on its thread for now.
- **A search is several FsData for one request** (the last with "done"):
  the client keeps them apart from the one-answer reads
  (`requestStream`, `takeStream`; `replies_` holds one answer a request,
  and a second would overwrite the first). Only a renderer that asked
  with `requestStream` gets them, and only from a host whose Welcome has
  flag 8, so no renderer from before them ever sees two answers. A
  "cancel" has no answer: the client sends it with a request number it
  never waits on, and drops what still comes for the cancelled request,
  forgetting it at its last answer (always sent, saying "cancelled").
- **Replace in Folder is an FsRead op, and FsRead passes
  `refusedFromViewer`** (a reader may read): `Host::onReplace` refuses a
  renderer that only reads itself. Any new op that writes must do the
  same, and be offered by a Welcome flag (16 here), or an older host takes
  it for a read it does not know (harmless) or, for DocSave's "saveAs",
  for a plain save of the old file (not harmless).
- **A page's untitled document is a view seat's only** (DocOpen flag 2):
  nothing keeps it but the host, which drops it once no renderer has a
  seat and it is still empty (`dropEmptyPageDocs`); with text it is an
  untitled buffer the host keeps and backs up until it is saved.
- **The host follows no document an app's window has open**
  (`shownByApp`, stage 7's watching): the window follows its own tab and
  sends its reload as an edit, then DocSaved; the host following it too
  would apply the reload twice (the window's edit moved past the host's,
  inserting the new text again). A page's Reload is refused the same
  way.
- **FSEvents names folders as the system spells them** (/private/tmp for
  /tmp): each change is spelled again under the root as it was given, and
  a document whose path is spelled otherwise than the watched root is
  matched by its folder's real place (`Doc::realDir`). Its journal also
  trails a little: a folder made just before a stream starts can still
  come as a change after it (`diskwatch:system` lets it pass first).
- **g++ cannot read CoreServices' headers** (blocks in MDItem.h, xpc's
  attributes), so the Mac's FSEvents watcher is built only with clang;
  under `make test CXX=g++-16` on the Mac `Watcher::make()` is null and
  the `diskwatch:system`, `headless` and `page` groups say they were
  skipped. The core links `-framework CoreServices` on the Mac
  (`CORELIBS`: the tests and `mutible-cli`).
- **inotify watches one folder at a time**: a folder made must be walked
  as it comes (IN_CREATE with IN_ISDIR) or a file in it is never seen, and
  a folder moved keeps its watches under the old name (IN_MOVE_SELF drops
  them). A walk of a large folder takes a while, so it runs on the
  watcher's thread, never the host's; `Watcher::settled()` says when the
  last `watch()` was applied (`SessionHost::diskWatchSettled`, for checks).
- **A watch is a read, not a new kind**: FsRead's "watch" to an older
  host is answered "unknown op"; FsChanged and DocDisk go only to Hello
  flag 32, DocReload only to a host whose Welcome has flag 4 (the trap
  below).
- **A message kind a peer does not know drops the connection**, both ways
  (`takeLines` takes a line that does not decode for one that is not a
  message): a new host sending DocDirty to an app from before it, or a
  renderer sending DocSave to an older host, would cut the link mid
  session. New host-to-renderer kinds go only to renderers whose Hello
  asked (DocDirty: flag 8), and new renderer-to-host kinds only to a host
  whose Welcome offers them (DocSave: flag 1, `Client::hostSaves`).
- **The host's saved text is not proof of the disk.** DocOpen's text
  counts saved with no stamp, and an app's DocSaved resets the stamp to
  none, so the host never assumes the file holds its saved text: with no
  stamp of its own write, `saveDocument` reads the file and compares
  before writing (and takes the encoding from it, since the host's
  `Model::Document` defaults to UTF-8 for a file an app read as
  Windows-1252).
- **Dirty is a whole-text compare when the lengths match**, so the host
  compares for DocDirty only while some renderer hears it (`tellDirty`):
  an app typing over a selection in a 25 MB file would otherwise compare
  25 MB a keystroke on the host's thread with nobody listening. Since the
  Mac's tabs save through the host its clients always hear (they need the
  writes, DocDirty flag 2), so the host compares for every edit of a
  document the Mac shows; the Mac's own `noteEdited` makes the same
  compare on the main thread, so this is a second thread's copy of a cost
  the editor already pays, not a new one on the keystroke's path.
- **Never DocSaved after the host's own write.** An app's DocSaved resets
  the host's stamp to none, so the host's next save would read the file
  to compare it; the Mac's host path takes the stamp from DocSaveDone and
  sends nothing more (`writeTextThroughHost:`). DocSaved stays for the
  window's own write (a host that does not save for apps) and for the
  times the window's saved text moves without a save (below).
- **The host writes its own text, not the one handed to the writer.**
  `writeText:toTab:interactive:` is given a text; through the host, the
  tab's text must be that text first. A tab behind with a keep-sorted
  sort (`saveTab:`) takes the sorted text as one edit (`setSourceText:`,
  so the sort is a host step, not a `pendingSort` waiting for the tab to
  show); the storage is never set behind the view's back, and a tab on
  show always writes what it holds (`hostWritesText:ofTab:`).
- **A wait dispatches others' edits into the mirror.** The save's
  `settle()` can put a page's typing into the storage while it waits, so
  the callers' old `sourceText = text` after a save would send that edit
  back undone (`saveCurrentFile:`, `writeTabForAutoSave:` now set it only
  when it is still the tab's text), and the text the file holds is the
  tab's only when the host wrote the version the mirror is at
  (`MCHostSave inStep`), else it is read from the file.
- **The host's count of the tab is taken only when the mirror is the
  host's** (nothing of the tab's document unconfirmed, `unconfirmed(doc)`,
  none of the host's edits deferred: `mirrorInStep`), at the top of the
  next turn, the latest of
  those heard before it, and not when the tab's text changed meanwhile
  (`textGeneration`): a reload's edit and then its DocSaved would
  otherwise flash the dot. Never for an untitled buffer, a deleted file,
  a held tab not shown yet, or a tab not read: their unsaved state is more
  than their text against the file.
- **The host acknowledges an edit before the DocDirty it caused**
  (`handle` runs `onEdit`, which sends DocEdited, then `tellDirty`), and
  DocSaved needs no ack, so the edit's ack lets a DocSaved queued behind
  it out while the edit's "unsaved" is still to be taken: `mirrorInStep`
  held (nothing unconfirmed), the "unsaved" reached the tab a turn later,
  and the DocSaved's "saved" only a round trip after that. Every clean
  tab's reload flashed its dot, and the save suite's reload step failed
  `!c.dirty` when it looked in between (CI's session-host pass on #296,
  2026-11-21; GTK's port of the dot met the same, #297). The core's
  `Client::saved` and `savedAs` now fence the dirty listener: a Ping goes
  behind the DocSaved, DocDirty heard before its Pong is held, and the
  latest held is told at the Pong (`dirtyFenced`, `host:dirty-fence`
  forces the order). No protocol change, and any app that calls
  `saved()` gets it; never tell the listener a DocDirty that a DocSaved
  of the renderer's own is still behind. The GUI check: `save`'s reload
  step counts `MCDirtyFromHostMarks` (every time the host's count marked
  a tab unsaved) after waiting on `savePending`. The same CI picture
  showed a second dot, stuck on a tab reloaded two steps before
  (enc.txt): its late "saved" came while another tab's typing was in
  flight, and `mirrorInStep` then counted every document's unconfirmed
  messages, so the notice was dropped and, the host telling only flips,
  never came again. A notice about one document is judged by that
  document's messages alone (`Client::unconfirmed(doc)`).
- **Where the window's saved text is not the mirror, the host is told.** A
  clean tab following the disk (`takeDiskRead:intoCleanTab:`) sends its
  reload as an edit, which the host would count unsaved against the old
  text: DocSaved follows it. Keep Mine on focus (`checkExternalChange`)
  and a held tab's first read (`loadActiveTab`) count the buffer against
  the file's text now: DocSaved flag 1 with that text (`savedAs:`), sent
  only to a host whose Welcome has flag 2, since an older host would take
  it for "the buffer was written".
- **The host acknowledges an edit before the DocDirty it causes**
  (`Host::handle` broadcasts, then `tellDirty`), so on GTK a reload's
  edit followed by its DocSaved reaches the document as "unsaved" with
  nothing unconfirmed (the ack in, the DocSaved out), before the
  DocSaved's "saved": taken at the next turn, it would flash the dot.
  The client fences it (#299: `Client::saved` and `savedAs` send a Ping
  behind the DocSaved and hold the document's DocDirty until its Pong,
  passing on only the latest), and `Document::hostDirty` judges a count
  by that document's own unconfirmed messages (`Client::unconfirmed(doc)`),
  never every tab's; GTK's own stopgap (`toldSavedAt_`) is gone. GTK's
  own compare runs on every edit, so the host's count matters only where
  the two disagree, and those tells are what keep them agreeing.
- **The window's edited mark is every tab's** (`documentEdited` is
  `hasUnsavedChanges`): a check that one tab became clean reads that
  tab's `dirty` and its strip item's dot, never the window's mark, which
  stays set while any other tab is unsaved (host_reattach's first CI run
  of stage 7 waited 15 s on it with two other tabs unsaved).
- **GtkStack moves the keyboard to a tab's first pane** when it shows
  another tab with the keyboard in the one going ("Terminal splits in the
  record"): `TerminalDock::activate` hands it to the tab's active pane.
- **Every accepted connection is a descriptor of the app's**, on the Mac
  at the default limit of 256: the server once took idle loopback
  connections without limit or deadline, and any local process could
  hold them until the app's opens, saves and backups failed with EMFILE
  (round 10's defect 10). Keep the cap and the request deadline
  ("Connections" above) on anything new the server accepts.
- **Confine with `confinedPath`, never by spelling.** A path from a page
  is held inside the folder by its `realpath` (symlinks followed) and
  must not hold "..", ".", empty names or a leading '/'; the Mac's `open`
  trusts that check.
- **A typeset is kept by the document's real path, not served by it**
  (R3's third slice): the app may publish a typeset for any file, one
  outside the folder too, so `/typeset/` checks the request's path with
  `openLocalFile` before it looks the PDF up; never serve an entry
  because the key matched. `/typeset-page/` makes the same checks, and
  its pictures are kept by that real path too.
- **The browser keeps no place in a PDF it frames.** Its viewer offers
  the page no scroll position, and a new address loads from the top, so
  the reader's place needs pictures in a scroller of the page's own
  (`/typeset-page/`). Draw them with CoreGraphics, not PDFKit: they are
  drawn on the server's threads.
- **A preview made in code typesets at once.** `LatexView`'s init runs
  `compileNow` before its caller can set `onTypeset`, so the first
  "working" is never heard: whoever makes one says its state afterwards
  (`typesetOf:view:`), and `typesetting` counts the typing debounce, or a
  page told "failed" would wait for a typeset already due.
- **The review's reading follows links** (R3's fourth slice):
  `readUncommitted` reads the work tree with `readFileStable`, so an
  untracked link to a file outside the folder had its target's text in
  the diff. Every read the page gets goes through `confineReview`, which
  makes such a file a link's note; never send a `Read` that skipped it.
- **Nothing may wake a closed pipe.** `GitReads`' thread outlives the
  server; `~Server` resets `git_` before the wake pipe closes, and the
  wake is cleared under the reads' lock.

- **Never edit the storage from the hook.** `willReplaceCharacters` runs
  inside the storage's edit: it records and sends a message, nothing else
  (no layout, no other edit, no wait).
- **`_liveHighlight` decides** whether a storage edit is the active tab's
  source. It is off whenever the storage holds something else (a preview,
  a message), as CLAUDE.md's rule already says; an edit made with it off
  while the storage holds source would go unrecorded, and the mirror
  check fails the suite that made it.
- **A whole fill is never an edit.** `displaySourceEditable` and the
  large-file emptying in `makeEditorReadOnly` turn `_liveHighlight` off
  first; showing a tab again puts its own text in, which the host already
  has.
- **UTF-8 cannot carry a lone surrogate.** Opening and resetting a
  document send UTF-8 (`doc.open`, `doc.reset`); a text holding one (none
  read from a file: such a file goes to Quick Look) becomes U+FFFD in the
  host's copy, and the mirror check would say so.
- The host keeps a UTF-16 copy and its saved text, and the language server
  sync a third copy: about 150 MB more for a 25 MB file. Large-file mode
  has no server, so two.
- **`Client::dispatch()` takes only what was queued when it was called.**
  A listener that acknowledges terminal output as it takes it (the
  `term:*` rig, `SessionWeb`'s server thread) lets the host send more at
  once; when dispatch() ran until the queue was empty, it never returned
  while `yes` printed no slower than it was taken, and the caller's
  deadline was never read (core tests under TSan hung in `term:credit`,
  2026-11-18; a probe saw one call run 20 s). What arrives meanwhile gets
  its wake, so a renderer that dispatches from its wake loses nothing; a
  caller that wants what its own processing set off (a synchronous test
  net) calls it again, or `settle()`. `waitFor` reads its deadline after
  every message, and `SessionHost::inspect` waits at most its timeout, so
  a hang on the host's thread fails a check instead of hanging it
  (`term:dispatch`).
- **A scrolled window gives a scrollable child its own size**, whatever
  the child asks for (GTK's): VTE held at a larger pty never grew (step
  2), and growing it by negative margins drew a Gtk-WARNING on every pan
  (step 3). A child laid out larger or smaller than the view sits in a
  scrollable box of its own that passes the adjustments through
  (`linux/src/TerminalFrame.{h,cpp}`, "VTE pans").

## Terminals on the host (stage 5, the Mac)

With the setting on, a grid terminal opened from then on (`TerminalView
onSessionHost`, decided in `initWithDirectory:` for its life) has its
shell on the process's host: `Shared::Pty` on the host's thread, the
host's headless screen and events, and any other renderer of the host
(the browser's view, a scripted client) able to attach, see its screen
from a replay and type into it. Line mode keeps its MCPty: its log reads
the tty's echo flag and modes directly, and it is a fallback for a
release. With the setting off nothing changes, MCPty included.

- **Opening.** `startShell` builds the environment as before (the
  integration's ZDOTDIR and nonce, `BROWSER`, `MUTIBLE_TERMINAL_ID`,
  `MUTIBLE_PID`, the venv and IDE link variables) and sends it as a
  `TermSpec` (`/bin/zsh -zsh -i`, the grid's size, the scrollback, the
  nonce) under a new terminal id per shell (a restart is a new id, so an
  old shell's late `TermExited` never ends the new one). It then
  `settle`s, so the shell's pid (`TermOpened`) and the host's first look
  at the tty are there before anything asks whether a shell runs:
  `runCommand:` starts one only when `shellPid` is 0. The shell is the
  app's child, as MCPty's was, so the Processes panel's tracking,
  `KERN_PROC_TTY` for background jobs, `proc_name` for the close
  question and the exec-into-another-shell poll all work as before.
- **Output.** `MCHostTermListener` (one per view, for its life; anything
  for a terminal id the view has let go is ignored) appends the host's
  bytes to the backlog, which `drainBacklog` parses in slices as before
  and acknowledges (`ackTerminal`) as each slice is parsed, so the host
  stops reading a program past 1 MB the screen has not taken, as MCPty's
  reader did. The view's own `TerminalStream` and `TerminalScreen` read
  the same bytes, so marks, the integration's events, the typing queue,
  titles, links and find are the code they were; the host's `TermMarks`
  and `TermShell`'s marks are not used.
- **Input.** `sendBytes` is `terminalInput`: keys, pastes, `typeLine:`,
  the private ZLE keys, and the screen's own replies (DA, DSR, OSC
  colors), which the host's screen never answers. What one run-loop turn
  sends goes as one write (4 KB or more at once), as MCPty's queue wrote
  it back to back. **Trap (CI, 2026-11-17):** sent as two messages,
  terminal_paste's Ctrl+D then Ctrl+C reached the shell apart: the D
  ended cat, and the C came during zsh's precmd and cut its prompt mark
  off, so the terminal stayed "running cat". The screen's own replies
  (`takeReplies`) go apart from the keys (`sendReplies:`: the turn's keys
  flushed first, then `terminalInput(t, replies, true)`), marked as
  answers, so they never take the size (below).
- **Size** (2026-11-18, stage 6's rule followed): `updateWindowSize` says
  the view's size to the host (`resizeTerminal`, no pixel size:
  `TermResize` has none) and does not resize the screen. The screen
  takes the pty's size from `TermShell`'s `cols` and `rows`
  (`hostShell:`), at the place in the output where the host's screen
  reflowed: each new size is queued with the backlog's length then
  (`_ptySizes`), and `drainBacklog` parses up to it, resizes
  (`takePtySizes`), and goes on. So a program another screen sized is
  never reflowed to fit the window, and when the window types (keys,
  pastes, `runCommand:`, all `TermInput` without flag 1) the host gives
  it its own size before the keys are written. `placeGrid` lays the grid
  out at the screen's size: a smaller pty at the top left with the rest
  empty (the grid's frame is the view's bounds), a larger one in a frame
  larger than the view, clipped by it (`clipsToBounds`, which is off by
  default since macOS 14) and moved after each slice of output so the
  cursor's cell stays in sight, as tmux pans; scrolled back, it stays
  put. Off the host, and with no shell, the screen is the view's size
  as before.
- **The tty.** What read the master reads the host's `TermShell` (the
  host looks after every read, 30 ms later, and every 200 ms):
  `ptyForeground` (`tcgetpgrp`: `busy`, `canTypeNow`'s shell-owns-it,
  `foregroundJob`, background jobs, the close question, Claude Code's
  binary), `ptyRaw` (ICANON off: `canTypeNow`), `ptyHidesInput` (ECHO off
  with ICANON on: secure keyboard entry) and `ptySignalsOff` (ISIG off:
  the screen's `setRawTty`, zsh's widths at the prompt). `TermShell`
  gained `signalsOff` for that last one, and the host's own screen now
  uses it too (it used ICANON, so ZLE's prompt counted clusters there).
  Each `TermShell` that arrives runs the hidden-input check and the
  typing queue, since the raw tty `canTypeNow` waits for can come after
  the B mark's bytes.
- **Closing.** `stopShell` is `closeTerminal`: the host closes the
  master, sends SIGHUP, and reaps, SIGKILL 2 s on. The shell's exit is
  `TermExited` after its last output (`shellExited`, as before); a shell
  that could not start says why on the screen.
- **What other screens see.** Every renderer of the host sees every
  terminal on it (the state's `terminals`); the browser's view of one
  window (`SessionBrowser`) shows only that window's, which the window
  reports with its tabs (below, "One window's terminals"). Notes a renderer
  writes on its own screen ("Process exited", the shell's end) are only on
  that screen. Terminal size follows the renderer that last typed
  (stage 6's **Size**, and **Size** above): the browser's view says the
  size of the page that typed there last (`tsize`, below), so a page that
  only looks never shrinks the window's terminal.

Checks: `make guitest SUITES=terminal_host` (the setting turned on by the
suite: the shell is the host's; a command typed on the Mac is on the
host's screen and on a second renderer's, attached through
`MCSessionConnectViewer` with its replay; a command that renderer types
runs in the Mac's terminal and the marks see it end; the size follows
whoever typed: the second renderer's smaller size kept but not taken
until it types `stty size`, then the pty, the host and the Mac's grid at
its size and the grid's frame the view's, the Mac's `runCommand:` taking
its own size back, the second's answer (flag 1) moving nothing, a larger
size drawn in a larger frame clipped with the cursor's cell inside the
view; the terminal goes on after it leaves). `terminal_pty` checks the host's teardown with
`MCSessionInspectTerminal` (its master, write buffer and reaping) when
the host owns the pty. CI's pass with the host runs terminal_pty,
terminal_marks, terminal_paste, terminal_exec, terminal_keys, run_file,
attention and terminal_host. `make test`'s `term:host` checks the modes
the host reports (`echoOff`, `raw`, `signalsOff`).

## The headless host (stage 6, core and CLI)

`mutible host --folder DIR` runs the session's host for one folder with no
window, and `mutible attach --folder DIR --dump` is the first renderer that
reaches it over a socket. Both are in `mutible-cli` (the Mac's
`Contents/MacOS/mutible-cli`, and the GTK binary, which hands the control
channel's subcommands to the same `mutibleCliMain`), built from the core
alone: `src/SessionServer.{h,cpp}` over `SessionHost`. GTK attaches with
`editor.session-host = headless` (below, "GTK attaches"), and the Mac too
(below, "The Mac reattaches").

- **The socket.** `host-<FNV-1a of the resolved folder, 16 hex>.sock` in the
  first of `ControlChannel::socketFolders()` (ControlPaths.h, now shared by
  the CLI: `$MUTIBLE_CONTROL_DIR`, else `$XDG_RUNTIME_DIR/mutible` or
  `/tmp/mutible-<uid>` on Linux, `$TMPDIR/Mutible` and the user's own
  temporary folder on the Mac). The folder is made 0700 and used only when
  it is a real folder of the user's that nobody else can enter; the socket
  is 0600; the server refuses a peer of another uid (`getpeereid`,
  `SO_PEERCRED`), and the renderer refuses a host of another uid. The app's
  sweep of `<pid>.sock` files never touches `host-*` names
  (`pidFromSocketName` gives 0). `attach` checks that the host's state names
  the folder it asked for.
- **One host per folder.** A `host-<hash>.lock` beside the socket is
  `flock`ed for the host's life; a second host fails to take it and exits 0
  naming the first's socket. Holding the lock, any socket already there is a
  dead host's and is removed before `bind`. The lock file is never removed:
  unlinking it would let a host starting at that moment lock a fresh file
  while another still holds the old inode.
- **The wire.** One message a line, `encode()`/`decode()` of
  SessionProtocol (terminal bytes base64), the same as `MUTIBLE_HOST=loopback`.
  The server's own thread polls the listening socket, a wake pipe and every
  connection, non-blocking; the host's Sink (on the host's thread) only
  appends to a connection's buffer under a lock and wakes it. A connection
  whose buffer passes 512 MB, or that sends a line that is not a message,
  is dropped. `HostLink` is the renderer's side: blocking writes under a
  lock, a reader thread delivering to `Client::deliver`; `close()` shuts the
  socket down, joins the reader, then closes. No SIGPIPE either way
  (`MSG_NOSIGNAL`, `SO_NOSIGPIPE`).
- **Attaching** is what the protocol already had: Hello, `StateQuery`
  (the folder, windows, documents, and now terminals: id, size, pid, the
  shell's state, how many renderers show it), `DocOpen` with an empty text
  for each document wanted (the host answers with its text, which the
  client hands the listener as one insertion), `TermAttach` for each
  terminal (a `replay()`, then live output). **Terminals have several
  renderers now**: the one that opened it and every one that attached
  since all get its output, shell state and marks; the pty is read while
  every one of them has credit; `TermClose` from any ends it and tells the
  others (`TermExited`); a renderer that goes is dropped from each, and a
  terminal nobody shows keeps running, its output going only into the
  host's screen. Detaching (the connection closing) is `Host::disconnect`:
  documents, windows and terminals stay.
- **Size** (tmux's `window-size latest`, 2026-11-18). A renderer's
  `TermResize` is its view's size, kept as that renderer's
  (`Term::Viewer`). The pty and the host's screen take the size of the
  renderer showing the terminal that typed into it last and has said a
  size; before that, the opener's (its `TermSpec`) while it shows it;
  else the first renderer that attached with a size; with none, the pty
  keeps its own. The size moves before the typed bytes are written, and a
  renderer going hands it on by the same rule. Typing is `TermInput`
  without flag 1: flag 1 (`terminalInput(t, bytes, true)`) marks an
  emulator's answer to a program's query (DA, DSR, OSC colors), which
  every renderer's emulator sends and which would otherwise hand the size
  to whichever answered last. `TermShell` carries the pty's `cols` and
  `rows`, so every renderer hears a change where the host's screen
  reflowed in the stream. A renderer draws its emulator at that size,
  never reflowing a program's screen to its view: a larger pty clipped
  around the cursor (tmux pans), a smaller one at the top left with the
  rest empty. It keeps sending its view's size, so its next key gives the
  pty that size with no other message. With one renderer nothing changed. A
  size a program wrote back (bash's readline, Traps) is put back for 5 s
  after the host resized.
- **Lifetime** (the owner's answer): `run()` returns once there has been no
  connection for `idleMs` (10 minutes; `--idle-minutes`, fractions allowed,
  for checks) and `Host::busyReason()` is empty: no document dirty, no
  terminal whose integration marks a command running, and none whose
  foreground process group is not the shell's. While something is unsaved
  or running it lives on, looking every second (a quarter of the idle time
  when shorter), and ends at the first look after that clears. The count
  starts at launch and again whenever the last renderer goes. Ending
  destroys the `SessionHost`, which hangs every shell up (SIGKILL for one
  that stays), removes the socket and keeps the lock file.
- **Signals.** `mutible host` blocks SIGINT, SIGTERM and SIGHUP before any
  thread starts and takes them on a thread of its own with `sigwait`, which
  stops the server as the lifetime would; the shells get a clean mask
  (HostPty.cpp resets it after the fork). It runs in the foreground; detach
  it with `&` or `nohup`.
- **`attach --dump`** (`Shared::dumpSession`): `== host DIR`, then for each
  document `== document ID PATH (version N, saved|unsaved)` and its text,
  then for each terminal `== terminal ID (COLSxROWS, shell PID[, running
  CMD][, in CWD])` and its screen as a fresh `TerminalScreen` draws the
  host's replay (trailing blanks dropped). It attaches to the terminals to
  get the replay, then closes the link, which detaches.

### Checks

`make test`'s `hostserver:*` groups (`tests/session_server_tests.inc`): a
host on a scratch socket in the test process, renderers as `Client` plus
`HostLink`. One per folder (a second start is Running, a stale socket is
replaced, an open socket folder refused); two renderers typing at once
converge with the host, one detaches, the other edits on, it attaches again
to the same text and undoes the other's step; a terminal's output while
nobody is attached reaches the host's screen and the next renderer's
replay, two renderers hear the same output, and closing it ends the shell;
the lifetime with a 150 ms timer (no renderer, an unsaved document kept and
then saved, a running command by the marks, a foreground `sleep` in `sh -i`);
the command line's parsing. Real sockets and shells, under TSan in CI.
`term:size` (`tests/session_terminal_tests.inc`) holds the size rule to the
pty's own winsize and `stty size`: up to four renderers on one terminal,
the opener's size until another types, a resize from one that is not the
typist kept but not taken, an answer that is not typing, the typist going
(to the latest typist left, then the first attached), one alone, and a
stale size written back to the pty (as readline does) put back.

### Ids (done, 2026-11-18)

Each connection's Welcome carries an id base in `v`, a multiple of
`kIdRange` (2^32, SessionProtocol.h), and the renderer takes the ids of
everything it opens on the host from it: `Client::newId()` gives base + 1,
base + 2, ..., one counter for documents, terminals and windows. Bases are
slots 1 to 2^21 - 1, so every id is under 2^53 and the browser page's JSON
numbers (and Json's doubles) carry it exactly; Json writes every integer
under 2^53 as its digits. The host gives each slot once, passing over any
in whose range it holds an id all the same, then (after two million
connections) the first slot no live renderer has and in whose range no
document, terminal (dying ones included) or window is held
(`Host::chooseIdBase`). Ids below 2^32 are in no base's range.

- **What the host holds keeps its id.** A renderer attaching opens a held
  document or terminal under the id `StateQuery` names; only new things
  take new ids. A renderer that connects again gets a new base.
- **Settle after `connect()`**: the base comes in the answer to its Hello.
  Both apps do (`SessionLink` on GTK and the Mac); `newId()` before the
  Welcome was taken settles first, and gives 0, never an id, when no host
  answered (or none had a slot left, or 2^32 ids were used).
- GTK's documents (`Document`), terminals and windows take `newId()`; the
  window's host id is mapped from its `Model::Session` id in
  `SessionLink.cpp`. The Mac's `MCSessionDoc`, host terminals and the
  session browser's window do too.
- Nothing persisted holds a host id: sessions keep tabs by path, backups
  have their own UUIDs, and the save planner's ids last one run.
- Checks: `host:id-bases` (three renderers through the encoding, slots
  coming round, the largest base), `host:new-id-before-welcome`,
  `hostserver:id-bases` (documents and shells of two renderers over the
  socket) and `sessionweb:big-ids` (the page's JSON).

### For the GTK and Mac attach steps

- Tabs are still reported, not decided (4a's note): a second renderer reads
  `windows` from the state but cannot change them through the host until
  `Model::Window`'s callbacks are data.
- Size: done in the host (stage 6's **Size**), and followed by the Mac's
  grid ("Terminals on the host", **Size**) and the page (`tsize`, and the
  view following `TermShell`; 2026-11-18). GTK's VTE sends its answers
  as answers and follows the pty's size since step 2 ("GTK reattaches",
  below): clipped when larger, panned to the cursor since step 3, and at
  the pty's own grid when smaller, so plain output wraps where the pty
  wraps (2026-11-18, "VTE pans").
- The disk's questions are the app's while one of its windows has the
  document open; the host writes the file, for a browser's page (stage
  7's first write, "Saving through the host", below) and for the Mac's
  and GTK's windows (its second and third writes), and follows the disk
  for what no window has ("The host watches the disk", below). Backups are
  the app's while one of its windows has the document open, and the
  headless host's once none has (step 3, below).

### GTK attaches (stage 6, step 1)

The GTK app reaches its hosts through `linux/src/SessionLink.{h,cpp}`
(linux/HANDOFF.md item 99): one `SessionLink::Link` per folder its windows
show (resolved; `forFolder`), shared by every window on the folder, each
with its own `Client` in code points and its own ids (`newId` from that
client's base, so ids of two hosts never meet in one client).

- **Who holds a link.** The window (`App::link`, made in `newWindow`
  before the editor, and anew in `openFolder` after the old tabs closed:
  `Editor::setSessionLink` tells the old link the window closed and the new
  one its tabs). The editor opens each new `Document` on its link, and the
  document keeps it for its life (`Document::sessionLink`); each
  `Terminal` gets its window's link when made and keeps it, so a terminal
  started before Open Folder stays on the old folder's host. The language
  server sync follows a document's host text through that document's link
  (`watched` maps each id to its link). The harness's mirror check groups
  documents by link (`checkDocuments`) and checks the window's tabs on the
  window's (`checkWindow`). A link goes when the last holder lets it go:
  an in-process host's thread ends, a headless one is detached from.
- **Where the host is**, read when a link is made, so a GUI suite switches
  between windows with `EnvLock::envSet`. `MUTIBLE_HOST` wins: `socket`,
  `inprocess`, `loopback`. Unset, `editor.session-host = headless`
  (`Settings::sessionHostMode`, a third value; `sessionHost()` stays true
  for it, so the Mac takes it as `on`) means socket; `off` and `on` both
  mean in-process on Linux. **The default stays in-process until step 2**
  (reattaching, and closing a window detaching), or every folder a user
  opened would leave a host with nothing to reattach to it; the default
  flips on both apps together later.
- **Socket.** `findHostSocket(resolveFolder(root), socketFolders())` and
  `HostLink::open`; a socket nothing listens on, or none, gets a host
  started as this program (`/proc/self/exe host --folder DIR`, with
  `--idle-minutes` from `MUTIBLE_HOST_IDLE_MINUTES`): forked twice with
  `setsid` between, so init (or the nearest subreaper) is its parent and
  reaps it; stdin `/dev/null`, stdout and stderr
  `$XDG_STATE_HOME/mutible/host-<hash>.log`, nothing else of the app's
  open files (`close_range`), signals at their defaults, the environment
  copied under `EnvLock::EnvRead` before the fork. The window waits up to
  5 s for the socket (the main thread, 20 ms polls; a host starts in a few
  milliseconds), connects, settles and checks `idBase()`. A host that
  answers and refuses (another user's), a host that does not come, or no
  Welcome: the in-process host for that folder, and one stderr line saying
  why. The GUI harness's binary takes the subcommands too (Harness.cpp's
  `main`, before anything of the checks), since the host it starts is
  `mutible-guitest host`.
- **Delivery** is as before: the link's reader thread (or the in-process
  host's) calls `Client::deliver`, whose wake queues one main-loop dispatch
  of the link; the idle source holds the link weakly (`Link::Wake`), so no
  other thread ever holds the link itself.
- **The host going** (killed, its socket closed): `HostLink`'s `closed`
  calls `Client::hostLost` (new in the core, additive; `HostLink::connect`
  does it too), after which the client sends nothing, every wait (settle,
  undo, `undoInfo`, the queries, a first `newId`) returns at once, and
  `undoInfo` says nothing can be undone (`hostserver:host-gone`). The link
  says so once on stderr and to its listeners: the window's status bar
  (`flashStatus`) and each terminal on it, which writes a line, drops its
  shell (it ended with the host's pty) and starts none again. Documents
  stay open and editable and save as before (saving is the app's); undo
  and redo do nothing in them. Step 2 reconnects (below).
- **Quitting** calls `SessionLink::detachAll` (the shutdown handler):
  every headless host is detached from and runs on until its lifetime
  ends. Closing a window detaches since step 2 (below).
- **Checks.** The harness sets `MUTIBLE_HOST=inprocess` for every suite
  (Wrapper.cpp, kept when a run asks for loopback), and its socket folder
  is the scratch world's (`XDG_RUNTIME_DIR` under `/tmp/mcgui-*`). The
  `host_link` suite: a window on a scratch folder under
  `MUTIBLE_HOST=socket` starts a host (its socket in the scratch folder,
  not the app's child), typing reaches it (`mutible attach --dump`, the
  real binary, prints the text unsaved), undo is the host's, a terminal's
  shell is the host's child and its output is in the dump, a second window
  shares the link, closing both leaves the host running and a new window
  finds it, the host killed by its pid is reported in the status bar and
  the terminal with nothing waiting; then `editor.session-host = headless`
  with `MUTIBLE_HOST` unset starts one, and `on` keeps it in the app. Each
  host it starts it ends by its pid (SIGKILL, or SIGTERM to see it stop);
  the wrapper's subreaper reaps them and would end any left.

### GTK reattaches (stage 6, step 2)

Closing a window detaches from its folder's host instead of ending what
it shows, and a window opening on the folder takes what a closed one
left (linux/HANDOFF.md item 101). Both host modes: a headless host keeps
it after the app quits; an in-process one while the app runs.

- **The protocol, additive.** Three renderer-to-host kinds, new rather
  than flags on DocClose and TermClose, so a host from before them answers
  "the host takes no ..." instead of closing what the renderer meant to
  keep: `DocPath` (doc, name: the file's path now; `Client::setPath`),
  `DocDetach` (doc: the renderer's seat goes, the document stays with its
  text and history even with no seat left, as when a connection goes;
  `Client::detach` sends the document's queued edits first and forgets the
  entry once the DocDetach is out) and `TermDetach` (term: the renderer
  stops being a viewer, so its credit holds nothing back; the shell runs
  on; `Client::detachTerminal`). The state's windows gain `connected`
  (whether the renderer that reported the record is still connected) and
  `seen` (the host's order of its last WindowTabs). `Client::clientId()`
  and `showsTerminal()` are new accessors, and
  `Shared::termLooksLikeAnswer` says whether bytes read as an emulator's
  answers (below). Checks: `host:detach`, `host:window-records`,
  `host:term-answers`.
- **Paths.** A GTK document tells the host its path when its text is set
  from a file (`resetHistory`) and on Save As or a rename (`setPath`);
  `tellHostPath` sends only a change.
- **Closing** (main.cpp `detachWindow`, from `onWindowRemoved`): the
  window's terminals are reported in its record (`"terms"`, as the Mac's
  window has them; `Link::windowTerminals`) and detached
  (`Terminal::detachFromHost`); the editor sends its record once more
  (`Link::windowDetached`, so its `seen` is the latest; a window with no
  tabs and no terminals sends WindowClose instead) and detaches every
  document (`Editor::detachFromHost`) but those the close question's
  Don't Save named, which close on the host with their tabs; then each
  link settles (what was typed goes before a headless link can be let
  go of). An explicit close of a tab or terminal still closes it on the
  host.
- **The question.** Kept, since an app's window still saves its own files
  (the host saves only what no app's window has open: "Saving through the
  host", below). On
  a headless host it has a fourth answer, Keep Unsaved: the unsaved files
  stay unsaved on the host for the folder's next window (and keep the
  host alive by its lifetime rule). In process the answers stay Save,
  Don't Save and Cancel, since the host ends with the app; unsaved files
  are never kept there, only saved or clean ones and terminals. Don't Save
  closes the files on the host; Cancel keeps the window.
- **Busy terminals.** No question at a window's close: the window close
  never asked on GTK, and closing now detaches, so a running command goes
  on (the core's lifetime rule keeps a headless host alive while one
  runs). Quitting with an in-process host ends the shells as before; the
  Mac's own close question is unchanged.
- **Opening** (`Link::takeForWindow`, from `newWindow` as the editor is
  made, and from Open Folder): a record is free when no window of this
  app has it and its renderer is gone or is this app's. The window takes
  the free record with the highest `seen` (the latest a closed window
  left) and reports its tabs under that record's id from then on; it gets
  the documents and terminals that record names and the host holds, then
  every held document (with a path) and terminal that no record names.
  What a record of an open window (here or another renderer's) names is
  that window's and left alone. **Two windows on one folder** each leave
  a record; the next window takes the latest, the one after it the other.
- **Documents** come back before the session restores the rest
  (`attachHeldDocuments`, then `Persistence::restoreSession`, which passes
  over a path open already: the host's live text keeps the file's tab,
  and a backup of it a process that went left is removed only when it
  holds that text, else it opens beside it as "a (unsaved copy).txt",
  since 2026-11-19, `linux/HANDOFF.md` item 124). `Editor::attachTab`
  makes the
  `Document` under the held id (`DocOpen` with an empty text: the host's
  text comes back as one insertion, which records nothing), and
  `Document::attachHeld` reads the file, whose text counts as saved, so
  the tab is unsaved when the two differ. The undo history is the host's.
  After the session, the record's tab on show is put on show.
- **Terminals** go to the dock as the record's layout has them (below,
  "Terminal splits in the record"; a tab each for a record without one):
  `makeTerminal` gives each its held id (`Terminal::attachHeld`,
  the pid and the host's TermShell at once, so busy() and the title work
  before it is shown), and when VTE is made it attaches (`attachShell`:
  TermAttach, the replay fed to VTE, then live output; the shell is zsh
  or bash by `/proc/<pid>/comm`, and the integration's word is the host's,
  since its nonce never reaches this renderer).
- **In process**, a link holding anything a closed window left is kept
  while the app runs (`SessionLink::keepWhileHolding`; `detachAll` lets
  them go at quit). The GUI harness ends what no open window shows
  between suites (`SessionLink::endDetached`, in `checkWhatSuiteLeft`),
  as it puts the two files back, so one suite's detached terminals never
  come back in the next suite's window.
- **The host gone, then back.** `forFolder` passes a lost link over, so
  the next window on the folder, or the status bar's Reconnect (shown
  while the host is gone), finds or starts the host again; every window on
  the old link moves to the new one (`rehostWindows`): each document opens
  on it anew under a new id with its buffer's text (`Document::rehost`:
  nothing typed is lost; its undo went with the old host; the file's text
  is the host's saved text and the difference an edit outside the
  history, `hostSavedFromDisk`, so the host counts it unsaved and its
  lifetime keeps it, as a backup restored now is too), and each
  terminal there starts a new shell (`Terminal::rehost`).
- **VTE as a renderer among several.** What VTE commits with no key
  pressed (`userInput_`) and that reads as answers
  (`termLooksLikeAnswer`: CSI ending in c, n, R, t, x or y, an OSC, a
  DCS, each whole) goes as an answer (`terminalInput(..., true)`), so it
  never takes the pty's size. TermShell's cols and rows are followed
  (`followPtySize`): while another renderer's size is in force, VTE has
  exactly the pty's columns and rows, laid out by its frame
  (`TerminalFrame`, "VTE pans" below), clipped where it is larger than
  the view and at the top left, the rest of the view the terminal's
  background, where it is smaller; plain lines wrap at the pty's width.
  The view's own size, from the frame, is still sent as this renderer's
  (`viewCells`), and is from the frame whenever VTE's cell is known, held
  or not: **trap**, VTE's grid is still the held one until VTE is laid
  out again after it is let go, and a size tick sent it then as the
  view's own, so the window's typing took the pty to 104x10 and back to
  40x6 (`host_wrap` caught it). The part on show follows the cursor since step 3
  (below). A size this view sent itself, or a VTE not mapped, is never
  followed (`sentSizes_`, those still on their way: once the latest
  comes back, the host having fitted in order, those before it go).
  **Trap:** measuring a VTE that is not on screen (its cell size, from
  every TermShell) left a second window's hidden terminal showing nothing
  it was typed (`processes`, its second window); `followPtySize` returns
  before it measures (`measureCells`), and the frame never measures a
  cell: it is told sizes in pixels, and a new cell while held comes with
  `char-size-changed`.
- **Checks**: `make guitest`'s `host_reattach` (linux/tests/gui/
  HostReattachSuite.cpp; its header says what it checks), with
  `_host_reattach_child`, a second app process, for quit and relaunch.

### The host's own backups, untitled buffers, one path twice, panning (stage 6, step 3)

The host-side items left before the default can flip (2026-11-18).

- **Backups of what no window shows** (`Host::keepBackups`,
  SessionHost.h's "Backups"; `HostOptions::backupDir`). `mutible host`
  keeps them in the apps' own store and format (`Backups::defaultDir`:
  `$MUTIBLE_BACKUP_DIR`, else `~/.local/share/mutible/Backups`, the Mac's
  `~/Library/Application Support/Mutible/Backups`), owned by the host's
  pid and start (`Backups::processStart`, each system's form, so the
  apps' `isLive` reads them), so no app takes one while the host lives.
  A document needs one when it is unsaved (dirty, or with no path and
  some text) and no app's window has it open: an app is a renderer whose
  Hello says it keeps backups (flag 4, `Client::setKeepsBackups`; GTK
  sets it on every link, the Mac on the process's client and every folder
  link's; a browser's view, a guest or `mutible attach` does not). Written at once when the last app seat goes (DocDetach, a
  connection lost), then after edits from other renderers as the apps
  time theirs (`SavePlan::Planner` on the host's thread, its due time
  folded into `tick()`'s wait: a second after typing stops, at most five
  after the first change not yet written); removed when the document is
  saved, closed, or opened again by an app (whose own backup takes over:
  GTK writes its own at once after reattaching, `backUpHeldDocuments`,
  the Mac in `finishSessionAttach`).
  What a stop by signal leaves unwritten is written as the host's thread
  ends (`writeBackupsNow`). The entry's base is the file as the host finds
  it when writing: its stamp when it reads as the host's saved text, a
  stamp that cannot match when it holds something else (a restore then
  asks Keep Mine or Reload), never there when it is missing and nothing
  was saved. Writes are synchronous on the host's thread (an fsync each;
  only for documents no window shows). After a crash, a kill or a reboot
  the folder's next window restores them as its own (`planForRoot`), and
  a launch opens a window for the folder (`foldersToReopen`).
- **An untitled buffer** (a held document with no path, which only
  another renderer makes): unsaved whatever its history says while it
  holds text (`busyReason` keeps the host for it), and backed up under a
  name in the folder that no file and no other buffer has
  (`Backups::untitledPath`: Untitled-1, -2, ...); nothing is written
  there. A window reattaching takes it (`Shared::reattachFrom`, below)
  and shows it as the tab of that name, a file not on disk yet, unsaved
  (`Editor::attachTab(id, name, false)`); the host keeps it pathless until
  Ctrl+S writes the file, which tells the host the path
  (`Document::tellHostPath` from `Editor::saveDocument`). A pathless
  document with no text and nothing unsaved (a renderer's document before
  it had a file) is never taken. GTK's `hostSavedFromDisk` now counts a
  buffer whose file is not on disk as all unsaved (saved text ""), so a
  restored backup of such a file keeps the host alive and backed up.
- **What a window takes** moved to the core (`Shared::reattachFrom`,
  SessionClient.h; GTK's `Link::free` and the Mac's `MCSession::Link::free`
  call it): the latest free record and what it names, then what no record
  names, oldest id first. **Never two documents of one path** in one
  window: the record's own first, then those no record names; the others
  stay on the host for the folder's next window, so two windows on one
  file, both closed unsaved, come back as the latest one's text in the
  next window and the other's in the window after it. `all` (the checks'
  `endDetached`) takes everything free.
- **VTE pans** (`Terminal::panToCursor`, on `cursor-moved` only while
  clipped, and when the clipping starts): the first row and column on
  show (`panRow`, `panCol`) move only as far as keeps the cursor's cell
  in the frame. **Step 2's clipping had never grown VTE**: the
  scrolled window gives a scrollable child its own size whatever it asks
  for, so VTE stayed at the view's grid (104x10 against a 300x120 pty in
  `host_pan`). Step 3 grew it by negative margins, which drew a
  Gtk-WARNING ("adjusted size ... must not decrease below min") on every
  pan; since 2026-11-18 VTE sits in a frame instead
  (`linux/src/TerminalFrame.{h,cpp}`, a GtkWidget implementing
  GtkScrollable): the frame is the scrolled window's child and passes
  its adjustments and scroll policies through to VTE, so the scroll bar,
  the wheel and the scrollback are VTE's as before; it measures as VTE
  does (VTE's natural size is its default 80x24 grid whatever it is laid
  out at, so a held grid asks the view for nothing and a pane beside it
  never moves), lays VTE out at its own size, or at the
  size and offset `Terminal::placeGrid` holds it at (the pty's cells and
  VTE's padding, moved up and left by the pan), clips it (overflow
  hidden), and paints the terminal's background (`applyColors`) to the
  right of and below VTE, never under it (a translucent color would
  double). A press in that empty part gives VTE the keyboard. The
  cursor's row counts from the top of the program's screen (VTE's last
  rows). Released (`releasePtySize`), VTE has the frame's size at (0, 0)
  again. **A smaller pty** gets the same hold with no pan: VTE has the
  pty's columns and rows, so a line longer than the pty with no cursor
  positioning wraps where the pty wraps (VTE rewraps what it has when
  held, so output that came before the TermShell wraps there too).
- **Checks**: `make test`'s `hostbackup:helpers`, `hostbackup:detach-
  unsaved`, `hostbackup:untitled`, `hostbackup:restore` (over the socket,
  stopped as a signal stops it), `reattach:two-of-one-path`,
  `reattach:untitled` (tests/session_backups_tests.inc); `make guitest`'s
  `host_backups` (the host's backup at Keep Unsaved, reattached while it
  lives, an untitled buffer a client left, the host SIGKILLed and a new
  window restoring both, two windows on one file), `host_pan` (with no
  Gtk-WARNING or Gtk-CRITICAL while it pans: `MCGuiLogWatchStart`, the
  harness's GLib log writer) and `host_wrap` (a smaller pty: VTE's grid
  is the pty's, the line wraps at its width, the empty part is VTE's
  background by pixels, the wheel, a drag's selection and a click's
  focus still VTE's, and the window typing takes the pty back in one
  step).

### Terminal splits in the record (stage 6, 2026-11-18)

A window's record keeps how its dock's terminals sit, so the folder's
next window puts split panes back as splits (linux/HANDOFF.md item 106).
`src/TermDockRecord.{h,cpp}` (core, `Shared::TermDock`) writes and reads
it for both apps; both do (the Mac's half below, "The Mac's terminal
splits").

- **The field.** `"termLayout"` in the record's tabs (the WindowTabs
  json), beside `"terms"`, which stays the flat list it was (an app
  before the layout reads `terms` alone; the browser's view still reads
  only `terms`):

      "terms": [11, 12, 13, 14],
      "termLayout": {
        "tabs": [
          {"columns": [[11], [12, 13]],
           "columnShares": [0.6, 0.4],
           "rowShares": [[1], [0.25, 0.75]],
           "activeColumn": 1, "activeRow": 0},
          {"columns": [[14]], "columnShares": [1], "rowShares": [[1]],
           "activeColumn": 0, "activeRow": 0}
        ],
        "activeTab": 1
      }

  One entry per dock tab, in the strip's order. `columns` holds host
  terminal ids, columns left to right, each top to bottom: the shape of
  `TerminalLayout::Layout`, which both docks use (GTK's `TerminalGroup`,
  the Mac's `MCTerminalPanel`), one level deep, so a split's direction is
  where its pane is (Split Terminal makes a column, Split Terminal Down
  stacks a pane in its column). `columnShares` are the columns' shares of
  the width, `rowShares[c]` the panes' shares of column c's height (the
  layout's fractions, written to four places), `activeColumn` and
  `activeRow` the tab's pane with the keyboard, `activeTab` the tab on
  show. The keys are the window list's for a terminal tab
  (TabSessions.cpp's `encodeTerminalTab`) with ids where that has panes.
  A pane not on the window's host (a terminal from before Open Folder) or
  with no shell yet is left out of `terms` and of the layout alike
  (`termLayoutJson` drops id 0, its share to its neighbour); a dock with
  none writes no `termLayout`.
- **Reading** (`termDockFrom(Reattach::tabs, ids of Reattach::terms)`,
  after `reattachFrom`): only the terminals the window took, each once.
  A pane naming one that is gone (its shell ended while no window showed
  it: the host ends the terminal and its state no longer lists it) or
  that another window shows drops out, its share given to its neighbour
  as closing it would (`TerminalLayout::removePane`), the others keeping
  their places; the active pane, if it went, is the one the keyboard
  would move to (above, else below; a column gone, the column before,
  else after); a tab left with none goes, the tab on show moving as
  closing it would. Shares that do not read or do not match their panes
  are equal ones. Taken terminals the layout does not place (a record
  with no `termLayout`, terminals no record names) follow as a tab each
  in `terms` order, and with no layout the first tab is on show.
  `TermDock::order()` is the order a dock makes them back: tab by tab,
  column by column, top to bottom.
- **GTK writes it** in `reportTerminals` (main.cpp), from each
  `TerminalGroup`'s columns, layout and active pane, on every dock change
  (`onChanged`: a split, a pane closed, a tab added, closed, moved or
  selected, a pane given the keyboard) and when a boundary moves (a bar
  dragged, the keyboard's resize: `TerminalGroup::onSharesChanged`, told
  by the dock 300 ms after the last move, `TerminalDock::onSharesChanged`),
  and once more as the window detaches. `Link::windowTerminals` keeps it
  beside `terms`; a record that changes only past the fourth place of a
  share is not sent again.
- **GTK rebuilds it** (`takeHeldTerminals`, `buildHeldTerminals`): the
  held terminals queued in `order()`, so `makeTerminal` gives each pane
  its held id as the dock makes it; the dock's first tab (made with the
  window) is the layout's first; `TerminalDock::restoreTab` makes the
  rest of each tab's panes (`TerminalGroup::append`: a new column or the
  foot of the last, with no size rule, since the record's panes fit
  where they were) and sets the shares (`setShares`) and the active
  pane; then the record's tab on show is shown with its active pane as
  the window's terminal (`focusPane(t, false)`: the keyboard stays where
  it was). Open Folder adds the new folder's tabs after the dock's own.
  **Trap:** with the keyboard in the dock, GtkStack showing another tab
  gives it to that tab's first pane (`gtk_widget_child_focus`), whose
  focus-in took the tab's active place, so every tab switch (a click on
  the strip, Next Terminal) lost the pane the record named and put the
  first one in it (`host_splits` saw the restored pane replaced as soon as
  the dock was shown and a tab picked). `TerminalDock::activate` now
  ignores focus-ins while it switches and gives the keyboard, when the
  dock had it, to the tab's active pane.
- **The Mac's terminal splits** (2026-11-18; with `editor.session-host`
  on or headless, the default unchanged):
  - **Writing.** `MCSessionWindow`'s record sets `termLayout` from its
    `termDock` block (`MCTerminalPanel sessionDock:`: each tab's columns as
    `sessionTerminal` ids, 0 for one not on a host, its layout and active
    pane, the tab on show) through `Shared::termLayoutJson`, beside `terms`
    (the panel's terminals tab by tab, so the same order). The record still
    goes with the half second's report, and at once on a dock change: the
    panel's `onChanged` (a tab added, closed, moved or shown, a split, a
    pane closed or given the keyboard, a keyboard resize, a bar's drag
    ended) calls `sessionDockChanged`, which asks `reportSoon` (one report
    on the main queue's next turn). **A drag is written once**: while a
    bar is dragged its tab's shares in `sessionDock:` are the ones from
    before the drag, so a report mid-drag sends nothing new, and the
    drag's end (`DragBar onDragEnd`, on mouse up) sends the new ones. The
    detach sends it once more, as before.
  - **Reading.** `takeHeld` makes `heldDock`
    (`Shared::termDockFrom(Held::tabs, the held terminals' ids)`) and puts
    the held terminals in its `order()`, so `nextTermPlace` gives each pane
    the panel makes its terminal. `finishSessionAttach` shows the dock (its
    first terminal takes the first) and calls `restoreHostDock:fresh:`,
    which makes the rest of each tab's panes column by column, top to
    bottom, sets the shares and each tab's active pane, and shows the
    record's tab on show; the keyboard stays where it was. A panel that
    was there already (Open Folder) keeps its own tabs first.
  - **A window brought back at launch** that takes held terminals builds
    its dock from the record, not from the window list
    (`restoreWindowLayout` asks `sessionHoldsTerminals` and keeps only the
    dock's height): the host's terminals are the ones running, and the
    record says where they sat. Names given with Rename Terminal are in
    neither record's layout yet.
- **Checks**: `make test`'s `termdock:*` (tests/term_dock_record_tests.inc:
  the exact shape, a record without the layout, terminals gone from the
  middle, top or a whole column with the neighbours and active pane
  following, whole tabs, records that do not read, through
  `reattachFrom`), and `make guitest`'s `host_splits` (HostSplitsSuite.cpp:
  over a headless host, split right then down with boundaries moved and a
  second tab, a plain detach, back the same; a shell killed while
  detached, its pane left out; the record rewritten without the layout,
  a tab each). On the Mac, `make guitest SUITES=host_reattach`'s splits
  part (HostReattachSuite.mm's header): with the process's host, a tab
  split right then down in the new column with both bars dragged (the
  host's record keeps the old shares mid-drag and takes the new ones at
  mouse up), the top right pane active and a second tab on show, back the
  same after close and reopen (ids, columns, shares within 0.01 and laid
  out so, active pane and tab); then the record rewritten in GTK's shape
  from JSON text, with another arrangement and a pane naming a terminal
  that is not there, back as that arrangement with the pane gone, its
  share given to its neighbour and the active pane moved to the one below.

**Left before the default flips:** the browser's view lists a closed
window's record among the windows.

### The Mac reattaches (stage 6, step 2)

The Mac's half of the above, with GTK's behavior and words (Keep Unsaved,
Reconnect, the status bar's sentences). `mac/src/SessionLink.{h,mm}` now
has links (`MCSession::Link`), `mac/src/SessionWindow.{h,mm}` a window's
place on its link, and `EditorController+Session.mm` the window's hooks.

- **Links.** The process's link (`processLink()`) wraps the one in-process
  host and client there always were: `editor.session-host = on`, and the
  fallback. With `headless` (or `MUTIBLE_HOST=socket`; any other
  `MUTIBLE_HOST` keeps the process's), `forFolder` gives one link per
  resolved folder: the folder's `mutible host`, found by its socket or
  started as `mutible-cli host --folder DIR` from beside the executable
  (the bundle's Contents/MacOS, build/ for the checks) with `posix_spawn`
  (`POSIX_SPAWN_SETSID`, `CLOEXEC_DEFAULT`, signals at their defaults,
  stdout and stderr to `~/Library/Logs/Mutible/host-<hash>.log`,
  `--idle-minutes` from `MUTIBLE_HOST_IDLE_MINUTES`), reaped by a thread
  of its own when it ends while the app runs. The window waits up to 5 s
  for its socket, connects (`HostLink::connect`), settles and checks
  `idBase()`; anything failing logs why (os_log, category session) and
  the window takes the process's link. A link's wake dispatches on the
  main queue through a weak pointer. Each `MCSessionDoc` and host
  `TerminalView` keeps the link it was opened on (`MCSessionLinkRef`):
  a tab's is its window's (`MCTabOwner sessionLinkForTab:`), else the
  live folder link holding its path (`linkForPath`), else the process's;
  a terminal's comes from its panel (`MCTerminalPanel sessionPlace`).
  `MCSessionCheckTabs` checks each tab against its own link's host.
- **The record.** `MCSessionWindow` (an associated object of the
  controller, so no new ivars) holds the window's link, its id there
  (`Link::newWindow`) and its record, the shape `MCSessionBrowser` used
  to report, now sent every half second while the setting is on
  (`Link::windowTabs` sends only a change), whether or not a page
  serves it. The page's server takes the window's id and link from it
  (a folder link's page attaches over the host's socket, `hostSocket`)
  and no longer closes the record when it stops.
- **Opening** (`attachSessionHeld`, before `restoreWindowLayout`, and
  `finishSessionAttach` after `finishRestoringWindow`; Open Folder calls
  `sessionFolderChanged` and the same two around its `restoreSession`).
  `Link::takeForWindow` is GTK's `free()`, plus one rule for the
  process's link, which serves every folder: only records whose `root` is
  the window's folder, and only unnamed held documents under it (unnamed
  terminals only on a folder's host). Each held document becomes a tab
  (`MCTab attachHeld:link:`: `DocOpen` with an empty text, the host's text
  taken as one insertion while `initWithTab:held:link:` settles, set as
  the tab's text without telling the host); unsaved when the file on disk
  differs. The tab keeps `heldFromHost` until first shown:
  `loadActiveTab` then reads the file as the saved text without
  forgetting the history or resetting the host. `restoreSession` passes
  over a path open already and removes that path's backup. Held
  terminals wait in the window (`nextTermPlace`): a window brought back
  with terminal tabs gives them to those, and the rest open as tabs in
  the dock, which is shown. A `TerminalView` given a held terminal
  (`attachHeldShell`) makes a fresh screen at the host's size, attaches
  (the replay, then output; replay bytes are never acknowledged), and
  says its own size. Its marks count only with the nonce this process gave
  the shell (`HostNonces`); its prompt state starts from the host's
  `TermShell`. A shell from before a relaunch has no nonce here and runs
  without the integration's marks (the foreground decides, as after
  `exec bash`).
- **Closing** (`detachFromSessionHost`, first in `windowWillClose:`, and
  for every window in `applicationWillTerminate:`): the record goes once
  more, marked `"detached": true` (`Link::windowDetached`; `WindowClose`
  instead when it names nothing), each terminal `detachFromSessionHost`s
  (`TermDetach`; its pid forgotten, so closing the view hangs nothing up),
  each document `detach`es (`DocDetach`) but those the question's Don't
  Save named, which close; then every link involved settles.
- **The question** (`confirmCloseAllTabsClosing:`, from
  `windowShouldClose:` and quitting): on a headless host that is there it
  is Save (Save All), Keep Unsaved, Don't Save, Cancel (1000 to 1003),
  with GTK's detail line. Otherwise the three answers as before; either
  way the tabs still unsaved after a Don't Save close on the host.
- **Paths.** `MCTab setCurrentPath:` tells its document (`pathChanged`:
  `DocPath` when it differs from what the host has).
- **The host gone.** The window's lost listener flashes GTK's sentence
  and adds a Reconnect button to the status bar (left of ↻); each host
  terminal on the link writes a line and starts no shell. Reconnect
  (`reconnectSessionHost:`), or a new window on the folder, makes a new
  link, and every window still on the lost one moves (`moveToSessionLink:`):
  its record anew, each document `rehostOn:saved:` (a new id, the buffer's
  text, the file's text as the host's saved text and the difference an
  edit outside the history, so the host counts it unsaved), each terminal
  `rehostOn:` (a new shell). A host that cannot be had again: the
  process's link takes them, so nothing typed is lost either way.
- **The page** (`Web::windowOpen`, in `lookAtState`): a record whose
  renderer is gone (`connected` false) or that a closing window left
  (`"detached"`, which GTK's `windowDetached` now sets too) is no window
  on the page; its documents stay in the page's list as the host's, and a
  window taking the record over shows again (`sessionweb:closed-windows`).
- **Checks.** `make guitest SUITES=host_reattach` (HostReattachSuite.mm;
  its header says what it checks): with `on`, a closed window's saved
  file and running shell back in the folder's next window under the same
  ids, the replay, another command; with `headless`, a host the window
  started, Keep Unsaved, `attach --dump` showing the file unsaved and the
  shell, the next window's tab back with the typed text and unsaved, the
  same shell; the host killed, Reconnect shown, typing on, Reconnect
  starting a new host with everything typed unsaved there and a new
  shell; Don't Save. The harness ends what closed windows left between
  suites (`MCSession::endDetached`).
- **Quitting** (hot exit, 2026-11-18, `docs/notes/sessions.md`): with
  `files.hot-exit = on-exit` quitting asks nothing, on a headless host
  too: each document detaches as Keep Unsaved leaves it (nothing is
  dropped, `sessionDropOnClose:nil`), and the Mac's backups are written
  besides, as a restart's always were; the next window takes the host's
  documents and removes the backups of their paths. With `off` quitting
  asks Keep Unsaved as before.

### The Mac's half of step 3

GTK's step 3 ("The host's own backups, untitled buffers, one path twice,
panning", above), on the Mac. All of it applies with
`editor.session-host` on or headless; the default (off) is unchanged.

- **Hello flag 4.** The process's client (`Start`) and each folder link's
  (`startSocket`) call `setKeepsBackups(true)`; `MCSessionConnectViewer`'s
  clients (the browser's view) do not.
- **The backup folder.** A host the Mac starts gets `MUTIBLE_BACKUP_DIR`
  set to `[MCBackupStore shared].directory` (`MCSessionHostEnvironment`):
  the CLI finds the store by `$HOME`, and the app's is by
  `NSHomeDirectory` (`CFFIXED_USER_HOME` in the checks), or
  `$MUTIBLE_BACKUP_DIR` already. Under `MUTIBLE_DEMO` and `MUTIBLE_BENCH`
  (no store) the environment is passed as it is.
- **What a window takes** is `Shared::reattachFrom`'s (`Link::free`). The
  process's link serves every folder, so before the core chooses, a record
  whose `root` is another folder is added to the open windows' ids (left
  alone, with what it names), and of the documents only those a record
  names or under the folder stay, and of the terminals only those a record
  names. `takeForWindow` closes the window's own id on the host when it
  takes a record (WindowClose, whether or not it was ever reported).
- **An untitled buffer** (a held document with no path) becomes the tab of
  `Backups::untitledPath` in the folder as the host names it (the link's,
  resolved: `/private/tmp` where the window's root says `/tmp`, so the tab
  is the file the host's backup names; no file, no tab and no other held
  document of that name), marked `MCTab untitled`: unsaved, not
  "(deleted)", no disk stamp, never auto-saved. The Mac has no untitled
  tabs of its own (New File asks for a name and makes the file), so it is
  a file's tab whose file is not on disk yet. Its `MCSessionDoc` has the
  host's path "" (`HostPathOf`), so a rename tells the host nothing; Cmd+S
  writes the file, clears `untitled` and sends `DocPath` before
  `DocSaved` (`saveTab:` in EditorController+Tabs.mm).
- **Its own backups at once** (`finishSessionAttach`, after the session
  restored the rest): `writeBackups` when any held document became a tab,
  since the host let go of its own as the tab opened it.
- **A file not on disk is wholly unsaved** (`MCSessionDoc hostSaved:`,
  GTK's `hostSavedFromDisk`: the host's copy reset to the saved text, the
  difference an edit outside the history). `rehostOn:saved:` takes ""
  for an untitled tab, a gone one, or a file that is not there
  (`hostSavedTextOf:`); a backup restored when its tab is first shown
  (`loadActiveTab`, `sessionRestoredBackupOf:saved:`) counts against the
  file's text, "" when gone, where it had been the host's saved text; a
  held document the host has saved whose file is gone from disk is reset
  that way as it attaches (its undo history goes), so a headless host
  keeps it, and backs it up, if this window leaves it again.
- **Checks:** `host_reattach`'s step 3 part (HostReattachSuite.mm's
  header): the host started with `MUTIBLE_BACKUP_DIR` out of the app's
  environment backs up into the app's store under its pid at Keep
  Unsaved, the window's backup gone; a saved file deleted on disk, an
  untitled buffer and a second document of the unsaved file (a script
  over the socket) left on the host; the next window has one tab of the
  file (the record's document, the other still held), the deleted file
  unsaved and gone (and unsaved in `attach --dump`), Untitled-1 unsaved
  and not on disk with the host keeping no path, its own backups written
  as it opened and the host's gone; Cmd+S writes Untitled-1 and the dump
  then shows its path, saved.
- **Trap:** `stringByResolvingSymlinksInPath` drops `/private` from
  `/private/tmp/...`, so the Mac's backup root (`sessionKey`) for a folder
  under /tmp is not the host's (`resolveFolder`, realpath): a host's
  backup of a folder under /tmp is not found by the window's restore.
  Real folders under the home are the same either way; the checks use a
  realpath'd folder.

## The browser's view (stage 6b)

`mutible host --folder DIR --web PORT`, or `mutible attach --folder DIR
--web PORT` for a host already running, serves a read-only view of the
session to a browser: `src/SessionWeb.{h,cpp}` (the server, the request
rules, the views as data) and `src/SessionWebPage.cpp` (the one page, its
CSS and script, no third-party code). The user's side is REFERENCE.md's
"Remote sessions (early)".

- **Access.** A socket on 127.0.0.1 (never `INADDR_ANY`). Every address is
  under the view's own path, `/v/ID/` (`Access::base`, 16 hex digits made
  at start): the page at `/v/ID/`, the socket at `/v/ID/ws`, files at
  `/v/ID/file/PATH`. A link (`http://127.0.0.1:PORT/v/ID/?t=TOKEN`, a token
  of 48 hex digits from /dev/urandom) is good for one exchange, and the
  page, the socket, files and reloads carry a credential in a cookie
  instead (below, "Links, credentials and the Mac's question").
  `Web::answer` refuses, in this order: anything but GET (405), a Host that
  is not 127.0.0.1, localhost or [::1] with any port (403; two Host or two
  Origin headers make the request bad), an Origin that is not the page's
  own (`sameOrigin`: http:// and the Host the request came under, so a page
  on another port or under the other loopback name is refused too) and a
  socket upgrade with no Origin at all (403; since 8b, when pages can
  edit), a path outside `/v/ID/` (404), then for the page a link's token
  (exchanged) or the credential, for files the credential (allowed), for
  the socket any credential cookie (an unknown one is taken and told
  `expired`), else 403. The page has `Referrer-Policy: no-referrer` and
  fetches nothing else. The page's Content-Security-Policy allows one
  script by a nonce made per response (`{{NONCE}}` in the page; `{{BASE}}`
  is the view's path), inline styles, data: pictures, and WebSockets to
  127.0.0.1 and localhost.
- **A renderer that only reads.** The server attaches to the host's Unix
  socket as a `Client` with `setReadOnly(true)` (Hello flag 1) while at
  least one browser is connected, and detaches when the last goes, so a
  host nobody looks at still ends when its lifetime says. The host refuses
  every message from such a renderer that would change the session
  (`Host::refusedFromViewer`: edits, resets, undo, saves, tabs, every
  terminal message but attach and ack), answering an Error; the edit with
  no operations that reports how far it has read is let through. Its
  `DocOpen` (`Client::view`) is a view seat: never a new document (a
  missing one answers `DocGone`), never keeping one open (a view closing
  leaves the document, even one a detached renderer left; the last
  renderer that edits closing it ends it, and the views hear `DocGone`).
- **Views as data.** `Web::DocView` keeps the host's text from its edits
  with an `IncrementalHighlighter<char16_t>`; each operation becomes a
  splice of the line list (the lines the highlighter re-lexed, and how many
  old lines they replace), each line `[text, [[start, length, style]...]]`
  in UTF-16 units, which is what JavaScript strings count. A Markdown file
  (md, markdown, mdown, mkd, mkdn) is `MarkdownHtml::page`, sent whole at
  most four times a second, and drawn in a sandboxed frame with no scripts;
  a file over 8 M units or 200,000 lines is only named. `Web::TermView`
  feeds a `TerminalScreen` of its own from the host's replay and output
  (acknowledged at once, so the view never holds a shell back) and sends
  its rows as styled runs: the whole terminal (`term`), then `termd` with
  the lines that went into the scrollback and the rows `takeDirty` marked
  (all rows once anything scrolled), at most every 30 ms. A replay starts
  the screen over. **Size** (2026-11-18): `TermShell`'s `cols` and `rows`
  reflow the view's screen where they come in the output
  (`TermView::follow`, as the host's screen did there), and the pages get
  the terminal whole (`term`) at the new size; the state is not looked
  at for it. A view made at a size the state guessed has its replay made
  again at the size the `TermShell` after it says. The state's `cols` and
  `rows` only size a view's first screen.
- **Colors** (stage 8b's first part): `Web::paletteJson(Settings)` gives
  the page every color the editor resolves (the theme, then the file's
  own): the syntax styles, each surface's background and text by its
  settings name, the gutter, line numbers, ruler columns and color,
  bracket colors, the Markdown preview's five colors and code background
  and `markdown.max-width`, `editor.word-wrap`, and `editor.font-size`,
  `terminal.font-size` and `tabs.font-size` (points as CSS pixels). The
  server reads the settings file (`Options::settingsPath`, else
  `$MUTIBLE_SETTINGS`, else `~/.config/mutible/settings.conf`) at start
  and looks at its stamp (mtime, size, inode) every second;
  `Server::reloadSettings()` (any thread) looks at once, which the Mac
  calls on `MCSettingsDidChangeNotification` with AppSettings' own path.
  A palette that changed goes to every page as `palette`, and the page
  sets its CSS variables, one rule per syntax style in its `#theme`
  style element, and the Markdown frame's colors (a `<style>` put before
  the page's `</head>`, so the frame keeps no script) again, in place.
- **The page** says only `show {doc, term}`, when what it looks at
  changes; anything else is dropped. It follows the session's tab on show
  until a tab is clicked, keeps 5,000 scrollback lines, sticks to the
  bottom unless scrolled up, and reconnects every 2 s after a drop.
- **Connections** (2026-11-19, round 10's defect 10): at most
  `Options::maxConnections` (64) at once, one past it closed as it is
  accepted (accepted, so the listening socket does not stay ready); a
  connection that has not sent a whole request, or not taken its answer,
  within `Options::requestDeadlineMs` (5 s) of its accept is closed. A
  page's socket once upgraded (waiting for the app's answer too) and a
  file being sent are not held to the deadline. `Server::connections()`
  counts them (`sessionweb:connections`).
- **Threads.** One thread of the server's: poll(2) over the listening
  socket, the browsers and a pipe the client's `setWake` writes; the
  client's messages are dispatched there, so the views are touched by
  that thread alone. `host --web` runs it beside `HostServer::run` and
  stops it when the host ends; `attach --web` ends when the host's socket
  goes (looked at every second with no browser) or its link closes.

### Checks

`make test`'s `sessionweb:*` groups (`tests/session_web_tests.inc`):
`request` and `refusals` (the rules above as pure functions, the CSP and
nonce, the accept key); `viewer-reads-only` (a `Host` refusing a viewer's
every changing message, view seats, `DocGone`); `doc-lines` (random edits
spliced as the page splices them equal a fresh highlight, line for line,
spans included; Markdown sanitized; large files named); `term-rows` (rows,
scrollback and a clear against the screen); `server` (a host and the view
in the test process, a scripted WebSocket client: refusals over the wire,
hello and state, the document and terminal whole and then live against the
host's text and screen, a second browser, Markdown, a closed and a missing
document, and the view detaching when the browsers go). `make guitest
SUITES=session_web` loads the page in a WKWebView that is in no window
against a host in the guitest process: lines and colored spans, typing and
terminal output followed, a wrong token refused.

### Links, credentials and the Mac's question (2026-11-18)

The owner's three (approved 2026-11-18): the token leaves the address bar
and the history, a page left idle expires, and the Mac asks before a new
browser connects. The logic is the core's `src/SessionWebAccess.{h,cpp}`
(`Web::Access`, every method safe from any thread, the clock passed in);
`Web::answer` and the server use it.

- **Links are single use.** `Access::newLink` makes a token good until it
  is exchanged once (we picked single use over "until exchanged", which is
  the same rule: a token that was never exchanged works, one that was is
  refused with "this link was used already", the last 64 spent ones
  remembered for that message). At most 8 unspent links live; the oldest
  goes. They do not expire: the headless host prints its first one at
  start and may be opened hours later. `Server::url()`/`token()` give the
  newest unspent link, made when none is; `Server::newLink()` a fresh one
  (the Mac's Show and Copy each call it). `Options::nextLink` (the CLI)
  hears a fresh address after each exchange, and `mutible host --web`
  prints it ("that address has been used; the next browser opens:").
- **The exchange** is the page's load: `GET /v/ID/?t=TOKEN` spends the
  token and answers the page with `Set-Cookie: mutible-view-PORT=CRED;
  Path=/v/ID/; HttpOnly; SameSite=Strict` (no Max-Age: gone when the
  browser quits). Cookies are not kept apart by port, so the name carries
  the port and the path is the view's own: a dev server on another
  127.0.0.1 port that the page frames in its browser pane never gets it.
  The page's script runs `history.replaceState(null, "", "/v/ID/")`
  before anything else, so the address bar and the history entry lose
  the token; a reload is `GET /v/ID/` with the cookie. A request with a
  live credential and a spent or wrong token gets the page through the
  credential (the same browser opening its link again). A live token is
  exchanged even when a credential came too, so a new link replaces a
  refused credential.
- **The browser's id** (`mutible-browser`, 32 hex digits, `Path=/v/;
  HttpOnly; SameSite=Lax; Max-Age` 400 days) is set at a browser's first
  exchange, shared by every view on the machine so a remembered browser
  is known again on another port (the Mac's port changes with each
  server). Lax, so a link followed from another site (a chat page) still
  sends it; it is only a name, worth nothing without a link's token. A
  value that is not 32 lowercase hex digits is replaced.
- **Idle expiry.** A credential ends after `Access::idle` with nothing
  from its browser: an HTTP request carrying it (`touch` in `answer`) or
  any message on its socket (`handleMessage` touches before anything
  else; what the server sends does not count). The page sends `alive` at
  most once a minute while it is visible and someone used it (keys,
  pointer, wheel, touch), so a person reading and scrolling keeps it, and
  a forgotten tab does not. The idle time is `Options::idleMinutes`
  (fractions allowed; < 0, the default, takes
  `editor.session-web-idle-minutes` from the settings file the palette
  comes from, read at each change of the file; 60 by default, 0 for
  ever). `lookAtAccess`, every turn of the server's loop (the poll's
  timeout at most a quarter of the idle time), sweeps (`expire`) at most
  every second (a quarter of the idle time when shorter) and tells each
  page of a credential gone `expired {text}` (kExpiredText, "This view has
  expired; open a new link from mutible.") and closes it. A reload with
  that cookie is refused with the sentence alone as its body.
- **The Mac's question.** With `Options::ask` set (the Mac), an exchange
  from a browser `Options::remembered` does not know makes a Pending
  credential and a question (`Access::Question`: id, the browser's id,
  `browserSummary` of its User-Agent, the peer address and port). The
  page loads; its socket gets `waiting {text}` instead of hello (it is not
  counted in `browsers()`, attaches nothing, and every message from it is
  dropped). `lookAtAccess` hands the oldest unanswered question to
  `Options::ask` (once: `asked_`), and the next only once that one is
  answered (`Server::answer`, any thread) or its credential expired.
  Allow welcomes the waiting socket (hello and state); Deny sends `denied
  {text}` and closes, and a reload of a denied credential is the page,
  told `denied` at once, never asked again. **Rate limit**: at most
  `kMaxWaiting` (3) questions unanswered and `kMaxQuestions` (10) put in
  `kQuestionWindow` (10 minutes); past either the exchange answers 429
  and the link is kept for later. Since every question costs a fresh link
  that only the person at the Mac makes, no page can make the Mac ask in a
  loop. Without `Options::ask` (`mutible host --web`, GTK until it
  follows through the Linux session) the link alone admits: Allowed at
  once.
- **The Mac's side** (`MCSessionBrowser`): `o.ask` looks up on the
  server's thread where the connection comes from (`MCSessionBrowserPeer`:
  libproc over the user's processes for the TCP socket from the peer's
  port to the server's; when that is `sshd`'s, the far end of its
  established connection, "ADDRESS, through an SSH tunnel"; else "this
  Mac"), then on the main queue begins a sheet on the window: "Allow
  BROWSER to open this window's session?", Allow (Return), Deny (Escape),
  Always Allow. The answer goes to the server that asked (a generation
  per server started). Always Allow stores `{id, browser, address, since}`
  under `MCSessionBrowsersRemembered` in the app's defaults, which
  `o.remembered` reads (any thread). View > Remembered Browsers…
  (`mutible.rememberedSessionBrowsers`, AppDelegate's
  `showRememberedBrowsers:`, `MCSessionBrowsersShowRemembered`) lists them
  in an alert with Done and Forget All. `MCSessionBrowserAskDelay` holds a
  sheet back for checks. A window not active also bounces the Dock icon
  once (`requestUserAttention:` informational).
- **The page**: `#gate`, a plain sentence over the whole page, for
  `waiting`, `denied` and `expired` (the last two end it: no reconnecting,
  the viewer and the browser frame emptied). File addresses are
  `/v/ID/file/PATH` with nothing secret in them (`fileTarget(base, rel)`).

#### Checks

`sessionweb:access`: cookies read and set, `browserSummary` over ten
agents; a link exchanged once then Spent, a wrong and an empty one, the
live link made afresh, the eight-link cap, browser ids kept or replaced
(four malformed); idle expiry to the millisecond, a touch that comes too
late, `expire`, 0 for ever; the question for a browser not remembered,
one waiting its turn, a remembered one never asked, Allow and Deny, three
waiting then Busy with the link kept, ten in ten minutes then Busy, one
more after the window, a question whose page went quiet no longer
answerable. `sessionweb:access-server` over the wire: a headless view
(no one asked, a 1.8 s idle time): the exchange's two cookies, the next
link said once, the spent link refused without the cookie and served
with it, a reload with nothing set and the page's own path in it, a file
through the cookie, `alive` keeping the page past its idle time, then
`expired` and the socket closed, a reload's sentence, files refused, the
socket told `expired`, a fresh link opening it again; then the Mac's
kind: the page `waiting` with nothing of the session and no file, a
second browser waiting its turn unasked, Allow (hello, and the list sent
while waiting dropped), the next question put, Deny (`denied`, closed,
its reload told at once and not asked again), a remembered browser
straight in. `sessionweb:refusals` and `sessionweb:files` check the rules
above as pure functions (the Set-Cookie lines, the old `/?t=` and
`/file/TOKEN/` forms gone, no file without the cookie). GUI:
`session_web` (the address after the page loads is `/v/ID/` with no
token, nothing behind it in the history; a reload through the cookie; the
same link in a second WKWebView refused as used; a 0.05-minute idle
written to the settings file, then the page's "This view has expired"
and its socket closed; the reload's sentence) and `session_browser` (the
link Show opened and the one Copy made differ, both under `/v/ID/`; the
plain request's Set-Cookie, the link refused the second time; the
window's sheet for that browser, its words and window; a scripted
browser Always Allowed and remembered; the WebKit page "Waiting for
approval in mutible" while the sheet is held, its address without the
token, then Allow; a picture's address with no secret; both windows'
scripted browsers, remembered, let in with no sheet; an unknown browser
Denied, seeing nothing; Remembered Browsers… listing it and Forget All).
The sheet's wording and the expired page need the owner's eyes.

### The Mac's own host in a browser (View > Show Session in Browser…)

`mac/src/SessionBrowser.{h,mm}` and `EditorController+SessionBrowser.mm`
serve the window's tabs from the process's in-process host, with no
`mutible host`. Two View items, no keys (`mutible.showSessionInBrowser`,
`mutible.copySessionBrowserLink`): Show opens the address through
`NSWorkspace openURL:` (`MCSessionBrowserOpener` in checks), Copy puts it
on the general pasteboard. With `editor.session-host` off both run an
NSAlert offering Turn On (`AppSettings writeValue:`), and do nothing on
Cancel.

- **One server per window**, an `MCSessionBrowser` kept as an associated
  object on the controller, made at the first choice and reused; it stops
  on the window's `NSWindowWillCloseNotification` (and in dealloc), so
  EditorController gained no hook.
- **Read-only or editable.** Show asks first (Editable, Read-Only,
  Cancel, the last answer first; `MCSessionBrowserEditable` in the app's
  defaults, editable until one is given); Copy uses the remembered
  answer. `MCSessionBrowser editable` set otherwise than the running
  server stops it, and the next start serves anew at a new address.
- **The in-process host.** `Web::Options::connect` replaces the Unix
  socket: `MCSessionConnectViewer` (SessionLink.mm) connects the view's
  client (a reader's or a guest's) as another renderer of `gHost`, and the host's Sink
  routes by `ClientId` (`gViewers`, under a lock, so the function it
  returns stops deliveries before the client goes). The state's folder is
  not checked then (one host serves every window), and `run()` does not
  look for a socket file.
- **One window's tabs.** The window's record (`MCSessionWindow`, since
  stage 6 sent while the setting is on, a server running or not; "The Mac
  reattaches") is what the page shows: the window sends `Client::windowTabs`
  (`windowJson`'s shape, by hand: groups of tabs with a document on the
  host, each group's tab on show) every half second when it changed, and
  closing, leaves it marked detached. `Web::Options::window` keeps the state the
  browsers get to that window and its documents, and a `show` of any other
  document answers `gone`. The window's id is the client's (`newId()`,
  below).
- **One window's terminals** (R5, 2026-11-18). Terminals opened while
  the setting is on are the host's (above, "Terminals on the host"), and
  the host does not know which window holds one. The window says, as it
  says its tabs: `reportTabs` adds `"terms"` (each panel terminal's
  `sessionTerminal`, from `controlTerminals`) to its `WindowTabs` json,
  and `lookNow()`s the server after a change, so the page follows an
  opened or closed terminal within the half-second report. With
  `Options::window` the server (not the page's script) keeps
  `allowedTerms` from that array at each look: the state's `terms` lists
  only those, `show` of another id shows nothing and attaches to nothing,
  `tkey`, `ttext` and `tpaste` for one are dropped, and a terminal the
  window stops reporting is taken off every page and its `TermView`
  dropped. A page's `tsize` for another window's terminal is dropped too,
  as is a read-only page's. A window that reports no `terms` has none on
  its page; a headless host's view (`window` 0) sees every terminal, as
  before. `Options::termNote` (in the hello) puts "Terminals opened while
  the session host is on appear here" in a muted line while the state
  has none.
- A tab opened before the setting was on has no document on the host and
  is left out of the page until it is opened again.

Checks: `sessionweb:in-process` (an in-process `SessionHost` with a routing
Sink, two windows' tabs, the view filtered to one, typing followed, the
host let go when the browser leaves); `sessionweb:window-terminals` (two
windows' terminals on one host: window 5's editable view lists only its
own, a show, keys, text, paste and resize for window 6's id reach nothing
and attach nothing, its own is shown and typed into, a view with no
window lists both, an opened terminal appears once reported and a closed
one leaves, one no longer reported is let go); `make guitest
SUITES=session_browser` (the alert's Cancel and Turn On, the address
caught rather than opened, Copy, plain requests with and without the
token and with another Host, a WebSocket that gets the window's document
and an edit typed in the window, two windows each with a terminal and a
page listing only its own, the other's id refused and its own typed into,
a closed terminal leaving its page, the port closed once the window is).

### Editing in the browser (stage 8b)

`Web::Options::editable` (the Mac's choice at Show; `mutible host --web
PORT --edit` or `attach ... --edit`): the page edits the document on show,
undoes through the host's history, asks for a save, and types into the
terminal on show. Read-only stays the default for the command line.

- **A guest, not an editor.** The server's client says Hello flag 2
  (`Client::setGuest`): its document seats are views, as a reader's
  (never a new document, never keeping one open, `DocGone` when the last
  editor closes it), and `Host::refusedFromViewer` lets it send only
  `DocEdit` in the history (no flag 1), `DocUndo` and `TermInput` beyond
  what a reader may (`sessionweb:guest`). A read-only view is still Hello
  flag 1 and its server refuses every page message but `show`
  (`refused` or `note`), before the host would.
- **The server sequences its pages**, as the host sequences renderers.
  `DocView` counts its own versions, one per change of its text whoever
  made it (`commit`), keeps a log of 20,000 (`since`), and queues each
  version as a `Change` that `flush` sends in order: the splices (or `all`
  lines when a splice will not do), `ops` to every page but the maker, and
  to the maker its `seq`, its `fix` and a `sel`. A page's `edit {base,
  seq, ops}` is moved past `since(base)` (`Ot::transform`, the server's
  winning ties: everything since is someone else's, as the page has one
  edit in flight), applied to the view, recorded on the server's client
  as one step (`openStep` with the page's name and coalescing,
  `breakCoalescing` when the page says `fresh`, selections mapped through
  what crossed), and committed. The client then does what any renderer's
  does: one edit in flight to the host, the host's edits moved past its
  own, handed back through `hostEdited`, which commits them as version
  `by` 0. A base older than the log, or ops that do not fit, are
  `refused` and the page gets the document again.
- **Commands.** Return, Tab (a caret, or a selection in one line),
  Shift+Tab, a selection over several lines, and Backspace carry `cmd` and
  `at` (the selection they acted on). The server maps `at` through what
  crossed and makes what `AutoIndent::newline`, `tab`, `backtab` or
  `backspace` say on its own text instead of the page's guess (Return:
  the line's indentation; Shift+Tab and block Tab: nothing), and sends the
  maker `fix`, the smallest replacement from its guess to that
  (`fixBetween`), plus AutoIndent's selection. The page takes `fix` as a
  change made just after its own edit: what is waiting moves past it with
  the caret after inserted text (`afterInsert`), so typing that followed
  Return lands after the indentation.
- **Undo** (`undo {redo}`) goes once the page has nothing unconfirmed;
  the server calls `requestUndo`, and the host's edit comes back as any
  other, with the step's selection for the page that asked
  (`Session::undoFor`). An outcome other than done is a `note`.
- **Save** (`save`) goes once the page is in step; the server settles its
  client (the host has every edit, and has passed it on) and calls
  `Options::save` on its own thread. The Mac's dispatches to the main
  queue, dispatches the app's client (so the tab has every edit), and
  saves the tab as `saveCurrentFile:` with a sender does for the tab on
  show (format and organize on save, the disk questions) or `saveTab:`
  for one behind. Without `Options::save` (a headless host) the host
  writes the file itself (below, "Saving through the host"). The tabs'
  dirty dots are the host's dirty flags in the state, polled every half
  second, and looked at again at once when one flips (DocDirty).
- **Terminal keys** (`tkey {key, mods}`, `ttext {text, mods}`, `tpaste
  {text}`) are encoded on the server by the view's own screen, which saw
  the same modes as the host's (`TerminalScreen::encodeKey` with
  `appCursorKeys`, `appKeypad` and `keyboardFlags`; `encodeChar`;
  `encodePaste` with `bracketedPaste`), and go to the host as
  `TermInput`. Mods are xterm's (Shift 1, Alt 2, Control 4); on a Mac the
  page leaves Option out, as it composes characters there.
- **Terminal size** (`tsize {term, cols, rows}`, 2026-11-18): an
  editable page measures the cells its terminal pane holds (a hundred
  zeros in the terminal's font, in a box of no size so the pane gains no
  scrolling; less the pane's padding) and says them on each whole
  `term`, a window resize and a `ResizeObserver` change, when they
  differ from what it last said. The server is one renderer for every
  page, so it keeps each page's size (`Conn::termCols`) and per terminal
  the page that typed last (`Session::typist`), and sends the host
  `TermResize` (`sizeTerminal`, only when it differs from what it last
  sent) with the typing page's size before its keys, or when that page
  resizes while it is still the last to have typed. A page that only
  looks is never the size. The host lets a guest's `TermResize` through
  `refusedFromViewer` (a reader's is still refused): the pty takes it
  only while the guest is the one that typed last (Host::sizer). The
  page draws the pty's size from `term` (its rows at `cols` ch, the pane
  scrolling sideways when wider).
- **The page's editor** (SessionWebPage.cpp, no third-party code): each
  line `{t, s, b}` (text, spans, its break: the server sends break 0 for
  the last line, 2 for "\r\n", 3 "\r", 4 U+2029, nothing for "\n").
  `shadow` is the server's lines at `version`, `flight` the edit sent,
  `queue` those waiting (typing that waited is joined into one while it is
  one step), and the lines on show are the shadow with them made
  (`applyOp`, which keeps the objects, so the elements, of lines it did
  not change, and the colors outside a change until the server's come).
  `render` keeps the elements whose line object is the same at both ends.
  Its transform is src/Ot.cpp line for line in UTF-16, the halving
  recursion included. The caret and selection are drawn over the lines
  from Range rectangles (only lines in view); a hidden textarea at the
  caret takes keys, input methods (only `compositionend`'s text is
  typed), and the clipboard (it holds the selected text, so Copy is
  enabled in every browser); a click is `caretRangeFromPoint` (or
  `caretPositionFromPoint`). Keys: the arrows (Option on a Mac, Control
  elsewhere, by words; Command on a Mac to the line's or file's ends),
  Home (to the first non-blank, then column 0) and End, Page Up and Down
  by the scroller's height, Backspace and Delete (a surrogate pair or a
  line break whole), Return, Tab, Shift+Tab, Escape, Command or Control
  A, Z, Shift Z (Control Y), S; Copy, Cut and Paste are the browser's
  events. A Markdown document shows its page, with Edit Source to edit it
  (an editable page opens on the source). A document with typing not yet
  confirmed is kept on show until it is.

#### Checks

`sessionweb:guest` (the host's rules for a guest, its `TermResize`
let through); `sessionweb:term-size` (a phone's page and a desktop's on
one view of a host terminal, the state looked at once: neither page's
size taken until it types, then the pty at the typing page's size before
its keys, `stty size` saying so, and both pages sent the terminal whole
at it; the other page's resize kept, the typist's taken; the editor's
renderer typing takes the opener's size back; its answer, flag 1, takes
nothing); `sessionweb:edit` (a
scripted page that keeps lines and pending edits as the script does,
against a host over its socket with the editor's renderer typing at the
same time: 120 rounds of both, ending on one text in the page, the host
and the renderer; Return after `def g():` making four spaces with the
next typing after them; undo and redo; a save reaching `Options::save`;
keys and Control C reaching the shell; a read-only view of the same host
refusing an edit, undo, save and keys; the socket refused for another
origin, a localhost page under a 127.0.0.1 Host, and no Origin);
`sessionweb:refusals` (`sameOrigin`); `sessionweb:doc-lines` (breaks and
versions). GUI: `session_web` types into the headless host's shell and
file through key events dispatched in the page; `session_browser` loads
the Mac's page in a windowless WKWebView, types into the window's file
with key events (Command Down, a line, Return), types in the window and
sees the page follow and its dirty dot, checks Return's indentation,
Command Z and Command S, and ends with page, window, host and disk on one
text. The page's line splitting and transform were also fuzzed under node
by hand against plain string edits and TP1 (not committed: CI has no
JavaScript runner).

### The tree in the browser (stage 7's first read)

The page has a file tree in a left sidebar (the owner's ask, 2026-11-18,
R6): `src/SessionFiles.{h,cpp}` reads the folder for the host, and the
protocol gained two kinds, `FsRead {req, window, json}` and `FsData {req,
json}` ("fs.read", "fs.data"), shaped as stage 7's general read: the
request's `op` names the read, so stage 7 adds reads (a file's bytes for
viewers, Quick Open's list) as new ops, not new messages.

- **Where a read is confined.** `Host::readRoot`: a headless host's own
  folder (`setFolder`), else the `root` of the window the request names,
  as that window's renderer reported it (`WindowTabs`; the Mac's
  `MCSessionBrowser reportTabs` sends the window's folder). With neither,
  the answer is `{"error": "no folder to read in"}`. `confinedPath`
  refuses an absolute path, an empty, "." or ".." name, a NUL, and a path
  whose `realpath` is outside the folder's `realpath`; what it gives back
  is spelled under the root (as the tabs spell paths), not the real path.
- **The listing** is the apps' trees' rule, not a new one: dotfiles only
  with `hidden` (the Mac passes `gShowHidden` at the server's start, as
  `Options::showHidden`), folders first in `TreeSort::order`, and what
  `FolderSearch::Ignore::Rules` (atTop, then `enter` down the path, a
  folder they ignore making all below it ignored) ignores is marked
  `ignored` and drawn in `tree.git-ignored`'s color (`paletteJson`'s
  `treeIgnored`), as the apps color it rather than leave it out. A symlink
  is marked `link`; one whose target is outside is `outside`, listed
  dimmed, never listed into or opened. Files carry `FileIcon::kindOf`'s
  name. At most `kMaxListed` (10,000) entries, the rest counted as `more`.
- **Viewers may read.** `refusedFromViewer` lets `FsRead` through (it
  changes nothing). `Client::requestRead` sends at once (never behind an
  edit in flight); the answer is `readAnswer(req)` after a dispatch, or
  `read()` waits.
- **The web server**: the page's `list {path}` becomes an `FsRead` for
  `Options::window`, answered to that page as `tree {path, root, entries,
  more | error}` (`takeReads`, beside `takeOutcomes`, after each
  dispatch; at most 256 reads outstanding). `open {path, keep}` is held
  inside `Options::folder` by `confinedPath` and must name a regular file;
  then `Options::open(abs, keep)` (the Mac: `openSessionPath:keep:` on the
  main queue, `revealPath:open:YES from:TreeClick|TreeCommandClick
  widen:NO`, so the window gets the tab its own tree's click would give,
  the row selected in its tree), answered `opening {path, abs, ok, why}`.
  A headless host has no `open`: the answer says it cannot open files yet.
  **Read-only pages list but do not open** (R7, reversing R6's decision):
  opening a tab changes the Mac's window, which a reader must leave as it
  is, so `open` is after the editable gate in `handleMessage` and a reader
  reads the file for a tab of its own instead (`readfile`, below, "The
  page's own tabs").
- **The page** keeps the open folders (`T.open`), each folder's last
  listing and errors, lists a folder each time it is opened (so it is
  current when opened), lists every open folder again on ↻, on `hello`
  (every reconnect) and when the page becomes visible again, and says
  which folders it has open (`watch`) whenever that changes: on a host
  that watches the disk their listings come again by themselves when
  they change ("The host watches the disk", below). On a page with
  `canOpen` (editable, with `Options::open`) a click on a file
  sends `open` (the second click of a double-click, or Command/Control,
  keeps the tab) and, once `opening` says ok, waits for a document with
  that `abs` in the state, then shows it (`takePending`); a file the page
  shows itself (R3, below) shows at once instead, and another viewer kind
  (database, archive, bookmark, a video format the browser lacks) says
  "NAME opens in the mutible window." in `#treenote`, and a text file with no
  document after 4 s says it opened in the window. The document on show's
  row is marked `current`. At 700 px and under the sidebar is hidden and
  the header's ☰ shows it over the page (a tap beside it closes it);
  wider, ☰ hides and shows it.

#### Checks

`host:fs-list` (the listing's order, dotfiles, ignore marks, links in and
out, kinds, one level at a time, an ignored folder's contents, twelve
refused paths, `confinedPath` directly, an unknown op, a window's folder
in an app's own host and none without one, a read-only renderer reading,
the messages through encode and decode, the cap); `sessionweb:tree` (over
the wire: no listing over plain HTTP, no socket without the token or from
another origin, the folder listed with marks, a subfolder, four escapes
refused, a read-only page's opens reaching `Options::open` with `keep`,
a folder and two escapes not opened, a headless server saying it cannot
open). GUI: `session_browser` reads the page's tree in WKWebView, clicks
a folder open and a file in it, sees the window open that file in its tab
and the page show it, and a picture's line. Checked by hand in headless
Chromium over CDP at 1100 and 390 px wide (not committed).

### Files for the page's viewers (R3, first slice)

The owner's ask (2026-11-17, trying 1.5.34): a PNG, PDF, video and the
like clicked in the page showed nothing. Now the host serves the bytes and
the browser shows them with its own viewers.

- **The host's read.** `FsRead`'s second op, `{"op": "read", "path",
  "offset", "length", "hidden"}`, answered `{"op", "path", "size",
  "offset", "length"}` with the bytes in `FsData.bytes` (base64 on the
  wire, as terminal bytes are; `Client::readAnswer(req, &json, &bytes)`,
  `forgetRead` for an answer nobody will take). Confined as the listing
  is (`confinedPath`), plus: no name starting with '.' anywhere in the
  path unless `hidden` (the tree's rule), only a regular file (`fstat`
  after opening with `O_NONBLOCK`, so a FIFO neither blocks the host's
  thread nor is read), opened at its `realpath` with `O_NOFOLLOW` (what
  was checked is what is read), at most `kMaxReadBytes` (4 MB) a read, on
  the host's thread. `length` 0 gives the size alone.
- **HTTP, not the socket.** `<img>`, `<video>` and the PDF viewer need an
  address, and video needs Range. `/v/ID/file/PATH` (it was
  `/file/TOKEN/PATH` until the credential's cookie, 2026-11-18; above,
  "Links, credentials and the Mac's question"): the browser sends the
  cookie with each, so an HTML page's relative pictures and style sheets
  are served too (`answer()` gives
  `Answer::File` with the path decoded by `pathDecode`: '+' kept, a NUL
  refused; the Host and Origin checks come first, as for the page). The
  server needs a page's socket open (the session is attached only then):
  without one, 503. `beginFile` reads the size (a read of 0 bytes), then
  `fileHead` answers 200, 206 (`byteRange`: one range, a suffix range,
  clipped; several ranges, another unit or a malformed header get the
  whole file, as HTTP allows) or 416, and `pumpFile` reads 2 MB pieces
  (`kFilePiece`) only while the connection holds under one piece unsent,
  so a 2 GB video costs 4 MB at most; at most 32 files at once. The
  connection ends after the file (Connection: close); a file that shrank
  mid-send cuts the connection, which the browser sees as too few bytes.
  `detach` (the last page gone) ends every file being sent; a dead
  connection's read in flight is `forgetRead`.
- **Only kinds `fileType` names** (by extension): image (png, jpg, jpeg,
  gif, webp, bmp, ico, avif, svg), pdf, audio (mp3, m4a, aac, wav, ogg,
  oga, opus, flac), video (mp4, m4v, mov, webm, ogv), html (html, htm,
  xhtml), and css with no view (an HTML page's own style sheets; fonts
  are left out: a font load is CORS and the sandboxed frame's origin is
  null). Anything else is 404: text opens as a tab, and nothing else is
  served at all. Caps on the whole file: 64 MB for pictures, HTML and
  CSS, 512 MB for PDF (413 past them), none for audio and video.
- **Script runs nowhere.** Every file response has its Content-Type and
  `X-Content-Type-Options: nosniff` (a .png holding HTML is never HTML),
  no-store, no referrer, and, except a PDF, `Content-Security-Policy:
  sandbox; script-src 'none'; object-src 'none'; base-uri 'none';
  form-action 'none'`: an SVG or HTML file opened even by its address in
  a tab of its own has an opaque origin and runs no script. The page
  frames HTML with `sandbox=""` too (no allow-scripts, no same-origin),
  matching the Mac's html.scripts default of ask by never running them;
  a later slice could offer Run Scripts. A PDF has no sandbox because
  Chrome's viewer refuses to run in a sandboxed frame or document; it
  runs in the viewer's own origin.
- **The page's CSP** gained `img-src data: 'self'` and `media-src 'self'`;
  `frame-src 'self'` was there. The rest is as before.
- **Tables over the socket.** `file {path}` (read-only pages too: it
  changes nothing) reads the first `kMaxTableBytes` (one read, 4 MB) and
  answers `table {path, columns, rows, truncated, size | error}` from
  `tableJson` (the core's `Csv`, tab for .tsv else `detectDelimiter`; a
  cut file's last row dropped; at most 200,000 cells, a cell cut at 2,000
  bytes on a character). At most 16 tables outstanding.
- **The tree's entries** carry `view` (`fileType`'s) when the page shows
  that file itself; `takeReads` adds it, so the rule is the core's.
- **The page.** A file with a `view` opens in a tab of the page's own
  (R7's `L.tabs`, the entry `{path, abs, view, viewer: true}`), on any
  page; an editable page also sends `open`, so the window opens it as
  before, and ignores its `opening` answer (`T.pending.shown`) so the page
  stays on its own tab even when the window's tab is a document (a CSV).
  `showViewer` hides `#code`, `#md` and `#note` and fills `#fileview` in
  `#docsec`: an `<img>` (on a checkerboard), `<audio>`/`<video controls
  preload=metadata>`, an `<iframe>` for a PDF, a sandboxed one for HTML,
  or the table (row numbers, the first row bold and sticky, as the Mac's
  CsvTable shows it; the `table` answer kept on the tab, so showing it
  again asks nothing). `#fileopen` in the doc bar is "Open in a new tab"
  (not for a table). A window tab, a text tab of the page's own, closing
  the viewer's tab, or a note hides it (`hideViewer` empties `#fileview`,
  so media stops). The tree marks the viewer's row through `L.on`.
  `mcSessionView.file()` says what shows, for the GUI check. A kind the
  browser cannot show (database, archive, bookmark) says it opens in the
  window, as before.
- **Not done in this slice**: SQLite and notebooks (the second slice,
  below), the LaTeX preview (the third), and git and review pages (the fourth); the window's viewer tabs (a picture's tab) are
  still not reported to the page (the Mac reports only tabs with a
  document on the host), so the page shows a picture only from its tree,
  and does not follow the window to one.

#### Checks

`sessionweb:files`: `fileType` over 30 names, `fileTarget`'s encoding,
`byteRange` over 15 headers, the heads (200, 206 with Content-Range, 416,
nosniff, the sandbox CSP on six kinds and not on PDF), `answer()` for
/file/ (token in the path, missing, wrong, in the query instead, a NUL,
another Host or Origin, POST), `tableJson` (quotes, TSV, a cut start, the
cell and size caps); the host's read directly (a piece, the size alone,
the 4 MB cap, the end, past it, a link inside, a dotfile with hidden, and
13 refusals: escapes, a link out, a dotfile, a folder, a FIFO, a missing
file, an empty name); FsData's bytes through encode and decode; then over
the wire with a headless host: 503 before a page, a PNG's bytes and
head, four refusals of access, nine escapes and kinds refused with
nothing of the outside file sent, text not served, SVG and HTML
sandboxed, PDF not, a 5 MB video whole (three pieces) and in four
ranges with a 416, the tree's views, a CSV's rows over the socket and
two refusals, and 503 again once the page has gone; the page's CSP.
GUI: `session_browser` clicks a PNG in the editable page's tree (the
page shows it in a tab of its own at the view's /file/ address, decoded
at its real width, and the window opens it) and a CSV (the page's table
has the core's rows, and the window opens it); then a PNG in the
read-only page's tree (its own tab, the window opening nothing, its ×
going back to the window's tab). Checked by hand in headless Chrome over CDP
against `mutible host --web` (not committed): a PNG at its width, an SVG
whose script did not run, an HTML page with its relative style sheet and
picture and its script not run, Chrome's PDF viewer in the frame, the
table, a WAV's duration read and a page fetch of a range answered 206.
The look in Safari, video seeking, and a phone need the owner's eyes.

### Notebooks, databases and Markdown in the page (R3, second slice)

What the core can render without the Mac: a notebook, a SQLite database,
and (already there) a Markdown file's page.

- **Read here, not through the host.** `notebook {path}` and `db {path,
  table, offset}` (any page: neither changes anything) are answered by
  `Web::notebookJson` and `Web::databaseJson` (`src/SessionWebViewers.cpp`),
  which read from `Options::folder` as `readfile` does: SQLite needs a
  path, not bytes, and a notebook may be past the host read's 4 MB.
  Both go through `openLocalFile`, the checks `localFileJson` made, now
  shared: `confinedPath`, no dotfile name unless the tree shows them,
  `O_NONBLOCK` and `fstat` as a regular file (a FIFO refused, never
  blocked on). `fileType` names `notebook` (.ipynb) and `database`
  (.sqlite, .sqlite3, .db, .db3) with no mime, so `/file/` serves neither
  as bytes (404) and the tree's entries carry the `view`.
- **A notebook** (at most `kMaxNotebookBytes`, 32 MB) is
  `Notebook::parse`, `toMarkdown` and `MarkdownHtml::page`, the window's
  own preview's Markdown: every text escaped, pictures data: images (SVG
  base64'd, so it is an `<img>` and runs nothing), links only relative,
  #, http, https and mailto, made valid UTF-8 (`SqliteRead::validUtf8`)
  because a WebSocket text frame that is not ends the connection.
  `MarkdownHtml` now writes a fence's label (the rest of its info string)
  as `<div class="code-label">` above the block, so Jupyter's "In [3]" and
  "Out [3]" show as in the window (Export as HTML gains it too; the
  corpus golden has the one CSS line). The page puts it in an `<iframe
  sandbox="" srcdoc>` with the palette's Markdown CSS (`withPalette`, as
  the Markdown frame; redrawn on `palette`): no scripts, an opaque origin,
  and the parent's CSP inherited (data: pictures only, nothing fetched).
- **A database** (`src/SqliteRead.{h,cpp}`, the core's one use of the
  system libsqlite3, now linked by the tests and `mutible-cli`; Linux
  already linked it): the header's 16 magic bytes checked first (a text
  file named .db is not opened), then `sqlite3_open_v2` at the file's
  `realpath` with `SQLITE_OPEN_READONLY | SQLITE_OPEN_URI`
  (`SQLITE_OPEN_NOFOLLOW` where defined) and `file:...?mode=ro`; a WAL
  database with no `-shm` or no `-wal` beside it (its writers closed)
  opens with `&immutable=1` at once, since a read-only connection makes
  them where the folder lets it (SQLite 3.22 on; the Mac's viewer tries
  `mode=ro` first and falls back on failure), `PRAGMA query_only`, the defensive flag,
  `TRUSTED_SCHEMA` off, no extensions, `SQLITE_LIMIT_ATTACHED` 0,
  `SQLITE_LIMIT_LENGTH` 16 MB (one row cannot take the memory), a 250 ms
  busy timeout, and an authorizer allowing only READ, SELECT, FUNCTION and
  RECURSIVE. The only statements are the schema's list and `SELECT * FROM
  "name" LIMIT ? OFFSET ?` for a name the schema listed (quoted, compared
  exactly; nothing of the page's becomes SQL). A progress handler stops
  each call at 2 s (`kTimeLimit`): a recursive view, a huge OFFSET. The
  connection is opened per request and closed at once, so no lock is held
  between reads; a writer waits at most one statement. A page is
  `kDbPageRows` (100) rows or about 4 MB of text; a text cell cut at 2,000
  bytes comes as {text, bytes}, a blob as {blob: size}, NULL as null.
- **The page**: `drawNotebook` and `drawDb` in `#fileview`, a viewer tab
  of the page's own like a picture (no "Open in a new tab": there is no
  address). The database bar has a `<select>` of tables and views
  (option values and text set as properties), "Rows 1–100, more after",
  Previous, Next and a Read-only label; the rows a `<table>` with a
  sticky header of the column names. Every name and cell goes in by
  `textContent`, so markup in a table or column name is text.
- **Markdown** was already the page (`MarkdownHtml::page` in the sandboxed
  `#md` frame) with `#modebtn` flipping to the lines; its label is now
  Show Source on a page that cannot edit (Edit Source stays for one that
  can), Show Page going back.
- **Not here**: the LaTeX preview (the third slice, below) and git and
  review pages (the fourth); no query box (the Mac's viewer has one).

#### Checks

`sessionweb:viewers`: `fileType` for the new kinds, `SqliteRead`'s pieces
(the header, WAL, `quoteName`, `uriFor`, `validUtf8` over good, cut,
surrogate and overlong bytes), a Latin-1 CSV's cell made valid; a
notebook rendered (heading, emphasis, In and Out labels, the picture);
a hostile notebook (script in Markdown, code, stream, error and HTML
outputs, a picture's data leaving its attribute, an SVG with onload and
script, javascript: and an iframe) with no `<script`, `<iframe`,
`onerror="`, `onload` or `javascript:` left and the page valid UTF-8;
nine notebook refusals (links and paths out, a dotfile, a folder,
missing, another kind) with nothing of the outside file sent, and 32 MB
refused; a database with hostile table and column names (`<script>`,
quotes) as text, NULL, blobs, a cut cell, bytes not UTF-8, reals; paging
over 250 rows; four names not in the schema refused (an injection
among them); a recursive view stopped at a 200 ms deadline in bounded
CPU; the byte cap; the file's bytes, mtime and the folder's names
unchanged; a writer mid-transaction not blocked (it commits after the
read); a WAL database closed (opened immutable, no -wal or -shm made) and
live; nine database refusals; then over the wire with a headless host.
GUI: `session_browser`, on the read-only page, clicks a hostile notebook
(the sandboxed frame's html has no `<script` and `window.pwned` stays
undefined), a hostile database (its names and cells as text), picks
another table, presses Next, and checks the file's bytes unchanged; and
a Markdown file's Show Source and Show Page. Checked by hand in headless
Chrome over CDP against `mutible host --web` (not committed): both
notebooks drawn, the hostile one running nothing; the database's tables,
cells, paging, and a recursive view stopped after 2 s; Show Source; the
database bar at 390 px; the file's hash unchanged. The look in Safari and
on a phone needs the owner's eyes.

### The LaTeX preview in the page (R3, third slice)

The owner's ask (R3): the LaTeX preview did not show in the browser view.
Now a LaTeX document on the page shows the PDF the window's own preview
typeset, and the new one each time the window typesets again; the page
never starts a typeset of its own when the window's PDF is current.

- **Text first.** A `.tex`, `.ltx` or `.latex` file (`typesetSource`, the
  Mac's list) stays text: a document's `doc` and a page's own `filetext`
  carry `latex: true` (`DocView::snapshot`), the tree gives it no `view`,
  and `/file/` does not serve it. The page opens it on its pages
  (`D.mode` "page", on an editable page too, as the window opens it in
  its preview), and `#modebtn` flips to the lines (Show Source, or Edit
  Source on an editable page) and back, as for Markdown.
- **The app typesets, the server keeps.** `Web::Typesets`
  (`src/SessionWebTypeset.cpp`) keeps, by the document's real path, the
  state (working, ready, failed), the last PDF that worked (kept while
  another runs and through a failure), the failure's text (64 KB, valid
  UTF-8), a version for every change and a `pdfVersion` for every new
  PDF; ready with the same bytes changes nothing. At most 64 entries, the
  one changed longest ago going first; a PDF over 512 MB is a failure.
  `Server::typesetWorking`, `typesetReady` and `typesetFailed` (any
  thread) change it and wake the server, whose loop (`sendTypesets`)
  sends each page showing that document a new `latex` message.
- **A page asks.** `latex {path}` (any page: the app typesets from a copy
  and changes nothing of the user's) takes a path from the folder or a
  document's path under it (`relativeInFolder`: as spelled, else both
  resolved), refuses anything `openLocalFile` refuses (outside the
  folder, a link out, a dotfile unless the tree shows them, not a regular
  file) or that is not LaTeX, with `state: "refused"` and the app never
  asked; else it remembers the document on the connection, calls
  `Options::typeset(abs)` and answers with `typesetJson`: `state` (ready,
  working, failed, none, refused), `url` (`typesetTarget` plus `?v=` the
  `pdfVersion`, so a new PDF is a new address the frame loads), `text`
  (the log, or what to do). With no `Options::typeset` (a headless host)
  a document with nothing published is "none", and its text says the
  window's own page shows the pages.
- **`/v/ID/typeset/PATH`.** The credential's cookie as for `/file/`
  (`Answer::typeset`), then `beginTypeset`: LaTeX only, a page's socket
  open (503 before), `openLocalFile` again (so a typeset kept for a file
  outside the folder is never served through a link), a PDF kept (404
  before), then the PDF's head (no sandbox CSP: Chrome's viewer will not
  run in one; nosniff, no-store), 206 and 416 by `byteRange`, and the
  bytes from memory 2 MB at a time as the connection takes them
  (`FileSend::mem`, counted among the 32 files at once).
- **The pages as pictures (keeping the reader's place).** The owner's
  ask: a new typeset reloaded the frame and threw the reader back to
  page 1, and the browser's viewer gives the page no way to read or set
  its place (pdf.js is a third-party dependency). So, when the app gives
  a renderer (`Options::typesetPageSizes` and `typesetPageRender`, both
  or neither), `Server::typesetReady` reads each new PDF's page sizes in
  points (on the caller's thread, once a PDF: the same bytes keep the
  sizes they had; at most `kMaxTypesetPages`, every size positive) and
  `Typesets` keeps them with it (`Entry::pages`); `typesetJson` then adds
  `pages` ([[w, h], ...], to a hundredth of a point) and `pageUrl`
  (`typesetPageTarget` plus `?v=` the `pdfVersion`). No renderer (a
  headless host, the Linux port), or pages that could not be read: no
  `pages`, and the page frames the PDF exactly as before.
- **`/v/ID/typeset-page/PATH?v=VERSION&p=N&w=WIDTH`.** `answer()` takes
  it as /typeset/ (the credential's cookie only, `Answer::typesetPage`),
  then `beginTypesetPage` checks as `beginTypeset` does, in its order: a
  renderer (404 without), LaTeX only, a page's socket open (503), the 32
  files, then the query (`typesetPageAsk`: v, p from 1 and w, digits
  alone, else 400; w clamped to [64, 4096]), `openLocalFile` again, a PDF
  kept, `v` its `pdfVersion` (404 for any other: an old picture is never
  served), sizes kept, `p` in range (404). The width drawn is
  `typesetPageWidth`: never more than `kMaxPagePixels` (24 million) in
  all, so a tall page is drawn narrower. The PNG (image/png, nosniff, the
  sandbox CSP, no-store) goes from memory like the PDF; a drawing that
  failed is a 500, too many waiting a 503.
- **`PageRenders`** (`SessionWebTypeset.cpp`) draws on threads of its own,
  at most `kThreads` (2) at once, started as work comes and joined by
  `~PageRenders` (reset first in `~Server`, so no drawing wakes a closed
  pipe). A page asked for twice while it is drawn is drawn once; the
  connection waits with `FileSend::drawing` (not held to the request
  deadline, as a file being sent is not) and `pumpFile` sends it once the
  wake comes. Pictures are kept by (document, version, page, width): only
  each document's newest version's (asking for a newer version drops the
  older ones, and an older one is refused), within 64 MB, the one used
  longest ago going first; a failure is kept as one, so it is not drawn
  again; at most `kMaxWaiting` (64) wait.
- **The Mac's renderer** (`mac/src/TypesetPages.{h,mm}`): CoreGraphics
  and ImageIO only (`CGPDFDocument`, a bitmap context, `CGImageDestination`
  to PNG), never PDFKit or AppKit, so it runs on the server's threads.
  Sizes are the crop box turned by the page's rotation; a page is drawn
  on white at the width asked, scaled up from the page's own drawing
  transform (`CGPDFPageGetDrawingTransform` never scales up). Each thread
  keeps the last document it parsed (`thread_local`, compared byte for
  byte), so a typeset's pages parse the PDF once a thread.
- **The Mac.** `LatexView` announces each typeset (`onTypeset(path, pdf,
  error)`: both nil as it starts; tectonic missing or the copy not
  written are failures too) and keeps what a page asks about
  (`typesetting`, which counts the typing debounce, `typesetPdf`,
  `typesetFailure`). `makeLatexPreview` calls `wireSessionTypesetOf:`
  (the SessionBrowser category), which forwards each to the window's
  `MCSessionBrowser` (`typesetOf:pdf:error:`, nothing while no server
  runs). The server's `Options::typeset` runs `typesetForSessionPage:` on
  the main thread: a tab of the window on that file (paths compared
  resolved) with a preview gives what the preview has
  (`typesetOf:view:`: its last PDF, then working or its failure), and
  never typesets again; otherwise the `MCSessionBrowser` keeps a preview
  of its own (`typesetInBackground:source:`, a `LatexView` in no window,
  at most four, stopped with the server) typeset from the tab's text, or
  the disk's for a file the window has not open, and again only when that
  text changed (`setPath:source:`). Its hidden sibling and output folder
  are the preview's usual ones (latex.md), gone when it ends.
- **The page.** `showMode` shows `#tex` (`#texnote`, the `#texframe`
  iframe, `#texlog`) for a LaTeX document on its pages and asks
  `latex` each time (the path as it knows it: a tab of its own by its
  path from the folder, a document by the session's path), again after a
  reconnect; `onLatex` sets the frame's address when it changed (ready,
  or working with the last PDF: "Typesetting again…"), shows the log for
  a failure ("The document did not typeset. tectonic said:"), and the
  text for none or refused. Leaving the pages (another tab, Show Source,
  a viewer, a note) empties the frame. `mcSessionView.typeset()` says
  what shows, for the GUI check.
- **The page's pictures.** With `pages`, `#texpages` (a scroller of the
  page's own, in place of the frame) holds one lazy `<img>` a page, laid
  out before any is drawn by its `aspect-ratio` (the page's points), so
  nothing shifts as pictures arrive; `w` is the column's width (at most
  1100 CSS pixels) times `devicePixelRatio`, rounded up to a step of 256
  (256 to 4096), so a small resize asks for nothing new and the server's
  pictures are found again. The place (`texPlace`) is the page whose span
  (its top, the first's from 0, to the next page's top) holds the
  scroller's top, and how far into that span. On a new `pageUrl`
  `drawPages` takes the place, sets each picture's ratio and address in
  place (extra pages removed, new ones added; the browser keeps the old
  picture until the new one loads), and goes back to it (`texGo`); a
  place past the new last page goes to the last page's top. A failure
  hides the scroller and keeps the place for the next that works;
  another document, Show Source, or leaving the pages forgets it. A
  resize keeps the place and draws again at the new step.
  `mcSessionView.texScroll(page, frac)` scrolls there for the GUI check.
- **Not done**: no click to the source, no edit box, no forward search,
  no text selection or find in the pictures, and no Warnings list on the
  page; without a renderer (a headless host, Linux) the frame still
  reloads from the top; a page's typing (an editable page) reaches the PDF
  when the window's preview typesets it, as the window's own typing does,
  so beside the source it waits for a save; an `\input` file typesets
  alone (as the window's preview of it would), not as its main document.
  Linux's port does not publish its previews yet (it has no session
  browser).

#### Checks

`sessionweb:typeset`: `typesetSource` over ten names, `typesetTarget`'s
encoding, `relativeInFolder` (relative, under the folder, through a link
either way, outside, a sibling folder's prefix); `Typesets` (working
once, ready, the same bytes again nothing new, working keeping the PDF,
ready again with the same address, a new PDF a new one, the same file by
a link's spelling, failure keeping the PDF, the failure's cap and
UTF-8, an empty failure, a null PDF, the 64-entry cap); `typesetJson` in
each state; `latex` on a document and a page's own file and not on
others; `answer()` for /typeset/ (decoded, the credential, a NUL, a near
miss, POST); then over the wire with a headless host and a stand-in app
that records what it was asked: 503 before a page, a published PDF
ready at once and the app asked with the file's path, its bytes, a
range and a 416, working with the last address, a 5 MB PDF at a new
address in pieces and a suffix range, the same bytes again sending
nothing, a failure's log, a document's absolute path taken as the same
file, one with nothing yet "working" and 404 until it is published,
nine refusals (outside, a link out, "..", a dotfile, not LaTeX, an
absolute path elsewhere, missing, a folder) with nothing of the outside
file sent and the app never asked, seven refused addresses (404) though
a typeset is kept for the outside file, no cookie 403, `/file/` not
serving the .tex, the file unchanged, and 503 once the page has gone.
GUI: `session_browser`, on the read-only page with a stand-in tectonic:
the window opens a document in its preview, the page follows it and
shows the pages at its /typeset/ address, a fetch from the page gets the
window's PDF byte for byte, Show Source shows the text; an agent's
rewrite read by the window (`checkTabsOnDisk`) typesets again and the
page's frame moves to a new address holding the new PDF; a document the
window has not open, clicked in the page's tree, shows in a tab of the
page's own typeset by the server's own preview, the window opening
nothing, the file unchanged and no sibling left. Checked by hand in
headless Chrome over CDP against a host with a scripted app (not
committed): the click, "Typesetting…", the frame in Chrome's PDF viewer,
a fetch of 200 application/pdf, the second PDF at ?v=2, Show Source and
Show Page. The look in Safari, the viewer's place after a re-typeset,
and a phone need the owner's eyes.

`sessionweb:typeset-pages` (the pictures): `typesetPageTarget`;
`typesetPageAsk` (good, both clamps, eleven bad queries); `typesetPageWidth`
(the clamps, a tall page narrower within the pixel cap); sizes kept with
new bytes only; `typesetJson` with and without sizes (to a hundredth,
kept while working); `PageRenders` (six pages, each drawn once, at most
two at once, the wake, the byte cap keeping the newest, a kept picture
drawn no more, a failure kept, a newer version dropping the older and
refusing it, Busy past 64 waiting, destroyed while drawing); `answer()`
for /typeset-page/ (decoded, the credential, a NUL, a near miss); then over
the wire with a headless host, a stand-in renderer, and a second server
with none: 503 before a page, `pages` and `pageUrl`, a picture at the
width asked and clamped both ways, drawn once for two asks, 404 for a
page past the end, 400 for p=0, no w and no v, 404 for another version
and v=0, 403 without the cookie, five refused paths (not LaTeX, a link
out, "..", an absolute path, missing) with a typeset kept for the outside
file and nothing of it sent, a shorter typeset's new sizes and address
with the old address 404 and its third page 404, a drawing that failed
500, a PDF whose pages could not be read sent with no `pages`, the
server with no renderer sending no `pages` and 404 for a picture, the
PDF's own address unchanged, and 503 once the page has gone. GUI:
`session_browser` now sees the window's six-page typeset as six pictures
(no frame), fetches the first (200, image/png, a PNG's signature),
scrolls halfway into the third page, and after the agent's rewrite (six
taller pages) checks the pictures' new address, the page still halfway
into its third page and the scroller's offset moved to keep it; the
server's own preview's one-page typeset shows as one picture. The
pictures' sharpness on a Retina screen and in Safari, and a phone, need
the owner's eyes.

### Source Control and the review in the page (R3, fourth slice)

The owner's ask (R3): git and review pages did not show in the browser
view. Now the page's sidebar has Source Control beside Files, listing the
folder's uncommitted changes as the window's panel does, and a file clicked
shows the review page of those changes (the window's Uncommitted Changes
page) in a tab of the page's own, inline or side by side. Read only, on
every page, editable or not.

- **The core reads; the server runs it.** `src/SessionWebGit.{h,cpp}`:
  `GitReads` runs the core's `ChangeReview::readUncommitted` (the GTK
  review page's reading: one status, ls-files, ls-tree, cat-file --batch,
  then the disk) on a detached thread of its own, one read at a time, an
  ask while one runs being one more read; a read that shows the same
  (`ChangeReview::fingerprint` with the top level, branch line and error)
  is no new `version`. Its destructor never waits: the thread holds the
  state by `shared_ptr`, and the wake is cleared under the lock, so
  `~Server` resets `git_` first, before the wake pipe closes.
  `Read::branchText` is new (the panel's branch line).
- **git with fixed arguments.** The page never names a command, an
  argument or a path to git: `git {watch}` asks for a read, `review
  {opened, side, file}` picks from the last read by file key (a key no
  file has is `gone`), the gap ids only a set the layout looks up
  (`openedFrom`, at most `kMaxOpenedGaps` 4096; `sideFrom`). The Mac's
  `Options::git` runs `MCGitRunSync`/`MCGitRunSyncWithInput` (the git it
  found, never /usr/bin/git's shim, its environment and time limits); a
  headless host's is the core's `runGit`: `posix_spawnp("git", "-C",
  dir, ...)`, no shell, its own process group, SIGPIPE default, stdin from
  an unlinked scratch file (no pipe to fill), GIT_OPTIONAL_LOCKS=0 (a
  status never rewrites the index), GIT_TERMINAL_PROMPT=0, GIT_EDITOR=true,
  GIT_DIR, GIT_WORK_TREE and GIT_INDEX_FILE dropped, SIGTERM at 30 s and
  SIGKILL 2 s later, 256 MB of output at most.
- **Held to the folder** (`confineReview`): a file outside the folder (a
  folder below the repository's top level) is dropped, a dotfile too
  unless the tree shows them (the page's read rule), and a work tree file
  that `lstat` says is a link, or whose `realpath` is outside the folder,
  is made a symbolic link's note with no text (`readUncommitted` reads the
  work tree with `readFileStable`, which follows links: an untracked link
  to a file outside would have shown it). A folder git places outside its
  own top level shows the "not in a repository" error.
- **What the page gets.** `git` (`gitStatusJson`): version, top, branch,
  initial, files ({key, section, letter, path, orig, header, added,
  removed}), count, error. `review` (`reviewJson`): one
  `ChangeReview::layout` of every file (three lines of context, gaps
  closed unless opened), each file's rows as arrays (`c`, `r`, `a`, `x`
  for a conflict line with its part, `g` a gap with its id and count, `n`
  a note), syntax and changed words as [start, length, style] in UTF-16
  units; a file asked side by side is laid out from its `SideBySide` rows
  with the same folding and gap ids (`s` rows, kind c, m, r, a, the words
  on both sides). Past `kReviewRowBudget` (20,000) rows a file comes as
  its header with `more` and the page asks for it alone (`reviewfile`). A
  row is cut at `kReviewRowChars` (2,000 units) with an ellipsis. The
  window's "Side by Side or Open shows it" note for a large file reads as
  the window's review showing it.
- **Updates.** A page that sent `git` watches until `git {watch: false}`
  (the page sends it when neither Source Control nor the review tab is on
  show). `Server::gitChanged()` (any thread) marks the read stale; the
  loop asks for one at most a second after the last (`takeGit`), only
  while a page watches, and sends `git` to each watching page whose last
  version differs, or that asked. The Mac calls it from the window's
  FSEvents callback (`sessionFolderChangedOnDisk`, the SessionBrowser
  category's hook in EditorController+Tree.mm), which sees writes under
  the folder and .git inside it: an agent's edit, git in a terminal, the
  window's own panel. **A limit**: a folder below the repository's top
  level sees no .git events, so a commit made elsewhere shows at the next
  change under the folder (as the tree's git colors). A host that watches
  the disk (`mutible host`, and the Mac's own since stage 7's watching:
  "The host watches the disk", below) says so with FsChanged's `git`,
  which calls `gitChanged` too; the page still reads again on ↻, `hello`
  (a reconnect) and `visibilitychange`, for a host that does not.
- **The page.** `#sidemodes` (Files, Source Control) swaps `#tree` for
  `#git` (`#gitsum` the branch and count, `#gitlist` the rows: name,
  folder, letter in the tree's git colors, `paletteJson`'s new `git`);
  ↻ reads the mode on show. A row's click opens the review tab (`L.tabs`
  entry path `/changes`, which no relative path is, label Changes,
  `view: "review"`) at that file (`G.goto`), drawn in `#fileview.review`
  by `textContent` only, each file a sticky header with Side by Side (or
  Inline, or Show Changes), a gap row's click opening it (`reviewfile`).
  A new `version` asks for the review again with the page's opened gaps
  and side-by-side files, and keeps the place by the file at the top.
  Syntax colors reuse the theme's rules (`#review .sN`); tints are the
  Mac's review's (removed #F14C4C at .16, added #73C991 at .16, words .38
  and .36). `mcSessionView.git()` and `review()` say what shows.
- **Not done here** (the window's commit and worktree pages and the
  pictures came next, below): no graph, no Open of a file from the
  review, no F7. Nothing stages, discards, commits or applies, by design.

#### Checks

`sessionweb:git` (a real git in a scratch repository): `runGit` (output,
a failure's status, stdin, not a repository); `readUncommitted` held to
the folder: the staged file first, a change, a new and a deleted file, a
dotfile left out (in with hidden), an untracked link to a secret outside
shown as a link with no text, a subfolder showing only its file, an error
outside a repository; `gitStatusJson`'s sections and letters; `reviewJson`:
removed and added rows with numbers, changed words, syntax spans, a gap,
the gap opened, side by side with the changed pair's words on both sides
and the same gap id, a 5,000-character line cut, a link's note, a
deleted file's removed row, the row budget's `more`; `openedFrom` and
`sideFrom` capped; `GitReads` (a version only for something new, the
wake, none once destroyed mid-read); then over the wire with a headless
host: a review before any read pending, the list, the review with a file
side by side, one file, a key no file has, nonsense answered as the whole
review, `gitChanged` reaching a watching page with a new version, nothing
once it stops watching, no byte of the secret, and the status and the
index file byte for byte as they were. GUI: `session_browser`, on the
read-only page: the folder made a repository with a staged change, a
change and a new file; Source Control lists them staged first; a click
opens the review at that file with the removed and added lines, changed
words, syntax colors and only Side by Side buttons, the window opening
nothing; Side by Side pairs the changed lines; the gap opened shows every
line; an agent's edit on disk reaches the review with no press, side by
side and the gap kept; HEAD and the staged list as they were. The look in
Safari, a long review's scrolling, and a phone need the owner's eyes.

### The window's commit and worktree pages, and pictures (R3, last slice)

What #262 left: the window's open commit pages and worktree pages
(Compare with Main) on the page, and the review's pictures (#217's image
diffs). Both read only, on every page.

- **The window says which.** `WindowTabs` gains `reviews`
  (`Web::reviewsReportJson`: [{kind "commit", hash | kind "worktree",
  path, title, onShow}], at most 32, a page that does not hold up left
  out). The Mac's `sessionBrowserReviews` (the SessionBrowser category)
  lists the `MCChangeReviewPage`s with a `commit` or a `worktree` in both
  groups' strips, the tab on show marked; `MCSessionWindow reviews` puts
  them in the record, so they ride the half-second report. The window's
  Uncommitted Changes page is left out (the page has its own).
- **The server checks it again** (`lookAtState`, `reviewPagesFrom`: the
  report is a renderer's): a known kind; a commit by 4 to 64 hex digits
  only (`commitName`; never `HEAD~1`, never `--output=...`); a worktree by
  an absolute path of valid UTF-8 with no control character; titles cut
  at 300 bytes; each id once. The page gets `reviews` [{id, kind, title,
  onShow}] in the window's state (`reviewsViewJson`), ids
  `commit:HASH` and `worktree:PATH`, and `Session::reviews` keeps what
  each id stands for. Another window's pages are never listed (the
  window filter comes first).
- **A page picks by id, git reads by the host's own state.** `review
  {page}` (any page) names an id; one the state does not list is answered
  `error` ("no longer open in the mutible window") and nothing runs. The
  first ask makes the page's `GitReads` over `readReviewPage`, on a
  thread of its own as the uncommitted read: a commit is the core's new
  `ChangeReview::readCommit` (the Mac's `ReadCommit` ported: `show -s`,
  `diff-tree -r -z -M` against the first parent or `--root`, one
  `cat-file --batch`; `review-read:commit`) in the session's folder's own
  repository, whatever root the window's page had, then `confineReview`
  to the folder; a worktree only when `worktree list --porcelain` run in
  the folder names it (realpath compared; not the main checkout, not a
  bare one), compared with `AgentView::mainRef` of that list (the main
  checkout's branch, never a name the report gives), read by the core's
  `readWorktree` and held by `confineReview` to the worktree's own folder.
  `readWorktree` now marks each file read from the disk `workRead`, so
  an untracked link in the worktree (to a secret elsewhere) is shown as a
  link with no text. Pending first, then `reviewread {page, version}`
  for each new version the page has not had, and the page asks again
  (`review {page}` answers from the last read at once). A worktree page
  shown is read again on `gitChanged` (the window's FSEvents see a
  worktree under the folder; one elsewhere is read again on ↻ and on
  coming back to the page), at most once a second with the uncommitted
  read; a commit page never (a commit does not change) except on ↻
  (`reread`). `watch: false` (leaving the tab) stops it; a page the
  window closes is let go (its read dropped, the conn's `reviewPage`
  cleared) at the next state.
- **The header** (`ReviewRead`'s title, detail and body): a commit's
  subject, its byline (`commitByline`) and the counts, the rest of its
  message; a worktree's "x compared with main", "against main at the
  branch point (1a2b3c4)", its branch and the counts. `review` carries
  them with `page`.
- **Pictures** (every review, the uncommitted one too): the page's reads
  give `ImageReading` `pageImages()`, whose `pictureSize` reads PNG, GIF,
  JPEG, WebP, BMP and ICO sizes from their headers (no decoding; a TIFF
  or HEIC file, which Chrome cannot show, stays the binary note), so
  `showAsImages` makes the file a picture under the window's caps (64
  megapixels, 4 MB a side). `reviewFileJson` gives such a file `images`
  [{side, label (the window's `imageLabel`), width, height, bytes, url}]
  and no image rows; without a URL maker the old note stays. The bytes
  are the read's (`Extra::oldImage`, `newImage`), served at
  `/v/ID/picture/PAGE/VERSION/FILE/SIDE` (`pictureTarget`; PAGE 0 the
  uncommitted review, else the number the server gave the page's reads):
  numbers only, so the address names no path or key. `answer()` takes it
  as `/file/` (the credential's cookie, Host and Origin), then
  `beginPicture`: a page's socket open (503 before), counted among the
  32 files at once, the read named by PAGE, only while VERSION is its
  latest (404 once the changes moved on: the page asks for the new
  review anyway), only a file shown as pictures and a side it shows, the
  kind `fileType` serves as an image (its mime, nosniff, no-store, the
  sandbox CSP, its 64 MB cap), Range as for a file, from memory
  (`FileSend::mem` aliasing the read, which stays alive while it is
  sent). `confineReview` now also drops a linked file's pictures (their
  bytes came through the link). `printOf` hashes the pictures' bytes, so
  a picture that changed alone is a new version.
- **The page.** The window's pages are tabs in the strip after its
  documents (class `review`, no ×: only the window closes them), titled
  as the window titles them; a click shows the review in `#fileview` as
  the Changes tab does (`L.tabs`' entry made from the state,
  `windowReviewTab`, path `/review/` + id). Per review state (`RV`, by id,
  "" the uncommitted one): its answer, opened gaps, side-by-side files.
  A page review has `#review .rtop` (title, a Read-only mark, detail, the
  body in pre-wrap) and no section headings (its files are Plain). A
  file's pictures are `.rimgs`: a column each, its label above an `<img>`
  at the pixel size (`width`/`height` attributes for its room before it
  loads), never taller than 320, on the checkerboard; an image that fails
  says so in its label. A page the window closes leaves the strip, and
  if it was on show the page goes back to the window's tab
  (`dropGoneReviews`). A reconnect asks for the review on show once the
  state lists it again. `mcSessionView.review()` gains `page`, `title`,
  `detail`, `body`, and each file's `images` (with `naturalWidth`).
- **Not done**: the page does not follow the window to a review page it
  shows (the tab on show is marked `onshow`, nothing more); no Open of a
  file and no F7 from these pages; GTK reports no review pages (it has no
  session browser); the Mac's own pages still read with ChangeReviewPage.mm's
  `ReadCommit` and `ReadWorktree`, beside the core's twins.

#### Checks

`review-read:commit`: `readCommit` over a scratch repository (the header,
the parent, three Plain files with the commit's text and not the work
tree's, an added file, a picture's two versions and bytes, a root commit,
a name that is no commit, no repository). `sessionweb:pictures`:
`pictureSize` over PNG (cut, zero, too wide), GIF, JPEG (and scan data
first), WebP VP8X, BMP top-down, ICO's largest entry, text, empty, TIFF;
`pictureTarget` and `pictureParts` over 12 bad parts; `answer()` refusing
/picture/ without the cookie. `sessionweb:review-pages`: the report and
its check (a hostile report: an option, `HEAD~1`, short and long names, a
relative path, a newline, an unknown kind, non-objects, a 400-byte title,
a duplicate; the 32 cap; the view leaves out the hash); against a real
git with an agent's worktree outside the folder: a commit's header,
detail, files held to the folder (no dotfile unless hidden, a subfolder's
one file), its picture's images JSON with the window's label and the
addresses, the note without them, names that are no commit (an option
among them, which writes nothing); the worktree's title and detail, its
change, its new picture, the dotfile left out, the untracked link to a
secret shown as a link with no bytes, not the main checkout, not a folder
that is no worktree; `GitReads` over a page with a picture that changed
alone giving a new version; then over the wire with an in-process host
whose window reports both pages and two that do not hold up, and another
window one of its own: the state's ids and titles only, pending then
`reviewread` then the review with its header, the pictures' bytes over
HTTP (type, nosniff, sandbox), refused without the cookie, under another
version, at five made-up addresses and for a text file; one file by key;
two pages the window does not have; the worktree page with the link and
its new picture; `gitChanged` bringing a new version; the window closing
the commit page (the state, the ask and its picture all refused); no
SECRET or TOKEN in anything sent; HEAD, the status and the worktree's
index unchanged. GUI: `session_browser`, on the read-only page: two
commits with a picture made bigger, and an agent's worktree (outside the
folder) with a change and a new picture; the window opens the commit's
page and the worktree's; the page's strip lists both (not as its own);
a click shows the commit's page with its title, its change, and the
picture's Before and After decoded at 2 and 4 pixels wide from /picture/
addresses, labeled as the window labels them, only Side by Side, the
window opening nothing; the worktree's tab shows "compared with", the
agent's change and the new picture decoded, no Apply to Main; the window
closing the commit's page takes its tab off the page, the worktree's
staying on show; closing the worktree's page puts the page back on the
window's tab. The look in Safari and Chrome (the header, the pictures'
row, a tall picture capped at 320), a phone, and a big worktree page
need the owner's eyes.

### The window's browser tabs in the page (R4)

The owner's ask (2026-11-18): the page lists the window's browser pane, the
web pages beside the editor, as a "Browser" group in the tab strip, and
frames the one picked.

- **The report.** No new message: the window's `WindowTabs` json gains
  `browser` {tabs: [{url, title}], active} (`Web::browserReportJson`:
  empty and over-8 KB addresses left out, the tab on show counted among the
  rest, titles cut at 300 bytes on a character, at most 100). The Mac's
  `MCSessionBrowser reportTabs` adds it when `browserTabs` gives one
  (EditorController+SessionBrowser's `sessionBrowserTabs`: the pane's
  `tabAddresses`, `tabPageTitles` and `selectedTab`; nil while the window
  has no pane), so it rides the half-second report and the host keeps it
  in `windows_` with the rest. A headless host has no window and GTK
  reports none: no `browser`, no group.
- **Only that window's.** `lookAtState` already drops other windows
  (`Options::window`); it replaces each kept window's `browser` with
  `browserViewJson`, which checks the renderer's report again and marks
  each tab `web` (`webAddress`: only these are framed) and `loopback`.
- **Opening an address** (`browse {url}`, answered `browsing {url, ok,
  why}`): only from an editable page (a read-only one gets a note, before
  anything else) and only when `Options::browse` is set (the hello's
  `canBrowse`; the page shows its field only then). `webAddress` takes
  http and https with a host, no user name or password before it (a
  `http://good@evil` trick), no space or control character, at most 8 KB;
  javascript:, file:, data:, about:, blob:, view-source: and the rest are
  refused in the core, never left to the Mac. The Mac's `browse` runs
  `openSessionAddress:` on the main queue: `agentBrowserPane` (made and
  shown, no home page), then `openAddresses:` (a tab at the address shown
  again, else a new tab), the keyboard kept where it was.
- **The page.** The pane is `#websec`, beside `#docsec` (R7: it was a
  "Browser" group in the editor's strip, shown in the editor's place, the
  owner's bug a), with its tabs in `#webtabs`: the title and address, an
  "Open in a new tab" link
  (`rel="noopener noreferrer"`), always shown with the frame because a page
  cannot tell a site that refused framing (X-Frame-Options, CSP
  frame-ancestors) from a blank one, a muted line for a loopback address,
  and the frame. The pane follows the window's tab on show until one is
  picked (`W.follow`); hidden, its frame is set to about:blank so the
  framed page stops. An address the page asked for is shown once the
  window's state lists it (`takeWebPending`, trailing slashes ignored, 15 s).
  Typed addresses with no scheme get http:// for localhost, 127.x and [::1],
  else https://.
- **The CSP** gained `frame-src 'self' http: https:` (was `'self'`): every
  other directive is as before (default-src 'none', the script nonce,
  connect-src to 'self' and ws:// loopback, frame-ancestors 'none',
  base-uri and form-action 'none'). The frame is sandboxed without
  allow-top-navigation (a framed page cannot take this page away), with
  `referrerpolicy="no-referrer"` (the page's address holds the token, and
  the page's own Referrer-Policy is no-referrer too). The page itself
  cannot be framed back (X-Frame-Options DENY, frame-ancestors 'none'), so
  framing this server's own address shows nothing of the session.

#### Checks

`sessionweb:browser` (webAddress over 6 good and 23 refused addresses,
loopbackAddress, the report's cuts and active index, through the
protocol's encoding unchanged, browserViewJson on a hostile report; then
an in-process host with two windows: only window 5's tab in the page's
state, a change reaching it, http and https opened and four other
addresses refused before `Options::browse`, a read-only page refused with
a note and no `canBrowse`, a server with no pane saying so; the CSP's
directives). GUI: `session_browser` types javascript: into the page's
field (refused, nothing opened), then an address on this Mac, which opens
in the window's browser pane, and the page lists it under Browser and
frames it with its link and loopback line. The look (the strip's group,
the frame, the narrow layout) needs the owner's eyes.

### The page's panes, its own tabs, closing, and terminals that end (R7)

The owner's four bugs on 1.6.1's read-only page (2026-11-18): the browser
pane drawn in the editor's place rather than as a panel, no way to close
a tab opened from the tree, no way to close the browser, and a terminal
closed on the Mac staying on the page.

- **Panes as the window has them.** The window's `WindowTabs` json gains
  `panes` {browser, terminal, browserShare} (`Web::panesReportJson`;
  `MCSessionBrowser panes`, from `sessionBrowserPanes`: `browserVisible`
  with a pane, `terminalVisible` with a panel, `browserFraction`;
  Maximize Pane is not followed). `lookAtState` passes it on checked again
  (`panesViewJson`: booleans, the share held to 0.1..0.9). The page lays
  out `#top` (`#docsec`, then `#websec` at `--webshare` of the width; a
  column at 700 px and under) above `#termsec`. Each pane shows while the
  window shows it and has something in it (a host that sends no `panes`,
  a headless one: shown); the page's own choice (`P[name].mine`, the ×
  in the pane's row and the Browser and Terminal buttons in the header)
  wins until the window shows or hides that pane again. **Hiding is the
  page's own, read-only and editable alike** (the owner left the editable
  case to us): a page is another screen with its own room, a phone's
  especially, and hiding the Mac's pane from it would change the window
  under the person at the Mac; the page asks nothing of the window for
  it. An address an editable page opens shows the pane on the page.
- **Terminals that end leave at once** (bug d). The bug the owner met was
  Toggle Terminal: the panel hid but its shells ran, the window kept
  reporting them, and the page kept its pane; now `panes.terminal` hides
  it. Terminals that really go were already dropped through the window's
  `terms`; besides that, the view's `Session` now overrides `termExited`
  (the host tells every attached renderer when a shell is reaped) and
  looks at the state at once, and `lookAtState` lets go of every
  `TermView` and page `term` not in the state it sends, for a headless
  view (`window` 0) too, which kept dead views before.
- **The page's own tabs** (bug b). `readfile {path}` (any page) is
  answered `filetext` by `Web::localFileJson(folder, rel, showHidden)`:
  `confinedPath`, no dotfile name unless the tree shows them, opened
  `O_NONBLOCK` and `fstat`ed as a regular file of at most 8 MB, refused
  with a NUL in its first 8 KB, then made into a `DocView`'s snapshot (the
  same lines, spans, Markdown page and indent a `doc` has). It is read
  from the disk by the server, not through the host: the file need not be
  open anywhere, and the server runs where the folder is (in the app, or
  `host`/`attach --web` on that machine). The page keeps them in `L.tabs`,
  drawn after the window's tabs in italics with a ×; showing one sends
  `show {doc: 0}` and fills `D` with id -1 (not editable), and closing it
  goes back to the window's tab on show. A file the window has open is
  shown from its tab instead (live). Readers and pages with no `canOpen`
  (a headless host's) use this; viewer kinds get a note.
- **Closing the window's tabs** (editable pages). `close {doc}` is answered
  `closing {doc, ok, why}`; refused for a reader, a document not this
  window's, and a dirty one (the host's flag in the state: the Mac would
  ask on its own screen, so the page says to save or close it there);
  else `Options::close` (the Mac's `closeSessionDocument:`: the tab's
  `dirty` checked again, then `closeTabEverywhere:prompt:YES`, both
  groups, so the page's tab, which stands for the document, goes). The
  hello's `canOpen` and `canClose` say which a page has; a reader's
  `Options::open` is no longer set on the Mac.

#### Checks

`sessionweb:tree` (a reader's `open` refused and nothing opened, its
`readfile` of a C file and a Markdown file as lines with spans and the
page, eight refused reads: a folder, two ways out, a dotfile, a binary
file, a missing one, an absolute path; an editable page's opens reaching
`Options::open`; a headless page reading); `sessionweb:close-and-panes`
(the panes report and its check, through the protocol; an in-process
host: window 5's panes in the state, a hidden terminal panel reaching the
page, closing a clean tab, refusing a dirty one, another window's, an
unknown and a malformed id, a reader refused, `canClose`);
`sessionweb:terminal-ends` (a headless view with a minute's poll: a
terminal its renderer closes and one whose shell exits leave the page
within seconds, and the closed one shows nothing when asked for by id).
GUI: `session_browser` sees the browser pane beside the editor, the
page's × hide it there only, the window hiding and showing its pane
followed, an editable page's × close a window tab, Toggle Terminal hide
and show the page's terminal pane, and the closed terminal's pane go;
then a read-only page whose tree click opens a tab of its own (the
window's tabs unchanged, no × on them) that its × closes. The look of the
panes and the ×s needs the owner's eyes.

### Saving through the host (stage 7's first write)

Stage 7 is the workspace's I/O in the host. Its first write: an editable
page on a headless `mutible host` saves the document on show, and the
host, not an app, writes the file and keeps its unsaved state against the
disk. Before this the page said to save from a window, which a headless
host does not have.

- **The protocol** (additive, SessionProtocol.h): `DocSave {doc, req,
  window, flag 1: overwrite}` (renderer to host) and `DocSaveDone {doc,
  req, outcome, v, json}` (the answer: Done with the version written, or
  Refused with `saveDoneJson`'s `{path, ok, reason, why, note, encoding,
  created}`); `DocDirty {doc, flag 1: dirty, v}` to each renderer whose
  Hello had flag 8 (`Client::hearDirty`), whenever a document's dirty flag
  flips, whoever flipped it (an edit, an undo, a reset, a host save, an
  app's DocSaved). The host's Welcome now carries flag 1 ("this host
  saves", `Client::hostSaves`), and `Client::requestSave` sends nothing
  (returns 0) to a host without it. DocSave goes through the client's
  outbox, so it reaches the host after every edit the renderer sent before
  it; the answer is `saveAnswer(req)` after a dispatch.
- **The rules** are the core's `Shared::saveDocument` (SessionFiles.h),
  the Mac's save as `docs/notes/sessions.md` gives it: `FolderSearch::
  writeFileAtomically` (a link stays a link, hard links and other owners
  in place, fsync'd), the file's own encoding and mark (UTF-16, a UTF-8
  mark, Windows-1252, Utf8Raw round-trip byte for byte), the text's own
  line endings, and a Windows-1252 file whose text now holds a character
  it lacks written as UTF-8 with a note ("Saved x as UTF-8: Windows-1252
  cannot store “→”", the Mac's words). **The disk check**: unchanged when
  the file's stamp is the one the host wrote it with (`Model::Document`'s
  stamp, set by `markSaved(written)` after a host save); with no stamp
  (the host never wrote it: an app read it, or an app's DocSaved, which
  resets the stamp), the file is read (`readFileStable`, 512 MB at most)
  and decoded, and is unchanged when its text is the host's saved text,
  which also gives the encoding. Otherwise `changed` (or `gone`, when the
  file went and the document had text from it) is refused with a
  sentence, and only `overwrite` writes: in the encoding the host last
  wrote the file in when it has a stamp (as the Mac's Keep Mine writes
  the tab's), else the file's own as it is now. A path the document
  never had a file at (no stamp, saved text "") is made.
- **Who may save what.** A guest's save (the page's server) is held to the
  session's folder (`readRoot(window)`: a headless host's own): the
  file's folder by its real place, and the file's own real place when it
  is there, so a link out is refused (`outside`) and a link to nothing too
  (`notfile`). And while an app's window that saves by itself has the
  document open (a seat of a renderer whose Hello had flag 4 and not 16,
  `shownByAppSavingItself`: neither app's now, since the Mac's and
  GTK's tabs save through the host, stage 7's second and third writes;
  an app from before them), the guest's save is refused (`app`, "x is open
  in a mutible window: save it there"): two writers would each keep a
  stamp the other's write moves, and the app has its own questions. A
  renderer that is neither guest nor reader (an app) is not held to the
  folder. A reader's DocSave is refused
  by `refusedFromViewer`, as every change is.
- **After a save** the host's document takes the encoding written and the
  stamp (`markSaved`), so its next save needs no read; the backups' rule
  runs (`backupsChanged`: a saved document needs none) and `tellDirty`.
  The write is synchronous on the host's thread, as the host's backups
  are (an F_FULLFSYNC each); a larger file would want a worker, with
  stage 7's bigger reads.
- **The web server** (`Server::onSave`): with `Options::save` (the Mac)
  nothing changed, but the document must be the window's (`allowed`).
  Without it, `requestSave(doc, Options::window, overwrite)`, kept in
  `Session::saveAsks` and answered to the asking page in `takeOutcomes`
  as `saved {doc, ok, version, path, reason, why, note, encoding}`. The
  hello's `canSave` is `editable && (Options::save || hostSaves())`. The
  server's client hears DocDirty (`hearDirty`), which makes the loop look
  at the state at once (`Session::dirtyCame`), so every page's dot
  follows within a turn rather than the half-second poll.
- **The page**: a Save button in the doc bar (`#savebtn`, on an editable
  page with `canSave` for a document it edits; marked with a dot while
  the host counts it unsaved, `drawSave` from `drawTabs` and
  `drawSelection`), ⌘S (Ctrl+S elsewhere) in the editor as before and
  now anywhere on the page but the terminal and fields (a document
  keydown, so the browser's Save Page As never opens). The save waits
  for the page's edits to be confirmed (`afterSync`), as before, and
  sends `overwrite`. `saved`: ok shows "Saved x" (or the encoding note) as
  a toast; `changed` or `gone` asks with `confirm()` ("…\n\nSave
  anyway?") and saves again with overwrite; anything else is the toast of
  `why`.
- **The Mac** (since stage 7's second write, below): its page still
  saves through the window (`Options::save`, the disk questions on its
  screen), and its tabs' saves are now the host's writes. GTK's too
  since stage 7's third write (below), so a page may save a file a GTK
  window shows.

#### Checks

`host:save-rules` (tests/session_save_tests.inc, `saveDocument` over real
files): UTF-8 with CRLF kept and the written stamp the file's;
Windows-1252 written back in it, then a character it lacks written as
UTF-8 with the Mac's note; UTF-16 LE with its mark unedited byte for
byte, UTF-8's mark kept, a UTF-8 file with a stray byte kept edited and
not; changed on disk refused with the file as it was, then overwritten,
in the encoding the host last wrote (Windows-1252 over an agent's UTF-8);
deleted refused with nothing made, then made with overwrite; a new file
made; the host's stamp trusted, then a writer since refused; held to a
folder: a subfolder and a link inside written (still a link), five ways
out refused with the outside file unchanged, a link to nothing and a
folder refused; no path, a lone surrogate; the answer's JSON. `host:save`
(a host over the encoding: an editor, a guest and a reader that hear
DocDirty, an app): typing tells the hearers once, the guest's save
writes CRLF and the version and tells them clean, a second save by the
stamp, an agent's write refused with the sentence and then overwritten,
an app's window holding it refused with its sentence, the app's own
write and DocSaved clean and the host's next save comparing the file and
writing, a document outside the folder refused to the guest and written
by the editor, no file yet, a reader refused by the host with nothing
written, a host from before saving (no Welcome flag) never sent DocSave,
the messages through the encoding. `sessionweb:save` (a headless host
over its socket, the page a scripted WebSocket, the state polled once a
minute): `canSave`, the page's typing marking it dirty at once, `saved`
ok with Windows-1252 and CRLF on disk and the dot gone, an agent's write
refused as `changed` with nothing written and then `overwrite`, at least
five states (each flip sent at once), a read-only page's `canSave` false
and its save a note. Not run: the page's Save button, its dot and the
`confirm()` in a real browser (no GUI suite drives a headless host's
page saving; `session_web` loads a headless page but does not save), so
the look and the question need the owner's eyes.

### The Mac's tabs save through the host (stage 7's second write)

With editor.session-host on (or headless), a Mac tab's save is the host's
write, so the host is the file's one writer and a page may save a file a
Mac window shows. With the setting off nothing changes: no tab has a
document, and `writeText:toTab:interactive:` writes as before.

- **The protocol** (additive, SessionProtocol.h). The host's Welcome adds
  flag 2 ("saves for apps", `Client::hostSavesForApps`); the Mac saves
  through a host only with it. The app's Hello adds flag 16 (with 4,
  `Client::setSavesThroughHost`): its windows save through the host. An
  app's DocSave carries `json {encoding: encodingName}`, the encoding the
  tab writes (`SaveAsk::keepEncoding`: kept whatever the file holds, and
  with overwrite the file is not read at all, since the window asked its
  questions). DocSaveDone's json adds `encodingName` and `stamp`
  (`stampJson`: exact, the device and inode as decimal strings). After
  any host write, every other renderer that hears DocDirty gets it with
  flag 2, the version written and `{path, encodingName, stamp}`, whether
  or not the state flipped (`tellWritten`; the asker gets only the flip,
  having DocSaveDone); `Client::hearWrites` hears them. DocSaved gains
  flag 1 with `text16`: the file holds that text, not the host's (Keep
  Mine), `Document::markSavedAs`.
- **The rule for a guest's save** is now "refused while an app that saves
  by itself has it open" (`shownByAppSavingItself`: a seat of a renderer
  with flag 4 and not 16, an app from before stage 7); the Mac's windows
  no longer hold a page off, nor GTK's since its tabs save through the
  host (stage 7's third write, below).
- **The write** (`EditorController+Session.mm`, `writeTextThroughHost:`):
  everything before it is the window's as it was: Save As's name,
  `mayWriteTab:` (Keep Mine, Reload, Cancel; gone asks), Save All's
  questions first, format and organize on save and the formatter's
  consent, keep-sorted. Then an untitled buffer's path goes first
  (`pathChanged`, put back if the write fails), and
  `saveOnHostEncoding:bom:` closes the turn's step, sends DocSave
  (overwrite, the tab's encoding) behind every edit, and waits for the
  answer (`settle`, 10 s). Refused or unanswered: the alert "“x” could
  not be saved." with the host's sentence (the status bar for
  auto-save). Written: the tab takes the encoding written (a
  Windows-1252 file holding what it cannot store is UTF-8 now, and the
  host's note, the Mac's words, goes to the status bar), the stamp from
  the answer, and `tab:tookSaveOf:stamp:dirty:` (shared with the window's
  own write: savedText, gone, dirty, Keep Open for a preview tab, didSave,
  the LaTeX previews, the title). A lost host, or one without flag 2: the
  window's own write, as before.
- **A page's save** (or any write the host makes for another renderer):
  DocDirty flag 2 reaches `MCSessionDoc hostWrote:json:` (the clients'
  `hearWrites`, routed by client and id through `DocRegistry`), which
  takes the tab's text as the text written when the mirror is at the
  version written, and hands `MCSessionWrite` to the window next turn
  (`tab:writtenByHost:`): the file's stamp must still be the one written
  (else it is a change outside, for the disk checks), the encoding taken,
  the text written (or the file's, read, when the tab was not in step),
  and `tab:tookSaveOf:` with the host's stamp, so the next Cmd+S does
  not ask about a change made outside.
- **Dirty from the host** (`tab:dirtyFromHost:`, from `hostDirty:`): when
  the host's count flips while the mirror is the host's, the window's
  dot is the host's, as the page's is. The traps above say when it is
  not taken, where the window tells the host its saved text, and why a
  DocDirty heard behind the window's own DocSaved waits for the host to
  take it (the client's fence, 2026-11-21).
- **Hot exit, backups, auto-save.** Quitting writes backups, not files,
  so it is unchanged (a headless host keeps the documents as Keep Unsaved
  leaves them). A save leaves the tab clean, so `updateTitle`'s
  `scheduleBackups` drops its backup as before, and the host's own
  backups follow DocSave (`backupsChanged`). Auto-save writes through the
  host with no question (`writeTabForAutoSave:` checks the disk by hand
  first, as before), failures in the status bar; its timer may now wait
  for the host's answer, a round trip in one process.

#### Checks

`host:save-for-apps` (an app with flag 16 and a guest over the
encoding): the app's save of a UTF-16 file an agent rewrote as UTF-8,
with overwrite and its encoding, written UTF-16 with its mark, the answer's
`encodingName` and stamp the file's; the guest told of the write (flag
2, the version, the path, the stamp), the app only of the flip; the
guest's save while the app has the file open written in the encoding last
written, and the app told with the version it is at; a save with nothing
changed told to the others and not the asker; a GTK-like app (no flag 16)
holding it refusing the guest again, until it detaches; Windows-1252 with
an arrow written as UTF-8 with the Mac's note, unedited Windows-1252 kept
so though the host never read it, a deleted file made again; DocSaved
flag 1 counting the document unsaved against another text and clean
against its own; the stamp's JSON exact through the wire (a device and
inode past 2^53), a DocSave's encoding through the wire; a host with only
flag 1 sent no encoding and no `savedAs`. `host:dirty-fence` (queued
messages taken in the order CI met): a reload's edit and `saved()` never
tell "unsaved", the "saved" told once the host took the DocSaved, typing
told at once after, a `savedAs` that flips nothing telling nothing, two
saves out held until the last, another document's edit in flight
leaving this one in step (`unconfirmed(doc)`), no Ping from a renderer
that does not hear. `host:save` and the
`sessionweb:*` groups unchanged. GUI: `host_reattach`'s stage 7 part on a
headless host (its header): a page's guest over the socket saves a file
the window shows with typing in it, the window's tab and its strip dot clean and
in step with the file's stamp, the host counting it saved; the window
types and saves with no question and the guest hears the host's write
with the file's stamp; a UTF-16 file saved from the window byte for byte
with its mark; an agent's write then Keep Mine written over by the host.
CI's pass with the host (`MUTIBLE_SESSION_HOST=on`, loopback) runs
`save`, `backups`, `format` and the editing suites with every save going
through the host, which is what checks the encodings, links, hard links,
changed and deleted files, and auto-save through it. Not run here (no
GUI on this machine for the builder): every GUI check above; the dot's
look and timing on screen need the owner's eyes.

### GTK's tabs save through the host (stage 7's third write)

The Mac's second write on GTK (linux/HANDOFF.md item 132). GTK always has
a host (in process by default, headless with `MUTIBLE_HOST=socket` or
`editor.session-host = headless`), so every GTK save of a tab with a host
document is the host's write: the host is the file's one writer, and a
page may save a file a GTK window shows.

- **The link** (`linux/src/SessionLink.cpp`, `makeClient`): every client
  says flag 16 in its Hello (`setSavesThroughHost`, with 4), and hears
  DocDirty and the host's writes (`hearDirty`, `hearWrites`), handed to the
  document by its id (`Link::listen`, `DocNotices`; each document enrols
  when made and on `rehost`, and unenrols when it goes).
- **The write** (`Document::save`, `saveThroughHost`): everything before
  it is the window's as it was (keep-sorted on the tab on show, organize
  and format on save, the LaTeX inputs) and GTK's own two refusals stay
  the window's (a link to nothing, a file not writable), since the host,
  for an app, writes through both. Then, when `hostSavesFile()` (open on
  a host whose Welcome has flag 2, not lost, not detached), the turn's
  step closes (`closeOutside`), an untitled buffer the host held is given
  its path (`tellHostPath`, put back with `setPath` when the write
  fails), `requestSave` with overwrite and the tab's encoding
  (`FolderSearch::encodingName`) goes behind every edit, and `settle(10
  s)` waits: the client's own wait, which takes the host's messages and
  never turns the main loop, so nothing of the window's reenters. Refused
  or unanswered: `save` is false with the host's sentence, so Ctrl+S's
  dialog ("Could not save “x”") and auto-save's status line say it as
  they said GTK's own errors. A host lost meanwhile: the window writes it
  itself, as before stage 7. Written: the encoding from `encodingName`,
  the host's note (the words GTK's own save used) for the status bar, the
  text the file holds (the buffer's when the host wrote the version the
  buffer is at with nothing unconfirmed, else the file's, read), and
  `rememberDisk` with the answer's stamp; then `Editor::saveDocument`'s
  tail as before (`tookSave`: a preview tab kept, the LaTeX previews and
  the language server told, the strip and the title), and the backups'
  planner drops the clean tab's backup as after its own write.
- **A page's save** (`hostWrote`, then `takeHostWrite` at the top of the
  next turn, the latest heard): the text written when the buffer was the
  host's at that version, the path (renamed since: nothing), the file's
  stamp still the one written (else a change outside, for the disk
  checks), the encoding, the text (or the file's, read), `rememberDisk`
  with the host's stamp, and `DocumentHost::documentWrittenByHost`
  (`tookSave`). The file watch's check then finds the file's text the
  saved text: no change, no question, and the next Ctrl+S asks nothing.
- **Dirty from the host** (`hostDirty`, then `takeHostDirty`): heard
  only with nothing unconfirmed, taken only if `edits_` has not moved
  since, never for an untitled buffer (`hostPath_` not the path), a file
  never read or gone, or one loading or not read yet. GTK's own compare
  runs on every edit, so this only matters where the two disagree, which
  the window keeps from happening by telling the host wherever its saved
  text moves without a host write (`tellHostSaved`): its own write (a
  host without flag 2, or one lost), a clean tab following the disk and
  Reload (`applyDiskText`: DocSaved), a change on disk asked about
  (`checkExternalChange`: DocSaved flag 1 with the file's text), a held
  tab read (`attachHeld`: the same).
- **Auto-save** writes through the host with no question
  (`Persistence::autoSaveNow` checks the disk first, as before); quitting
  writes backups, not files, as before.

#### Checks

`host_reattach`'s stage 7 part, on a fresh headless host (its header):
a page's guest over the socket saves a file the window shows with typing
in it, the window's tab and its strip dot clean and in step with the
file's stamp, the host counting it saved; the window types and saves
(Ctrl+S) with no question and the guest hears the host's write with the
file's stamp; a UTF-16 file saved from the window byte for byte with its
mark; an agent's write, then Keep Mine and Ctrl+S, the window's text
written over it; a directory where the file was, which the host refuses
("is not a regular file on disk"), the save dialog showing that sentence
and the tab still unsaved; the host killed, then Ctrl+S written by the
window itself. Not checked: an untitled buffer's Save As refused (its
path put back), which needs the file chooser.
Since every GTK save of a tab goes through the host, in process by default,
the whole GTK run exercises it too (save, backups, format, latin1_files's
note, auto_save). `MUTIBLE_HOST=socket` for the backups suite fails as it
did before this (its SIGKILLed children leave their documents on the
headless host); CI runs no such pass.

### The host watches the disk (stage 7)

Before this, a headless host's page read its tree and Source Control again
only on ↻, on coming back to the page or on a reconnect, and a document on
a headless host never followed its file. Now the host watches the disk:
`src/DiskWatch.{h,cpp}` is the core's one platform interface for the
system's notices, and `SessionHost.h`'s "The disk" says what the host does
with them.

- **Who watches.** `SessionHost::watchDisk(true)`, before any renderer
  (the Welcome says it, flag 4): `mutible host` (`HostOptions::watchDisk`,
  set by `runHost`; the checks choose, default off) and the Mac's own
  in-process host (`SessionLink.mm`'s `Start`, for a window's page), and
  GTK's in-process host (`SessionLink.cpp`'s `startInProcess`, for the
  page View > Session Browser serves; checked in `session_browser`). A
  window with no page served starts no walk: roots come only from a
  renderer's watch or a headless host's folder. A GTK window on a
  headless host has the headless host's watching, as any renderer does.
- **What it watches** (`Host::watchRoots`, handed to the watcher whenever
  `watchGeneration` moves): the headless host's folder and every window
  folder a renderer named in a watch (`readRoot(window)`, so the Mac's
  page watches its window's folder), each with the folders open in some
  tree (`FsRead {"op": "watch", "paths", "hidden"}`, a renderer's list
  replacing its last, at most 4,096, each `cleanRelative`) and every held
  document's folder (so a file opened from build output follows too).
- **The filter** (`DiskWatch::Filter`, pure): not .git's insides but
  .git, .git/refs and .git/refs/heads (an index or HEAD written through
  its lock, a branch moved); not what the ignore files ignore
  (`Ignore::Rules`, entered folder by folder, each folder's files read
  once) nor a node_modules, unless open in a tree (then `OpenOnly`: its
  listing follows, Source Control does not, and what is below it only
  when open too).
- **The systems** (`DiskWatch::Watcher`): inotify watches one folder at a
  time, so the Linux watcher walks the filter's folders on a thread of its
  own (at most `kMaxFolders`, 16,384, as GTK's LspWatcher), adds a watch
  for each folder made (IN_CREATE with IN_ISDIR, reported `below`), drops
  those that go or move, reads the filter again when a .gitignore or
  .ignore changes, and turns IN_Q_OVERFLOW into each root `below`; it asks
  for IN_CLOSE_WRITE, not IN_MODIFY, so a large write is one event. When
  the open folders change it walks only the folders newly open and drops
  only those no longer taken. FSEvents watches a whole tree: one stream a
  root on a serial queue of the watcher's (`FSEventStreamSetDispatchQueue`,
  latency 0.05 s, NoDefer), folder events only, MustScanSubDirs and
  dropped events as `below`, and the host drops what the filter does not
  take. Both hand changes over through a pipe the host's poll(2) waits on
  (`Queued`: 65,536 waiting at most, then each root `below`). Elsewhere
  (g++ on the Mac, Traps) `make()` is null and nothing comes.
- **Bursts** (`DiskWatch::Coalescer`): a batch is due 0.15 s after its
  last change or 1 s after its first, whichever is first; each folder
  once, none under one changed `below`; past 4,096 folders, each root
  `below`. The host's loop folds the batch's wait into its poll timeout.
- **The trees** (`Host::diskChanged`): every renderer that hears the disk
  (Hello flag 32, `Client::hearDisk`) gets FsChanged {window, json: {root,
  listings, git}} with the "list" answer again of each folder it has open
  that changed (read once a batch per root and dotfile rule; at most 256
  a batch), so a folder that went comes as its error and the page closes
  it. The web server keeps each page's open folders (`Conn::watched`,
  from the page's `watch`, which the page sends with every open, close,
  ↻, `hello` and return to the page), tells the host their union
  (`updateWatch`, again when a page goes), and sends each listing as
  `tree` to the pages that have that folder open (`takeDisk`).
- **Source Control**: `git` is true when a counted change was in a folder
  not ignored; the server calls `gitChanged()`, whose read waits out its
  second as before. A folder below the repository's top level sees no
  .git events, as the Mac's window's stream does not.
- **Documents** (`lookAtDisk`, `SessionFiles.h`'s `diskAction` and
  `readDiskText`): a document with a path whose folder changed, and that
  no app's window has open, is looked at: its stamp first (the host's own
  write or read, or the stamp the last look saw, reads nothing), then the
  file (64 MB at most). The file still holds the saved text: its stamp
  taken (`Document::confirmSaved`), gone no longer. Changed and the
  document clean: followed, the one range that differs as the host's own
  step, "Reload from Disk" (by 0, sent to every seat as any edit is,
  undoable), counted saved with the stamp read. Changed under unsaved
  changes, or no longer text: marked changed on disk (`Doc::disk` 1),
  never written over; its save is refused "changed" until a person says to
  overwrite, as before. Deleted: gone (2), unsaved, its text kept; back
  with the saved text, in step; back with other text and nothing typed,
  followed. Renderers that hear the disk get DocDisk {doc, flag 0, 1 or 2}
  when that moves, and the state's documents say `"disk": "changed"` or
  `"gone"`. A host save, an app's DocSaved and a reset put it back to 0.
- **Reload** (DocReload {doc, req}, answered DocReloadDone {outcome, v,
  json {path, ok, reason, why}}; a guest may send it, and only to a host
  whose Welcome has flag 4, `Client::requestReload`): the file's text in
  place of the document's, the same step, undoable (the Mac's Reload by
  choice forgets the history; here a page's mistake is one Undo away).
  Refused for a document an app's window has open ("reload it there"),
  with no file, gone, or not text.
- **The page**: `#disknote` in the doc bar says "Changed on disk" or
  "Deleted on disk" (its title says what a save does), the tab is in
  italics, and an editable page on a host that reloads (`canReload` in the
  hello) shows Reload beside Save for a changed document, sent once the
  page's edits are confirmed (`afterSync`); `reloaded` says why when it
  was refused.
- **On the Mac's own host the windows keep their own watching**: the
  window's FSEvents stream still refreshes its tree, its git colors and
  the page's `gitChanged` (`sessionFolderChangedOnDisk`), and its clean
  tabs follow the disk themselves (`followCleanTabInBackground:`), telling
  the host with DocSaved; the Mac's client does not hear the disk (no
  Hello flag 32), and the host follows no document a window has open. The
  host's stream serves only the page's tree and Source Control. **What
  could move later**: the window's tree refresh (`TreeRefresh::plan`'s
  batches), its git colors' trigger, the page's `gitChanged` hook and a
  clean tab's following could all hear the host's notices instead (one
  stream for both, the filter shared), once the window is a renderer that
  hears the disk; GTK's tree and LspWatcher monitors likewise.

#### Checks

`make test`'s `diskwatch:*` (tests/session_disk_tests.inc): `paths`
(`under`, `relativeTo`, `cleanRelative` over seven bad paths); `coalesce`
(the quiet time and the most, a trailing slash, below folding what is
under it, the cap folding into each root); `filter` (a scratch folder with
.gitignore files at two levels: .git's insides, ignored folders, a
node_modules at two depths, a subfolder's own file, the same opened in a
tree, malformed open folders); `reload-rules` (`diskAction`'s table, and
`readDiskText` over UTF-16, a missing file, a folder, bytes); `host` (a
Host over the encoding with a guest that hears the disk, a reader, an
older page that does not, an editor: the watch's answer and
`watchRoots`, bad paths left out, a file made in an open folder listed to
the trees that have it and to no other, an ignored open folder listed
with no git, .git's insides and node_modules nothing, .git itself git
only, a `below` listing every open folder, a folder gone listed as its
error; a clean document following as "Reload from Disk", and no edit when
nothing changed; unsaved changes marked, DocDisk to those that hear, the
state's `disk`, a save refused; the guest's Reload, Undo taking it back;
deleted, Reload refused, back in step; gone and back with other text
followed; an app's window holding it, the host leaving it, Reload refused
with its sentence, followed once the app lets go; a document's ignored
folder watched but listed to no tree; a renderer going moving the
generation; a host that does not watch; the four kinds through the
encoding); `system` (the real watcher: a node_modules write counting for
nothing, a file's folder, a folder made after the watch began and a file
in it, node_modules once open, no roots); `headless` (`mutible host`'s
HostServer with watching, over its socket: a file made reaching the
renderer as its folder's listing with git, a document following its
file, marked under typing, Reload, deleted); `page` (a page of that host:
its tree following a file made with nobody asking, the page's watch list
replaced, a document changed under the page's typing marked in the state,
Reload with the file's text in the page and the mark gone). Not run here:
the page's look (the note, Reload, the italic tab) needs the owner's eyes,
and no GUI suite checks the Mac's own host watching for its page.

### Quick Open and Find in Folder in the page (stage 7's heavier reads)

Before this the page could only browse its tree. Now <kbd>⌘P</kbd> (Go to
File in the header) is Quick Open and <kbd>⇧⌘F</kbd> (the sidebar's
Search) is Find in Folder, on every page, a reader's too: both only read.
The host runs them on workers of its own (`src/SessionSearch.{h,cpp}`),
the first of stage 7's reads off its thread.

- **The protocol** (additive, no new kind): FsRead ops "files" (Quick
  Open's list), "search" (Find in Folder) and "cancel" (stops one; no
  answer of its own), offered by the host's Welcome flag 8
  (`Client::hostSearches`). A search is answered by several FsData for
  one req, rows in batches, the last with "done" and the counts; "files"
  by one. An older host answers "no read" to these ops (FsRead is not
  new, so nothing drops), but `Client::requestStream` sends nothing to a
  host without flag 8, and the page's hello says `canSearch` false, so
  the page shows neither.
- **The workers** (`ReadWorkers`, owned by `Host`, made at the first such
  read): a thread a read, at most `kMaxReadJobs` (8) on a host (one more
  is answered at once, "too many searches"), answers queued under a lock
  and a pipe written once per batch of them, which `SessionHost::loop`
  polls (`Host::readsFd`) and drains at its top (`Host::takeReads`, which
  sends FsData to renderers still connected). A renderer that goes has
  its reads cancelled (`disconnect`); `~Host` cancels every read and
  joins each thread. A guard thread per read (`Guard` in the .cpp)
  raises the walk's stop flag when the renderer cancels or the time is
  up, and sends a batch that has waited `kBatchMs` (100 ms) though no new
  row came; a batch goes at once past `kBatchRows` (500). The checks pump
  a threadless `Host` with `waitReads`, and slow a search down with
  `ReadWorkers::eachFile` (FolderSearch's `onFileRead`).
- **Files** ("files", `hidden`): `CommandPalette::listFiles`, Quick Open's
  own walk (Find in Folder's skips, the ignore files, breadth first, at
  most `kMaxListedFiles`, 20,000), with two new arguments the Mac leaves
  off: `dotfiles` false unless the tree shows them, and `confine`, which
  neither lists nor enters a symlink whose real place is outside the
  folder (the Mac's Quick Open follows one, as before). 15 s at most.
- **Search** ("search", query and the three toggles, `include`,
  `exclude`, `hidden`): `FolderSearch::search` with the Mac's options
  (`kMaxSearchRows` 20,000, `contextLines` 1, repeats dropped) and two
  new ones, `skipDotfiles` and `confine` (a link out is neither read nor
  entered: `insideRoot`, the real path under the root's), and
  `onRows`, called on the merging thread with each file's rows as they
  are taken (repeated context dropped between them), which is what lets
  the rows go before the search ends. Each row is `searchRowJson`: path,
  line, column (code points), the display text, `hl` the matches in that
  text in UTF-16 units (TextFind::findAllInLine over the display text, as
  the Mac's row draws them), before, after, count. The last answer says
  filesSearched, filesMatched, totalMatches, rowCount, truncated (the row
  cap), tooSlow (the regex fallback's 2 s on a line, as the app),
  timedOut (`kSearchSeconds`, 30 s, the host's own limit: it serves
  others), cancelled, error (a regex that does not compile), skipped.
- **The server** (`SessionWeb.cpp`): `files {seq, query, recent, fresh}`
  ranks the host's listing, kept on the session (`Session::files`, with
  UTF-16 copies), by `quickOpenJson` (`CommandPalette::rankFiles`, the
  app's palette matching, `recent` first among equals, 100 rows with
  `pos`, `kind`, `view`); with no listing, or `fresh` (the palette
  opening) a second after the last, it asks the host for one and answers
  the pages waiting when it comes. Ranking is on the server's thread:
  `rankFiles` now passes over a path that does not hold the query's
  letters in order before any alignment, so a typed name costs a few ms
  over 20,000 paths (`search:files` bounds it, and the worst case, a
  query every path holds). `search {seq, ...}` cancels that page's last
  search (`cancelSearch`) and starts the new one (`requestStream`); its
  answers go to the page as `found {seq, rows, done, ...}`
  (`takeStreams`, after each dispatch); a query too short to search
  answers `short` at once. A page that goes has its search cancelled; the
  session detaching drops them all (the host cancels the renderer's reads
  as it goes). `CommandPalette::symbolRows` moved to
  `CommandPaletteSymbols.cpp` so `mutible-cli` links `CommandPalette.cpp`
  without the language servers' names.
- **The page**: Quick Open is `#qo`, a field over the page with its list
  (the name, then the folder, matched letters bold), asking at each input
  (`QO.seq`, older answers dropped), the arrows, Return, a click, Escape
  and a click outside closing it; the pick goes through `openFile` with
  the row's `kind` and `view`, so a picture opens in the page's viewer
  and a text file as the tree's click (the window's tab on an editable
  page, a tab of the page's own on a reader's), kept. Find in Folder is
  the sidebar's third mode (`setSideMode("search")`, `#search`): the field
  with Aa, ab and .* inside, searched a quarter second after the last key
  or at once on Return or a toggle; `found` rows are appended as they
  come, a sticky file header with its count per file, each row its
  context lines muted and its hit line with `<mark>`s; the summary counts
  as they come and says why it stopped. A row's click is `openAt(path,
  line, col)`: `openFile(..., {kind: "text"})`, then, once that file is
  on show (`takeGoLine` from `onDoc`, `showLocal` and `takePending`, or
  at once when it already is), its line in code mode, the caret at the
  match's column, the line marked `.hit` and scrolled a third down. ↻
  searches again; a reconnect asks again for both. The document keydown
  takes <kbd>⌘P</kbd> and <kbd>⇧⌘F</kbd> (Control elsewhere, but not in the
  terminal off a Mac, where they are the shell's). `mcSessionView`
  gained `quickOpen()`, `search()`, `hitLine()` and `caretLine()`.
- **Not in this slice**: Replace (Replace in Folder stays the app's; the
  page has no replace field), files to include and exclude on the page
  (the protocol takes them), the window going to the line too (an
  editable page's click opens the window's tab as the tree's does; the
  line is the page's). GTK's in-process host is a `SessionHost` too, so
  its page has both, but no GTK check drives them there.

#### Checks

`search:files` (Quick Open's list over a scratch folder: an ignored log,
a build folder, node_modules, dotfiles with and without `hidden`, a link
inside followed once, a folder link and a file link out of the folder
left out; the Mac's own list still following the link out;
`quickOpenJson`'s ranking, positions, kinds, views, recent first and
`more`; 20,000 paths ranked within a bound for a typed name and for the
worst case); `search:rows` (the rows, the context with repeats dropped,
`hl` in UTF-16 past an é and an emoji, the column, the counts, nothing
outside or ignored, dotfiles with `hidden`, case, whole word, a regex and
its marks, a regex that does not compile, a query too short, include, an
unknown op); `search:batches` (300 rows from twelve slowed files in more
than one batch, in the walk's order); `search:caps` (20,050 matching
lines cut at 20,000, truncated; a slowed search stopped by its time
limit, timedOut and not cancelled, its rows kept); `search:cancel` (a
cancel mid-walk: one last answer, cancelled, fewer files searched);
`search:host` (a Host over the encoding: Welcome flag 8, a guest's list
and a reader's search through `requestStream` and `takeStream`, a
cancelled search dropped by the client and forgotten at its last
answer, a renderer that goes having its read stopped, eight at once and
the ninth refused, no folder for the window named, a client of a host
without flag 8 sending nothing, FsData through the encoding);
`sessionweb:search` (a headless host and a read-only page over the wire:
`canSearch`, the listing ranked, a query's one row, a search's rows with
context and marks, a new search replacing the last, a short query, a
cancel, nothing outside or ignored). GUI: `session_browser`, on the
editable page, <kbd>⌘P</kbd> lists the window's folder as the host listed
it, "clc" leaves calc.py with its letters marked, Return opens it in the
window and the page shows it; on the read-only page, <kbd>⇧⌘F</kbd> shows
Search, "return" lists two files grouped with their counts, the second's
row its context line and hit line with the match marked, and a click
opens it in a tab of the page's own at that line (the caret and the
mark on it), the window opening nothing. Not run here (no GUI on this
machine for the builder): the GUI checks above; the look of Quick Open
and the search results, real keys in Safari and Chrome, and a phone need
the owner's eyes.

### New File, Save As and Replace in Folder from the page (stage 7's page-side writes)

Before this a page could save a document under its own name only; an
untitled tab's save opened the window's save panel on the Mac's screen,
and Replace in Folder was the app's. Now an editable page makes files,
names them, and replaces across the folder. A reader sees none of it.

- **The protocol** (additive, gated by the host's Welcome flag 16,
  `Client::hostWritesForPages`; a client sends none of these to a host
  without it): DocOpen flag 2 from a guest makes an untitled document
  (`Client::openUntitled`: no path, no text, the guest's seat a view as its
  others); DocSave's json `{"saveAs": rel, "hidden"}` from a guest names
  the file anew (`Client::requestSaveAs`, answered as a save, DocSaveDone);
  FsRead's op "replace" (`Client::requestReplace`, sent behind the
  renderer's edits through the outbox, its one answer a stream's) replaces
  on the rows a search showed (SessionSearch.h has its shape). An older
  host would have taken the saveAs json for a plain save, which is why
  the flag gates it rather than the host ignoring what it does not know.
- **The path** (`SessionFiles.h`'s `newFilePath`, pure but for stat and
  realpath, both the host's and the server's): `rel` from the folder, its
  names as a read's (no absolute path, ".", "..", empty name, NUL, a name
  over 255), no dotfile unless `hidden`, the folder it goes in there
  already, a folder, and inside the session's by its real place (a page
  makes no folders), and what is at the path a regular file inside the
  folder by its real place (`exists`), else refused: a link out
  (`outside`), a link to nothing, a folder, a FIFO (`notfile`).
- **The host's Save As** (`Host::onSaveAs`, a guest only; a DocSave whose
  saveAs names the document's own file is a plain save): refused for a
  document an app's window has open ("use Save As there": the window
  names its own files), for a path another document has
  (`docAtPath`, by spelling or real place: never two documents of one
  path), and for a file there already unless flag 1 (`exists`, "x
  already exists: saving would replace it."). Then `saveDocument` with
  the new path, the folder as root, the document's own encoding kept and
  overwrite (the questions are asked), and on success the document's path
  is the new one (`watchGen_` moves, `realDir` cleared), counted saved
  with the stamp written, its disk state in step, every hearer told the
  write (`tellWritten` with the new path). The old file is left as it was.
- **New documents** (`Host::onOpen`, guest and flag 2): `Doc::fromPage`;
  `dropEmptyPageDocs` lets one go when no renderer has a seat on it and it
  is still empty and pathless (a view's close, a renderer's disconnect),
  so a page's New File it never typed into leaves nothing. With text it is
  an untitled buffer as any other: `busyReason` keeps the host for it and
  the host's backups keep it (`Backups::untitledPath`).
- **Replace** (`Host::onReplace`): refused for a renderer that only reads
  (FsRead itself passes `refusedFromViewer`, so the op checks), for no
  folder, for a query too short, and up front when the workers are full
  (so nothing is half done). On the host's thread, each file a document
  holds (`docAtPath`) and that is clean is checked and replaced by
  `replaceRows` on the document's text and goes in as one step of the
  host's ("Replace in Folder", `hostStep`, shared with Reload from Disk),
  every seat told, the document left unsaved (as the Mac's Replace in
  Folder edits a clean tab and leaves the saving to the person); one with
  unsaved changes is skipped (`unsaved`), as the Mac leaves a dirty tab.
  The other files go to a worker (`runReplace`): confined again, read
  with `readFileStable` (64 MB at most, `kMaxReplaceBytes`), decoded with
  `decodeFileText` (UTF-8, UTF-16 with its mark, Windows-1252, stray
  bytes), checked and replaced by `replaceRows`, refused when the
  encoding cannot store the result (`canEncode`: never written as UTF-8
  behind the person's back, unlike a save) or when the file's stamp moved
  while it was read, and written with `writeFileAtomically` in its own
  encoding and mark. The answer counts what was replaced, written and
  edited, and names each file left alone with its sentence.
- **Changed since the search** (`replaceRows`): every row's line must
  still read as the row showed it (`displayText`), and the text must hold
  as many matches as the rows counted (a match added on a line no row
  shows is a change too), else the file is left alone whole
  (`changed`). A row the page took out ("on" false) keeps every match on
  its line (`replaceInText`'s skip lines). The page cannot take out one
  match of several on a line: a row is a line, as Dismiss is in the
  app's Find in Folder (#210).
- **The server** (`SessionWeb.cpp`): the hello's `canNew` (an app's
  `Options::newFile`, or a headless view with no `save`, no window and a
  host that writes for pages), `canSaveAs` (an app's `Options::saveAs`, or
  no `save` and such a host; GTK's page has both (HANDOFF 139),
  `SessionBrowser::Host::newFile` and `saveAs`, see "The GTK app" below), `canReplace` (editable, such a host, and one that searches).
  `newfile` calls `Options::newFile` (the Mac: `openUntitledSessionTab`,
  the window's untitled tab, Untitled-1, -2, ... as a restored buffer is
  named, which the page then follows as the window's tab on show) or has
  the host make one (`openUntitled`, a `DocView` made for it at once) and
  tells the page `newfile {doc}` to show it. `saveas {doc, path,
  overwrite}`: for an app, `newFilePath` against `Options::folder`,
  another document of the state at that path, and `exists` are answered
  at once, then `Options::saveAs(doc, abs, ask)` after a settle, and the
  app answers `Server::savedAs(ask, ok, why)` from any thread (queued
  under `saveAsMu_`, taken at the loop's top); else `requestSaveAs`,
  answered in `takeOutcomes`. Both reach the page as `savedas {doc, ok,
  path, reason, why}`. `replace {seq, query, toggles, replacement, files}`
  is checked (rows as `replaceRowsFrom` takes them, at most
  `kMaxReplaceRows` in all) and sent; its answer comes as `replaced` with
  the page's seq (`takeReplaces`). A `DocView` whose state path changed
  (Save As) is colored again by its new name and sends every line as a
  version of no text change (`DocView::setPath`), so a page with typing in
  flight keeps it.
- **The Mac** (`EditorController+SessionBrowser.mm`, `MCSessionBrowser
  newFile` and `saveAs`): Save As from the page moves the tab to the new
  path (`currentPath`, which sends DocPath) and writes through the host
  with no question (`writeText:toTab:interactive:failure:`, the failure
  going back to the page rather than an alert); a failure puts the old
  path back. A tab already on that file is refused ("close it there
  first"). Then the language servers, the colors (by the new extension),
  the strip, the session and the title follow, as for a file moved. The
  answer goes through `answerSaveAs:`, which drops it for a server
  stopped meanwhile. Unlike the Mac's own Save As (a new tab on the new
  file, the old tab closed) the tab and its document stay, so every page
  keeps showing it.
- **The GTK app** (`linux/src/SessionBrowser.cpp`, `main.cpp`'s
  `newSessionFile` and `saveSessionDocumentAs`): the same two options,
  each hopped from the server's thread to the main loop (`later`), and
  `Server::savedAs` called there. New File is `Editor::addUntitled`, an
  untitled tab named by `Backups::untitledPath` with a new document on the
  host and no path, put on show. Save As is `Editor::saveDocumentAs`:
  `Document::setPath(path, false)` names the tab without telling the host,
  then the window's own `saveDocument` writes it, through the host when the
  window's host is in process (`saveThroughHost`, which gives the host the
  path first and puts it back if the write fails), and the document's path
  on the host follows (DocPath). A failure puts the old name back. A tab
  already on that file is refused as on the Mac.
- **The page**: New File in the header (`#newbtn`) and <kbd>⌘N</kbd>
  where the browser gives it to the page (most keep it); Save As… in the
  doc bar (`#saveasbtn`) and <kbd>⇧⌘S</kbd>; Save on a document the state
  lists with no path opens Save As instead. The dialog (`#sa`) starts on
  the document's path from the folder (an untitled tab's name, from the
  window's tab), the name without its extension selected; under the field
  the typed folder's entries that start as the typed name does, from the
  tree's own listings (`T.kids`, a folder the page has none of listed once
  as the dialog asks), arrows and Tab or a click to complete; Return or
  Save sends `saveas` once the page's edits are confirmed (`afterSync`);
  `exists` asks with `confirm()` ("…\n\nSave anyway?", #293's words) and
  sends again with overwrite; any other refusal is the dialog's note. Find
  in Folder on an editable page with `canReplace` has a replace field and
  Replace All under the query (`#rbox`); each row's hit line and each
  file's header have a box (`.rpick`), a row taken out dimmed; while the
  field holds text each match is struck through with its replacement after
  it (`ins`, the text as typed, or for a regex the browser's own replace of
  the match, JavaScript's engine as TextFind's). Replace All is enabled
  only for a search that ended whole (not cut short, timed out, too slow,
  cancelled, or an error) with a row left in; it asks with `confirm()`
  naming the matches and files and what replaces them, and sends once the
  page's edits are confirmed. Return in the replace field searches again
  (#210: never a write on Return). `replaced` is a toast of the counts and
  each file's sentence, then the search runs again. `mcSessionView.writes()`
  says what the GUI checks read.

#### Checks

`host:new-path` (`newFilePath` over a scratch folder: a name, a
subfolder, a file there, a link inside; fifteen refusals each for its
reason, a NUL, a long name, a dotfile with `hidden`, no folder);
`search:replace-rows` (all rows, one taken out, all taken out, three
changes since the search, a regex with a group, CRLF kept, rows from a
page checked and capped, `replaceHidesPath`); `host:save-as` (a Host over
the encoding with an editor, a guest that hears DocDirty and a reader: a
guest's Save As written into a subfolder, the path the state's, clean, the
old file as it was, the next save by the stamp; `exists` then overwrite;
`name`, `nofolder`, `dotfile`, a dotfile with `hidden`, its own file a
save; another document at the path `open`; an app holding it `app`; New
File's document, a plain save `nofile`, typing, Save As naming it; an
empty one dropped when the guest goes and one with text kept; a reader's
DocOpen flag 2 a view of nothing, its Save As refused; the two through the
encoding); `host:replace` (an app with a clean and a dirty document, a
guest, a reader: rows from a real search, a file changed after it, a row
taken out, a crafted dotfile row; the open document edited as one step
named Replace in Folder and its mirror with it, the file on disk as it
was; a closed file written but for the row out; UTF-16 written back with
its mark, Windows-1252 kept; dirty, changed and dotfile named; Undo on the
host; a replacement Windows-1252 cannot store left alone; a reader and a
short query refused); `sessionweb:writes` (a headless host's editable and
read-only pages over the wire: the three flags; New File's document shown,
typed into, saved as hello.txt, the state's path following; `exists`, then
overwrite; `../` refused; a search, then a replace with a row out written;
the read-only page with none of them and its replace refused);
`sessionweb:writes-app` (an in-process host and an app's page with
`Options::saveAs` and `newFile`: `exists` answered before the window hears,
the window handed the path and its refusal reaching the page, a second
and an unknown answer dropped, `../` never reaching the window; a page
without them having neither). GUI: `session_browser` on the editable page:
New File makes the window's untitled tab, which the page shows and types
into; <kbd>⇧⌘S</kbd> opens on its name, the folder's files complete
`pkg/`, a file there already asks and Cancel writes nothing, made.py is
written through the host and the window's tab is that file, the page's tab
named so; Find in Folder for "made" with a file the window has not open,
the previews and boxes, Return in the replace field searching again and
writing nothing, Replace All with a row out (asked first) editing the
window's tab as one step and writing the other file, Undo in the window
taking it back. Not run here (no GUI on this machine for the builder):
the GUI checks above; the dialog's and the boxes' look, real keys in
Safari and Chrome (which keep <kbd>⌘N</kbd>), and a phone need the
owner's eyes.

### The Mac over SSH (stage 8)

`mutible ssh://thinkpad/~/proj` opens a Mac window whose files,
terminals, saving (and, later, git, search and language servers) live on
the remote machine, which runs a headless mutible host; the Mac only
draws and drives. Since stage 7 a window's documents and terminals go
through its host, so this is mostly a transport.

#### The design

- **Starting the remote host.** The Mac runs the user's own ssh, so
  ~/.ssh/config (aliases, ProxyJump, ControlMaster), keys and the agent
  all apply: `ssh -T -o BatchMode=yes -o ConnectTimeout=20 -o
  ServerAliveInterval=15 -o ServerAliveCountMax=3 -e none [-p PORT] [-l
  USER] -- HOST 'mutible host --stdio <path>'` (`Shared::sshCommand`; the
  path single-quoted for the remote shell, which ssh hands the command
  to, and `~` expanded by the remote mutible, not the shell). No pty, and
  BatchMode, because nobody sees the pty ssh would ask on: a password, a
  key's passphrase or an unknown host key fails at once instead of
  hanging, and the window's alert says to run `ssh HOST` once in a
  terminal (and `ssh-add` for a passphrase), then open the address again
  (`Shared::explainSshFailure`, from ssh's exit status and its stderr).
  `$MUTIBLE_SSH` names another program (the GUI check's stand-in).
- **What the remote needs.** mutible installed (the PPA's `mutible`, whose
  binary hands `host` to the same `mutibleCliMain`; or a build; a Mac's
  launcher does the same) and on the PATH a non-interactive ssh command
  gets (~/.profile is not read for it). The handshake is the version
  check: the host's Welcome must offer flags 1, 2 (it saves for apps) and
  32 (it runs its own shell, TermSpec's hostShell, new here) and name its
  folder. ssh's 127 or "command not found" says mutible is not installed
  there; "unknown option --stdio" (or "unknown command host"), or a
  Welcome without those flags, says it is too old there.
- **What runs where.** The remote: the documents' text and undo (the
  host), every save (the host's write, stage 7), terminals (the remote
  user's login shell in the remote's environment), and, as stage 9 moves
  them into the host, git, language servers, formatters and Claude Code.
  The Mac: drawing, keys, its own settings and theme (colors, fonts, the
  terminal's palette), the undo menu's asking, and its backups of what it
  shows.
- **A dropped connection.** The window keeps every tab's text and stays
  editable; the title says "(disconnected)", the status bar says why and
  shows Reconnect. Reconnect runs ssh again and reattaches through the
  stage 6 path (`moveToSessionLink:`): each document opened on the new
  host from the window's text, the file's text as the host's saved text,
  so unsaved text is never lost (undo starts again), and each terminal a
  new shell. A save while disconnected says to reconnect; the window
  never writes the Mac's file at that path.
- **Left out of this stage.** Port forwarding for the browser pane (a
  dev server on the remote's localhost), dragging a file from Finder to
  the remote, the Mac's Quick Look (and its image, PDF and media viewers)
  on remote files.

#### As built (2026-11-21)

- **`mutible host --stdio PATH`** (`HostServer::startStdio`, `runHostStdio`
  in tools/mutible-cli.cpp): the headless host serving one renderer on its
  stdin and stdout, no socket and no lock (another host of the folder may
  run beside it), ending as soon as its input closes (`run()` returns
  true when the one connection goes). PATH is a folder, or a file whose
  folder it serves; the Welcome then names the file (`json {"file"}`,
  `Client::welcomeJson`), so the window shows it. The CLI moves fd 1 off
  the protocol's pipe (stdout becomes stderr, stdin /dev/null) before any
  thread starts, so nothing a library or child prints can corrupt the
  stream; the protocol goes on the dups. It passes its own environment as
  the shells' (`HostOptions::shellEnv`). No disk watching and no backups
  of its own yet: the one renderer is the window, which keeps them.
- **The host's own shell** (TermSpec's `hostShell`, Welcome flag 32): the
  host runs its login shell (`SHELL` in its environment, else the
  password database, else /bin/sh) as a login shell, in its environment
  with the renderer's few variables laid over it (`ownShellSpec`,
  HostPty.h). The Mac's remote terminals send only TERM, COLORTERM and
  TERM_PROGRAM, no nonce and no ZDOTDIR: the integration is a file of the
  Mac's, so a remote shell has no marks (TerminalView's `_remote`, which
  also keeps local pid lookups away: `proc_name`, background jobs, the
  Claude check and `shellPid` for the Processes panel, since a remote pid
  names some other process here).
- **The transport** (`HostLink::spawn`, SessionServer.h): a command
  started with `posix_spawn` in a session of its own (no other descriptor
  of the app's, signals at their defaults), spoken to over its stdin and
  stdout, its stderr kept (8 KB) and its exit status reaped by the reader
  before the client hears the host is gone. Writes never raise SIGPIPE
  (F_SETNOSIGPIPE on the Mac). `close()` closes the command's input (the
  remote host ends, then ssh), then hangs up and kills its group if it
  stays.
- **The address** (`Shared::parseSshUrl`): ssh://[user@]host[:port][/path],
  `/~` and `/~/x` the remote home's, no path the home, %XX decoded; a host
  or user starting with '-' (an option to ssh), a host with what no host
  name holds, a bad port or a line break in the path is refused. The
  launcher sends `ssh://` arguments as a request file of mode "ssh" (as
  "web"); `application:openURLs:` takes those and ssh:// URLs and calls
  `openRemoteAddress:` (main.mm).
- **The Mac's link** (`MCSession::forRemote`, Link::Kind::Remote): one per
  machine and path as addressed, its client attached through the same
  `attachClient` as a folder's socket link, waiting at most 30 s for the
  Welcome on the main thread. `key()` (the machine's ssh:// and the
  folder) is what Reconnect moves windows by, so a remote folder spelled
  as a local one is never confused with it.
- **The window** is an ordinary EditorController on the remote folder's
  path, put on the link before `showWindow` (`useRemoteSessionLink:`, an
  MCSessionWindow `initWithLink:folder:`), so the stage 6 window code is
  shared, not forked. Every tab of it has a document on the remote host
  whatever editor.session-host says (MCTabOwner's `tabNeedsSession:`).
  Its files are read through the host (`Link::readText`: FsRead "read" in
  4 MB pieces, 64 MB at most, decoded as `MCReadDiskText` decodes) in
  `loadActiveTab`, and saved by it (stage 7's `writeTextThroughHost:`).
  Nothing of its folder is looked for on the Mac: no stamps
  (`recordStamp:ofTab:` keeps none, so `checkExternalChange` and the
  questions before a save pass it by), no FSEvents stream, no restored
  session, no local language server, no viewer that reads the disk, the
  auto-save loop's disk check skipped, and the window's own write refused
  (`writeText:toTab:`). Its title is "file — host:path — mutible"
  (WindowTitle's `remote`, with " (disconnected)" while the link is lost).
  Closing it detaches as stage 6 does and then lets the link go
  (`Link::detach`), which ends the remote host.

#### Checks

- `make test`: `hostserver:stdio` (a host on two pipes: its Welcome's
  flags, folder and file; a document typed into and saved by the host; a
  hostShell terminal with the host's environment under the renderer's and
  the host as its parent; the input closing ends the host and its shell),
  `hostserver:spawn` (a command's pipes as a link, its exit status and
  stderr; one that outlives its input hung up by close; a program not
  there), `hostserver:ssh-url`, `hostserver:ssh-command` (the argv, the
  quoting, each failure's sentence), `hostserver:own-shell`,
  `hostserver:command-line` (`--stdio`), `window-title:remote`.
- `make guitest SUITES=remote_ssh` (RemoteSshSuite.mm, its header): the
  real path through `application:openURLs:` with a stand-in for ssh that
  runs this checkout's mutible-cli as `mutible`: the window, its title,
  ssh's arguments, the file's tab, a save written by the host, a terminal
  whose parent is the host and whose environment is the stand-in's, the
  connection killed (disconnected, the text kept and typed into),
  Reconnect (a new host with everything typed, saved there, a new shell),
  and the host ending with the window. Not run here (the builder had no
  screen to use): CI runs it.

#### Traps

- **A stand-in that serves the Mac's own disk hides local reads.** The
  GUI check's "remote" folder is a scratch folder of this Mac's, so any
  code that reads or stamps the tab's path locally passes there and fails
  on a real remote. Every local look in a remote window goes through
  `sessionIsRemote`; a new one must too.
- **A remote pid is not a process here.** `proc_name`, `KERN_PROC_*` and
  `proc_pidpath` of a remote terminal's pids name whatever local process
  has that number; TerminalView's `_remote` guards each.
- **`exec /bin/sh -c` in a stand-in may fork rather than exec**, leaving
  the host a grandchild; kill the stand-in's process group (it is a
  session leader: `HostLink::spawn`), never its pid alone.
- **`MCGuiAlertsShown()` is the whole run's**, every suite's before this
  one's too: "no question was asked" counts from the suite's start (#314's
  first CI run failed on fourteen alerts of earlier suites, and passed
  alone on retry).
- **A wait counted in naps is not a wait in time.** `HostLink`'s waits
  (`reap`, `close`'s grace before the hang-up) use steady_clock deadlines;
  and no core check bounds `close()` by the wall clock (#314's first CI
  run): `hostserver:spawn` checks it hung the command up instead.
- **A line that is not a message ends `--stdio` silently** (the connection
  is dropped, as any peer's is), so a hand test must send real kinds
  (`session.hello`, not `Hello`).

### Still to do

- **Stage 8, after this first slice** (in order of value): a lasting host
  behind the stdio end (`--stdio` bridging to the folder's socket host on
  the remote, so terminals, undo and unsaved text survive a drop there,
  and the window reconnects by itself); the window's tree, Quick Open and
  Find in Folder through the host's reads (FsRead "list", "files",
  "search"), and its disk watching (Hello flag 32: FsChanged, DocDisk);
  connecting off the main thread with a progress line; the shell
  integration's marks on the remote (the remote mutible's own integration
  file, a nonce from the Mac); viewers for remote pictures, PDFs and media
  through FsRead "read"; then stage 9 (git, servers, formatters, agents)
  and the parts left out above (ports for the browser pane, Finder drags,
  Quick Look).
- **Stage 7, after the watching, the Mac's and GTK's saves, Quick Open and
  Find in Folder, and the page's New File, Save As and Replace** (in order
  of value): the windows hearing the host's watching instead of their own
  streams ("The host watches the disk", what could move); GTK's page
  without New File and Save As (its `SessionBrowser.cpp` would give
  `Options::newFile` and `saveAs` as the Mac's do; Replace already works
  there, on the host); the page closing a headless host's documents (a
  page's untitled document with text stays until it is saved or the host
  ends); a page's Replace taking out one match of several on a line; the
  listings FsChanged carries, a file's read and the write (a save's, a
  replace's open documents) on a worker rather than the host's thread.
- Lines are re-sent whole per edit; a long line typed into from a browser
  wants character splices.
- No snapshot or patch messages exist yet: the view polls the host's state
  every half second for tabs and documents (terminal sizes come with
  `TermShell`).
- Several carets, folding, find, completion and the rest of the editor
  are the Mac's and GTK's, not the page's; so is reordering tabs, which
  waits for the host to decide tabs (stage 6's note). Closing goes to the
  window (R7), not the host.
- Mac terminals opened before the setting is on are the app's own.
- Typing the session had not confirmed when the connection drops is lost
  (the page says so); it is not resent on reconnecting.
- A headless host's page cannot open text files (no window
  decides tabs); it shows pictures, PDFs, media, HTML, tables, notebooks
  and databases (R3), and a LaTeX document's text, saying the window's
  own page shows its pages (it typesets nothing). Source Control, the
  review of uncommitted changes (R3's fourth slice), and the window's
  open commit and worktree pages with every review's pictures (its last
  slice) are in; the page does not follow the window to a review page.
