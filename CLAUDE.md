# CLAUDE.md — working notes for MiniCode

MiniCode is a small native macOS code editor built from scratch, no Electron,
no third-party dependencies. It links only Apple system frameworks (Cocoa,
WebKit, CoreServices, Quartz for PDFKit) plus the C++ standard library and the
system zlib. The LaTeX preview runs tectonic, an external binary the user
installs or the app downloads on request, and the LSP client talks to
language servers the user has installed in the same way. This file is the handoff
for future sessions: architecture, workflow, and the hard-won gotchas.

There is also a GTK4 Linux port under `linux/` and an Android port under
`android/`, both sharing the portable C++ core verbatim. They have their own
handoff notes in `BUILD-LINUX.md`, `linux/README.md` and `android/README.md`;
this file covers the macOS app except where it says otherwise.

## Build / test / run / release

- `make` — build `MiniCode.app` (ad-hoc signed; that signature is required to
  run on Apple Silicon and to keep granted permissions stable).
- `make test` — build and run the pure-C++ unit tests (`tests/run_tests.cpp`).
  1403 checks over the tokenizer (including ~25,000 random edits comparing
  incremental against full highlighting, and a timing line for a 100k-line
  file), Markdown parser, terminal output stream and screen grid, settings
  parser, comment toggling, the LaTeX and SyncTeX readers (including the preview's click-to-source
  matching), JSON, the LSP
  client, finding file references and URLs in terminal output, and the
  folder search (on a scratch tree in the temp directory).
  Exits non-zero on failure. The Makefile uses clang++ on the Mac and make's
  default (g++) elsewhere, so it runs on Linux too, and CI runs it there.
- `make run [DIR=~/path]` — build and launch.
- `make icon` — regenerate `resources/AppIcon.icns` from `tools/makeicon.m`.
- `make demos [SCENES="tour latex"] [POSTERS=1]` — record the README's GIFs
  into `docs/demos/` by playing scripted scenes in the real app (see "Demo
  GIFs" below). Windows appear on screen for about 20 s per scene.
- `make dist-zip` / `make dmg` — package for distribution.
- `scripts/release.sh 1.2.0` — cut a release from a clean `main`: runs the
  tests, stamps the version into `Info.plist` and `packaging/minicode.rb` and
  commits that, builds the zip and checks the built app reports the version,
  publishes it on the Homebrew tap repo, bumps the cask's version + sha256,
  then tags `v1.2.0` and pushes `main` with the tag. The version on the
  command line is the only place a version is typed; never edit it by hand.
  Then `brew update && brew upgrade --cask e-c-hansen/tap/minicode` so the
  user's own copy has it; they run MiniCode from Homebrew and expect a
  finished, user-visible Mac change to reach it (Linux-only changes need no
  release, since Linux users build from source).

## Layout of the code

The parts that don't need a GUI are plain C++17 and are unit-tested in
isolation. Keep them dependency-free.

- `src/SyntaxHighlighter.{h,cpp}` — hand-rolled lexer, grammar chosen by file
  extension: Python; C, C++, Objective-C, Java, Go and Rust (one C-like
  grammar); JavaScript, TypeScript and JSON; shell, YAML, TOML and conf; and
  TeX/LaTeX (`.tex`, `.ltx`, `.latex`, `.sty`, `.cls`, `.bib`). Emits
  `{start, length, style}` tokens. Lexes a line at a time from a `LexState`
  (normal, block comment, triple string, backslash-continued string, TeX
  math, TeX verbatim/comment/math environment); `IncrementalHighlighter<Ch>`
  (char or char16_t) keeps line starts and end states and, after an edit,
  re-lexes from the edited line until a line ends in the state it used to.
  `tests/LegacyHighlighter.h` is the old whole-file lexer, frozen as the
  oracle the tests compare against; it has no TeX, so for TeX the random
  edits are checked against a full lex by the new highlighter instead
  (`newGrammar` in the tests).
  TeX has its own line lexer (`lexTexLine`) and uses only the existing
  styles: commands are Keyword, preamble and definition commands
  (`\usepackage`, `\newcommand`) Preprocessor, sectioning commands Function,
  `\begin`/`\end` names Type, `%` comments Comment (not `\%`), math
  (`$`, `$$`, `\(`, `\[` and the math environments) Number with the commands
  inside still Keyword, and `\verb` and verbatim/listing bodies String. A
  blank line ends any open math, as it does in TeX, so an unclosed `$`
  recolors only its own paragraph. Tokens start and end only at ASCII
  characters, which keeps UTF-8 and UTF-16 results identical.
- `src/MarkdownParser.{h,cpp}` — CommonMark subset -> flat list of styled runs.
- `src/Settings.{h,cpp}` — the settings file (`key = value`): parsing with
  per-line errors, defaults, and the resolved color of every surface
  (opacity applied, fallbacks from `window.*`). All color decisions live here
  so they are unit-tested; the GUI only converts `Rgba` to NSColor.
- `src/LatexDoc.{h,cpp}` — LaTeX source -> the byte ranges that hold editable
  text (fields, prose runs, list items, math), plus the two edits the preview
  can make (replace a span, add an `\item`). A structural reader, not a
  typesetter: it reports only what it understands, and an edit replaces exactly
  one byte range, so unknown macros can never be damaged.
- `src/SyncTex.{h,cpp}` — reads the `.synctex` file TeX writes beside the PDF:
  a point on a page -> candidate source lines, best first.
- `src/Json.{h,cpp}` — small JSON value, strict parser, compact serializer.
- `src/LspClient.{h,cpp}` — LSP client with no I/O: framing, handshake,
  document sync, request/response matching, result parsing, UTF-16
  position conversion, server choice by extension. See "LSP" below.
- `src/TerminalStream.{h,cpp}` — pty byte stream -> styled lines (SGR colors,
  in-line cursor movement) + shell-integration events (OSC 133 marks, OSC 7
  cwd). A line model, not a screen: handles sequences and UTF-8 split across
  reads, and leaves alternate-screen text out of the log. Also home to the
  shared pieces: `TermParser` (the DEC/ANSI state machine, with a Handler
  interface), `applySgr`, `termCharWidth`.
- `src/TerminalScreen.{h,cpp}` — the cell grid for full-screen programs
  (xterm subset: CUP/CUU.., ED/EL/ECH, IL/DL/ICH/DCH, DECSTBM + IND/RI/SU/SD,
  alt screen 47/1047/1049, DECSC/DECRC, pending wrap, tabs, wide chars, DEC
  line drawing, DECCKM/keypad/bracketed paste, DSR/DA replies, resize), plus
  key/paste encoding. Tests replay bytes recorded from vim and less.
- `src/TermLinks.{h,cpp}` — what Cmd+click can open in a line of terminal
  output: `path:line:col`, Python's `File "x", line N`, `file(line,col)`
  from TypeScript and MSVC, and URLs with sentence punctuation trimmed.
  Text only; each port checks the path exists before showing a link.
- `src/FolderSearch.{h,cpp}` — Find in Folder's search: a folder, a query and
  a cancel flag in, matches out (path, line, column in characters and in
  bytes, cleaned line text). The Mac's `Search.mm` rules (two characters,
  case-insensitive, dotfiles and `node_modules`/`build`/... skipped, 1 MB and
  UTF-8 only, 2000 matches, ANSI stripped), plus symlink-loop and named-pipe
  safety. Used by the GTK port; `Search.mm` still has its own copy of the
  loop, and Android can use this one.

The GUI is Objective-C++ (`.mm`), the normal way to drive AppKit from C++.

