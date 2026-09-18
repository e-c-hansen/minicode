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

## Media rendering (images and video)

Open an image or video file and see it, instead of the "binary file" message.
Both are doable natively with no third-party dependency, so VLC is not needed:

- Images (png, jpg, jpeg, gif, heic, webp, bmp, tiff): render with an
  `NSImageView` in the editor pane, scaled to fit, in place of the text view.
- Video and audio (mp4, mov, m4v, mp3, wav, ...): play with an `AVPlayerView`
  from AVKit, which is a macOS system framework. Transport controls come for
  free.

The work is a new media view in `EditorController` that the open path routes to
based on file extension, the same way it already routes `.md` to the Markdown
renderer and `.tex` to the LaTeX preview. Modest effort, no new dependencies.
Images are next; `CLAUDE.md` ("Next task") has the concrete plan and the
traps in the open path.

## LSP client

Autocomplete, go-to-definition, and inline diagnostics by speaking the Language
Server Protocol to servers the developer already has installed (clangd, pyright,
gopls, ...). The client is light; the servers are external. This is the biggest
single feature for making it a daily coding tool, and it fits the no-dependency
thesis because MiniCode would only implement the JSON-RPC client.

## Editor niceties

- Real incremental re-highlighting (currently a debounced full re-lex; fine for
  normal files, slower on very large ones).
- Line numbers, auto-indent, bracket matching.
- Broader, more correct syntax highlighting (the scalable answer is tree-sitter,
  which would be the one place to weigh a dependency).

## Terminal

Now a real shell on a pty with colors and in-line cursor movement, shown as a
log. What remains is a screen model:

- A cell grid with cursor addressing across lines, scroll regions and the
  alternate screen, so vim, htop, less and multi-line progress bars work. The
  parser in `TerminalStream` already recognizes those sequences; it would feed
  a grid instead of a single live line. Then pagers no longer need to be `cat`.
- Keys sent straight to the pty (arrows, Tab completion, Ctrl+R) instead of a
  line-based input field.
- Selection-aware copy, clickable links (OSC 8), bash/fish integration.

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
