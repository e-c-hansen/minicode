# Agents: the inline agent, Request a Change, the IDE link, the workspace

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## The inline agent, Request a Change and restarting (Mac)

- **The inline agent** (Cmd+I, `AgentPanel.mm`; hover is Option+Cmd+I). What
  has the keyboard decides: a text file in the editor or a Markdown preview (on
  the source) is an edit; an idle terminal a command; anything else (tree, git
  panel, browser, LaTeX preview, picture, busy terminal, another window) a
  task. Cmd+Return in the bar makes any request a task. `MCAgentContext`
  carries blocks, so the bar knows nothing of the views. `MCAgentRun` runs
  `/bin/zsh -l -c 'exec <agent.command> -p --tools Read,Grep,Glob
  --no-session-persistence --output-format stream-json --verbose
  --include-partial-messages'` in the folder, the prompt on stdin (written off
  the main thread: a big buffer outgrows a pipe), five minutes at most, exit
  127 as not installed. stdout is read as it comes into `InlineAgent::Stream`
  on a serial queue of its own (text deltas, tool calls, the result line; no
  JSON at all means plain text, as the GUI suites' stand-ins print), and a
  `preview` of it reaches the bar at most every 80 ms. The prompt
  (`InlineAgent::editPrompt`) asks for SEARCH/REPLACE blocks or an answer;
  `applyEdits` matches each SEARCH ever more loosely about whitespace, all or
  nothing, so typing elsewhere meanwhile does no harm; a SEARCH found in
  several places is taken only where exactly one touches the selection (a
  caret: holds it), else refused (D22, 2026-11-07), and after a match that
  ignored indentation the REPLACE lines are re-indented to the file's. **The
  edit is pending, not made**: the session keeps every request's blocks
  (`_rounds`) and what they make of the buffer (`pendingText`), the bar shows
  `InlineAgent::pendingDiff` (SideBySide's rows and word changes), the lines
  it replaces are tinted red (lines only added: the line above underlined
  green, its break tinted); Return (Keep) applies the rounds again to the
  buffer as it is then, placed by the selection they were shown with moved
  through the user's edits (`InlineAgent::moveOffset`), never the caret of
  the moment, and when that is not the change the bar showed
  (`InlineAgent::sameChange`) the bar shows the new diff and waits for
  Return again; else the result goes in through `replaceSourceWithText:`
  as one undo step; Esc drops it, nothing to restore. A bar closed from
  elsewhere with an edit waiting says in the status bar that it was not made.
  Commands are typed, never run. `make guitest SUITES=agent_panel` streams
  `tests/fixtures/claude/stream` (written to the SDK's message shapes, not
  recorded) through a stand-in script; never run the real Claude Code there. Tasks write the prompt to
  `$TMPDIR/Mutible-agent/` and run `cd <root> && <agent.command> "$(cat
  file)"` in a new terminal tab. Traps: `NSTextView string` is the live
  storage, so "the text before" must be a copy; the bar's place uses line
  fragments up to the caret only (the first Cmd+I once laid out the whole file:
  1.7 s); `firstRectForCharacterRange:` gave nothing usable offscreen.
- **Request a Change** (Shift+Cmd+K, `ChangePanel.mm`, an `MCChangeSession` per
  window): `MCChangeBar` across the top of the right pane, laid out first
  (`layoutChangeBarIn:`), styled as developer mode (an amber stripe, the
  sidebar tinted 13% amber, a DEV pill, the source folder) so it is not taken
  for an ordinary field; its box wraps and grows to 10 lines
  (`heightForWidth:`). Return sends, Shift or Option+Return adds a line, Esc
  (`complete:`) closes. The prompt (`ChangeRequest::prompt`: the request, then
  asking for `make`, CLAUDE.md, and no commit, push or release) goes to
  `$TMPDIR/Mutible-change/request-<pid>-<n>.txt`, and `cd <source> &&
  <develop.command> "$(cat <file>)"` is typed into a new terminal tab (or
  Claude's previous idle one), focused; the whole prompt on the line had
  flooded it, the tab title and zsh's history. Source folder
  (`MCChangeSourceFolder`): `develop.source`, else the checkout the bundle was
  built in, else `MCChangeCloneFolder()` (`~/Library/Application
  Support/Mutible/source`), cloned on first use by one typed line (`(git clone
  --branch v<version> <repo> <dir> || git clone <repo> <dir>) && cd <dir> &&
  claude ...`). "Source" means a Makefile and `mac/src/EditorController.mm`. A 2 s
  timer watches `<source>/Mutible.app/Contents/_CodeSignature/CodeResources`
  (codesign is `make app`'s last step, and that file is its target, so a
  `make` with nothing to do leaves it alone) and offers Restart once it is
  newer and stable. Prompt files whose writer has quit, or a day old, are removed as
  the next is written (`MCRemoveStalePromptFiles`, the inline agent's too).
- **Restart** (`AppDelegate restartIntoApp:resumingIn:source:command:`,
  `MCRestarting`): the quit questions titled "Restart mutible?", leaving
  Claude's terminal out; unsaved tabs come back from backups;
  `applicationWillTerminate` writes `MCRestart` and `MCLaunchAfterExit` spawns
  a detached `/bin/sh` that waits for our pid and runs `open -n
  <source>/Mutible.app`; the new process takes a record under two minutes old
  (`reopenAfterRestart:`), brings every window back from sessions.json
  (written by that same quit; see "Every window comes back") and runs `develop.command --continue` in the
  resuming window (never the record's command, and only in a folder that
  looks like the source). After a restart the user runs the repo's build until the next `brew
  upgrade`. Restart mutible (mutible menu, the status bar's ↻,
  `restartMutible:`; no key, Cmd+R is Refresh) uses
  `NSBundle.mainBundle.bundlePath` (beeps when not a .app).
- `mc-dev` (the user's `~/Code/dotfiles/shell/aliases.sh`) runs the repo's
  build; launching a build registers it with Launch Services, so Finder may
  offer two mutibles (`lsregister -u` it).

## New Agent Task (Mac, A8, 2026-11-18)

- Edit > New Agent Task… (`mutible.agent.newTask`, Option+Shift+Cmd+A,
  beside Shift+Cmd+A's panel; Mac only, no Linux row yet). Core
  `AgentTask.{h,cpp}` (`agenttask:*`): `nameFromPrompt` (first four words
  that say something, ASCII, 32 bytes), `sanitizeName` (a branch and a
  folder: `[A-Za-z0-9._-]`, no `..`, no `.lock`, 60 bytes), `uniqueName`
  (-2, -3), `worktreePath` (`<folder>/.claude/worktrees/<name>`, where
  Claude Code's own go and where `MCAgentModel`, the tree's Worktrees node,
  Compare with Main and Apply to Main look, so nothing of the review is
  new), `insideClaudeHome` (a window on ~ would write ~/.claude: refused),
  `worktreeAddArgs` (`worktree add -b <name> <path> HEAD`), and
  `startCommand`: `cd <wt> && <agent.command> '<prompt>'` when the prompt
  is one line of at most 160 bytes, else `"$(cat <file>)"` from
  `$TMPDIR/Mutible-task/task-<pid>-<n>.txt` (Request a Change's lesson:
  a long line floods the terminal, its title and history). The
  `agenttask:shell` group runs the line through /bin/sh with printf for
  claude: quotes, `$( )`, backticks and `!` arrive as one argument and
  nothing in them runs.
- Mac `AgentTaskBar.{h,mm}`: `MCAgentTaskBar` (a bar across the top of
  the right pane under Request a Change's, blue where that one is amber:
  the prompt box, wrapping and growing to 8 lines, the name field, which
  follows the prompt until typed in, `nameEdited`, and the branch and
  folder it gives) and `MCAgentTaskSession`, owned by `MCAgentWindow`
  (`tasks`, made on first use; EditorController has `newAgentTask:`,
  `agentTaskBarDidChange`, `layoutAgentTaskBarIn:` in
  `relayoutRightArea`, and the colors in `applySettings`). `begin` looks
  at the repository on its own queue (toplevel, HEAD, branch, `status
  --porcelain` count) for the line beside the heading
  (`AgentTask::startsFromText`: uncommitted changes stay behind) or the
  refusal; `start` looks again (so it never trusts a stale look), picks a
  free name against `for-each-ref refs/heads` and the folders, runs `git
  worktree add` (300 s limit), adds `/<rel>.claude/worktrees/` to the
  common dir's `info/exclude` when `check-ignore` says the folder is not
  ignored (never .gitignore), then types the line into a terminal tab
  (`changeTerminalRunning:focus:`, Request a Change's, which makes room
  for Claude's screen) and `[MCAgentModel addTask:]`. The agent command is
  looked for as the language servers' are (`MCResolveCommand`), plus
  Claude Code's old `~/.claude/local/claude` (read only, a shell alias
  names it); not found, the bar says the inline agent's sentence
  (claude.com/claude-code, agent.command).
- The panel: `MCAgentModel tasks` (in memory, newest first, forgotten
  with the folder) and `pendingTasks`, those no indexed session works in
  (its worktreePath, or a cwd in the task's folder, by realpath). Their
  rows (`T <name> | starting · <name> · <age>`) sit above the sessions
  with a blue ring; a click shows the terminal (else Compare with Main);
  the menu has Show Terminal, Compare with Main…, Open Worktree in New
  Window and Copy Worktree Path. Once Claude Code writes its transcript
  the session row stands for the task. Session rows with a worktree gained
  Compare with Main… (`onCompareWorktree`, MCAgentWindow's
  `compareWorktreeAt:`).
- Checked by `make guitest SUITES=agent_task` (a stand-in claude script;
  the transcript it writes for a SESSION prompt goes to the suite's
  scratch MUTIBLE_CLAUDE_PROJECTS). Not run against a real Claude Code
  session: its folder-trust question for the new worktree is answered in
  the terminal.

## The Claude Code IDE link (Mac, agent.ide-link)

Claude Code's `/ide` answered per window, as VS Code's extension does. Off by
default; View > Claude Code IDE Link (`MCIdeLinkMenuItem`, its own target)
writes `agent.ide-link` through `Settings::withValue`. Core `IdeLink.{h,cpp}`
(`ide:*` tests): SHA-1, base64, the upgrade handshake, RFC 6455 frames, MCP
over JSON-RPC with a tool table, the lock file, the tools' JSON shapes. Mac
`IdeLinkServer.mm`: sockets on the main queue, the lock file, the tools.

- **The protocol, as read** from Claude Code 2.1.319 (the bun binary's
  embedded JS: search it for "Failed to read IDE lockfile", "ws-ide",
  "callIdeRpc", "openDiff") and the VS Code extension 2.1.318
  (`~/.vscode/extensions/anthropic.claude-code-*/extension.js`: "Fre(",
  "Wnt(", the tool handlers): `$CLAUDE_CONFIG_DIR/ide` or `~/.claude/ide`,
  `<port>.lock` 0600 holding pid, workspaceFolders, ideName, transport "ws",
  runningInWindows, authToken. Claude Code connects to `ws://127.0.0.1:port`
  with subprotocol "mcp" and `X-Claude-Code-Ide-Authorization`, sends
  `ide_connected {pid}`, listens for `selection_changed {text, filePath,
  fileUrl, selection}` (0-based lines, UTF-16 characters), calls `openDiff
  {old_file_path, new_file_path, new_file_contents, tab_name}` and expects
  `[FILE_SAVED, text]` or `[DIFF_REJECTED, tab]` (it writes the file itself),
  then `close_tab`; `getDiagnostics {uri?}` before and after edits (it
  compares the answer's uri with the one it sent, so answer under that
  spelling). It shows the model only getDiagnostics and executeCode. It uses
  openDiff only when its global `diffTool` is "auto". It auto-connects when
  CLAUDE_CODE_SSE_PORT is the port (mutible's terminals get it,
  `MCIdeLinkTerminalEnvironment`), else /ide lists lock files whose folders
  hold its cwd (unresolved, so the lock lists the folder as spelled and
  resolved); it skips the pid check outside VS Code and JetBrains terminals,
  and deletes lock files of dead pids itself.
- **Rules kept**: 127.0.0.1 only, a 48-hex-digit secret per window, Host must
  be loopback, nothing said before the token; writes nothing under ~/.claude
  but its own lock file (temp file and rename, removed on window close,
  setting off and atexit); removes at start only lock files with ideName
  mutible, another pid, that pid gone. openFile goes through
  `openTabForAgent:preview:quiet:` (beside the user while they type) and
  highlights startText..endText with an empty note; openDiff is a
  comparison tab (`showComparison:preview:NO quiet:YES`) with a decision bar
  (`MCComparison decisionPrompt/onDecision`, CompareView) and Accept and
  Reject per hunk, FILE_SAVED carrying `acceptedText`; closing the tab
  rejects (`closePageOf:` calls `tabClosed`); close_tab and closeAllDiffTabs
  close only comparisons the link offered. Not offered: saveDocument,
  executeCode. `selection_changed` is remembered per client (`IdeConn
  lastSelection`), so one who joins while a selection waits on the 0.3 s
  timer is not sent it twice. A client that goes leaves its diff saying so.
- Checked by `make guitest SUITES=idelink` (a WebSocket client on a thread;
  `MC_GUI_NODE=/opt/homebrew/bin/node` adds node's own client) and the
  `ide:*` groups. Not run against a real Claude Code session.

## The agent workspace (Mac)

- **Core**: `AgentTranscript` reads Claude Code's JSONL behind a `Format`
  interface (a second agent would be a second Format), skimming lines for type,
  timestamp, ids, cwd, gitBranch, version, text and each tool call's file_path,
  command and description; outputs are byte ranges read later by `readOutput`
  from two 64 KB windows. Bad lines are counted and skipped, unknown fields
  ignored, Claude Code's request copies left out; only user and assistant lines
  count. `Tail` reads whole lines from the last offset, holding back a line
  being written, and restarts on a replaced or shrunk file. `AgentIndex` finds
  sessions whose cwd is the folder or inside it, from the files' own cwd (the
  first decides), never from the lossy encoded folder names; a file whose first
  cwd is elsewhere is read for 64 KB once. Not thread-safe: copy rows out.
  `AgentEdits` (2026-11-15, `agentedits:*`): one session's Edit,
  MultiEdit, Write and NotebookEdit calls on one file (`collect`: a
  tool call's input is a `Block::input` Span, read once on demand, never
  kept per line; paths through `samePath`, realpath and /private/tmp =
  /tmp), and `analyze` against the file as it is: each change stands or
  is gone, and undone last first while each new text is found exactly
  once (a created Write ends it with no file) gives the file before the
  session; anything ambiguous stops that with a sentence saying why.
  `AgentEvidence` also lists the file edits (`Evidence::edits`).
  `AgentEvidence`: every Bash call, test commands recognised anywhere in a
  chain past env, time, timeout and assignments (pytest forms, make test and
  check, ctest, npm/pnpm/yarn test, cargo test, go test, python -m unittest,
  tox, nox, jest and vitest, through npx, uvx and the like), their summaries read
  from the tail, no verdicts. **Entries** (A6, 2026-11-16, `evidence:entries`):
  `Evidence::entries` is the session item by item, each an `EntryKind`
  (prompt, message, command, edit, tool, subagent, sent, received,
  notification, hook; commands and edits point into their lists) so a
  port gives each its own look. A text-only user line is a prompt (origin
  human or coordinator), a message from another session (origin `peer`,
  named by `origin.name`, its `body`), a task notification (its
  `<summary>`) or a slash command; an `attachment` line of a `hook_*` type
  with something said is a hook. SendMessage's `to` is named by what a
  peer message said of that id, or a subagent's task (`toolUseResult.agentId`
  of its Agent call). A command keeps `toolUseResult.stderr`
  (`stderrLineCount`: Claude Code appends it to the output). Entry text is
  cut at 16 KB. Paths: `~/.claude/projects`
  (`$MUTIBLE_CLAUDE_PROJECTS`), the `/private/tmp/claude-<uid>` scratchpads
  (`$MUTIBLE_CLAUDE_TMP`). Fixtures: `tests/fixtures/claude`.
- **The panel's column** stays the list's width (`autoresizesOutlineColumn`
  off, `fitColumn` on layout, expansion and the clip view's frame
  changes), so a title's "…" is at the pane's edge once rows expand
  (2026-11-15; file rows' paths had widened it off screen); a file row's
  name and folder each give way in the middle.
- **Mac**: `MCAgentModel` keeps the index on one serial queue and git on
  another, copies rows to the main thread, follows FSEvents on transcripts and
  scratchpads, and empties (saying so) if the transcripts folder goes.
  `MCAgentWindow` is made lazily. The Agents panel (Shift+Cmd+A) swaps in like
  the git panel: sessions newest first, subagents nested, files in the folder,
  the worktree and outside (gone ones struck through); a click on a file
  is `MCSessionFilePage` (AgentPages, kind `session-file`), what that
  session changed there (each change an `InlineAgent::pendingDiff`, in
  syntax colors from `ChangeReview::syntaxByLine` of each side as written,
  the whole as an MCComparison of before and now when `analyze` rebuilt it),
  read on its own queue and again on the model's change notification;
  the row menu keeps Open. The Evidence view lists the edits among the
  commands, each a link to that page. The Evidence view
  (`MCEvidencePage`, an `EditorPage`): the final message beside the last test
  command's result, then the entries in order, every command with the first
  and last 20 lines of output read lazily (Show all up to 1 MB); Open in
  Terminal types a command at the session's folder. **Each kind its own
  look** (A6, 2026-11-16, the owner's "hard to skim"; `agents` suite's
  kinds steps): prompts, Claude's messages and messages to and from other
  sessions are Markdown through the window's own builder
  (`MCAgentWorkHost agentMarkdown:width:`, +Markdown's
  `markdownFragmentFor:width:`, which saves and puts back the preview's
  page state, math left as TeX), fonts scaled to 12.5 and moved to the
  entries' indent by an outer text block (`FitMarkdown`; a table stays
  page-wide). Built on the main thread a slice of about 12 ms a turn, plain
  text until then, cached per entry with its text (dropped on a settings
  change and a settled new width). Colors: the command in
  `syntax.function`, tool names in `syntax.keyword`, sessions and
  subagents in `syntax.type`, hooks in `syntax.comment`, links in
  `markdown.link`, your prompt's label in `markdown.heading`, output in a
  box tinted as a Markdown code block, stderr, an exit line, a tool's
  error and a failed hook in the diagnostics' error color, the rest muted.
- **Waiting for you** (spec feature 7, 2026-11-06): a terminal's id
  (`TerminalView terminalID`, what `mutible state` reports) is given when it
  is made, not on first use, because its shell gets it as
  `MUTIBLE_TERMINAL_ID`, with `MUTIBLE_PID` (inherited ones are dropped).
  `mutible hooks --install` (run by the user only; backs up, merges, a second
  run changes nothing; a symlinked settings.json stays a link and a
  hard-linked one is written in place, `ClaudeHooks::writeSettingsFile`) adds `mutible notify --from-hook` to Claude Code's
  Notification, Stop, UserPromptSubmit and PostToolUse hooks. **The app
  offers it too** (2026-11-15, `ClaudeHooksOffer.{h,mm}`, the owner's yes
  to writing ~/.claude/settings.json on the user's two presses): the
  terminal posts `MCTerminalClaudeDidStartNotification` when a typed
  command's program is `claude` (`ClaudeHooks::startsClaude`) or, a second
  later, the foreground binary is Claude Code's
  (`isClaudeProgramPath`: a native install's process is named for its
  version), and a bar over that window's terminals (`MCTerminalPanel
  noticeBar`, an `MCInstallBar` in `showMessage:` form) offers Turn On…
  once per launch (`MCInstallBarTakeOffer`), muted for good by Not Now
  (`MCInstallBarMuted("claude-hooks")`), never once the hooks are in;
  mutible > Install Claude Code Hooks… (`mutible.installClaudeHooks`)
  and the Agents panel header's line (shown while they are missing) run
  the same `MCInstallClaudeHooks`: a question with the JSON to be added,
  then `ClaudeHooks::install` (the CLI's code, moved to the core), then
  where it wrote and the backup. The command is `commandFor`'s: plain
  `mutible notify --from-hook` when `MCFindProgram(@"mutible")` finds
  it, else the bundle's launcher by its quoted path. The harness gives
  every run a scratch `CLAUDE_CONFIG_DIR`; tests never touch ~/.claude; notify never
  starts mutible and with --from-hook prints nothing and exits 0 whatever
  happens. Core: `ClaudeHooks` (hook JSON, the settings merge) and
  `AttentionBoard` (marks, sessions); Mac: `MCTerminalAttention` draws it
  (the tab's dot and tooltip, the Dock badge, Ctrl+Cmd+W, Show Terminal in
  the Agents panel, the session's scratchpad and worktrees first for a
  clicked path). Typing (the grid's `onTyped`, the input line's edits) and
  closing reach it as Terminal.h notifications. `make guitest
  SUITES=attention`. Trap: once a Control+Command key reaches the main menu,
  AppKit redraws its own Emoji & Symbols and Start Dictation items in their
  Globe-key form beside hidden twins with the same key; a menu key clash
  check must skip them (ControlSuite does).

## The plan usage forecast (Mac, A13, 2026-11-18)

- **Where**: under the Agents panel's heading (`MCUsageView`,
  `UsageView.{h,mm}`), between the hooks line and the sessions: the
  5-hour window, the week, the burn rate, unseen usage (the fit's line is
  the text's tooltip), then "Log plan usage from Claude Code's status
  line…" or "Stop logging plan usage…". `MCUsageModel` is one per app (the
  plan is the user's, not a folder's), refreshed by the panel's reload
  (its model's changes and the 30 s timer; at most once in 5 s), on its
  own serial queue.
- **Core** `ClaudeUsage.{h,cpp}` (`usage:*`): `parseUsageLine` skims an
  assistant line (type, timestamp, requestId, sessionId, isSidechain,
  message.id, message.model, message.usage) without decoding the reply;
  only the usage object goes through `Json::parse`. A line with all-zero
  tokens (`<synthetic>`) is no usage. Cache writes: the total is
  `cache_creation_input_tokens`, the hour's share
  `cache_creation.ephemeral_1h_input_tokens` (an advisor turn's split
  covers only its first iteration; the rest is priced as 5 minutes).
  `Tally` dedupes by message id plus request id (ccusage's rule: without
  a request id, plus the session, plus the time unless the id is `msg_`),
  keeping the main thread's copy over a subagent's, then the larger.
  `priceFor` is a table of API list prices (USD per million; 5 m writes
  1.25 times input, 1 h writes 2 times; Haiku 5.5 above 100K and Sonnet
  4/4.5 above 200K context at their long prices for the whole request;
  fast mode 2 times, Opus 4.6's 6), longest name first, an unknown model
  its family's newest price. `Scanner` reads files changed in the last
  8 days (`kHorizon`) from where it stopped, whole lines, 4 MB windows
  growing to 64 MB for one line; a replaced or shorter file is read again
  and the tally drops the copies. `blocks` and `blockCostPerHour` are
  ccusage's. Samples: `v1 <time> <5h %> <5h reset> <week %> <week reset>`
  ("-" for a missing window). `fitScale`: per window (one resets_at),
  readings from the window's start (reset minus 5 h or 7 d, when the
  transcripts reach back that far) to the first sample, then sample to
  sample once $0.25 or 2 points moved; the scale is the readings'
  percent-per-dollar median weighted by dollars, so a minority of
  readings with unseen usage does not move it; each reading's excess over
  scale times cost is unseen usage. `forecast`: the last sample plus the
  scale times the cost since; the rate over 15 min, 1 h and the window
  (the week: 1 h, 5 h and the window), the middle one central, the
  others the range; a window that reset since the last sample is worked
  out (5 h from the first request after the reset). `describe` gives the
  lines every port shows. Credit to ccusage (MIT) in the header and in
  `docs/REFERENCE.md`; nothing of it is linked.
- **The wrapper** `ClaudeStatusLine.{h,cpp}` (`statusline:*`) and
  `mutible statusline [--then COMMAND]` (mutible-cli, intercepted before
  the command line parse: it runs every few seconds; about 20 ms with a
  chain): the sample goes to `usageDir` (Mac: ~/Library/Application
  Support/Mutible/usage; XDG state elsewhere; `$MUTIBLE_USAGE_DIR`),
  skipped when the last line says the same and is under a minute old, the
  log moved to `.1` past 1 MB. Then `/bin/sh -c COMMAND` with the same
  stdin (fed on a thread), its stdout and stderr ours, 10 s at most,
  signals passed on; exit 0 always. The launcher, with no mutible-cli,
  still runs the chained command. Trap: the status line runs every few
  seconds, so `mutible statusline` is handled before the command line's
  parse and reads no more than the last 512 bytes of its log.
- **Install and undo**: the waiting dots' consent pattern exactly
  (`StatusLineOffer.{h,mm}`: a question with the JSON that goes in, the
  write only on the second yes, a backup first, then where it went). The
  old command travels in the new one (`mutible statusline --then
  '<old>'`), every other key of statusLine (padding, refreshInterval) and
  of the file kept. Install keeps a record (`statusline-install.json` in
  the usage folder: the file before and as written); undo writes the old
  file back byte for byte while the file is still what install wrote (or
  removes it, when install made it), else puts back only statusLine (its
  old command, or none) after a backup.
- Checked by `make guitest SUITES=agents` (the plan usage steps at its
  end: a fixture transcript and log, the forecast's lines, install over
  an existing statusLine in a scratch CLAUDE_CONFIG_DIR with Cancel
  first, the built mutible-cli logging and chaining, undo restoring the
  file byte for byte). Not yet run against a real Claude Code session's
  status line; the look of the section in a narrow sidebar needs the
  owner's eyes.
- **Linux** (2026-11-18, `linux/HANDOFF.md` item 103): the same section
  in the GTK Agents panel, over the same core and wording
  (`linux/src/UsageView.{h,cpp}`: `UsageModel`, one per app, on a
  GThreadPool of one thread; `StatusLineOffer.{h,cpp}`: the two presses
  through `Question`, the JSON in its preview box). The log is in
  `$XDG_STATE_HOME/mutible/usage`. Checked by the `plan_usage` GUI suite
  (both "no reading yet" and the fitted forecast, Cancel then install
  over an existing statusLine in a scratch CLAUDE_CONFIG_DIR, the built
  `mutible statusline` logging and chaining, undo byte for byte, and a
  symlinked settings file kept a link through both).

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- The inline agent, Request a Change and restarting (Mac): Traps: `NSTextView string` is the live storage, so "the text before" must be a copy; the bar's place uses line fragments up to the caret only (the first Cmd+I once laid out the whole file: 1.7 s);
- The agent workspace (Mac): Trap: once a Control+Command key reaches the main menu, AppKit redraws its own Emoji & Symbols and Start Dictation items in their Globe-key form beside hidden twins with the same key; a menu key clash check must skip them (ControlSuite does).
- The plan usage forecast (Mac, A13, 2026-11-18): Trap: the status line runs every few seconds, so `mutible statusline` is handled before the command line's parse and reads no more than the last 512 bytes of its log.
