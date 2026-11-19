# Language servers

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## LSP (read before touching it)

The protocol is the core's `LspClient.h`, tested against a scripted server.
Its code is split by concern (2026-11-08), each file's opening comment
saying what it holds; a new request adds its parser to the file for its
kind, its method and capability entry to `LspClient.cpp`:

- `LspClient.cpp`: the `Client` class only (initialize and the
  capabilities offered, document sync, requests, progress, registrations,
  server requests, shutdown).
- `LspProtocol.cpp`: framing, `Reader`, `progressText`.
- `LspText.cpp`: positions, `PositionIndex`, UTF-8/UTF-16, URIs, `lineOf`.
- `LspServers.cpp`: languages, commands, the second server, `offers`,
  routing, the status line, Python servers, `configurationSection`.
- `LspDiagnostics.cpp`: publishes, `diagnosticSpans`, `DiagnosticStore`,
  Problems rows, `diagnosticsInRange`.
- `LspCompletion.cpp`: completion (parse, resolve, filter, accept, late
  edits) and signature help.
- `LspFeatures.cpp`: hover, folding, semantic tokens, locations and
  references, symbols.
- `LspEdits.cpp`: WorkspaceEdits, formatting results, prepareRename, code
  actions.
- `LspSync.cpp`: `ChangeTracker`, ranged contentChanges, watched files
  (the copy of each file is `Model::Text`, `src/Text.cpp`).
- `LspInternal.h`: `lower` and `trim`, for these files only.

