# The UI map

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## UI map (read this before UI work)

```
container (PanelHost, laid out by hand in layoutContainer)
├── blurView (NSVisualEffectView)                 window.blur
├── titlebarView + titleLabel                     with a custom title bar
├── NSSplitView (vertical)                        sidebar | right pane
│   ├── treeScroll → ClickOutline                 file tree, MCTreeCell rows,
│   │   a Worktrees node first; or MCGitPanel (Shift+Cmd+G), the Agents
│   │   panel (Shift+Cmd+A) or MCProcessesPanel (View > Toggle Processes)
│   │   swapped in with replaceSubview:
│   └── rightArea (PanelHost, relayoutRightArea)
│       ├── changeBar (MCChangeBar)                Request a Change, top, over all
│       ├── tabStrip (MCTabStrip)                  top of the editor's side
│       ├── split: the other group's strip, MCEditorMirror and a vertical
│       │   DragBar beside the focused group, which holds all below
│       ├── installBar (MCInstallBar)              under the strip
│       ├── emptyLabel                             no file open
│       ├── editorScroll → CodeTextView            over MCSourceStorage, with
│       │   MCLineGutter (vertical ruler), the MCEditorFind bar at top
│       │   right, agent notes and the MCAgentBar over the text
│       ├── or in the editor's slot: LatexView, image, PDFView, AVPlayerView,
│       │   diffScroll, CompareView, Quick Look, CSV table, SqliteView,
│       │   an EditorPage (Evidence, a worktree's changes)
│       ├── JSON outline                           right of the text
│       ├── MCMarkdownSidePreview                  a Markdown page right of
│       │   its source (Ctrl+Cmd+V), a vertical DragBar between
│       ├── browser (BrowserView)                  right of the editor: its
│       │   MCTabStrip of pages, +, the toolbar, the pages (one per live tab,
│       │   the agent page)
│       ├── problemsPanel (MCProblemsPanel)        above the terminal
│       ├── terminalPanel (MCTerminalPanel)        bottom, full width: strip,
│       │   +, ◫, TerminalViews side by side, MCTerminalFindBar
│       ├── browserDivider (DragBar, vertical)
│       └── termDivider (DragBar)                  kept topmost
├── status bar                                    Saved..., settings and keys
│                                                 errors, problem counts, server
│                                                 item, ↻ restart, hints key
├── hintsPanel                                    Shift+Cmd+H
└── MCPaletteOverlay                              while the palette is open
```

- Colors come from `Settings` (defaults are VS Code Dark Modern's). Each view has an
  `applySettings`, run at startup and on `MCSettingsDidChangeNotification`,
  that recolors what is on screen (editor re-highlights, Markdown re-renders
  keeping scroll, terminal text marked `MCTerminalDefaultForeground` follows).
  A new setting goes in `Settings.cpp` (a row of `keyTable()`, `MC_KEY(key,
  Word | CommandLine | Path, isColor, body)`, which says how far its value
  runs on the line and applies it; only a family matched by prefix, such as
  `lsp.*` or a surface's `.background`, is code in `apply()`), its default,
  the commented `defaultFileText` (`settings:key-table` fails on a row it
  lacks) and the view's `applySettings`. Per-language blocks are not done. Still inline hex (`Hex`, `THex`, `SHex`): accents #4EA1F7,
  muted #9D9D9D (Dark Modern's descriptionForeground; #9CA3AF, Tailwind's gray-400,
  until 2026-11-14, and still in linux/src and Android), dividers #333333,
  the hints overlay, terminal panel messages,
  the git panel, the developer amber #E8A33D, the test lane's pass
  #73C991 and failure #F14C4C (VS Code's testing.iconPassed and
  iconFailed), the ANSI
  palette in `TermColor::rgb()`; `linux/src/Palette.h` mirrors the defaults.
  The find bar and Find in Folder take a palette per theme
  (`FindWidget.mm`, `PaletteFor`), not keys of their own, and so do the
  language server's popups by the caret (`PopupStyle`, LspShared.h;
  `docs/notes/lsp.md`), read at each showing.
- The window always uses `NSWindowStyleMaskFullSizeContentView`;
  `layoutContainer` keeps the split below the title bar with
  `contentLayoutRect`, empty before the window is shown, so `titlebarHeight`
  falls back to frame math (or the split is 0 px tall). With a custom title bar
  (any `titlebar.*` key, or a non-opaque window) `titleLabel` mirrors
  `window.title` via KVO.
