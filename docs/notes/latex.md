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
- **Edits are byte splices** of the span's own LaTeX (nothing escaped or
  re-serialized), marked dirty, saved only by Cmd+S, undone through
  `LatexView`'s own snapshot stack (50) via `undo:`/`redo:`.
- **Clicks in `\input` files** (2026-11-15, the owner's yes to saving them
  from the preview): `MCLatexSpanAtPointInFiles` tries the inputs the
  boxes under the point name, likeliest first, then the document
  (`SyncTexIndex::tagsAtPoint`); an input is read from its tab's buffer,
  else the disk, and parsed with `LatexDoc::parseInput` (all body text: a
  part has no `\begin{document}`, and `parse` found nothing in it). The
  edit goes into that file's tab (`EditorController+LatexInputs.mm`,
  opened behind the preview when not open, `putBackText:inTab:actionName:`
  "Edit in Preview", one step of that tab's undo, not the preview's
  stack), refused if the file changed while the box was open. The bar
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