Every port's build list names all of them (`linux:build-lists` checks that a
list naming `LspClient.cpp` names every file implementing `LspClient.h`). On
the Mac
`Lsp.mm` and its `Lsp+<Concern>.mm` categories (`LspSession`; "Layout of the
code" lists them), `LspServer.mm`, `LspPopups.mm` and `CodeTextView.mm` own
processes and UI; the GTK port has `linux/src/Lsp.cpp` over the same core.
GTK trap: a popover parented to a `GtkTextView` must be unparented before the
view is disposed (on `unrealize`), or dispose loops forever.

- **Hooks are few**: `documentOpened:` (nil for messages, binaries and
  previews), `documentSaved`, `setRoot:`, `shutdown`, `openPaths` and
  `bufferText` (every tab's text), `tabsChanged`, and
  `textView:doCommandBySelector:` to `handleCommand:`. The session observes the
  storage itself; didChange is debounced 0.3 s, every request flushes first,
  and unchanged text is dropped. A server with `textDocumentSync.change` 2
  gets ranges: `Lsp::ChangeTracker` (`_changes`) merges the storage's edits
  (`editedRange`, `changeInLength`) into regions of the text as sent, and
  the Mac's flush (2026-11-08) hands `Client::didChange` the length and a
  `TextReader` over the storage: only each region's new text (and a unit
  either side) is read, positions come from the client's copy, a
  `Model::Text` (`SyncedText` until 2026-11-16; UTF-16 in chunks of about
  1,024 units, each with its own line starts, lengths, line counts and
  code points in Fenwick trees, so an edit rewrites only the chunks it
  touches in log time; one flat string moved its tail per edit,
  `lsp:sync-scale`), and
  contentChanges go last region first. The regions are trusted, so a pause
  costs what was typed, not the file (9 ms of main at 2.5 MB before);
  lengths that do not add up, a boundary inside a surrogate pair or
  between \r and \n, over 256 regions, or ranges as big as the text send
  the whole text, and every save compares the whole text (didSave), which
  repairs a copy an edit the tracker missed made wrong.
  `MUTIBLE_LSP_SYNC_CHECK=1` compares after every flush (`setCheckSync`,
  counted in `syncRepairs`), and `lsp:sync-oracle` checks the copy against
  a server applying what it is sent. **The GTK port counts characters**
  (2026-11-16): its tracker's regions, length and reader are code points,
  and `didChange(..., Model::Unit::CodePoint)` turns them into UTF-16
  through the copy's code point tree, the new text's neighbours read from
  the copy (so only the regions' own text is read), checked by
  `lsp:sync-code-points`; until then a character outside the BMP made each
  of its sends the whole text, converted and compared. The UTF-8
  overloads (tests) still compare the outside text with `rangedChanges`
  first. `textFor` is the
  copy as UTF-16, made on first asking and shared until the next edit.
- **Every tab's file stays open** on its server (`syncOpenDocuments`: on show,
  server start, before rename and references, on `tabsChanged`); only a running
  server takes background tabs, so a tab never starts one. With only the file
  on show open, rename missed unsaved uses elsewhere and the server read
  edited-but-unsaved files from disk. A tab is converted and compared only
  when its text is not what it last gave (2026-11-08, `MCLspDoc sentText`
  and `live`): a background tab's `sourceText` is immutable, so the same
  object is the same text; the file on show was sent whole when attached
  and follows by flushChanges. It had converted every tab whole on every
  call (`make guitest SUITES=lsp_open_tabs`).
- Requests during the handshake are queued; server-to-client requests are
  always answered (nulls for configuration, -32601 for unknown methods).
- **Watched files** (2026-11-06): `client/registerCapability` for
  `workspace/didChangeWatchedFiles` is kept (others are answered null and
  ignored), and `MCLspWatcher`, made with the first registration, sends
  what the servers' globs want (`Lsp::globMatch`, RelativePatterns below
  their base): its own FSEvents stream with file events on the folder and
  the patterns' folders outside it (basedpyright registers `**` and its
  site-packages, Ruff its config files), files open in tabs included,
  `.git`, `.hg/store` and node_modules' packages left out. The capability
  is offered only by an owner that sends (`setFileWatching`, the Mac): told
  the client watches, rust-analyzer stops watching itself, so the GTK port
  does not offer it. Trap: FSEvents keeps a file's created flag on its
  later events, so a path already reported is Changed whatever the flags
  say (`Lsp::FileEventClassifier`). Batches are matched on the watcher's
  queue (2026-11-08): `updateFileWatcher` gives it each server's
  `Client::watchTarget()` with the bases' resolved spellings
  (`setTargets:`), `Lsp::prepareWatched` builds the framed notification
  there, and main only writes it (`Client::sendWatchedBatch`, refused when
  `watchersGeneration` moved on since the copy; then main matches as
  before). 20,000 changes against 12 watchers took 47 ms of main; now
  under 2 (`lsp_watch`).