- **Translucency**: paint a translucent color once, as a plain layer color.
  `PanelScrollView` draws no scroll or clip background and sizes a layer-backed
  backdrop in `tile`. Traps: a clip view's translucent `backgroundColor`
  renders opaque; NSScrollView never resizes extra subviews (an autoresizing
  backdrop stays 0x0); the backdrop stops where the gutter starts, which paints
  its own. Property checks passed every time; only pixels caught these.
- **Manual frames**, not Auto Layout. Flags: `sidebarCollapsed`,
  `editorHidden`, `terminalVisible`, `browserVisible`, plus NSSplitView's
  collapse of `rightArea`. Position the sidebar divider only after the window
  is on screen and laid out, or it collapses to zero. Cmd+B fought the
  delegate's 160 px minimum; `sidebarCollapsed` lets it drop to 0. Dragging the
  divider far right collapses the right pane (`canCollapseSubview:`, a 5 px
  grab area either side, minimum 150). `relayoutRightArea` re-raises the drag
  bars (the terminal's last) only when out of order, never during a drag,
  counting the agent bar last. Bars take the cursor from a tracking area
  (cursor rects lose to text views) and clear their hover light in
  `viewDidHide`.
- **Editor, browser and terminal**: the top splits into `editorRect` and
  `browserRect` (`browserFraction`, `kMinSideWidth` 150 a side), and every view
  in the editor's slot gets `editorRect`. A bar dragged within 16 points of an
  edge hides that side; the smallest top is the tab strip plus 40 points
  (`kMinTopSlotHeight`). Shift+Cmd+E or the terminal bar at the top sets
  `editorHidden`; `collapseRightAreaIfEmpty`; opening a file, Cmd+1 and the
  preview toggle call `revealEditor`. Cmd+1 focuses what the tab shows
  (`focusActiveDocument`), never a text view hidden behind a viewer. The GTK
  port lays its right side out the same way (November 2026,
  `linux/src/WindowLayout.cpp` over the core's `RightAreaLayout`, which
  follows these rules with a 4 px gap for each bar); a rule changed here
  changes there too.
- **The page never takes the keyboard by itself.** WebKit makes the web view
  first responder whenever the page focuses a field, even hidden. The window is
  an `MCKeyboardWindow`, whose `makeFirstResponder:` asks `MCWebView
  mc_refusesKeyboard` before the current responder resigns (refusing in
  `becomeFirstResponder` left the window first responder). Allowed: a mouse
  down in the page, Tab from the toolbar, a page loaded from the toolbar while
  the keyboard is there; never while hidden. `navigateToString:` clears the
  toolbar allowance. Tab in the address bar goes into the page through the
  field's delegate while the field editor still has the keyboard.
- **The window's title** (2026-11-18, `EditorController+WindowTitle.mm`
  over the core's `WindowTitle.h`, `window-title:*`, checked in
  `daily_work`): the file's path from the window's folder ("prep/a.json —
  mutible"), ~ for home outside it, an untitled tab's name, a diff's or a
  page's name as before, with "● " for unsaved changes and the viewer's
  detail (size, pages, tasks done) after. While the terminal or the
  browser has the keyboard, its title (the active terminal's `title`, the
  page's `pageTitle`) instead; any other pane (the tree, the git panel,
  problems) keeps the document's. It is set by `updateTitle`, by KVO on
  the window's `firstResponder` (context `kWindowTitleFocusContext`,
  removed in dealloc), and by the terminal's and the browser's
  `onTitlesChanged` (their `reloadStrip`), which refresh only while one of
  them has the keyboard. `representedURL` is the file while its path is
  the title (the proxy icon, Cmd+click on the title), nil otherwise. Paths
  are compared with their folders standardized (/private/tmp is /tmp).
  The GTK window is to use the same function.
- Window tabbing is off (`setAllowsAutomaticWindowTabbing:NO`, or Cmd+W closes
  a merged group); `window.releasedWhenClosed = NO` (or ARC and the legacy
  release double-free it on close).

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- UI map (read this before UI work): Traps: a clip view's translucent `backgroundColor` renders opaque;
