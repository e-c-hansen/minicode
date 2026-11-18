# The control channel, notes and tours

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## The control channel, notes and tours (Mac)

- Each mutible listens on `$TMPDIR/Mutible/<pid>.sock` (0600 in a 0700
  folder; another uid refused by `getpeereid`, closed with no answer, which
  the CLI reports as "closed the connection without answering" whether the
  close beats its write (EPIPE) or its read; removed on exit; at startup
  sockets of dead pids, and of live pids where nothing listens, are removed).
  Use `getenv("TMPDIR")`: `NSTemporaryDirectory()` ignores it; without it,
  `confstr(_CS_DARWIN_USER_TEMP_DIR)`. `MUTIBLE_CONTROL_DIR` overrides both
  sides (tests). One JSON object per line, checked against
  `ControlProtocol`'s table of commands and arguments (unknown or wrongly
  typed is an error answer, never a crash), handled on the main thread within
  100 ms (`ControlServer.mm`, `ControlCommands.mm`). A connection has one
  request at a time with the main thread (the next line is read once the
  answer is out), writes are non-blocking with a write source, and a peer
  that takes nothing for 10 s is dropped, so one client can neither queue
  unbounded main-thread work nor hold up the others.
- A headless session host (`mutible host --folder DIR`) puts
  `host-<hash>.sock` and its `.lock` in the same folder
  (`ControlChannel::socketFolders()`, shared by the CLI); the sweep of dead
  pids' sockets skips them, and the CLI never picks one (no pid in the
  name; `hostserver:*` checks it). See session-host.md's
  "The headless host". `--web PORT` (on `host` or `attach`) also serves a
  read-only view to a browser on 127.0.0.1 (session-host.md, "The browser's
  view").
- `tools/mutible-cli.cpp` builds `Contents/MacOS/mutible-cli`, which the
  launcher execs for subcommands: newest live socket (or `--pid`) in the
  shell's `$TMPDIR/Mutible` and the user's own temporary folder's (a nix
  shell's TMPDIR differs from the app's), only in folders that are the
  user's, real and 0700, moving on to the next when one refuses the
  connection, and last in the folders a copy from before the rename uses
  (`$TMPDIR/MiniCode`; `Rename::oldSocketFolder`, so `mutible` reaches a <!-- rename-mutible: keep -->
  running minicode); starting mutible and waiting 10 s when none runs, except for <!-- rename-mutible: keep -->
  reads (`state`, `selection`, `term-output`, `procs`, `page`), which exit 3
  at once, and `notify` and `open-url` (`ControlProtocol::launchesApp`); a
  running mutible with no socket (older than the channel) gets "quit and
  open it again", nothing launched. Exit codes 0, 1 command error, 2 usage, 3
  no app.
