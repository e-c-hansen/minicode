# Viewers in the editor's slot

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## Viewers in the editor's slot (Mac)

- `openFileAtPath:` calls `resetViewMode` (mode flags cleared, text view
  read-only) and routes by kind before reading text; `refreshDisplay` always
  relays out (it once left an image on screen). Viewers save nothing
  (`setPlainMessage:@""`, `saveCurrentFile:` returns); `checkExternalChange`
  reloads keeping the place.
- `FileSniff::classify` (first 8 KB and name): a NUL byte means binary, never
  text, unless the head is UTF-16 with its BOM (`isUtf16Text`); SQLite's
  header means a database; Office, iWork, OpenDocument, ePub,
  archives and fonts go to Quick Look; a file that is not UTF-8 and has no
  NUL anywhere opens as Windows-1252 text (2026-11-18, sessions.md), one
  with a NUL past the head goes to Quick Look when it fails to decode;
  .csv, .tsv, .json, .ipynb and .html (.htm, .xhtml) get viewers. `MCFileViewers` per window; a tab's
  `viewerKind` is without text (Quick Look, big CSV, SQLite), over text (the
  CSV table, whose edits go through `setViewerText:actionName:reshow:` so
  dirty, undo, backups and saving work as for text) or beside text (the JSON
  outline, `besideTextShown`). Shift+Cmd+V flips them (`canTogglePreview`).
