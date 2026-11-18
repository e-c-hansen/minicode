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
  again in `viewWillDraw`. **The name column is the sidebar's width**
  (2026-11-18, C3): `fitTreeColumn` (`sizeLastColumnToFit`) after every
  reload (`applyTreeListings:`, `reloadTreeKeepingState:`, a focus change)
  and on the sidebar scroll view's frame changes, and
  `autoresizesOutlineColumn` is off, so a deeper row never widens the
  column past the sidebar (there is no horizontal scroller; the columns
  went off its edge). Trap: `NSTableViewLastColumnOnlyAutoresizingStyle`
  moves the column only by the change in the outline's frame, so a width
  lost once (the 80-point minimum) stayed lost while the outline kept the
  sidebar's, and the next reload (a file added to the folder) gave every
  row a cell that narrow: names cut at about 90 points in a 250-point
  sidebar. The `tree` suite leaves the column at its minimum, adds a file
  and checks every top row's name reaches the sidebar's edge again.
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

- **Focus on Folder** (2026-11-17, `EditorController+TreeFocus.mm`, its
  state in `EditorController+TreeFocusIvars.h`, `TreeFocusBar.{h,mm}`,
  the core's `TreeFocus` in FileDetails, `tree:focus`, `make guitest
  SUITES=tree_focus`): the tree's root is one folder inside the window's
  (`treeTop`: `_treeFocusItem ?: _root`), and the data source, reveals,
  drops, New File's folder and the context menu's defaults start there.
  The window's folder (`_root`, `rootPath`) never changes. The focused
  folder is a node of `_root`'s tree, reached by listing each folder down
  to it (`treeItemForFocus:`), so `loadedTreeFolders`, FSEvents and
  Refresh keep it current with no code of their own; `applyTreeListings:`
  reloads only folders at or below the top (`treeShowsFolder:`: reloading
  an item the outline does not hold is never asked), and after a listing
  or a rescan `dropLostTreeFocus` checks each folder on the way down
  still holds the next (by identity) and falls back to the whole folder
  with a status note when not (deleted, renamed, or a dotfolder with
  hidden files turned off). Changing the focus keeps the open folders:
  those on show are read, those outside are remembered
  (`_treeFocusExpanded`), and widening opens the old root and the folders
  down to it and selects its row. **Reveals widen, opens do not**:
  `revealPath:open:from:widen:` widens to `TreeFocus::widened` (the
  nearest common ancestor of the focus and the path's folder) when
  `widen`, which `revealPath:andOpen:` and `revealPath:open:from:` set
  exactly when they do not open; `showFile:` and a terminal's file link
  pass NO, so Quick Open, a definition or a file from Finder never
  unfocus the tree. The breadcrumb is a `PanelScrollView topBar` (the
  clip view and scroller start below it in `tile`, so scroll positions
  mean what they did), drawn in the sidebar's text color, the parts that
  fit chosen by `TreeFocus::fit`; a click on a part refocuses, the first
  part and the × show the whole folder, its right click has Show Whole
  Folder in Tree. Kept as `treeFocus` (a path from the folder) in the
  folder's session (`TabSessions::Session`, which a focus alone now keeps)
  and in the window record's tabs; `restoreSession` sets it first, through
  `TreeFocus::restored`, so a gone folder comes back whole, and
  `finishRestoringWindow` re-expands from the top and remembers the
  record's folders outside it (`windowRecordNow` lists those too). Not on
  the Worktrees node or a worktree row. The GTK port has it too (2026-11-17,
  `linux/src/FileTree.cpp`'s "Focus on Folder" section, `linux/HANDOFF.md`
  item 94, `./build/mutible-guitest tree_focus`): its tree makes its model
  again from the focused folder (`makeTreeModel`, `replaceModel` on the
  same selection and list view), so the folders' watches, the dotfile
  filter, git colors and drops start there with no code of their own;
  monitors on the folders from the window's folder down to the focused
  folder's parent see it deleted or renamed; `revealPath(path, widen)`
  and `showsPath` are the Mac's reveal rule; Persistence keeps the focus
  through its `treeFocus` and `restoreTreeFocus` hooks. The breadcrumb is
  labels laid out by `TreeFocus::fit` in a `GtkCustomLayout`. It keeps no
  open folders in a window's record, so a window brought back opens none.
- **Add to Focus, several folders** (2026-11-18, the owner: "what if I'm
  jumping between two different subdirectories"; the same files, core
  `tree:focus-set`, `tab-sessions:put`, the `tree_focus` suite's second
  half): the focus is a list, `_treeFocusItems` and `_treeFocusPaths` in
  the order added, kept merged by the core's `TreeFocus::added` (a folder
  inside a focused one changes nothing; one holding focused folders takes
  the first one's place and the rest go: the outer wins, so no file shows
  twice). None is the whole folder and one is exactly the single focus
  above, breadcrumb and widening included. With several, `treeTop` is nil
  and `treeTopRows` (the data source's top) is the focused items
  themselves: each a root row, labelled with its path (`treeRootLabelFor:`,
  in `configureTreeCell:`, medium weight), opened and closed as any row,
  so listings reload it as an item (`reloadItem:f == top ? nil : f` gives
  `f` with top nil) and `treeShowsFolder:` asks whether a folder is a root
  or under one. `reExpandTreeTop:` opens each root exactly when the set
  holds it (a root new to the set, or the folder that was the single
  focus, starts open). Commands with no row to go by (New File, Copy
  Path, Reveal in Finder) take `treeDefaultFolder`, the first root; a drop
  on the empty space below the roots is refused (it is in none of them).
  `revealPath:` walks from the root holding the path (`treeRootFor:`).
  **A reveal outside every root adds the path's folder**
  (`TreeFocus::revealed`; with one folder it is still `widened`), so the
  set only grows; a file at the top of the window's folder adds "" and so
  shows the whole folder. Add to Focus (`addFolderToFocus:`,
  `mutible.tree.addToFocus`, the File menu, a folder row's right click)
  takes the clicked row's folder on a right click, else the selected
  row's, else the open file's, the first not shown yet
  (`folderToAddToFocus`). **Add Folder to Focus…** (2026-11-18,
  `chooseFolderToAddToFocus:`, `mutible.tree.addFolderToFocus`, the File
  menu, the palette, and the bar's + (`addRect`, `onAdd`, before the last
  ×, on the breadcrumb and the label row alike), since with the tree
  focused every row is inside a focused folder and nothing is left to
  right click): mutible's folder picker (`MCFolderPick`) with
  `scopeFolder` the window's folder, so the core's `FolderPicker` (its
  `scope`: `parse`'s fourth argument, `inScope`, `folderpick:scope`)
  offers only folders strictly inside it, labels them by their path from
  it, completes a name with Tab to that path, and drops Choose in Finder…;
  `../`, `~/` or an absolute path elsewhere list nothing but "Only folders
  inside …". Its recent rows are the open tabs' folders not shown yet,
  most recent first; Return goes through `addTreeFocus:` (the core's
  merging). Add to Focus with nothing new to add and no clicked row
  (`clickedRow < 0`: the palette or the File menu) opens it instead of
  beeping, so it stays enabled there. Remove from Focus on a root row of several, a label's × in the
  label row, and `dropLostTreeFocus` (each root checked as the one focus
  was; those gone dropped, the whole folder when none is left) take
  folders out; one left gives the breadcrumb back. The label row is the
  same `MCTreeFocusBar` (`setFolders:`, `showsFolders`): one label per
  folder on a faint ground of the sidebar's text color with its ×, then
  the trailing × (Show Whole Folder); the full paths when they fit, else
  `TreeFocus::shortLabels` (the fewest last names that tell them apart),
  cut to share the room by `TreeFocus::shared`. `TabSessions::Session.
  treeFocus` is a list (written as a JSON array; the single string written
  before reads as a list of one), restored through `TreeFocus::
  restoredSet`. `treeFocusRememberedExpanded` lists only folders outside
  the roots, so a root closed since it was added comes back closed. **The
  GTK port follows**: it keeps one folder, the list's first
  (`linux/src/Persistence.cpp` writes a list of one and restores the
  first), until the Linux session ports several.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- The file tree (Mac): Trap: NSTableCellView moves its textField to the cell's width without a layout pass, so `MCTreeCell` places its fields again in `viewWillDraw`.
- The file tree (Mac): Trap: `NSTableViewLastColumnOnlyAutoresizingStyle` moves the column only by the change in the outline's frame, so a width lost once stayed lost and a reload cut every name off; `fitTreeColumn` fits it after each reload and frame change.
