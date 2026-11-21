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
  tab's own file (a link, a script, a drop) is cancelled; a link the user
  clicked or a form they sent (`WKNavigationTypeLinkActivated`,
  `FormSubmitted`) goes to `openMarkdownLink:`, a target=_blank too; one
  the page started by itself (a meta refresh, a script setting location,
  `window.open`) only when it is http(s) and the page's scripts run
  (`MCHtmlMayFollow`), else it goes to `onRefused` and the status bar
  names it (`htmlPageRefused:`). **Trap** (round 10's documents defect
  2, 2026-11-19): a `<meta http-equiv="refresh">` to `zoommtg://` (any
  app's scheme) went to `openURL:` with no click, and an https one
  loaded in the browser pane; `html.scripts` never applied, since a
  meta refresh is not a script. `html_page` loads both kinds and checks
  nothing reached `openURL:` (`MCGuiOpenedURLs`, the harness records
  it). GTK follows the same rule from the core (`HtmlPage::mayFollow`,
  `htmlpage:follow`; HANDOFF item 121), but WebKitGTK cannot tell the
  main frame at the policy check, so `HtmlPage::policy` refuses there,
  in any frame, a jump the page made by itself to a scheme no frame
  loads (`loadsInFrame`: an app's), and the main frame's to a web
  address or a file is stopped at its response check
  (`is_main_frame_main_resource`) or its load's failure, the page left as
  it was: putting it back by loading it again fired the refresh again.
  Refused ones go to `Host::refused`, the status bar's note in the Mac's
  words (`refusedNote`); the GTK html_page checks a link spy
  (`HtmlView::linkSpy`) and `MCGuiLaunchedUris` (the harness's recorder
  in `Editor::launchUriHook`, so no check starts another app).
  **Trap** (2026-11-19, after #249): a page whose scripts run can click
  its own link (`a.click()`) or send its own form (`form.submit()`), and
  WebKit reports those as `LinkActivated` and `FormSubmitted`, the
  user's, so such a page still sent `zoommtg://` (any app's scheme) to
  `openURL:` with no click. `WKNavigationAction` has no public sign of a
  real click (`buttonNumber` and `modifierFlags` read a left button and
  none for `a.click()` as for a click; `_isUserInitiated` is SPI, not
  used), so every followed link to another app
  (`HtmlPage::opensAnotherApp`: any scheme but http, https and file)
  asks first in a sheet naming the app (`URLForApplicationToOpenURL:`)
  and the address (`openAppQuestion`, `openAppDetail`, `htmlpage:apps`),
  Open the default: one question per click by hand. One sheet at a
  time; another meanwhile is refused (`onRefused`), and its box, Refuse
  any more from this page, refuses the rest until the page loads again,
  so a page clicking its link in a loop cannot hold the window.
  `html_page` checks a page's `a.click()` at load asks and, cancelled,
  reaches no `openURL:`, and a real click (mouse down and up handed to
  the web view) asks and opens. The GTK port can tell the two apart
  (WebKitGTK's `webkit_navigation_action_is_user_gesture`), and does
  (HANDOFF item 123; `HtmlPage::follow(nav, gestureKnown)`,
  `htmlpage:app-gesture`): a followed link to another app asks only when
  the user's click or key made it, in a `Question::ask` window over the
  app's with the Mac's words and buttons (Open, then Cancel; Return
  Open, Esc Cancel), and one a script made is refused unasked, named by
  `refusedNote` in the status bar. No box: a script's click never asks,
  so a page cannot hold the window. The app's name is its scheme's
  default handler (`g_app_info_get_default_for_uri_scheme`; none, "Open
  in another app?"). The GTK `html_page` checks a script's `a.click()`
  to smb:// and `form.submit()` to mailto: (refused, no question,
  nothing opened), a real click (XTest) on a zoommtg:// link (asks,
  naming the desktop file the suite writes in the scratch world; Cancel
  opens nothing, Open reaches `MCGuiLaunchedUris`), real clicks on an
  https and a file link (no question), and a Markdown preview's
  zoommtg:// link (unasked, as before, on both platforms). **Scripts** (the owner's call,
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
  **Trap** (2026-11-20, the taskbench's PDF at 2,210 ms against VS Code's
  1,451): a PDFView given a document runs PDFKit's page analysis, Vision's
  `VNRecognizeDocumentsRequest` (text, tables, forms) over every page it
  shows, on its own queues: an ML model compiled (Espresso) and a whole
  core busy for two seconds and more after even a two-page PDF of text had
  drawn in about 30 ms, again for every reload and every LaTeX typeset.
  The page was on screen at once; the benchmark's view waits for the app
  to go quiet, and `sample` showed the time was that analysis. It only
  gives selectable text to a page with none, so `MCPDFAnalyseOnlyScans`
  (PageZoom.mm, both PDF views, before the document is handed over) turns
  PDFView's undocumented `documentAnalysisEnabled` off unless the first
  page has no characters (`MCPDFAnalysesPages`: a scan keeps it), and
  does nothing on a macOS without the selector. `previews` opens a PDF of
  text, checks the switch is off, and bounds the process's CPU (every
  thread, `getrusage`) over the next 3 s against 1.5 s before it (the
  pass used a few hundred ms at once, then a whole core from about 1.5 s
  on, so a shorter window missed most of it); the image-only `paper.pdf`
  keeps it on. Taskbench after: the PDF step 392 to 436 ms (300 of it the
  quiet wait) over five runs, the viewers task's CPU 3,166 to about
  1,500 ms. The MP4 step, next but one, then shows CoreMedia's own work
  after the player is up (750 to 970 ms, about 500 ms of it carried into
  the step after), which the PDF's 2 s had covered.
- **Video and audio** (`+isMediaPath:`): `AVPlayerView`, paused on the first
  frame; `stopMedia` (from `resetViewMode`, window close, dealloc) drops the
  player so sound stops; size (rotation applied) and length arrive by KVO;
  Shift+Cmd+Space plays from any pane (Cmd+Return is the commit box's). `.ts`
  and `.mts` are TypeScript; `asset.playable` is checked first; WebM, mkv, ogv,
  wmv and flv get "Cannot display".
  **Trap** (2026-11-20, the taskbench's MP4 at 750 to 1,100 ms, the step
  after it as long again): the time was AVKit's, after the frame was up,
  not ours (the main thread's own part is about 50 ms cold: the view, the
  item, `playable`). Three things kept the app busy. (1) The transport bar
  (SwiftUI on macOS 27) given an item still loading animated into its
  ready state for about 600 ms at a tenth of a core; the bar is now put up
  only once the item is ready (`controlsStyle` None in `showMediaAtPath:`,
  Inline in `mediaItemChanged:`), and one made then starts in that state.
  (2) Left on the hidden player view when another file showed, the bar
  kept the app busy at the display's rate for another half second, which
  was the "spill" into the next step; `stopMedia` sets the style to None.
  (3) `allowsVideoFrameAnalysis` (macOS 13 on, default YES) runs VisionKit's
  Live Text over a paused frame, and a file here opens paused, so every
  video started an analysis (in the app and in mediaanalysisd), the PDF
  trap's twin; `analyseFramesOncePlayed:` turns it off for each new player
  and on at its first nonzero rate (`AVPlayerRateDidChangeNotification`),
  so a frame paused while watching can still be read (its Live Text
  button appears then). How it was found: `sample` showed a nearly idle
  process, since the work came in bursts at the display's rate that a
  1 ms sample mostly misses; the process's CPU per 50 ms
  (`proc_pid_rusage`) showed the tails, and switching each part off
  showed whose they were. Only a window on a visible screen does any of
  this: with the screen locked the MP4 opened in about 370 ms before and
  after, so measure on screen. `previews` opens `clip.mov`, checks Live
  Text off and the open's main-thread CPU, waits for the bar once the
  item is ready, plays it with Shift+Cmd+Space and waits for Live Text,
  then shows a picture and checks the player and its bar are gone. The
  Markdown page's player (MarkdownMedia.mm) is made on a click and plays
  at once, so it keeps AVKit's defaults.
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
  disk. **The header row** (2026-11-19, round 9: "the CSV header row is
  shown as data"; `csv:header`, `previews`): `Csv::looksLikeHeader` (no
  field of row 0 empty, a number or a date, and a column under it four in
  five numbers or dates in the first 200 rows; text alone is not guessed)
  picks it when a file comes on show; the reader's choice, from the right
  click menu's First Row Is Header (shown read-only too), is kept per path
  in the user defaults (`MCCsvFirstRowIsHeader`, the 500 newest). With it
  the grid's rows are the file's from 1 (`headerRows`), the column titles
  are row 0's fields in bold (an empty one keeps its letter; the letter is
  the header's tooltip), the number column gives the file's row, and the
  info line says "N rows under a header". The view's own API
  (`cellAtRow:`, `setCell:row:`, `rowCount`, the menu items' rows) stays
  in the file's rows; only the data source and Edit Cell map. The same
  file shown again (an edit, a reload) keeps the choice; it is guessed
  again only for another path. **One table view serves every CSV tab**: a cell being typed into is
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
  GTK (2026-11-21): `mutible diff` opens the agents' diff preview tab
  (`Preview::AgentDiff`, kept by `--keep`, a double-click or Keep Open; a
  decision tab never is one), a rejected hunk's right rows keep only a trace
  of tint (`CompareView::rowTint`), and a tab keeps its scroll
  (`Document::compareScroll`, saved in `Editor::stash`) while another shows.
  Trap: laying both panes out to size them took 1.5 s on the main thread
  for 50,000 lines a side; every row has one fixed height (a paragraph
  style), so sizes and row rects are arithmetic and layout is
  non-contiguous. Keep anything that would lay out the whole text out.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- Viewers in the editor's slot (Mac): **Trap** (round 10's documents defect 2, 2026-11-19): a `<meta http-equiv="refresh">` to `zoommtg://` (any app's scheme) went to `openURL:` with no click, and an https one loaded in the browser pane; `html.scripts` never applied, since a meta refresh is not a script.
- HTML pages (GTK): **Trap** (the same defect, 2026-11-19): the GTK port let a page's own main-frame load run to `WEBKIT_LOAD_COMMITTED`, then loaded the file again and handed the address to the link handler (`openMarkdownLink`, which gives any scheme but http(s) and file to the desktop's handler), and loading the file again fired its refresh again, so the page went round for as long as it showed. WebKitGTK says which frame only at the response check (`webkit_response_policy_decision_is_main_frame_main_resource`); stop it there, or at `load-failed` (an unreachable address, a missing file, where the default handler would commit an error page at that address), and the page never leaves. A scheme no frame loads is refused at the policy check, before anything loads.
- Viewers in the editor's slot (Mac): **Trap** (2026-11-19, after #249): a page whose scripts run can click its own link (`a.click()`) or send its own form (`form.submit()`), and WebKit reports those as `LinkActivated` and `FormSubmitted`, the user's, so such a page still sent `zoommtg://` (any app's scheme) to `openURL:` with no click.
- PDFs: **Trap** (2026-11-20, the taskbench's PDF at 2,210 ms against VS Code's 1,451): a PDFView given a document runs PDFKit's page analysis, Vision's `VNRecognizeDocumentsRequest` (text, tables, forms) over every page it shows, on its own queues: an ML model compiled (Espresso) and a whole core busy for two seconds and more after even a two-page PDF of text had drawn in about 30 ms, again for every reload and every LaTeX typeset.
- Video and audio: **Trap** (2026-11-20, the taskbench's MP4 at 750 to 1,100 ms, the step after it as long again): the time was AVKit's, after the frame was up, not ours (the main thread's own part is about 50 ms cold: the view, the item, `playable`).
- CSV and TSV: **Trap (round 9)**: the grid's `validateMenuItem:` ended in `[super validateMenuItem:]`, which NSTableView does not have (it validates through `validateUserInterfaceItem:`), so validating any menu item the grid was the target of threw, AppKit dropped the key, and no menu key worked while the grid had the keyboard: Shift+Cmd+P "did nothing".
- JSON outline: Trap: an attributed string's own default paragraph style word-wraps and overrides the field's `lineBreakMode`, so a long value (or one with line breaks) wrapped inside its fixed-height row and drew over the rows below; `previews` checks every row's field sits in its row and needs one line (`cellSizeForBounds:`).
- Viewers in the editor's slot (Mac): Trap: laying both panes out to size them took 1.5 s on the main thread for 50,000 lines a side; every row has one fixed height (a paragraph style), so sizes and row rects are arithmetic and layout is non-contiguous.