- Commands: `open path[:line[:col]]` (`--to`, `--note`, `--from`, `--keep`),
  `window`, `browse` (into the pane's agent page; see below), `page`,
  `term` (`--cwd`, `--title`,
  `--run` typed and entered visibly), `open-url` (BROWSER's; see "The
  terminal"), `diff` (`--base` never starting with
  "-"), `split`, `state` (`--pretty`), `selection`, `term-output`, `tour`, `procs`
  (`--json`; see "Processes and ports").
  None closes, replaces or reorders the user's tabs and terminals. `open`,
  `diff` and `tour` read and open only plain files (a FIFO blocks `open(2)`
  for good), `diff` up to 16 MB a side, reading and aligning off the main
  thread (`MCComparison prepare`, once).
- **The keyboard**: the editor is one text view for every tab, so putting the
  first responder back (`keepKeyboardDuring:`) is not enough: it then shows
  the agent's file, and the user's next keystroke lands there. While the
  window is key and the keyboard is in the editor's slot
  (`userIsInDocument`), `openTabForAgent:preview:quiet:` and
  `showComparison:preview:quiet:` add the tab without showing it, with a
  status note and `"background": true` (the CLI says so on stderr); a note
  waits with its tab (`MCNoteState.pending`) and is placed when it is shown.
  The user's own steps (Option+], a click) always show.
- **Notes** (`AgentNotes.mm`): a layout-manager highlight and a box over the
  text view, never in the file or its undo. The box sits in room of its own
  above its first line (2026-11-16, the walk's A5: it hid the `def` it was
  about): `MCGapLayoutManager setRoom:aboveCharacter:` keeps the room, MCFolds
  (the layout manager's delegate) answers it as paragraph spacing before that
  line, the room follows edits in `processEditingForTextStorage:` and is never
  changed inside one (a frame change waits a turn, `updateRoomSoon`); the
  fragment is taller but its used rect is the text's, and its baseline lower
  (the gutter numbers it so). A note left in a tab not on show
  (`pending`) gives the tab a blue dot (`MCTabAttentionNote`) and a tooltip
  line until shown (`notesWaitingChanged`); upper third (in large-file mode
  the line's place is an estimate, so it is read again after laying out
  there until it stays put, as `scrollEditorToCharacter:offset:` does);
  closed by Escape, ×,
  or an edit touching its lines (undo does not bring it back); plain text up to
  500 characters (cleaned by the handlers too, bidi controls dropped); not
  persisted. They go in one agent preview tab per window (`agentPreview`,
  italic title), replaced by each such open and kept by an edit or a
  double-click on its title; comparisons have a preview tab of their own, so
  the skill's open-then-diff keeps the note. A note moves the caret only in
  the preview tab, and a clean tab the agent just rewrote takes the disk's
  text before its note is placed (`notesCatchUpWithDisk`), or the reload a
  moment later closed it. Tours step with Option+] and [ (keymap commands
  enabled only during a tour) and show "2 of 3".
- `skills/mutible/SKILL.md` (under 150 lines; `mutible install-skill` or
  `make install`): show work as
  notes, a diff and tests in a terminal; open code with a note when asked where
  something is; read `state` and `selection` for "this"; never rearrange the
  user's tabs, `--run` long or destructive commands, or write workspace scripts
  unasked; plain paths on exit 3.
- **Agents check pages** (spec feature 6, 2026-11-06; `PageCheck`,
  `BrowserPages.mm`, `ControlPageCommands.mm`): `browse` loads into a second
  web view in `BrowserView`, the agent page, with a Yours | Agent switcher
  and an x that closes it (the user's page as it was); an address typed in
  the bar or a link from elsewhere goes to the user's page. The pane shows
  the agent page unless the keyboard is in the pane (`"pane_busy"`).
  `browse` takes only http, https, file, about:blank, host:port and paths,
  never a search or another scheme. `page --url/--text/--console/
  --screenshot/--reload`, `--yours` for the user's page; text, console and
  screenshots only for local pages unless `control.page-any-origin`, checked
  again when the read comes back. `page --paste [--all]` (off unless
  `control.page-input`; 2026-11-09, for practising in an online code pad):
  shows the agent page, gives its web view the keyboard
  (`giveKeyboardToAgentPage`, the window must be key), reads
  `document.activeElement` in the isolated world (`PageCheck::focusKind`: a
  textarea, a text-like input, contenteditable, or an iframe it cannot see
  into; never a password field), then posts Cmd+A and Cmd+V key events to the
  app's queue (`typeCommandKey:`), so they go the way the user's keys go. It
  never takes text as an argument: the clipboard is the caller's. Trap: in
  the offscreen harness WebKit hands the page no key events, so the menus
  take them (Edit > Paste reaches the page; the key downs do not), and a
  check can see the paste but not Cmd+A reaching an editor like Monaco.
  A screenshot never replaces a file unless
  `--overwrite` is given, and never goes through a symlink (O_EXCL, or an
  atomic rename with `--overwrite`). Status from
  `decidePolicyForNavigationResponse` (which answers as WebKit does without
  it: allow what it can show). The agent page's configuration installs, at
  document start, a listener in an isolated `WKContentWorld` (uncaught
  errors, rejections, and the only message handler) and a page-world
  wrapper of console.* that relays each call as a DOM event: an isolated
  world cannot see the page's console object, so there is no other public
  way; relayed lines can only be console calls (`fromPage`). Traps: a
  snapshot paints only the visible area, so `--full` rasterizes
  `createPDFWithConfiguration:` (sRGB; WebKit's #FF0000 reads back as about
  (1, 0.15, 0) in both kinds of picture); `WKWebView.title` can still be
  empty at didFinish, so `--wait` reads `document.title`; a GUI check's
  command line is built when queued, so start a test server before
  queueing steps. `BrowserPagesSuite` serves pages from a server inside the
  test process (127.0.0.1 and ::1); 20 checks grow mutible plus the agent
  page's WebKit process by about 0 MB (the page process itself is 25-35 MB).
- Ground rules for this and the agent workspace: nothing runs because a folder
  or file was opened; Claude Code's data is read, never written, moved or
  deleted, with one exception the owner approved (2026-11-15): the hooks
  for the waiting dots go into ~/.claude/settings.json when the user asks
  (Install Claude Code Hooks…, after a question showing exactly what is
  added, with a backup; see "The agent workspace"); nothing leaves the Mac.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- The control channel, notes and tours (Mac): Trap: in the offscreen harness WebKit hands the page no key events, so the menus take them (Edit > Paste reaches the page; the key downs do not), and a check can see the paste but not Cmd+A reaching an editor like Monaco.
- The control channel, notes and tours (Mac): Traps: a snapshot paints only the visible area, so `--full` rasterizes `createPDFWithConfiguration:` (sRGB;