- **HTML pages** (2026-11-15, `HtmlPageView.{h,mm}`, `MCViewerHtml`, over
  the text like the CSV table; `FileSniff::Kind::Html`, `sniff:html`, `make
  guitest SUITES=html_page`): the page is the saved file, loaded with
  `loadFileURL:allowingReadAccessToURL:` scoped to the window's folder (the
  file's own when outside it), in an `MCWebView` (declared in Browser.h
  now) with a non-persistent data store. It loads again, the scroll read
  and put back in `WKContentWorld.defaultClientWorld`, when the file's
  stamp moved: from `showText:` (a toggle back, a clean tab following the
  disk) and from `recordStamp:ofTab:` (a save, Keep Mine) through
  `htmlFileMayHaveChanged`. Every main-frame navigation away from the
  tab's own file (a link, a script, a drop) is cancelled and handed to
  `openMarkdownLink:`; a target=_blank too. **Scripts** (the owner's call,
  2026-11-15, against "nothing runs because a file was opened"):
  `html.scripts` (ask, on, off); with ask, a page whose text
  `FileSniff::htmlMayRunScripts` flags shows with
  `allowsContentJavaScript` off under a bar (Run Scripts, Don't Run,
  Remember for every HTML file, which writes the key through
  `AppSettings writeValue:`); an answer not remembered is kept per file
  until quit. An answer leaves the bar as a line saying what was chosen
  (`noting`, a × to close it) until another file shows: the bar going at
  once moved the page up under the pointer (round 8, D1). Another tab showing leaves the page loaded, hidden, its
  media paused (macOS 12; macOS 11 drops it), so it comes back scrolled
  where it was. Cmd+1 and the toggle give it the keyboard through
  `MCFileViewers focusInWindow:` (`grantKeyboard`); nothing else does.
  **Beside the source** (Control+Command+V, 2026-11-19,
  `EditorController+PageSide.mm`): a second `MCHtmlPage`, the window's
  (`_sideHtml`), shows the buffer (`showText:path:`), again 0.4 s after
  typing stops (`pageSideSourceChanged`), keeping the scroll as a reload
  does. When the text is what the disk holds it loads the file
  (`loadFileURL:allowingReadAccessToURL:`, the window's folder); else
  `loadHTMLString:baseURL:` with the file's URL, which WebKit lets read
  the file's folder, so relative stylesheets and pictures still load (the
  navigation is the tab's own file, so the policy allows it). Scripts
  follow the same rule and answers. The GUI check is html_page's last
  steps; Linux has no side page for HTML.
  The GTK port shows .html as its page too (`linux/src/HtmlView.cpp`, the
  rules in `src/HtmlPage.{h,cpp}`, `linux/HANDOFF.md` item 83); this file's
  .mm keeps its own copy of those rules.
- **Images** (not SVG, which is source): `NSImageView`, scaled down never up,
  `animates`, size set to pixels (a 144 dpi screenshot drew at half size); one
  that fails to decode falls through to the text path's "Cannot display".
  The review and commit pages show a binary picture's versions the same
  way (sized from pixels, never past 320 points tall), over empty rows of
  their own (git.md, "Pictures" in the review view).
  **PDFs**: `PDFView`, reload keeping page, point and scale; a new one
  opens at the top of its first page (`MCPDFShowFirstPageTop`, PageZoom.mm,
  the LaTeX preview's too; until 2026-11-17 a tall page opened scrolled to
  its foot, which read as a blank page), and a tab shown again its own place.
- **Video and audio** (`+isMediaPath:`): `AVPlayerView`, paused on the first
  frame; `stopMedia` (from `resetViewMode`, window close, dealloc) drops the
  player so sound stops; size (rotation applied) and length arrive by KVO;
  Shift+Cmd+Space plays from any pane (Cmd+Return is the commit box's). `.ts`
  and `.mts` are TypeScript; `asset.playable` is checked first; WebM, mkv, ogv,
  wmv and flv get "Cannot display".
- **Quick Look**: `MCQuickLookSlot` (`QLPreviewView`) in a tab;
  `MCQuickLookPanel` for Space in the tree, where `ClickOutline` is the panel's
  controller and keeps the keyboard. It follows arrows and clicks; Space closes
  it only for the tree that drives it (another window's tree, or one whose
  control ended, gets its selection shown); a space within a second of a
  typed letter is type-select's.
- **CSV and TSV** (`Csv`, `CsvTable.mm`): rows keep their bytes and endings,
  fields their bytes and quoting, so an unedited file round-trips and one cell
  edit changes one line; BOM and a missing final newline kept; a touched row
  of one empty field is written `""`; delimiter guessed (it must split most
  lines as it splits the first). Over 50 MB read-only, 100,000 rows from
  disk. **One table view serves every CSV tab**: a cell being typed into is
  committed in `stashActiveTab` and the toggle (`commitEditing`), and
  dropped before `showText:` replaces the document, or NSTableView commits
  it into the next file at the old row. Undo steps keep only the changed
  splice (`registerViewerUndoFrom:`); going back from the text to the table
  turns its NSTextView typing steps (target: the text storage) into one
  "Typing" step from `MCTab.textSessionStart`, since the table empties the
  text view they were recorded against. **Trap (round 9)**: the grid's
  `validateMenuItem:` ended in `[super validateMenuItem:]`, which
  NSTableView does not have (it validates through
  `validateUserInterfaceItem:`), so validating any menu item the grid was
  the target of threw, AppKit dropped the key, and no menu key worked while
  the grid had the keyboard: Shift+Cmd+P "did nothing". Before calling
  super in a validation, check the superclass has it. `previews` now
  validates every menu item against each viewer's keyboard and presses
  Shift+Cmd+P and Cmd+P there (CSV table and a cell being typed into, the
  JSON outline, SQLite's lists, a notebook, an image, a PDF, a movie, an
  HTML page, a Markdown page, the compare view, the review and commit
  pages). The palette closing hands the keyboard back to a page too
  (`grantKeyboard`), which refused it before.
- **JSON outline**: beside the text; a node selects its text with the keyboard
  kept in the outline, the caret selects its node; re-parsed 0.3 s after
  typing, the last good outline kept under an error line, dimmed and
  selecting nothing (`stale`: its offsets are off). `.jsonc`, tsconfig*.json,
  `.vscode/*.json` and the like allow comments (`JsonOutline::allowsComments`).
  **Its error is a problem too** (2026-11-18, `Lsp+JsonCheck.mm`, core
  `JsonOutline::problems` and `checksAsJson`, `jsonoutline:problems`, `make
  guitest SUITES=problems`): a JSON file no server has (`_servers` empty)
  is parsed on a queue when it comes on show and 0.3 s after typing; the
  error is the list of a server of the session's own, `mutible.json`, in
  `_store`, so squiggle, Problems, the status bar's count, hover and F8
  are a server's (`paintDiagnostics` and `collectProblems` take
  `jsonCheckedUri` where `_uri` is nil; `problemsKnown` is YES meanwhile,
  so a valid file says "No problems"). As VS Code says them: "Trailing
  comma" on the comma, "Comments are not permitted in JSON" over a comment
  in strict JSON, else the parser's message on the token; an empty file is
  none. JSONC files (allowsComments) parse leniently. A server attaching
  (attachDocument) erases the editor's list; a closed tab's goes in
  `tabsChanged`; `stopAllServers` clears the store, and documentOpened
  checks again. Over 8 M characters: no check. **Linux** (2026-11-19,
  `linux/src/LspSessionJsonCheck.cpp`, HANDOFF item 114): the same, with
  the check on a detached thread whose answer comes back by `afterMs`
  under the session's `life_` and a generation; `documentChanged` checks
  when neither `server_` nor `alsoServer_` has the file (servers off
  included), `shownUri()` stands for `uri_` in painting and F8, and the
  GTK `problems` suite checks it through the Problems list's rows.
  Every row is one line of constant height (2026-11-18): the label and the
  summary go through `JsonOutline::oneLine` (line breaks, tabs and other
  control characters as `\n`, `\t`, `\u2028` escapes the Mac draws dimmed,
  cut at `kLineChars` characters with an ellipsis before any layout), the
  attributed string carries a truncating-tail paragraph style, and the field
  is `usesSingleLineMode`. The row's tooltip (`JsonOutline::tooltip`) gives
  a string's value decoded from the text (`decodeString`, to
  `kTooltipChars`, longer than the preview and with its line breaks) and a
  key the row had to escape or cut. GTK's rows use the same `oneLine` text
  (escapes not dimmed there). Trap: an attributed string's own default
  paragraph style word-wraps and overrides the field's `lineBreakMode`, so
  a long value (or one with line breaks) wrapped inside its fixed-height row
  and drew over the rows below; `previews` checks every row's field sits in
  its row and needs one line (`cellSizeForBounds:`).
