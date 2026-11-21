# Opening from Finder, the Dock and the command line

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## Opening from Finder, the Dock and the command line (Mac)

- **Info.plist** `CFBundleDocumentTypes`, all Alternate with no
  `NSDocumentClass`: text and source UTIs, Markdown
  (`net.daringfireball.markdown`, also imported), the exact image, video and
  audio UTIs shown (not `public.image` or `public.movie`), PDF,
  `public.folder`, Office, iWork, OpenDocument and SQLite, and by extension
  TeX, Go, Rust, TOML, conf, ini, tsx and `.ts` (macOS types it as MPEG-2
  video). No `public.data` (VS Code has none either). Where nothing else claims
  a type (.conf, .bib) mutible is the double-click app anyway.
- Open With, double-click and Dock drops arrive as odoc events at
  `application:openURLs:`: folders first (a window on it, else a new one), then
  each file to the window with the innermost root holding it, else the window
  in front (a new window only with none open); paths compared resolved and
  respelled from the window's root; `showFile:` opens hidden dotfiles too.
  A file shown that way, or named on the command line, gets the keyboard
  (`focusActiveDocument`; a new window had left it in the tree), and any
  text put whole into the editor (`displaySourceEditable`) starts with the
  caret at 1:1, since NSTextView leaves it at the end of a replaced text
  (2026-11-16; a tab shown again gets its own place back after).
  `applicationShouldOpenUntitledFile:` returns NO.
- Launch Services delivers the open between will- and didFinishLaunching, so
  setup is in `applicationWillFinishLaunching:`, and didFinishLaunching opens
  no window of its own when one exists and argv is empty (a Finder launch has /
  as cwd); with none and argv empty it brings back the last run's windows
  (`restoreOpenWindows`), else opens the cwd. `main()` registers `NSTreatUnknownArgumentsAsOpen` NO, so argv goes
  through the argv code only.
- **The Dock icon's menu** (2026-11-14, RecentFolders.mm): the recent
  folders go to `NSDocumentController noteNewRecentDocumentURL:` as well as
  mutible's own list, so the Dock lists them with mutible closed too (a
  folder URL is taken because Info.plist claims public.folder; checked in
  the real bundle). A pick arrives as an odoc at `application:openURLs:`.
  `applicationDockMenu:` adds New Window and Open Folder…
  (`openFolderInNewWindow:`, mutible's folder picker over the window in
  front, then `openURLs:`; the window in front keeps its folder; see "The
  palette, symbols and Go Back"). The first launch seeds the Dock's list from
  the stored folders when it is empty; Clear Recently Opened clears both.
  How the Dock shows it needs the owner's eyes.
- The launcher (`scripts/mutible-launcher`, `Contents/Resources/mutible`):
  paths go to a running app with `open -b ai.ible.mutible`, else `open -a`
  its own bundle (VS Code's `code` rules); `-r` writes a request file to
  `$TMPDIR/Mutible-open/*.mutible-open` (read by `MCTakeOpenRequest` only
  from the app's own temporary folder), as do web addresses (mode "web")
  and `ssh://` addresses (mode "ssh", stage 8: each opens a window on that
  machine's `mutible host --stdio` through `openRemoteAddress:`;
  docs/notes/session-host.md, "The Mac over SSH"); `-n` runs the binary detached;
  "running" is `ps -axo comm=` ending in `/Mutible` (pgrep found nothing from
  Claude Code's shell); `MUTIBLE_LAUNCHER_DRYRUN=1` prints the command.
- **Never `lsregister` or `open` a build in the repo or a worktree**: Finder
  then offers that copy too. Check claims with a renamed copy registered
  outside /private/tmp (Launch Services ignores apps there), asked with
  `LSCopyApplicationURLsForURL`, then unregistered; send odoc events offscreen
  through `NSAppleEventManager dispatchRawAppleEvent:`.

## Traps

None recorded under a "Trap" label; read the sections above.