- **Lookup** (`MCFindProgram`): PATH plus Homebrew, Cargo, Go, ~/.local/bin,
  npm's prefixes, nvm and fnm (newest first), volta, pnpm, bun, pip --user,
  pyenv, MacPorts, /usr/bin and keg-only llvm, since a Finder-launched app has
  a short PATH. `/usr/bin/clangd` and `/usr/bin/git` are xcrun shims that pop
  the install dialog: never run them; `MCDeveloperTool` finds the real binary
  through /var/db/xcode_select_link. The child's PATH puts the server's own
  folder first (an nvm script's `#!/usr/bin/env node` then gets nvm's node;
  Homebrew's node was broken here).
- **Settings**: `lsp.enabled`, `lsp.<cpp|python|go|rust|typescript>` (a command
  line or `off`), `lsp.<lang>.also` (auto, off, a command),
  `python.interpreter`. Any server by extension (Mac, 2026-11-06):
  `lsp.<name>` plus `lsp.<name>.extensions` (`Settings::lspExtensions`, the
  name is the languageId; one without the other is an error on its line), and
  the optional built-ins latex (texlab), markdown (marksman), toml (taplo) and
  shell (bash-language-server), which count for a file only when installed or
  named (`LspSession language:forPath:installed:`), so no status line or
  install bar for them; `lookAgainForMissingServer` picks one up when it
  appears. The Mac maps with the three-argument `Lsp::languageForExtension`;
  the GTK port keeps the five. A user's own server never gets an install bar
  (`missingServer` wants `Install::server`). Python tries basedpyright, pyright, pylsp,
  jedi-language-server; basedpyright gets `typeCheckingMode = standard` unless
  the project sets it. A change restarts the servers.
- **States** (`MCLspEntry` per language and second server) show in
  `MCLspStatusField` and the Language Servers popover. Exits restart after 1,
  2, 4 s; the fourth crash in a row (three minutes up clears it) stays Crashed.
  The popover lives in a static (`gServersPopover`), so it holds sessions
  weakly (a strong capture kept a closed window's session alive).
  `$/progress` (`window.workDoneProgress`, `Client::progress`): the newest
  work begun on a running server stands in for "no problems" in
  `Lsp::statusLine` and is a line on the server's card; redraws coalesce to
  one a run-loop turn.
- **Processes** (`MCLspServer`): pipes; stdout is framed and parsed by an
  `Lsp::Reader` on the server's own serial read queue and only the parsed
  messages hop to the main thread (`Client::deliver`), where the Client
  stays single-threaded, and are freed back on the read queue (a 6 MB
  answer parsed on main stalled it 50 ms); writes on
  a serial queue with raw `write()` and `F_SETNOSIGPIPE`; stderr drained (a
  full pipe stalls clangd); `$MUTIBLE_LSP_LOG` logs all traffic. Window close:
  shutdown, exit, stdin closed, SIGTERM at 2 s, SIGKILL at 3; quit:
  `MCLspTerminateAllServers` with 0.5 s; a crash closes stdin, which ends them.
  `LspSession dealloc` cancels performs scheduled on itself (it crashed 50 ms
  after a window closed).
- **The second server**: `Lsp::defaultAlsoCommand` runs `ruff server` for
  Python in a project configuring Ruff anywhere up to the root or `.git`
  (`Formatter::configFile`), when ruff is found (the project's .venv first). It
  is its own entry ("python.also"). `_servers` per file, main first; `_docs`
  keyed by `DocKey`. `Lsp::routeRequest` sends one-server requests (completion,
  hover, signature, definition, references, rename) to the main server, also
  while it starts, else one that offers it. Diagnostics per server in
  `Lsp::DiagnosticStore`, tagged "(Ruff F401)"; code actions merged
  (`Lsp::mergeCodeActions`, 3 s); Organize Imports (Shift+Option+O) prefers a
  tool's `source.organizeImports.<tool>` (`pickOrganizeImports`); basedpyright
  offers none. Traps: Ruff's diagnostic source is "Ruff", its server name
  "ruff". Editing ruff.toml reaches Ruff through watched files, with no
  restart (`lsp_watch` checks it where Ruff is installed).
- **Python interpreter**: the folder's choice (`MCPythonInterpreters`),
  `python.interpreter`, the nearest .venv/venv/env, `$VIRTUAL_ENV`,
  `$CONDA_PREFIX`, else the server's own (/usr/bin/python3 skipped). Sent only
  when it differs: `python.pythonPath` by workspace/configuration for the
  pyrights, `pylsp.plugins.jedi.environment`, jedi's `initializationOptions` (a
  restart). **An environment inside the folder needs the user's yes**
  (2026-11-06, review 3): a server runs the interpreter it is given and is
  looked for in its bin, so `serverPythonInterpreterWithSource:` leaves the
  .venv out until the folder's answer (`MCPythonEnvironmentConsent` in
  `MCAppDefaults()`) is "allow"; the install bar asks first (Use It, Don't Use
  It, × for this launch). Run File, which the user starts, still uses it
  (`pythonInterpreterWithSource:`). **The bar asks only when something a
  person would notice waits** (2026-11-15, round 8's C1): basedpyright
  reads a root .venv's site-packages by itself, running nothing, so its
  completions and errors worked before any answer and Use It changed
  nothing visible. `FolderTools::environmentBarQuestion` (the server from
  `LspSession folderEnvironmentServer`: nil before one is tried, "" when
  none runs) gives "" then, and otherwise says what the server is missing;
  the Language Servers popover offers Allow and Don't Allow while the
  folder has no answer (`environmentPopoverText`). Linux's bar keeps the
  old words. Format Document shares the answer (see
  "Formatting"). mutible > Ask Again About This Folder's Tools (and Ask
  Again in the Language Servers popover, which sends the same action) takes
  it back (`MCForgetFolderToolsConsent`) and clears a × put-off, so the bar
  asks at once (`make guitest SUITES=tools_consent`).
- **Servers that run the folder's code** (2026-11-08, `Lsp::checkProjectCode`,
  `lsp:project-code`, `make guitest SUITES=server_consent`) take the same
  answer. Until it is "allow", rust-analyzer starts with build scripts, proc
  macros and checkOnSave off (initializationOptions and the
  `rust-analyzer` section), and does not start at all (`MCLspStateWaiting`)
  when the crate's folders or the window's (up to, not including, home)
  hold a `.cargo/config[.toml]`, `rust-toolchain[.toml]`,
  `rust-analyzer.toml` or `rust-project.json`: `cargo metadata` runs the
  rustc and rustc-wrapper a cargo config names, and rustup's proxies run a
  toolchain named by path (rust-analyzer itself, from ~/.cargo/bin). The
  TypeScript server finds the folder's node_modules/typescript first, so it
  gets `tsserver.path` to the user's own, or waits. The bar asks (Allow,
  Don't Allow, the same × put-off); an answer restarts them. clangd (does
  not load compile commands' plugins) and gopls are left alone.