- **Notebooks** (`Notebook`): nbformat 4 (and 3's cells) to Markdown, rendered
  read-only (`showsRenderedMarkdown`); text outputs as blocks fenced
  ```output (`Notebook::kOutputLanguage`), which the Mac draws unboxed with
  a left rule in the text color (2026-11-15), and Jupyter's prompts after
  the language (```python In [3], ```output Out [3] on a result), which
  the parser keeps as `MdRun::label` and the Mac sets as a small muted line
  above the block (`markdownLabelFor:`); GTK and Android show plain code
  for now, pictures as
  data: images (SVG too, base64'd), HTML as text, an output past 5,000 lines
  or 512 KB cut to its first and last 1,000 (`shortenOutput`). Nothing runs.
- **SQLite** (`SqliteView.mm`): `SQLITE_OPEN_READONLY` with a `mode=ro` URI,
  query_only and the defensive flag; a writing statement
  (`sqlite3_stmt_readonly`) refused before it runs, and an authorizer refuses
  BEGIN, SAVEPOINT, ATTACH and setting pragmas (a BEGIN held a lock other
  writers waited on); only the first statement; a progress handler stops a
  query at 5 s; one background queue, opening included; 1,000 rows. A WAL
  file with no `-wal` beside it opens with `immutable=1` (read-only cannot
  make the WAL files); a failure is shown in the tab. A reopen keeps the
  table or query on show. NULL is `NSNull` in the rows (`isNullAtRow:`).
- **Diffs and pages**: git diffs and commits in `diffScroll`; the side-by-side
  diff is `CompareView` (`showComparison:preview:`, `MCComparison`, panes
  scrolling together); an `EditorPage` (`showPage:`) is a read-only view in a
  tab, reused by key (Evidence, a worktree's changes).
- **The compare view** (2026-11-07, W16): `MCComparison prepare` (off the
  main thread where callers can) works out each pane's text, syntax spans
  (by `fileName` or `path`, plain past 2 MB a side) and changed words
  (`SideBySide::wordChanges`, budgeted) in the core; F7 and Shift+F7 step
  through `SideBySide::hunks` (VS Code's accessibleDiffViewer ids), the
  compare view answering them. A comparison with a decision gets a ✓ and ✕
  per hunk in a gutter between the panes (`rectForHunk:accept:`,
  `pressHunkButtonAtPoint:`, for tests); `acceptedText` is
  `SideBySide::assemble` of the accepted hunks (the right text when all).
  The last hunk's choice sends the answer only after `lastChoiceGrace` (3 s;
  `sending`, the bar's Send Now), so a misclick can be pressed again and
  taken back; the bar's own buttons and closing the tab send at once.
  Trap: laying both panes out to size them took 1.5 s on the main thread
  for 50,000 lines a side; every row has one fixed height (a paragraph
  style), so sizes and row rects are arithmetic and layout is
  non-contiguous. Keep anything that would lay out the whole text out.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- CSV and TSV: **Trap (round 9)**: the grid's `validateMenuItem:` ended in `[super validateMenuItem:]`, which NSTableView does not have (it validates through `validateUserInterfaceItem:`), so validating any menu item the grid was the target of threw, AppKit dropped the key, and no menu key worked while the grid had the keyboard: Shift+Cmd+P "did nothing".
- JSON outline: Trap: an attributed string's own default paragraph style word-wraps and overrides the field's `lineBreakMode`, so a long value (or one with line breaks) wrapped inside its fixed-height row and drew over the rows below; `previews` checks every row's field sits in its row and needs one line (`cellSizeForBounds:`).
- Viewers in the editor's slot (Mac): Trap: laying both panes out to size them took 1.5 s on the main thread for 50,000 lines a side; every row has one fixed height (a paragraph style), so sizes and row rects are arithmetic and layout is non-contiguous.