- `src/main.mm` — `AppDelegate`, the menu bar, multi-window bookkeeping, and one
  local key monitor (only for Ctrl+`).
- `src/EditorController.{h,mm}` — the window: file tree, editor, data-safety,
  Markdown rendering, find, scope of most features. This is the big file.
- `src/LineComments.{h,cpp}` — Cmd+/: comment marker per file name, and the
  toggle itself, in UTF-16 offsets so results map straight onto NSRange.
- `src/AppSettings.{h,mm}` — singleton that loads
  `~/.config/minicode/settings.conf` (or `$MINICODE_SETTINGS`), watches it,
  and posts `MCSettingsDidChangeNotification`.
- `src/Terminal.{h,mm}` — shell panel: zsh on a pty via forkpty; a log view
  with a line input, swapped for the grid while a program needs a screen.
- `src/TerminalGridView.{h,mm}` — draws a `TerminalScreen` and encodes keys
  (via `TerminalScreen::encodeKey`/`encodeChar`) straight to the pty.
- `src/Browser.{h,mm}` — WKWebView panel.
- `src/Latex.{h,mm}` — LaTeX preview: runs tectonic, shows the PDF with PDFKit,
  and turns a double-click into a popover editing the source behind that text.
- `src/Search.{h,mm}` — scoped, project-wide text search window.
- `src/Lsp.{h,mm}` — language server processes, `CodeTextView` (the editor's
  NSTextView subclass: squiggles, tooltips, Cmd+click), the completion popup,
  and `LspSession`, one per window.
- `src/Demo.{h,mm}` — demo scenes and the window recorder behind
  `make demos`. Inert unless `MINICODE_DEMO` is set; two one-line hooks in
  `main.mm`. `tools/makegif.m` turns the recorded frames into a GIF.

## UI map (read this before UI work)

Window content view (`EditorController`, built in `init`):

```
container (PanelHost, laid out by hand in layoutContainer)
├── blurView (NSVisualEffectView)                 window.blur, whole window
├── titlebarView + titleLabel                     drawn when customTitlebar
├── NSSplitView (vertical, thin divider)          sidebar | right pane
│   ├── treeScroll → ClickOutline (NSOutlineView)  file tree, bg #252526
│   └── rightArea (PanelHost, laid out by hand in relayoutRightArea)
│       ├── editorScroll → textView (NSTextView)   top slot, bg #1E1E1E
│       ├── browser (BrowserView)                  top slot when open
│       ├── latex (LatexView)                     editor's slot, .tex preview
│       ├── terminal (TerminalView)                docked bottom, bg #181818
│       └── termDivider (DragBar)                  kept topmost
├── status bar (buildStatusBarInContainer)        bottom strip
└── hintsPanel (buildHintsPanelInContainer)       Shift+Cmd+H overlay
```

- Panel backgrounds, panel text, syntax and Markdown colors come from
  `Settings` (defaults are the VS Code Dark+ values in `Settings.cpp`). Each
  view has an `applySettings` method that runs at startup and on
  `MCSettingsDidChangeNotification`; it must recolor what is already on
  screen (editor re-highlights, Markdown re-renders keeping scroll, terminal
  output marked with `MCTerminalDefaultForeground` gets the new color). A new
  setting goes in `Settings.cpp` (parser + default + the commented
  `defaultFileText`, which a unit test checks key by key) and the view's
  `applySettings`.
- Not yet configurable, still inline hex helpers (`Hex`, `THex`, `SHex`):
  accents (#4EA1F7), muted #9CA3AF, divider line #333333, hints overlay,
  terminal panel messages, the Search window, and the ANSI palette in
  `TermColor::rgb()`. `linux/src/Palette.h` mirrors the macOS defaults.
- The window uses `NSWindowStyleMaskFullSizeContentView` always, so content
  runs under the title bar. `layoutContainer` keeps the split view below it
  using `contentLayoutRect` (empty before the window is shown, so
  `titlebarHeight` falls back to frame math, or the split comes out 0px tall).
  With a custom title bar (any `titlebar.*` key, or a non-opaque window) the
  system title is hidden and `titleLabel` mirrors `window.title` via KVO.
- Translucency: a translucent color must be painted once, and as a plain
  layer color. The tree and editor use `PanelScrollView`, which draws no
  scroll/clip background and sizes a layer-backed backdrop view in `tile`.
  Two traps paid for here: a clip view's translucent `backgroundColor` still
  renders opaque, and NSScrollView never resizes extra subviews (a backdrop
  added with autoresizing stays 0x0). Property checks passed both times; only
  a pixel capture caught it (see verification below).
- Settings file in the editor: `applyHighlighting` gives each color value a
  swatch (background = the color, `NSLinkAttributeName` = `minicode-color`).
  `textView:clickedOnLink:` opens NSColorPanel; `colorPicked:` rewrites the
  line with `Settings::setColor` (which uncomments it) and saves after
  150 ms. `linkTextAttributes` is only a pointing-hand cursor so swatches
  keep their colors.
- Layout is manual frames, not Auto Layout (see gotchas for why). Pane state
  flags: `sidebarCollapsed`, `editorHidden`, `terminalVisible`,
  `browserVisible`, plus NSSplitView's own collapse of `rightArea`.
- A new shortcut goes in three places: the menu in `main.mm` (plus a
  forwarding method on `AppDelegate`), `hintsText` in EditorController (one
  `appendHintsRow:` call), and the README shortcut table, which is
  sorted by action and has a column per platform. The hints panel is an
  attributed string on tab stops; never line it up with spaces, because the key
  glyphs (⌘ ⇧ ⌃ ⌫ ⏎) differ in width even in a monospaced font.
- Fonts: system font for Markdown/UI, `monospacedSystemFontOfSize:` 12-13 for
  code and terminal.

## LaTeX preview (read before touching it)

Opening a `.tex`/`.ltx`/`.latex` file sets `isLatex` and opens in preview, the
same way Markdown does; `LatexView` takes the editor's slot in
`relayoutRightArea` (the text view hides, the split logic is unchanged).

- **tectonic does the typesetting.** Found via `MINICODE_TECTONIC`, then
  `~/Library/Application Support/MiniCode/bin`, then PATH and the Homebrew
  directories (a Finder-launched app has a short PATH). If it is missing the
  panel offers to download the pinned release into Application Support.
- **The buffer is typeset from a hidden sibling** `.<name>.minicode.tex` in the
  document's own folder, so relative `\input`/`\includegraphics` still resolve
  and the user's file is never written. It is deleted when tectonic exits.
  Output (PDF + `.synctex.gz`) goes to `NSTemporaryDirectory()/MiniCode-latex`.
- **Compiles are debounced (0.6s) and generation-counted**; a result from a
  stale run is dropped. The scroll position and zoom are restored by page index
  and point, since the `PDFDestination` belongs to the old document.
- **Unknown commands are looked through, not skipped.** Real documents (and
  custom classes like an Overleaf resume) wrap their text in `\normalfont{}`,
  `\raisebox{}{}`, `\href{}{}` and the author's own macros; text the reader
  cannot see is text the preview cannot edit, and the click then lands on the
  wrong line. So `LatexDoc` skips only a list of known machinery commands
  (`\label`, `\includegraphics`, `\setlength`, ...) and otherwise enters the
  first braced argument that `looksLikeProse` accepts. That heuristic is what
  keeps `{0.5em}`, `{l}`, `{sec:intro}`, `{GDM.png}` and URLs out, while
  letting `{2014 - 2019}` (digits plus spaces) in. A URL only disqualifies a
  group that is one unbroken token: the user's "italics block" bug was
  `\it{Note: ... \href{https://...}{...}}`, skipped whole because the text
  inside held `://`. A group that holds commands is always entered, since
  what is inside gets judged one level down anyway.
- **Letters spelled as commands stay in the run.** `\ss`, `\o`, `\LaTeX`,
  `\c{c}` and `\'{e}` are part of a word, so they no longer end a span
  (`kTextSymbols`, `kLetterAccents`), and `displayText` reads them as the
  letters they print. `\\` ends a run like `&` does (the check for it used to
  compare against two backslashes and never fired). `\url{...}` is a field,
  since its argument is printed text.
- **A click that matches nothing refuses.** `spanForClick` returns null rather
  than the nearest span whenever there was a real word to match on, because
  opening the wrong text for editing is worse than opening none. Only a
  "word" with no letters or digits at all (a bullet, a logo) falls back to
  the nearest span. Text TeX makes up is refused on purpose: section and page
  numbers, and words that come from a `\newcommand` body.
- **A single letter or digit is always refused**, before any matching, even
  when the text around it agrees with the source. Page, section, list and
  footnote numbers are single characters TeX made up, and they used to fall
  through to the nearest span (a page number opened the paragraph above it,
  a section number its heading). Context could tell most real ones apart,
  but not with a guarantee, and the word beside the character opens the same
  span anyway. The price, accepted by the user: a list item's number or
  label, a one-letter word ("a", "I") and a one-letter math variable no
  longer open anything; click a neighbouring word instead.
  The exception is a single Han, kana or Hangul character: those scripts
  have no spaces, so PDF word selection often returns one ideograph, and
  refusing them made CJK documents uneditable from the page. Such a
  character is taken only when the page text around it agrees with the
  source; Greek and accented single letters stay refused.
- **Click to source**, all of it in `MCLatexSpanAtPoint` (Latex.mm), which the
  sweep harness calls too:
  - **Lines: `SyncTexIndex::textHitsAtPoint`.** A line of text's box carries
    the line its *paragraph* closed on, often several past the words. The
    glue and kerns between words carry the line each word was read on, so the
    record just right of the click comes first, then the one just left, then
    the plain box hits. Glue sitting on a box's end edge closes the box (a
    table cell) and may belong to the next row, so then left goes first.
  - **Word: PDFKit's `selectionForWordAtPoint:`, plus 40 characters either
    side** (`extendSelectionAtStart:`/`AtEnd:`). A word TeX hyphenated at a
    line end is rejoined from that context (`MCJoinHyphenation`).
  - **Match: `LatexDoc::matchKey`** on both sides: letters and digits,
    lowercase, ligatures split (the PDF gives U+FB03 for "ffi"), accents
    dropped (é, ß -> e, ss), other scripts kept. A key of three characters or
    fewer must be a run of whole words in the span ("at" is not in "Math"),
    except in math, which the PDF reads back as "x2" for `x^2`.
  - **Choice: context.** Every nearby span holding the word is a candidate,
    scored by how much of the page text around the click agrees with the
    text around the word in the source (`lead`/`trail`, built from the
    neighbouring spans so URLs and comments never get in). A later candidate
    must beat the nearest by 4 characters, because the PDF's reading order is
    not always the source's: a tabular reads back column by column.
  - **Extras**: spans cover all their lines (`endLine`), not just the first;
    `\title`/`\author`/`\date` are candidates at `\maketitle`; a word split by
    a font change mid-word (`foo\emph{bar}baz`) matches a *join*, one span
    over the whole chain, offered only when its range has balanced braces;
    a footnote mark glued to a word ("footnote1", "1The") gets a second try
    without the digits.
- **Measure before changing the matcher.** `tests/latex/sweep.sh doc.tex [-v]`
  typesets a document the way the app does and double-clicks every word of
  every page (first, middle and last character) through
  `MCLatexSpanAtPoint`, reporting found / refused / wrong, plus "not placed"
  when a common word landed in a span that nothing confirms is the right
  occurrence. Single characters are counted apart, with how many still
  offered a span, which must be 0 (the Mac harness reports this but has not
  been rebuilt since the change). `tests/latex/torture.tex` is the corpus
  of constructs. On
  2026-09-21 the torture document went from 76% found-and-placed to 98.7%
  (the rest is generated text, correctly refused) and the user's resume
  (four versions) from 87% to 100%, with zero wrong in both. Set
  `TECTONIC_CACHE_DIR` to a scratch copy of the cache when running it. One
  trap paid for: PDFKit's `characterBoundsAtIndex:` drifts away from
  `page.string`'s indices as a page goes on; the bounds of a one-character
  `selectionForRange:` do not.
- **Edits are byte splices.** The popover holds the span's own LaTeX source
  (not plain text), so no escaping is invented and nothing is re-serialized.
  Edits arrive back at `EditorController` through `onSourceEdited`, mark the
  buffer dirty, and are saved only by Cmd+S. Preview edits have their own undo
  stack inside `LatexView` (source snapshots, capped at 50), reached through
  `undo:`/`redo:` on the responder chain, with `EditorController` forwarding
  when focus never got as far as the preview. The text view keeps its own undo
  for typing; the two stacks are separate.
- **Export PDF (Shift+Cmd+S)**: `exportPDF:` opens a save sheet, then
  `LatexView pdfForPath:source:completion:` hands back the PDF bytes exactly
  as tectonic wrote them (kept in `_pdfData` with the source they came from,
  `_pdfSrc`). If that source is not the buffer's, it typesets now, skipping
  the debounce, and answers when a run finishes with nothing queued; a
  failed run answers with an error. Works from the source view too (the
  LatexView is created hidden if needed). The menu item is `s` with an
  explicit Cmd|Shift mask, like the other shifted shortcuts. **Testing trap**:
  a synthetic `NSEvent keyEventWithType:` for Shift+Cmd+S with
  `charactersIgnoringModifiers:@"s"` matches *Save* (Cmd+S) first and writes
  the file, and with `@"S"` matches nothing. Neither is what a keyboard does.
  `CGEventCreateKeyboardEvent` + `CGEventPostToPid(getpid(), ...)` goes
  through the window server like real hardware, and it reached Export and
  not Save.
- **Packages missing from tectonic's bundle.** tectonic's bundle is an
  older TeX Live than Overleaf's. The user's resume needed `twemojis`, which the
  bundle lacks; the fix was to put `twemojis.sty` and `all-twemojis.pdf` in the
  document's own folder (TeX looks there first). tectonic cannot generate a
  `.sty` from a `.dtx`/`.ins` itself: its virtual filesystem throws away files
  docstrip writes. A short script that keeps the `.dtx`'s code lines (not
  starting with `%`, outside other guards) produced a working `.sty`.
- **Spacing differs from Overleaf by design.** Overleaf defaults to pdfLaTeX,
  tectonic is XeTeX; fonts and line breaking differ, and `\hfill`-built lines
  show it most. The user was told to switch Overleaf to XeLaTeX to compare.
- **The Linux port** (`linux/src/Latex.cpp`, over `PageWords.cpp` and
  `LatexClick.cpp`) follows all of the above with poppler in place of PDFKit,
  and differs on purpose in a few places, listed in `linux/HANDOFF.md` item 7:
  the GtkTextBuffer keeps the source and preview edits are splices into it
  (so undo is the buffer's), a click is traced only while the PDF is of the
  buffer as it is, a click on no text refuses, and tectonic's download is the
  static musl build checked against a pinned SHA-256.
  `tests/latex/sweep-linux.sh` is its sweep: 0 wrong on torture.tex and
  notes.tex, and none of their 54 single-character clicks offered a span.
  It is the sweep that found page and section numbers falling through to the
  nearest span, which led to refusing single characters.
- Headless testing worked well here: drive `openEditorForPage:point:`,
  `startAddItem:` and `commitEdit:` directly from a `MINICODE_LATEXTEST` block,
  finding page points with `[PDFDocument findString:]`. Point
  `MINICODE_TECTONIC` and `TECTONIC_CACHE_DIR` at scratch copies so the test
  neither needs the network nor touches the user's cache.

## LSP (read before touching it)

The protocol lives in `LspClient.cpp`, pure C++ and tested against a scripted
server in `run_tests.cpp`; `Lsp.mm` owns processes and UI on the Mac. The GTK
port has its own `linux/src/Lsp.cpp` over the same core (item 6 of
`linux/HANDOFF.md` has its design). One GTK trap worth knowing: a popover
given to a `GtkTextView` with `gtk_widget_set_parent` must be unparented
before the view is disposed (it is done on `unrealize`), or GtkTextView's
dispose loops forever warning "is not a child of GtkTextView" and the window
never closes.

- **EditorController hooks are few on purpose**: `documentOpened:` (path, or
  nil for messages, binary files and previews), `documentSaved`, `setRoot:`
  (Open Folder), `shutdown` (windowWillClose), and `textView:doCommandBySelector:`
  forwarding to `handleCommand:`. The session sees typing by observing
  `NSTextStorageDidProcessEditingNotification` itself, and didChange is
  debounced 0.3 s; every request flushes first. `Client::didChange` drops
  unchanged text, so the storage replacement on file switch costs nothing.
- **Queued until Ready**: didOpen and requests made during the handshake are
  queued in the client and sent after `initialized`. Server-to-client requests
  are always answered (configuration gets nulls, unknown methods get
  -32601), because some servers wait for the reply.
- **Server lookup**: PATH, then /opt/homebrew/bin, /usr/local/bin, ~/.cargo/bin,
  ~/go/bin, ~/.local/bin, /usr/bin. `/usr/bin/clangd` is an xcrun shim that
  pops the "install developer tools" dialog on a Mac without them, so it is
  never run; `MCDeveloperTool` finds the real binary via
  /var/db/xcode_select_link. The child gets that wider PATH too (servers spawn
  node, go, cargo). `which gopls` said "not found" on this Mac while
  ~/go/bin/gopls exists; the app's search does find it.
- **Settings**: `lsp.enabled`, `lsp.<cpp|python|go|rust|typescript>`. The
  command value is a whole command line (to a ` #` comment), unlike every
  other key; `off` disables one language. A change restarts the servers.
- **Processes**: NSTask with pipes. stderr goes to /dev/null (clangd logs
  every request there, a full pipe would stall it) or to `$MINICODE_LSP_LOG`,
  which also logs all traffic (opened O_APPEND, several servers share it).
  Writes run on a serial queue with raw `write()` and `F_SETNOSIGPIPE`, so a
  dead server gives EPIPE, not SIGPIPE. Window close: shutdown, exit on the
  reply, stdin closed; SIGTERM after 2 s, SIGKILL after 3. App quit
  (`applicationWillTerminate` → `MCLspTerminateAllServers`): shutdown + exit
  at once, wait up to 0.5 s, then signals. If MiniCode crashes, stdin EOF
  makes servers exit.
- **Diagnostics are drawn, not attributed.** `CodeTextView` draws
  squiggles after `super drawRect:` from `enumerateTextSegmentsInRange:`
  under TextKit 2 or the layout manager's rects under TextKit 1, only for
  the viewport. In practice it is TextKit 1: AppKit builds any NSTextView
  subclass that overrides `drawRect:` with a TextKit 1 layout manager
  (checked on 2026-09-25; a plain NSTextView gets TextKit 2, a subclass
  whose only override is `drawRect:` gets TextKit 1, and no switch
  notification is posted). Notes elsewhere that assume TextKit 2 predate
  this finding.
  Marks are NSRanges that follow edits until the server republishes, and they
  survive re-highlighting because they are not storage attributes. Tooltips
  are `addToolTipRect:` per visible mark, rebuilt after drawing when dirty.
- **Completion**: auto-triggers on `.`, `->`, `::` (if the server lists the
  trigger char), manual on Ctrl+Space or Option+Esc (`complete:`). The list
  filters client-side as you type (prefix matches first, then subsequence),
  closes when the caret leaves the word. Accept replaces from the textEdit's
  start (or the word start) to the caret. snippetSupport is off, so clangd
  inserts `lengthSquared` without parentheses; its labels carry a leading
  space or bullet, stripped in `parseCompletion`.
- **Definition**: the target is mapped back into the tree root's spelling of
  the path (clangd answers with /private/tmp/... for a /tmp root) before
  `revealPath:andOpen:`; outside the root it goes to `openFileAtPath:`.
- **Tested headless** with a temporary `MINICODE_LSPTEST` block (31 checks,
  removed): real clangd on a scratch project, diagnostics range and message,
  status text, squiggle pixels via `cacheDisplayInRect:`, mark shifting,
  auto and Ctrl+Space completion with filtering, Return/Esc, hover, F12 and
  Cmd+click across files, markdown and missing-server cases, per-window
  shutdown, and no server process left after quit. Point
  `MINICODE_SETTINGS` at a scratch file and `MINICODE_LSP_LOG` at a scratch
  log when doing this again. Other sessions (and the user's own MiniCode)
  may have clangd running: kill only PIDs the test started.

## Current state (handoff, 2026-09-26)

- **Released: 1.4.5** (2026-09-26), on GitHub Releases and the Homebrew tap,
  with the Mac zip and the signed Android APK; the user's own Mac runs it
  from Homebrew. The user expects a finished Mac change to be pushed,
  released and upgraded on their Mac (see Build / release). What each
  release added: 1.4.1 Linux parity, review fixes, the TeX grammar, images
  in the Markdown preview; 1.4.2 GIFs that play (the editor is TextKit 1,
  see LSP); 1.4.3 real Markdown tables and an editor as wide as its pane;
  1.4.4 working Markdown links, Shift+Cmd+P keeping its place, the user's
  README rewrite; 1.4.5 editing Markdown from the preview (double-click a
  block). After 1.4.5, on `main` and pushed but not released: the Linux
  preview brought level with the Mac (links, pictures, tables, place
  keeping, double-click editing), Linux-only, so no release was cut.
  1,522 core checks pass; Mac and Linux builds are warning-free; CI builds
  and tests macOS and Linux.
- **In progress: a git panel for the Mac**, started 2026-09-26 by a
  subagent in a separate git worktree (its own branch; not merged, not
  pushed). The brief: `src/GitStatus.{h,cpp}` parsing `git status
  --porcelain=v2 --branch -z` and unified diffs, tested; a Source Control
  view that takes the sidebar's place (branch, staged and unstaged changes,
  Space stages, Return shows a colored diff in the editor's slot, a commit
  field with Command+Return); only non-destructive git commands (status,
  diff, add, restore --staged, commit); README and this file updated. When
  it reports, review the branch, merge it, and let the user try it before
  a release. `git worktree list` shows where it is.
- **Markdown preview** (Mac and Linux, not Android): links, pictures with
  GIFs playing, real tables, Shift+Cmd+P / Ctrl+Shift+P keeping the place,
  and double-click editing of a block in a popover. Details under
  "Markdown" in the gotchas below.
- **macOS**: terminal links (Cmd+click `file:line` and URLs, log and grid) are
  in 1.4.0; the user has not yet clicked them in the grid (Claude Code, vim).
  A language server that fails `initialize` now says "failed to start"
  instead of "starting…" forever (unseen on screen). All eight demo GIFs
  were re-recorded on 2026-09-23, and `latex.gif` again on 2026-09-25 so its
  source view shows the TeX colouring.
- **Android** (1.4.0 APK): editor, terminal with a key row (Esc, Tab, sticky
  Ctrl, Up/Down history, symbols), terminal links, Markdown, images, PDFs,
  browser, and through Termux the language servers and the LaTeX preview
  with double-tap editing. Projects must live in phone storage, opened with
  leader O → Phone storage. Missing: project search, comment toggling,
  PDF pages past the first outside the LaTeX preview, image zoom, terminal
  scrollback. The release key and its backup are described under
  Distribution.
- **Linux**: all nine items of `linux/HANDOFF.md` are done (the user's work,
  2026-09-24), and a three-part review on 2026-09-25 was fixed the same day:
  huge and special files refused before reading, renames followed in every
  window, in-place saves for hard links and foreign owners, PDF pages
  rendered on a worker thread with a placeholder for pages that fail, a
  per-preview LaTeX sibling cleaned up through a folder handle, the Find in
  Folder picker made safe after its window closes, stale language servers
  ignored. Verified in the Docker container; still needs the ThinkPad: the
  folder picker after closing its window, F2 rename in the tree, renames
  across two windows under GNOME, and the PDF placeholder at 2x.
- **Reddit**: drafts for five subreddits are a Claude Doc, "MiniCode Reddit
  drafts" (https://claude.ai/code/artifact/06496ec8-70b7-443d-af5c-9c53f3996ffe),
  not in the repo. The user is posting from a new account.
- The user's phone is a Unihertz Titan 2 (Android 16, 576 by 640 dp, hardware
  keyboard, Termux and F-Droid installed). Wireless debugging changes port on
  every reconnect, so ask for the new one rather than guessing. Test files are
  in `/sdcard/mc-test`.
- Still needs the user's eyes on the Mac: the terminal grid, LSP squiggles,
  completion and hover, the LaTeX popover's placement, and whether
  Ctrl+Space reaches the app with several input sources.
- tectonic 0.17.0 lives at
  `~/Library/Application Support/MiniCode/bin/tectonic` with its cache in
  `~/Library/Caches/TectonicProject.Tectonic` (about 44 MB; text fonts but no
  Computer Modern math fonts, so math needs the network).
- `tests/latex/sweep.sh` double-clicks every word of a typeset document
  through the real click path: 100% on the user's resume, 98.7% on
  `tests/latex/torture.tex` (1,081 found, 14 refused, 0 wrong of 1,095).

## Images

Opening an image file (png, jpg, jpeg, gif, tif/tiff, bmp, heic/heif, webp,
ico, icns) shows it in the editor's slot instead of the "Cannot display"
message. SVG and PDF are deliberately not in the list: SVG is source people
edit, and a PDF would show only its first page.

- `openFileAtPath:` calls `resetViewMode` first (clears
  `isMarkdown`/`isLatex`/`isImage`/`previewMode`/`sourceText`/`dirty`, makes
  the text view read-only), then routes by extension *before* trying to read
  the file as text. Opening another folder and trashing the open file call it
  too; before, both left a stale PDF preview over the welcome text.
- `showImageAtPath:` adds an `NSImageView` to `rightArea` lazily;
  `relayoutRightArea` gives it `topRect` and hides `editorScroll`, the same
  way the LaTeX view takes the slot. `NSImageScaleProportionallyDown`, so
  small images are never blown up; `animates` plays GIFs. The background is
  the editor surface color, re-applied in `applySettings`.
- The image's `size` is set to its pixel size, because `NSImage.size` is in
  points and a 144 dpi screenshot would otherwise draw at half size. The
  pixel size is shown in the title bar.
- Nothing is saved: `setPlainMessage:@""` empties the hidden text view and
  sets `showingMessage`, and `saveCurrentFile:` also returns while `isImage`.
  `checkExternalChange` reloads the image when the file changes on disk.
- An image that fails to decode falls through to the text path and gets the
  usual "Cannot display" message.
- Verified with a temporary `MINICODE_IMGTEST` block (16 checks): tex, then
  png, then bin, then txt, view visibility, image and title sizes including a
  2x-dpi PNG, Cmd+S leaving the PNG bytes unchanged, a pixel read from a
  `screencapture -l` of a large image, and the text file coming back editable.
- **PDFs** (`showPDFAtPath:`) take the slot the same way, in a `PDFView`
  (autoscaled, single page continuous, editor background), with the page
  count in the title bar. `isPDF` is cleared by `resetViewMode`, blocks
  `saveCurrentFile:`, and `checkExternalChange` reloads it keeping the page,
  point and (if the user zoomed) scale. Verified with a temporary
  `MINICODE_PDFTEST` block (10 checks, including a pixel read and a reload
  on disk change). That test found that `refreshDisplay` only relaid out when
  a LatexView existed, so in a window that never opened a .tex, going from
  an image or PDF to a text file left the image on screen; it now always
  relays out. The user asked that test windows not pop up while they work:
  ask before running one, and don't activate the app from a test.
- Not yet: zoom and scrolling for large images. Linux has images and PDFs
  since September 2026 (`linux/src/MediaView.cpp`, `linux/src/PdfView.cpp`;
  see `linux/HANDOFF.md`, item 4).

## Demo GIFs (`make demos`)

`scripts/demos.sh` builds a scratch world under `/private/tmp/minicode-demo`
(a copy of `demo/` made into a 5-commit git repo with fixed dates, so hashes
are stable; a scratch HOME/ZDOTDIR; `tools/demo-settings.conf` as
`MINICODE_SETTINGS`; a copy of the tectonic cache; a tiny `.vimrc` in the
scratch home for the vim scene), launches the raw binary
with `MINICODE_DEMO=<scene>` under `env -i`, polls and kills after 2 min,
then runs `build/makegif`. It only ever kills its own pid (the user often has
MiniCode open) and deletes the scratch folder on exit. The scene list comes
from `MINICODE_DEMO=list`. `DEMO_KEEP_FRAMES=<dir>` keeps the raw frames and
the app's log for debugging (also when the scene fails); look at frames
with the Read tool. Scenes: tour, terminal, settings, latex, lsp, vim, files, links.

- **A scene** is a function in `src/Demo.mm` calling builder methods
  (`clickFile:`, `type:`, `key:caption:`, `clickAt:`, `doubleClickAt:`,
  `scrollEditorTo:`, `waitFor:timeout:recorded:`, `run:`, `pause:`,
  `hidePointer`, `poster`) plus a row in `kScenes`. Steps are queued and
  played in order; points are resolved lazily when the step runs.
  `key:` also knows `f12`, `left`/`right`/`up`/`down`, `space`, `esc` and
  `return`; function and arrow keys get the function-key character and the
  flags a real keyboard sends. `pointForText:` goes through
  `firstRectForCharacterRange:`, never `tv.layoutManager`, which would
  switch the editor to TextKit 1 for the rest of the run.
- **Input is real events.** Clicks and keys are `NSEvent`s posted with
  `postEvent:`, so they go through the tree's `mouseDown:`, the text input
  system (US key codes, shift for capitals/symbols), the Ctrl+` key monitor
  and the menus' key equivalents. The pointer and key captions are drawn by
  an overlay view in the window (`hitTest:` returns nil); window captures
  show neither the real cursor nor keys.
- **The user's own input is swallowed** while a scene plays (a local monitor
  passes only events whose timestamp, in whole microseconds, the demo
  posted). Found the hard way: the window comes to the front, and a stray
  "p" the user typed elsewhere landed in the welcome text, made it dirty,
  and the next file click hit the "Save changes?" alert. AppKit keeps posted
  timestamps only to about a nanosecond, so match on rounded microseconds.
- **Capture** is `screencapture -x -o -l<wid>` about every 100 ms from a
  background thread, with per-frame times in `manifest.txt`. A child window
  (NSPopover) is included in the main window's capture, which then grows to
  take it in (1920x1206 instead of 1920x1200 when the popover pokes 3 pt
  above the window); the manifest records `child` rects and makegif crops
  the window back out. Other windows in front (NSColorPanel) are captured
  separately in parallel and composited (`over`). `waitFor:...recorded:NO`
  pauses the recorder so a long wait (zsh starting, tectonic) is cut from
  the GIF.
- **makegif** writes the GIF itself: ImageIO's GIF writer stores every frame
  whole, and with alpha it uses disposal 2 (restore to background), so
  frame-diff transparency does not work through it. makegif uses one
  median-cut palette for the whole animation (sqrt-count weighting so text
  anti-aliasing gets colors), merges identical frames, stores only the
  changed rectangle with unchanged pixels transparent (disposal 1), and LZW
  encodes. The six GIFs total about 1.6 MB at 960 px wide.
- **Paths in frames.** `NSHomeDirectory()` ignores `$HOME`, so the terminal
  prompt shows the real cwd; that is why the project lives under /tmp, not
  a fake home. `stringByResolvingSymlinksInPath` turns `/private/tmp/...`
  into `/tmp/...`, so the tree root is `/tmp/minicode-demo/demo`.
- **The display must be awake.** With the screen asleep (it was, after the
  Mac idled), `screencapture` returns black full-screen images and
  `-l<wid>` fails with "could not create image from window", which the
  recorder reports as a Screen Recording permission problem.
  `caffeinate -u -d -t 200 &` wakes the display and keeps it on for a run.
- **lsp scene**: clangd on `demo/vec` (main.cpp + vec.h). There is no
  compile_commands.json, so clangd writes no index into the project
  (checked: nothing appears in the scratch copy or the scratch home). It
  saves with Cmd+S before F12, or leaving the dirty file raises the "Save
  changes?" alert and the scene hangs until the 2-minute kill. It stops with
  a message if the status says no server was found.
- **vim scene**: sets `terminalHeight` via KVC before opening the panel (as
  if the bar had been dragged), and waits on the terminal's `gridMode` and
  `atPrompt` ivars (KVC reads them directly). Never press Esc in the
  *editor* in a scene: NSTextView maps it to `complete:`, which opens the
  LSP completion list.
- The LaTeX scene is skipped (exit 3) when tectonic is missing. Keep
  `demo/paper/notes.tex` free of math: the cache has no math fonts, and
  tectonic would go to the network.

- Eight scenes as of 2026-09-23: tour, terminal, settings, latex, lsp, vim,
  files, links. `links` Command+clicks a `grep -Hn` result and a URL; the
  hover underline follows the real Command key and mouse, which a recording
  cannot press, so the scene adds the underline itself before clicking. `files` opens `demo/images/icon.png` (the app icon at 256 px, made
  with `sips` from `resources/AppIcon.icns`) and `demo/paper/notes.pdf` (the
  demo's own notes.tex typeset by tectonic; rebuild it with tectonic if that
  file changes). All seven were re-recorded that day so the tree and git log
  match the current demo folder. To look at frames without re-recording, read
  the GIF with ImageIO and composite frames in order (frames after the first
  store only the changed region).

## Android (`android/`, read `android/README.md` first)

A Kotlin app around the same C++ core, developed against a real phone over
adb (no emulator). `android/README.md` has the build, the device workflow and
the file map; what belongs here is what it cost to learn:

- **The core is portable.** All 1,079 checks in `tests/run_tests.cpp` compile
  with the NDK and pass on the phone unchanged, at Mac-like speeds (100k-line
  full lex 29 ms, one keystroke 0.13 ms). Highlighting, Markdown and the
  terminal screen are that same code; only the GUI is new.
- **Phone keyboards do not have the keys a desktop has.** On a Unihertz
  Titan 2: no Ctrl, Esc or Tab; Alt is the symbol layer (Alt+S types "4");
  Android claims Sym with some letters before an app sees them (Sym+H opens
  the microphone, Sym+B reaches the app). One unclaimed key is left, so it
  is a leader: press, then a letter. `adb logcat -s MiniCodeKeys` prints
  every key the app sees and is the way to work this out on a new device.
- **A letter does not arrive as a key event while a text field has focus.**
  The keyboard reaches the field through the input method, so shortcut
  letters are caught in `CodeEditText.commitText`. Moving focus away and
  hiding the keyboard does not change it. In the file list and the terminal
  the same letters do arrive as key events, so both paths run one table.
- **A terminal must refuse composition.** `TerminalView` declares
  `TYPE_NULL`; while it accepted text, the keyboard re-sent the word it was
  composing on every keystroke and "ls demo" reached the shell as
  "sso dlemodlemo".
- **The Titan's spare key repeats in bursts** (a hundred key-downs per
  press), so presses are separated by the key's release, not by a timer.
- **`TermKey` ordinals are load-bearing.** `Pty.kt` mirrors the enum in
  `src/TerminalScreen.h`; when Enter was numbered as Escape, nothing ran and
  the symptom looked like a dead Enter key.
- **What a development machine cannot test:** any shortcut needing a
  modifier (an injected key carries no Sym), keycodes above ~288 (the spare
  key is 403, hence Menu and Function as leaders too), and `sendevent`,
  which SELinux refuses. Those need a person at the phone; say so rather
  than claiming a shortcut works.
- **Language servers run in Termux** (`Termux.kt`, `LspSession.kt`,
  `lsp_jni.cpp`): RUN_COMMAND starts bash, which dials a one-shot
  127.0.0.1 listener with /dev/tcp, sends a token, and execs the server on
  the socket. Listen on 127.0.0.1 by name: `getLoopbackAddress()` is ::1
  on Android and bash's connect is refused. Only files in shared storage
  get a server, since Termux cannot see anything else.
- **Toolchain:** Gradle 8.14.3 via the wrapper (9.7 drops an API the Android
  plugin uses) and JDK 21 (Gradle 8 refuses 27). The SDK and NDK are about
  3.6 GB, installed with `sdkmanager`, no Android Studio.

## Memory benchmark (`make membench`)

`scripts/membench.sh` runs one workload on MiniCode and on VS Code + Chrome +
Preview and sums the physical footprint (`tools/procmem.c`, via
`proc_pid_rusage`) of every process each side owns. Result on 2026-09-21
(M3, 16 GB): MiniCode 320 MB, the others 1,283 MB; in the README's Memory
section. Traps paid for while building it:

- Ownership is parent pid OR *responsible* pid. WebKit's helpers are started
  by launchd, so only the responsible pid ties them to MiniCode, and an app
  launched from a shell makes the *terminal* responsible. So MiniCode is
  launched with `open -n -g` (LaunchServices, background) and `--env`.
- `stringByResolvingSymlinksInPath` turns /private/tmp into /tmp, so the
  window root and a bench path spelled /private/tmp/... did not match and
  revealPath opened nothing. `src/Bench.mm` resolves its paths the same way.
- VS Code's "run task on folder open" was skipped on some launches. A tiny
  extension loaded with `--extensionDevelopmentPath` opens the terminal and
  types the tectonic command instead, deterministically.
- MiniCode's workload uses two windows (code + terminal + browser, and the
  LaTeX preview), matching VS Code beside Preview. `MINICODE_BENCH` also skips
  `activateIgnoringOtherApps` so the MiniCode half never takes focus.
- Always check each side's breakdown before quoting a number: the first run
  "measured" MiniCode at 55 MB because clangd and WebKit were missing.

## Verification reality (important)

How much of the GUI you can actually check depends on which port you are
working on, and on whether the session is running on a machine with a display.
Check first; do not assume either answer.

**The macOS app, in a session on the user's Mac: more than it looks.** The
app really launches (a window appears on the user's screen, so keep runs short
and never leave strays). Synthetic key and mouse events sent from inside the
app work, and so does capturing the app's own window as pixels. What stays
out of reach is judging the look: a pixel value can be read, but whether it
looks right is the user's call. A session on a machine without a display (CI,
the Linux box) gets none of this, only a compile.


- Test the pure-C++ core with `make test`.
- For GUI logic, add a temporary `getenv("MINICODE_*TEST")` block in `main.mm`
  that drives the code path and NSLogs PASS/FAIL, run the binary headless in
  the background with a poll-and-kill loop (there is no `timeout` on macOS),
  then remove the block. Techniques that proved reliable:
  - Read state via KVC (`[controller valueForKey:@"rightArea"]`).
  - Key shortcuts: build an `NSEvent keyEventWithType:` and send it through
    `[window performKeyEquivalent:]` then `[NSApp.mainMenu performKeyEquivalent:]`.
  - Mouse drags on views with their own tracking loop (NSSplitView): post
    LeftMouseDragged/LeftMouseUp with `[NSApp postEvent:atStart:NO]`, then call
    `mouseDown:`. For simple views (DragBar) call mouseDown/Dragged/Up directly.
  - Use `hitTest:` at pixel offsets to measure real grab areas.
  - Terminal tests must set `HOME` and `ZDOTDIR` to scratch dirs, or test
    commands land in the user's real `~/.zsh_history`.
- Pixels: `/usr/sbin/screencapture -x -o -l<windowNumber> out.png` run from
  inside the app captures the window with its alpha channel, and
  `NSBitmapImageRep colorAtX:y:` reads it (y from the top, scale for Retina).
  This is the only way to verify transparency; view properties can look right
  while nothing is drawn. `CGWindowListCreateImage` is gone in the macOS 15 SDK.
- Undo in tests: everything run from one callback lands in one undo group
  (`groupingLevel` stays 1). Wrap each simulated key press in
  begin/endUndoGrouping with `groupsByEvent` off. Programmatic edits call
  `breakUndoCoalescing` so each Cmd+/ is its own step for real users.
- Cleaning up temp test blocks: remove them by exact text, not by searching
  for a marker comment that also appears on the `getenv` line.
- When a check fails, print the actual output before changing code: most
  failures in this session were wrong checks, not wrong code.
- Otherwise confirm the app builds clean and launches without crashing, and
  tell the user which behavior needs their eyes. Do not claim GUI behavior is
  verified when it was only compiled.

**The GTK port from the Mac: a Docker container.** Docker Desktop is installed
on the Mac (start it with `open -a Docker`). `linux/dev/gtk-dev.sh` builds the
image (`linux/dev/Dockerfile`: ubuntu:26.04, the CI packages, Xvfb, xcompmgr,
xdotool, ImageMagick), starts a container with the repo mounted, and builds and
tests the port; `/tools/launch.sh` inside it runs the app. `linux/dev/README.md`
has the workflow. The details behind it:

- Mount the repo (`-v ~/Code/public/minicode:/work`) and build into a container-local
  directory (`meson setup /build`), not `linux/build`.
- `Xvfb :99 -screen 0 1280x800x24` plus `xcompmgr` gives a composited display,
  so translucency works. Launch with `GDK_BACKEND=x11 GTK_A11Y=none` under
  `dbus-run-session`, a scratch `HOME`/`XDG_CONFIG_HOME`, and
  `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` (WebKit's bubblewrap sandbox
  cannot start in a container and aborts the whole app). `GTK_CSD=1` draws
  GTK's own title bar, since there is no window manager.
- Drive it with `xdotool key ctrl+shift+t`, `xdotool mousemove X Y click 1`,
  and drags as `mousedown`, a few `mousemove`s, `mouseup`. Capture with
  `import -window root` and read pixels with
  `convert img -format '%[pixel:p{X,Y}]' info:`. The compositor's backdrop is
  gray 128, so a see-through panel's alpha can be computed from the blend.
- `docker cp` screenshots out and look at them. Stray dotted glyph debris in
  root captures is xcompmgr damage, not the app: it vanishes after a resize
  and never appears in `import -window <id>`.
- In helper scripts use `pkill -x minicode`, never `pkill -f` with a path: it
  matches the `docker exec bash -c` command line and kills the script.
- Disk on the Mac is tight (it had 730 MB free); the image is about 1.8 GB.

**The GTK port on the user's Ubuntu machine: a live session.** As of August
2026 the primary Linux box is Ubuntu 26.04 on a ThinkPad T480, GNOME on
Wayland, and a session there has `$WAYLAND_DISPLAY` set and can really launch
the app. Confirm with `echo $WAYLAND_DISPLAY` before relying on this. When it
holds, these give real runtime evidence rather than compile-only evidence:

- `meson test -C build` for the shared core.
- Launch detached (`setsid gtk-launch org.minicode.Editor &`, or run the binary
  directly) and check with `pgrep -a minicode` that it is still alive a few
  seconds later. This is not free: a window really does appear on the user's
  screen, so say what you are doing and do not leave strays running.
- `busctl --user list | grep minicode` shows the GApplication registered its id
  on the session bus.
- `systemctl --user list-units --type=scope | grep minicode` is the useful one.
  A launch through the desktop entry lands in
  `app-gnome-org.minicode.Editor-*.scope`, and that scope name is precisely how
  GNOME Shell ties the window back to its launcher, so seeing it proves the
  dock association works. A nested `vte-spawn-*.scope` proves the VTE terminal
  panel actually came up.
- `journalctl --user --since "-15 min" | grep -i minicode` catches a process
  that started and then died.
- Do not bother with `org.gnome.Shell.Introspect.GetWindows`; it answers
  `AccessDenied` for callers that are not whitelisted.
- Real clicks and keys: a private rootful Xwayland (`Xwayland :87 -geometry
  1300x850`, one window on the user's desktop) with the app inside it on
  `GDK_BACKEND=x11`, driven by XTest through Python `ctypes`, and `xwd -root`
  for captures. `linux/HANDOFF.md` ("How to work on it") has the details.
  Prefer it to hooks that call code paths: in September 2026 three bugs the
  user hit by hand (no visible tree selection, Find in Folder not scrolling
  to the match, Down from the search field) had all passed such hooks.

## Gotchas already paid for (don't rediscover these)

- **Single-click open in the file tree**: `NSOutlineView` selection
  notifications / target-action did not fire reliably. `ClickOutline` overrides
  `mouseDown:` to compute the row from the click point and open directly.
- **Sidebar collapsed to zero width** on launch: the split divider must be
  positioned AFTER the window is on screen and laid out. The editor / terminal /
  browser are laid out by hand in a `PanelHost` view, not nested split views —
  that was far more predictable.
- **The editor was wider than its pane** from the first commit until
  2026-09-25: `horizontallyResizable` was on, so the text view kept the
  window-wide frame it was created with and wrapped lines past the right
  edge. It is off now, and `PanelScrollView tile` sets a wrapping text
  view's width to the visible width.
- **Terminal resize bar**: the `DragBar` is a 12px grab area drawing a 1px
  line, and `relayoutRightArea` re-raises it to the top of `rightArea`.
  Otherwise a panel added later (the browser) covers half of it. Its cursor
  comes from a tracking area, because cursor rects lose to the overlapping
  text views.
- **Cmd+B collapse** fought the split delegate's 160px min; a `sidebarCollapsed`
  flag lets the minimum drop to 0.
- **Hiding panes**: two separate mechanisms. (1) Dragging the sidebar divider
  far right collapses the whole right pane via NSSplitView
  (`canCollapseSubview:` YES for `rightArea`; `effectiveRect:` widens the 1px
  divider's grab area to 5px each side; min pane width 150 so the drag doesn't
  visibly stall). (2) Shift+Cmd+E, or dragging the terminal bar to the top,
  sets `editorHidden`: only the file editor goes, and `relayoutRightArea`
  gives the terminal the full height (bar pinned at the top edge so it can be
  dragged back down). If that leaves the pane empty, `collapseRightAreaIfEmpty`
  collapses it. Opening a file, Cmd+1 and the preview toggle call
  `revealEditor`; showing the terminal/browser calls `showRightArea`. Headless,
  a real split-view drag can be tested by posting LeftMouseDragged/Up events
  with `postEvent:` and then calling the split view's `mouseDown:` (its
  tracking loop dequeues them).
- **Custom hotkeys**: prefer real MENU items with key equivalents. They work in
  every pane (like Cmd+C) and for shift-variants that share a letter (Cmd+G vs
  Cmd+Shift+G both work). A local key monitor was flaky; it now handles only
  Ctrl+` (which is awkward as a clean menu item alongside Shift+Cmd+T).
- **Window tabbing**: `setAllowsAutomaticWindowTabbing:NO`, otherwise macOS
  merges windows into tabs and Cmd+W closes the whole group.
- **Two app instances**: launching the `.app` twice just activates the existing
  one. The CLI launcher (`Contents/Resources/minicode`) runs the raw binary
  detached, which does start a separate process.
- **Terminal**: `zsh -l -i +Z` (interactive, line editor off) on a pty from
  `forkpty`. `ZDOTDIR` points at a generated `.zshenv` (in
  `NSTemporaryDirectory()/MiniCode-zsh`) that hands `ZDOTDIR` straight back to
  the user, loads their files, and on the first `precmd` installs hooks that
  emit OSC 133 A/C/D + OSC 7 and blank `PS1`. Installing at first precmd is
  what keeps the hook last, after .zshrc. Echo is off while zsh reads a
  command (precmd `stty -echo`) and on while it runs (preexec), so the pty's
  ECHO flag, read from the master with `tcgetattr`, tells us when a program
  wants hidden input. Everything the child needs is built before `fork`: only
  async-signal-safe calls between fork and exec. `PROMPT_SP` prints before
  precmd, so it is unset in .zshenv. `TERM=xterm-256color` so tools emit
  color. Pagers are no longer forced to `cat` (the grid runs them). The last line of
  the output view is "live": the stream re-sends it whole on every change and
  the view replaces text from `_liveStart`. Panel-written lines (headers,
  exit statuses) must go through `ensureNewline`, which ends the live line and
  calls `breakLine()` on the stream, or the next update overwrites them. When testing headless, give the shell a scratch
  `HOME` and `ZDOTDIR`: `/etc/zshrc` sets `HISTFILE` from them, and test
  commands otherwise land in the user's real `~/.zsh_history`.
- **Terminal links (Cmd+click)**: `TermLinks` finds candidates in a line;
  `TerminalView linkInLine:atByte:` resolves a file against the shell's cwd
  (from OSC 7), then `projectRoot`, keeping only paths that exist (cached,
  emptied on every output chunk and cwd change), resolved with
  `stringByResolvingSymlinksInPath` so /private/tmp matches a /tmp root.
  The log view maps a click through `characterIndexForPoint:` (checked
  against that character's own rect, since it answers with the nearest
  character past a line's end) and UTF-16 -> the line's UTF-8 bytes; the
  grid builds each row's UTF-8 with a byte -> cell map (wide characters'
  right halves add no bytes). The hover underline in the log is a real
  storage attribute, restored afterwards, because TextKit 2 ignores
  underline rendering attributes; output arriving clears it first, since
  the live line is replaced whole. Cmd pressed or released without moving
  is caught by a local flagsChanged monitor (flagsChanged: only reaches the
  first responder, usually the input line). EditorController
  `openTerminalLink:` opens URLs in the browser panel, folders in the tree,
  and files at `goToLine:column:` (switching a preview to source first). A
  path wrapped across two rows of the grid is not found. The GTK port
  (Ctrl+click, `linux/src/Terminal.cpp`, `linux/HANDOFF.md` item 9) reads
  VTE's logical lines, so a wrapped path is found there. Its trap: VTE's
  scroll adjustment and its text calls number rows differently once `clear`
  has dropped the scrollback (`Terminal::topRow`).
- **Terminal grid mode** (`feat/terminal-screen`): every pty chunk goes to
  BOTH `TerminalStream` (log) and `TerminalScreen` (grid), so either is
  current when shown and no mid-chunk handoff is needed. `updateMode` shows
  the grid while `_screen.altScreen()`, or while `_stickyGrid`: a 0.1 s timer
  (running only between OSC 133 C and A) reads termios from the master, and a
  command that stays out of ICANON for two polls without the alt screen
  (git's `less -FRX`, python REPL, ssh) keeps the grid until its command
  ends. The two-poll debounce is what stops `less -F` on short output from
  flashing the grid. The pty size comes from the panel bounds and the grid's
  cell metrics in both modes (the log's `lineFragmentPadding` is 0 so its
  wrap column matches), so switching modes never sends SIGWINCH. Tiny or
  collapsed bounds keep the old size, or the screen would be cut to 20x5.
  Screen replies (`\e[6n` -> CPR) are written back after each chunk; vim
  sends two DSRs at startup. `TerminalScreen` must stay copyable: an earlier
  version kept a `Grid*` into itself and every copy drew the original's grid.
- **Synthetic key events in tests**: Shift+Cmd+H only matches the menu with
  `charactersIgnoringModifiers:@"h"` (lowercase). An arrow key event needs
  `characters` = the function-key character (U+F701 etc.); an empty string
  makes `-[NSMenu performKeyEquivalent:]` throw and abort the app. Drive
  keys with `[NSApp sendEvent:]` from a background thread via dispatch_sync
  to main; that exercises the real menu-then-first-responder routing.
- **Images in the Markdown preview** (`src/MarkdownImage.{h,mm}`): the
  parser gives `![alt](src)` its own run (`image`, `src`; a badge's
  `[![alt](src)](url)` also sets `link`), and the Mac inserts an
  `MCMarkdownImage` attachment. Its cell (`MCAnimatedImageCell`) sizes the
  picture to the line's width and, for a GIF, draws the current frame and
  advances it on a timer that runs only while the picture is on screen.
  Local files only, relative to the Markdown file; web images show their
  alt text. Traps: 1.4.1 shipped with a TextKit 2 view provider, which the
  TextKit 1 editor never asks for, so GIFs were still; and setting
  `attachmentCell` in the attachment's init reads back nil, so the cell is
  returned from an `attachmentCell` override. Linux and Android show the
  alt text for now.
- **Markdown**: block elements call `ensureLineStart` so they aren't glued to
  the previous paragraph; headings get `paragraphSpacingBefore`. Tables: the
  parser lays them out as aligned monospace (what Linux and Android show)
  and also tags every run with its table, row, column and alignment
  (`tableId`/`tableRow`/`tableCol`, -1 for padding and rules). The Mac skips
  the padding and builds an `NSTextTable` from the cells
  (`appendMarkdownTable:`), so wide tables wrap inside their columns.
  Every run also carries its source `line`, stored on the rendered text as
  `kMarkdownSourceLine`: Shift+Cmd+P uses it to open the preview at the
  caret, and to open the source at a preview the user scrolled (otherwise
  the source comes back with its old selection and caret height). Links are
  real `NSLinkAttributeName`s, resolved in `openMarkdownLink:`: `#heading`
  by GitHub's anchor spelling, web addresses in the browser panel, other
  paths relative to the file through `openTerminalLink:` (`#L12` works).
- **Editing Markdown from the preview** (`src/MarkdownEdit.{h,cpp}`, pure
  C++ and tested; `src/MarkdownEditPanel.mm`; `editMarkdownAtCharacter:`):
  a double-click on the read-only preview (`CodeTextView
  onPreviewDoubleClick`) reads `kMarkdownSourceLine`, plus
  `kMarkdownTableCell` (row, column) in a table, and `MarkdownEdit::blockAt`
  classifies source lines the way the parser does to find the block:
  heading text, list item text, whole paragraph or quote, code between the
  fences, or one table cell (newlines and bare pipes are escaped on the way
  back). The popover works like the LaTeX one (Return saves, Shift+Return
  types a newline, Add item on a list item). `applyMarkdownSource:` splices,
  marks the buffer dirty, registers undo with the window's undo manager and
  re-renders at the same scroll. An edit is dropped if the buffer changed
  while the popover was open.
- **The Linux preview does the same** (2026-09-26): `linux/src/Markdown.cpp`
  returns a `Markdown::Page` (spans of rendered text with their source line
  and link, heading anchors via `MarkdownParser::anchor`, and the embedded
  widgets). Tables are a `GtkGrid` of wrapping labels and pictures an
  `MdPicture` (its own small widget, since GtkPicture always asks for the
  image's full size and a text view grants it), both in child anchors and
  sized by `Markdown::fit` from the pane's width. A label's natural width is
  pinned by `max_width_chars = 1` plus a width request, so it wraps. The
  editor (`Editor.cpp`) handles clicks, the popover, snapshot undo (Ctrl+Z
  in the preview; the buffer holds rendered text) and the toggle position.
  Two traps: act on a link from an idle, because the text view moves the
  caret after the click and undid a scroll to `#section`; and
  `gtk_text_view_get_iter_at_location` returns the *nearest* character, so a
  click can land a pixel left of the character's box (reject only a point
  past a line's end). Checked in the Docker container: links, anchors,
  GIFs playing, tables wrapping, list and cell edits, undo and redo, and the
  three toggle cases. Android shows none of this yet. Table cells are inline-parsed and padded by *display*
  width (code points, CJK/emoji = 2), never UTF-8 byte length, or any
  non-ASCII cell knocks the columns out of line.
- **Search**: scoped to a folder (default = open folder or selected folder),
  min 2 chars, generation bumped up front + per-file cancellation, ANSI stripped
  from result lines. On Linux the tree's `GtkSingleSelection` must have
  autoselect off, or its first row counts as selected from launch and the
  search is scoped to whatever folder sorts first.
- **Highlighting is incremental** (macOS, Android, where `Highlighter.kt`
  does the same over a native mirror of the text, and Linux, where
  `Editor.cpp` keeps a UTF-8 mirror and retags large ranges in idle slices;
  `linux/HANDOFF.md` item 3). Opening a file runs `applyHighlighting` (full pass, also used by
  `recolorEditor` on a settings change). After that the text storage delegate
  `textStorage:willProcessEditing:` feeds every character edit to
  `IncrementalHighlighter` and records the lines to recolor; `textDidChange:`
  (or a `performSelector:afterDelay:0` for storage edits without
  `didChangeText`) calls `flushHighlighting`, which lexes those lines from the
  stored states and repaints just them, swatches included. Two traps: (1)
  never change attributes inside `willProcessEditing`: it widens the edited
  range and NSTextView moves the caret to its end, so typed characters land
  on the next line. (2) Reset a range with one `setAttributes:` of the source
  attributes, never `addAttribute:` of the plain color: over a range with many
  attribute runs that took 3.4 s for 50k lines, `setAttributes:` takes ~1 ms.
  `_liveHighlight` must be off whenever the storage holds something that is
  not source (Markdown preview, messages); set it off before every
  `setAttributedString:`. Measured on a 50k-line file: ~0.35 ms of
  highlighting per keystroke (the rest of NSTextView's ~2.5 ms is its own),
  15 ms to type `/*` at line 100, ~60 ms for the worst recolor.
- **Color picks overwritten by the text color**: an NSTextView with
  `usesFontPanel` (the default) calls `updateFontPanel` when its selection
  moves, which sets the shared NSColorPanel to the color under the caret,
  and the panel forwards that as an action to its target, `colorPicked:`.
  So after every pick (which moves the caret) the line was rewritten with
  #D4D4D4, the editor's text color. Found by the settings demo scene, from
  the stack trace of `colorPicked:`. The editor now sets
  `usesFontPanel = NO` (MiniCode has no use for the Font panel).
- **Crash on window close**: `NSWindow` defaults to `releasedWhenClosed = YES`,
  a legacy manual release. With ARC also owning the window through a `strong`
  property, closing double-frees it ("MiniCode quit unexpectedly"). Set
  `window.releasedWhenClosed = NO` so ARC is the sole owner.
- **Linux: accelerators take keys from the terminal.** GTK runs application
  accelerators in the window's capture phase, before the focused widget, so
  a plain Ctrl shortcut never reached bash or vim in the VTE panel, and a
  key controller on the terminal cannot get in first. `main.cpp` unbinds the
  plain Ctrl ones while the terminal has the focus (`updateShellKeys`). The
  Mac is spared by using Command.
- **Linux: the file tree.** The stylesheet's transparent row background
  also wiped out the theme's selection color (an application stylesheet
  outranks the theme whatever the selectors), so a clicked row showed
  nothing; `ThemeCss.cpp` now colors `:selected`, `:hover` and
  `:focus-visible` itself. One click acts, as on the Mac, through a
  `GtkGestureClick` on the list view with the rows not activatable, so a
  double-click cannot act twice (`FileTree.cpp`); not
  `gtk_list_view_set_single_click_activate`, which selects rows on hover.
  Showing or hiding dotfiles closed every open folder when a dotfile sorted
  into the root; `setShowHidden` reopens them.
- **Linux: scrolling to a line of a new file.** GtkTextView animates
  towards a position computed from estimated heights of lines not laid out
  yet, and ends in the wrong place (a match on line 250 left 166 to 208 on
  screen). `Editor::settleOnCaret` scrolls again once the view stops with
  the caret off screen. Check the visible rect in tests, not just the caret.
- **Linux: shifted punctuation accelerators.** `<Ctrl><Shift>period` never
  matches in GTK 4; bind the character (`<Ctrl>greater`), and exempt it in
  `shellOwns`. Ctrl+Shift+. exists for Toshy users, whose Ctrl+H is taken.
- **Linux: GTK criticals on a mid-scroll switch.** Unmapping a
  GtkScrolledWindow while its adjustment animates (GtkTextView scrolls to
  the caret with one) ends the animation twice on GTK 4.22 and logs two
  criticals. `linux/src/ScrollSettle.h` ends it from the child's unmap first.

## Distribution

- Source repo: `e-c-hansen/minicode` (PUBLIC). The release zip is published on
  this repo's own Releases, attached to the `vX.Y.Z` tag it was built from.
- Homebrew tap: `e-c-hansen/homebrew-tap` (PUBLIC), which holds nothing but
  `Casks/minicode.rb`. That file is *written* by `scripts/release.sh` from
  `packaging/minicode.rb`, which is the only copy anyone edits; the tap exists
  because `brew install --cask e-c-hansen/tap/minicode` needs a repo called
  `homebrew-<something>`, and nothing more.
- The cask clears the quarantine flag in `postflight_steps` (Homebrew 7
  deprecated the `postflight` block; `{{appdir}}` is the template token in step
  arguments), so the unsigned app opens cleanly. After editing the cask, check
  it with `brew style` from a tap checkout
  (`/opt/homebrew/Library/Taps/e-c-hansen/homebrew-tap`).
- Not notarized (that needs the paid Apple Developer Program). The full sign +
  notarize flow is scripted in `scripts/sign-and-notarize.sh` for when/if paid.
- The Android APK is attached to the same GitHub Release, signed with the
  release key at `~/.config/minicode/release.keystore` (password in the
  Keychain, service `minicode-android-keystore`). Every update must carry that
  signature, so the key must never be regenerated; the user keeps a backup.
  Users install by hand or through Obtainium; it is not on the Play Store.
- Homebrew is the only way MiniCode updates: the app has no updater, so the
  cask must NOT declare `auto_updates`, or a plain `brew upgrade` would skip it.

## Conventions

- No third-party dependencies, ever. New capabilities use system frameworks
  (e.g. planned media rendering uses NSImageView + AVKit, not VLC).
- End commit messages with the Co-Authored-By trailer.
- README / user-facing prose: plain human voice, real sentences, commas, no
  emoji, no LLM-ish phrasing. The user cares about this.

## What's next

See `ROADMAP.md`. In flight: the Mac git panel (see Current state). Candidates,
none started: the git panel on Linux (the core parser is shared); the
Markdown preview's links, tables, pictures and editing on Android; a gap
between a paragraph and a list that follows it in the preview (the parser
emits none); Android project
search and comment toggling;
Android on F-Droid; the other throng features discussed (terminals that
survive closing the app, switching between projects); notarization once the
user has a paid Apple Developer account (`scripts/sign-and-notarize.sh`).
