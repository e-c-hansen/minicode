# The layout of the code (full map)

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## Layout of the code

### The core (`src/*.cpp`, pure C++17, tested by `make test`)

No dependencies and no I/O unless noted, so the ports can use it. New logic
goes here first, with tests; the GUI only displays it.

- `SyntaxHighlighter` (about 200 grammars, `IncrementalHighlighter`),
  `MarkdownParser` (CommonMark plus GitHub, math, `lines()`), `MarkdownEmoji`
  (GitHub's common emoji shortcodes, for the parser), `MarkdownEdit`
  (the block behind a preview click), `MarkdownPage` (a page in pieces, and
  which survive an edit), `MarkdownHtml` (Export as HTML's page), `MarkdownTasks` (task lists), `MathTex`
  (the Markdown math batch), `MermaidDiagram` (```mermaid blocks for mmdc),
  `LatexDoc` (editable ranges of LaTeX source), `LatexScratch` (the
  preview's output folders and sibling records), `LatexLog` (tectonic's
  problems with their files and lines),
  `SyncTex` (page point to source lines, and line to boxes), `Json`, `IdeLink` (the Claude Code
  IDE link: WebSocket, MCP, lock file).
- `Settings` (the settings file, themes, every resolved color), `Commands` (the
  command registry and keys.conf; the GTK port's keys are a table of their
  own in `CommandsLinux.cpp`, the Mac's rows untouched), `EditorTabs` (tab rules, and
  `Model::Window` and `Model::Session`, which hold the tabs by them: see
  "Tabs, sessions and the disk"), `Backups` (backups
  of unsaved tabs, with a POSIX `Fs`), `TabSessions` (what a folder and a
  window keep between runs: sessions.json's `Store`, the window record),
  `SavePlan` (when backups, auto-saves and GTK's session are written,
  from a clock the port ticks), `FileDetails` (tree sorts, ages, folder
  totals, the tree's focus: Focus on Folder's breadcrumb and widening, and Add to Focus's set of folders), `RunFile` (Run File's quoting), `Runnables` (the tests in a
  file, found through the highlighter's tokens, and the command for one
  test or the file), `RightAreaLayout` (the right
  side's panes and bars by the Mac's rules, which the GTK port lays out from).
- The document model (SESSION-DESIGN.md's stage 1, 2026-11-16; namespace
  `Model`): `Text` (UTF-16 in chunks of about 1,024 units with Fenwick
  trees over lengths, LSP line starts and code points, so both a UTF-16
  port and a code point port address it in log time; a version every
  change moves on; `apply`, the multi-range edit in EditSpans' shape, in
  either unit; `text:*`, an oracle against a plain string) and `Document`
  (a file's text, saved text, stamp, encoding and mark, gone and large,
  with the Mac's rules: dirty means differs from the saved text, a stamp
  changed by date, inode or size, UTF-16 with its mark read exactly and
  written back, no lone surrogate written; `document:*`). The language
  server client's copy of each file is a `Model::Text`; the ports keep
  their own documents until stages 3 and 4 move the session into the core.
  `UndoHistory` (stage 2, 2026-11-16): a document's undo and redo as the
  Mac's rules give them (named steps; typing joined while the caret stays
  where the last step left it, until `breakCoalescing`, a moved selection
  or an undo; a multi-range edit as one step; Reload from Disk a step that
  keeps the ones before it; an undo refused, the history cleared, when a
  range no longer holds what the step put there; `foreign` moves the steps
  through a change made outside it and drops only those it touches). It
  never holds the text: steps keep what they removed and inserted, as
  ascending, apart pieces composed from the edits as they come (a batch
  recorded last first, as every port applies one, in one pass), and
  `undo()` hands back a plan for the port to apply; in UTF-16 units or
  code points. `undo:*`, with a fuzz against a plain string. The GTK
  port's `Document` records every buffer edit into one and GTK's undo is
  off (`linux/HANDOFF.md` item 72); the Mac's editor moves onto it next,
  its NSUndoManager rules being the ones above.
- The session's host (SESSION-DESIGN.md's stage 4, 2026-11-17; namespace
  `Shared`, since `Session` names classes already): `Ot` (edits that
  crossed in flight made to agree: lists of operations applied in order,
  split into deletions and insertions; the host's insertion goes first at
  one place, an insertion inside a deleted range survives at its start;
  `ot:*`, a fuzz holding both orders to one text in both units),
  `SessionProtocol` (the messages, and `encode`/`decode` as one JSON line
  each for loopback; a text with a lone surrogate goes as an array of
  units), `SessionHost` (`Host`, the logic with no thread, and
  `SessionHost`, a Host on a thread of its own: every open document's
  `Model::Document`, version and `UndoHistory`, one per document whoever
  edits it, a log of applied edits in both units back to the oldest base
  a renderer may still send, and the windows' tabs as reported) and
  `SessionClient` (a renderer's side: an outbox with one edit in flight
  at a time, as ot.js's client, so the host moves an edit past nothing of
  the sender's own; edits from elsewhere moved past the unconfirmed ones
  and handed to the document's `Listener`; steps and coalescing decided
  by the renderer, which knows the keys; waits that are a round trip
  through the host's queue: `settle`, `undo`, the undo state; and on
  request a copy of the host's text kept from its own edits), and
  `SessionServer` (stage 6: a headless host for one folder on a Unix
  socket, `HostServer`, one per folder by a lock file, ending after its
  idle time with no renderer and nothing unsaved or running; `HostLink`, a
  renderer's connection; `dumpSession`, `mutible attach --dump`'s text;
  `hostserver:*`; docs/notes/session-host.md), and `SessionFiles` (stage
  7's first read: `FsRead` lists one folder of the session's, confined to
  it by `confinedPath`, with the apps' trees' dotfile, order and ignore
  rules, for the browser's tree; `host:fs-list`; and its first write,
  `saveDocument`, the host's DocSave by the apps' save rules;
  `host:save-rules`, `host:save` in tests/session_save_tests.inc; and
  following the disk, `diskAction` and `readDiskText`), `SessionSearch`
  (stage 7's heavier reads on the host's own worker threads,
  `ReadWorkers`: FsRead's "files", Quick Open's list, and "search", Find
  in Folder, answered as they go and cancelled by "cancel", for the
  browser's page; `search:*`), `DiskWatch`
  (stage 7's watching: the core's one platform interface for the disk's
  notices, inotify on Linux and FSEvents on the Mac, each on a thread or
  queue of its own behind a pipe the host polls, with the pure `Filter`,
  which folders count, and `Coalescer`, bursts made one batch;
  `diskwatch:*` in tests/session_disk_tests.inc), `SessionWebViewers`
  (what the browser's page renders itself: a notebook as a page, a SQLite
  database's tables and rows; `sessionweb:viewers`), `SessionWebTypeset`
  (the LaTeX preview's typesets the app publishes, kept by the document's
  real path and served at /typeset/, and `PageRenders`, their pages drawn
  as pictures at /typeset-page/ by the app's renderer;
  `sessionweb:typeset`, `sessionweb:typeset-pages`), `SessionWebGit` (the page's
  Source Control: `runGit`, a POSIX git runner with fixed arguments, the
  core's `readUncommitted` held to the folder, the list and the review as
  JSON, `GitReads` on a thread of its own; `sessionweb:git`; the window's
  commit and worktree pages, `ReviewPage` and `readReviewPage`, and the
  pictures' header sizes and /picture/ addresses; `sessionweb:pictures`
  and `sessionweb:review-pages` in tests/session_web_review_tests.inc), and
  `SessionWebAccess`
  (who may use the browser's view: single-use links, the credential's
  cookie, idle expiry, the app's question before a new browser and its
  rate limit; `sessionweb:access*`). The wire
  unit is the renderer's (Hello says which); the host converts at the
  edge. A UTF-16 renderer must never split a surrogate pair (a renderer
  counting code points could not follow). `host:*`: the edit loop, undo
  across renderers, a fuzz with renderers in both units typing, undoing
  and resetting while messages wait and go in random order (each link in
  its own order), and the threaded host (TSan in CI). Since stage 4a the
  GTK port's documents are mirrors of a host in the same process
  (`linux/src/SessionLink.{h,cpp}`, `linux/HANDOFF.md` item 87): undo
  asks the host, the language server sync reads its text, and the GTK
  harness checks every buffer against the host as each suite ends; CI's
  GTK job runs a named set of suites again with every message through
  the wire format (`MUTIBLE_HOST=loopback`). The
  Mac (4b) is next, behind a setting for one release.
- Editing: `LineDiff` (Myers, split at lines unique on both sides past
  1,000 edits, so a large reformat keeps a hunk per changed run; UTF-16,
  `mapOffset`), `LineComments`,
  `AutoIndent` (with `ScanCache`), `KeepSorted`, `MultiCaret` (carets and line
  commands), `AutoClose` (auto-closing pairs), `Snippet` (LSP snippets:
  parse, expand, the tab stops through edits), `LineIndex` (line starts, a gap
  buffer), `PieceTable` (a text as pieces of an original and of its
  edits: large-file mode's storage), `BracketMatch`, `Folding`, `TextFind` (find and replace, UTF-16 and
  UTF-8), `TextScan` (a large text searched for what every match holds
  first, sixteen bytes at a time: the find bar's large files and Find in
  Folder's needle), `Regex` (its linear-time engine), `Formatter` (Format Document's
  plan), `InstallHints`, `LargeFile.h` (VS Code's large-file thresholds,
  header only), `WrapIndent.h` (where a wrapped line goes on,
  editor.wrapping-indent, header only), `SpellCheck` (which words of a file are prose for the
  spell checker).
- Language servers: `LspClient.h` over nine files (see "LSP": protocol,
  results, WorkspaceEdits, diagnostics with versions, completion edits,
  symbols, folding ranges, the second server's routing and
  `DiagnosticStore`, the Problems rows, watched files' globs and
  registrations), `PythonEnvs` (interpreters found without running any).
- Terminal: `TerminalStream` (bytes to lines and shell events; `TermParser`,
  `applySgr`, `termCharWidth`), `TerminalScreen` (the grid, scrollback, reflow,
  find, mouse encoding), `TermLinks`, `ShellIntegration.h` (the zsh
  integration's .zshenv stub and private keys, header only),
  `TerminalLayout` (a terminal tab's columns and stacks: shares, splits,
  bars, keyboard resizing, minimums).
- `FolderSearch` (Find and Replace in Folder, atomic writes, TODOs),
  `GitStatus` (status, diffs, git time limits), `GitGraph` (log, refs, lanes),
  `GitModel` (what every port's Source Control panel shows and runs),
  `QuickDiff` (the editor's change bars against the index), `ChangeReview`
  (the review view's rows, and what staging, unstaging or discarding one
  block or a few lines makes of the file), `MergeConflicts` (git's
  conflict blocks, and what keeping a side or both makes of the text).
- `CommandPalette` (matching, modes, Quick Open; symbol rows in
  `CommandPaletteSymbols.cpp`, apart so `mutible-cli` links the rest
  without the language servers' names), `FolderPicker`
  (the folder picker's paths, rows and completion), `ThemePicker` (the
  theme picker's list and its preview, choose and cancel), `HintsSearch.h`
  (the shortcut hints' search, header only), `WindowTitle.h` (the
  window's title: the file's path from the folder, ~ outside it, the
  terminal's or the page's title while it has the keyboard; header only),
  `SidebarPanels.h` (the sidebar's row of panel icons: names, toggles,
  tips, badges, the panel on show and a click's toggle; header only), `SymbolOutline`
  (symbols without a server), `NavHistory` (Back and Forward), `InlineAgent`,
  `ChangeRequest`, `ControlProtocol` (the control channel and the command
  line's parsing), `SideBySide` (diff rows), `PageCheck` (agents checking
  pages: addresses, the origin limit, the console record).
- `ProcessEnv` (the environment as an immutable snapshot and a child's
  explicit envp, so threads that start programs never read `environ`;
  docs/notes/build.md).
- `Rename.h` (header only): every name from before the rename to mutible
  (1.6.0) that is still read (MINICODE_* variables, `minicode.*` command <!-- rename-mutible: keep -->
  ids, an old copy's socket folder, the old hook command and ideName), and
  the first launch's migration paths and rules; `scripts/rename-mutible.sh`
  skips it, so the old names stay.
- Agent work: `AgentTranscript`, `AgentIndex`, `AgentEvidence`, `AgentView`,
  `AgentTask` (New Agent Task's names, worktree place and command line),
  `LineHistory` (blame porcelain, the lines a session's edits wrote, each
  line's answer), `ClaudeUsage` (the plan usage forecast: transcript
  tokens, prices, blocks, burn rate, status line samples, the fit) and
  `ClaudeStatusLine` (`mutible statusline`: installing it over the user's
  status line, undo, the log);
  `ProcessTable` (processes and ports per folder: tracking, groups, Stop's plan).
- Files: `FileSniff` (how a file is shown), `Csv`, `JsonOutline`, `Notebook`,
  `SqliteRead` (a SQLite database read-only, with a deadline: tables and a
  page of rows; the system libsqlite3, the one library the core links),
  `WebBookmark` (web bookmarks as .url and .webloc files: reading, writing,
  names, which addresses may open).

### The Mac app (`mac/src`, Objective-C++)

- `main.mm`: `AppDelegate`, the menus, windows, the open handler, and the one
  local key monitor (which asks `MCKeymapHandleKeyDown` first).
- `EditorController.{h,mm}` and its categories (below); `EditorTab` (`MCTab`),
  `EditorPanes` (`PanelHost`, `DragBar`, `PanelScrollView`), `FileTree`
  (`FileItem`, `ClickOutline`, `gShowHidden`), `EditorPage.h`.
- The editor: `SourceStorage` (`MCSourceStorage`), `PieceString` (a large
  file's text held once: `MCPieceString`, its snapshots, and the attributed
  string over it), `WrapIndentContainer` (the
  text container that sets a wrapped line's continuation in), `CodeTextView.mm`, `Gutter`,
  `FoldView`, `SpellMarks` (spell checking's marks and menu items),
  `EditorFont`, `EditorFind`, `FindWidget` (the find bar's and
  Find in Folder's colors, icon buttons and inputs), `MultiCaretView`, `EditingCommands`
  (AutoIndent and KeepSorted on an NSTextView), `MarkdownCode` (code colors in
  previews), `TestGutter` (the ▶ beside each test).
- Language servers: `Lsp.mm` (`LspSession`, one per window, split into
  categories as EditorController is: `Lsp+Servers`, `Lsp+Python`, `Lsp+Sync`
  (documents and watched files), `Lsp+Diagnostics`, `Lsp+Completion`,
  `Lsp+Navigation` (definition, hover, symbols), `Lsp+Actions` (rename,
  references, code actions), `Lsp+Semantic` (semantic tokens),
  `Lsp+WorkspaceEdit`, `Lsp+InlayHints`, `Lsp+JsonCheck` (JSON no server
  has, checked by the editor); `Lsp+Private.h` and the
  `Lsp+<Concern>Ivars.h` follow the rules below), `LspServer`
  (processes), `LspPopups` (completion, hover, signature), `SnippetSession`
  (a snippet's tab stops in the view), `LspShared.h` (only
  the Lsp files import it), `LspPanel` (status item, popover, interpreter
  sheet), `LspPicker` (references and fixes list, rename box), `LspWatcher`
  (files changed on disk, for the servers), `LspInlayHints` (drawing inlay
  hints in the storage's gaps), `ProblemsPanel`, `InstallPanel`.
- `Environment` (`MCEnvironment`, `MCEnv`, `MCSetEnv` over the core's
  `ProcessEnv`: every child's environment, on any thread).
- `Migration` (the first launch since the rename: copies Application
  Support, ~/.config, WebKit's data and the defaults domain from
  minicode's names, once, by `Rename.h`'s rules). <!-- rename-mutible: keep -->
- `AppSettings` (with the theme picker's preview, `previewTheme:`),
  `Appearance` (Adjust Settings, Theme menu, opacity popover),
  `ThemePickerPanel` (the theme picker in the palette), `Keymap`.
- Terminal: `Terminal` (`TerminalView`, `MCPty`, the shell integration),
  `TerminalGridView`, `TerminalPanel`, `TerminalFindBar`, `RunCommand`,
  `HoverHint` (the link hint beside the pointer, a subview, since AppKit's
  toolTip set after the pointer rests never shows).
- Viewers: `Latex`, `MarkdownMath`, `MarkdownMermaid`, `MarkdownImage`, `MarkdownMedia`, `MarkdownEditPanel`,
  `FileViewers`, `QuickLook`, `CsvTable`, `JsonOutlineView`, `SqliteView`,
  `HtmlPage`, `CompareView`, `MarkdownSidePreview` (Markdown's page beside
  its source), `PageSidePane` (the split for a LaTeX or HTML page beside
  its source).
- `EditorMirror` (the split editor's group without the keyboard).
- `Browser` (with `MCKeyboardWindow`), `Bookmarks` (the bookmark folder,
  reading and writing bookmark files), `TabStrip`, `Search` (Find in Folder and
  TODOs window), `GitPanel`, `ChangeReviewPage` (every uncommitted change in
  one tab), `GitGutter` (change bars), `StatusBranchItem` (the status
  bar's branch and counts), `EditorConflicts` (merge conflicts
  in the editor), `TreeDetails`, `FileTypeIcons` (a file's icon and
  color by its `FileIcon::Type`, for the tree and Find in Folder), `TreeFocusBar` (the focused tree's
  breadcrumb row, or its label row for several folders),
  `FormatRunner`, `BackupStore`, `SessionStore` (sessions.json),
  `SessionLink` (the session's hosts, the process's and each folder's
  headless one, and each tab's document on them and its saves through
  them, editor.session-host;
  `docs/notes/session-host.md`), `SessionWindow` (a window's link, record
  and what it took from a closed window's, stage 6), `RemoteWorkspace`
  (a window over SSH's folder through its host: the tree's `MCTreeSource`,
  the host's disk notices, Quick Open's list, Find in Folder, new files;
  stage 8), `TypesetPages`
  (the session web page's LaTeX pages drawn as PNGs with CoreGraphics,
  off the main thread; `docs/notes/session-host.md`),
  `PalettePanel`, `Navigation`, `HintsSearchBox` (the hints panel's search box),
  `SidebarIcons` (`MCSidebarHost`, the split view's first subview, and its
  row of panel icons, workbench.sidebar-icons).
- Agents: `AgentPanel` (Cmd+I), `ChangePanel` (Request a Change),
  `ControlServer`, `ControlCommands`, `IdeLinkServer` (Claude Code's /ide),
  `ClaudeHooksOffer` (the waiting dots' hooks, offered in the app),
  `StatusLineOffer` and `UsageView` (the plan usage forecast in the
  Agents panel, and its status line log's install and undo),
  `AgentNotes`, `AgentModel`, `EditorLineHistory` (Show Line History),
  `AgentsPanel`, `AgentPages`, `AgentWindow`, `AgentTaskBar` (New Agent
  Task), `ProcessMonitor` (libproc,
  `mutible procs`), `ProcessesPanel`.
- `Demo` (`make demos`, inert unless `MUTIBLE_DEMO`), `Bench`
  (`MUTIBLE_BENCH`; the Linux port's is `linux/src/Bench.cpp`).
- Elsewhere: `tools/mutible-cli.cpp`, `scripts/mutible-launcher`,
  `tools/makegif.m`, `tools/procmem-mac.c`, `tools/procmem-linux.c`, `tools/taskbench-mac.m`,
  `scripts/membench-mac.sh`, `scripts/membench-linux.sh`, `scripts/taskbench-mac.sh`, `skills/mutible/SKILL.md`,
  `tests/fixtures/claude`, `mac/tests/gui`, `tests/latex`, `tests/markdown`,
  `tests/keepsorted`, `site/` (the homepage, `docs/notes/release.md`).

### EditorController and its categories

`EditorController.mm` keeps init, dealloc, the window delegate,
`validateMenuItem:`, undo and redo, and the forwarding accessors of the active
tab. The rest is in `EditorController+<Concern>.mm`: Layout, StatusBar, Panels
(terminal dock, browser, keyboard between panes), Search, Tree, TreeDetails,
Tabs (tabs, closed tabs, saving), Session (the session host's hooks), Remote (a window over SSH's tree, Quick Open, Find in Folder and new files through its host, and what it refuses there), Groups (the split editor, and the window's Model::Window behind both), Persistence (sessions, backups, auto-save,
files changed outside), PageTabs (the review, commit and comparison tabs
as sessions keep them, made again on restore), TreeFocus (Focus on Folder and Add to Focus), TreeReveal (the tree following the active tab), WindowTitle (the window's title and representedURL, by who has the keyboard), Rendering (what the editor's slot shows), Highlighting,
Settings, Format, EditorView (line numbers, brackets, zoom, comments, tasks,
editing hooks), Bookmarks (web bookmarks, the browser window), Navigation,
LanguageServers, Agents (inline agent, Request a
Change, control channel, agent work), Processes (the Processes panel's
hooks, its state in associated objects), SidebarIcons (the sidebar's row of
panel icons: what each shows, a click's panel; no ivars, the host is the
`sidebarHost` property), LargeFile (large-file mode, its
status bar note, the 1 GB question), Tests (the ▶ beside each test, Run
Test at Cursor), LineHistory (Show Line History's host), Markdown and
MarkdownRender, LatexInputs (a LaTeX document's \input files, as its
preview edits them), and PageSide (Open Preview to the Side for LaTeX and
HTML files, over `PageSidePane`). Rules, so it
stays warning-free:

- `EditorController+Private.h` holds the class extension (ivars in its ivar
  block, private properties), each category's private methods in an `@interface
  EditorController (Concern)`, and helpers several files share as `static
  inline` (so a file that does not call one is not warned). Only
  EditorController files import it. Shared non-window state is a global defined
  in one file (`gShowHidden` in FileTree.mm), never a static in a header.
- **Ivars (2026-11-06): the shared block is closed to one category's state.**
  It holds only variables two or more category files use; one used by a
  single category goes in that category's own class extension,
  `src/EditorController+<Concern>Ivars.h` (`@interface EditorController ()
  { ... }`, importing `+Private.h`), imported by that category's `.mm` and by
  `EditorController.mm`, whose `@implementation` must see a class extension
  for its ivars to exist (miss it and the link fails on `OBJC_IVAR_$_...`).
  Make the header when the category has none yet. Better still, a feature in
  files of its own keeps its state in its own object, reached through one
  property or ivar (`MCEditorFind`, `MCFolds`, `MCAgentSession`). The
  `structure:ivars` core test fails on a shared variable fewer than two
  categories use, an ivar header `EditorController.mm` does not import, or a
  category using another's ivars. LspSession is split the same way
  (2026-11-07: `Lsp+Private.h`, `Lsp+<Concern>Ivars.h`, `Lsp.mm` laying them
  out) and the same test checks it; a category reaches another's state
  through a method declared in `Lsp+Private.h` (detachDocument asks each
  category to forget its requests), never its ivars.
- Methods the rest of the app calls go in `EditorController.h` in an
  `@interface EditorController (Concern)` named for the implementing category:
  a method the class declares but a category implements draws warnings. For the
  same reason host protocols (MCChangeHost, MCAgentHost, MCAgentNotesHost,
  MCAgentWorkHost, MCNavigatorHost) are adopted by their categories, not the
  class extension.
- A KVO context and its observer share a file (the player's, in +Rendering).
- No new weight: a feature goes in its own files and classes, and
  EditorController gains only hooks.

## Traps

None recorded under a "Trap" label; read the sections above.
