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

- **A tree listing runs on the host's thread**, between typing messages;
  hence `kMaxListed`. Stage 7's bigger reads belong on a worker.
- **Confine with `confinedPath`, never by spelling.** A path from a page
  is held inside the folder by its `realpath` (symlinks followed) and
  must not hold "..", ".", empty names or a leading '/'; the Mac's `open`
  trusts that check.

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
  integration's ZDOTDIR and nonce, `BROWSER`, `MINICODE_TERMINAL_ID`,
  `MINICODE_PID`, the venv and IDE link variables) and sends it as a
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
  off, so the terminal stayed "running cat". `updateWindowSize` is
  `resizeTerminal` (no pixel size: `TermResize` has none).
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
  window (`SessionBrowser`) shows all of the app's host terminals, since
  the Mac does not report which window holds one. Notes the panel writes
  on the screen itself ("[shell exited — restarting]") are only on the
  Mac's screen. Terminal size follows the Mac (any `TermResize` wins).

Checks: `make guitest SUITES=terminal_host` (the setting turned on by the
suite: the shell is the host's; a command typed on the Mac is on the
host's screen and on a second renderer's, attached through
`MCSessionConnectViewer` with its replay; a command that renderer types
runs in the Mac's terminal and the marks see it end; the terminal goes on
after it leaves). `terminal_pty` checks the host's teardown with
`MCSessionInspectTerminal` (its master, write buffer and reaping) when
the host owns the pty. CI's pass with the host runs terminal_pty,
terminal_marks, terminal_paste, terminal_exec, terminal_keys, run_file,
attention and terminal_host. `make test`'s `term:host` checks the modes
the host reports (`echoOff`, `raw`, `signalsOff`).

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
- The renderer that last typed into a terminal should set its size (the
  design's tmux `window-size latest`); today any `TermResize` wins.
- Saving, dirty, backups and the disk are still the app's (stage 7): a
  headless host's unsaved document is only as safe as the host process.

## The browser's view (stage 6b)

`minicode host --folder DIR --web PORT`, or `minicode attach --folder DIR
--web PORT` for a host already running, serves a read-only view of the
session to a browser: `src/SessionWeb.{h,cpp}` (the server, the request
rules, the views as data) and `src/SessionWebPage.cpp` (the one page, its
CSS and script, no third-party code). The user's side is REFERENCE.md's
"Remote sessions (early)".

- **Access.** A socket on 127.0.0.1 (never `INADDR_ANY`), and a token of 48
  hex digits from /dev/urandom made at start, printed once in the address
  (`http://127.0.0.1:PORT/?t=TOKEN`). `Web::answer` refuses, in this order:
  anything but GET (405), a Host that is not 127.0.0.1, localhost or [::1]
  with any port (403; two Host or two Origin headers make the request bad),
  an Origin that is not the page's own (`sameOrigin`: http:// and the
  Host the request came under, so a page on another port or under the
  other loopback name is refused too) and a socket upgrade with no Origin
  at all (403; since 8b, when pages can edit), no token or a wrong
  one (403, compared with `IdeLink::sameSecret`), then serves `/` (the page)
  or `/ws` (the WebSocket upgrade, IdeLink's accept key and frames), else
  404. The token is in the query of both because a browser's WebSocket
  cannot send headers; the page has `Referrer-Policy: no-referrer` and
  fetches nothing else. The page's Content-Security-Policy allows one
  script by a nonce made per response (`{{NONCE}}` in the page), inline
  styles, data: pictures, and WebSockets to 127.0.0.1 and localhost.
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
  the screen over; a size change seen in the host's state (looked at every
  `pollMs`, 500) attaches again for a fresh replay at the new size.