- **Completion** opens after `.`/`->`/`::` and on a word's second letter; an
  `isIncomplete` list is asked again. `Lsp::completionKeyAccepts`: Return
  and Tab take the selected item whenever the list shows (VS Code's
  acceptSuggestionOnEnter "on", the owner's report 2026-11-08: Return on a
  list that came up by itself and on a word typed out were new lines);
  Shift+Return and Shift+Tab never take. A key pressed before the list was
  filtered for the last letter (`_filteredFor`) refilters first: "pro" and
  Tab in one go took "print". The letter typed is the storage's own record
  (`MCSourceStorage singleInsertedCharacter`), never the notification's
  editedRange, which also covers attributes set in the same session: a
  letter typed between brackets read as "r)\n" changed and the list never
  opened there. `make guitest SUITES=completion_keys` presses
  real keys, under basedpyright too where installed. `filterCompletions` ranks
  exact, same-case prefix, any-case prefix, subsequence (sortText within) and
  reselects the first row unless the arrows chose an item still listed. Docs
  show beside the list after `completionItem/resolve` (80 ms).
  **Snippets** (2026-11-08, `snippetSupport` on): an item with
  insertTextFormat 2 (or the list's itemDefaults) is expanded by
  `Snippet::expand` (variables, mirrors, the line's indentation unless
  insertTextMode 1, the file's unit and line break) before
  `completionEdits`, so it stays one undo step; `MCSnippetSession`
  (`CodeTextView snippets`) then follows the stops, each replacement as
  `MCSourceStorage onReplaceCharacters` reports it (trap: the editedRange
  of NSTextStorageDidProcessEditing is the union of an editing session's
  changes, attribute fixes included, so a letter typed over a stop could
  read as the whole line replaced and end the session; seen only in a
  whole `make guitest`) (`Snippet::Session`:
  the current stop and those around it grow at their edges, a nested stop
  typed over from around is skipped). Its Tab, Shift+Tab and Esc come first
  in CodeTextView's `doCommandBySelector:`, before the carets and
  AutoIndent, except that `commandTakenFirst` lets a showing list keep
  them (a choice stop's options are the completion list in `_choiceMode`).
  Mirrors are carets (`setCaretRanges:`). It ends at $0, on Esc, an undo or
  redo, a selection outside the stop (checked a turn later), or a
  replacement swallowing the stop. `$CLIPBOARD` reads the pasteboard only
  when the snippet names it (`Context::readClipboard`), and placeholders
  nest 64 deep at most (`Snippet.cpp` kMaxDepth; deeper is text as
  written; bad input parses in linear time). Python's servers are told to send bare
  names (pylsp include_params and jedi's disableSnippets off;
  basedpyright's completeFunctionParens stays off). Trap: the session's
  boxes are invalidated a run-loop turn after a storage edit; asking the
  layout manager for rects inside NSTextStorageDidProcessEditing left it
  broken, and the next firstRectForCharacterRange threw.
  **Accepting** inserts the word and `additionalTextEdits` (imports,
  `#include`) as one multi-range replacement (`Lsp::completionEdits`), its
  undo step an `MCAcceptUndo` of its own. When the server sends the edits
  only from `completionItem/resolve`, the word goes in at once and the edits
  when the answer comes (`kResolveAfterAccept`, 1 s, then a note), placed by
  `Lsp::lateAdditionalEdits` only before everything edited since: in the
  same undo step when nothing changed meanwhile, else a step of their own
  (VS Code's rule). Never a nested run loop: the old 0.4 s wait ran the main
  queue inside the key handler. A big list is parsed and first filtered on
  `CompletionQueue()` (the handler takes the result, `Client::TakingHandler`)
  and freed there; the list's panels are built ahead (`prepareStep`, a piece
  per run-loop turn after a server is ready) and the docs show a turn later.
  Signature help (`(`, `,`) sits above the line. Both close on focus loss, key
  loss and tab switch. Trap: the list's `close` must `reloadData` after
  emptying its labels, or layout asks for a vanished row.
- **Diagnostics are drawn, not attributed**: squiggles after `super drawRect:`
  for the viewport, from marks (NSRanges that follow edits). Kept per open file
  with the text they were computed on; `diagnosticSpans` places them on the
  text as it is now, leaving out ones about text typed over. Publishes for a
  version older than one since sent are dropped (versions count on across a
  close and reopen); `Client::textFor` gives a publish's text. Tags: an
  Unnecessary one is faded (no squiggle for a hint), a Deprecated one struck
  through, a plain hint three dots. Positions convert through one
  `Lsp::PositionIndex` per text, never a scan per position. Workspace edits
  carry the server's document versions from when they were asked
  (`Client::versions`, `staleEditFile`) and are refused for a file that moved
  on. Files no tab has open are kept apart (`_otherDiags`) for
  Problems. A diagnostic `Lsp::drawsOnText` refuses (clang's
  `fatal_too_many_errors`, which clang places at the file's first
  character, so it sat on a line-1 comment) is an `MCDiagnosticMark
  quiet`: counted in the status bar and listed in Problems, but no
  squiggle, gutter dot, hover or F8 stop.
- **clangd without compile flags** (2026-11-16, `Lsp::clangdHasCompileFlags`,
  `lsp:clangd-flags`, `make guitest SUITES=clangd_flags`): no
  compile_commands.json or compile_flags.txt from the file's folder up
  (each folder and its `build/`, as clangd looks), no `.clangd` with a
  CompileFlags section, no `--compile-commands-dir`, so clangd guesses and
  missing-header errors may be false. clangd's card in the Language
  Servers popover says so (`MCLspServerInfo compileFlagsNote`,
  `Lsp::kClangdNoFlagsText`) while it holds, and the status bar once per
  folder a launch (`noteMissingCompileFlags`, from `documentOpened:`);
  never a bar. Checked with stats, not clangd's log.
- **Inlay hints** (2026-11-07, `Lsp+InlayHints.mm`, `LspInlayHints.mm`,
  core `InlayHints.{h,cpp}`, `lsp:inlay-*`, `make guitest
  SUITES=inlay_hints`): VS Code's editor.inlayHints. Asked for the lines on
  show and 30 either side (the first line counted by `LineBreakCounter`
  in pieces, no copy of the text before), when the file attaches or a
  server becomes ready, 0.3 s after typing (the request out is cancelled
  by any edit, and an answer for older text is dropped), when scrolling
  leaves the window asked about, and on `workspace/inlayHint/refresh`.
  Only a Ready server that offers them is asked. `shortenInlayLabels` is
  VS Code's maximumLength 43 per line. Parameter-name hints (kind 2) are
  dropped unless `editor.inlay-hint-parameter-names` (off; the owner's
  call, 2026-11-14: they made lines too long, and no formatter counts
  them; `Lsp::dropParameterHints`). A hint after a word follows it when
  typing lands right at it; one before an argument stays put
  (`followsBefore`). A hint at a line's start goes with what follows it and
  is a line-start gap (see "Gaps"); only one at the start of an empty last
  line is left out. basedpyright gets its `inlayHints` settings named, as
  its VS Code extension's defaults (genericTypes on, callArgumentNamesMatching
  off). `editor.inlay-hints` (on, off, on-unless-pressed, off-unless-pressed:
  Control+Option held, one app-wide flagsChanged monitor posting to the
  sessions; `MCInlayHintsSetPressed` for checks; hidden is cleared, shown
  asks again), View > Inlay Hints (`MCInlayHintsMenuItem`, writes the
  setting), `editor.inlay-hint-color`, `editor.inlay-hint-background`. The
  label is drawn after the text in `drawRect:`, at the glyph box's right
  edge minus the gap (the box includes the kern); a null (folded) glyph gets
  none. **The caret beside them** (the owner's report, 2026-11-08): a click
  never asks the view's `characterIndexForInsertionAtPoint:` but the layout
  manager's `characterIndexForPoint:...fractionOfDistanceBetweenInsertionPoints:`,
  so the editor's is `MCGapLayoutManager` (SourceStorage.h), which measures
  a point against the glyph's own width and takes the room as past it (a
  click on a hint's left half put the caret before the word's last letter).
  The caret is drawn where typing goes, left of a `followsBefore` room
  (`MCInlayHints caretShiftAtOffset:`): CodeTextView moves the
  NSTextInsertionIndicator after each `updateInsertionPointStateAndRestartTimer:`,
  undoing its own last shift when AppKit left the indicator alone (macOS 14
  and later; earlier systems keep AppKit's place). A double-click on a hint
  applies its `textEdits` (`Lsp::inlayHintAcceptEdits`: sorted, the import
  too, refused when overlapping; after typing only a hint inserting at
  itself, at its moved offset) through `replaceRanges:` as one step, "Insert
  Inlay Hint".
- **Problems** (Shift+Cmd+M, `MCProblemsPanel`, above the terminal):
  `problemRows` (files with errors first, then severity, line, column),
  `countProblems`, `problemCountsText`, `problemOneLine`, `stepProblem` (F8 and
  Shift+F8, wrapping, through several at one place, the message in the hover
  box). Hints are left out (pyright sends one per unused name).
  `onProblemsChanged` is debounced 50 ms; `problemsLabel` toggles it; Cmd+W and
  Esc work in it.
- **Hover**: 0.4 s on a non-blank character (`characterIndexUnderPoint:` checks
  the character's own box) shows `MCHoverPopup`, a non-key child panel,
  diagnostics first, Markdown asked for first (the pyrights sent plain text
  otherwise). Option+Cmd+I shows it at the caret.
- **Definition**: dropped unless generation, document and selection are as
  asked (`detachDocument` bumps them). Targets are respelled in the root's
  form. Trap: `stringByResolvingSymlinksInPath` turns /private/tmp into /tmp;
  use realpath for URIs. `initialize` gets the resolved folder (basedpyright
  found every reference twice across /tmp and /private/tmp) and `CanonicalURI`
  merges spellings.
- **The popups' colors** (2026-11-18, the grading walker on 1.6.3: a
  dark references list on the light theme): the hover box, the
  completion list and its documentation, the signature hint, the picker
  and the rename field read `PopupStyle()` (LspShared.h, which is
  `MCFindStyle current`, FindWidget.h) at each showing, so a theme change
  shows next time: editorWidget.background for the windows (VS Code's
  editorHoverWidget.* and editorSuggestWidget.* default to it),
  `widgetEdge` for borders, widget foreground and `muted` for text,
  `selection` for the lists' selected row (`MCPopupRowView`, LspPicker.h,
  which leaves the row's text in its own colors), `listHighlight` for the
  argument being typed, `textLink` for links, the theme's syntax string
  color for inline code, the find match highlight on a reference's name,
  and input.* with focusBorder for the rename field. Checked in
  lsp_fixes's last part, dark-modern and light-modern, each popup's
  window background against the theme's (pictures under
  MC_GUI_SNAPSHOTS, `lsp-popups-<theme>-<popup>`).
- **Rename, references, quick fixes** (F2, Shift+F12, Cmd+.): prepareRename
  when offered ("This can't be renamed"); `MCRenameBox` is a key-capable child
  panel (an NSPopover never shows for a window not on screen) over the name
  itself (2026-11-18; it was a box under the line that hid the next line's
  code): the field is the line's height from just above its bottom edge, in
  the editor's font with the old name selected, its text where the name's
  is, the border 2 points above the line, and "Return to rename, Esc to
  cancel" goes to the status bar (`flash:`); lsp_fixes checks the field's
  frame meets the name's line and not the next; references and
  fixes share `MCPickerPopup`, which never takes the keyboard (keys through
  `handleCommand:`). Fixes send the diagnostics touching the caret, else the
  caret line's; an action without its edit is resolved, its edit applied, then
  its command run (edits the server sends meanwhile are undone under its
  title). Core: `parseWorkspaceEdit` (changes and documentChanges merged;
  overlaps and file operations refused whole), `offsetEdits`,
  `groupReferences`, `parseCodeActions`, `diagnosticsInRange` (as sent,
  `Diagnostic::raw`), `onApplyEdit`.
- **The workspace edit applier** (`applyWorkspaceEdit:actionName:failure:`,
  `applyEditsToPaths:`) changes every file or none: read and check each
  (editor, `bufferText`, or disk; positions checked against what the server was
  sent, `Client::textFor`; a dirty tab only if each range holds the disk's
  text), open each and check it shows as that text, then edit each as one
  multi-range replacement (one undo step per file). A refusal after edits puts
  them back with `undoNestedGroup` (a key press's own group left alone) and
  closes tabs it opened (`closeFile`). Nothing is saved. `_applyingEdit` keeps
  completion, signature help and the rename box out.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- LSP (read before touching it): GTK trap: a popover parented to a `GtkTextView` must be unparented before the view is disposed (on `unrealize`), or dispose loops forever.
- LSP (read before touching it): Trap: FSEvents keeps a file's created flag on its later events, so a path already reported is Changed whatever the flags say (`Lsp::FileEventClassifier`).
- LSP (read before touching it): Traps: Ruff's diagnostic source is "Ruff", its server name "ruff".
- LSP (read before touching it): ... (trap: the editedRange of NSTextStorageDidProcessEditing is the union of an editing session's changes, attribute fixes included, so a letter typed over a stop could read as the whole line replaced and end the session; seen only in a whole `make guitest`) (`Snippet::Session`: the current stop and those around it grow at ...
- LSP (read before touching it): Trap: the session's boxes are invalidated a run-loop turn after a storage edit; asking the layout manager for rects inside NSTextStorageDidProcessEditing left it broken, and the next firstRectForCharacterRange threw.
- LSP (read before touching it): Trap: the list's `close` must `reloadData` after emptying its labels, or layout asks for a vanished row.
- LSP (read before touching it): Trap: `stringByResolvingSymlinksInPath` turns /private/tmp into /tmp; use realpath for URIs.
