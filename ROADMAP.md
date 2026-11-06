# Roadmap

What is not built yet, roughly ordered by value. Nothing here is needed for
the current version to stand on its own. Last rewritten 2026-11-05, after the
second review's list was worked through.

## Not planned

- **A debugger.** Decided against (2026-11-05).
- **Windows.** Not planned unless that is decided; the core is portable C++,
  but there is no Windows front end of any kind.

## For daily work on the Mac

- **Claude Code's IDE link** (M to L). Claude Code connects to VS Code and
  JetBrains through an extension that tells it the open file and selection
  and shows its proposed edits as diffs in the editor. minicode has the
  pieces (the control channel, the side-by-side diff, notes on the code) but
  does not speak that protocol, so a Claude Code session in its terminal
  does not know what the user is looking at unless the skill's `minicode
  state` and `minicode selection` are used.
- **One text storage per tab** (M). The window has one text view and swaps
  each tab's text into it, so a switch relexes and relays out the file.
- **Non-contiguous layout** (M). Contiguous layout is left on because
  everything that reads positions relies on it; turning it off for large
  files needs trying on a real screen first.
- **The command registry's fixed keys.** The keys the editor and terminal
  handle themselves (moving lines with Option Up and Down, the terminal's
  Ctrl keys) are listed in keys.conf but cannot be changed yet.
- **Running notebook cells.** Notebooks are rendered read-only from their
  saved outputs; there is no kernel.
- **Line mode in the terminal.** Kept for a release as a fallback after the
  grid terminal became the default in 1.5.5; decide whether to drop it. Its
  log still ignores multi-line progress bars that redraw with cursor-up.

## Editor and language servers

- Incremental sync (send the edit, not the whole file) for large files.
- Snippet completions (placeholders you tab through); completion asks for
  plain text, so a method completes without its parentheses.
- Highlighting: Swift's `#"..."#` and `\(...)`, string templates in Kotlin,
  Scala, Dart and C#, Groovy's slashy strings, a block comment across lines
  inside a JavaScript template field, and a regex at the start of a line
  that continues a division. The worst case of incremental highlighting
  (typing `/*` at the top of a huge file) could color the visible part
  first and the rest in idle time, as the GTK port already does.
- Broader, more correct highlighting would mean tree-sitter, the one place a
  dependency might be weighed.
- Terminal: OSC 8 hyperlinks that programs print themselves (`ls
  --hyperlink`, GCC's diagnostic URLs), on both desktop ports. Other shells
  (bash, fish) run without the shell integration's marks.
- Settings: the terminal's ANSI palette as keys; the Search window and the
  hints overlay still use fixed colors.

## LaTeX

- Add a table row the way a list entry is added now (the column count and
  the `&` separators are already in reach of `LatexDoc`).
- Click the page to move the cursor to that line in the source view.
- Error markers in the source view from tectonic's log, instead of a log pane.
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

The GTK port (`linux/HANDOFF.md`) has the editor, tree, terminal (VTE),
browser, Markdown and LaTeX previews, language servers, source control and
media. Almost everything the Mac gained from 1.5.0 on is still missing, its
logic already in the shared core where it could be:

- tabs and the landing state (`EditorTabs`), backups (`Backups`), auto-save;
- the command palette and Quick Open (`CommandPalette`), Go to Symbol
  (`SymbolOutline`) and Go Back (`NavHistory`);
- line numbers (`LineIndex`), bracket matching (`BracketMatch`), folding
  (`Folding`), several carets and the line commands (`MultiCaret`), zoom;
- find and replace with options and Replace in Folder (`TextFind` and
  `FolderSearch` are already built into the port);
- rename, references, quick fixes, the Problems list, the second server and
  Organize Imports (all in `LspClient`), completion's additional edits, and
  the install hints (`InstallHints`);
- Format Document (`Formatter`), themes from a menu, Adjust Settings;
- the inline agent, Request a Change, the control channel, the Agents
  panel and Evidence (`InlineAgent`, `ChangeRequest`, `ControlProtocol`, the
  Agent* core);
- the viewers for CSV, JSON, notebooks and SQLite (`FileSniff`, `Csv`,
  `JsonOutline`, `Notebook`);
- typeset math (above). Blur is not possible on GNOME: GTK 4 has no API for
  it and Mutter offers no Wayland blur protocol; KDE Plasma has its own,
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

- **Notarization**, once the user has a paid Apple Developer account;
  `scripts/sign-and-notarize.sh` is ready.
- A GitHub Action that runs `scripts/release.sh` on a tag.
