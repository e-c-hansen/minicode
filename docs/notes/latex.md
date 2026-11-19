# The LaTeX preview

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## LaTeX preview (read before touching it)

- **tectonic**: `MUTIBLE_TECTONIC`, then `~/Library/Application
  Support/Mutible/bin` (0.17.0 is there; its cache,
  `~/Library/Caches/TectonicProject.Tectonic`, has text fonts but no math
  fonts), then PATH and Homebrew; missing, the panel offers the pinned
  download, checked against GitHub's published SHA-256 (`kTectonicSha256`,
  as the Linux port's) before it is unpacked, and only its `tectonic` file
  moved into place (`MUTIBLE_TECTONIC_DOWNLOAD` points the GUI check at a
  file:// stand-in, held to the same hash).
- **The buffer is typeset from a hidden sibling**
  `.<name>.<pid>-<view>.mutible.tex` in the document's folder, so relative
  paths resolve and the user's file is never written; deleted when tectonic
  exits. Before it is written, a `sibling.record` (pid, start time, size,
  SHA-256; `LatexScratch`) goes in the output folder, so the first typeset of
  a launch removes a copy a crashed process left, and only when it is still
  exactly those bytes. tectonic runs `--untrusted`. Output in
  `NSTemporaryDirectory()/Mutible-latex/<16 hex of the resolved path's
  SHA-256>-<pid>-<start>-<view>` (`MCLatexWorkFolder`, `LatexView
  workFolder`; `$MUTIBLE_LATEX_WORK` in the checks). **Each preview has its
  own sibling and folder** (`LatexScratch::Owner`, 2026-11-08, DO-D5): two
  windows on one file with different unsaved text once shared both, so one
  typeset's end removed the other's sibling and they overwrote one PDF. The
  folder goes with its preview (`stopPreview` on window close, dealloc on tab
  close); a folder whose process has gone (`ownerGone`) is removed by the
  first typeset of a launch; folders named by the hash alone (older
  mutibles) are left to their own records. `make guitest SUITES=latex` runs
  a stand-in tectonic.
- Compiles are debounced (0.6 s) and generation-counted; a run has 5 minutes,
  then SIGTERM and SIGKILL, with a log line (a looping document spun a core and
  queued every later edit). Scroll and zoom are restored by page index and
  point. The first document opens at the top of its first page
  (`showFirstPageTop`, now and again on the next turn, unless a forward
  search placed it; the PDF view's inner scroll view takes no automatic
  insets): PDFView alone left it part way down, the title under the bar.
- **Unknown commands are looked through, not skipped**: `LatexDoc` skips only
  known machinery (`\label`, `\includegraphics`, `\setlength`, ...) and enters
  the first braced argument `looksLikeProse` accepts (not `{0.5em}`, `{l}`,
  `{sec:intro}`, `{GDM.png}` or URLs; yes `{2014 - 2019}`). A URL disqualifies
  only a group that is one unbroken token; a group holding commands is always
  entered. Letters spelled as commands (`\ss`, `\o`, `\LaTeX`, `\c{c}`,
  `\'{e}`) stay in the run (`kTextSymbols`, `kLetterAccents`) and `displayText`
  reads them; `\\` ends a run like `&`; `\url{...}` is a field.
- **A click that matches nothing refuses** (null rather than the nearest span
  whenever there was a real word); text TeX makes up is refused. **A single
  letter or digit is always refused** (page, section, list and footnote numbers
  fell through to the nearest span), accepted by the user at the cost of
  one-letter words, except a Han, kana or Hangul character whose surroundings
  agree with the source.
- **Click to source** (`MCLatexSpanAtPoint`, which the sweep calls): lines from
  `SyncTexIndex::textHitsAtPoint` (a box carries the line its paragraph closed
  on, glue and kerns each word's line: right of the click first, then left,
  then boxes; glue on a box's end edge goes left first); the word from
  `selectionForWordAtPoint:` plus 40 characters each side, rejoining
  hyphenation (`MCJoinHyphenation`); `LatexDoc::matchKey` on both sides
  (letters and digits, lowercase, ligatures split, accents dropped; three
  characters or fewer must be whole words, except in math); choice by context,
  a later candidate beating the nearest only by 4 characters (a tabular reads
  back by column). Spans cover all their lines; `\title`/`\author`/`\date`
  count at `\maketitle`; a word split by a font change matches a join with
  balanced braces; a footnote mark glued to a word is retried without digits.
- **Measure before changing the matcher**: `tests/latex/sweep.sh doc.tex [-v]`
  double-clicks every word of every page and reports found, refused, wrong and
  not placed (scratch `TECTONIC_CACHE_DIR`). The bar is 0 wrong; the user's
  resume is at 100% and `tests/latex/torture.tex` at 98.7% (the rest rightly
  refused). `make latex-sweep` (`sweep.sh --fixture`, a step of CI's macOS
  job) clicks the committed typeset of torture.tex (`tests/latex/fixtures`,
  tectonic 0.17.0) with no tectonic, and fails on a wrong answer or a rate
  under `tests/latex/sweep_bound.txt`; raise the bound when a change raises
  the rate, and typeset the fixture again (`--synctex --untrusted`, the
  sibling named `.torture.mutible.tex`, Input paths rewritten to
  `/fixture`) only with the tectonic the app pins. `build/latex-sweep` links
  the app's objects (the old one-line clang build had lost frameworks).
  Trap: `characterBoundsAtIndex:` drifts from `page.string`'s indices down a
  page; a one-character `selectionForRange:`'s bounds do not.
- **The edit box keeps its draft** (2026-11-19, as #223 made the
  Markdown page's: markdown.md, "The box keeps its draft"): the popover
  is semi-transient, so an app switch leaves it open; `cancelEdit:` (Esc,
  Cancel) is the one way to drop the text; `commitEdit:` (Return, Apply)
  and AppKit closing it any other way (`popoverWillClose:`, unless
  `_editSettled`) apply it through `applyEditText:`; a second double-click
  applies the open box first; the tab leaving (`stashActiveTab`), Cmd+S,
  closing a tab or the window, and quitting do too (`commitPendingEdit`
  through the controller's `commitPageEdit`). The button says Apply.
  `latex` checks it offscreen (posted resign and become-active
  notifications, `popoverWillClose:` by hand).
  **Apply edits the text as it is then** (2026-11-19, round 10's
  documents defect 3): the box keeps the text it opened on (`_editBase`,
  an input file's `_editingSrc`), and `applyEditText:` makes the edit on
  the tab's text now (`sourceNow`: the editor beside the page, else the
  tab's buffer), the span (adding an entry, its list) found again by
  `MarkdownEdit::rebaseSpan` (`mdedit:rebase`): the same offsets when the
  change is after it, shifted when before, else the one place holding its
  exact bytes. Not found once, nothing changes: the draft goes to the
  clipboard and the bar says so (`keepLostEdit:file:`). `latex` has an
  agent rewrite the file while the box is open, in the full preview and
  beside the source, and the rewrite of the edited sentence itself.
  **Trap**: with the box kept open across an app switch, an agent's
  rewrite read into the clean tab had the draft spliced at the old
  offsets (full preview) or the old text plus the edit put back over the
  reload (beside the source), reverting the agent silently. Never splice
  a box's edit into text other than the one it opened on without finding
  the span again.
- **Edits are byte splices** of the span's own LaTeX (nothing escaped or
  re-serialized), marked dirty, saved only by Cmd+S, undone through
  `LatexView`'s own snapshot stack (50) via `undo:`/`redo:` in the full
  preview. An edit beside the source pushes nothing there (it is the
  source's undo step), and the stack is cleared when the text changes
  from elsewhere (`setPath:source:` with other text; `stepFrom:` checks
  `sourceNow` too). **Trap** (round 10's documents defect 8): an edit on
  the side page pushed a snapshot, and Cmd+Z in the full preview after
  typing in the source put that snapshot back, dropping the typing.
- **Clicks in `\input` files** (2026-11-15, the owner's yes to saving them
  from the preview): `MCLatexSpanAtPointInFiles` tries the inputs the
  boxes under the point name, likeliest first, then the document
  (`SyncTexIndex::tagsAtPoint`); an input is read from the disk, as the
  typeset read it, and parsed with `LatexDoc::parseInput` (all body text: a
  part has no `\begin{document}`, and `parse` found nothing in it); the
  span is then found again in its tab's buffer (`rebaseSpan`, then the
  buffer's own parse), and the box edits that. Not there once, exactly
  as the page shows it, the click is refused ("has unsaved changes to
  that text ... save it"). Show in Source maps its line the same way.
  **Trap** (round 10's documents defect 4): the input was parsed from its
  buffer, so SyncTeX's lines (the disk's) matched against a chapter with
  unsaved lines above the click picked another paragraph, and Return
  edited it. The
  edit goes into that file's tab (`EditorController+LatexInputs.mm`,
  opened behind the preview when not open, `putBackText:inTab:actionName:`
  "Edit in Preview", one step of that tab's undo, not the preview's
  stack), made on the file's text when applied, as the document's. The bar
  names input files with unsaved changes (tectonic reads them from disk);
  Cmd+S with the preview on show saves them (`saveLatexInputsOfActiveTab`),
  and any save of a file a preview read typesets it again
  (`latexFileSaved:`). The sweep clicks the three-file fixture too
  (`sweep_bound_inputs.txt`, 100%); its single-file rate is unchanged.
  Not done: Linux, and Cmd+Z in the preview undoing an input file's edit.
- **Export PDF** (Shift+Cmd+S): `pdfForPath:source:completion:` hands back
  tectonic's bytes (`_pdfData`, `_pdfSrc`), typesetting first if they are not
  the buffer's. Trap: a synthetic `NSEvent` for Shift+Cmd+S with "s" matched
  Save and wrote the file, with "S" nothing; `CGEventCreateKeyboardEvent`
  behaves like hardware.
- **Forward search** (Option+Cmd+J, `showLineInPDF:`, LaTeX Workshop's
  `latex-workshop.synctex`): `SyncTexIndex::boxesForLine` lights the lines of
  text holding the glue, kerns and void boxes the caret's line set (never a
  line box's own record, which names the line its paragraph ended on), on
  the first page it reached, else the nearest line below or above within
  40; square annotations for 1.5 s. It waits for a typeset when the PDF is
  not the buffer's (`_forwardLine`, run from `showDocument:`). From an
  `\input` file (`revealSourceLine:inFile:`, `SyncTexIndex::tagForFile`,
  realpath on both sides: tectonic writes absolute Input paths) the main
  document is a `% !TEX root = ...` comment's (`LatexLog::rootComment`),
  else, for a file with no `\documentclass`, the open tab whose preview's
  typeset read it (`typesetReadFile:`); its tab is shown.
- **Inverse search, the page to the source** (2026-11-19, round 9's D4:
  "Cmd-click on the page does nothing"): Command-click on the page
  (`MCPdfView mouseDown:`, before PDFView's own selection starts) or
  Show in Source at the top of its context menu (`menuForEvent:`) calls
  `showSourceForPage:point:`. It places the click as a double-click does
  (`spanForPage:point:file:source:hits:`, shared with
  `openEditorForPage:point:`, so `\input` files count), finds the clicked
  word in the span's LaTeX (`MarkdownEdit::sourceOffsetFor`, as the edit
  box selects it) and hands `onShowSource(path, line, column)` the word's
  line and composed-character column (path nil for the document); with no
  span, SyncTeX's first `textHitsAtPoint` line (refused for TeX's own
  files). The controller's `latexShowSource:line:column:` opens an input
  file's tab and calls `goToLine:column:` (one Go Back step; it leaves the
  full preview and lights the line). The bar says "⌘-click to show it in
  the source". Lines are the typeset text's (`_src`): beside the source
  with unsaved typing they may be a little off; Linux has none of this.
- **Shift+Cmd+V keeps the place** (2026-11-19): from the page,
  `togglePreview:` asks `sourceLineOnScreen` (core
  `SyncTexIndex::topLineInRange`, `synctex:top-line`: the document's
  line whose words' glue, kerns and math are highest in the shown part
  of the first visible page that has any, the leftmost of a row; a line
  of text's own box only when no word is there) and puts that line at
  the source's top, unless `sourceLineIsOnScreen:` says the caret's own
  line is on show, which then comes back as it was. To the page,
  `keepSourceLineInView:` runs a quiet forward search (`_forwardQuiet`:
  no light, no status, no move when any of the line's boxes is on show),
  after the typeset under way when the page is not the text's;
  `_forwardPlaced` keeps the first document's top-of-page placing from
  undoing it.
- **The page beside the source** (Control+Command+V, 2026-11-19,
  `EditorController+PageSide.mm`, `PageSidePane.{h,mm}`): the tab's own
  `LatexView`, framed at the right of the source by `layoutPageSideIn:`
  (after relayoutRightArea framed it for the editor's slot, so its
  frame wins); the source keeps the keyboard. It typesets on save
  (`pageSideSaved:` from `saveTab:`) and when it opens or its tab shows
  (`refreshPageSide`, as the full preview does on show), never on typing.
  A double-click on a page older than the buffer (`bufferText`, answered
  only while the page is beside the source) typesets the buffer and edits
  nothing (`catchUpWithBuffer`); an edit goes into the source as one undo
  step (`latexDidEditSource:` through `putBackText:inTab:actionName:`),
  and the view passes `undo:`/`redo:` up the chain (`respondsToSelector:`)
  so Cmd+Z there is the source's (the controller's `undo:` uses the
  tab's undo manager). `latexIsShowing` now means the full preview only.
  The caret's line is kept in view (`pageSideSelectionChanged`, 0.25 s,
  `keepSourceLineInView:`), and Option+Cmd+J lights it there without
  leaving the source. Command-click there moves the caret. Trap:
  `latexIsShowing` (undo routing, focus on a tab's showing) must stay the
  full preview only, or Cmd+Z beside the page steps the preview's own
  stack instead of the text's.
- **Problems on lines**: `LatexLog` reads tectonic's `error: file:line:
  message` summary (TeX's `! ...` blocks with `l.N` and the open-file
  parentheses only when no summary placed anything); the sibling's name is
  the document, an `\input` name is found as TeX finds it. The failure panel
  lists them above the log, each place a `mutible-latex-problem:<n>` link
  (`onOpenProblem`, goToLine); while the source shows and is still the text
  last typeset, they are `CodeTextView buildMarks`, gutter dots that follow
  edits and that no language server's publish replaces. **Warnings** too,
  from every typeset (2026-11-08): tectonic's own `warning:` lines (overfull
  boxes, placed at a paragraph's first line; underfull ones left out, as
  LaTeX Workshop's default) and TeX's transcript, kept with `--keep-logs`
  and removed once read (`parseTranscriptWarnings`: undefined references
  and citations, labels defined twice, package and class warnings with an
  input line; lines wrapped at 79 joined; the open-file parentheses skip a
  box's text, an error's quoted line and the warnings themselves). Errors
  first (`collect`); rows in `MCDiagnosticColor`; a typeset that worked
  lists them behind the status bar's Warnings button. Committed logs:
  `tests/latex/logs` (warnings.txt and .log, with their source).
- tectonic's bundle is older than Overleaf's: a missing package (`twemojis`)
  goes in the document's folder, and since tectonic discards docstrip's output,
  a script keeping a `.dtx`'s code lines made the `.sty`. Spacing differs from
  Overleaf by design (XeTeX; compare with Overleaf set to XeLaTeX).
- Headless: drive `openEditorForPage:point:`, `startAddItem:`, `commitEdit:`
  with points from `[PDFDocument findString:]`, on scratch `MUTIBLE_TECTONIC`
  and `TECTONIC_CACHE_DIR`.
- Linux (`Latex.cpp`, `PageWords.cpp`, `LatexClick.cpp`, poppler) differs on
  purpose (`linux/HANDOFF.md` item 7); `tests/latex/sweep-linux.sh` found the
  single-character problem.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- LaTeX preview (read before touching it): Trap: `characterBoundsAtIndex:` drifts from `page.string`'s indices down a page; a one-character `selectionForRange:`'s bounds do not.
- LaTeX preview (read before touching it): Trap: a synthetic `NSEvent` for Shift+Cmd+S with "s" matched Save and wrote the file, with "S" nothing;
- LaTeX preview (read before touching it): Trap: `latexIsShowing` (undo routing, focus on a tab's showing) must stay the full preview only, or Cmd+Z beside the page steps the preview's own stack instead of the text's.
- LaTeX preview (read before touching it): **Trap**: with the box kept open across an app switch, an agent's rewrite read into the clean tab had the draft spliced at the old offsets (full preview) or the old text plus the edit put back over the reload (beside the source), reverting the agent silently.
- LaTeX preview (read before touching it): **Trap** (round 10's documents defect 8): an edit on the side page pushed a snapshot, and Cmd+Z in the full preview after typing in the source put that snapshot back, dropping the typing.
- LaTeX preview (read before touching it): **Trap** (round 10's documents defect 4): the input was parsed from its buffer, so SyncTeX's lines (the disk's) matched against a chapter with unsaved lines above the click picked another paragraph, and Return edited it.
