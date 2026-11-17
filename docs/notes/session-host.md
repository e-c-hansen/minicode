# The session's host on the Mac (editor.session-host)

SESSION-DESIGN.md (in `.grades/`) is the design; its "Stage 4a as built"
and "Stage 4b as built" say what was built. GTK's side is
`linux/HANDOFF.md` item 87. This file is the Mac's.

## What it is

With `editor.session-host = on` (off by default for one release), or
`MINICODE_SESSION_HOST=on` in the environment (the GUI checks' second
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
- **Saving** tells the host (`MCSessionDoc saved`) when the text written is
  the tab's; dirty, the disk, backups and auto-save stay the Mac's (stage
  7 moves them).

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
  (`MINICODE_SESSION_HOST=on`, `MINICODE_HOST=loopback`,
  `MINICODE_HOST_DELAY_US=200`), a step of its own after the halves ("GUI
  checks with the session's host"): keystroke, multiedit, editor_keys,
  auto_close, save, backups, lsp_sync, snippets, format, git_conflicts and
  test_gutter.
- `make guitest`: at every suite's end the harness settles with the host
  and fails the suite (`FAIL [suite] the session's host and the window it
  renders differ`) unless every tab with a document has the host's text,
  nothing of the host's waits to go in, and the host reported nothing
  wrong (`CheckSessionMirrors`, `MCSessionCheckTabs`). With the setting off
  no tab has a document and the check does nothing.
- `MINICODE_HOST=loopback` puts every message through the protocol's
  encoding and back; `MINICODE_HOST_DELAY_US` delays each one on the host's
  thread. Errors the host reports go to the log (`os_log`, subsystem
  com.minicode.editor, category session).

## Traps

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

## The headless host (stage 6, core and CLI)

`minicode host --folder DIR` runs the session's host for one folder with no
window, and `minicode attach --folder DIR --dump` is the first renderer that
reaches it over a socket. Both are in `minicode-cli` (the Mac's
`Contents/MacOS/minicode-cli`, and the GTK binary, which hands the control
channel's subcommands to the same `minicodeCliMain`), built from the core
alone: `src/SessionServer.{h,cpp}` over `SessionHost`. Neither app attaches
yet; GTK is next, after the Linux session's GTK terminals (#128).

- **The socket.** `host-<FNV-1a of the resolved folder, 16 hex>.sock` in the
  first of `ControlChannel::socketFolders()` (ControlPaths.h, now shared by
  the CLI: `$MINICODE_CONTROL_DIR`, else `$XDG_RUNTIME_DIR/minicode` or
  `/tmp/minicode-<uid>` on Linux, `$TMPDIR/MiniCode` and the user's own
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
  SessionProtocol (terminal bytes base64), the same as `MINICODE_HOST=loopback`.
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
- **Signals.** `minicode host` blocks SIGINT, SIGTERM and SIGHUP before any
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

### For the GTK and Mac attach steps

- A renderer needs ids that do not clash with the host's: today each app
  numbers its documents, windows and terminals from 1, which is fine for
  one renderer in process but not for two on one host. Either the host
  hands out ids (`doc.open` without one), or each renderer takes a range
  from its Welcome.
- Tabs are still reported, not decided (4a's note): a second renderer reads
  `windows` from the state but cannot change them through the host until
  `Model::Window`'s callbacks are data.
- The renderer that last typed into a terminal should set its size (the
  design's tmux `window-size latest`); today any `TermResize` wins.
- Saving, dirty, backups and the disk are still the app's (stage 7): a
  headless host's unsaved document is only as safe as the host process.
