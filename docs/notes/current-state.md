# Current state

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## The rename (2026-11-18)

minicode becomes mutible in 1.6.0 (CLAUDE.md, "The name"). The Mac side, <!-- rename-mutible: keep -->
the core and the docs moved on the `rename-mutible` branch, and the Linux
side on top of it: app id `ai.ible.Mutible`, `mutible` with `mbl` and <!-- rename-mutible: keep -->
`minicode` as links, the XDG folders copied on first launch <!-- rename-mutible: keep -->
(`linux/src/MigrationLinux`, over `Rename::linuxItems`), and the PPA's
new `mutible` source with a transitional `minicode` package in the same <!-- rename-mutible: keep -->
PPA (linux/HANDOFF.md item 97). Android is frozen under its old name. What needs the user's
eyes after 1.6.0: the first launch's migration on their own Mac (the
tabs, backups, settings, and browser sign-ins come back), macOS asking
again for folder and microphone access (it keeps those by bundle id),
and `brew uninstall --cask minicode && brew install --cask <!-- rename-mutible: keep -->
ible-ai/tap/mutible`.

## Current state (2026-11-15)

- **Released: 1.5.26** (2026-11-14), and the user's Homebrew copy is on it.
  Since 1.5.8: 1.5.9 security and data safety (bracketed paste, secure
  keyboard entry, symlinked config writes, the tectonic checksum and
  `--untrusted`, formatter consent, the cask's arm64 requirement); 1.5.10
  watched files, LSP parsing off the main thread, progress, the terminal's
  command marks and Unicode 16 widths, footnotes and HTML blocks; 1.5.11
  the compare view's word changes and per-hunk Accept and Reject,
  auto-closing pairs, large-file mode, the linear regex engine, parallel
  Find in Folder, the tree's git colors, Stage All, Discard and the branch
  picker; 1.5.12 inlay hints, semantic tokens, more Format Document
  languages with the server fallback, the preview to the side, LaTeX
  forward search and problems, hunk staging, the streaming inline agent;
  1.5.13 bracket-pair colors, more syntax styles, change bars, the commit
  verbs, the side preview at scale, Export as HTML; 1.5.14 security fixes
  in Export as HTML and the math batch, snippets, terminal splits and VT
  conformance, LaTeX warnings, the fast switch back to a large tab, the
  PPA build fix; 1.5.15 VS Code's Cmd+Up/Down and word separators, and the
  terminals' own zoom; 1.5.16 round 7's fixes of the round-6 reassessment
  and the owner's list (preview tabs, the split editor, browser tabs, Tab
  and Return in completion, inlay hints that leave the caret alone, the
  hints panel, Markdown's measure); 1.5.17 Markdown tables and code
  backgrounds as drawn, Cmd+F in the browser; 1.5.18 and 1.5.19 Linux
  only; 1.5.20 web bookmarks and the browser window (`mutible
  https://...`); 1.5.21 Linux only; 1.5.22 the browser toolbar's star,
  the bookmark folder as a menu, and saving a page once; 1.5.23 `mutible
  page --paste` (the page-paste branch of 2026-11-10, merged late: the
  owner's settings already had `control.page-input`, which the status bar
  called unknown); 1.5.24 Linux only (bookmarks); 1.5.25 the Dock
  icon's menu, `tabs.font-size`, paste without smart spaces, the find
  bar's history, parameter-name inlay hints off by default, and in the
  terminal Cmd+L (Clear to Previous Mark), Cmd+T and a scroll bar that
  takes the mouse, with the Mac's files moved to `mac/`; 1.5.26 Dark
  Modern as the default theme on every port (`theme = dark-plus` for the
  old look), VS Code's muted grays, "mutible" as the window title, Linux
  double-clicks recognized when busy and layout waits, and the README split
  into a short front page and `docs/REFERENCE.md`, with GIFs the Demos
  workflow records. Every release from 1.5.7 on has notes of what
  changed on GitHub (`gh release view v1.5.17`), written for users and
  given to release.sh as `MUTIBLE_RELEASE_NOTES`.
- **PPA**: current at 1.5.26 (`1.5.26-1~ppa1~resolute1`, built by
  Launchpad 2026-11-15, uploaded from the ThinkPad from the v1.5.26 tag).
  Uploads go from the ThinkPad (`ssh thinkpad`, checkout
  `~/mutible`), since the Mac has no Docker; unlock its gpg key first
  (`echo test | gpg --clearsign -u EEF9635EDB7F9335`), or signing fails
  once the passphrase cache has expired (1.5.14 was skipped for that
  reason). ppa.sh then waits for Launchpad's build (see "Build, test, run,
  release"). 1.5.20, 1.5.22, 1.5.23 and 1.5.25 were cut on the Mac with
  `MINICODE_NO_PPA=1`; 1.5.22, 1.5.23 and 1.5.25 changed only the Mac, so
  the PPA owes them nothing. 1.5.26 was cut on the Mac the same way and
  uploaded from the ThinkPad afterwards.
- The release is arm64 only (built on Apple silicon with no `-arch` flag);
  the README says so and the cask declares `depends_on arch: :arm64`.
- **Left to the owner**: notarization (the paid Apple Developer Program)
  and a run of the IDE link against a real
  Claude Code session (it needs the owner's logged-in Claude Code, and no
  agent writes to `~/.claude`). Multiplayer and remote workspaces are the
  owner's ideas for later, not plans.
- **Needs the user's eyes** on a real screen with real keys and mouse.
  Older work: the gutter and folds, the find bar and Find in Folder,
  Problems, the Agents panel and Evidence, the viewers, notes and tours,
  the palette's symbols, trackpad momentum in the terminal; the grid
  terminal with their own .zshrc, prompt, autosuggestions and fzf, Claude
  Code's Shift+Return, vim with the mouse; real F2, F8, F9, Shift+F12 (fn on
  a laptop), the Control+Command folding keys, Shift+Option+O; a real crash
  or force quit with unsaved tabs; a real Request a Change end to end and
  its relaunch; Finder's Open With, double-click and Dock drops; `mutible
  -r` against the running app; the LaTeX popover's placement; Ctrl+Space
  with several input sources; the waiting dots once the user has run
  `mutible hooks --install`; push, pull and autofetch against a real
  remote (ssh keys, a credential helper); Mermaid with a real mmdc; BROWSER
  with Claude Code's links and `vite --open`; page zoom and Zoom to Fit on a
  trackpad. From 1.5.9 to 1.5.15: the secure-input lock with a real sudo;
  the one-line control-character paste question (not yet seen on screen);
  inlay hints, semantic colors and bracket colors as they look; the side
  preview's scroll sync on a trackpad; snippets from a real clangd,
  rust-analyzer or gopls; dragging terminal splits; cursor shapes and the
  kitty keyboard protocol in a real vim or helix; LaTeX warnings from a
  real tectonic; a large-file tab's scroll bar thumb and a jump far down.
  From round 7: scrolling and typing in a 100,000-line tab while the
  catch-up fills in above the pane; rust-analyzer on a real crate before
  and after Allow; fzf's Ctrl+R and Ctrl+T with emoji in the list; inlay
  hints at a line's start; the inline agent's insertion marks; the compare
  view's grace before the last choice is sent; change bars after a commit
  made in another terminal; Show Hidden Files across two windows; a real
  UTF-16 file (a PowerShell script or Windows log) opened, edited and
  saved. From 1.5.17: a new zoom on a Markdown page scrolled down (the
  stale margins never reproduced offscreen); the look of the browser's
  find bar and of WebKit's selected match. From 1.5.25 and 1.5.26: the
  terminal's scroll bar, Cmd+L in a terminal, `tabs.font-size`, the Dock
  menu, the bookmark star's Trash, `mutible page --paste --all` in a
  real code pad; Dark Modern as a whole (the status bar, title bar and
  tree now share #181818), and the inline colors left from Dark+ beside
  it (popups at #252526, now lighter than the window; the #4EA1F7
  accent beside Dark Modern's #0078D4; #333333 dividers).
  **Linux**: none of the 1.5 work ran on Linux beyond CI and Launchpad's
  package build; the ThinkPad still owes the folder picker after closing
  its window, F2 rename in the tree, renames across two windows under
  GNOME, the PDF placeholder at 2x and the git panel. **Android**: the
  leader key and real typing in the commit and edit boxes (Enter and
  Shift+Enter), the key rows against the curved corners, bash and vim on
  the real keyboard, the on-screen keyboard resizing the pane, real sound
  and keys in the player, and for web pictures a badge link, the scroll
  staying put and the toggle making no requests (then delete
  `/sdcard/mc-test/web-test`).
- The Reddit drafts are a Claude Doc, "Mutible Reddit drafts"
  (https://claude.ai/code/artifact/06496ec8-70b7-443d-af5c-9c53f3996ffe).

## Traps

None recorded under a "Trap" label; read the sections above.