- **Colors** (stage 8b's first part): `Web::paletteJson(Settings)` gives
  the page every color the editor resolves (the theme, then the file's
  own): the syntax styles, each surface's background and text by its
  settings name, the gutter, line numbers, ruler columns and color,
  bracket colors, the Markdown preview's five colors and code background
  and `markdown.max-width`, `editor.word-wrap`, and `editor.font-size`,
  `terminal.font-size` and `tabs.font-size` (points as CSS pixels). The
  server reads the settings file (`Options::settingsPath`, else
  `$MINICODE_SETTINGS`, else `~/.config/minicode/settings.conf`) at start
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

### The Mac's own host in a browser (View > Show Session in Browser…)

`mac/src/SessionBrowser.{h,mm}` and `EditorController+SessionBrowser.mm`
serve the window's tabs from the process's in-process host, with no
`minicode host`. Two View items, no keys (`minicode.showSessionInBrowser`,
`minicode.copySessionBrowserLink`): Show opens the address through
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
- **One window's tabs.** The Mac reports nothing to the host about windows
  otherwise, so while a server runs the window sends `Client::windowTabs`
  (`windowJson`'s shape, by hand: groups of tabs with a document on the
  host, each group's tab on show) every half second when it changed, and
  `windowClosed` when it stops. `Web::Options::window` keeps the state the
  browsers get to that window and its documents, and a `show` of any other
  document answers `gone`. The window's id is the client's (`newId()`,
  below).
- **Terminals** opened while the setting is on are the host's (above,
  "Terminals on the host"), every window's; `Options::termNote` (in the
  hello) puts "Terminals opened while the session host is on appear here"
  in a muted line while the state has none.
- A tab opened before the setting was on has no document on the host and
  is left out of the page until it is opened again.

Checks: `sessionweb:in-process` (an in-process `SessionHost` with a routing
Sink, two windows' tabs, the view filtered to one, typing followed, the
host let go when the browser leaves); `make guitest
SUITES=session_browser` (the alert's Cancel and Turn On, the address
caught rather than opened, Copy, plain requests with and without the
token and with another Host, a WebSocket that gets the window's document
and an edit typed in the window, the port closed once the window is).

### Editing in the browser (stage 8b)

`Web::Options::editable` (the Mac's choice at Show; `minicode host --web
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
  for one behind. A headless host has no `save`: the page says to save
  from a window. The tabs' dirty dots are the host's dirty flags in the
  state, polled every half second.
- **Terminal keys** (`tkey {key, mods}`, `ttext {text, mods}`, `tpaste
  {text}`) are encoded on the server by the view's own screen, which saw
  the same modes as the host's (`TerminalScreen::encodeKey` with
  `appCursorKeys`, `appKeypad` and `keyboardFlags`; `encodeChar`;
  `encodePaste` with `bracketedPaste`), and go to the host as
  `TermInput`. Mods are xterm's (Shift 1, Alt 2, Control 4); on a Mac the
  page leaves Option out, as it composes characters there. Resizing is
  left to the app that opened the terminal.
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

`sessionweb:guest` (the host's rules for a guest); `sessionweb:edit` (a
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
  **Read-only pages may list and open** (the decision for R6): a tab opened
  or switched changes no text, so both come before the editable gate in
  `handleMessage`.
- **The page** keeps the open folders (`T.open`), each folder's last
  listing and errors, lists a folder each time it is opened (so it is
  current when opened), lists every open folder again on ↻, on `hello`
  (every reconnect) and when the page becomes visible again. There is no
  watching: the host watches nothing yet (stage 7). A click on a file
  sends `open` (the second click of a double-click, or Command/Control,
  keeps the tab) and, once `opening` says ok, waits for a document with
  that `abs` in the state, then shows it (`takePending`); a viewer kind
  (image, video, audio, pdf, database, archive, bookmark) says "NAME opens
  in the minicode window." in `#treenote` instead, and a text file with no
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

### Still to do

- Lines are re-sent whole per edit; a long line typed into from a browser
  wants character splices.
- No snapshot or patch messages exist yet: the view polls the host's state
  every half second for tabs, documents and terminal sizes.
- Several carets, folding, find, completion and the rest of the editor
  are the Mac's and GTK's, not the page's; so are closing and reordering
  tabs, which wait for the host to decide tabs (stage 6's note).
- Saving from a headless host waits for stage 7; Mac terminals are the
  app's own until they move into the host.
- Typing the session had not confirmed when the connection drops is lost
  (the page says so); it is not resent on reconnecting.
- The tree does not follow the disk by itself: the host watches nothing
  yet. Stage 7's watching should send the page a folder's listing when it
  changes. A headless host's page cannot open files (no window decides
  tabs); viewers in the page are R3.
