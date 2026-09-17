# Roadmap

A running list of things not yet built, roughly ordered by value. Nothing here
is required for the current version to stand on its own; this is where it could
go next.

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
renderer. Modest effort, no new dependencies.

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

What macOS has that the GTK port doesn't yet. The shared C++ core already does
the hard parts, so most of this is wiring on the GTK side.

- **Settings file.** Load `~/.config/minicode/settings.conf` with the shared
  `Settings.cpp` (already pure C++ and tested), watch it with `GFileMonitor`,
  and apply colors through CSS providers and the text buffer tags that
  `Palette.h` currently hardcodes. Cmd+, becomes Ctrl+,.
- **Transparency.** Per-panel opacity via RGBA CSS backgrounds on an RGBA
  window. This needs a compositor; GNOME on Wayland has one. `window.blur` has
  no portable GTK equivalent (KDE can blur, GNOME can't), so ignore it there
  and say so in the default file.
- **Color swatches.** Clickable color values in the settings file opening
  `GtkColorDialog`, rewriting the line with `Settings::setColor`.
- **Comment toggle.** Ctrl+/ using the shared `LineComments.cpp`; its UTF-16
  offsets need converting to GTK's character offsets (`Utf8Offsets.h` is the
  place for that).
- **Shortcut alignment.** macOS moved the terminal to Shift+Cmd+T; the port
  still uses Ctrl+T. Ctrl+Shift+T matches the other pane toggles, and frees
  Ctrl+T.
- **Dividers.** The macOS behavior of dragging the terminal bar to the top to
  hide the editor, and the sidebar divider to the edge to collapse the right
  side. The port has its own editor collapse (Ctrl+Shift+E) already.
- **Terminal.** VTE rather than the shared `TerminalStream`, which is fine;
  VTE is a real emulator.

## Smaller polish

- Settings: the file (colors, opacity, blur) exists; font size, tab width and
  the terminal's ANSI palette are natural next keys. The Search window and the
  hints overlay don't follow it yet.
- Search panel: live regex, and find-and-replace across files.
- A `make release` is already scripted; a GitHub Action to run it on a tag would
  fully automate cutting versions.
