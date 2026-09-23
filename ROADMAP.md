# Roadmap

A running list of things not yet built, roughly ordered by value. Nothing here
is required for the current version to stand on its own; this is where it could
go next.

## LaTeX (done, with room to grow)

Shipped: tectonic typesets the buffer, PDFKit shows it, and double-clicking
text on the page edits the source behind it, including adding list entries.
What is left, roughly in order of value:

- Add a table row the way a list entry is added now (the column count and the
  `&` separators are already in reach of `LatexDoc`).
- Click the page to move the cursor to that line in the source view.
- A LaTeX grammar for the syntax highlighter, so the source view is colored.
- Error markers in the source view from tectonic's log, instead of a log pane.
- Text a macro prints (`\newcommand{\company}{Initech}`) is refused when
  clicked, because it is not where it appears. Offering the macro's
  definition instead would make it editable, with a note that every use
  changes. Section and page numbers stay refused; TeX makes those up.
- Tables read back from the PDF column by column, so a click in a table
  leans on SyncTeX's line alone. Using the cell's `&` position in the row
  would pin it down exactly.

## Media rendering (images done, video next)

Images open in the editor pane on macOS: an `NSImageView`, scaled to fit and
never enlarged, with the pixel size in the title bar. Still to do:

- Zoom and scroll for large images, and a checkerboard behind transparency.
- Linux has images (a `GtkPicture`) and PDFs (poppler-glib) since September
  2026; zoom is missing there too.
- Video and audio (mp4, mov, m4v, mp3, wav, ...): play with an `AVPlayerView`
  from AVKit, which is a macOS system framework. Transport controls come for
  free. It routes the same way images do, by extension in `openFileAtPath:`.

## LSP client (done, with room to grow)

Shipped on macOS: completion, go to definition (F12 and Cmd+click), hover info,
and diagnostics drawn as squiggles, from servers the user already has (clangd,
pyright or pylsp, gopls, rust-analyzer, typescript-language-server). What is
left, roughly in order of value:

- Incremental sync (send the edit, not the whole file) for large files.
- Find references, rename, and format document; all are one request each
  on top of what exists.
- Snippet completions (placeholders you tab through); v1 asks for plain text,
  so a method completes without its parentheses.
- Completion that opens while typing an identifier, not only after `.`, `->`
  and `::` or Ctrl+Space.
- A problems list for the whole folder, and a way back after go to definition.
- The Linux port: the core (`Json`, `LspClient`) is portable; it needs a
  GTK front end (GSubprocess for the server, GtkPopover for the list).

## Editor niceties

- Incremental re-highlighting is done: the lexer keeps a state per line and an
  edit re-lexes only the lines it can have changed. What is left is the worst
  case, where one edit changes the color of everything below it (typing `/*`
  at the top of a huge file). That lexes the rest of the file in a few
  milliseconds, but recoloring hundreds of thousands of lines of text storage
  can take tens of milliseconds; coloring only the visible part first and the
  rest in idle time would hide it.
- The Linux port still does a debounced full re-lex. `IncrementalHighlighter`
  takes UTF-8 as well as UTF-16, so wiring it to GtkTextBuffer's insert and
  delete signals is the remaining step.
- Line numbers, auto-indent, bracket matching.
- Broader, more correct syntax highlighting (the scalable answer is tree-sitter,
  which would be the one place to weigh a dependency).

## Terminal

A real shell on a pty. Ordinary output is a colored log with a line input;
full-screen programs (vim, less, man, htop, git's pager, REPLs, ssh) get a
cell grid (`TerminalScreen`, drawn by `TerminalGridView`) with keys sent
straight to the pty, and pagers are no longer forced to `cat`. What remains:

- Mouse reporting (modes 1000/1002/1006), so clicks and drags reach vim,
  htop and tmux. Today the grid keeps the mouse for its own selection.
- Scrollback while the grid shows. In the alternate screen the wheel sends
  arrow keys (like xterm's alternateScroll); in the raw-mode fallback (git's
  `less -X`, a REPL) lines scrolled off the top are only in the log.
- Multi-line progress bars in the log view: they redraw with cursor-up, which
  the log ignores. Rendering the last screenful of the grid under the log
  would fix it.
- Keys at the log prompt (Tab completion, Ctrl+R) still go through the line
  input; the grid only takes over for programs that ask for raw input.
- Input methods (dead keys, CJK input) in the grid: it reads `characters`
  from the key event rather than going through NSTextInputClient.
- Word/line selection by double/triple click, clickable links (OSC 8),
  bash/fish integration.

## Android port

The newest port (`android/`), a Kotlin app around the same C++ core, with the
file list, editor, Markdown preview, images, PDFs, a terminal on
`/system/bin/sh` and a browser. What is left:

- **Termux**, which is the interesting one: it is where a phone keeps git,
  python, clangd and a TeX engine. Termux will run a command for another app
  through its RUN_COMMAND intent, which needs the app to hold
  `com.termux.permission.RUN_COMMAND` and the user to set
  `allow-external-apps=true`. Pointing the terminal at Termux's shell is the
  first step, since the pty and the screen already work.
- **Language servers**, which follow from Termux: the client is portable C++
  and already compiles for Android.
- **The LaTeX preview**, which also follows: the reader and click-to-source
  are portable, but tectonic is not built for Android, so it needs Termux or
  a machine on the network.
- Project search, comment toggling, PDFs past the first page, zoom for large
  images, terminal scrollback.

## Linux port parity

The GTK port now has the settings file, per-panel transparency, color swatches,
Ctrl+/, Ctrl+Shift+T and the divider drags. What is left:

- **Blur.** No portable GTK equivalent (KDE can blur behind windows, GNOME
  can't), so `window.blur` is ignored on Linux.
- **Live color picking.** GTK's color dialog only reports a color when it
  closes. A popover with a `GtkColorChooserWidget` could apply picks live.
- **Terminal.** VTE rather than the shared `TerminalStream`, which is fine;
  VTE is a real emulator.

## Smaller polish

- Settings: the file (colors, opacity, blur) exists; font size, tab width and
  the terminal's ANSI palette are natural next keys. The Search window and the
  hints overlay don't follow it yet.
- Search panel: live regex, and find-and-replace across files.
- A `make release` is already scripted; a GitHub Action to run it on a tag would
  fully automate cutting versions.
