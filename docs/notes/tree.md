# The file tree

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## The file tree (Mac)

- `ClickOutline mouseDown:` runs the native tracking first and opens the file
  on release only if no drag began (`dragBegan`); NSOutlineView's selection
  notifications did not fire reliably. It records the click's count,
  Command and whether the press before was on the same row
  (`sameRowAsLastPress`: the window server's click count is by time alone),
  so a single click opens in the preview tab and a double-click or Cmd+click
  keeps (`EditorTabs::treeClick`). It keeps the press as its row's item, not
  the row number: the tracking runs the main queue, where a listing can
  insert rows above it (a click on main.go once opened helper.go). Drag and drop moves rows into folders
  (`validateDrop:` retargets onto a folder; into itself, a descendant or where
  it is refused), copies files from Finder in and rows out; a clash is reported
  after the drop (Finder waits on a modal during it). `pathMovedFrom:to:` moves
  tabs, the LaTeX path and a preview's relative pictures. F2 renames,
  Cmd+Delete trashes (the registry's `Tree` commands, run by the key monitor),
  Space previews with Quick Look. Show Hidden Files (`gShowHidden`) is one
  switch for every window: flipping it rescans each (a window left alone
  listed its next changed folder by the new rule, showing dotfiles there
  only), its item has a checkmark, and it is kept in the app's defaults
  (`MCShowHiddenFiles`, read in `applicationWillFinishLaunching:`).
- **FSEvents refresh what changed** (2026-11-06): the stream names folders,
  and `TreeRefresh::plan` (FileDetails) turns a batch into the loaded
  folders to list again (a subtree event: those below too; at or above the
  root: a full rescan) and rows to restat (a folder's date); `MCTreeList`
  reads them on the tree's queue, one batch at a time, generation-counted
  (a full rescan or new root drops it), and `applyTreeListings:` reloads
  only folders whose rows on show changed, keeping expansion, selection and
  scroll. Refresh (Cmd+R), file operations and the hidden-files switch stay
  full rescans. `reloadItem:reloadChildren:` on a collapsed folder, or one
  under a collapsed folder, does show the new children when opened (the
  `tree` suite checks it). Expansion still lists synchronously.
- **Git colors** (`MCTreeGitColors`, TreeGitColors.mm): one `git status
  --porcelain=v2 -z --ignored=matching` per look (`Git::Decorations`:
  last record wins per path, folders take the strongest child, deletions
  and ignored files never propagate, as VS Code's), 0.5 s after events,
  skipped for events inside ignored folders and .git/objects and logs; a
  folder outside git reruns rev-parse only when its own top folder changes.
  Worktree rows are never colored. `tree.git-*` keys, VS Code's dark or
  light defaults by `Theme::dark`.
- **The Worktrees node** (first, shown with or without hidden files, not again
  inside .claude; 2026-11-15: neither it nor a worktree row offers Rename…
  or Move to Trash, tagged `kNotOnWorktreeMenuTag` and hidden in
  `menuNeedsUpdate:`, and `renameSelected:`/`deleteSelected:`, which F2
  and Cmd+Delete reach through the key monitor, refuse them with a status
  note): each worktree named for its agent with its ahead count and
  changes (git off the main thread); right click Compare with Main (the
  review page in worktree mode, `initForWorktree:`, every changed file in
  one scroll against main at the merge base, so main's later work never
  shows as the agent deleting it; `AgentView::compareBase`; with Apply to
  Main…, see "The review view") and Open in New Window. A window on `/` or
  the home folder indexes no sessions
  (`AgentView::indexesFolder`): every transcript on the Mac is inside them.
- **Icons by kind** (2026-11-15): `FileIcon::kindOf` (FileDetails, by
  extension only, `tree:icons`; the image, video and audio tables there
  are the ones `isImagePath:`/`isMediaPath:` read) picks the row's SF
  Symbol (`MCTreeIconSymbol`, TreeDetails.mm), each image made once
  (`MCTreeSymbolImage`, which falls back to doc.text for a symbol the
  system lacks: cylinder is macOS 12). Folders and worktree rows keep
  theirs; the git colors tint the name, not the icon. GTK not yet.
- **Columns** (`MCTreeColumns` bitmask, `toggleTreeColumn:`): size, saved and
  created (`st_birthtimespec`) on their own keys, the rightmost dropped first
  on a narrow row; switching one on widens a narrow sidebar
  (`MCTreeCellWidthFor`, up to 40%). Trap: NSTableCellView moves its textField
  to the cell's width without a layout pass, so `MCTreeCell` places its fields
  again in `viewWillDraw`.
- **Folder totals** (`FolderSizes::measure`, `MCFolderSizer`, last asked
  first): every regular file below, hidden ones included (as Finder and du
  count), never through a symlink or into another device; subfolders cached by
  stamp (dev, inode, own mtime); FSEvents paths (`/private/tmp` mapped back to
  `/tmp` through realpath) invalidate a folder and its parents, and a store is
  refused if an invalidation came after the count began. 200,000 entries or 2
  s, then "≥ X".
- **Sort By**: name ascending with folders first is exactly the old order
  (`caseInsensitiveCompare:`, so nothing moves for anyone who never sorts); a
  new key starts in its own direction (dates newest, sizes largest). While a
  folder lacks a total, folders stay in name order, then the list re-sorts once
  (`folderTotalsArrived:`, through `reloadTreeKeepingState:`). Remembered per
  folder (`MCTreeSort`). `FileItem` keeps `children` in name order and `shown`
  in the window's.
- **The context menu by keyboard**: Shift+F10, a hidden Ctrl+Return (otherwise
  a text view's U+2028) and the PC Menu key (keyCode 0x6E).
  `MCShowContextMenuInWindow` builds a right mouse down just inside the
  selected row's bottom edge (so `clickedRow` is that row) or at the caret,
  asks `menuForEvent:` (restoring a text view's selection, which a right click
  moves), and pops it with `popUpContextMenu:withEvent:forView:` on the next
  run-loop turn.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- The file tree (Mac): Trap: NSTableCellView moves its textField to the cell's width without a layout pass, so `MCTreeCell` places its fields again in `viewWillDraw`.
