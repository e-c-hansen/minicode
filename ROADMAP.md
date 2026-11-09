# Roadmap

What is not built yet, roughly ordered by value. Nothing here is needed for
the current version to stand on its own. Last rewritten 2026-11-05; brought
up to date with 1.5.15 and the round-7 fixes on 2026-11-08.

## Not planned

- **A debugger.** Decided against (2026-11-05).
- **Windows.** Not planned unless that is decided; the core is portable C++,
  but there is no Windows front end of any kind.
- **Under consideration, not planned**: editing with others at once, and
  working on a remote machine's folder.

## Left to the owner

- **Claude Code's IDE link against a real session.** Built in 1.5.8 (View >
  Claude Code IDE Link) from the protocol as read in Claude Code's and the
  VS Code extension's code, and checked with a WebSocket client of its own
  (and node's, where node is installed); it has not yet been run against a real Claude Code session.
- **Notarization**, once the owner has a paid Apple Developer account;
  `scripts/sign-and-notarize.sh` is ready.
- **Branch protection on main**, with the six CI jobs required, so a branch
  is checked before it lands rather than after.

## For daily work on the Mac

- **One text storage per tab** (M). The window has one text view and swaps
  each tab's text into it, so a switch colors and lays out the file again.
  A long tab shown again far down already lays out only its place and fills
  in the lines above it in idle time, and large-file mode lays out only what
  is shown.
- **A line of millions of characters.** A keystroke in a 5,000,000-character
  line costs about 0.35 s, all of it TextKit typesetting the paragraph again;
  minicode's own work on it is a few characters. It would need a view that
  lays out only the visible part of a line.
- **The command registry's fixed keys.** The keys the editor and terminal
  handle themselves (the editor's Command arrows, the terminal's Command
  arrows and prompt keys, the tree's Space) are listed in keys.conf but
  cannot be changed yet.
- **Running notebook cells.** Notebooks are rendered read-only from their
  saved outputs; there is no kernel.
- **The data viewers**: sorting and filtering in the CSV table, and SQLite
  results past the first 1,000 rows.
- **Line mode in the terminal.** Kept as a fallback after the grid terminal
  became the default in 1.5.5; decide whether to drop it. Its log still
  ignores multi-line progress bars that redraw with cursor-up.

## Editor and language servers

- Highlighting: Swift's `#"..."#` and `\(...)`, string templates in Kotlin,
  Scala, Dart and C#, Groovy's slashy strings, a block comment across lines
  inside a JavaScript template field, and a regex at the start of a line
  that continues a division. The worst case of incremental highlighting
  (typing `/*` at the top of a huge file) could color the visible part
  first and the rest in idle time, as the GTK port already does.
- Broader, more correct highlighting would mean tree-sitter, the one place a
  dependency might be weighed.
- Terminal: OSC 8 hyperlinks on Linux (the Mac opens them; VTE is told to
  allow them, but Ctrl+click reads only the text). Other shells (bash, fish)
  run without the shell integration's marks.
- Settings: the terminal's ANSI palette as keys; the find bar and Find in
  Folder follow the theme but have no keys of their own, and the hints
  overlay still uses fixed colors.

## LaTeX

- Add a table row the way a list entry is added now (the column count and
  the `&` separators are already in reach of `LatexDoc`).
- Click the page to move the cursor to that line in the source view (the
  other way, Show Line in PDF, is built).
- Text a macro prints (`\newcommand{\company}{Initech}`) is refused when
  clicked, because it is not where it appears. Offering the macro's
  definition instead would make it editable, with a note that every use
  changes. Section, page and list numbers stay refused.
- Tables read back from the PDF column by column, so a click in a table
  leans on SyncTeX's line alone; the cell's `&` position in the row would pin
  it down.

## Math in Markdown

The Mac typesets a page's formulas with tectonic; Linux and Android show the
TeX.

- **Linux**: the same design. `src/MathTex` is shared and tested; the port
  needs the run (a `GSubprocess` on a worker, as its LaTeX preview runs
  tectonic), a cache like the Mac's, and poppler to draw the pages, tinted
  with cairo's IN operator, as child anchors or paintables on the baseline.
- **Android**: tectonic in Termux, as the LaTeX preview already runs it, with
  `PdfRenderer` drawing the pages tinted with a `PorterDuff` SRC_IN filter,
  as `ReplacementSpan`s on the baseline.
- Copying a typeset formula should copy its TeX, not an attachment.
- `\color` inside a formula is drawn in the text's color, since the page is
  tinted whole.

## Media

- Zoom and scroll for large images, and a checkerboard behind transparency,
  on the Mac. Linux zooms PDFs and the LaTeX preview but not images.

## Linux parity

The GTK port has the editor, tree, terminal (VTE), browser, Markdown and LaTeX
previews, language servers, source control and media. Almost everything the
Mac gained from 1.5.0 on is still missing. `linux/PARITY.md` is the plan, in
stages, and says what is done. Blur is not possible on GNOME: GTK 4 has no API
for it and Mutter offers no Wayland blur protocol; KDE Plasma has its own,
untried.

Real key presses and a person's eyes on the Linux-only pieces are listed in
`BUILD-LINUX.md` under "Not verified".

## Android parity

The Kotlin app (`android/README.md`) has the file list, editor, Markdown
preview, images, PDFs, video and audio, source control, a browser, and a
terminal on Termux's bash, with language servers and the LaTeX preview in
Termux. Still to do:

- project search, comment toggling, PDFs past the first page, zoom for
  large images, and scrollback in the terminal (the shared screen keeps
  scrollback, which the port leaves off);
- the same Mac features as for Linux, where they make sense on a phone;
- F-Droid.

## Distribution

- A GitHub Action that runs `scripts/release.sh` on a tag.
- A CI job that builds the Debian package in an ubuntu:26.04 container, so
  a package that fails on Launchpad fails before the upload (ppa.sh now
  waits for Launchpad's result and fails loudly, but only after uploading).
